import glob,re,sys
gen=sys.argv[1]
hits=[]
for p in glob.glob(gen+'/sims3_recomp.*.cpp'):
    t=open(p,encoding='utf-8',errors='replace').read()
    for part in re.split(r'(?m)^(?=DEFINE_REX_FUNC\()',t):
        m=re.match(r'DEFINE_REX_FUNC\((\w+)\)',part)
        if not m: continue
        a=[l.strip()[3:] for l in part.splitlines() if l.strip().startswith('// ')]
        for i,x in enumerate(a):
            if re.match(r'li r7,27$',x):
                win=a[i:i+10]
                if any(w.startswith('bl ') or w=='bctrl' for w in win) and any(re.match(r'li r6,(4|8|16|24)$',w) for w in a[max(0,i-3):i+4]):
                    hits.append((m.group(1),a[max(0,i-6):i+14]))
print(len(hits))
for n,w in hits[:6]:
    print('==',n); print('\n'.join('  '+s for s in w))