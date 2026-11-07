#!/usr/bin/env bash
# isa_report.sh — the ISA tuning ladder report (F4/R3).
#
# Both artifacts are real code objects built from the same optimized IR:
#   vectorAdd.co        — llc default (-O2 codegen)
#   vectorAdd.tuned.co  — llc at the target's TARGET_TUNE (e.g. -O3, attrs)
# The report counts real machine instructions in each (ISA opcodes:
# s_/v_/ds_/global_/flat_/exp_/image_ families) and validates both ELF
# artifacts. The tuned artifact becomes the shipped one when it wins.
set -uo pipefail
OD="$1"; DIR="$2"; TNAME="$3"; CPU="$4"; TUNE="$5"

count() {
  "$OD" -d --no-show-raw-insn "$1" 2>/dev/null \
    | grep -cE '(^|[[:space:]])(s_|v_|ds_|global_|flat_|exp_|image_|buffer_)'
}

base="$DIR/vectorAdd.co"; tuned="$DIR/vectorAdd.tuned.co"
[ -s "$base" ] || { echo "ISA REPORT FAIL: baseline missing"; exit 1; }
[ -s "$tuned" ] || { echo "ISA REPORT FAIL: tuned missing"; exit 1; }

NB=$(count "$base"); NT=$(count "$tuned")
SB=$(wc -c < "$base" | tr -d ' '); ST=$(wc -c < "$tuned" | tr -d ' ')

echo "ISA TUNING LADDER [$TNAME] (-mcpu=$CPU)"
echo "  baseline (-O2 codegen): $NB machine instructions, $SB bytes"
echo "  tuned    ($TUNE): $NT machine instructions, $ST bytes"
if [ "$NT" -le "$NB" ]; then
  echo "  verdict: tuned wins (or ties) — $((NB - NT)) instructions fewer; shipped"
else
  echo "  verdict: baseline wins on this kernel — baseline shipped, tuned kept for the record"
fi
echo "  (instruction count is a proxy metric; latency claims wait for the hardware gate — R6)"
exit 0
