"""Synthetic fixtures ONLY. All callers must supply paths under their own /tmp tree."""
import ctypes, json, os, sqlite3
from pathlib import Path

class Catalog:
    def __init__(self, module, path):
        self.path = Path(path)
        self.lib = ctypes.CDLL(str(Path(module).resolve()))
        self.lib.rummage_photos_request.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        self.lib.rummage_photos_request.restype = ctypes.c_void_p
        self.lib.rummage_photos_free.argtypes = [ctypes.c_void_p]
    def call(self, request, check=True):
        ptr = self.lib.rummage_photos_request(os.fsencode(self.path), json.dumps(request).encode())
        if not ptr: raise RuntimeError('null ABI result')
        try: result = json.loads(ctypes.string_at(ptr))
        finally: self.lib.rummage_photos_free(ptr)
        if check and not result.get('ok'): raise RuntimeError(result)
        return result
    def drain(self, max_batches=1000):
        for _ in range(max_batches):
            result = self.call({'op':'work','budget':256})
            for row in result.get('metadata',[]):
                self.call({'op':'metadata','id':row['id'],'signature':row['signature'], 'width':100, 'height':80})
            if not result.get('worked') and not result.get('metadata'): return
        raise RuntimeError('work did not drain')

def seed_synthetic(module, catalog, source, count, job_history=1):
    """Returns Catalog. Creates no image files; not an import/decode benchmark."""
    source = Path(source); source.mkdir(parents=True, exist_ok=True)
    cat = Catalog(module,catalog)
    row = cat.call({'op':'add_source','path':str(source)})
    sid, job = row['source']['id'], row['job_id']
    with sqlite3.connect(cat.path) as db:
        db.execute('DELETE FROM directories WHERE job_id=?',(job,))
        db.execute("UPDATE jobs SET state='completed',completed=?,total=? WHERE id=?",(count,count,job))
        db.executemany("INSERT INTO jobs(kind,source_id,state,completed,total) VALUES('scan',?,'completed',?,?)", ((sid,count,count) for _ in range(job_history-1)))
        job = db.execute('SELECT max(id) FROM jobs').fetchone()[0]
        db.executemany('INSERT INTO photos(source_id,relative_path,filename,device,inode,size,mtime,mtime_ns,description,rating,capture_date,metadata_signature,seen_job) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)',
            ((sid,f'folder-{i//1000:04}/photo-{i:07}.jpg',f'photo-{i:07}.jpg',1,i+1,1000000+i,1700000000+i,0,'coast holiday' if i%5==0 else 'mountain walking',i%6,f'2024-{i%12+1:02}-{i%28+1:02}T12:30','synthetic',job) for i in range(count)))
        db.execute("INSERT INTO photo_search(rowid,filename,description,tags) SELECT id,filename,description,'' FROM photos")
    return cat
