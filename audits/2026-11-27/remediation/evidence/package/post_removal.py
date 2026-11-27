import json,os,subprocess
from pathlib import Path
out=Path('/tmp/rummage-remediation-package-checks'); stage=out/'staged-install';prefix=stage/'prefix';src=Path('/tmp/rummage-photo-library')
env=os.environ.copy();env['PATH']='/nix/store/p1hnmz5vhjlfj3jxwzf33ahz9qv20q52-desktop-file-utils-0.28/bin:'+env['PATH']
for kind in ['DATA','CONFIG','STATE','CACHE']:env['XDG_'+kind+'_HOME']=str(stage/kind.lower())
env.pop('RUMMAGE_PHOTOS_LIBRARY',None)
checks=[('base-after-addon-removal',['bash',str(src/'scripts/ci/check-installed.sh'),str(prefix),'0.6.1',str(src/'LICENSE')]),('addon-dynamic-linkage',['readelf','-d',str(stage/'addon/libexec/rummage/librummage-photos.so')])]
results=[]
for name,command in checks:
 result=subprocess.run(command,env=env,capture_output=True,text=True,timeout=60);(out/(name+'.log')).write_text(result.stdout+result.stderr);results.append({'name':name,'command':command,'exit':result.returncode});print(name,result.returncode)
 if result.returncode:raise SystemExit(result.returncode)
 if name=='addon-dynamic-linkage':assert 'libsqlite3.so' in result.stdout
(out/'post-removal-results.json').write_text(json.dumps(results,indent=2)+'\n')
