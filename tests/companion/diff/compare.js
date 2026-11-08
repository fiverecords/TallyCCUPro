'use strict'
// Joins runner results with the emulator logs (by wall-clock window) and compares old vs new
// case by case. Writes diff.json (raw differences, one record per case with all aspects).
//
//   node compare.js RUN_OLD RUN_NEW diff.json [bmd_params.cpp]
// bmd_params.cpp (default: this repo's) names the parameters in the BMD log.
const fs = require('fs')
const path = require('path')
const [, , DA, DB, OUT, PARAMS_SRC] = process.argv
if (!OUT) { console.error('usage: node compare.js RUN_OLD RUN_NEW diff.json [bmd_params.cpp]'); process.exit(2) }
const PARAMS_FILE = PARAMS_SRC || path.resolve(__dirname, '..', '..', '..', 'firmware', 'TallyCCUPro', 'bmd_params.cpp')

// (group,param) -> {key, subCount, type}
const PARAMS = {}
for (const m of fs.readFileSync(PARAMS_FILE, 'utf8').matchAll(/\{\s*(\d+),\s*(\d+),\s*(TYPE_\w+),\s*"(\w+)",\s*(true|false),\s*(\d+)\s*\}/g)) {
	PARAMS[`${m[1]}.${m[2]}`] = { key: m[4], type: m[3], subCount: +m[6] }
}

function readJsonl(f) {
	if (!fs.existsSync(f)) return []
	return fs.readFileSync(f, 'utf8').split('\n').filter(Boolean).map((l) => { try { return JSON.parse(l) } catch (e) { return { bad: l } } })
}
// Tally and override writes that repeat the current value (the firmware re-sends them
// periodically, TAL-12) are dropped: only changes are compared
const REFRESHED = new Set(['setCameraTally', 'tallySetOverride', 'cameraSetOverride'])
function tallyChanges(list) {
	const last = {}
	return list.filter((e) => {
		if (!REFRESHED.has(e.fn)) return true
		const k = e.fn + ':' + e.cam
		const v = JSON.stringify(e.v)
		if (last[k] === v) return false
		last[k] = v
		return true
	})
}
function load(dir) {
	const doc = JSON.parse(fs.readFileSync(path.join(dir, 'results.json'), 'utf8'))
	return {
		doc,
		bmd: tallyChanges(readJsonl(path.join(dir, 'bmd.jsonl'))),
		net: readJsonl(path.join(dir, 'net.jsonl')),
		sd: readJsonl(path.join(dir, 'sdops.jsonl')),
		serial: readJsonl(path.join(dir, 'serial.jsonl')),
		emu: readJsonl(path.join(dir, 'emu.jsonl')),
	}
}
function fmtBmd(e) {
	const p = PARAMS[`${e.group}.${e.param}`]
	let v = e.v
	if (p && Array.isArray(v) && e.n > p.subCount) v = v.slice(0, p.subCount)
	const vs = (v || []).map((x) => (x && typeof x === 'object' ? (typeof x.f === 'number' ? `${+x.f.toPrecision(7)}(${x.raw})` : JSON.stringify(x)) : x))
	const name = p ? p.key : e.fn === 'setCameraTally' ? 'tally' : e.fn
	return `${e.fn} cam${e.cam} ${name}${e.group >= 0 ? ` [${e.group}.${e.param}]` : ''} = ${vs.join(',')}`
}
function inWin(list, a, b) { return list.filter((e) => e.wt >= a && e.wt <= b) }

function caseView(R, r, nextWt0) {
	// window: from case start to the next case start (or +1 s), so late effects are not lost
	const a = r.wt0, b = Math.min(nextWt0 || r.wt1 + 1000, r.wt1 + 3000)
	const bmd = inWin(R.bmd, a, b).filter((e) => e.fn !== 'begin').map(fmtBmd)
	const net = inWin(R.net, a, b).filter((e) => ['refused', 'conn_fail', 'orphan_rst', 'soft_reset', 'mcu_reset', 'blackhole'].includes(e.ev)).map((e) => `${e.ev}${e.port ? ' port ' + e.port : ''}${e.why ? ' ' + e.why : ''}${e.err ? ' (' + e.err + ')' : ''}`)
	const emu = inWin(R.emu, a, b).map((e) => e.line).filter((l) => /WATCHDOG|boot #|RESET|EXIT|crash|overflow|cannot/i.test(l)).map((l) => l.replace(/ pid \d+.*$/, '').replace(/after \d+ ms/, 'after N ms'))
	const sd = inWin(R.sd, a, b).map((e) => `${e.op} ${path.basename(String(e.path))} ${e.v}`)
	const serialErr = inWin(R.serial, a, b).map((e) => e.line).filter((l) => /error|fail|invalid|unknown|reject|skip|too long/i.test(l))
	const exits = R.doc.emuEvents.filter((e) => e.wt >= a && e.wt <= b)
	return {
		requests: r.http.map((e) => `${e.method} ${(e.wireUrl || e.url).replace(/^http:\/\/[^/]+/, "")}${e.body ? ' ' + e.body : ''}`),
		status: r.http.map((e) => e.status ?? (e.error && (e.error.code || e.error.name || e.error.message)) ?? null),
		responseText: r.http.map((e) => (e.responseText === undefined || e.responseText === null ? e.responseText : String(e.responseText).replace(/\r\n/g, '\n').trim())),
		tcpRx: r.tcp.filter((t) => t.dir === 'rx' && t.line !== 'PONG').map((t) => t.line),
		tcpTx: r.tcp.filter((t) => t.dir === 'tx' && t.line !== 'PING').map((t) => t.line),
		tcpEvents: r.tcp.filter((t) => t.dir === 'event').map((t) => t.event + (t.error ? ' ' + t.error : '')),
		logsWarn: r.logs.filter((l) => l.level === 'warn' || l.level === 'error').map((l) => `${l.level}: ${l.message}`),
		logsInfo: r.logs.filter((l) => l.level !== 'warn' && l.level !== 'error' && l.level !== 'debug' && l.message !== 'Subscribed to CCU changes via TCP').map((l) => `${l.level}: ${l.message.replace(/127\.0\.0\.\d+/g, 'HOST')}`),
		statusChanges: r.statusChanges.map((s) => `${s.status}${s.message ? ': ' + s.message : ''}`),
		variables: r.variables,
		feedback: r.steps.filter((s) => s.do === 'feedback').map((s) => `${s.feedback}=${JSON.stringify(s.value)}`),
		steps: r.steps.filter((s) => s.do === 'externalHttp' || s.do === 'sse' || s.do === 'waitTcp' || s.do === 'waitBoot').map((s) => (s.do === 'waitTcp' ? `waitTcp ${s.match} -> ${s.found ? 'seen' : 'NOT seen'}` : s.do === 'waitBoot' ? `waitBoot ok=${s.ok} http=${s.http}` : `${s.do} ${s.path || ''} -> ${s.status ?? s.error} ${s.do === 'externalHttp' ? (s.text || '').slice(0, 200) : ''}`)),
		verdict: r.verdict,
		fail: r.fail || null,
		bmd,
		net,
		emu,
		sd,
		serialErr,
		emuEvents: exits.map((e) => `${e.ev}${e.cause ? ' ' + e.cause : ''}${e.code !== undefined ? ' code ' + e.code : ''}`),
	}
}

const A = load(DA), B = load(DB)
const idx = (R) => {
	const m = new Map()
	R.doc.results.forEach((r, i) => m.set(r.id, { r, next: R.doc.results[i + 1] && R.doc.results[i + 1].wt0 }))
	return m
}
const ma = idx(A), mb = idx(B)
const out = []
const counts = {}
for (const [id, { r: ra, next: na }] of ma) {
	const x = mb.get(id)
	if (!x) { out.push({ id, onlyIn: 'old' }); continue }
	const va = caseView(A, ra, na), vb = caseView(B, x.r, x.next)
	const diff = {}
	for (const k of Object.keys(va)) {
		if (JSON.stringify(va[k]) !== JSON.stringify(vb[k])) { diff[k] = { old: va[k], new: vb[k] }; counts[k] = (counts[k] || 0) + 1 }
	}
	out.push({ id, suite: ra.suite, action: ra.action, options: ra.options, tier: ra.tier, mode: ra.mode, diff, old: va, new: vb })
}
for (const id of mb.keys()) if (!ma.has(id)) out.push({ id, onlyIn: 'new' })
// emulator-level events outside any case window
function stray(R) {
	const wins = R.doc.results.map((r) => [r.wt0, r.wt1 + 1000])
	const inAny = (wt) => wins.some(([a, b]) => wt >= a && wt <= b)
	return {
		bmd: R.bmd.filter((e) => e.fn !== 'begin' && !inAny(e.wt)).map((e) => ({ wt: e.wt, s: fmtBmd(e) })),
		emuEvents: R.doc.emuEvents,
		netProblems: R.net.filter((e) => ['refused', 'conn_fail', 'orphan_rst'].includes(e.ev)).length,
		refused: R.net.filter((e) => e.ev === 'refused').map((e) => ({ wt: e.wt, port: e.port })),
	}
}
const doc = { generated: new Date().toISOString(), old: { dir: DA, meta: A.doc.meta, suites: A.doc.suites.map((s) => ({ id: s.id, ms: s.ms, error: s.error, ready: s.ready })), stray: stray(A) }, new: { dir: DB, meta: B.doc.meta, suites: B.doc.suites.map((s) => ({ id: s.id, ms: s.ms, error: s.error, ready: s.ready })), stray: stray(B) }, counts, cases: out }
fs.writeFileSync(OUT, JSON.stringify(doc, null, 1))
console.log('cases', out.length, 'with differences', out.filter((c) => c.diff && Object.keys(c.diff).length).length)
console.log('by aspect', counts)
