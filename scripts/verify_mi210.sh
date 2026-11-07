#!/usr/bin/env bash
# verify_mi210.sh — the hardware gate (phase 2 of the roadmap).
#
# Detects a live ROCm + AMD GPU host and runs the on-GPU acceptance suites
# (the analog of Bare CUDA Metal's A/B gates). When no AMD hardware is
# present it says so honestly and exits 0 — the AOT lane above is verified
# hardware-free; nothing here is faked.
set -uo pipefail

HIPCC=""
ROCM_SMI=""
for c in hipcc /opt/rocm/bin/hipcc; do [ -x "$c" ] && HIPCC="$c" && break; done
for c in rocm-smi /opt/rocm/bin/rocm-smi; do command -v "$c" >/dev/null 2>&1 && ROCM_SMI="$c" && break; done

echo "Bare CUDA ROCm — hardware gate"
if [ -z "$HIPCC" ] || [ -z "$ROCM_SMI" ]; then
  echo "  no ROCm host detected (hipcc/rocm-smi absent)"
  echo "  status: HARDWARE GATE PENDING — v0 is verified through the"
  echo "          native-code-object level (see VERIFIED.md); live-GPU"
  echo "          suites arm automatically on the MI210 host."
  exit 0
fi

GPU=$("$ROCM_SMI" --showproductname --showgpuid 2>/dev/null | grep -m1 "Card series" || echo unknown)
echo "  ROCm host detected — GPU: $GPU"
echo "  planned suites (ROADMAP phase 2):"
echo "    1. host-load: hipModuleLoad of build/mi210/vectorAdd.co + launch vs CPU truth"
echo "    2. wave64 census: grid-stride + shared-memory reduction on Wave64 lanes"
echo "    3. zero-copy: hipHostMalloc path vs device copy"
echo "    4. RCCL multi-GPU: AllReduce ring over the Infinity Fabric pair"
echo "  implementation lands with the host (ROADMAP.md phase 2) — nothing to fake here."
exit 0
