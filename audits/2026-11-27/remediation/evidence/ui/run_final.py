import os,pathlib,subprocess,sys
root=pathlib.Path(__file__).parent
env=os.environ.copy();env['RUMMAGE_PHOTOS_LIBRARY']='/tmp/rummage-remediation-build/librummage-photos.so'
for count in [1000,10000,100000]:
 subprocess.run([sys.executable,str(root/'run_harness.py'),str(root/'photo_stress-rummage-remediation-build'),str(count),f'final-photo-{count}'],env=env,check=True)
