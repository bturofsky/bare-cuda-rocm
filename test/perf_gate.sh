#!/usr/bin/env bash
# perf_gate.sh — the ISA-proxy performance comparison (family F5, ROCm R6-preflight).
#
# Same workload, two code objects:
#   baseline — hand-authored native-quality IR (test/perf_baseline.ll), what a
#              hand-written ROCm kernel lowers to
#   ours     — the translated CUDA kernel (PTX -> our LLVM IR -> same llc)
# Metric: machine-instruction counts via llvm-objdump. STATIC PROXY, honestly
# labeled: the real <= 2.0x-on-GPU-clocks gate fires at R6 on the MI210.
# This report emits the measured ratio and validates both artifacts.
set -uo pipefail
cd "$(dirname "$0")/.."
LLVM_BIN=${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}
T=${TARGET_LLVM_CPU:-gfx90a}
TN=${TARGET_NAME:-mi210}
TDIR=build/$TN
mkdir -p "$TDIR"

# ours (reuse the aot pipeline pieces)
./translator/ptx2llvm.bin fixtures/vectorAdd.ptx _Z9vectorAddPKfS0_Pfi > "$TDIR/vectorAdd.ll" 2>/dev/null || exit 1
"$LLVM_BIN/opt" -O2 -o "$TDIR/vectorAdd.opt.ll" "$TDIR/vectorAdd.ll" 2>/dev/null || exit 1
"$LLVM_BIN/llc" -march=amdgcn -mcpu="$T" -filetype=obj -o "$TDIR/vectorAdd.co" "$TDIR/vectorAdd.opt.ll" 2>/dev/null || exit 1

# baseline
"$LLVM_BIN/opt" -O2 -o "$TDIR/baseline.opt.ll" test/perf_baseline.ll 2>/dev/null || exit 1
"$LLVM_BIN/llc" -march=amdgcn -mcpu="$T" -filetype=obj -o "$TDIR/baseline.co" "$TDIR/baseline.opt.ll" 2>/dev/null || exit 1

count() { "$LLVM_BIN/llvm-objdump" -d --no-show-raw-insn "$1" 2>/dev/null \
  | grep -cE '(^|[[:space:]])(s_|v_|ds_|global_|flat_|exp_|image_|buffer_)'; }

NB=$(count "$TDIR/baseline.co"); NO=$(count "$TDIR/vectorAdd.co")
[ "${NB:-0}" -gt 0 ] && [ "${NO:-0}" -gt 0 ] || { echo "PERF PROXY FAIL: empty disassembly"; exit 1; }
RATIO=$(python3 -c "print(f'{$NO/$NB:.2f}')")

echo "ISA-PROXY PERFORMANCE REPORT [$TN] (-mcpu=$T) — static, non-gating"
echo "  native-quality baseline : $NB machine instructions"
echo "  translated CUDA kernel  : $NO machine instructions"
echo "  ratio                   : ${RATIO}x"
echo "  both artifacts ELF-validated; the <= 2.0x GPU-clock gate fires at R6 on the MI210"
exit 0
