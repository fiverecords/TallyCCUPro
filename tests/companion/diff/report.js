'use strict'
// Summarises a diff.json from compare.js: identical cases, differences grouped by the set of
// aspects that differ, and emulator events (watchdog, boots) of each run. Every difference
// still has to be reviewed by hand (show.js): an expected improvement and a regression look
// the same here.
//   node report.js diff.json [--md report.md] [--examples 5]
const fs = require('fs')
const path = require('path')
const argv = process.argv.slice(2)
const file = argv[0]
if (!file) { console.error('usage: node report.js diff.json [--md report.md] [--examples 5]'); process.exit(2) }
const opt = (k, def) => { const i = argv.indexOf(k); return i > 0 ? argv[i + 1] : def }
const N = +opt('--examples', 5)
const d = JSON.parse(fs.readFileSync(path.resolve(file), 'utf8'))
const groups = new Map()
let same = 0
const only = []
for (const c of d.cases) {
	if (c.onlyIn) { only.push(`${c.id} (only in ${c.onlyIn})`); continue }
	const keys = Object.keys(c.diff || {})
	if (!keys.length) { same++; continue }
	const k = keys.sort().join(',')
	if (!groups.has(k)) groups.set(k, [])
	groups.get(k).push(c.id)
}
const L = []
const verdicts = (side) => {
	const v = {}
	for (const c of d.cases) if (c[side]) v[c[side].verdict] = (v[c[side].verdict] || 0) + 1
	return JSON.stringify(v)
}
L.push(`# Companion differential report`, '')
L.push(`- old: ${d.old.dir} (${d.old.meta.bin})`, `- new: ${d.new.dir} (${d.new.meta.bin})`)
L.push(`- cases: ${d.cases.length}, identical: ${same}, with differences: ${d.cases.length - same - only.length}, unmatched: ${only.length}`)
L.push(`- verdicts old ${verdicts('old')}, new ${verdicts('new')}`)
for (const side of ['old', 'new']) {
	const ev = d[side].stray.emuEvents.filter((e) => e.ev === 'watchdog' || e.ev === 'boot')
	L.push(`- ${side}: ${ev.filter((e) => e.ev === 'watchdog').length} watchdog resets, ${d[side].stray.refused.length} refused connections, ${d[side].stray.bmd.length} BMD writes outside any case`)
}
L.push('', '## Differences by aspect', '', '| aspects | cases | examples |', '|---|---|---|')
for (const [k, ids] of [...groups].sort((a, b) => b[1].length - a[1].length)) L.push(`| ${k} | ${ids.length} | ${ids.slice(0, N).join(', ')} |`)
if (only.length) L.push('', '## Unmatched cases', '', ...only.map((x) => '- ' + x))
L.push('', `Inspect one: node show.js ${file} '<case id regex>' [aspect regex]`)
const md = L.join('\n') + '\n'
console.log(md)
const out = opt('--md')
if (out) fs.writeFileSync(out, md)
