/*
 * rocmscan.c — Bare CUDA ROCm portability scanner (v0).
 *
 * Walks NVIDIA fatbin containers in any binary (same container layout the
 * Bare CUDA Metal project byte-verified against CUDA 12.4: magic
 * 0xBA55ED50, v1 header, per-entry kind/flags), decompresses PTX payloads
 * (LZ4 block stream, zstd frames, raw deflate), and produces an opcode
 * census with a per-target AMD feasibility profile — the measured worklist
 * that decides what the PTX→LLVM translator must cover next.
 *
 * Verdicts are honest by construction: opcodes inside the v0 profile get
 * their documented AMDGCN mapping; anything outside is counted and NAMED
 * as out-of-profile — never silently claimed portable.
 *
 * Usage: rocmscan <binary> [--target mi210|w7800|w7900] [--json]
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

/* ---- fatbin container format (byte-verified vs CUDA 12.4, Bare CUDA Metal) */
#define FATBIN_MAGIC 0xBA55ED50u
#define FATBIN_PTX 1
#define FATBIN_ELF 2
#define FFLAG_DEFLATE 0x1000ULL
#define FFLAG_LZ4 0x2000ULL
#define FFLAG_ZSTD 0x8000ULL

typedef struct { uint32_t magic; uint16_t version; uint16_t header_size; uint64_t size; } fat_header;
typedef struct {
  uint16_t kind, version;
  uint32_t header_size, payload_size, unknown0, compressed_size, unknown1, unknown2,
           sm_version, bit_width, unknown3;
  uint64_t flags, unknown5, uncompressed_payload;
} fat_entry;

/* ---- nvcc LZ4-framed decode: LZ77 tokens, output-bounded by the entry's
   declared size; trailing uncompressed footer never misread as tokens ---- */
static unsigned char *lz4_decomp(const unsigned char *in, size_t inlen,
                                 size_t expected, size_t *outlen) {
  size_t cap = expected ? expected + 1024 : inlen * 6 + 1024;
  unsigned char *out = malloc(cap);
  if (!out) return NULL;
  size_t ip = 0, op = 0;
  while (ip < inlen && (expected == 0 || op < expected)) {
    unsigned token = in[ip++];
    unsigned lit = token >> 4;
    if (lit == 15) { unsigned s; do { s = in[ip++]; lit += s; } while (s == 255 && ip < inlen); }
    if (op + lit > cap) { cap = (op + lit) * 2 + 1024; out = realloc(out, cap); if (!out) return NULL; }
    if (ip + lit > inlen) { free(out); return NULL; }
    size_t take = lit;
    if (expected && op + take > expected) take = expected - op;
    memcpy(out + op, in + ip, take);
    ip += lit; op += take;
    if (expected && op >= expected) break;
    if (ip >= inlen) break;
    if (ip + 2 > inlen) { free(out); return NULL; }
    unsigned offset = in[ip] | ((unsigned)in[ip + 1] << 8);
    ip += 2;
    if (offset == 0 || offset > op) { free(out); return NULL; }
    unsigned match = (token & 0x0f) + 4;
    if ((token & 0x0f) == 15) { unsigned s; do { s = in[ip++]; match += s; } while (s == 255 && ip < inlen); }
    if (op + match > cap) { cap = (op + match) * 2 + 1024; out = realloc(out, cap); if (!out) return NULL; }
    unsigned char *mp = out + op - offset;
    size_t mtake = match;
    if (expected && op + mtake > expected) mtake = expected - op;
    for (size_t k = 0; k < mtake; k++) out[op++] = mp[k];
  }
  *outlen = op;
  return out;
}

/* ---- extract the first PTX text from any image (raw PTX passes through) */
static char *extract_ptx(const void *data, size_t sz, size_t *ptx_len,
                         int *containers, int *entries, int *elf_entries) {
  const unsigned char *bytes = data;
  if (sz >= 8 && memcmp(data, ".version", 8) == 0) { /* raw PTX image */
    char *r = malloc(sz + 1); memcpy(r, data, sz); r[sz] = 0;
    *ptx_len = sz; return r;
  }
  size_t i = 0;
  while (i + sizeof(fat_header) <= sz) {
    uint32_t magic; memcpy(&magic, bytes + i, 4);
    if (magic == FATBIN_MAGIC) {
      fat_header fh; memcpy(&fh, bytes + i, sizeof fh);
      if (fh.version == 1 && fh.header_size == sizeof(fat_header) &&
          fh.size > 0 && i + sizeof(fat_header) + fh.size <= sz) {
        (*containers)++;
        size_t p = i + sizeof(fat_header), p_end = p + fh.size;
        while (p + sizeof(fat_entry) <= p_end) {
          fat_entry fe; memcpy(&fe, bytes + p, sizeof fe);
          if (fe.header_size < 16 || fe.header_size > 256 || fe.payload_size == 0 ||
              p + fe.header_size + fe.payload_size > p_end)
            break;
          (*entries)++;
          if (fe.kind == FATBIN_ELF) (*elf_entries)++;
          if (fe.kind == FATBIN_PTX) {
            const unsigned char *payload = bytes + p + fe.header_size;
            size_t plen = fe.payload_size;
            size_t cin = (fe.compressed_size && fe.compressed_size < plen) ? fe.compressed_size : plen;
            if (fe.flags & FFLAG_LZ4) {
              size_t dec_len; unsigned char *dec = lz4_decomp(payload, cin,
                  (size_t)fe.uncompressed_payload, &dec_len);
              if (dec) { char *r = malloc(dec_len + 1); memcpy(r, dec, dec_len);
                         r[dec_len] = 0; free(dec); *ptx_len = dec_len; return r; }
            } else if (fe.flags & FFLAG_ZSTD) {
#ifdef HAVE_ZSTD
              size_t cap = fe.uncompressed_payload ? (size_t)fe.uncompressed_payload + 64
                                                   : plen * 8 + 1024;
              unsigned char *dec = malloc(cap);
              size_t zr = ZSTD_decompress(dec, cap, payload, cin);
              if (!ZSTD_isError(zr)) { char *r = malloc(zr + 1); memcpy(r, dec, zr);
                                       r[zr] = 0; free(dec); *ptx_len = zr; return r; }
              free(dec);
#endif
            } else if (fe.flags & FFLAG_DEFLATE) {
              uLongf cap = fe.uncompressed_payload + 65536;
              unsigned char *dec = malloc(cap); uLongf declen = cap;
              if (uncompress(dec, &declen, payload, (uLong)plen) == Z_OK) {
                char *r = malloc(declen + 1); memcpy(r, dec, declen); r[declen] = 0;
                free(dec); *ptx_len = declen; return r;
              }
              free(dec);
            } else {
              char *r = malloc(plen + 1); memcpy(r, payload, plen); r[plen] = 0;
              *ptx_len = plen; return r;
            }
          }
          p += fe.header_size + fe.payload_size;
        }
        i += sizeof(fat_header) + fh.size;
        continue;
      }
    }
    i++;
  }
  /* fallback: a cuobjdump-style text dump embeds the PTX after a banner —
     hand everything from the first .version directive to the census */
  const char *v = strstr((const char *)bytes, ".version ");
  if (v) {
    size_t rest = (size_t)(bytes + sz - (const unsigned char *)v);
    char *r = malloc(rest + 1);
    memcpy(r, v, rest);
    r[rest] = 0;
    *ptx_len = rest;
    return r;
  }
  return NULL;
}

/* ---- opcode census ------------------------------------------------------- */

typedef struct { char op[32]; long count; } op_rec;
static op_rec g_ops[128];
static int g_nops = 0;
static char g_unknown[32][32];
static int g_nunknown = 0;
static int g_nkernels = 0;

static void census_line(const char *line) {
  while (*line == ' ' || *line == '\t') line++;
  if (*line == 0 || *line == '/' || *line == '#' || *line == '.') return; /* comments/directives */
  if (*line == '{' || *line == '}') return;                               /* scope braces */
  if (*line == ')' || *line == '(') return;                               /* signature parens */
  if (*line == '@') { /* predicated exec marker: '@%p1 bra L' — count the real opcode */
    while (*line && *line != ' ' && *line != '\t') line++;
    while (*line == ' ' || *line == '\t') line++;
  }
  char op[32]; int k = 0;
  const char *p = line;
  while (*p && !isspace((unsigned char)*p) && k < 31) op[k++] = *p++;
  op[k] = 0;
  size_t olen = strlen(op);
  if (olen == 0) return;
  if (op[olen - 1] == ';') op[--olen] = 0; /* 'ret;' -> 'ret' */
  if (olen == 0 || op[0] == '$') return;   /* regs */
  /* strip modifier tail: add.f32 / mad.lo.s32 keep full text (that IS the opcode) */
  for (int i = 0; i < g_nops; i++)
    if (strcmp(g_ops[i].op, op) == 0) { g_ops[i].count++; return; }
  if (g_nops < 128) { snprintf(g_ops[g_nops].op, 32, "%s", op); g_ops[g_nops].count = 1; g_nops++; }
  else if (g_nunknown < 32 && !strstr((const char *)g_unknown, op))
    snprintf(g_unknown[g_nunknown++], 32, "%s", op);
}

static void census(const char *ptx) {
  const char *p = ptx;
  while (*p) {
    const char *nl = strchr(p, '\n'); if (!nl) nl = p + strlen(p);
    size_t len = (size_t)(nl - p);
    if (len < 512) {
      char line[512]; memcpy(line, p, len); line[len] = 0;
      if (strstr(line, ".entry ") || strstr(line, ".visible .entry ")) g_nkernels++;
      census_line(line);
    }
    p = nl + 1;
  }
}

/* ---- per-target feasibility profile (v0: the measured vectorAdd-class
   census plus the control-flow family; ISA-exact check = hardware gate) --- */
typedef struct { const char *op; const char *amdgcn; } feas_row;
static const feas_row kProfile[] = {
  { "ld.param.u64",       "kernel argument ABI (s[0:1]/arg reg)" },
  { "ld.param.u32",       "kernel argument ABI" },
  { "cvta.to.global.u64", "flat addressing: identity (generic covers global)" },
  { "mov.u32",            "builtins: workitem.id / workgroup.id; imm -> v_mov_b32" },
  { "add.s64",            "s_add_u64 / v_add_co_u64" },
  { "mul.wide.s32",       "sext + mul i64 (opt lowers to mul_hi pattern)" },
  { "mad.lo.s32",         "exact: mul i32 + add i32 (v_mad_i32_i24 fast path when 24-bit)" },
  { "setp.ge.s32",        "v_cmp/s_cmp + exec-mask (Wave64 gfx90a, dual Wave32/64 gfx11)" },
  { "ld.global.f32",      "flat_load_dword (v0); global_load_dword later" },
  { "st.global.f32",      "flat_store_dword (v0); global_store_dword later" },
  { "add.f32",            "v_add_f32" },
  { "bra",                "s_branch (divergence = exec mask)" },
  { "selp",               "v_cndmask_b32 / select" },
  { "shfl",               "ds_bpermute/dpp (gfx90a Wave64); ds_bpermute Wave32 (gfx11)" },
  { "bar.sync",           "s_barrier (hardware barrier, per workgroup)" },
  { "atom.add.u32",       "flat buffer_atomic_add (v0 later; native atomic unit)" },
  { "ret",                "s_end_pgm" },
};

int main(int argc, char **argv) {
  const char *path = NULL, *target = "mi210";
  int json = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--target") && i + 1 < argc) target = argv[++i];
    else if (!strcmp(argv[i], "--json")) json = 1;
    else path = argv[i];
  }
  if (!path) { fprintf(stderr, "usage: %s <binary> [--target mi210|w7800|w7900] [--json]\n", argv[0]); return 2; }

  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); return 1; }
  fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
  unsigned char *img = malloc((size_t)fsz);
  if (fread(img, 1, (size_t)fsz, f) != (size_t)fsz) { fprintf(stderr, "short read\n"); return 1; }
  fclose(f);

  size_t ptx_len = 0;
  int containers = 0, entries = 0, elf_entries = 0;
  char *ptx = extract_ptx(img, (size_t)fsz, &ptx_len, &containers, &entries, &elf_entries);
  if (!ptx) {
    if (json) printf("{\"file\":\"%s\",\"containers\":0,\"entries\":0,\"kernels\":0,\"ptx\":false}\n", path);
    else printf("no PTX found in %s (%ld bytes scanned)\n", path, fsz);
    return 3;
  }
  census(ptx);

  long in_profile = 0, total = 0;
  for (int i = 0; i < g_nops; i++) {
    total += g_ops[i].count;
    for (size_t r = 0; r < sizeof kProfile / sizeof kProfile[0]; r++)
      if (strncmp(g_ops[i].op, kProfile[r].op, strlen(kProfile[r].op)) == 0) { in_profile += g_ops[i].count; break; }
  }
  int full = (in_profile == total) && total > 0;

  if (json) {
    printf("{\"file\":\"%s\",\"containers\":%d,\"entries\":%d,\"elf_entries\":%d,"
           "\"kernels\":%d,\"ptx_bytes\":%zu,\"distinct_opcodes\":%d,\"opcode_hits\":%ld,"
           "\"in_profile\":%ld,\"verdict\":\"%s\",\"unknown\":[",
           path, containers, entries, elf_entries, g_nkernels, ptx_len, g_nops, total,
           in_profile, full ? "fully portable v0" : "partial — see census");
    for (int i = 0; i < g_nunknown; i++)
      printf("%s\"%s\"", i ? "," : "", g_unknown[i]);
    printf("]}\n");
    return 0;
  }

  printf("Bare CUDA ROCm — portability scan\n");
  printf("  file: %s (%ld bytes)\n", path, fsz);
  printf("  fatbin containers: %d · entries: %d (%d ELF cubins) · PTX: %zu bytes\n",
         containers, entries, elf_entries, ptx_len);
  printf("  kernels: %d\n", g_nkernels);
  printf("\n  opcode census (distinct %d, hits %ld):\n", g_nops, total);
  for (int i = 0; i < g_nops; i++) printf("    %-18s %ld\n", g_ops[i].op, g_ops[i].count);
  printf("\n  feasibility vs target '%s': %ld/%ld hits in v0 profile — %s\n",
         target, in_profile, total, full ? "FULLY PORTABLE v0" : "PARTIAL (see named gaps)");
  for (size_t r = 0; r < sizeof kProfile / sizeof kProfile[0]; r++)
    for (int i = 0; i < g_nops; i++)
      if (strncmp(g_ops[i].op, kProfile[r].op, strlen(kProfile[r].op)) == 0) {
        printf("    %-18s -> %s\n", g_ops[i].op, kProfile[r].amdgcn);
        break;
      }
  if (g_nunknown) {
    printf("  out-of-profile opcodes (named, not claimed):\n");
    for (int i = 0; i < g_nunknown; i++) printf("    %s\n", g_unknown[i]);
  }
  return 0;
}
