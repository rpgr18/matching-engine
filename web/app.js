// Drives the WebAssembly engine (lob.mjs, built from web/lob_wasm.cpp) and renders it.
//
// Every animation frame: apply the next slice of synthetic order flow, then read the book's top
// levels, the new trades and any events about the visitor's orders straight out of the module's
// memory, and update the page. The engine is synchronous, so a visitor's order is fully
// processed, fills and all, by the time lob_user_add returns.

import createLob from './lob.mjs';

const USER_BASE = 2 ** 40;
const LEVELS = 12;
const SPEEDS = [0, 10, 30, 100, 300, 1000, 3000, 10000, 30000, 100000, 300000, 1000000];
const PROFILES = [
  { prefill: 3000, hint: 'Adds, cancels and takers in roughly the mix of a lit equity book.' },
  { prefill: 1200, hint: 'Market makers quoting near the touch: ten cancels for every trade.' },
  { prefill: 400, hint: 'A thin book and large takers that sweep two or three levels each.' },
];
const REJECT = [
  'invalid order id', 'quantity must be positive', 'post-only needs a GTC limit order',
  'price outside the band', 'duplicate order id', 'the book is full',
  'it would have taken liquidity (post-only)', 'that order is no longer resting', 'unknown symbol',
];
const KIND = { ACCEPT: 0, REJECT: 1, TRADE: 2, CANCEL: 3, MODIFY: 4 };

const $ = (id) => document.getElementById(id);
const px = (ticks) => (ticks / 100).toFixed(2);
const int = (n) => Math.round(n).toLocaleString('en-US');
const plural = (n, word) => `${int(n)} ${word}${n === 1 ? '' : 's'}`;

let lob;
let heap = () => lob.HEAPF64;
let ptr = {};

const ui = {
  profile: 0,
  speed: 4,
  paused: false,
  budget: 0,
  last: 0,
  side: 0,
  type: 'limit',
  window: { ms: 0, n: 0, shown: null },
};
const orders = new Map();  // engine id -> order record
let orderSeq = 0;
const activity = [];       // newest first
const tape = [];           // newest first

// ---------------------------------------------------------------------------------------------
// Engine access

function stats() {
  const p = lob._lob_stats() >> 3;
  const h = heap();
  return {
    requests: h[p], trades: h[p + 1], volume: h[p + 2], orders: h[p + 3],
    bidLevels: h[p + 4], askLevels: h[p + 5], bid: h[p + 6], ask: h[p + 7], engineMs: h[p + 8],
  };
}

function depth() {
  const code = lob._lob_depth(LEVELS);
  const nb = Math.floor(code / 1000);
  const na = code % 1000;
  const h = heap();
  const base = ptr.depth;
  const read = (k) => ({ price: h[base + 3 * k], size: h[base + 3 * k + 1], count: h[base + 3 * k + 2] });
  const bids = [];
  const asks = [];
  for (let i = 0; i < nb; i++) bids.push(read(i));
  for (let i = 0; i < na; i++) asks.push(read(nb + i));
  return { bids, asks };
}

function reset() {
  lob._lob_reset(ui.profile, 1 + Math.floor(Math.random() * 1e6), PROFILES[ui.profile].prefill);
  orders.clear();
  activity.length = 0;
  tape.length = 0;
  orderSeq = 0;
  ui.window = { ms: 0, n: 0, shown: null };
  drainTape();
  renderOrders();
  renderActivity();
  renderTape();
  fillPrice('mid');
}

// ---------------------------------------------------------------------------------------------
// Visitor orders

function submit(ev) {
  ev.preventDefault();
  const qty = Math.floor(Number($('qty').value));
  const market = ui.type === 'market';
  const price = Math.round(Number($('price').value) * 100);
  if (!(qty > 0) || (!market && !(price > 0))) {
    setResult('Enter a positive price and quantity.', 'bad');
    return;
  }
  const tif = Number($('tif').value);
  const post = !market && $('post').checked ? 1 : 0;
  const rec = {
    n: ++orderSeq, side: ui.side, market, price, qty, tif, post,
    filled: 0, notional: 0, status: 'sent', reason: '', resting: null,
  };
  const id = lob._lob_user_add(ui.side, market ? 0 : price, qty, market ? 1 : 0, market ? 1 : tif, post);
  orders.set(id, rec);
  processEvents();
  setResult(outcome(rec), rec.status === 'rejected' ? 'bad' : 'good');
  renderOrders();
}

function cancelOrder(id) {
  lob._lob_user_cancel(id);
  processEvents();
  renderOrders();
}

function processEvents() {
  const h = heap();
  const fills = new Map();  // order record -> { qty, notional, count, resting } for this pass
  for (let n; (n = lob._lob_drain_events()) > 0;) {
    for (let i = 0; i < n; i++) {
      const p = ptr.events + 9 * i;
      const kind = h[p];
      const reason = h[p + 2];
      const id = h[p + 3];
      const other = h[p + 4];
      const price = h[p + 5];
      const qty = h[p + 6];
      if (kind === KIND.ACCEPT) {
        const o = orders.get(id);
        if (o) o.status = 'working';
      } else if (kind === KIND.REJECT) {
        const o = orders.get(id);
        if (o) {
          o.status = 'rejected';
          o.reason = REJECT[reason] ?? 'rejected';
          log(`<b>#${o.n}</b> rejected: ${o.reason}`);
        }
      } else if (kind === KIND.TRADE) {
        for (const oid of [id, other]) {
          const o = orders.get(oid);
          if (!o) continue;
          o.filled += qty;
          o.notional += qty * price;
          const f = fills.get(o) ?? { qty: 0, notional: 0, count: 0, resting: oid === other };
          f.qty += qty;
          f.notional += qty * price;
          f.count += 1;
          fills.set(o, f);
        }
      } else if (kind === KIND.CANCEL) {
        const o = orders.get(id);
        if (o) {
          o.status = ['cancelled', 'ioc', 'fok', 'stp'][reason] ?? 'cancelled';
          o.left = qty;
          const why = ['cancelled', 'the unfilled rest was cancelled (IOC)',
                       'killed: it could not fill in full (FOK)', 'cancelled to prevent a self-trade'][reason];
          log(`<b>#${o.n}</b> ${why}${reason === 0 ? ` with ${int(qty)} open` : ''}`);
        }
      }
    }
  }
  // One line per order per pass, however many resting orders it traded against.
  for (const [o, f] of fills) {
    const verb = o.side === 0 ? 'bought' : 'sold';
    const how = f.resting ? 'as a resting order' : f.count > 1 ? `in ${f.count} fills` : '';
    log(`<b>#${o.n}</b> ${verb} ${int(f.qty)} @ ${px(f.notional / f.qty)} ${how}`);
  }
  // Anything still working is either resting (where in the queue?) or done.
  const ip = ptr.info;
  for (const [id, o] of orders) {
    if (o.status !== 'working' && o.status !== 'resting') continue;
    if (lob._lob_order_info(id)) {
      const hh = heap();
      o.status = 'resting';
      o.resting = { qty: hh[ip + 2], ahead: hh[ip + 3], aheadQty: hh[ip + 4] };
    } else {
      o.status = 'filled';
      o.resting = null;
    }
  }
}

function outcome(o) {
  const avg = o.filled ? px(o.notional / o.filled) : '';
  switch (o.status) {
    case 'rejected': return `Rejected: ${o.reason}.`;
    case 'filled': return `Filled ${int(o.filled)} at an average of ${avg}.`;
    case 'resting': {
      const r = o.resting;
      const rest = `resting ${int(r.qty)} @ ${px(o.price)} with ${plural(r.ahead, 'order')} ahead.`;
      return o.filled ? `Filled ${int(o.filled)} at ${avg}; ${rest}` : rest[0].toUpperCase() + rest.slice(1);
    }
    case 'ioc': return o.filled ? `Filled ${int(o.filled)} at ${avg}; the rest was cancelled.` : 'Nothing to trade against at that price; cancelled (IOC).';
    case 'fok': return 'Not enough liquidity to fill it in full, so nothing traded (FOK).';
    case 'stp': return 'Cancelled to prevent a self-trade.';
    case 'cancelled': return o.filled ? `Cancelled after filling ${int(o.filled)}.` : 'Cancelled.';
    default: return '';
  }
}

function log(html) {
  activity.unshift(html);
  if (activity.length > 60) activity.pop();
  renderActivity();
}

// ---------------------------------------------------------------------------------------------
// Rendering

const ladderRows = [];

function buildLadder() {
  const root = $('ladder');
  const row = (cls) => {
    const el = document.createElement('div');
    el.className = `lrow ${cls}`;
    el.setAttribute('role', 'row');
    el.innerHTML =
      '<span class="n" role="cell"></span><span class="size bidside" role="cell"><i></i><b></b></span>' +
      '<span class="c px" role="cell" tabindex="0"></span>' +
      '<span class="size askside" role="cell"><i></i><b></b></span><span class="n" role="cell"></span>';
    const [bn, bs, p, as, an] = el.children;
    const r = { el, bn, bs, bsBar: bs.firstChild, bsTxt: bs.lastChild, p, as, asBar: as.firstChild, asTxt: as.lastChild, an, price: null };
    p.addEventListener('click', () => r.price != null && setPrice(r.price));
    p.addEventListener('keydown', (e) => { if (e.key === 'Enter' && r.price != null) setPrice(r.price); });
    root.appendChild(el);
    return r;
  };
  for (let i = 0; i < LEVELS; i++) ladderRows.push(row('askrow'));
  const spread = document.createElement('div');
  spread.className = 'lrow spread';
  spread.innerHTML = '<span></span><span id="mid-l"></span><span class="c" id="spread-c"></span><span id="mid-r"></span><span></span>';
  root.appendChild(spread);
  for (let i = 0; i < LEVELS; i++) ladderRows.push(row('bidrow'));
}

function renderBook() {
  const { bids, asks } = depth();
  const mine = { 0: new Set(), 1: new Set() };
  for (const o of orders.values()) if (o.status === 'resting') mine[o.side].add(o.price);
  const max = Math.max(1, ...bids.map((l) => l.size), ...asks.map((l) => l.size));

  for (let i = 0; i < LEVELS; i++) {
    const lvl = asks[LEVELS - 1 - i];
    const r = ladderRows[i];
    r.price = lvl ? lvl.price : null;
    r.p.textContent = lvl ? px(lvl.price) : '';
    r.asTxt.innerHTML = lvl ? `${int(lvl.size)}${mine[1].has(lvl.price) ? '<span class="me" title="Your order"></span>' : ''}` : '';
    r.asBar.style.width = lvl ? `${(100 * lvl.size) / max}%` : '0';
    r.an.textContent = lvl ? int(lvl.count) : '';
    r.bsTxt.textContent = '';
    r.bsBar.style.width = '0';
    r.bn.textContent = '';
  }
  for (let j = 0; j < LEVELS; j++) {
    const lvl = bids[j];
    const r = ladderRows[LEVELS + j];
    r.price = lvl ? lvl.price : null;
    r.p.textContent = lvl ? px(lvl.price) : '';
    r.bsTxt.innerHTML = lvl ? `${mine[0].has(lvl.price) ? '<span class="me" title="Your order"></span>' : ''}${int(lvl.size)}` : '';
    r.bsBar.style.width = lvl ? `${(100 * lvl.size) / max}%` : '0';
    r.bn.textContent = lvl ? int(lvl.count) : '';
    r.asTxt.textContent = '';
    r.asBar.style.width = '0';
    r.an.textContent = '';
  }
  const bb = bids[0];
  const ba = asks[0];
  $('spread-c').textContent = bb && ba ? `spread ${px(ba.price - bb.price)}` : 'one side empty';
}

function renderStats(frameMs, frameN) {
  const s = stats();
  $('s-requests').textContent = int(s.requests);
  $('s-trades').textContent = int(s.trades);
  $('s-orders').textContent = int(s.orders);
  $('s-spread').textContent = s.bid >= 0 && s.ask >= 0 ? px(s.ask - s.bid) : '–';
  // Browsers coarsen timers, so only measure from frames that did enough work to time.
  const w = ui.window;
  if (frameN >= 2000) {
    w.ms += frameMs;
    w.n += frameN;
  }
  if (w.ms >= 4) {
    w.shown = (w.ms * 1e6) / w.n;
    w.ms = 0;
    w.n = 0;
  }
  const ns = $('s-ns');
  ns.textContent = w.shown != null ? `${w.shown.toFixed(0)} ns` : 'Raise the speed to 100,000/s or more to measure it.';
  ns.classList.toggle('note', w.shown == null);
}

function drainTape() {
  const n = lob._lob_drain_tape();
  const h = heap();
  for (let i = 0; i < n; i++) {
    const p = ptr.tape + 6 * i;
    tape.unshift({ seq: h[p], price: h[p + 1], qty: h[p + 2], sell: h[p + 3] === 1, you: h[p + 4] === 1 || h[p + 5] === 1 });
  }
  if (tape.length > 40) tape.length = 40;
  return n;
}

function renderTape() {
  $('tape').innerHTML = tape.slice(0, 18).map((t) =>
    `<li><span class="${t.sell ? 'down' : 'up'}" aria-label="${t.sell ? 'seller' : 'buyer'} initiated">${t.sell ? '▼' : '▲'}</span>` +
    `<span>${px(t.price)}</span><span class="q">${int(t.qty)}</span><span class="you">${t.you ? 'you' : ''}</span></li>`).join('') ||
    '<li class="empty">Waiting for the first trade…</li>';
}

function renderOrders() {
  const list = [...orders.entries()].reverse().slice(0, 12);
  if (!list.length) {
    $('orders').innerHTML = '<li class="empty">No orders yet.</li>';
    return;
  }
  $('orders').innerHTML = list.map(([id, o]) => {
    const what = `${o.side === 0 ? 'BUY' : 'SELL'} ${int(o.qty)} ${o.market ? 'at market' : '@ ' + px(o.price)}`;
    let status = outcome(o);
    let queue = '';
    if (o.status === 'resting') {
      const r = o.resting;
      status = `${o.filled ? `${int(o.filled)} filled, ` : ''}${int(r.qty)} resting · ${plural(r.ahead, 'order')} (${int(r.aheadQty)} shares) ahead`;
      const pos = r.ahead === 0 ? 100 : Math.max(4, 100 / (1 + r.ahead));
      queue = `<span class="queue" title="Queue position"><i style="width:${pos}%"></i></span>`;
    }
    const cancel = o.status === 'resting' ? `<button class="x" data-cancel="${id}" aria-label="Cancel order ${o.n}">Cancel</button>` : '';
    return `<li><span class="what ${o.side === 0 ? 'buy' : 'sell'}">#${o.n} ${what}</span>${cancel}<span class="status">${status}${queue}</span></li>`;
  }).join('');
}

function renderActivity() {
  $('activity').innerHTML = activity.slice(0, 30).map((a) => `<li>${a}</li>`).join('') ||
    '<li class="empty">Fills, cancels and rejects show up here.</li>';
}

// ---------------------------------------------------------------------------------------------
// Ticket

function setPrice(ticks) {
  if (ui.type === 'market') setType('limit');
  $('price').value = px(ticks);
  updateSubmit();
}

function fillPrice(which) {
  const s = stats();
  if (s.bid < 0 && s.ask < 0) return;
  const bid = s.bid >= 0 ? s.bid : s.ask;
  const ask = s.ask >= 0 ? s.ask : s.bid;
  setPrice(which === 'bid' ? bid : which === 'ask' ? ask : Math.round((bid + ask) / 2));
}

function setSide(side) {
  ui.side = side;
  for (const b of $('side').children) b.setAttribute('aria-checked', String(Number(b.dataset.side) === side));
  updateSubmit();
}

function setType(type) {
  ui.type = type;
  for (const b of $('otype').children) b.setAttribute('aria-checked', String(b.dataset.type === type));
  const market = type === 'market';
  $('price').disabled = market;
  $('tif').disabled = market;
  $('post').disabled = market;
  updateSubmit();
}

function updateSubmit() {
  const qty = Math.max(0, Math.floor(Number($('qty').value)) || 0);
  const label = `${ui.side === 0 ? 'Buy' : 'Sell'} ${int(qty)}${ui.type === 'market' ? ' at market' : $('price').value ? ' @ ' + Number($('price').value).toFixed(2) : ''}`;
  $('submit').textContent = label;
  $('submit').classList.toggle('sell', ui.side === 1);
}

function setResult(text, tone) {
  const el = $('result');
  el.textContent = text;
  el.className = `result ${tone || ''}`;
}

// ---------------------------------------------------------------------------------------------
// Controls and loop

function setSpeed(i) {
  ui.speed = i;
  const r = SPEEDS[i];
  $('speed-out').textContent = r === 0 ? 'stopped' : `${int(r)} requests/s`;
}

function setProfile(p) {
  ui.profile = p;
  for (const b of $('profile').children) b.setAttribute('aria-checked', String(Number(b.dataset.profile) === p));
  $('profile-hint').textContent = PROFILES[p].hint;
  reset();
}

function frame(t) {
  const dt = Math.min(0.25, ui.last ? (t - ui.last) / 1000 : 0);
  ui.last = t;
  let ms = 0;
  let n = 0;
  if (!ui.paused && SPEEDS[ui.speed] > 0) {
    ui.budget += SPEEDS[ui.speed] * dt;
    n = Math.floor(ui.budget);
    ui.budget -= n;
    if (n > 0) ms = lob._lob_step(n);
  }
  processEvents();
  if (drainTape()) renderTape();
  renderBook();
  renderStats(ms, n);
  if (n > 0) renderOrders();
  requestAnimationFrame(frame);
}

// ---------------------------------------------------------------------------------------------
// Benchmark in a worker, so the live book keeps running

function runBenchmark() {
  const btn = $('bench-run');
  const out = $('bench-out');
  btn.disabled = true;
  out.innerHTML = '<p class="bench-note">Generating one million requests…</p>';
  const worker = new Worker(new URL('./bench_worker.js', import.meta.url), { type: 'module' });
  worker.onmessage = ({ data }) => {
    if (data.phase === 'dense') out.innerHTML = '<p class="bench-note">Replaying through the dense ladder + flat index…</p>';
    if (data.phase === 'map') out.innerHTML = '<p class="bench-note">Replaying through std::map + std::unordered_map…</p>';
    if (data.phase === 'done') {
      const a = data.n / data.denseMs / 1000;
      const b = data.n / data.mapMs / 1000;
      const bar = (cls, name, v) =>
        `<div class="bench-bar ${cls}"><div class="lab"><span>${name}</span><b>${v.toFixed(1)} M req/s</b></div>` +
        `<div class="track"><i style="width:${(100 * v) / Math.max(a, b)}%"></i></div></div>`;
      out.innerHTML = bar('', 'Dense ladder + flat index', a) + bar('other', 'std::map + std::unordered_map', b) +
        `<p class="bench-note">${(a / b).toFixed(1)}× faster in this browser · ${data.agree ? `both produced the same ${int(data.trades)} trades` : 'the designs disagreed!'}</p>`;
      btn.disabled = false;
      btn.textContent = 'Run again';
      worker.terminate();
    }
  };
  worker.onerror = (e) => {
    out.innerHTML = `<p class="bench-note">The benchmark could not run here: ${e.message}</p>`;
    btn.disabled = false;
  };
  worker.postMessage({ requests: 1_000_000 });
}

// ---------------------------------------------------------------------------------------------

async function main() {
  lob = await createLob();
  ptr = {
    depth: lob._lob_depth_buf() >> 3,
    events: lob._lob_event_buf() >> 3,
    tape: lob._lob_tape_buf() >> 3,
    info: lob._lob_info_buf() >> 3,
  };
  buildLadder();

  $('profile').addEventListener('click', (e) => { const b = e.target.closest('button'); if (b) setProfile(Number(b.dataset.profile)); });
  $('speed').addEventListener('input', (e) => setSpeed(Number(e.target.value)));
  $('pause').addEventListener('click', () => {
    ui.paused = !ui.paused;
    $('pause').textContent = ui.paused ? 'Resume' : 'Pause';
    $('step').disabled = !ui.paused;
  });
  $('step').addEventListener('click', () => { lob._lob_step(1); });
  $('reset').addEventListener('click', reset);
  $('side').addEventListener('click', (e) => { const b = e.target.closest('button'); if (b) setSide(Number(b.dataset.side)); });
  $('otype').addEventListener('click', (e) => { const b = e.target.closest('button'); if (b) setType(b.dataset.type); });
  $('qty').addEventListener('input', updateSubmit);
  $('price').addEventListener('input', updateSubmit);
  document.querySelector('.quick').addEventListener('click', (e) => { const b = e.target.closest('button'); if (b) fillPrice(b.dataset.fill); });
  $('ticket').addEventListener('submit', submit);
  $('orders').addEventListener('click', (e) => { const b = e.target.closest('[data-cancel]'); if (b) cancelOrder(Number(b.dataset.cancel)); });
  $('bench-run').addEventListener('click', runBenchmark);

  setSpeed(ui.speed);
  setProfile(0);
  setSide(0);
  $('engine-status').textContent = 'Engine: C++20 compiled to WebAssembly, running in this tab';
  requestAnimationFrame(frame);
}

main().catch((err) => {
  $('engine-status').textContent = `The engine failed to load: ${err.message}`;
  console.error(err);
});
