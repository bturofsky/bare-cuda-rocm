# Bare CUDA ROCm

**CUDA kernels compiled ahead-of-time to native AMD GPU code objects — no
runtime interception, no emulation layer, no source rewrite.**

Part of the **Bare CUDA** family. Its sibling,
[Bare CUDA Metal](https://github.com/bturofsky/bare-cuda-metal), runs
bitwise-verified CUDA binaries on Apple silicon. This project points the
same discipline at AMD: translate once, compile **AOT to native GPU
binaries** with the LLVM AMDGPU back-end, and dispatch with nothing between
the kernel and the hardware.

```
+-----------------------------------------------------------------------------------+
|                     CUDA binary (real nvcc 12.4 fatbin)                           |
+-----------------------------------------------------------------------------------+
                                   |
                                   v  [ portability scanner ]
              fatbin container walk -> PTX extract (LZ4 / zstd / deflate)
                                   -> opcode census -> per-target AMD feasibility
                                   |
                                   v  [ translator ]
                       PTX subset -> textual LLVM IR (amdgcn-amd-amdhsa)
                                   |
                                   v  [ LLVM AOT pipeline — full optimizer ]
                       opt -O2  ->  llc -march=amdgcn -mcpu=<gfx>
                                   |
                                   v
            Native AMDGCN code objects (.co / ELF EM_AMDGPU)
              gfx90a  (Instinct MI210, CDNA2, Wave64)
              gfx1101 (Radeon PRO W7800, RDNA3, dual Wave32/64)
              gfx1100 (Radeon PRO W7900, RDNA3, dual Wave32/64)
                                   |
                                   v
        [ hardware gate: hipModuleLoad + launch on the ROCm host (phase 2) ]
```

## Verified today (v0, all measured — see [VERIFIED.md](VERIFIED.md))

| Gate | Proves | Result |
|---|---|---|
| Scanner | real nvcc 12.4 fatbin (LZ4-PTX) → census | 22/22 opcode hits — **FULLY PORTABLE v0** |
| Translator | PTX → LLVM IR | IR passes `opt` verification |
| Optimizer | `opt -O2` | register slots fully promoted |
| Codegen | `llc -march=amdgcn` × 3 targets | `EF_AMDGPU_MACH_AMDGCN_GFX90A / GFX1101 / GFX1100` + kernel symbol, ELF-validated |
| Honesty | unsupported opcode | **exit 2, opcode named** — never a wrong translation |
| Harness | 24 structured checks | **24/24 green**, builds warning-free under `-Wall -Wextra -Werror` |

The entire AOT lane runs on **any machine with LLVM's AMDGPU back-end — no
AMD GPU required to build, cross-compile, or validate**. The live-GPU gate
auto-arms when the ROCm host is present (`scripts/verify_mi210.sh`); nothing
is faked in between.

## Target pull-down

Three registered GPUs, one switch. Adding a fourth GPU is one file.

```bash
make targets                      # list the pull-down
make select                       # interactive picker (persists .target)
make aot                          # build + validate for the selected target
make TARGET=w7800 aot             # override for one invocation
make aot-all                      # build + validate for every registered target
make test                         # the 24-check acceptance harness
make doctor                       # toolchain + fixture audit
```

| Target | GPU | Architecture | LLVM CPU | Lane model |
|---|---|---|---|---|
| `mi210` | AMD Instinct MI210 | CDNA2 (GFX9) | `gfx90a` | Wave64, RCCL/Infinity Fabric multi-GPU lane (phase 2) |
| `w7800` | Radeon PRO W7800 32GB | RDNA3 (GFX11) | `gfx1101` | dual Wave32/Wave64 |
| `w7900` | Radeon PRO W7900 | RDNA3 (GFX11) | `gfx1100` | dual Wave32/Wave64 |

## Design rules (what makes this "bare")

1. **Translate once, compile AOT.** All optimization happens in LLVM's AMDGPU
   back-end — real instruction selection, register allocation, and scheduling
   per GPU generation. No runtime bytecode translation, ever.
2. **Honesty contract.** An opcode outside the supported profile hard-fails
   with the opcode *named* (`exit 2`). The project never ships a wrong
   translation dressed up as success — the same contract its sibling
   verifies bitwise against real NVIDIA hardware.
3. **blockDim travels as data.** The translated kernel takes the CUDA-visible
   block size as an explicit trailing argument (`_ntid_x`), so hardware lane
   width (Wave64/Wave32) never leaks into kernel semantics.
4. **Validator-gated artifacts.** Every `.co` must prove `EM_AMDGPU` + the
   exact per-target `EF_AMDGPU_MACH_*` flag + the kernel symbol, or the gate
   fails. Byte-level proof, not "it seemed to build."

## Repository layout

```
targets/    the pull-down registry — one file per GPU (mi210, w7800, w7900)
scan/       rocmscan — fatbin walk, PTX census, per-target feasibility
translator/ ptx2llvm — PTX subset -> LLVM IR (the AOT lane's front end)
test/       run_tests.sh (24 checks) + ELF validator per target
scripts/    doctor, target picker, hardware gate (auto-detects the ROCm host)
fixtures/   real CUDA 12.4 nvcc artifacts (fatbin, PTX, cuobjdump dump)
```

## What runs today vs. what's next

- **Today (v0):** scanner over real nvcc fatbins (LZ4/zstd/deflate), PTX→LLVM
  IR translation of the verified vectorAdd-class profile, full LLVM AOT
  pipeline to native code objects for three AMD generations, ELF-level
  validation, structured acceptance harness.
- **Phase 2 (hardware gate):** `hipModuleLoad` + launch of the compiled
  `.co` on the MI210, element-verified against CPU truth; Wave64 census;
  zero-copy host memory. The suite skeletons already exist and auto-detect
  the host.
- **Phase 3:** RCCL collectives over the Infinity Fabric pair (4× MI210),
  shared-memory + atomics lowering, more of the opcode census — the scanner's
  verdict tells us exactly what to build next, in the measured order.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the lowering contract and
[ROADMAP.md](ROADMAP.md) for the full plan.

## Family

| Project | Target | Status |
|---|---|---|
| [Bare CUDA Metal](https://github.com/bturofsky/bare-cuda-metal) | Apple M1–M5 via Metal, AOT `.metallib` | bitwise-verified vs RTX 8000, 15 acceptance suites |
| **Bare CUDA ROCm** (this repo) | AMD MI210 / W7800 / W7900 via LLVM AMDGPU | v0: AOT lane verified through native code objects |

## Credits & license

MIT — original work. NVIDIA and AMD own their respective trademarks; this is
an independent interoperability project, unaffiliated with either, shipping
none of their SDK code.
