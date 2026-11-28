#!/usr/bin/env python3
"""Read-only frozen-revision inventory; exact normalized 12-line clone candidates (not semantic duplication)."""
import collections,hashlib,json,pathlib,re,subprocess
ROOT=pathlib.Path('/tmp/rummage-photo-library'); OUT=pathlib.Path(__file__).parent
BASE='63ca25bc'; HEAD='1dd23687'
def git(*args):return subprocess.check_output(['git','-C',str(ROOT),*args])
files=git('ls-tree','-r','--name-only',HEAD).decode().splitlines()
changed=set(git('diff','--name-only',BASE,HEAD).decode().splitlines())
modules=collections.defaultdict(lambda:dict(files=0,bytes=0,lines=0,changed_files=0,cpp_files=0,cpp_lines=0))
rows=[];code={};natives={'.cpp','.hpp','.h','.c'}
for p in files:
 data=git('show',HEAD+':'+p)
 area='/'.join(p.split('/')[:2]) if p.startswith('src/') else p.split('/')[0] if '/' in p else '(root files)'
 row=dict(path=p,area=area,bytes=len(data),lines=data.count(b'\n'),changed=p in changed)
 rows.append(row);mod=modules[area];mod['files']+=1;mod['bytes']+=len(data);mod['lines']+=row['lines'];mod['changed_files']+=p in changed
 if pathlib.Path(p).suffix in natives:
  mod['cpp_files']+=1;mod['cpp_lines']+=row['lines']
  if p.startswith('src/'):
   normalized=[]
   for i,line in enumerate(data.decode(errors='replace').splitlines(),1):
    line=line.strip()
    if not line or line.startswith(('//','#','/*','*')) or re.fullmatch(r'[{}();]+',line):continue
    normalized.append((i,re.sub(r'\s+',' ',line)))
   code[p]=normalized
(OUT/'inventory.json').write_text(json.dumps(dict(head=git('rev-parse',HEAD).decode().strip(),base=git('rev-parse',BASE).decode().strip(),areas=dict(sorted(modules.items())),files=rows),indent=2)+'\n')
windows=collections.defaultdict(list);span=12
for p,lines in code.items():
 for i in range(len(lines)-span+1):
  block='\n'.join(text for _,text in lines[i:i+span])
  if len(block)<320:continue
  windows[hashlib.sha256(block.encode()).digest()].append((p,i))
candidates=[];seen=set()
for group in windows.values():
 if len(group)<2 or len(group)>20:continue
 for a in range(len(group)):
  for b in range(a+1,len(group)):
   p,i=group[a];q,j=group[b]
   if p==q and abs(i-j)<span:continue
   if (p,i,q,j) in seen:continue
   if i>0 and j>0 and code[p][i-1][1]==code[q][j-1][1]:continue
   length=span
   while i+length<len(code[p]) and j+length<len(code[q]) and code[p][i+length][1]==code[q][j+length][1]:length+=1
   for k in range(length-span+1):seen.add((p,i+k,q,j+k))
   candidates.append(dict(lines=length,a=[p,code[p][i][0],code[p][i+length-1][0]],b=[q,code[q][j][0],code[q][j+length-1][0]],touches_changed_file=p in changed or q in changed,sample='\n'.join(t for _,t in code[p][i:i+min(length,18)])))
candidates.sort(key=lambda r:-r['lines'])
(OUT/'clone-candidates.json').write_text(json.dumps(dict(method='12 consecutive nonblank, noncomment, nonpreprocessor, nonpunctuation-only C/C++ lines; whitespace collapsed; >=320 chars; no identifier/literal renaming; overlapping matches coalesced; candidates are not findings',candidates=candidates),indent=2)+'\n')
with (OUT/'inventory.tsv').open('w') as f:
 f.write('area\tfiles\tchanged_files\tcpp_files\tcpp_lines\tbytes\n')
 for area,m in sorted(modules.items()):f.write('\t'.join(map(str,[area,m['files'],m['changed_files'],m['cpp_files'],m['cpp_lines'],m['bytes']]))+'\n')
print(json.dumps(dict(tracked_files=len(files),changed_files=len(changed),areas=len(modules),source_cpp_files=len(code),clone_candidates=len(candidates),changed_file_clone_candidates=sum(c['touches_changed_file'] for c in candidates),largest_first_party=sorted([r for r in rows if r['path'].startswith('src/') and pathlib.Path(r['path']).suffix in natives],key=lambda r:-r['lines'])[:12]),indent=2))
