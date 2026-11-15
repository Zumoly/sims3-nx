"""Map image offsets to ELF symbols. usage: python3 symbolize.py <hex offsets...>"""
import bisect, subprocess, sys

BIN = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-"
out = subprocess.run([BIN + "nm", "-n", "-C", "/b/out/app/sims3"], capture_output=True, text=True).stdout
addrs, names = [], []
for line in out.splitlines():
    p = line.split(" ", 2)
    if len(p) == 3 and p[1] in "TtWw":
        addrs.append(int(p[0], 16))
        names.append(p[2])
for a in sys.argv[1:]:
    v = int(a, 16)
    i = bisect.bisect_right(addrs, v) - 1
    print("%-9s %s +0x%x" % (a, names[i][:110], v - addrs[i]))
