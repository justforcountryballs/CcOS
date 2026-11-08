# make a tiny stand-in for a library: same name, same symbols, functions that do nothing
import subprocess, sys, re, os
lib = sys.argv[1]                       # e.g. /usr/lib/x86_64-linux-gnu/libx265.so.199
importers = sys.argv[2:]                # libraries that use it
soname = re.search(r'SONAME\s+(\S+)', subprocess.run(['objdump','-p',lib],capture_output=True,text=True).stdout).group(1)
defined = {}
for l in subprocess.run(['nm','-D','--defined-only','-S',lib],capture_output=True,text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4: defined[p[3].split('@')[0]] = (p[2], int(p[1],16))
    elif len(p) == 3: defined[p[2].split('@')[0]] = (p[1], 0)
need = {}
for imp in importers:
    for l in subprocess.run(['objdump','-T',imp],capture_output=True,text=True).stdout.splitlines():
        if '*UND*' not in l: continue
        p = l.split()
        name = p[-1]; ver = p[-2] if len(p) >= 2 else ''
        ver = ver.strip('()')
        if ver == 'Base' or ver.startswith('0'): ver = ''
        if name in defined: need[name] = ver
c = ['/* stand-in for %s: ccos only plays media, it never needs this */' % soname]
vers = {}
for n in sorted(need):
    t, size = defined[n]; v = need[n]
    if t in ('T','t','W','i'): c.append('long %s(void) { return -1; }' % n)
    else: c.append('char %s[%d];' % (n, max(size,8)))
    if v and v not in ('Base',): vers.setdefault(v,[]).append(n)
open('stub.c','w').write('\n'.join(c)+'\n')
vs = ''.join('%s { global: %s; };\n' % (v, '; '.join(sorted(ns))) for v,ns in vers.items())
vs += '{ global: *; };\n' if not vers else ''
open('stub.map','w').write(vs)
out = sys.argv[0].replace('mkstub.py','') + 'out/' + soname
os.makedirs(os.path.dirname(out), exist_ok=True)
args = ['gcc','-shared','-fPIC','-O1','-o',out,'stub.c','-Wl,-soname,'+soname]
if vers: args.append('-Wl,--version-script=stub.map')
r = subprocess.run(args,capture_output=True,text=True)
print(soname, len(need), 'symbols', 'ok' if r.returncode == 0 else r.stderr[:500])
