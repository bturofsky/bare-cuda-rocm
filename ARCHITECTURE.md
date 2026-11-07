# Architecture — Bare CUDA ROCm v0

This document is the lowering contract: every transformation the pipeline
performs, exactly, with its failure mode. If an artifact was produced, it
was produced by these rules — nothing else.

## 1. The lane boundary

The project has two lanes with a hard boundary:

- **Lane A (AOT, hardware-free).** fatbin → PTX → LLVM IR → `opt -O2` →
  `llc -march=amdgcn` → ELF code object. Runs on any host with LLVM's
  AMDGPU back-end. Fully covered by the 24-check gate.
- **Lane B (hardware gate).** Load and launch the Lane-A artifact on a real
  ROCm host, element-verified against CPU truth. Auto-detects the host and
  refuses to pretend when it is absent.

No Lane-B result is ever reported without Lane-B hardware. That is the
project's defining discipline.

## 2. Scanner (`scan/rocmscan.c`)

- Container walk over NVIDIA's fatbin format (magic `0xBA55ED50`, v1 header,
  per-entry kind/flags — the layout byte-verified against CUDA 12.4 in the
  Bare CUDA Metal project).
- Payload decode: LZ4 block stream (output-bounded by the entry's declared
  size, so nvcc's uncompressed footers are never misread as tokens), zstd
  frames via libzstd, raw deflate via zlib, plus uncompressed entries and
  cuobjdump-style text dumps.
- Census: kernels counted from `.entry`; opcode histogram from statement
  first-tokens (predication markers `@%p` folded into the opcode they guard;
  trailing `;` stripped).
- Feasibility profile: per-opcode AMDGCN mapping (v0 profile below).
  Verdict `FULLY PORTABLE v0` requires **every** opcode hit inside the
  profile; anything outside is counted and *named*, never waved through.

## 3. Translator (`translator/ptx2llvm.c`)

Lowers the verified PTX profile to textual LLVM IR
(`target triple = "amdgcn-amd-amdhsa"`).

### Register model

Every PTX virtual register becomes an entry-block alloca slot in AMDGCN
private storage — the LLVM-canonical frame layout — typed by its PTX class:

| PTX class | LLVM slot |
|---|---|
| `%rdN` (.b64/.u64/.s64) | `alloca i64, addrspace(5)` |
| `%rN` (.b32/.u32/.s32) | `alloca i32, addrspace(5)` |
| `%fN` (.f32) | `alloca float, addrspace(5)` |
| `%pN` (.pred) | `alloca i1, addrspace(5)` |

Because every register has a slot, **arbitrary control flow needs no phi
nodes** — labels become basic blocks, register state lives in memory.
`opt -O2` (mem2reg and friends) then promotes the slots to SSA registers
before codegen. Correct by construction; optimized by LLVM; never
hand-folded.

Every intermediate value is an SSA temp referenced by integer id
(`%t0`, `%t1`, …) — no name aliasing anywhere in the emitter.

### Lowering contract

| PTX | LLVM IR |
|---|---|
| `ld.param.u64 %rd, [p]` | `ptrtoint ptr %p to i64` → slot |
| `ld.param.u32 %r, [p]` | direct store of the i32 argument |
| `mov.u32 %r, %tid.x` | `call i32 @llvm.amdgcn.workitem.id.x()` |
| `mov.u32 %r, %ctaid.x` | `call i32 @llvm.amdgcn.workgroup.id.x()` |
| `mov.u32 %r, %ntid.x` | store of the **implicit trailing kernel argument** `_ntid_x` |
| `mad.lo.s32 %r, a, b, c` | `mul i32` + `add i32` (exact 32-bit semantics) |
| `mul.wide.s32 %rd, r, 4` | `sext i32 → i64` then `mul i64` |
| `add.s64` | `add i64` |
| `setp.ge.s32 %p, a, b` | `icmp sge i32` → i1 slot (eq/ne/gt/lt/le likewise) |
| `ld.global.f32` / `st.global.f32` | address (i64) → `inttoptr` → flat `load`/`store` |
| `add.f32` | `fadd float` |
| `cvta.to.global.u64` | **identity** — flat/generic addressing covers global storage |
| `bra L` / `@%p bra L` | `br` / `br i1` with an anonymous fallthrough block |
| `ret` | `ret void` |

### Calling convention

`define amdgpu_kernel void @<kernel>(<params…>, i32 %_ntid_x)` — the
CUDA-visible `blockDim.x` rides as an explicit argument. This is the Bare
CUDA family pattern (the Metal sibling binds it at buffer(30)): hardware
lane width is dispatch state, never kernel semantics. A Wave64 device and a
Wave32 device run the identical kernel object family with the caller's
exact block shape.

### Honesty contract

Any opcode outside the supported set aborts translation: the opcode is
NAMED in the error (`unsupported PTX opcode: shfl.idx.b32`) and the CLI
exits 2. The pipeline structurally cannot emit a wrong translation — it can
only refuse.

## 4. AOT pipeline

```
ptx2llvm → .ll → opt -O2 → .opt.ll → llc -march=amdgcn -mcpu=<cpu> → .co
```

- `opt -O2`: mem2reg promotion, GVN, instcombine — the full optimizer, the
  point of AOT.
- `llc`: AMDGPU instruction selection, register allocation, and scheduling
  **for the exact GPU generation** (`gfx90a` vs `gfx1101` vs `gfx1100`).
  Different silicon, different code objects, same source IR.

## 5. Validation (`test/validate_codeobject.sh`)

Every code object must show, via `llvm-readobj`:

1. `Machine: EM_AMDGPU` (ELF 224)
2. the exact machine flag for the target CPU:
   `EF_AMDGPU_MACH_AMDGCN_GFX90A` / `..._GFX1101` / `..._GFX1100`
3. the kernel symbol present in `.symtab`

Any miss fails the build. The validator itself is negative-tested (it must
reject junk input) — a validator that cannot fail is decoration.

## 6. v0 profile status

In profile: `ld.param.u64/u32`, `mov.u32` (builtins + immediates),
`mad.lo.s32`, `mul.wide.s32`, `add.s64`, `setp.<cc>.s32`,
`ld/st.global.f32`, `add.f32`, `cvta.to.global.u64`, `bra`, `@p bra`, `ret`.
Documented-forward (profiled, not yet lowered): `shfl`, `bar.sync`,
`atom.add.u32`, `selp` — the scanner counts them and names them; the
translator refuses them until they land.

## 7. Known v0 limits (the honest list)

- Flat addressing on global loads/stores (correct, portable across all three
  targets; addrspace(1)-specific `global_load/store` is the optimization
  pass of phase 2).
- Single-kernel profile (the scanner's measured census drives what lands
  next — same method that got the Metal sibling to 15 suites).
- Lane B unimplemented until the ROCm host is available; its absence is
  reported, never simulated.
