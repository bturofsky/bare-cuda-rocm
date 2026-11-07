#!/usr/bin/env bash
# select_target.sh — the interactive target pull-down.
# Writes the chosen GPU to .target; every later `make` uses it.
#   ./scripts/select_target.sh          interactive
#   make select                         same thing
cd "$(dirname "$0")/.."

mapfile -t targets < <(grep -E '^TARGETS' targets/registry.mk | sed 's/TARGETS *= *//' | tr ' ' '\n')

declare -A GPU ARCH CPU WAVE ROLE
for t in "${targets[@]}"; do
  GPU[$t]=$(grep -E '^TARGET_GPU'      "targets/$t.mk" | cut -d= -f2- | sed 's/^ *//')
  ARCH[$t]=$(grep -E '^TARGET_ARCH'    "targets/$t.mk" | cut -d= -f2- | sed 's/^ *//')
  CPU[$t]=$(grep -E '^TARGET_LLVM_CPU' "targets/$t.mk" | cut -d= -f2- | sed 's/^ *//')
  WAVE[$t]=$(grep -E '^TARGET_WAVE'    "targets/$t.mk" | cut -d= -f2- | sed 's/^ *//')
  ROLE[$t]=$(grep -E '^TARGET_ROLE'    "targets/$t.mk" | cut -d= -f2- | sed 's/^ *//')
done

current=$(cat .target 2>/dev/null || echo "${targets[0]}")
echo "Bare CUDA ROCm — select the build target"
echo "========================================"
i=1
for t in "${targets[@]}"; do
  mark=" "
  [ "$t" = "$current" ] && mark="*"
  printf " %s %d) %-6s %-34s %-13s llvm: %-8s wave: %s\n" \
    "$mark" "$i" "$t" "${GPU[$t]}" "${ARCH[$t]}" "${CPU[$t]}" "${WAVE[$t]}"
  i=$((i+1))
done
printf "   q) keep current (%s)\n" "$current"
read -r -p "target [1-${#targets[@]}]: " pick
if [[ "$pick" =~ ^[0-9]+$ ]] && [ "$pick" -ge 1 ] && [ "$pick" -le "${#targets[@]}" ]; then
  sel="${targets[$((pick-1))]}"
  echo "$sel" > .target
  echo "selected: $sel — make aot / make test now build for ${GPU[$sel]}"
else
  echo "kept: $current"
fi
