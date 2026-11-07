# Roadmap — Bare CUDA ROCm

Each phase ships with its own acceptance gate and updates VERIFIED.md with
live measurements. No phase is declared done by intention — only by a green,
structured harness.

## Phase 1 (shipped, v0): the hardware-free AOT lane

- [x] Portability scanner over real nvcc 12.4 fatbins (LZ4 / zstd / deflate /
      cuobjdump dumps) with per-target AMD feasibility profiles
- [x] PTX → LLVM IR translator (vectorAdd-class profile), honesty-contract
      hard-fails
- [x] Full LLVM AOT pipeline (`opt -O2` → `llc -march=amdgcn`) to native
      code objects for **three** AMD generations
- [x] ELF-level validator (machine + per-target mach flag + kernel symbol),
      negative-tested
- [x] Target pull-down (mi210 / w7800 / w7900) — one file per GPU
- [x] 24-check structured harness, warning-free `-Werror` builds
- [x] `make doctor` toolchain audit

## Phase 2 (next): the hardware gate on the MI210

Trigger: an SSH-reachable ROCm host with an Instinct MI210. The lane is
already scaffolded (`scripts/verify_mi210.sh` auto-detects and refuses to
fake anything).

- [ ] host-load: `hipModuleLoad(build/mi210/vectorAdd.co)` + launch +
      element-verified vs CPU truth (the v1 acceptance suite)
- [ ] Wave64 census: grid-stride + shared-memory reduction over 64-lane waves
- [ ] zero-copy: `hipHostMalloc` path vs device copy, bandwidth measured on
      GPU clocks (the Metal sibling's measurement discipline, ported)
- [ ] RDNA3 cross-check: same suites on W7800 (gfx1101) — dual Wave32/Wave64
      behavior recorded

## Phase 2.5 (shipped): the ISA tuning ladder — F4 in the family plan

- [x] `make isa-tune` — baseline (-O2 codegen) vs tuned (`TARGET_TUNE`:
      `-O3`, `-mattr` pinning) code objects from the same optimized IR,
      both ELF-validated
- [x] Machine-instruction measurement (`test/isa_report.sh`): mi210 297
      instructions, w7800/w7900 146 — **honest tie on the v0 kernel**
- [x] Tuning wins are deferred to Phase 3 on purpose: bigger kernels are
      where codegen level changes scheduling; the ladder measures and
      reports rather than claiming

## Phase 3: the deeper census

The scanner's verdict orders this work — the measured worklist, not a
wishlist:

- [ ] shared memory: `ld.local/st.local/bar.sync` → AMDGCN `ds_*` + `s_barrier`
- [ ] atomics: `atom.add.u32` → `buffer_atomic_add`
- [ ] shuffles: `shfl.*` → `ds_bpermute`/dpp (Wave64 on gfx90a; Wave32 on gfx11)
- [ ] multi-kernel modules + typed parameter binding (mirroring the Metal
      sibling's v2.3 design)
- [ ] RCCL collectives over the Infinity Fabric pair on the 4× MI210 stack —
      the multi-GPU lane the Metal sibling never needed

## R7 (roadmap): the source lane

Unmodified CUDA C++ source compilation (the breadth that lets
llama.cpp/GROMACS/AMReX-class applications build against the layer):
`hipify-clang`/`hipify-perl` rewrite CUDA-isms to HIP on the ROCm host,
`hipcc` compiles to native code objects — reusing our scanner for the
feasibility census and our element-verified harness for the gate. Host-gated
(R1 hardware first); staged here honestly rather than faked.

## Phase 4: the family bridge

- [ ] shared harness format with Bare CUDA Metal: one spec → both backends,
      so a CUDA workload's acceptance record spans Apple silicon and AMD
- [ ] cuBLAS-class surface: hipBLAS / rocBLAS mapping (the analog of the
      Metal sibling's cuBLAS→MPS port, A/B-verified)

## Target registry (extensible by design)

Adding a GPU = one `targets/<name>.mk` file:

```
TARGET_NAME      = <key>
TARGET_GPU       = marketing name
TARGET_ARCH      = silicon family
TARGET_LLVM_CPU  = llc -mcpu identifier
TARGET_WAVE      = lane model
TARGET_ROLE      = verification order
```

…plus one key in `targets/registry.mk`. The scanner profile, the validator's
mach-flag map, and the harness pick it up automatically.
