#!/usr/bin/env bash
# Show how the compiler lowered each decoder dispatch: instruction count,
# indirect jumps/calls (jump tables / function-pointer tables) and the number
# of conditional branches in each benchmark pass function.
set -euo pipefail
bin=${1:-build/bl_decoder_bench}
for s in $(nm "$bin" | awk '/run_pass/ {print $3}'); do
  name=$(echo "$s" | c++filt | sed -E 's/.*Variant\)([0-9]).*/\1/')
  case $name in 0) n=framing;; 1) n=handwritten;; 2) n=gen-fold;; 3) n=gen-table;; *) n=$name;; esac
  dis=$(objdump -d --no-show-raw-insn "$bin" --disassemble="$s")
  insns=$(grep -cE '^\s+[0-9a-f]+:' <<<"$dis" || true)
  ind=$(grep -cE '(jmp|call)\s+\*' <<<"$dis" || true)
  calls=$(grep -cE 'call\s' <<<"$dis" || true)
  cond=$(grep -cE '\sj(e|ne|a|ae|b|be|g|ge|l|le|s|ns|z|nz)\s' <<<"$dis" || true)
  printf '%-12s insns=%-5s cond_branches=%-4s indirect=%-3s calls=%s\n' "$n" "$insns" "$cond" "$ind" "$calls"
done
