#!/bin/sh
# Which xstart did the link keep, and which objects define it? Also: was multiple definition allowed?
N=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm
cd /b/out/app
echo "--- in final ELF"
$N sims3 | grep -E ' (__imp__)?xstart$'
echo "--- objects defining it"
for f in CMakeFiles/sims3_default_code.dir/generated/default/*.obj CMakeFiles/sims3_dll_code.dir/generated/sims3/*.obj; do
  r=$($N "$f" 2>/dev/null | grep -E ' [TtWw] (__imp__)?xstart$')
  [ -n "$r" ] && echo "$f: $r"
done
echo "--- link flags"
grep -m1 -o -E 'allow-multiple-definition|muldefs|-z,muldefs' build.ninja || echo "no muldefs in build.ninja"
grep -rho -E 'allow-multiple-definition|muldefs' /b/src/sdk/cmake /b/src/tools/switch/cmake 2>/dev/null | sort -u
