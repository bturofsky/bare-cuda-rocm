/*
 * ptx2llvm_main.c — CLI: translate one kernel from a PTX text to LLVM IR.
 *
 *   ptx2llvm <ptx_file> <kernel_name>   — IR on stdout
 *   exit 0 = translated · 2 = honesty-contract hard-fail (opcode named on
 *   stderr) · 1 = I/O or kernel-not-found
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int translate_ptx_to_llvm(const char *ptx, size_t ptx_len, const char *kernel_name,
                          char *out, size_t out_cap, char *err, size_t err_cap);

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <ptx_file> <kernel_name>\n", argv[0]);
    return 1;
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 1; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *ptx = malloc((size_t)sz + 1);
  if (!ptx || fread(ptx, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "read failed\n"); return 1; }
  fclose(f);
  ptx[sz] = 0;

  char *out = malloc(4u << 20);
  char err[256] = "";
  int rc = translate_ptx_to_llvm(ptx, (size_t)sz, argv[2], out, 4u << 20, err, sizeof err);
  if (rc == 0) {
    fputs(out, stdout);
  } else if (rc == 2) {
    fprintf(stderr, "ptx2llvm: honesty hard-fail: %s\n", err);
  } else {
    fprintf(stderr, "ptx2llvm: %s\n", err[0] ? err : "translation failed");
    rc = 1;
  }
  free(out);
  free(ptx);
  return rc;
}
