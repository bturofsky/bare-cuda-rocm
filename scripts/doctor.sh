#!/usr/bin/env bash
# doctor.sh — verify the AOT lane's toolchain and print the target table.
# Exit non-zero if the cross-compile lane cannot run.
set -uo pipefail
LLC="${1:-/opt/homebrew/opt/llvm/bin/llc}"
OPT="${2:-/opt/homebrew/opt/llvm/bin/opt}"
RDOBJ="${3:-/opt/homebrew/opt/llvm/bin/llvm-readobj}"

fail=0
say() { printf '%s\n' "$*"; }
bad() { printf '  [MISSING] %s\n' "$*"; fail=1; }

say "Bare CUDA ROCm — doctor"
for tool in "$LLC" "$OPT" "$RDOBJ"; do
  [ -x "$tool" ] && say "  [ok] $(basename "$tool"): $tool" || bad "$tool"
done

if "$LLC" --version 2>/dev/null | grep -q "amdgcn"; then
  say "  [ok] LLVM AMDGPU back-end registered (amdgcn)"
else
  bad "amdgcn target not registered in $LLC — install llvm with the AMDGPU target"
fi

if [ -s fixtures/vectorAdd.ptx ] && [ -s fixtures/vectorAdd.real.fatbin ]; then
  say "  [ok] fixtures present (real CUDA 12.4 nvcc fatbin + PTX text)"
else
  bad "fixtures missing — fixtures/vectorAdd.ptx and fixtures/vectorAdd.real.fatbin required"
fi

command -v pkg-config >/dev/null 2>&1 && ldflags_ok=1 || ldflags_ok=0
echo '#include <zstd.h>' | cc -I/opt/homebrew/include -x c -fsyntax-only - 2>/dev/null \
  && say "  [ok] libzstd headers (fatbin zstd payloads)" \
  || bad "libzstd headers not found (brew install zstd)"

if [ "$fail" -eq 0 ]; then
  say "doctor: PASS — the AOT lane can run on this machine (no GPU required)"
else
  say "doctor: FAIL — fix the missing pieces above"
fi
exit "$fail"
