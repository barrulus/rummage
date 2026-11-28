import json,pathlib,subprocess
root=pathlib.Path(__file__).parent
record=json.loads(pathlib.Path('/tmp/rummage-branch-audit-2026-11-27/root/strict-catalog-commands.json').read_text())
compile=record['compile'];compile[compile.index('-o')+1]=str(root/'catalog-strict.o')
link=[compile[0],'-shared',str(root/'catalog-strict.o'),'-lsqlite3','-fsanitize=address,undefined','-o',str(root/'catalog-strict.so')]
(root/'strict-commands.json').write_text(json.dumps({'compile':compile,'link':link},indent=2))
subprocess.run(compile,check=True);subprocess.run(link,check=True)
