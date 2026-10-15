// Drives the WebAssembly engine (lob.mjs, built from web/lob_wasm.cpp) and renders it.
//
// Every animation frame: apply the next slice of synthetic order flow, then read the book's top
// levels, the new trades and any events about the visitor's orders straight out of the module's
// memory, and update the page. The engine is synchronous, so a visitor's order is fully
// processed, fills and all, by the time lob_user_add returns.

import createLob from './lob.mjs';

const LEVELS = 12;
const SPEEDS = [0, 10, 30, 100, 300, 1000, 3000, 10000, 30000, 100000, 300000, 1000000];
const PROFILES = [
  { prefill: 3000, hint: 'Adds, cancels and takers in roughly the mix of a lit equity book.' },
  { prefill: 1200, hint: 'Market makers quoting near the touch. Ten cancels per trade.' },
  { prefill: 400, hint: 'Thin book, large takers. Each sweeps two or three levels.' },
];
const REJECT = [
  'invalid order id', 'quantity must be positive', 'post-only needs a GTC limit',
  'price outside the band', 'duplicate order id', 'book full',
  'post-only: would take liquidity', 'order no longer resting', 'unknown symbol',
];
const KIND = { ACCEPT: 0, REJECT: 1, TRADE: 2, CANCEL: 3 };
const TIF = ['GTC', 'IOC', 'FOK'];

const $ = (id) => document.getElementById(id);
const px = (ticks) => (ticks / 100).toFixed(2);
const int = (n) => Math.round(n).toLocaleString('en-US');
const plural = (n, word) => `${int(n)} ${word}${n === 1 ? '' : 's'}`;
const clock = () => {
  const d = new Date();
  const p = (v, n = 2) => String(v).padStart(n, '0');
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
};

let lob;
const heap = () => lob.HEAPF64;
let ptr = {};

const ui = {
  profile: 0, speed: 4, paused: false, budget: 0, last: 0,
  side: 0, type: 'limit', tif: 0,
  window: { ms: 0, n: 0, shown: null },
};
const orders = new Map();  // engine id -> order record
let orderSeq = 0;
const activity = [];       // newest first
const tape = [];           // newest first
let lastTrade = null;

// ---------------------------------------------------------------------------------------------
// Engine access

function stats() {
  const p = lob._lob_stats() >> 3;
  const h = heap();
  return { requests: h[p], trades: h[p + 1], orders: h[p + 3], bid: h[p + 6], ask: h[p + 7] };
}

function depth() {
  const code = lob._lob_depth(LEVELS);
  const nb = Math.floor(code / 1000);
  const na = code % 1000;
  const h = heap();
  const read = (k) => ({ price: h[ptr.depth + 3 * k], size: h[ptr.depth + 3 * k + 1], count: h[ptr.depth + 3 * k + 2] });
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
  lastTrade = null;
  ui.window = { ms: 0, n: 0, shown: null };
  lob._lob_drain_tape();
  renderOrders();
  renderActivity();
  renderTape(0);
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
    setResult('ENTER A POSITIVE PRICE AND QUANTITY', 'bad');
    return;
  }
  const tif = market ? 1 : ui.tif;
  const post = !market && $('post').checked ? 1 : 0;
  const rec = {
    n: ++orderSeq, side: ui.side, market, price, qty, tif, post,
    filled: 0, notional: 0, status: 'sent', reason: '', resting: null,
  };
  const id = lob._lob_user_add(ui.side, market ? 0 : price, qty, market ? 1 : 0, tif, post);
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
          log(`<b>#${o.n}</b> <span class="bad">REJECTED</span> ${o.reason}`);
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
          const why = [`CXL ${int(qty)} open`, `IOC remainder ${int(qty)} cancelled`,
                       'FOK killed: could not fill in full', 'cancelled: self-trade prevention'][reason];
          log(`<b>#${o.n}</b> ${why}`);
        }
      }
    }
  }
  for (const [o, f] of fills) {
    const side = o.side === 0 ? '<span class="buy">BOT</span>' : '<span class="sell">SLD</span>';
    const how = f.resting ? ' (passive)' : f.count > 1 ? ` (${f.count} fills)` : '';
    log(`<b>#${o.n}</b> ${side} ${int(f.qty)} @ ${px(f.notional / f.qty)}${how}`);
  }
  // Anything still working is either resting (where in the queue?) or done.
  for (const [id, o] of orders) {
    if (o.status !== 'working' && o.status !== 'resting') continue;
    if (lob._lob_order_info(id)) {
      const hh = heap();
      o.status = 'resting';
      o.resting = { qty: hh[ptr.info + 2], ahead: hh[ptr.info + 3], aheadQty: hh[ptr.info + 4] };
    } else {
      o.status = 'filled';
      o.resting = null;
    }
  }
}

function outcome(o) {
  const avg = o.filled ? px(o.notional / o.filled) : '';
  switch (o.status) {
    case 'rejected': return `REJECTED: ${o.reason}`;
    case 'filled': return `FILLED ${int(o.filled)} @ ${avg} AVG`;
    case 'resting': {
      const r = o.resting;
      const pre = o.filled ? `FILLED ${int(o.filled)} @ ${avg}, ` : '';
      return `${pre}RESTING ${int(r.qty)} @ ${px(o.price)}, ${plural(r.ahead, 'order')} ahead`;
    }
    case 'ioc': return o.filled ? `FILLED ${int(o.filled)} @ ${avg}, REST CANCELLED` : 'NOTHING AT THAT PRICE, CANCELLED (IOC)';
    case 'fok': return 'NOT ENOUGH LIQUIDITY, NOTHING TRADED (FOK)';
    case 'stp': return 'CANCELLED: SELF-TRADE PREVENTION';
    case 'cancelled': return o.filled ? `CANCELLED AFTER ${int(o.filled)} FILLED` : 'CANCELLED';
    default: return '';
  }
}

function log(html) {
  activity.unshift(`<span class="t">${clock()}</span>${html}`);
  if (activity.length > 80) activity.pop();
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
    const r = { bn, bsBar: bs.firstChild, bsTxt: bs.lastChild, p, asBar: as.firstChild, asTxt: as.lastChild, an, price: null };
    p.addEventListener('click', () => r.price != null && setPrice(r.price));
    p.addEventListener('keydown', (e) => { if (e.key === 'Enter' && r.price != null) setPrice(r.price); });
    root.appendChild(el);
    return r;
  };
  for (let i = 0; i < LEVELS; i++) ladderRows.push(row('askrow'));
  const spread = document.createElement('div');
  spread.className = 'lrow spread';
  spread.innerHTML = '<span class="c" id="spread-c"></span>';
  root.appendChild(spread);
  for (let i = 0; i < LEVELS; i++) ladderRows.push(row('bidrow'));
}

const YOU = '<span class="me" title="Your resting order">YOU</span>';

function renderBook() {
  const { bids, asks } = depth();
  const mine = [new Set(), new Set()];
  for (const o of orders.values()) if (o.status === 'resting') mine[o.side].add(o.price);
  const max = Math.max(1, ...bids.map((l) => l.size), ...asks.map((l) => l.size));
  const put = (r, lvl, side) => {
    r.price = lvl ? lvl.price : null;
    r.p.textContent = lvl ? px(lvl.price) : '';
    const [txt, bar, n, otherTxt, otherBar, otherN] =
      side === 0 ? [r.bsTxt, r.bsBar, r.bn, r.asTxt, r.asBar, r.an] : [r.asTxt, r.asBar, r.an, r.bsTxt, r.bsBar, r.bn];
    const you = lvl && mine[side].has(lvl.price) ? YOU : '';
    txt.innerHTML = lvl ? (side === 0 ? you + int(lvl.size) : int(lvl.size) + you) : '';
    bar.style.width = lvl ? `${(100 * lvl.size) / max}%` : '0px';
    n.textContent = lvl ? int(lvl.count) : '';
    otherTxt.textContent = '';
    otherBar.style.width = '0px';
    otherN.textContent = '';
  };
  for (let i = 0; i < LEVELS; i++) put(ladderRows[i], asks[LEVELS - 1 - i], 1);
  for (let j = 0; j < LEVELS; j++) put(ladderRows[LEVELS + j], bids[j], 0);
  const bb = bids[0];
  const ba = asks[0];
  $('spread-c').textContent = bb && ba ? `SPREAD ${px(ba.price - bb.price)} · MID ${((bb.price + ba.price) / 200).toFixed(3)}` : 'ONE SIDE EMPTY';
}

function renderStrip(frameMs, frameN) {
  const s = stats();
  $('t-bid').textContent = s.bid >= 0 ? px(s.bid) : '–';
  $('t-ask').textContent = s.ask >= 0 ? px(s.ask) : '–';
  $('t-spread').textContent = s.bid >= 0 && s.ask >= 0 ? px(s.ask - s.bid) : '–';
  $('t-last').textContent = lastTrade != null ? px(lastTrade) : '–';
  $('t-seq').textContent = int(s.requests);
  $('t-trades').textContent = int(s.trades);
  $('t-orders').textContent = int(s.orders);
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
  $('t-ns').textContent = w.shown != null ? `${w.shown.toFixed(0)} ns incl. flow generation` : 'set RATE ≥ 100,000/s to measure';
}

function drainTape() {
  const n = lob._lob_drain_tape();
  const h = heap();
  const t = clock();
  for (let i = 0; i < n; i++) {
    const p = ptr.tape + 6 * i;
    tape.unshift({ t, price: h[p + 1], qty: h[p + 2], sell: h[p + 3] === 1, you: h[p + 4] === 1 || h[p + 5] === 1 });
  }
  if (n) lastTrade = tape[0].price;
  if (tape.length > 40) tape.length = 40;
  return n;
}

function renderTape(fresh) {
  $('tape').innerHTML = tape.slice(0, 20).map((t, i) =>
    `<li${i < fresh ? ' class="new"' : ''}><span class="t">${t.t}</span>` +
    `<span class="${t.sell ? 'down' : 'up'}">${t.sell ? '▼' : '▲'} ${px(t.price)}</span>` +
    `<span class="q">${int(t.qty)}</span><span class="you">${t.you ? 'YOU' : ''}</span></li>`).join('') ||
    '<li class="empty">WAITING FOR PRINTS…</li>';
}

function renderOrders() {
  const list = [...orders.entries()].reverse().slice(0, 10);
  const working = [...orders.values()].filter((o) => o.status === 'resting').length;
  $('working-meta').textContent = `${working} WORKING`;
  if (!list.length) {
    $('orders').innerHTML = '<li class="empty">NO ORDERS. USE ORDER ENTRY.</li>';
    return;
  }
  $('orders').innerHTML = list.map(([id, o]) => {
    const what = `#${o.n} ${o.side === 0 ? 'BUY' : 'SELL'} ${int(o.qty)} ${o.market ? 'MKT' : '@ ' + px(o.price)} ${o.market ? '' : TIF[o.tif]}${o.post ? ' POST' : ''}`;
    let status = outcome(o);
    let queue = '';
    if (o.status === 'resting') {
      const r = o.resting;
      status = `${o.filled ? `${int(o.filled)} FILLED · ` : ''}${int(r.qty)} OPEN · ${int(r.ahead)} AHEAD (${int(r.aheadQty)} SH)`;
      const pos = r.ahead === 0 ? 100 : Math.max(4, 100 / (1 + r.ahead));
      queue = `<span class="queue" title="Queue position"><i style="width:${pos}%"></i></span>`;
    }
    const cancel = o.status === 'resting' ? `<button class="x" data-cancel="${id}" aria-label="Cancel order ${o.n}">CXL</button>` : '';
    return `<li><span class="what ${o.side === 0 ? 'buy' : 'sell'}">${what}</span>${cancel}<span class="status">${status}${queue}</span></li>`;
  }).join('');
}

function renderActivity() {
  $('activity').innerHTML = activity.slice(0, 40).map((a) => `<li>${a}</li>`).join('') ||
    '<li class="empty">NO ACTIVITY.</li>';
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

function check(group, attr, value) {
  for (const b of $(group).children) b.setAttribute('aria-checked', String(b.dataset[attr] === String(value)));
}

function setSide(side) {
  ui.side = side;
  check('side', 'side', side);
  updateSubmit();
}

function setType(type) {
  ui.type = type;
  check('otype', 'type', type);
  const market = type === 'market';
  $('price').disabled = market;
  $('post').disabled = market;
  for (const b of $('tif').children) b.disabled = market;
  updateSubmit();
}

function setTif(tif) {
  ui.tif = tif;
  check('tif', 'tif', tif);
  updateSubmit();
}

function updateSubmit() {
  const qty = Math.max(0, Math.floor(Number($('qty').value)) || 0);
  const market = ui.type === 'market';
  const where = market ? 'MKT' : $('price').value ? '@ ' + Number($('price').value).toFixed(2) : '';
  $('submit').innerHTML = `SEND ${ui.side === 0 ? 'BUY' : 'SELL'} ${int(qty)} ${where} ${market ? '' : TIF[ui.tif]}<kbd>ENTER</kbd>`;
  $('submit').className = `send ${ui.side === 0 ? 'buy' : 'sell'}`;
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
  $('speed-out').textContent = r === 0 ? 'STOPPED' : `${int(r)}/s`;
  $('live').classList.toggle('off', r === 0 || ui.paused);
}

function setPaused(p) {
  ui.paused = p;
  $('pause').innerHTML = `${p ? 'RESUME' : 'PAUSE'} <kbd>SPC</kbd>`;
  $('step').disabled = !p;
  $('live').classList.toggle('off', p || SPEEDS[ui.speed] === 0);
  $('live').lastChild.textContent = p ? 'PAUSED' : 'LIVE';
}

function setProfile(p) {
  ui.profile = p;
  check('profile', 'profile', p);
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
  const fresh = drainTape();
  if (fresh) renderTape(fresh);
  renderBook();
  renderStrip(ms, n);
  if (n > 0) renderOrders();
  requestAnimationFrame(frame);
}

function onKey(e) {
  const tag = e.target.tagName;
  if (e.metaKey || e.ctrlKey || e.altKey) return;
  if (tag === 'INPUT' && e.target.type !== 'checkbox' && e.target.type !== 'range') return;
  const k = e.key.toLowerCase();
  if (k === 'b') setSide(0);
  else if (k === 's') setSide(1);
  else if (k === 'm') setType(ui.type === 'market' ? 'limit' : 'market');
  else if (e.key === ' ' && tag !== 'BUTTON') setPaused(!ui.paused);
  else return;
  e.preventDefault();
}

// ---------------------------------------------------------------------------------------------
// Benchmark in a worker, so the live book keeps running

function runBenchmark() {
  const btn = $('bench-run');
  const out = $('bench-out');
  btn.disabled = true;
  out.innerHTML = '<p class="bench-note">GENERATING 1,000,000 REQUESTS…</p>';
  const worker = new Worker(new URL('./bench_worker.js', import.meta.url), { type: 'module' });
  worker.onmessage = ({ data }) => {
    if (data.phase === 'dense') out.innerHTML = '<p class="bench-note">REPLAY: DENSE LADDER + FLAT INDEX…</p>';
    if (data.phase === 'map') out.innerHTML = '<p class="bench-note">REPLAY: STD::MAP + STD::UNORDERED_MAP…</p>';
    if (data.phase === 'done') {
      const a = data.n / data.denseMs / 1000;
      const b = data.n / data.mapMs / 1000;
      const bar = (cls, name, v) =>
        `<div class="bench-bar ${cls}"><div class="lab"><span>${name}</span><b>${v.toFixed(1)}M REQ/S</b></div>` +
        `<div class="track"><i style="width:${(100 * v) / Math.max(a, b)}%"></i></div></div>`;
      out.innerHTML = bar('', 'DENSE LADDER + FLAT INDEX', a) + bar('other', 'STD::MAP + UNORDERED_MAP', b) +
        `<p class="bench-note">${(a / b).toFixed(1)}× FASTER IN THIS BROWSER · ${data.agree ? `BOTH MADE THE SAME ${int(data.trades)} TRADES` : 'DESIGNS DISAGREED'}</p>`;
      btn.disabled = false;
      btn.textContent = 'RUN AGAIN';
      worker.terminate();
    }
  };
  worker.onerror = (e) => {
    out.innerHTML = `<p class="bench-note">BENCHMARK FAILED: ${e.message}</p>`;
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

  const on = (id, attr, fn) => $(id).addEventListener('click', (e) => {
    const b = e.target.closest('button');
    if (b && b.dataset[attr] !== undefined) fn(b.dataset[attr]);
  });
  on('profile', 'profile', (v) => setProfile(Number(v)));
  on('side', 'side', (v) => setSide(Number(v)));
  on('otype', 'type', (v) => setType(v));
  on('tif', 'tif', (v) => setTif(Number(v)));
  document.querySelector('.quick').addEventListener('click', (e) => { const b = e.target.closest('button'); if (b) fillPrice(b.dataset.fill); });
  $('speed').addEventListener('input', (e) => setSpeed(Number(e.target.value)));
  $('pause').addEventListener('click', () => setPaused(!ui.paused));
  $('step').addEventListener('click', () => { lob._lob_step(1); });
  $('reset').addEventListener('click', reset);
  $('qty').addEventListener('input', updateSubmit);
  $('price').addEventListener('input', updateSubmit);
  $('ticket').addEventListener('submit', submit);
  $('orders').addEventListener('click', (e) => { const b = e.target.closest('[data-cancel]'); if (b) cancelOrder(Number(b.dataset.cancel)); });
  $('bench-run').addEventListener('click', runBenchmark);
  document.addEventListener('keydown', onKey);

  setSpeed(ui.speed);
  setProfile(0);
  setSide(0);
  setTif(0);
  $('engine-status').textContent = 'ENGINE: C++20 → WASM · 86 KB · RUNNING IN THIS TAB';
  renderBook();  // draw once now; a tab opened in the background gets no frames until shown
  renderStrip(0, 0);
  requestAnimationFrame(frame);
}

main().catch((err) => {
  $('engine-status').textContent = `ENGINE FAILED TO LOAD: ${err.message}`;
  console.error(err);
});
