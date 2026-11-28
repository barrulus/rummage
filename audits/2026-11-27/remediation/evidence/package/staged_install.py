#!/usr/bin/env python3
import os,json,subprocess,importlib.util
from pathlib import Path
root=Path('/tmp/rummage-remediation-build'); src=Path('/tmp/rummage-photo-library'); out=Path('/tmp/rummage-remediation-package-checks/staged-install');out.mkdir(exist_ok=True);base=out/'prefix';addon=out/'addon'
env=os.environ.copy()
env["PYTHONDONTWRITEBYTECODE"]="1"
env['PATH']='/nix/store/p1hnmz5vhjlfj3jxwzf33ahz9qv20q52-desktop-file-utils-0.28/bin:'+env.get('PATH','')
for kind in ('DATA','CONFIG','STATE','CACHE'):
 p=out/kind.lower();p.mkdir(exist_ok=True);env[f'XDG_{kind}_HOME']=str(p)
env.pop('RUMMAGE_PHOTOS_LIBRARY',None)
results=[]
def run(name,args,customenv=None):
 p=subprocess.run(args,env=customenv or env,capture_output=True,text=True,timeout=120)
 (out/(name+'.log')).write_text(p.stdout+p.stderr);results.append({'name':name,'command':args,'exit':p.returncode})
 (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
 if p.returncode:raise RuntimeError(name+' failed; inspect log')
for component in ('Unspecified','media','pdf'):
 run('install-'+component,['cmake','--install',str(root),'--prefix',str(base),'--component',component])
run('check-base',['bash',str(src/'scripts/ci/check-installed.sh'),str(base),'0.6.1',str(src/'LICENSE')])
# Preserve HOME: the repository checker otherwise replaces it for its disposable fixture.
check=out/'check-photos-installed-preserve-home.py'
original=(src/'scripts/ci/check-photos-installed.py').read_text()
needle="env.update(HOME=str(root / 'home'), XDG_DATA_HOME=str(root / 'data'),"
assert needle in original
check.write_text(original.replace(needle,"env.update(XDG_DATA_HOME=str(root / 'data'),"))
run('check-absent',['python3',str(check),str(base/'bin/rummage')])
run('install-photos',['cmake','--install',str(root),'--prefix',str(addon),'--component','photos'])
files=sorted(str(p.relative_to(addon)) for p in addon.rglob('*') if p.is_file())
assert files==['libexec/rummage/librummage-photos.so'],files
run('install-photos-combined',['cmake','--install',str(root),'--prefix',str(base),'--component','photos'])
run('check-present',['python3',str(check),str(base/'bin/rummage'),'--module',str(base/'libexec/rummage/librummage-photos.so')])
(base/'libexec/rummage/librummage-photos.so').unlink()
run('check-removed',['python3',str(check),str(base/'bin/rummage')])
(out/'ownership.json').write_text(json.dumps({'photo_component_files':files,'base_files_after_addon_removal':sorted(str(p.relative_to(base)) for p in base.rglob('*') if p.is_file()),'scope':'CMake staged install and direct ABI/diagnostics; not a distro package-manager or upgrade test','checker_adaptation':'Preserved HOME; XDG fixture isolation retained. No production source modified.'},indent=2)+'\n')
print('Staged base, add-on ownership, installed checks, and add-on removal passed')
