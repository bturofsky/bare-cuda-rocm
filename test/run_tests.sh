#!/usr/bin/env bash
# run_tests.sh — the v0 acceptance harness.
#
# Structured PASS/FAIL runner with counters and a non-zero exit on any
# failure. Covers: translator IR validity, opt -O2 promotion, per-target
# codegen + ELF validation, the honesty hard-fail contract, scanner
# behavior (real fatbin / zstd / deflate / raw PTX), the target pull-down,
# and the validator's negative path.
set -uo pipefail
cd "$(dirname "$0")/.."

LL="${1:-/opt/homebrew/opt/llvm/bin}"
LLC="$LL/llc"; OPTB="$LL/opt"; RDOBJ="$LL/llvm-readobj"
PASS=0; FAIL=0

ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
check(){ local name="$1" rc="$2"; [ "$rc" -eq 0 ] && ok "$name" || bad "$name"; }

say() { printf '%s\n' "$*"; }
hr()  { printf '\n== %s ==\n' "$*"; }

mkdir -p build

# build fresh (the warning harness is -Werror in the Makefile rules)
hr "build (-Wall -Wextra -Werror)"
make --no-print-directory scan >/dev/null 2>&1; check "rocmscan builds warning-free" $?
make --no-print-directory translator/ptx2llvm.bin >/dev/null 2>&1; check "ptx2llvm builds warning-free" $?

hr "translator: PTX -> LLVM IR"
./translator/ptx2llvm.bin fixtures/vectorAdd.ptx _Z9vectorAddPKfS0_Pfi > build/va.ll 2>/dev/null
check "fixture PTX translates (exit 0)" $?
mkdir -p build
"$OPTB" -O2 -disable-output build/va.ll 2>/dev/null
check "emitted IR passes opt verification" $?
"$OPTB" -O2 -o build/va.opt.ll build/va.ll 2>/dev/null
check "opt -O2 pipeline runs" $?
N_ALLOCA=$(grep -c "alloca" build/va.opt.ll)
N_ALLOCA=${N_ALLOCA:-99}
[ "$N_ALLOCA" -le 2 ] && ok "opt -O2 promoted register slots (allocas: $N_ALLOCA)" \
  || bad "opt -O2 left $N_ALLOCA allocas (mem2reg did not promote)"
grep -q "amdgpu_kernel" build/va.ll && ok "kernel emitted as amdgpu_kernel" || bad "missing amdgpu_kernel"
grep -q "_ntid_x" build/va.ll && ok "blockDim-as-data contract present (_ntid_x)" || bad "_ntid_x missing"

hr "honesty contract (never a wrong translation)"
cat > build/bad.ptx <<'EOF'
.version 8.4
.target sm_86
.address_size 64
.visible .entry bad_kernel(
.param .u64 bad_kernel_param_0
)
{
.reg .b32 %r<2>;
ld.param.u64 %rd1, [bad_kernel_param_0];
shfl.idx.b32 %r1, %r1, %r1, 3;
ret;
}
EOF
./translator/ptx2llvm.bin build/bad.ptx bad_kernel > /dev/null 2> build/bad.err
RC_BAD=$?
[ "$RC_BAD" -eq 2 ] && ok "unsupported opcode -> exit 2 (hard-fail)" || bad "unsupported opcode exit=$RC_BAD (want 2)"
grep -q "shfl.idx.b32" build/bad.err && ok "failing opcode NAMED on stderr" || bad "opcode not named"

./translator/ptx2llvm.bin fixtures/vectorAdd.ptx _no_such_kernel_ > /dev/null 2>&1
[ $? -eq 1 ] && ok "kernel-not-found -> exit 1" || bad "kernel-not-found exit wrong"

hr "AOT codegen: one code object per registered target"
for pair in "mi210:gfx90a" "w7800:gfx1101" "w7900:gfx1100"; do
  t="${pair%%:*}"; cpu="${pair##*:}"
  mkdir -p "build/$t"
  "$LLC" -march=amdgcn -mcpu="$cpu" -filetype=obj -o "build/$t/va.co" build/va.opt.ll 2>/dev/null
  bash test/validate_codeobject.sh "$RDOBJ" "build/$t/va.co" "$t" "($cpu)" "$cpu" >/dev/null 2>&1
  check "native .co for $t (-mcpu=$cpu) validates" $?
done

# negative: validator must reject a non-ELF
echo "not an elf" > build/junk.co
bash test/validate_codeobject.sh "$RDOBJ" build/junk.co x x gfx90a >/dev/null 2>&1
[ $? -ne 0 ] && ok "validator rejects non-ELF input" || bad "validator accepted junk"

hr "scanner: fatbin census + per-target feasibility"
./scan/rocmscan fixtures/vectorAdd.real.fatbin > build/scan.txt 2>/dev/null
check "scan runs on the real nvcc fatbin" $?
grep -q "FULLY PORTABLE v0" build/scan.txt && ok "verdict: FULLY PORTABLE v0" || bad "verdict not FULLY PORTABLE"
grep -q "kernels: 1" build/scan.txt && ok "one kernel found" || bad "kernel count wrong"
./scan/rocmscan fixtures/vectorAdd.zstd.fatbin > build/scan_z.txt 2>/dev/null
grep -qE "PTX: [0-9]+ bytes" build/scan_z.txt && ok "zstd fatbin PTX decoded" || bad "zstd fatbin failed"
./scan/rocmscan fixtures/vectorAdd.deflate.fatbin > build/scan_d.txt 2>/dev/null
grep -qE "PTX: [0-9]+ bytes" build/scan_d.txt && ok "deflate fatbin PTX decoded" || bad "deflate fatbin failed"
./scan/rocmscan fixtures/vectorAdd.ptx > build/scan_r.txt 2>/dev/null
grep -q "FULLY PORTABLE" build/scan_r.txt && ok "raw PTX input passthrough" || bad "raw PTX failed"

hr "target pull-down"
make --no-print-directory targets > build/targets.txt 2>/dev/null
check "make targets runs" $?
grep -q "mi210" build/targets.txt && grep -q "w7800" build/targets.txt && grep -q "w7900" build/targets.txt \
  && ok "all three GPUs listed" || bad "pull-down incomplete"
grep -q "gfx90a" build/targets.txt && grep -q "gfx1101" build/targets.txt && grep -q "gfx1100" build/targets.txt \
  && ok "all three llvm CPUs mapped" || bad "cpu mapping incomplete"

hr "ISA tuning ladder (F4/R3)"
make --no-print-directory isa-tune > build/isa.txt 2>/dev/null
check "make isa-tune runs (baseline + tuned code objects)" $?
grep -q "ISA TUNING LADDER" build/isa.txt && ok "ladder report produced (machine-instruction counts)" || bad "no ladder report"
grep -q "verdict" build/isa.txt && ok "verdict emitted (proxy metric — latency waits for the hardware gate)" || bad "no verdict"
grep -q "VALIDATE OK" build/isa.txt && ok "tuned artifact passes ELF validation" || bad "tuned artifact failed validation"

hr "ISA-proxy performance report (F5, static)"
LLVM_BIN="$(LLVM_BIN)" make --no-print-directory perf-gate > build/perfproxy.txt 2>/dev/null
check "perf-gate runs (baseline + translated artifacts build)" $?
grep -q "ratio" build/perfproxy.txt && ok "static ratio reported (<= 2.0x GPU-clock gate fires at R6)" || bad "no ratio"

hr "negative-test inventory (honest-error contract)"
bash test/negative_rocm.sh > build/negrocm.txt 2>&1
rc=$?
grep -c "PASS" build/negrocm.txt | head -1 | sed 's/^/    passed cases: /'
[ $rc -eq 0 ] && ok "all hostile inputs produce the named hard-fail contract" || bad "negative inventory failed"

hr "summary"
say "  passed: $PASS  failed: $FAIL"
if [ "$FAIL" -eq 0 ]; then
  printf '  \033[32mV0 GATE: ALL GREEN — AOT lane verified end-to-end\033[0m\n'
  exit 0
fi
printf '  \033[31mV0 GATE: FAILURES ABOVE\033[0m\n'
exit 1
