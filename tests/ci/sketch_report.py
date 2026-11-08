#!/usr/bin/env python3
"""Reads the sketches report of arduino/compile-sketches and checks the firmware size.

  python3 tests/ci/sketch_report.py REPORT_DIR_OR_FILE [--ram-limit 4600]

Prints flash, static RAM ("RAM for global variables" = .data + .bss + .noinit) and the warning count of
every compiled sketch (also to $GITHUB_STEP_SUMMARY when set). Fails if a sketch did not
compile, if no RAM figure is found, or if the static RAM is above the limit.
"""
import argparse, glob, json, os, sys


def absolute(entry):
    cur = entry.get('current', {}) if isinstance(entry, dict) else {}
    v = cur.get('absolute') if isinstance(cur, dict) else None
    return v if isinstance(v, int) else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('report')
    ap.add_argument('--ram-limit', type=int, default=int(os.environ.get('RAM_LIMIT', 4600)))
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.report, '*.json'))) if os.path.isdir(a.report) else [a.report]
    if not files:
        print('no sketches report in %s' % a.report)
        return 1
    rows, bad = [], []
    for f in files:
        doc = json.load(open(f))
        for board in doc.get('boards', []):
            for sk in board.get('sketches', []):
                sizes = {s.get('name'): s for s in sk.get('sizes', []) if isinstance(s, dict)}
                flash = absolute(sizes.get('flash', {}))
                ram_entry = next((s for n, s in sizes.items() if n and 'ram' in n.lower()), {})
                ram = absolute(ram_entry)
                warn = sk.get('warnings')
                nwarn = absolute(warn) if isinstance(warn, dict) else None
                name = '%s %s' % (board.get('board'), sk.get('name'))
                ok = sk.get('compilation_success', False)
                rows.append((name, ok, flash, sizes.get('flash', {}).get('maximum'), ram, ram_entry.get('maximum'), nwarn))
                if not ok:
                    bad.append('%s: compilation failed' % name)
                elif ram is None:
                    bad.append('%s: no RAM size in the report (keys: %s)' % (name, list(sizes)))
                elif ram > a.ram_limit:
                    bad.append('%s: static RAM %d B > limit %d B' % (name, ram, a.ram_limit))
    lines = ['| sketch | compiled | flash (B) | static RAM (B) | RAM limit | warnings |', '|---|---|---|---|---|---|']
    for name, ok, flash, fmax, ram, rmax, nwarn in rows:
        lines.append('| %s | %s | %s / %s | %s / %s | %d | %s |' % (name, 'yes' if ok else 'NO', flash, fmax, ram, rmax,
                                                               a.ram_limit, '-' if nwarn is None else nwarn))
    text = '\n'.join(lines + [''] + ['**%s**' % b for b in bad]) + '\n'
    print(text)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as s:
            s.write('### Firmware size\n\n' + text)
    if not rows:
        print('no sketches in the report')
        return 1
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
