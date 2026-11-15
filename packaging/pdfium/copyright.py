#!/usr/bin/env python3
"""Append the source bundle's upstream notices to Debian's copyright file."""
from pathlib import Path
import sys

sources, copyright_file = map(Path, sys.argv[1:])
notices = sorted(path for path in sources.rglob('*')
                 if path.is_file() and (path.name.lower().startswith(('license', 'copying'))))
if not notices:
    raise ValueError('The PDFium source bundle has no license notices')
with copyright_file.open('a') as output:
    output.write('\nFiles: third_party/pdfium-sources/*\n'
                 'Copyright: The respective upstream authors, as recorded below.\n'
                 'License: bundled-PDFium-components\n'
                 'Comment:\n'
                 ' Source dependencies and build tools are pinned by flake.lock.\n'
                 ' The following verbatim upstream notices cover this source bundle;\n'
                 ' runtime notices are also installed by rummage-pdf.\n\n'
                 'License: bundled-PDFium-components\n')
    for notice in notices:
        output.write(f' .\n {notice.relative_to(sources)}\n .\n')
        try:
            text = notice.read_text(encoding='utf-8')
        except UnicodeDecodeError:
            # OpenJPEG's legacy viewer notice is encoded as Latin-1.
            text = notice.read_text(encoding='latin-1')
        for line in text.splitlines():
            output.write(' ' + (line if line.strip() else '.') + '\n')
