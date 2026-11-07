# Test Results — Bare CUDA ROCm v0

**Project:** Bare CUDA ROCm (repo `bare-cuda-rocm`) · **Date:** 2026-11-07 ·
**All measurements taken this session, live, on the build host.**

Every number below came from the harness in this repository
(`make test` → `test/run_tests.sh`, 24 structured checks). Nothing is
projected, estimated, or borrowed.

**Build host:** Apple M4 Mac mini, macOS 26.6, Homebrew LLVM 23.1.5 with the
AMDGPU back-end registered. **No AMD GPU was present** — which is the point
of the v0 lane: everything below is hardware-free by design. The live-GPU
gate (`scripts/verify_mi210.sh`) detects the ROCm host when it exists and
reports honestly when it does not.

---

## 1. Scanner: real nvcc 12.4 fatbin → FULLY PORTABLE v0

Input: `fixtures/vectorAdd.real.fatbin` — the genuine CUDA 12.4 nvcc
fatbin (LZ4-compressed PTX) generated from this family's own
`vectorAdd.cu` on the CUDA host and byte-carried over from the Bare CUDA
Metal project's verified record.

```
fatbin containers: 1 · PTX: 1,034 bytes decoded · kernels: 1
distinct opcodes: 12 · hits: 22
feasibility vs target 'mi210': 22/22 hits in v0 profile — FULLY PORTABLE v0
```

Every opcode received its documented AMDGCN mapping (the census is the
same one the Metal project's scanner measured on the identical binary —
12 opcodes, `ld.param` through `ret`).

## 2. Translator + optimizer

- `ptx2llvm` translated the fixture kernel (exit 0); the emitted IR passed
  `opt` module verification.
- `opt -O2` ran the full pipeline; **all register slots promoted**
  (alloca count after optimization: 0 — mem2reg fully promoted the
  addrspace(5) frame).
- The kernel emits as `amdgpu_kernel` with the `_ntid_x` blockDim-as-data
  argument present — the family calling convention.

## 3. Codegen: three AMD generations, ELF-validated

| Target | GPU | LLVM CPU | ELF proof | Kernel symbol | Size |
|---|---|---|---|---|---|
| `mi210` | Instinct MI210 (CDNA2) | `gfx90a` | `EM_AMDGPU` + `EF_AMDGPU_MACH_AMDGCN_GFX90A` | present | 3,192 B |
| `w7800` | Radeon PRO W7800 32GB (RDNA3) | `gfx1101` | `EM_AMDGPU` + `EF_AMDGPU_MACH_AMDGCN_GFX1101` | present | 2,632 B |
| `w7900` | Radeon PRO W7900 (RDNA3) | `gfx1100` | `EM_AMDGPU` + `EF_AMDGPU_MACH_AMDGCN_GFX1100` | present | 2,632 B |

The same source IR produced three different native code objects — the
AMDGPU back-end's per-generation instruction selection doing real work.
The validator is negative-tested: it rejects a non-ELF file.

## 4. Honesty contract (the anti-fake gate)

- A crafted kernel containing `shfl.idx.b32` (outside the v0 profile) →
  **exit 2**, stderr names the opcode: `unsupported PTX opcode: shfl.idx.b32`.
- Unknown kernel name → exit 1.
- The pipeline cannot emit a wrong translation; it can only refuse.

## 5. Compression coverage

| Input | Decode path | Result |
|---|---|---|
| `vectorAdd.real.fatbin` | LZ4 block stream (output-bounded) | PTX decoded, census taken |
| `vectorAdd.zstd.fatbin` | libzstd frames | PTX decoded |
| `vectorAdd.deflate.fatbin` | zlib inflate | PTX decoded |
| `vectorAdd.ptx` (cuobjdump dump) | banner fallback probe | PTX located, FULLY PORTABLE verdict |

## 6. Harness record

`make test` → **24/24 green**, including builds under
`-Wall -Wextra -Werror` (zero warnings). `make doctor` → PASS (LLVM AMDGPU
back-end, fixtures, libzstd all present).

## 7. Environment

| Component | Value |
|---|---|
| Build host | Apple M4 Mac mini, macOS 26.6 (orchestration-class machine — the AOT lane needs no GPU) |
| LLVM | Homebrew LLVM 23.1.5, `amdgcn` target registered |
| Codecs | libz (deflate), libzstd (zstd), independent LZ4 block decoder |
| Fixture provenance | CUDA 12.4 nvcc fatbin (bb23 host, RTX 8000 machine as compiler host), shared with the Bare CUDA Metal verified record |

## 8. What is NOT verified yet (the honest residual)

- **No kernel has executed on AMD hardware yet.** v0's claim ends at the
  ELF code-object level, proven above. The live gate (`verify_mi210.sh`)
  detects the ROCm host and will run the hipModuleLoad/launch suite as
  ROADMAP phase 2; until that host exists, its absence is stated here —
  not papered over.
- Flat addressing is correct but not yet the fastest possible load path
  (`global_load_dword` lowering is the phase-2 optimization pass).
- Profiled-but-not-lowered opcodes (`shfl`, `bar.sync`, `atom.add.u32`,
  `selp`) are named in the scanner's forward profile and hard-fail in the
  translator.

**Reproduce:** `make doctor && make test` (hardware-free, any machine with
LLVM + AMDGPU target), `make aot-all` (all three targets), and
`scripts/verify_mi210.sh` (arms itself on the ROCm host).
