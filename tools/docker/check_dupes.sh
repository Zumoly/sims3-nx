#!/bin/sh
# The SDK links with --allow-multiple-definition, which hides collisions. Fail if the two recompiled
# modules still define any of the same global symbols. Linkonce/vague symbols (V, u, and C++ W) are shared on
# purpose by templates in the common headers, so only strong and data symbols count, plus weak guest functions.
N=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm
cd /b/out/app
syms() { find "$1" -name '*.obj' -print0 | xargs -0 "$N" 2>/dev/null | awk '$2 ~ /^[TDBR]$/ {print $3}' | sort -u; }
guest() { find "$1" -name '*.obj' -print0 | xargs -0 "$N" 2>/dev/null | awk '$2 == "W" && ($3 ~ /^sub_/ || $3 ~ /^__(rest|save)/ || $3 == "xstart") {print $3}' | sort -u; }
syms CMakeFiles/sims3_default_code.dir > /tmp/a
syms CMakeFiles/sims3_dll_code.dir > /tmp/b
guest CMakeFiles/sims3_default_code.dir >> /tmp/a
guest CMakeFiles/sims3_dll_code.dir >> /tmp/b
sort -u /tmp/a -o /tmp/a
sort -u /tmp/b -o /tmp/b
comm -12 /tmp/a /tmp/b > /tmp/dupes
echo "defined default=$(wc -l < /tmp/a) sims3=$(wc -l < /tmp/b) shared=$(wc -l < /tmp/dupes)"
head -40 /tmp/dupes
[ ! -s /tmp/dupes ]
