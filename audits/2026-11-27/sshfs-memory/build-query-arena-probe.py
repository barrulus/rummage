from pathlib import Path
import subprocess,shlex,sqlite3
root=Path('/tmp/rummage-query-arena-experiment');build=Path('/tmp/rummage-remediation-build')
commands=subprocess.check_output(['ninja','-C',str(build),'-t','commands','rummage-bench-photos'],text=True).splitlines()
line=next(c for c in commands if ' -c ' in c and c.endswith('/src/tools/bench_photos.cpp'))
a=shlex.split(line);old=a[a.index('-o')+1];new=str(root/'probe.o')
a[a.index('-o')+1]=new;a[a.index('-c')+1]=str(root/'probe.cpp')
if '-MF' in a:a[a.index('-MF')+1]=new+'.d'
subprocess.run(a,cwd=build,check=True)
link=commands[-1].replace(old,new).replace(' -o rummage-bench-photos ',' -o '+str(root/'probe')+' ')
subprocess.run(link,cwd=build,shell=True,check=True)
for name in ['librummage-photos.so']:
 p=root/name
 if not p.exists():p.symlink_to(build/name)
with sqlite3.connect('file:/tmp/rummage-index-memory-20261127/catalog/catalog.sqlite3?mode=ro',uri=True) as src,sqlite3.connect(root/'catalog.sqlite3') as dst:src.backup(dst)
