import pathlib,sys
sys.path.insert(0,'/tmp/rummage-branch-audit-2026-11-27/catalog')
from audit_support import seed_synthetic
root=pathlib.Path(__file__).parent/'runtime'/sys.argv[2]
cat=seed_synthetic(sys.argv[1],root/'data/rummage/photos/catalog.sqlite3',root/'source',int(sys.argv[3]))
cat.call({'op':'background_control','paused':True})
print(cat.path)
