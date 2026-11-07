#!/usr/bin/env bash
# negative_rocm.sh — the negative-test inventory (honest-error discipline:
# accepting a program is not the same as implementing it).
#
# Hostile inputs against the translator and scanner. Every case must
# produce the named hard-fail contract (exit 2 with the opcode named, or a
# clean exit with reason) — never a hang, crash, or wrong translation.
set -uo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/neg
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad() { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }

T=./translator/ptx2llvm.bin
K=_Z9vectorAddPKfS0_Pfi

# N1: empty file
: > build/neg/empty.ptx
$T build/neg/empty.ptx $K >/dev/null 2>&1; [ $? -eq 1 ] && ok "N1 empty PTX -> exit 1" || bad "N1"

# N2: no .entry at all
printf '.version 8.4\n.target sm_86\n' > build/neg/noentry.ptx
$T build/neg/noentry.ptx $K >/dev/null 2>&1; [ $? -eq 1 ] && ok "N2 no entry -> exit 1" || bad "N2"

# N3: unsupported opcode, named on stderr
cat > build/neg/suld.ptx <<'EOF'
.version 8.4
.target sm_86
.address_size 64
.visible .entry bad_kernel(
.param .u64 bad_kernel_param_0
)
{
ld.param.u64 %rd1, [bad_kernel_param_0];
suld.b.1d.v2.u32 [%rd1], {%r1, %r2};
ret;
}
EOF
$T build/neg/suld.ptx bad_kernel >/dev/null 2>build/neg/suld.err; rc=$?
{ [ "$rc" -eq 2 ] && grep -q "suld.b.1d.v2.u32" build/neg/suld.err; } \
  && ok "N3 unsupported opcode -> exit 2, opcode named" || bad "N3"

# N4: unknown kernel name
$T fixtures/vectorAdd.ptx _nope_ >/dev/null 2>&1; [ $? -eq 1 ] && ok "N4 unknown kernel -> exit 1" || bad "N4"

# N5: out-of-profile arithmetic, named on stderr
cat > build/neg/vreg.ptx <<'EOF'
.version 8.4
.target sm_86
.address_size 64
.visible .entry bad_kernel(
.param .u64 bad_kernel_param_0
)
{
ld.param.u64 %rd1, [bad_kernel_param_0];
mad.wide.u32 %vx1, %r1, %r2, %r3;
ret;
}
EOF
$T build/neg/vreg.ptx bad_kernel >/dev/null 2>build/neg/vreg.err; rc=$?
{ [ "$rc" -eq 2 ] && grep -q "mad.wide.u32" build/neg/vreg.err; } \
  && ok "N5 out-of-profile arithmetic -> exit 2, named" || bad "N5"

# N6: truncated real fatbin — scanner must not hang or crash
head -c 200 fixtures/vectorAdd.real.fatbin > build/neg/trunc.fatbin
./scan/rocmscan build/neg/trunc.fatbin >/dev/null 2>&1; [ $? -eq 3 ] \
  && ok "N6 truncated fatbin -> clean exit 3" || bad "N6"

# N7: random binary junk
head -c 4096 /dev/urandom > build/neg/junk.bin
./scan/rocmscan build/neg/junk.bin >/dev/null 2>&1; [ $? -eq 3 ] \
  && ok "N7 binary junk -> clean exit 3" || bad "N7"

# N8: corrupted payload inside the real container — decode fails honestly
cp fixtures/vectorAdd.real.fatbin build/neg/corrupt.fatbin
printf '\xDE\xAD\xBE\xEF' | dd of=build/neg/corrupt.fatbin bs=1 seek=120 conv=notrunc 2>/dev/null
./scan/rocmscan build/neg/corrupt.fatbin >/dev/null 2>&1; rc=$?
[ "$rc" -ne 139 ] && [ "$rc" -ne 124 ] && ok "N8 corrupted container -> no crash/hang" || bad "N8"

# N9: absurd declared payload size (zip-bomb shape) — decoder stays
# output-bounded AND input-bounded; malloc of the inflated cap fails -> NULL
cp fixtures/vectorAdd.real.fatbin build/neg/bomb.fatbin
printf '\x00\x00\x00\x00\x01\x00\x00\x00' | dd of=build/neg/bomb.fatbin bs=1 seek=72 conv=notrunc 2>/dev/null
timeout 20 ./scan/rocmscan build/neg/bomb.fatbin >/dev/null 2>&1; rc=$?
{ [ "$rc" -ne 124 ] && [ "$rc" -ne 139 ]; } && ok "N9 zip-bomb shape -> bounded, no hang" || bad "N9"

# N10: truncated entry body (mismatched braces)
head -c 90 fixtures/vectorAdd.ptx > build/neg/trunc.ptx
$T build/neg/trunc.ptx $K >/dev/null 2>&1; rc=$?
{ [ "$rc" -eq 1 ] || [ "$rc" -eq 2 ]; } && ok "N10 truncated entry -> clean exit" || bad "N10"

printf '  inventory: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
