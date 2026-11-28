import json, pathlib, shlex, subprocess
root=pathlib.Path('/tmp/rummage-photo-library');build=pathlib.Path('/tmp/rummage-photo-browser-build');out=pathlib.Path('/tmp/rummage-branch-audit-2026-11-27/catalog')
e=next(e for e in json.loads((build/'compile_commands.json').read_text()) if e['file']==str(root/'src/isolate/src/protocol.cpp'))
args=shlex.split(e['command']);args[args.index('-o')+1]=str(out/'protocol_utf8');args.remove('-c');args[-1]=str(out/'protocol_utf8.cpp');args.extend(['-I'+str(root/'src/isolate/src'),str(build/'src/isolate/librummage_isolate.a')])
subprocess.run(args,cwd=e['directory'],check=True)
result=subprocess.run([str(out/'protocol_utf8')],capture_output=True,text=True);(out/'protocol_utf8.log').write_text(result.stdout+result.stderr);print(result.stdout);assert result.returncode==0
