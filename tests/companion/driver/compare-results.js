'use strict'
// Compares two run-cases.js result files case by case (e.g. firmware v3.7.1 vs v3.8).
//
//   node compare-results.js old.json new.json [--out diff.json] [--show 25] [--ignore responseText,tcpRx]
//
// Aspects compared per case: requests (what the MODULE sent: method + URL + body; a difference
// here means the module state diverged, usually because of different TCP echoes), status,
// responseText, tcpRx, tcpTx, verdict, expectations (failed ones), variables.

const fs = require('fs')

const args = process.argv.slice(2)
const files = args.filter((a, i) => !a.startsWith('--') && !(i > 0 && args[i - 1].startsWith('--')))
if (files.length !== 2) {
	console.log(fs.readFileSync(__filename, 'utf8').split('\n').slice(1, 8).join('\n'))
	process.exit(2)
}
const opt = (k, d) => {
	const i = args.indexOf(k)
	return i >= 0 ? args[i + 1] : d
}
const show = +opt('--show', 25)
const ignore = new Set(String(opt('--ignore', '')).split(',').filter(Boolean))

const load = (f) => JSON.parse(fs.readFileSync(f, 'utf8'))
const A = load(files[0])
const B = load(files[1])

const norm = (s) => (s === null || s === undefined ? s : String(s).replace(/\r\n/g, '\n').trim())
const aspects = {
	requests: (r) => r.http.map((e) => `${e.method} ${e.wireUrl || e.url}${e.body ? ' ' + e.body : ''}`),
	status: (r) => r.http.map((e) => e.status ?? (e.error && (e.error.code || e.error.name)) ?? null),
	responseText: (r) => r.http.map((e) => norm(e.responseText)),
	tcpRx: (r) => r.tcp.filter((t) => t.dir === 'rx' && t.line !== 'PONG').map((t) => t.line),
	tcpTx: (r) => r.tcp.filter((t) => t.dir === 'tx' && t.line !== 'PING').map((t) => t.line),
	verdict: (r) => r.verdict,
	expectations: (r) => r.expectations.filter((x) => !x.ok).map((x) => `${x.what} ${x.key || ''}=${JSON.stringify(x.actual)}`),
	variables: (r) => r.variables,
}

const byId = (doc) => new Map(doc.results.map((r) => [r.id, r]))
const ma = byId(A)
const mb = byId(B)
const onlyA = [...ma.keys()].filter((k) => !mb.has(k))
const onlyB = [...mb.keys()].filter((k) => !ma.has(k))
const diffs = []
const counts = {}
for (const [id, ra] of ma) {
	const rb = mb.get(id)
	if (!rb) continue
	const d = { id, suite: ra.suite, action: ra.action, aspects: {} }
	for (const [name, fn] of Object.entries(aspects)) {
		if (ignore.has(name)) continue
		const va = fn(ra)
		const vb = fn(rb)
		if (JSON.stringify(va) !== JSON.stringify(vb)) {
			d.aspects[name] = { a: va, b: vb }
			counts[name] = (counts[name] || 0) + 1
		}
	}
	if (Object.keys(d.aspects).length) diffs.push(d)
}

console.log(`A: ${files[0]} (${A.meta.host}, ${A.results.length} cases, pass ${A.summary.pass} warn ${A.summary.warn} fail ${A.summary.fail})`)
console.log(`B: ${files[1]} (${B.meta.host}, ${B.results.length} cases, pass ${B.summary.pass} warn ${B.summary.warn} fail ${B.summary.fail})`)
console.log(`only in A: ${onlyA.length}  only in B: ${onlyB.length}  cases with differences: ${diffs.length}`)
console.log('differences by aspect: ' + JSON.stringify(counts))
for (const d of diffs.slice(0, show)) {
	console.log(`\n# ${d.id}  [${d.suite}]`)
	for (const [k, v] of Object.entries(d.aspects)) {
		const s = (x) => {
			const j = JSON.stringify(x)
			return j && j.length > 400 ? j.slice(0, 400) + '…' : j
		}
		console.log(`  ${k}:\n    A ${s(v.a)}\n    B ${s(v.b)}`)
	}
}
if (diffs.length > show) console.log(`\n… ${diffs.length - show} more (use --show N or --out)`)
const out = opt('--out')
if (out) {
	fs.writeFileSync(out, JSON.stringify({ a: files[0], b: files[1], counts, onlyA, onlyB, diffs }, null, 1))
	console.log('diff written to ' + out)
}
