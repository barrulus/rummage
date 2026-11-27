import hashlib,json,sqlite3,struct,time,urllib.parse
from pathlib import Path
root=Path('/tmp/rummage-sshfs-verified-remote-matched')
c=json.loads((root/'results.json').read_text())[0];p=root/c['name'];offset=time.time_ns()-time.monotonic_ns()
with sqlite3.connect('file:'+str(p/'data/rummage/photos/catalog.sqlite3')+'?mode=ro',uri=True) as db:
 rows={f'photo:{i}':('file://'+urllib.parse.quote(str(Path(source)/relative),safe="!&'()*+,=:@$~_-./"),mtime) for i,source,relative,mtime in db.execute('select p.id,s.path,p.relative_path,p.mtime*1000000000+p.mtime_ns from photos p join sources s on s.id=p.source_id')}
def text_chunks(data):
 result={};pos=8
 while pos+12<=len(data):
  size=struct.unpack('>I',data[pos:pos+4])[0];kind=data[pos+4:pos+8];payload=data[pos+8:pos+8+size];pos+=size+12
  if kind==b'tEXt':
   key,value=payload.split(b'\0',1);result[key.decode()]=value.decode()
 return result
out=[]
for phase in (1,3,5):
 result={'phase':phase,'selections':0,'full_cache_existed_before_input':0,'valid_cache':0,'ready_when_valid_cache_existed':0,'current_ready_when_valid_cache_existed':0,'large_cache_existed_before_input':0,'grid_ready_with_large_cache':0}
 for t in [x for x in c['metrics']['transitions'] if x['phase']==phase][1:]:
  result['selections']+=1;uri,mtime=rows[t['identity']];name=hashlib.md5(uri.encode()).hexdigest()+'.png'
  for tier in ['x-large','large']:
   f=p/'cache/thumbnails'/tier/name
   if not f.exists() or f.stat().st_mtime_ns > t['input_ns']+offset:continue
   tags=text_chunks(f.read_bytes());valid=tags.get('Thumb::URI')==uri and tags.get('Thumb::MTime')==str(mtime//10**9)
   if tier=='large':
    result['large_cache_existed_before_input']+=int(valid);result['grid_ready_with_large_cache']+=int(valid and t['current_photo_ready']);continue
   result['full_cache_existed_before_input']+=1;result['valid_cache']+=int(valid);result['ready_when_valid_cache_existed']+=int(valid and t['preview_ready']);result['current_ready_when_valid_cache_existed']+=int(valid and t['current_photo_ready'])
 out.append(result)
(root/'cache-coverage.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
