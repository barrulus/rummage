"""Exercise export byte and restore record ceilings on a disposable synthetic catalog."""
import json,pathlib,sqlite3,sys,tempfile,time,resource
from audit_support import seed_synthetic
module=sys.argv[1];output=pathlib.Path(sys.argv[2]);report={}
with tempfile.TemporaryDirectory(prefix='rummage-capacity-check-') as temporary:
 root=pathlib.Path(temporary);catalog=seed_synthetic(module,root/'data/catalog.db',root/'source',4097)
 with sqlite3.connect(catalog.path) as db:db.execute('UPDATE photos SET description=?',('x'*65536,))
 started=time.monotonic();result=catalog.call({'op':'export','encoded':True},check=False)
 assert '256 MiB' in result['error'],result
 report['export_over_256_mib']={'error':result['error'],'seconds':time.monotonic()-started,'synthetic_photos':4097,'description_bytes_each':65536}
 assert catalog.call({'op':'query','limit':1})['total']==4097
 result=catalog.call({'op':'restore','data':{'version':1,'photos':[None]*1000001}},check=False)
 assert '1000000' in result['error'],result
 report['restore_over_one_million']={'error':result['error'],'catalog_rows_unchanged':catalog.call({'op':'query','limit':1})['total']==4097}
 with sqlite3.connect(catalog.path) as db:
  report['integrity']=db.execute('PRAGMA integrity_check').fetchone()[0]
  report['foreign_keys']=db.execute('PRAGMA foreign_key_check').fetchall()
report['peak_rss_kib']=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
output.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
