#!/usr/bin/env python3
import json,pathlib,shlex,subprocess,sys
build=pathlib.Path(sys.argv[1]); name=sys.argv[2] if len(sys.argv)>2 else 'view_stress'
root=pathlib.Path(__file__).parent; source=root/(name+'.cpp'); obj=root/(name+'-'+build.name+'.o');exe=root/(name+'-'+build.name)
x=next(v for v in json.load(open(build/'compile_commands.json')) if v['file'].endswith('/tests/quick_look_test.cpp'))
a=shlex.split(x['command']); args=[]; skip=False
for i,v in enumerate(a):
 if skip: skip=False;continue
 if v in ['-o','-c']: skip=True;continue
 if v.startswith('-W'):continue
 args.append(v)
args+=['-o',str(obj),'-c',str(source)]
subprocess.run(args,cwd=build,check=True)
text=(build/'build.ninja').read_text(); block=text.split('build tests/rummage-tests:',1)[1].split('\n\n',1)[0]
vals={}
for line in block.splitlines():
 if ' = ' in line:
  k,v=line.strip().split(' = ',1);vals[k]=v
libs=shlex.split(vals['LINK_LIBRARIES'].replace('$$','$'))
cmd=[args[0],str(obj),'-o',str(exe)]+shlex.split(vals.get('LINK_FLAGS',''))+libs
# CMake normally adds these rpaths through the generated link rule.
paths=sorted({str(pathlib.Path(x).parent) for x in libs if x.startswith('/nix/store/') and '.so' in x})
cmd+=['-Wl,-rpath,'+':'.join(paths)]
subprocess.run(cmd,cwd=build,check=True)
print(exe)
