import json,os,subprocess
from pathlib import Path
out=Path('/tmp/rummage-remediation-package-checks');src=Path('/tmp/rummage-photo-library');results=[]
env=os.environ.copy();env['PYTHONDONTWRITEBYTECODE']='1';env['HUGO_RESOURCEDIR']=str(out/'website-resources')
checks=[('docs',['python3','scripts/check-docs.py']),('website-build',['/tmp/rummage-hugo/hugo','--source','website','--destination',str(out/'website-public'),'--cacheDir',str(out/'website-cache'),'--noBuildLock','--minify','--panicOnWarning','--printPathWarnings']),('website-check',['python3','website/check.py',str(out/'website-public')])]
for name,command in checks:
 p=subprocess.run(command,cwd=src,env=env,capture_output=True,text=True,timeout=120);(out/(name+'.log')).write_text(p.stdout+p.stderr);results.append({'name':name,'command':command,'exit':p.returncode});(out/'gates-results.json').write_text(json.dumps(results,indent=2)+'\n');print(name,p.returncode,p.stdout[-1000:],p.stderr[-1000:],flush=True)
