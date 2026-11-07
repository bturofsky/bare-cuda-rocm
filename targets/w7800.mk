# AMD Radeon PRO W7800 32GB — the workstation lane, second target.
TARGET_NAME      = w7800
TARGET_GPU       = AMD Radeon PRO W7800 32GB (GDDR6)
TARGET_ARCH      = RDNA3 (GFX11)
TARGET_LLVM_CPU  = gfx1101
TARGET_WAVE      = 32 (dual Wave32/Wave64 — compiler picks per kernel)
TARGET_LANES     = Wave32 preferred by the back-end; simdops use 32-lane groups
TARGET_MULTI_GPU = single card; no RCCL lane
TARGET_ROLE      = secondary — workstation story; dual-dispatch design proof
