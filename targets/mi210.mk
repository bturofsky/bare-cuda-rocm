# AMD Instinct MI210 — the primary verification target.
TARGET_NAME      = mi210
TARGET_GPU       = AMD Instinct MI210
TARGET_ARCH      = CDNA2 (GFX9)
TARGET_LLVM_CPU  = gfx90a
TARGET_WAVE      = 64
TARGET_LANES     = Wave64 native; Matrix Core FMA (not MFMA in v0)
TARGET_MULTI_GPU = RCCL over Infinity Fabric (2 links, 4x MI210 stack) — hardware-gate phase
TARGET_ROLE      = primary — the server story; every AOT artifact is verified for this CPU first
TARGET_TUNE      = -O3 -mattr=-xnack  # ISA ladder: max codegen level, xnack pinned off (MI210 default)
