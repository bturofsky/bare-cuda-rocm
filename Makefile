# Bare CUDA ROCm — v0 build.
#
# Lane 1 (works on any machine with LLVM's AMDGPU back-end, no GPU needed):
#   scan  — portability scanner: nvcc fatbin → PTX census → per-target feasibility
#   aot   — PTX → LLVM IR → opt -O2 → llc -march=amdgcn → native .co code object
#           for the selected target (make TARGET=mi210|w7800|w7900)
#
# Lane 2 (hardware-gated, on the ROCm host — auto-skips honestly elsewhere):
#   scripts/verify_mi210.sh  — live GPU acceptance suites
#
# The target pull-down:
#   make targets        list GPUs
#   make select         interactive pull-down (persists to .target)
#   make aot            build code objects for the selected target
#   make aot-all        build for every registered target

include targets/registry.mk

ifneq ($(TARGET),)
ACTIVE_TARGET := $(TARGET)
else ifneq ($(wildcard .target),)
ACTIVE_TARGET := $(shell cat .target)
else
ACTIVE_TARGET := $(DEFAULT_TARGET)
endif

include targets/$(ACTIVE_TARGET).mk

LLVM_BIN ?= /opt/homebrew/opt/llvm/bin
LLC   := $(LLVM_BIN)/llc
OPT   := $(LLVM_BIN)/opt
RDOBJ := $(LLVM_BIN)/llvm-readobj

BUILD := build/$(ACTIVE_TARGET)

.PHONY: all targets select doctor scan aot aot-all isa-tune test clean

all: doctor scan aot

targets:
	@echo "Bare CUDA ROCm — target pull-down"
	@echo "================================="
	@for t in $(TARGETS); do \
	  gpu=$$(grep -E '^TARGET_GPU' targets/$$t.mk | cut -d= -f2- | sed 's/^ *//'); \
	  arch=$$(grep -E '^TARGET_ARCH' targets/$$t.mk | cut -d= -f2- | sed 's/^ *//'); \
	  cpu=$$(grep -E '^TARGET_LLVM_CPU' targets/$$t.mk | cut -d= -f2- | sed 's/^ *//'); \
	  wave=$$(grep -E '^TARGET_WAVE' targets/$$t.mk | cut -d= -f2- | sed 's/^ *//'); \
	  mark=" "; [ "$$t" = "$(ACTIVE_TARGET)" ] && mark="*"; \
	  printf "%s %-8s %-30s %-13s llvm: %-8s wave: %s\n" "$$mark" "$$t" "$$gpu" "$$arch" "$$cpu" "$$wave"; \
	done
	@echo "active: $(ACTIVE_TARGET)  (override: make TARGET=<name> ... | make select)"

select:
	@bash scripts/select_target.sh

doctor:
	@bash scripts/doctor.sh "$(LLC)" "$(OPT)" "$(RDOBJ)"
	@echo "[doctor] active target: $(ACTIVE_TARGET) — $(TARGET_GPU) ($(TARGET_ARCH), llvm -mcpu=$(TARGET_LLVM_CPU), wave $(TARGET_WAVE))"

scan: scan/rocmscan
	@./scan/rocmscan fixtures/vectorAdd.real.fatbin

scan/rocmscan: scan/rocmscan.c
	$(CC) -O2 -std=c11 -Wall -Wextra -Werror -DHAVE_ZSTD -I/opt/homebrew/include -o $@ scan/rocmscan.c -lz -L/opt/homebrew/lib -lzstd -L/opt/homebrew/opt/lz4/lib -llz4

aot: translator/ptx2llvm.bin $(BUILD)
	@mkdir -p $(BUILD)
	@./translator/ptx2llvm.bin fixtures/vectorAdd.ptx _Z9vectorAddPKfS0_Pfi > $(BUILD)/vectorAdd.ll
	@$(OPT) -O2 -o $(BUILD)/vectorAdd.opt.ll $(BUILD)/vectorAdd.ll
	@$(LLC) -march=amdgcn -mcpu=$(TARGET_LLVM_CPU) -filetype=obj \
	    -o $(BUILD)/vectorAdd.co $(BUILD)/vectorAdd.opt.ll
	@bash test/validate_codeobject.sh $(RDOBJ) $(BUILD)/vectorAdd.co \
	    "$(TARGET_NAME)" "$(TARGET_GPU)" "$(TARGET_LLVM_CPU)"

aot-all:
	@for t in $(TARGETS); do echo "== target: $$t =="; $(MAKE) --no-print-directory TARGET=$$t aot || exit 1; done

# F4/R3 — the ISA tuning ladder: same optimized IR, two codegen levels,
# measured in real machine instructions. The ladder is per-target via
# TARGET_TUNE in targets/*.mk.
isa-tune: translator/ptx2llvm.bin $(BUILD)
	@mkdir -p $(BUILD)
	@./translator/ptx2llvm.bin fixtures/vectorAdd.ptx _Z9vectorAddPKfS0_Pfi > $(BUILD)/vectorAdd.ll
	@$(OPT) -O2 -o $(BUILD)/vectorAdd.opt.ll $(BUILD)/vectorAdd.ll
	@$(LLC) -march=amdgcn -mcpu=$(TARGET_LLVM_CPU) -filetype=obj \
	    -o $(BUILD)/vectorAdd.co $(BUILD)/vectorAdd.opt.ll
	@$(LLC) -march=amdgcn -mcpu=$(TARGET_LLVM_CPU) $(TARGET_TUNE) -filetype=obj \
	    -o $(BUILD)/vectorAdd.tuned.co $(BUILD)/vectorAdd.opt.ll
	@bash test/validate_codeobject.sh $(RDOBJ) $(BUILD)/vectorAdd.tuned.co \
	    "$(TARGET_NAME)-tuned" "$(TARGET_GPU)" "$(TARGET_LLVM_CPU)"
	@bash test/isa_report.sh "$(LLVM_BIN)/llvm-objdump" $(BUILD) \
	    "$(TARGET_NAME)" "$(TARGET_LLVM_CPU)" "$(TARGET_TUNE)"

translator/ptx2llvm.bin: translator/ptx2llvm.c translator/ptx2llvm_main.c
	$(CC) -O2 -std=c11 -Wall -Wextra -Werror -o $@ translator/ptx2llvm.c translator/ptx2llvm_main.c

test: all
	@bash test/run_tests.sh "$(LLVM_BIN)"

clean:
	rm -rf build scan/rocmscan translator/ptx2llvm.bin

$(BUILD):
	mkdir -p $(BUILD)
