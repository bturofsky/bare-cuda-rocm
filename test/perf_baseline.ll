; perf_baseline.ll — the NATIVE-quality baseline for the ROCm ISA-proxy gate.
; This is what a hand-written ROCm kernel lowers to: typed pointers, GEP
; addressing, direct workitem/workgroup builtins — no PTX register model.
; The gate compares machine-instruction counts of this baseline against our
; translated kernel (same workload). STATIC PROXY ONLY: latency claims wait
; for the hardware gate (R6).
target triple = "amdgcn-amd-amdhsa"

declare i32 @llvm.amdgcn.workitem.id.x()
declare i32 @llvm.amdgcn.workgroup.id.x()

define amdgpu_kernel void @vectorAdd_native(ptr %a, ptr %b, ptr %c, i32 %n, i32 %ntid) {
entry:
  %gid = call i32 @llvm.amdgcn.workgroup.id.x()
  %tid = call i32 @llvm.amdgcn.workitem.id.x()
  %off = mul i32 %gid, %ntid
  %i = add i32 %off, %tid
  %cond = icmp ult i32 %i, %n
  br i1 %cond, label %body, label %end
body:
  %ia64 = sext i32 %i to i64
  %ae = getelementptr float, ptr %a, i64 %ia64
  %be = getelementptr float, ptr %b, i64 %ia64
  %av = load float, ptr %ae
  %bv = load float, ptr %be
  %s = fadd float %av, %bv
  %ce = getelementptr float, ptr %c, i64 %ia64
  store float %s, ptr %ce
  br label %end
end:
  ret void
}
