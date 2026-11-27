import concurrent.futures,json,os,pathlib,subprocess,time
root=pathlib.Path('/tmp/rummage-photo-library');out=pathlib.Path(__file__).parent
checks={'ci-unit':['python3','-m','unittest','discover','-s','scripts/ci/tests','-v'],'performance-tool-unit':['python3','-m','unittest','discover','-s','scripts/performance','-p','test_*.py','-v'],'docs':['python3','scripts/check-docs.py'],'i18n':['python3','scripts/i18n-check.py','--strict'],'whitespace':['git','diff','--check','63ca25bc..1dd23687']}
def run(item):
 name,cmd=item;start=time.monotonic();env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1')
 with (out/(name+'.log')).open('w') as f:
  try: status=subprocess.run(cmd,cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=240).returncode
  except subprocess.TimeoutExpired:status='timeout'
 return name,dict(command=cmd,exit=status,seconds=round(time.monotonic()-start,3))
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:results=dict(pool.map(run,checks.items()))
(out/'checks.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2))
