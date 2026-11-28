import sys,time,statistics,sqlite3,json,tempfile
from pathlib import Path
sys.path.insert(0,'/tmp/rummage-branch-audit-2026-11-27/catalog')
from audit_support import seed_synthetic
root=Path(tempfile.mkdtemp(prefix='rummage-progress-profile-'))
cat=seed_synthetic('/tmp/rummage-remediation-build/librummage-photos.so',root/'catalog/db.sqlite',root/'source',100000,1000)
db=sqlite3.connect(cat.path)
db.execute("UPDATE photos SET metadata_signature='' WHERE id>99744")
db.commit()
# Retain a connection consistently; separately measure last-connection closing below.
queries={
'pending':"SELECT p.id,s.path,p.relative_path,p.device,p.inode,p.size,p.mtime,p.mtime_ns FROM photos p JOIN sources s ON s.id=p.source_id WHERE s.active=1 AND s.availability='online' AND p.availability='online' AND p.metadata_signature='' AND NOT EXISTS(SELECT 1 FROM metadata_claims c WHERE c.photo_id=p.id AND c.expires>unixepoch()) AND p.seen_job IN (SELECT id FROM jobs WHERE state IN ('queued','running','completed')) ORDER BY p.id LIMIT 4",
'aggregate':"SELECT sum(availability IN ('online','unreadable') AND metadata_signature<>''),sum(availability='online' AND metadata_signature=''),sum(availability NOT IN ('online','unreadable') OR (availability='unreadable' AND metadata_signature='')) FROM photos WHERE seen_job=1000"}
def timing(fn):
 fn(); times=[]
 for _ in range(15):
  t=time.perf_counter(); fn(); times.append((time.perf_counter()-t)*1000)
 return {'median_ms':round(statistics.median(times),3),'max_ms':round(max(times),3)}
def measure(label):
 out={'label':label,'abi':{},'sql':{},'plans':{}}
 for op in ['background_state','metadata_pending','progress','jobs']:
  out['abi'][op]=timing(lambda:cat.call({'op':op,'limit':4}))
 for name,sql in queries.items():
  out['sql'][name]=timing(lambda:db.execute(sql).fetchall())
  out['plans'][name]=db.execute('EXPLAIN QUERY PLAN '+sql).fetchall()
 print(json.dumps(out),flush=True)
measure('baseline')
queries['pending_forced']=queries['pending'].replace('photos p JOIN','photos p INDEXED BY photos_pending_metadata JOIN')
db.execute('DROP INDEX photos_seen_job')
db.execute('CREATE INDEX photos_seen_job ON photos(seen_job,availability,metadata_signature)')
db.commit()
measure('covering_seen_job')
db.execute('ANALYZE')
db.commit()
measure('covering_analyzed')
db.close()
print(json.dumps({'no_persistent_connection':{op:timing(lambda:cat.call({'op':op,'limit':4})) for op in ['background_state','metadata_pending','progress']},'fixture':str(root)}),flush=True)
