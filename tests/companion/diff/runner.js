'use strict'
// Differential runner: starts ONE emulated firmware + a fake vMix, runs the case plan through the
// REAL Companion module (../driver/driver.js) and records, per case, a wall-clock window so the
// emulator logs (BMD commands, network trace, SD ops, serial, watchdog/boot lines) can be attributed.
// Run it once per firmware version and compare the two directories with compare.js.
//
//   node runner.js --bin EMU --ip 127.0.0.2 --vmix 127.0.0.12 --out DIR
//        [--which LABEL] [--sd DIR | --sd-ref GITREF] [--cases a.json,b.json]
//        [--tier core,extended,robustness] [--suite RE] [--case RE] [--exclude-tags slow]
//        [--quiet-ms 120] [--settle-ms 4000] [--emu-env K=V,K=V]
//
// --bin      emulator binary (tests/emulator/build.sh; --ref builds an older firmware)
// --sd       pages for the emulated SD card (default: sdcard/ of this repo)
// --sd-ref   take sdcard/*.html from a git commit instead (use the same ref as the firmware)
// Exit code 1 if a case fails or the emulator resets by watchdog outside the cases that reboot on
// purpose (waitBoot steps).

const fs = require('fs')
const path = require('path')
const { spawn, execFileSync } = require('child_process')
const { createHarness } = require('../driver/driver.js')
const { FakeVmix } = require('./fake-vmix')

const REPO = path.resolve(__dirname, '..', '..', '..')
const args = {
	tier: 'core,extended,robustness', excludeTags: '', quietMs: 120, settleMs: 4000, which: 'new',
	cases: [path.join(__dirname, '..', 'driver', 'cases.json'), path.join(__dirname, 'static-cases.json')].join(','),
	bin: path.join(REPO, 'tests', 'out', 'emu', 'bin', 'emu'),
}
for (let i = 2; i < process.argv.length; i++) {
	const k = process.argv[i].replace(/^--/, '')
	const v = process.argv[i + 1]
	args[k.replace(/-([a-z])/g, (_, c) => c.toUpperCase())] = v
	i++
}
for (const k of ['ip', 'vmix', 'out']) if (!args[k]) { console.error(`missing --${k}`); process.exit(2) }
const WHICH = args.which
const IP = args.ip
const OUT = path.resolve(args.out)
const EMU_BIN = path.resolve(args.bin)
// SEC-01: the web pages send this header; harmless for firmware that predates it
const CSRF = { 'X-TallyCCU': '1' }
fs.mkdirSync(OUT, { recursive: true })
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const trim = (s, n = 2000) => (s && s.length > n ? s.slice(0, n) + `…[+${s.length - n}]` : s)

// ---------------------------------------------------------------- run dir
const SD = path.join(OUT, 'sd')
fs.rmSync(SD, { recursive: true, force: true })
fs.mkdirSync(SD, { recursive: true })
for (const f of ['index.html', 'safemode.html', 'sdcard.html', 'tally.html']) {
	const data = args.sdRef
		? execFileSync('git', ['-C', REPO, 'show', `${args.sdRef}:sdcard/${f}`])
		: fs.readFileSync(path.join(args.sd || path.join(REPO, 'sdcard'), f))
	fs.writeFileSync(path.join(SD, f), data)
}
for (const f of ['eeprom.bin', 'bmd.jsonl', 'net.jsonl', 'sdops.jsonl', 'serial.jsonl', 'emu.jsonl', 'vmix.jsonl']) fs.rmSync(path.join(OUT, f), { force: true })
const FIFO = path.join(OUT, 'serial.fifo')
fs.rmSync(FIFO, { force: true })
execFileSync('mkfifo', [FIFO])
const fifoFd = fs.openSync(FIFO, 'r+')

// ---------------------------------------------------------------- emulator
const emuLines = [] // {wt, src:'serial'|'emu', line}
const emuEvents = [] // watchdog / boot / exit
let emu = null
let bootCount = 0
let bootAtSerial = null
function startEmu() {
	const env = {
		...process.env,
		EMU_BIND_IP: IP,
		EMU_SD_DIR: SD,
		EMU_EEPROM: path.join(OUT, 'eeprom.bin'),
		EMU_BMD_LOG: path.join(OUT, 'bmd.jsonl'),
		EMU_NET_LOG: path.join(OUT, 'net.jsonl'),
		EMU_SD_LOG: path.join(OUT, 'sdops.jsonl'),
		EMU_SERIAL_IN: FIFO,
		EMU_SERIAL_PACE: '1',
		EMU_REMAP_ALL: args.vmix,
		EMU_MAX_BOOTS: '50',
	}
	for (const kv of (args.emuEnv || '').split(',').filter(Boolean)) { const i = kv.indexOf('='); env[kv.slice(0, i)] = kv.slice(i + 1) }
	emu = spawn(EMU_BIN, [], { env, stdio: ['ignore', 'pipe', 'pipe'] })
	const sink = (src) => {
		let buf = ''
		return (d) => {
			buf += d.toString('latin1')
			let i
			while ((i = buf.indexOf('\n')) >= 0) {
				const line = buf.slice(0, i).replace(/\r$/, '')
				buf = buf.slice(i + 1)
				const e = { wt: Date.now(), src, line }
				emuLines.push(e)
				fs.appendFileSync(path.join(OUT, src === 'serial' ? 'serial.jsonl' : 'emu.jsonl'), JSON.stringify(e) + '\n')
				if (src === 'emu') {
					if (/WATCHDOG RESET/.test(line)) emuEvents.push({ wt: e.wt, ev: 'watchdog', line })
					const m = /boot #(\d+) \(cause: (\S+)\)/.exec(line)
					if (m) { bootCount = +m[1]; emuEvents.push({ wt: e.wt, ev: 'boot', boot: +m[1], cause: m[2] }) }
					if (/RESET \(/.test(line)) emuEvents.push({ wt: e.wt, ev: 'reset', line })
				}
			}
		}
	}
	emu.stdout.on('data', sink('serial'))
	emu.stderr.on('data', sink('emu'))
	emu.on('exit', (code, sig) => { emuEvents.push({ wt: Date.now(), ev: 'exit', code, sig }); emu = null })
}
async function httpGet(p, timeout = 3000, headers) {
	try {
		const r = await fetch(`http://${IP}${p}`, { headers, signal: AbortSignal.timeout(timeout) })
		return { status: r.status, text: await r.text() }
	} catch (e) { return { error: e.cause ? e.cause.code || e.message : e.message } }
}
async function waitHttp(timeout = 20000) {
	const t0 = Date.now()
	while (Date.now() - t0 < timeout) {
		const r = await httpGet('/?listPresets', 1500)
		if (r.status) return true
		await sleep(300)
	}
	return false
}

// ---------------------------------------------------------------- net cut (iptables)
const cutRules = []
function netCut(port) {
	const rules = [
		['-I', 'OUTPUT', '-d', IP, '-p', 'tcp', '--dport', String(port), '-j', 'DROP'],
		['-I', 'OUTPUT', '-s', IP, '-p', 'tcp', '--sport', String(port), '-j', 'DROP'],
	]
	for (const r of rules) { execFileSync('iptables', r); cutRules.push(r) }
}
function netRestore() {
	while (cutRules.length) {
		const r = cutRules.pop()
		try { execFileSync('iptables', ['-D', ...r.slice(1)]) } catch (e) {}
	}
}

// ---------------------------------------------------------------- case runner
function slimHttp(e, t0) {
	return {
		dt: e.t - t0, method: e.method, url: e.url, wireUrl: e.wireUrl !== e.url ? e.wireUrl : undefined,
		body: e.body, status: e.status, responseText: trim(e.responseText), error: e.error || e.bodyError || undefined,
		ms: e.ms !== null && e.ms !== undefined ? Math.round(e.ms) : null,
	}
}
function slimTcp(e, t0) {
	const o = { dt: e.t - t0, dir: e.dir }
	if (e.line !== undefined) o.line = e.line
	if (e.event) o.event = e.event
	if (e.error) o.error = e.error.code || e.error.message || e.error
	if (e.injected) o.injected = true
	return o
}
function lastPair(entries, field, key) {
	let val, found = false
	for (const e of entries) for (const p of e[field] || []) if (p.key === key) { val = p.value; found = true }
	return found ? val : undefined
}
function evalExpect(h, expect, r, i) {
	const out = []
	if (!expect) return out
	if (expect.requests !== undefined && expect.requests !== null) out.push({ step: i, what: 'requests', expected: expect.requests, actual: r.http.length, ok: r.http.length === expect.requests })
	for (const [k, v] of Object.entries(expect.query || {})) { const a = lastPair(r.http, 'query', k); out.push({ step: i, what: 'query', key: k, expected: v, actual: a, ok: a === v }) }
	for (const [k, v] of Object.entries(expect.form || {})) { const a = lastPair(r.http, 'form', k); out.push({ step: i, what: 'form', key: k, expected: v, actual: a, ok: a === v }) }
	for (const [k, v] of Object.entries(expect.variable || {})) { const a = h.variables[k]; out.push({ step: i, what: 'variable', key: k, expected: v, actual: a, ok: a === v }) }
	if (expect.log) {
		const hit = r.logs.find((l) => l.level === expect.log.level && l.message.includes(expect.log.match))
		out.push({ step: i, what: 'log', expected: `${expect.log.level}: ${expect.log.match}`, actual: hit ? hit.message : null, ok: !!hit })
	}
	return out
}

const vmix = new FakeVmix(args.vmix)

async function runCase(h, c) {
	const caseMark = h.mark()
	const t0 = Date.now()
	const steps = c.steps || [{ do: 'action', action: c.action, options: c.options, expect: c.expect }]
	const stepResults = [], expectations = [], warnings = []
	let fail = null
	let lastMark = caseMark
	const runOpts = { quietMs: +args.quietMs, settleMs: +args.settleMs, fillDefaults: true }
	for (let i = 0; i < steps.length; i++) {
		const s = steps[i]
		const sm = h.mark()
		const sr = { i, do: s.do, wt: Date.now() }
		try {
			switch (s.do) {
				case 'action': {
					const r = await h.run(s.action, s.options || {}, { ...runOpts, fillDefaults: s.fillDefaults !== false })
					sr.action = s.action
					sr.success = r.success
					if (r.errorMessage) sr.errorMessage = r.errorMessage
					sr.requests = r.http.map((e) => e.method + ' ' + e.url + (e.body ? ' <' + e.body.length + 'B>' : '') + ' -> ' + (e.status ?? (e.error && e.error.code) ?? 'ERR'))
					if (!r.success) fail = fail || `step ${i} action ${s.action} failed: ${r.errorMessage}`
					expectations.push(...evalExpect(h, s.expect, r, i))
					break
				}
				case 'externalHttp': {
					const headers = s.method === 'POST' ? { ...CSRF, 'Content-Type': 'application/x-www-form-urlencoded' } : { ...CSRF }
					const r = await h.externalHttp(s.path, { method: s.method, body: s.body, headers, timeout: s.timeout || 8000 })
					Object.assign(sr, { path: s.path, status: r.status, text: trim(r.text, 600), error: r.error })
					await h.settle(sm, runOpts)
					break
				}
				case 'waitTcp': {
					const e = await h.waitForTcp(s.match, { dir: s.dir || 'rx', mark: lastMark, timeout: s.timeout || 3000 })
					sr.match = s.match
					sr.found = e ? e.line : null
					if (!e) {
						if (s.optional) warnings.push(`step ${i}: optional waitTcp ${s.dir} "${s.match}" not seen`)
						else fail = fail || `step ${i}: waitTcp ${s.dir} "${s.match}" timed out`
					}
					await h.settle(sm, runOpts)
					break
				}
				case 'serverPush':
				case 'inject':
					sr.via = 'inject'
					await h.injectTcpRx(s.line)
					sr.line = s.line
					await h.settle(sm, runOpts)
					break
				case 'sse': {
					const r = await h.externalSse(s.path || '/events', { holdMs: s.holdMs || 1500 })
					Object.assign(sr, { status: r.status, text: trim(r.text, 4000), error: r.error })
					await h.settle(sm, runOpts)
					break
				}
				case 'feedback': {
					const id = await h.addFeedback(s.feedback, s.options || {})
					await sleep(80)
					sr.feedback = s.feedback
					sr.value = h.feedbackValues[id]
					if (s.expect !== undefined) expectations.push({ step: i, what: 'feedback', key: s.feedback, expected: s.expect, actual: sr.value, ok: sr.value === s.expect })
					break
				}
				case 'dropTcp':
					await h.dropTcp()
					break
				case 'wait':
					await sleep(s.ms || 0)
					break
				// ---- extensions (emulator / environment)
				case 'vmixTally':
					vmix.setTally(s.state)
					await sleep(s.ms || 400)
					break
				case 'vmixDown':
					await vmix.stop()
					break
				case 'vmixUp':
					await vmix.start()
					break
				case 'serial':
					bootAtSerial = bootCount
					fs.writeSync(fifoFd, s.line + '\n')
					await sleep(s.ms || 300)
					break
				case 'waitBoot': {
					const b0 = bootAtSerial !== null ? bootAtSerial : bootCount
					bootAtSerial = null
					const tw = Date.now()
					while (bootCount === b0 && Date.now() - tw < (s.timeout || 20000)) await sleep(100)
					sr.boot = bootCount
					sr.ok = bootCount !== b0
					sr.http = await waitHttp(20000)
					if (!sr.ok) fail = fail || `step ${i}: no reboot seen`
					break
				}
				case 'netCut':
					netCut(s.port || 8098)
					sr.port = s.port || 8098
					break
				case 'netRestore':
					netRestore()
					break
				default:
					throw new Error('unknown step ' + s.do)
			}
		} catch (e) {
			fail = fail || `step ${i} (${s.do}) threw: ${e.message}`
			sr.exception = e.message
		}
		stepResults.push(sr)
		if (!['waitTcp', 'wait', 'feedback'].includes(s.do)) lastMark = sm
		if (h.exit) { fail = fail || 'module process exited: ' + JSON.stringify(h.exit); break }
	}
	const all = h.since(caseMark)
	for (const e of all.http) {
		if (e.error) warnings.push(`http error ${e.method} ${e.url}: ${e.error.name} ${e.error.code || e.error.message}`)
		else if (e.status >= 400) warnings.push(`http ${e.status} ${e.method} ${e.url}`)
	}
	for (const l of all.logs) if (l.level === 'error' || l.level === 'warn') warnings.push(`module ${l.level}: ${l.message}`)
	for (const e of all.tcp) if (e.dir === 'event' && e.event !== 'connect') warnings.push(`tcp ${e.event}${e.error ? ' ' + (e.error.code || e.error.message) : ''}`)
	for (const x of expectations) if (!x.ok) warnings.push(`expect ${x.what}${x.key ? ' ' + x.key : ''}: expected ${JSON.stringify(x.expected)} got ${JSON.stringify(x.actual)}`)
	if (h.crashes.length) fail = fail || 'module crashed'
	return {
		id: c.id, suite: c.suite, kind: c.kind, tier: c.tier, mode: c.mode, action: c.action, options: c.options,
		verdict: fail ? 'fail' : warnings.length ? 'warn' : 'pass', fail: fail || undefined, warnings, expectations,
		wt0: t0, wt1: Date.now(), ms: Date.now() - t0, steps: stepResults,
		http: all.http.map((e) => slimHttp(e, t0)), tcp: all.tcp.map((e) => slimTcp(e, t0)),
		logs: all.logs.map((l) => ({ dt: l.t - t0, level: l.level, message: l.message })),
		statusChanges: all.status.map((s) => ({ status: s.status, message: s.message })),
		variables: all.variables, feedbackValues: { ...h.feedbackValues }, statusNow: h.status,
	}
}

// ---------------------------------------------------------------- main
async function main() {
	const files = args.cases.split(',').filter(Boolean)
	let cases = []
	for (const f of files) cases.push(...JSON.parse(fs.readFileSync(f, 'utf8')).cases)
	const tiers = new Set(args.tier.split(','))
	const exTags = new Set((args.excludeTags || '').split(',').filter(Boolean))
	const reS = args.suite && new RegExp(args.suite)
	const reC = args.case && new RegExp(args.case)
	cases = cases.filter((c) => tiers.has(c.tier) && (!reS || reS.test(c.suite)) && (!reC || reC.test(c.id)) && !(c.tags || []).some((t) => exTags.has(t)))
	const groups = []
	for (const c of cases) {
		let g = groups[groups.length - 1]
		if (!g || g.suite !== c.suite) groups.push((g = { suite: c.suite, cases: [] }))
		g.cases.push(c)
	}
	await vmix.start()
	startEmu()
	if (!(await waitHttp(20000))) throw new Error('emulator did not come up')
	// Same starting configuration on both versions (what a configured installation has): CCU and
	// tally override on, tally slot 0 = vMix input 1 -> camera 1, slot 1 = input 2 -> camera 2.
	const setup = []
	for (const p of ['/?overrideCCU=1&overrideTally=1', '/?tallyCam0Input=1&cam0ID=1&tallyCam1Input=2&cam1ID=2']) {
		setup.push({ p, r: await httpGet(p, 3000, CSRF) })
	}
	await sleep(500)
	const results = [], suites = []
	const wtStart = Date.now()
	console.log(`[${WHICH}] ${cases.length} cases in ${groups.length} suites on ${IP}`)
	for (const g of groups) {
		const t0 = Date.now()
		const sres = { id: g.suite, cases: g.cases.length, wt0: t0 }
		let h
		try {
			if (!emu) { startEmu(); await waitHttp(20000) }
			h = await createHarness({ host: IP, readyTimeout: 10000, label: WHICH })
			sres.ready = { status: h.status, tcp: { ...h.tcpState(), lastEvent: undefined } }
			for (const c of g.cases) {
				const r = await runCase(h, c)
				results.push(r)
				if (r.verdict === 'fail') console.log(`  [${WHICH}] FAIL ${r.id}: ${r.fail}`)
				if (h.exit) break
			}
		} catch (e) {
			sres.error = e.message
			console.log(`  [${WHICH}] suite ${g.suite} error: ${e.message}`)
		} finally {
			netRestore()
			if (h) { sres.exit = await h.close(); sres.stderr = h.stderr.filter((l) => l.trim()).slice(-20) }
			sres.wt1 = Date.now()
			sres.ms = sres.wt1 - t0
			suites.push(sres)
		}
		const done = results.filter((r) => r.suite === g.suite)
		console.log(`[${WHICH}] suite ${g.suite}: ${done.length} cases pass ${done.filter((r) => r.verdict === 'pass').length} warn ${done.filter((r) => r.verdict === 'warn').length} fail ${done.filter((r) => r.verdict === 'fail').length} ${sres.ms} ms`)
		await sleep(400)
	}
	// stop
	if (emu) { emu.kill('SIGKILL'); await sleep(200) }
	await vmix.stop()
	fs.closeSync(fifoFd)
	fs.writeFileSync(path.join(OUT, 'vmix.jsonl'), vmix.events.map((e) => JSON.stringify(e)).join('\n') + '\n')
	const doc = { meta: { which: WHICH, ip: IP, vmix: args.vmix, bin: EMU_BIN, wtStart, wtEnd: Date.now(), setup, node: process.version }, emuEvents, suites, results }
	fs.writeFileSync(path.join(OUT, 'results.json'), JSON.stringify(doc))
	// watchdog resets outside the cases that reboot on purpose
	const rebootWin = results.filter((r) => r.steps.some((st) => st.do === 'waitBoot')).map((r) => [r.wt0, r.wt1 + 1000])
	const strayWdt = emuEvents.filter((e) => e.ev === 'watchdog' && !rebootWin.some(([a, b]) => e.wt >= a && e.wt <= b))
	const n = (v) => results.filter((r) => r.verdict === v).length
	console.log(`[${WHICH}] done: ${results.length} results (pass ${n('pass')}, warn ${n('warn')}, fail ${n('fail')}), ` +
		`${strayWdt.length} unexpected watchdog resets -> ${path.join(OUT, 'results.json')}`)
	for (const e of strayWdt) console.log(`  [${WHICH}] UNEXPECTED ${e.line}`)
	if (n('fail') || strayWdt.length) process.exitCode = 1
}
process.on('SIGINT', () => { netRestore(); if (emu) emu.kill('SIGKILL'); process.exit(130) })
main().catch((e) => { console.error(e); netRestore(); if (emu) emu.kill('SIGKILL'); process.exit(1) })
