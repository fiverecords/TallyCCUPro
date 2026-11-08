#!/usr/bin/env python3
"""DOC-12: the firmware version is written in one place only, FIRMWARE_VERSION in
firmware/TallyCCUPro/Configuration.h. The serial banner, 'status', /safemode-status and the
serial configurator take it from there.

Fails if:
- Configuration.h does not define FIRMWARE_VERSION exactly once, or another file defines it;
- a firmware source states a version of its own: 'Version X.Y', 'VX.Y' (file headers, banner)
  or a string literal holding the version number;
- a web page or tool states one ('Version X.Y', 'VX.Y').

Mentions of releases in comments ('as in v3.7.1', 'v3.8+') are history and allowed; so are the
README changelog and the release notes.

  tests/ci/check_version.py        prints the version; exit 1 on a problem
"""
import glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FW = os.path.join(ROOT, 'firmware', 'TallyCCUPro')
CONFIG = os.path.join(FW, 'Configuration.h')

DEFINE = re.compile(r'^\s*#\s*define\s+FIRMWARE_VERSION\b(.*)$')
STATED = re.compile(r'\b[Vv]ersion\s*:?\s*[Vv]?\d+\.\d+|\bV\d+\.\d+')
LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def rel(p):
    return os.path.relpath(p, ROOT)


def main():
    problems = []
    fw_files = sorted(glob.glob(os.path.join(FW, '*.h')) + glob.glob(os.path.join(FW, '*.cpp')) +
                      glob.glob(os.path.join(FW, '*.ino')))
    version = None
    for p in fw_files:
        for n, line in enumerate(open(p, encoding='utf-8', errors='replace'), 1):
            m = DEFINE.match(line)
            if not m:
                continue
            if p != CONFIG:
                problems.append('%s:%d: FIRMWARE_VERSION defined outside Configuration.h' % (rel(p), n))
                continue
            v = re.fullmatch(r'\s*"(\d+(?:\.\d+)+[0-9A-Za-z.\-]*)"\s*(//.*)?', m.group(1))
            if not v:
                problems.append('%s:%d: FIRMWARE_VERSION is not a quoted version: %s' % (rel(p), n, line.strip()))
            elif version is not None:
                problems.append('%s:%d: FIRMWARE_VERSION defined twice' % (rel(p), n))
            else:
                version = v.group(1)
    if version is None and not problems:
        problems.append('%s: no #define FIRMWARE_VERSION "X.Y"' % rel(CONFIG))

    num = re.compile(r'(?<![\d.])' + re.escape(version) + r'(?![\d])') if version else None
    for p in fw_files:
        for n, line in enumerate(open(p, encoding='utf-8', errors='replace'), 1):
            if DEFINE.match(line):
                continue
            if STATED.search(line):
                problems.append('%s:%d: version written by hand (use FIRMWARE_VERSION): %s' % (rel(p), n, line.strip()))
                continue
            if num and any(num.search(lit) for lit in LITERAL.findall(line)):
                problems.append('%s:%d: string literal with the version (use FIRMWARE_VERSION): %s' % (rel(p), n, line.strip()))

    others = sorted(glob.glob(os.path.join(ROOT, 'sdcard', '*.html')) + glob.glob(os.path.join(ROOT, 'tools', '*.py')))
    for p in others:
        for n, line in enumerate(open(p, encoding='utf-8', errors='replace'), 1):
            if STATED.search(line):
                problems.append('%s:%d: version written by hand (read it from the firmware): %s' % (rel(p), n, line.strip()))

    for x in problems:
        print('FAIL ' + x)
    if problems:
        return 1
    print('firmware version %s, only in %s (%d firmware files, %d pages and tools checked)' % (
        version, rel(CONFIG), len(fw_files), len(others)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
