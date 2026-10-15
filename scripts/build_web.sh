#!/usr/bin/env bash
# Compiles the engine to WebAssembly for the browser demo: web/lob.mjs + web/lob.wasm.
# Needs Emscripten (brew install emscripten, or emsdk). Serve web/ over HTTP to try it:
#   python3 -m http.server 8790 --directory web
set -euo pipefail
cd "$(dirname "$0")/.."

FUNCS=(lob_reset lob_step lob_user_add lob_user_cancel lob_user_modify lob_depth lob_depth_buf
       lob_order_info lob_info_buf lob_drain_events lob_event_buf lob_drain_tape lob_tape_buf
       lob_stats lob_bench_prepare lob_bench_run lob_bench_requests)
EXPORTS=$(printf '"_%s",' "${FUNCS[@]}")

em++ -std=c++20 -O3 -DNDEBUG -Iinclude web/lob_wasm.cpp -o web/lob.mjs \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createLob -sENVIRONMENT=web,worker \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sFILESYSTEM=0 \
  -sEXPORTED_FUNCTIONS="[${EXPORTS%,}]" -sEXPORTED_RUNTIME_METHODS='["HEAPF64"]'

ls -l web/lob.mjs web/lob.wasm
