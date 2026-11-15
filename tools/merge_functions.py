"""Replace the scanned function list in the manifest's [modules.functions] table.
usage: python -I merge_functions.py <manifest.toml> <missing_funcs.toml>"""
import re, sys

MARK = "# Reached only through data pointers or code-built addresses (tools/missing_funcs.py)."
manifest, extra = sys.argv[1], sys.argv[2]
text = open(manifest, encoding="utf-8").read().split(MARK)[0].rstrip() + "\n"
have = set(re.findall(r'"(0x[0-9A-Fa-f]{8})"\s*=', text))
lines = [l for l in open(extra, encoding="utf-8").read().splitlines()
         if l.strip() and re.match(r'"(0x[0-9A-Fa-f]{8})"', l).group(1) not in have]
text += "\n" + MARK + "\n" + "\n".join(lines) + "\n"
open(manifest, "w", encoding="utf-8", newline="\n").write(text)
print("kept", len(have), "hand entries, wrote", len(lines), "scanned entries")
