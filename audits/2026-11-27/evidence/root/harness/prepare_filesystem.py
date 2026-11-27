import pathlib,subprocess,json,hashlib
root=pathlib.Path('/tmp/rummage-branch-audit-2026-11-27/root/fixtures');root.mkdir(exist_ok=True)
bin='/tmp/rummage-branch-audit-2026-11-27/build-release/rummage-bench-dir'
for count in [1000,10000,100000]:
 d=root/f'entries-{count}'
 if not d.exists():subprocess.run([bin,'--make',str(count),str(d)],check=True)
src=root/'copy-source';src.mkdir(exist_ok=True)
f=src/'payload.bin'
block=hashlib.sha256(b'rummage-audit-2026-11-27').digest()*32768
with f.open('wb') as stream:
 for _ in range(256):stream.write(block)
for i in range(1000):(src/f'small-{i:04}.txt').write_text(f'fixture {i}\n'*100)
manifest={'directory_entries':[1000,10000,100000],'large_copy_bytes':f.stat().st_size,'small_copy_files':1000,'seed_block_sha256':hashlib.sha256(block).hexdigest(),'fixture_type':'empty camera-style files for listing; deterministic dense repeated bytes forcopy; not real photos'}
(root/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print(manifest)
