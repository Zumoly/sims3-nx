"""For functions with undefined labels: find the bound check before the switch and the case numbers
that use each undefined label. usage: python -I switch_bounds.py <generated/sims3>"""
import glob, os, re, sys

gen = sys.argv[1]
FUNCS = {"889DF148": "146", "88D79100": "247", "88DF07B8": "28"}
for addr, n in FUNCS.items():
    data = open(os.path.join(gen, "sims3_recomp.%s.cpp" % n), encoding="utf-8", errors="replace").read()
    i = data.find("DEFINE_REX_FUNC(sub_%s)" % addr)
    j = data.find("DEFINE_REX_FUNC(", i + 10)
    body = data[i:j].splitlines()
    sw = next(k for k, l in enumerate(body) if "switch (" in l)
    var = re.search(r"switch \(ctx\.(r\d+)", body[sw]).group(1)
    asm = [l.strip()[3:] for l in body[max(0, sw - 70):sw] if l.strip().startswith("// ")]
    print("== sub_%s switch on %s" % (addr, var))
    for a in asm[-22:-9]:
        print("   ", a)
    defs = set(re.findall(r"(?m)^\s*(loc_[0-9A-F]{8}):", "\n".join(body)))
    case, pending, seen, end = None, [], {}, None
    for k in range(sw + 1, len(body)):
        l = body[k].strip()
        m = re.match(r"case (\d+):", l)
        if m:
            pending.append(int(m.group(1)))
            continue
        m = re.match(r"goto (loc_[0-9A-F]{8})", l)
        if m and pending:
            for c in pending:
                seen[c] = m.group(1)
            pending = []
        if l.startswith("default:"):
            end = k
            break
    bad = sorted(c for c, t in seen.items() if t not in defs)
    print("    cases", len(seen), "undefined at cases", bad[:12], "...", bad[-3:] if bad else "")
    # the point where targets stop looking like this function's own: first label outside the first run
    labels = [seen[c] for c in sorted(seen)]
    print("    first 3:", labels[:3], "| targets by case:", [labels[c] for c in (0, 8, 32, 64, 96, 128, 200)])
    print("    lines after switch end:", [x.strip() for x in body[end:end + 4]])
