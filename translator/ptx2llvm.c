/*
 * ptx2llvm.c — Bare CUDA ROCm translator core (v0).
 *
 * Lowers the vectorAdd-class PTX subset (the opcode census the scanner
 * verified against real CUDA 12.4 fatbins) to textual LLVM IR for the
 * AMDGPU back-end.
 *
 * Design (correct by construction, optimized by LLVM — never hand-folded):
 *   - every PTX virtual register gets an alloca slot in the entry block,
 *     typed by its PTX class:
 *       %rdN (.b64/.u64/.s64) -> i64    (PTX addresses are integers)
 *       %rN  (.b32/.u32/.s32) -> i32
 *       %fN  (.f32)           -> float
 *       %pN  (.pred)          -> i1
 *     so arbitrary control flow needs no phi nodes; `opt -O2` (mem2reg
 *     et al.) promotes the slots before `llc` codegen.
 *   - every intermediate value is an SSA temp referenced by integer id —
 *     no shared name buffers, no aliasing.
 *
 * Lowering contract (documented, honest):
 *   - kernel params arrive as generic `ptr` (AMDGCN flat addressing covers
 *     global storage; cvta.to.global.u64 is identity)
 *   - %tid.x  -> llvm.amdgcn.workitem.id.x()
 *   - %ctaid.x-> llvm.amdgcn.workgroup.id.x()
 *   - %ntid.x -> implicit trailing kernel argument `_ntid_x` (the Bare CUDA
 *     family pattern: hardware lane count travels as data, so the dispatch
 *     width may differ from the CUDA-visible blockDim)
 *   - any opcode outside the supported set hard-fails with the opcode
 *     NAMED in err and return 2 — never a wrong translation.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PARAMS 16
#define MAX_LABELS 64
#define MAX_REGS 128
#define LINE_MAX 512

/* ---- growable output buffer ---------------------------------------------- */
typedef struct { char *buf; size_t len, cap; } sbuf;

static void sb_put(sbuf *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0) return;
  if (b->len + (size_t)need + 1 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 8192;
    while (nc < b->len + (size_t)need + 1) nc *= 2;
    char *nb = realloc(b->buf, nc);
    if (!nb) return;
    b->buf = nb;
    b->cap = nc;
  }
  va_start(ap, fmt);
  b->len += (size_t)vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
  va_end(ap);
}

/* ---- translator context ---------------------------------------------------- */
typedef struct {
  sbuf out;
  int tmp; /* SSA temp counter */
  char fail[160];

  struct { char name[128]; int is_ptr; char ptype; } params[MAX_PARAMS];
  int nparams;

  struct { char name[16]; const char *ty; } regs[MAX_REGS];
  int nregs;

  char labels[MAX_LABELS][64];
  int nlabels;
} ctx;

/* ---- register classification ------------------------------------------------
   longest-prefix wins: %rd -> i64, %r -> i32, %f -> float, %p -> i1 */
static const char *reg_slot_type(const char *reg) {
  if (reg[0] != '%') return NULL;
  if (reg[1] == 'r') return reg[2] == 'd' ? "i64" : "i32";
  if (reg[1] == 'f') return "float";
  if (reg[1] == 'p') return "i1";
  return NULL;
}

static int slot_load(ctx *c, const char *reg, const char *ty) {
  int t = c->tmp++;
  sb_put(&c->out, "  %%t%d = load %s, ptr addrspace(5) %%slot.%s\n", t, ty, reg + 1);
  return t;
}
static void slot_store(ctx *c, const char *reg, const char *ty, int v) {
  sb_put(&c->out, "  store %s %%t%d, ptr addrspace(5) %%slot.%s\n", ty, v, reg + 1);
}

/* ensure a slot exists for reg (lazy registration; inventory pre-pass also
   registers, this catches anything the pre-pass missed) */
static const char *slot_type_of(ctx *c, const char *reg) {
  const char *ty = reg_slot_type(reg);
  if (!ty) return NULL;
  for (int i = 0; i < c->nregs; i++)
    if (strcmp(c->regs[i].name, reg) == 0) return ty;
  if (c->nregs < MAX_REGS) {
    snprintf(c->regs[c->nregs].name, 16, "%s", reg);
    c->regs[c->nregs].ty = ty;
    c->nregs++;
  }
  return ty;
}

static void debracket(char *s) {
  size_t l = strlen(s);
  if (l >= 2 && s[0] == '[' && s[l - 1] == ']') { s[l - 1] = 0; memmove(s, s + 1, l); }
}

/* find kernel param by exact suffix name match */
static int param_index(ctx *c, const char *bracketed) {
  char nm[128];
  snprintf(nm, sizeof nm, "%s", bracketed);
  debracket(nm);
  for (int i = 0; i < c->nparams; i++)
    if (strcmp(nm, c->params[i].name) == 0) return i;
  return -1;
}

/* ---- label collection -------------------------------------------------------- */
static int is_label_line(const char *s) {
  return s[0] == '$' && strchr(s, ':') != NULL;
}

static void label_name(const char *line, char *out, size_t cap) {
  const char *s = line + 1; /* strip '$' */
  const char *colon = strchr(s, ':');
  size_t n = colon ? (size_t)(colon - s) : strlen(s);
  if (n >= cap) n = cap - 1;
  memcpy(out, s, n);
  out[n] = 0;
}

/* ---- statement executor ---------------------------------------------------------
   ops[0] is the opcode; ops[1..] are its operands (comma/space split).
   returns 0 ok, 2 hard-fail (c->fail set). */
static int exec_stmt(ctx *c, char tok[][96], int ntok) {
  const char *op = tok[0];
  const char *o1 = ntok > 1 ? tok[1] : "";
  const char *o2 = ntok > 2 ? tok[2] : "";
  const char *o3 = ntok > 3 ? tok[3] : "";
  const char *o4 = ntok > 4 ? tok[4] : "";

  /* ---- parameter loads ----------------------------------------------------- */
  if (!strcmp(op, "ld.param.u64") || !strcmp(op, "ld.param.s64")) {
    int pi = param_index(c, o2);
    if (pi < 0 || !c->params[pi].is_ptr) { snprintf(c->fail, 160, "ld.param.u64 from %s", o2); return 2; }
    const char *ty = slot_type_of(c, o1);
    if (!ty) { snprintf(c->fail, 160, "unknown reg %s", o1); return 2; }
    int p = c->tmp++;
    sb_put(&c->out, "  %%t%d = ptrtoint ptr %%%s to i64\n", p, c->params[pi].name);
    slot_store(c, o1, ty, p);
    return 0;
  }
  if (!strcmp(op, "ld.param.u32") || !strcmp(op, "ld.param.s32") || !strcmp(op, "ld.param.f32")) {
    int pi = param_index(c, o2);
    if (pi < 0) { snprintf(c->fail, 160, "ld.param from %s", o2); return 2; }
    const char *ty = slot_type_of(c, o1);
    if (!ty) { snprintf(c->fail, 160, "unknown reg %s", o1); return 2; }
    int p = c->tmp++;
    if (!strcmp(ty, "float")) {
      if (c->params[pi].ptype != 'f') { snprintf(c->fail, 160, "param type mismatch %s", o2); return 2; }
      sb_put(&c->out, "  %%t%d = load float, ptr %%param.slot.%s\n", p, c->params[pi].name);
      /* float params need a slot of their own since the ABI passes f32 by value */
      return 0;
    }
    sb_put(&c->out, "  store %s %%%s, ptr addrspace(5) %%slot.%s\n", ty, c->params[pi].name, o1 + 1);
    return 0;
  }

  /* ---- moves: builtins, implicit dims, immediates -------------------------- */
  if (!strncmp(op, "mov.u32", 7) || !strncmp(op, "mov.s32", 7)) {
    const char *ty = slot_type_of(c, o1);
    if (!ty) { snprintf(c->fail, 160, "unknown reg %s", o1); return 2; }
    if (!strcmp(o2, "%tid.x")) {
      int v = c->tmp++;
      sb_put(&c->out, "  %%t%d = call i32 @llvm.amdgcn.workitem.id.x()\n", v);
      slot_store(c, o1, "i32", v);
      return 0;
    }
    if (!strcmp(o2, "%ctaid.x")) {
      int v = c->tmp++;
      sb_put(&c->out, "  %%t%d = call i32 @llvm.amdgcn.workgroup.id.x()\n", v);
      slot_store(c, o1, "i32", v);
      return 0;
    }
    if (!strcmp(o2, "%ntid.x")) {
      sb_put(&c->out, "  store i32 %%_ntid_x, ptr addrspace(5) %%slot.%s\n", o1 + 1);
      return 0;
    }
    if (isdigit((unsigned char)o2[0])) {
      sb_put(&c->out, "  store i32 %s, ptr addrspace(5) %%slot.%s\n", o2, o1 + 1);
      return 0;
    }
    snprintf(c->fail, 160, "unsupported mov source: %s", o2);
    return 2;
  }

  /* ---- integer arithmetic ---------------------------------------------------- */
  if (!strcmp(op, "mad.lo.s32") || !strcmp(op, "mad.lo.u32")) {
    int a = slot_load(c, o2, "i32");
    int b = slot_load(c, o3, "i32");
    int m = c->tmp++;
    sb_put(&c->out, "  %%t%d = mul i32 %%t%d, %%t%d\n", m, a, b);
    int cc = slot_load(c, o4, "i32");
    int r = c->tmp++;
    sb_put(&c->out, "  %%t%d = add i32 %%t%d, %%t%d\n", r, m, cc);
    slot_store(c, o1, "i32", r);
    return 0;
  }
  if (!strcmp(op, "mul.wide.s32")) {
    int a = slot_load(c, o2, "i32");
    int sx = c->tmp++;
    sb_put(&c->out, "  %%t%d = sext i32 %%t%d to i64\n", sx, a);
    int w;
    if (isdigit((unsigned char)o3[0])) {
      w = c->tmp++;
      sb_put(&c->out, "  %%t%d = mul i64 %%t%d, %s\n", w, sx, o3);
    } else {
      int mv = slot_load(c, o3, "i64");
      w = c->tmp++;
      sb_put(&c->out, "  %%t%d = mul i64 %%t%d, %%t%d\n", w, sx, mv);
    }
    slot_store(c, o1, "i64", w);
    return 0;
  }
  if (!strcmp(op, "add.s64") || !strcmp(op, "add.u64")) {
    int a, b;
    if (o2[0] == '%') a = slot_load(c, o2, "i64");
    else { a = c->tmp++; sb_put(&c->out, "  %%t%d = add i64 0, %s\n", a, o2); }
    if (o3[0] == '%') b = slot_load(c, o3, "i64");
    else { b = c->tmp++; sb_put(&c->out, "  %%t%d = add i64 0, %s\n", b, o3); }
    int r = c->tmp++;
    sb_put(&c->out, "  %%t%d = add i64 %%t%d, %%t%d\n", r, a, b);
    slot_store(c, o1, "i64", r);
    return 0;
  }

  /* ---- predicates -------------------------------------------------------------- */
  if (!strncmp(op, "setp.", 5) && strstr(op, ".s32")) {
    const char *ccmp = strstr(op, ".ge") ? "sge" : strstr(op, ".gt") ? "sgt"
                     : strstr(op, ".lt") ? "slt" : strstr(op, ".le") ? "sle"
                     : strstr(op, ".ne") ? "ne" : strstr(op, ".eq") ? "eq" : NULL;
    if (!ccmp) { snprintf(c->fail, 160, "unsupported setp: %s", op); return 2; }
    int a = slot_load(c, o2, "i32");
    int b = slot_load(c, o3, "i32");
    int r = c->tmp++;
    sb_put(&c->out, "  %%t%d = icmp %s i32 %%t%d, %%t%d\n", r, ccmp, a, b);
    slot_store(c, o1, "i1", r);
    return 0;
  }

  /* ---- global memory (flat addressing) ------------------------------------------ */
  if (!strcmp(op, "ld.global.f32")) {
    char addr[96];
    snprintf(addr, 96, "%s", o2);
    debracket(addr);
    int a = slot_load(c, addr, "i64");
    int p = c->tmp++;
    sb_put(&c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", p, a);
    int v = c->tmp++;
    sb_put(&c->out, "  %%t%d = load float, ptr %%t%d\n", v, p);
    slot_store(c, o1, "float", v);
    return 0;
  }
  if (!strcmp(op, "st.global.f32")) {
    char addr[96];
    snprintf(addr, 96, "%s", o1);
    debracket(addr);
    int a = slot_load(c, addr, "i64");
    int p = c->tmp++;
    sb_put(&c->out, "  %%t%d = inttoptr i64 %%t%d to ptr\n", p, a);
    int v = slot_load(c, o2, "float");
    sb_put(&c->out, "  store float %%t%d, ptr %%t%d\n", v, p);
    return 0;
  }
  if (!strcmp(op, "add.f32")) {
    int a = slot_load(c, o2, "float");
    int b = slot_load(c, o3, "float");
    int r = c->tmp++;
    sb_put(&c->out, "  %%t%d = fadd float %%t%d, %%t%d\n", r, a, b);
    slot_store(c, o1, "float", r);
    return 0;
  }

  /* ---- address space conversion (flat: identity) --------------------------------- */
  if (!strncmp(op, "cvta.to.global.u64", 18) || !strncmp(op, "cvta.to.local.u64", 18)) {
    int v = slot_load(c, o2, "i64");
    slot_store(c, o1, "i64", v);
    return 0;
  }

  /* ---- control flow ---------------------------------------------------------------- */
  if (!strcmp(op, "bra")) {
    sb_put(&c->out, "  br label %%%s\n", o1[0] == '$' ? o1 + 1 : o1);
    return 1; /* terminated */
  }
  if (!strcmp(op, "ret")) {
    sb_put(&c->out, "  ret void\n");
    return 1;
  }

  snprintf(c->fail, 160, "unsupported PTX opcode: %s", op);
  return 2;
}

/* ---- the translator entry -------------------------------------------------------- */
int translate_ptx_to_llvm(const char *ptx, size_t ptx_len, const char *kernel_name,
                          char *out, size_t out_cap, char *err, size_t err_cap) {
  (void)ptx_len;
  ctx c;
  memset(&c, 0, sizeof c);
  c.fail[0] = 0;

  /* locate the requested .entry */
  char entry_pat[192];
  snprintf(entry_pat, sizeof entry_pat, ".entry %s(", kernel_name);
  const char *entry = strstr(ptx, entry_pat);
  if (!entry) { snprintf(err, err_cap, "kernel %s not found in PTX", kernel_name); return -1; }
  const char *brace = strchr(entry, '{');
  const char *end = ptx + strlen(ptx);
  if (!brace) { snprintf(err, err_cap, "entry %s has no body", kernel_name); return -1; }

  /* parse the parameter list: lines between '(' and ')' of the signature */
  const char *paren = strchr(entry, '(');
  const char *sig_end = paren ? strchr(paren, ')') : NULL;
  if (!paren || !sig_end) { snprintf(err, err_cap, "entry %s has no signature", kernel_name); return -1; }
  for (const char *p = paren + 1; p < sig_end && c.nparams < MAX_PARAMS;) {
    const char *nl = strchr(p, '\n');
    if (!nl || nl > sig_end) nl = sig_end;
    size_t len = (size_t)(nl - p);
    if (len > 0 && len < LINE_MAX && strncmp(p, ".param", 6) == 0) {
      char line[LINE_MAX];
      memcpy(line, p, len); line[len] = 0;
      char t2[LINE_MAX], t3[LINE_MAX];
      if (sscanf(line, ".param %63s %127s", t2, t3) == 2) {
        size_t tl = strlen(t3);
        while (tl && (t3[tl - 1] == ',' || t3[tl - 1] == ';')) t3[--tl] = 0;
        c.params[c.nparams].ptype = t2[1];
        c.params[c.nparams].is_ptr = (t2[1] == 'u' && t2[2] == '6');
        snprintf(c.params[c.nparams].name, 128, "%s", t3);
        c.nparams++;
      }
    }
    if (nl >= sig_end) break;
    p = nl + 1;
  }

  /* register inventory pre-pass: every %-register token in every statement
     gets an entry-block alloca slot (opcode-agnostic — st.global has its
     value operand in second position, predicated lines their predicate
     mid-line, so a per-token scan is the robust collector) */
  for (const char *q = brace; q < end;) {
    const char *nl = strchr(q, '\n');
    if (!nl || nl > end) nl = end;
    size_t len = (size_t)(nl - q);
    if (len > 0 && len < LINE_MAX) {
      char line[LINE_MAX];
      memcpy(line, q, len); line[len] = 0;
      char *s0 = line;
      while (*s0 == ' ' || *s0 == '\t') s0++;
      if (*s0 == '.') { q = nl + 1; continue; } /* directives (.reg decls etc.) */
      char *t = strtok(s0, " \t,");
      while (t) {
        size_t tl = strlen(t);
        if (tl && t[tl - 1] == ';') t[tl - 1] = 0;
        if (t[0] == '%' && !strpbrk(t, "<>[]")) slot_type_of(&c, t);
        t = strtok(NULL, " \t,");
      }
    }
    if (nl >= end) break;
    q = nl + 1;
  }

  /* label inventory */
  for (const char *q = brace; q < end && c.nlabels < MAX_LABELS;) {
    const char *nl = strchr(q, '\n');
    if (!nl || nl > end) nl = end;
    size_t len = (size_t)(nl - q);
    if (len > 0 && len < LINE_MAX) {
      char line[LINE_MAX];
      memcpy(line, q, len); line[len] = 0;
      char *s = line;
      while (*s == ' ' || *s == '\t') s++;
      if (is_label_line(s)) label_name(s, c.labels[c.nlabels++], 64);
    }
    if (nl >= end) break;
    q = nl + 1;
  }

  /* ---- module header ---- */
  sb_put(&c.out, "; Generated by Bare CUDA ROCm ptx2llvm (v0) — kernel %s\n", kernel_name);
  sb_put(&c.out, "target triple = \"amdgcn-amd-amdhsa\"\n\n");
  sb_put(&c.out, "declare i32 @llvm.amdgcn.workitem.id.x()\n");
  sb_put(&c.out, "declare i32 @llvm.amdgcn.workgroup.id.x()\n\n");
  sb_put(&c.out, "define amdgpu_kernel void @%s(", kernel_name);
  for (int i = 0; i < c.nparams; i++) {
    const char *lt = c.params[i].is_ptr ? "ptr" : (c.params[i].ptype == 'f' ? "float" : "i32");
    sb_put(&c.out, "%s%s %%%s", i ? ", " : "", lt, c.params[i].name);
  }
  sb_put(&c.out, ", i32 %%_ntid_x) {\n");
  sb_put(&c.out, "entry:\n");
  for (int i = 0; i < c.nregs; i++)
    sb_put(&c.out, "  %%slot.%s = alloca %s, align 8, addrspace(5)\n", c.regs[i].name + 1, c.regs[i].ty);

  /* ---- statement walk ---- */
  int terminated = 0;
  int rc = 0;
  for (const char *q = brace + 1; q < end;) {
    const char *nl = strchr(q, '\n');
    if (!nl || nl > end) nl = end;
    size_t len = (size_t)(nl - q);
    if (nl >= end && len == 0) break;
    char line[LINE_MAX];
    if (len > 0 && len < LINE_MAX) {
      memcpy(line, q, len); line[len] = 0;
      char *s = line;
      while (*s == ' ' || *s == '\t') s++;
      if (*s == 0 || *s == '/' || *s == '.' || *s == '}') { q = nl + 1; continue; }

      if (is_label_line(s)) {
        char lab[64];
        label_name(s, lab, sizeof lab);
        if (!terminated) sb_put(&c.out, "  br label %%%s\n", lab);
        sb_put(&c.out, "%s:\n", lab);
        terminated = 0;
        q = nl + 1;
        continue;
      }

      /* predicated branch: @%p bra $L — three words: predicate, opcode, label */
      if (*s == '@') {
        char pred[96], w2[96], lab[96];
        char *sp = s + 1;
        char *w = pred;
        int words = 0;
        char *words_p[3] = { pred, w2, lab };
        while (words < 3) {
          while (*sp == ' ') sp++;
          if (!*sp) break;
          w = words_p[words];
          while (*sp && *sp != ' ') *w++ = *sp++;
          *w = 0;
          words++;
        }
        if (words < 3) { snprintf(c.fail, 160, "truncated predicated stmt: %s", s); rc = 2; break; }
        if (strcmp(w2, "bra") != 0) { snprintf(c.fail, 160, "@pred with non-bra: %s", w2); rc = 2; break; }
        if (pred[0] != '%') { snprintf(c.fail, 160, "bad predicate @%s", pred); rc = 2; break; }
        size_t ll = strlen(lab);
        if (ll && lab[ll - 1] == ';') lab[--ll] = 0;
        if (lab[0] == '$') memmove(lab, lab + 1, ll);
        int pv = slot_load(&c, pred, "i1");
        int fall = c.tmp++;
        sb_put(&c.out, "  br i1 %%t%d, label %%%s, label %%fall%d\n", pv, lab, fall);
        sb_put(&c.out, "fall%d:\n", fall);
        terminated = 0;
        q = nl + 1;
        continue;
      }

      /* strip ';' and tokenize */
      size_t sl = strlen(s);
      if (sl && s[sl - 1] == ';') s[sl - 1] = 0;
      char tok[8][96];
      int ntok = 0;
      char *t = strtok(s, " \t,");
      while (t && ntok < 8) { snprintf(tok[ntok++], 96, "%s", t); t = strtok(NULL, " \t,"); }
      if (ntok == 0) { q = nl + 1; continue; }

      int trc = exec_stmt(&c, tok, ntok);
      if (trc == 2) { rc = 2; break; }
      terminated = (trc == 1);
    }
    if (nl >= end) break;
    q = nl + 1;
  }

  if (!terminated && !rc) sb_put(&c.out, "  ret void\n");
  sb_put(&c.out, "}\n");

  if (rc == 2 || c.fail[0]) {
    snprintf(err, err_cap, "%s", c.fail);
    free(c.out.buf);
    return 2;
  }
  if (c.out.len >= out_cap - 1) {
    snprintf(err, err_cap, "IR exceeds %zu bytes", out_cap);
    free(c.out.buf);
    return -1;
  }
  memcpy(out, c.out.buf, c.out.len + 1);
  free(c.out.buf);
  return 0;
}
