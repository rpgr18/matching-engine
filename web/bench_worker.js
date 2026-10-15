// Runs the in-browser benchmark in its own instance of the engine, off the main thread, so the
// live book keeps animating while it runs.

import createLob from './lob.mjs';

const ready = createLob();

onmessage = async ({ data }) => {
  const lob = await ready;
  const trades = () => lob.HEAPF64[lob._lob_info_buf() >> 3];

  lob._lob_bench_prepare(0, data.requests);
  const n = lob._lob_bench_requests();

  postMessage({ phase: 'dense' });
  const denseMs = lob._lob_bench_run(0, 3);
  const denseTrades = trades();

  postMessage({ phase: 'map' });
  const mapMs = lob._lob_bench_run(1, 3);
  const mapTrades = trades();

  postMessage({ phase: 'done', n, denseMs, mapMs, trades: denseTrades, agree: denseTrades === mapTrades });
};
