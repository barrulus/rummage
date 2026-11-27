import ast,json,pathlib,re,subprocess
root=pathlib.Path('/tmp/rummage-photo-library');out=pathlib.Path(__file__).parent
paths=subprocess.check_output(['git','ls-files'],cwd=root,text=True).splitlines();results={}
for ext,command in [('.nix',['nix-instantiate','--parse']),('.sh',['bash','-n'])]:
 checked=[];fail=[]
 for p in paths:
  if p.endswith(ext):
   r=subprocess.run(command+[p],cwd=root,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
   checked.append(p)
   if r.returncode:fail.append(dict(path=p,exit=r.returncode,stderr=r.stderr))
 results[ext]=dict(command=command,checked=checked,failures=fail)
checked=[];failed=[]
for p in paths:
 if p.endswith('.py'):
  try:ast.parse((root/p).read_text(),filename=p);checked.append(p)
  except Exception as e:failed.append(dict(path=p,error=str(e)))
results['.py']=dict(method='ast.parse, no imports or bytecode',checked=checked,failures=failed)
(out/'syntax-checks.json').write_text(json.dumps(results,indent=2)+'\n')
links=[]
for p in paths:
 if p.endswith('CMakeLists.txt'):
  text=(root/p).read_text()
  for match in re.finditer(r'(add_(?:library|executable)|target_link_libraries|add_subdirectory)\s*\((.*?)\)',text,re.S):
   links.append(dict(file=p,line=text[:match.start()].count('\n')+1,command=match.group(1),arguments=' '.join(match.group(2).split())))
(out/'cmake-target-inventory.json').write_text(json.dumps(links,indent=2)+'\n')
print(json.dumps({k:dict(count=len(v['checked']),failures=v['failures']) for k,v in results.items()},indent=2))
