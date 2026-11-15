#!/bin/sh
# Compiles every translated shader with uam. usage: compile_shaders.sh <glsl_dir> <dksh_dir>
src=$1
out=$2
mkdir -p "$out"
ok=0
bad=0
for f in "$src"/*.glsl; do
  name=$(basename "$f" .glsl)
  case "$name" in
    ps_*) stage=frag ;;
    *) stage=vert ;;
  esac
  U=/opt/devkitpro/tools/bin/uam; [ -x /b/tools/uam ] && U=/b/tools/uam
  if "$U" --stage=$stage -o "$out/$name.dksh" "$f" 2> "$out/$name.err"; then
    ok=$((ok + 1))
    rm -f "$out/$name.err"
  else
    bad=$((bad + 1))
    echo "FAILED $name: $(head -n 3 "$out/$name.err" | tr '\n' ' ')"
  fi
done
echo "compiled $ok, failed $bad"
