"""Registers and scratch memory each compiled shader needs. usage: python -I dksh_scratch.py <dksh_dir>"""
import glob, os, struct, sys

rows = []
for path in glob.glob(os.path.join(sys.argv[1], "*.dksh")):
    d = open(path, "rb").read()
    magic, header, control, code, programs, count = struct.unpack_from("<6I", d, 0)
    words = struct.unpack_from("<8I", d, programs)
    # type, entrypoint, num_gprs, constbuf1_off, constbuf1_sz, per_warp_scratch_sz
    rows.append((words[5], words[2], code, os.path.basename(path)))
rows.sort(reverse=True)
print(len(rows), "shaders; with scratch:", sum(1 for r in rows if r[0]))
for scratch, gprs, code, name in rows[:14]:
    print("scratch %6d  gprs %3d  code %6d  %s" % (scratch, gprs, code, name))
