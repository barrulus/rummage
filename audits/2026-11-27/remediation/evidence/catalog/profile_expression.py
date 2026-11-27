import sqlite3,time,statistics,json
p='/tmp/rummage-progress-profile-u0erpj4e/catalog/db.sqlite'; db=sqlite3.connect(p)
db.execute('DROP INDEX photos_seen_job');db.execute("CREATE INDEX photos_seen_job ON photos(seen_job,availability,(metadata_signature=''))");db.commit()
qs={
'grouped':"SELECT availability,metadata_signature='',count(*) FROM photos WHERE seen_job=1000 GROUP BY availability,metadata_signature=''",
'counts':"SELECT (SELECT count(*) FROM photos WHERE seen_job=1000 AND availability IN ('online','unreadable') AND (metadata_signature='')=0),(SELECT count(*) FROM photos WHERE seen_job=1000 AND availability='online' AND (metadata_signature='')=1),(SELECT count(*) FROM photos WHERE seen_job=1000)"
}
for name,q in qs.items():
 ts=[]
 for _ in range(15):
  t=time.perf_counter();r=db.execute(q).fetchall();ts.append((time.perf_counter()-t)*1000)
 print(json.dumps({'name':name,'median':statistics.median(ts),'result':r,'plan':db.execute('EXPLAIN QUERY PLAN '+q).fetchall()}))
