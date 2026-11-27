#!/usr/bin/env python3
"""One isolated UBSan case. Use ASAN_OPTIONS=detect_leaks=0 when loading in Python.
LD_PRELOAD must contain the compiler's libasan.so before loading an ASan module.
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 makes the signed overflow fail fast.
"""
import sys, tempfile
from pathlib import Path
from audit_support import Catalog
import os
with tempfile.TemporaryDirectory(prefix='rummage-audit-time-') as temporary:
    root=Path(temporary); source=root/'source'; source.mkdir(); file=source/'future.jpg'
    file.write_bytes(b'fixture'); os.utime(file,ns=(10**19,10**19))
    assert file.stat().st_mtime_ns > 2**63-1, 'fixture filesystem does not support extended timestamps'
    cat=Catalog(sys.argv[1],root/'catalog/data.db')
    cat.call({'op':'add_source','path':str(source)});cat.call({'op':'work'})
    print(cat.call({'op':'query'}),flush=True)
