#!/usr/bin/env bash
# validate_codeobject.sh — ELF-level proof that a .co is a genuine AMDGCN
# code object for the requested target. Exits non-zero on any mismatch.
#   usage: validate_codeobject.sh <llvm-readobj> <file.co> <target_name> <gpu> <llvm_cpu>
set -euo pipefail

RDOBJ="$1"; CO="$2"; TNAME="$3"; GPU="$4"; CPU="$5"

case "$CPU" in
  gfx90a)  MACH_FLAG="EF_AMDGPU_MACH_AMDGCN_GFX90A" ;;
  gfx1101) MACH_FLAG="EF_AMDGPU_MACH_AMDGCN_GFX1101" ;;
  gfx1100) MACH_FLAG="EF_AMDGPU_MACH_AMDGCN_GFX1100" ;;
  *) echo "VALIDATE FAIL: unknown llvm cpu $CPU"; exit 1 ;;
esac

[ -s "$CO" ] || { echo "VALIDATE FAIL: $CO missing/empty"; exit 1; }

HDR=$("$RDOBJ" -h "$CO" 2>/dev/null)

echo "$HDR" | grep -q "Machine:" || { echo "VALIDATE FAIL: not an ELF"; exit 1; }
echo "$HDR" | grep -q "EM_AMDGPU" || { echo "VALIDATE FAIL: machine != EM_AMDGPU"; exit 1; }
echo "$HDR" | grep -q "$MACH_FLAG" || { echo "VALIDATE FAIL: missing $MACH_FLAG"; exit 1; }

SYM=$("$RDOBJ" --symbols "$CO" 2>/dev/null)
echo "$SYM" | grep -q "_Z9vectorAddPKfS0_Pfi" || { echo "VALIDATE FAIL: kernel symbol missing"; exit 1; }

BYTES=$(wc -c < "$CO" | tr -d ' ')
echo "VALIDATE OK [$TNAME] $GPU (llvm -mcpu=$CPU): EM_AMDGPU + $MACH_FLAG + kernel symbol present, $BYTES bytes"
