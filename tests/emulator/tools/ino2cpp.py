#!/usr/bin/env python3
"""Arduino-style .ino -> .cpp: #include <Arduino.h>, prototypes for top-level
functions inserted before the first function definition, #line directives."""
import re, sys
src, dst = sys.argv[1], sys.argv[2]
lines = open(src, encoding='utf-8', errors='replace').read().split('\n')
proto_re = re.compile(r'^((?:static\s+|inline\s+)*[A-Za-z_][\w:<>\*&\s]*?[\s\*&]+)([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{?\s*(//.*)?$')
protos, first = [], None
depth = 0
for i, l in enumerate(lines):
    s = l.strip()
    if depth == 0 and not l.startswith((' ', '\t', '#', '/', '*')):
        m = proto_re.match(l.rstrip())
        if m and m.group(2) not in ('if', 'while', 'for', 'switch', 'return'):
            ret = m.group(1).strip()
            if ret not in ('else',):
                protos.append('%s %s(%s);' % (ret, m.group(2), m.group(3).strip()))
                if first is None:
                    first = i
    # crude brace tracking (ignores braces in strings/comments mostly fine for this sketch)
    code = re.sub(r'"(\\.|[^"\\])*"', '""', l)
    code = re.sub(r"'(\\.|[^'\\])*'", "''", code)
    code = code.split('//')[0]
    depth += code.count('{') - code.count('}')
out = ['#include <Arduino.h>', '#line 1 "%s"' % src]
for i, l in enumerate(lines):
    if i == first:
        out.extend(protos)
        out.append('#line %d "%s"' % (i + 1, src))
    out.append(l)
open(dst, 'w').write('\n'.join(out))
print('prototypes:', len(protos), file=sys.stderr)
