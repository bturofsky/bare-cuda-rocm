# AMD Radeon PRO W7900 — the workstation lane, third target.
TARGET_NAME      = w7900
TARGET_GPU       = AMD Radeon PRO W7900
TARGET_ARCH      = RDNA3 (GFX11)
TARGET_LLVM_CPU  = gfx1100
TARGET_WAVE      = 32 (dual Wave32/Wave64 — compiler picks per kernel)
TARGET_LANES     = Wave32 preferred by the back-end; simdops use 32-lane groups
TARGET_MULTI_GPU = single card; no RCCL lane
TARGET_ROLE      = tertiary — largest RDNA3 workspace; same code path as W7800
