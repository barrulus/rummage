#!/usr/bin/env python3
import pathlib,json,shlex,subprocess,sys
src=pathlib.Path('/tmp/rummage-photo-library'); build=pathlib.Path('/tmp/rummage-branch-audit-2026-11-27/build-asan'); out=pathlib.Path('/tmp/rummage-branch-audit-2026-11-27/root/mutations');out.mkdir(exist_ok=True)
entries=json.loads((build/'compile_commands.json').read_text())
e=next(x for x in entries if x['file']==str(src/'tests/main.cpp') and 'rummage-tests.dir' in x['command'])
args=shlex.split(e['command']); base=[];i=0
while i<len(args):
 if args[i]=='-o':i+=2;continue
 if args[i]=='-c':i+=1;continue
 if args[i]==e['file']:i+=1;continue
 base.append(args[i]);i+=1
base+=['-I'+str(src/'src/formats/image/src'),'-Wno-error=unused-result']
commands=[]
def run(args):
 commands.append(shlex.join(args));subprocess.run(args,cwd=build,check=True)
driver=out/'driver.o';run(base+['-c','/tmp/rummage-branch-audit-2026-11-27/root/harness/mutation_driver.cpp','-o',str(driver)])
lines=subprocess.check_output(['ninja','-t','commands','tests/rummage-tests'],cwd=build,text=True).splitlines();line=next(x for x in reversed(lines) if ' -o tests/rummage-tests ' in x)
raw=shlex.split(line); first=next(i for i,x in enumerate(raw) if x.endswith(('/g++','/c++')));raw=raw[first:]
if '&&' in raw:raw=raw[:raw.index('&&')]
link=[];i=0
while i<len(raw):
 if raw[i]=='-o':i+=2;continue
 if raw[i].endswith('.o') and 'lsan_suppressions' not in raw[i]:i+=1;continue
 link.append(raw[i]);i+=1
for name in sys.argv[1:] or ['text','tiff','archive','picture','svg']:
 obj=out/(name+'.o');run(base+['-c',str(src/f'tests/fuzz/fuzz_{name}.cpp'),'-o',str(obj)])
 run([link[0],str(driver),str(obj),*link[1:],'-o',str(out/('mutate-'+name))])
(out/'build-commands.txt').write_text('\n'.join(commands)+'\n')
