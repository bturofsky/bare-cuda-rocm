# Bare CUDA ROCm — target pull-down registry.
#
# A "target" is one AMD GPU generation this project AOT-compiles CUDA kernels
# for. Each target is a single file: GPU identity + the exact LLVM AMDGPU CPU
# identifier + the lane model the back-end must honor. Adding a GPU is one
# new file plus one line in TARGETS — nothing else in the tree changes.
#
#   make targets          list the pull-down
#   make select           interactive pull-down (persists .target)
#   make TARGET=w7900 aot override for one invocation
#   TARGET unset          the persisted .target choice (default: mi210)

TARGETS = mi210 w7800 w7900

# The primary verification target (server lane): AMD Instinct MI210.
DEFAULT_TARGET = mi210
