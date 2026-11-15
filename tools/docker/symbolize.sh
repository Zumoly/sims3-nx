#!/bin/sh
# Map image offsets to ELF symbols. usage: symbolize.sh <hex offsets...>
N=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm
C=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-c++filt
E=/b/out/app/sims3
$N -n "$E" | grep -E ' [TtWw] ' > /tmp/syms
for a in "$@"; do
  awk -v t="$a" 'BEGIN { v = strtonum("0x" t) }
    { x = strtonum("0x" $1); if (x <= v) { s = $3; b = x } else exit }
    END { printf "%s %s +0x%x\n", t, s, v - b }' /tmp/syms
done | $C | cut -c1-150
