#!/bin/sh
# Compare unresolved-branch markers: previous generated code (volume) against the new codegen (bind).
OLD=/b/src/app/generated/sims3
NEW=/nx/app/generated/sims3
for d in "$OLD" "$NEW"; do
  echo "$d cond=$(cat "$d"/sims3_recomp.*.cpp | grep -c 'ERROR: conditional branch to unknown') fatal=$(cat "$d"/sims3_recomp.*.cpp | grep -c 'REX_FATAL') bctr=$(cat "$d"/sims3_recomp.*.cpp | grep -c 'unresolved\|Unresolved')"
done
echo "--- enclosing functions of new markers (count, name)"
cat "$NEW"/sims3_recomp.*.cpp | awk '/^DEFINE_REX_FUNC\(/ {f=$0} /ERROR: conditional branch to unknown/ {c[f]++} END {for (k in c) print c[k], k}' | sort -rn | head -40
echo "--- distinct enclosing functions"
cat "$NEW"/sims3_recomp.*.cpp | awk '/^DEFINE_REX_FUNC\(/ {f=$0} /ERROR: conditional branch to unknown/ {c[f]++} END {n=0; for (k in c) n++; print n}'
