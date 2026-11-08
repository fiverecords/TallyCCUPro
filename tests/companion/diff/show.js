'use strict'
// Prints the differences of the cases matching a regex.
//   node show.js diff.json CASE_RE [ASPECT_RE] [MAX_CHARS]
const path = require('path')
const [, , file, caseRe, aspectRe, max] = process.argv
if (!file || !caseRe) { console.error('usage: node show.js diff.json CASE_RE [ASPECT_RE] [MAX_CHARS]'); process.exit(2) }
const d = require(path.resolve(file))
const re = new RegExp(caseRe)
const asp = aspectRe ? new RegExp(aspectRe) : null
const L = +max || 600
for (const c of d.cases) {
	if (!re.test(c.id) || !c.diff) continue
	console.log('#', c.id)
	for (const [k, v] of Object.entries(c.diff)) {
		if (asp && !asp.test(k)) continue
		console.log('  ' + k + '\n    OLD ' + JSON.stringify(v.old).slice(0, L) + '\n    NEW ' + JSON.stringify(v.new).slice(0, L))
	}
}
