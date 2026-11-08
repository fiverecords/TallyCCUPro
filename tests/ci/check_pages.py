#!/usr/bin/env python3
"""Checks the web pages served from the SD card (sdcard/*.html):
  - every inline <script> passes `node --check` (syntax);
  - each file keeps its line endings: index.html CRLF, the others LF, never mixed.

  python3 tests/ci/check_pages.py [file.html ...]     (default: sdcard/*.html)
"""
import glob, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = re.compile(rb'<script(?![^>]*\bsrc\s*=)[^>]*>(.*?)</script\s*>', re.S | re.I)
# expected line endings per page; any other page is LF
EXPECTED_EOL = {'index.html': 'CRLF'}


def check(path, tmp):
    errors = []
    data = open(path, 'rb').read()
    crlf = data.count(b'\r\n')
    lf = data.count(b'\n') - crlf
    eol = 'mixed' if crlf and lf else ('CRLF' if crlf else 'LF')
    want = EXPECTED_EOL.get(os.path.basename(path), 'LF')
    if eol == 'mixed':
        errors.append('mixed line endings: %d CRLF, %d LF (expected %s)' % (crlf, lf, want))
    elif eol != want:
        errors.append('line endings are %s, expected %s' % (eol, want))
    for i, m in enumerate(SCRIPT.finditer(data)):
        line = data.count(b'\n', 0, m.start(1)) + 1
        js = os.path.join(tmp, '%s.%d.js' % (os.path.basename(path), i))
        # keep line numbers: pad with the lines before the script
        open(js, 'wb').write(b'\n' * (line - 1) + m.group(1))
        r = subprocess.run(['node', '--check', js], capture_output=True, text=True)
        if r.returncode != 0:
            msg = r.stderr.strip().replace(js, '%s (script %d)' % (os.path.relpath(path, ROOT), i))
            errors.append(msg)
    return errors, len(SCRIPT.findall(data)), eol


def main():
    files = sys.argv[1:] or sorted(glob.glob(os.path.join(ROOT, 'sdcard', '*.html')))
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for f in files:
            errors, n, eol = check(f, tmp)
            print('%-5s %s (%d inline scripts, %s)' % ('FAIL' if errors else 'ok', os.path.relpath(f, ROOT), n, eol))
            for e in errors:
                print('      ' + e.replace('\n', '\n      '))
            bad += bool(errors)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
