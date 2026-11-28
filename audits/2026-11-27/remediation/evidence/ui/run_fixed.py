import os,pathlib,subprocess,sys
root=pathlib.Path(__file__).parent
env=os.environ.copy();env['RUMMAGE_PHOTOS_LIBRARY']='/tmp/rummage-remediation-build/librummage-photos.so'
for kind,exe in [('view','view_stress-rummage-remediation-build'),('photo','photo_stress-rummage-remediation-build')]:
 for count in [1000,10000,100000]:
  subprocess.run([sys.executable,str(root/'run_harness.py'),str(root/exe),str(count),f'fixed-{kind}-{count}'],env=env,check=True)
