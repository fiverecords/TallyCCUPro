'use strict'
// Runs cases.json through the real Companion module against a host and writes a results file.
//
//   node run-cases.js --host 127.0.0.3 [--out results.json] [--cases cases.json]
//        [--tier core,extended] [--suite REGEX] [--case REGEX] [--exclude-tags slow]
//        [--quiet-ms 120] [--settle-ms 4000] [--gap-ms 0] [--suite-gap-ms 300]
//        [--limit N] [--mock] [--verbose]
//
// --mock starts mock-server.js on --host (HTTP 80 + TCP 8098) for the duration of the run.

const fs = require('fs')
const path = require('path')
const { createHarness } = require('./driver')

function parseArgs(argv) {
	const a = {
		cases: path.join(__dirname, 'cases.json'),
		tier: 'core,extended',
		quietMs: 120,
		settleMs: 4000,
		gapMs: 0,
		suiteGapMs: 300,
		excludeTags: 'slow',
	}
	for (let i = 0; i < argv.length; i++) {
		const k = argv[i]
		const v = () => argv[++i]
		switch (k) {
			case '--host':
				a.host = v()
				break
			case '--out':
				a.out = v()
				break
			case '--cases':
				a.cases = v()
				break
			case '--tier':
				a.tier = v()
				break
			case '--suite':
				a.suite = new RegExp(v())
				break
			case '--case':
				a.case = new RegExp(v())
				break
			case '--exclude-tags':
				a.excludeTags = v()
				break
			case '--quiet-ms':
				a.quietMs = +v()
				break
			case '--settle-ms':
				a.settleMs = +v()
				break
			case '--gap-ms':
				a.gapMs = +v()
				break
			case '--suite-gap-ms':
				a.suiteGapMs = +v()
				break
			case '--limit':
				a.limit = +v()
				break
			case '--mock':
				a.mock = true
				break
			case '--verbose':
				a.verbose = true
				break
			case '-h':
			case '--help':
				a.help = true
				break
			default:
				throw new Error('unknown argument ' + k)
		}
	}
	return a
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const trim = (s, n = 2000) => (s && s.length > n ? s.slice(0, n) + `…[+${s.length - n}]` : s)

function slimHttp(e, t0) {
	return {
		dt: e.t - t0,
		method: e.method,
		url: e.url,
		wireUrl: e.wireUrl !== e.url ? e.wireUrl : undefined,
		query: e.query.map((p) => [p.key, p.value]),
		body: e.body,
		form: e.form ? e.form.map((p) => [p.key, p.value]) : undefined,
		status: e.status,
		responseText: trim(e.responseText),
		error: e.error || e.bodyError || undefined,
		ms: e.ms !== null ? Math.round(e.ms) : null,
	}
}
function slimTcp(e, t0) {
	const o = { dt: e.t - t0, dir: e.dir }
	if (e.line !== undefined) o.line = e.line
	if (e.event) o.event = e.event
	if (e.error) o.error = e.error
	if (e.injected) o.injected = true
	if (e.unterminated) o.unterminated = true
	return o
}

function lastPair(entries, field, key) {
	let val
	let found = false
	for (const e of entries) {
		for (const p of e[field] || []) {
			if (p.key === key) {
				val = p.value
				found = true
			}
		}
	}
	return found ? val : undefined
}

function evalExpect(h, expect, r, stepIndex) {
	const out = []
	if (!expect) return out
	if (expect.requests !== undefined && expect.requests !== null) {
		out.push({ step: stepIndex, what: 'requests', expected: expect.requests, actual: r.http.length, ok: r.http.length === expect.requests })
	}
	for (const [k, v] of Object.entries(expect.query || {})) {
		const actual = lastPair(r.http, 'query', k)
		out.push({ step: stepIndex, what: 'query', key: k, expected: v, actual, ok: actual === v })
	}
	for (const [k, v] of Object.entries(expect.form || {})) {
		const actual = lastPair(r.http, 'form', k)
		out.push({ step: stepIndex, what: 'form', key: k, expected: v, actual, ok: actual === v })
	}
	for (const [k, v] of Object.entries(expect.variable || {})) {
		const actual = h.variables[k]
		out.push({ step: stepIndex, what: 'variable', key: k, expected: v, actual, ok: actual === v })
	}
	if (expect.log) {
		const hit = r.logs.find((l) => l.level === expect.log.level && l.message.includes(expect.log.match))
		out.push({ step: stepIndex, what: 'log', expected: `${expect.log.level}: ${expect.log.match}`, actual: hit ? hit.message : null, ok: !!hit })
	}
	return out
}

async function runCase(h, c, ctx) {
	const caseMark = h.mark()
	const t0 = Date.now()
	const steps = c.steps || [{ do: 'action', action: c.action, options: c.options, expect: c.expect }]
	const stepResults = []
	const expectations = []
	const warnings = []
	let fail = null
	let lastMark = caseMark
	const runOpts = { quietMs: ctx.quietMs, settleMs: ctx.settleMs, fillDefaults: true }

	for (let i = 0; i < steps.length; i++) {
		const s = steps[i]
		const sm = h.mark()
		const sr = { i, do: s.do }
		try {
			switch (s.do) {
				case 'action': {
					const r = await h.run(s.action, s.options || {}, runOpts)
					sr.action = s.action
					sr.success = r.success
					if (r.errorMessage) sr.errorMessage = r.errorMessage
					sr.requests = r.http.map((e) => e.method + ' ' + e.url + (e.body ? ' <' + e.body.length + 'B>' : '') + ' -> ' + (e.status ?? (e.error && e.error.code) ?? 'ERR'))
					if (!r.success) fail = fail || `step ${i} action ${s.action} failed: ${r.errorMessage}`
					expectations.push(...evalExpect(h, s.expect, r, i))
					break
				}
				case 'externalHttp': {
					const r = await h.externalHttp(s.path, { method: s.method, body: s.body })
					Object.assign(sr, { path: s.path, status: r.status, text: trim(r.text, 300), error: r.error })
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
				case 'serverPush': {
					if (ctx.mock) {
						sr.via = 'mock'
						sr.clients = ctx.mock.push(s.line)
					} else {
						sr.via = 'inject'
						await h.injectTcpRx(s.line)
					}
					sr.line = s.line
					await h.settle(sm, runOpts)
					break
				}
				case 'inject':
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
					await sleep(50)
					sr.feedback = s.feedback
					sr.value = h.feedbackValues[id]
					if (s.expect !== undefined) {
						expectations.push({ step: i, what: 'feedback', key: s.feedback, expected: s.expect, actual: sr.value, ok: sr.value === s.expect })
					}
					break
				}
				case 'dropTcp':
					await h.dropTcp()
					break
				case 'wait':
					await sleep(s.ms || 0)
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
		if (h.exit) {
			fail = fail || 'module process exited: ' + JSON.stringify(h.exit)
			break
		}
		if (ctx.gapMs) await sleep(ctx.gapMs)
	}

	const all = h.since(caseMark)
	for (const e of all.http) {
		if (e.error) warnings.push(`http error ${e.method} ${e.url}: ${e.error.name} ${e.error.code || e.error.message}`)
		else if (e.status >= 400) warnings.push(`http ${e.status} ${e.method} ${e.url}`)
		if (e.bodyError) warnings.push(`http body error ${e.url}: ${e.bodyError.name}`)
	}
	for (const l of all.logs) if (l.level === 'error' || l.level === 'warn') warnings.push(`module ${l.level}: ${l.message}`)
	for (const e of all.tcp) if (e.dir === 'event' && e.event !== 'connect') warnings.push(`tcp ${e.event}${e.error ? ' ' + (e.error.code || e.error.message) : ''}`)
	for (const x of expectations) if (!x.ok) warnings.push(`expect ${x.what}${x.key ? ' ' + x.key : ''}: expected ${JSON.stringify(x.expected)} got ${JSON.stringify(x.actual)}`)
	if (h.crashes.length) fail = fail || 'module crashed'

	return {
		id: c.id,
		suite: c.suite,
		kind: c.kind,
		tier: c.tier,
		mode: c.mode,
		action: c.action,
		options: c.options,
		verdict: fail ? 'fail' : warnings.length ? 'warn' : 'pass',
		fail: fail || undefined,
		warnings,
		expectations,
		ms: Date.now() - t0,
		steps: stepResults,
		http: all.http.map((e) => slimHttp(e, t0)),
		tcp: all.tcp.map((e) => slimTcp(e, t0)),
		logs: all.logs.map((l) => ({ dt: l.t - t0, level: l.level, message: l.message })),
		statusChanges: all.status.map((s) => ({ status: s.status, message: s.message })),
		variables: all.variables,
	}
}

const DEFAULTS = parseArgs([])

// Programmatic entry point. args: same fields as the CLI (host, cases, tier, suite, case,
// excludeTags, quietMs, settleMs, gapMs, suiteGapMs, limit, mock, verbose, out) plus
// mockInstance (an already started mock-server to use for serverPush) and write:false.
async function runPlan(userArgs) {
	const args = { ...DEFAULTS, ...userArgs }
	const log = args.silent ? () => {} : (...a) => console.log(...a)
	const plan = typeof args.cases === 'string' ? JSON.parse(fs.readFileSync(args.cases, 'utf8')) : args.cases
	const tiers = new Set(args.tier.split(',').filter(Boolean))
	const exTags = new Set((args.excludeTags || '').split(',').filter(Boolean))
	let cases = plan.cases.filter(
		(c) =>
			tiers.has(c.tier) &&
			(!args.suite || args.suite.test(c.suite)) &&
			(!args.case || args.case.test(c.id)) &&
			!(c.tags || []).some((t) => exTags.has(t)),
	)
	if (args.limit) cases = cases.slice(0, args.limit)

	let mock = args.mockInstance || null
	let ownMock = false
	if (!mock && args.mock) {
		const { startMock } = require('./mock-server')
		mock = await startMock({ host: args.host })
		ownMock = true
	}

	const out = args.out || path.join(__dirname, 'results', `results-${args.host}-${new Date().toISOString().replace(/[:.]/g, '-')}.json`)
	if (args.write !== false) fs.mkdirSync(path.dirname(out), { recursive: true })
	const meta = {
		host: args.host,
		mock: !!mock,
		startedAt: new Date().toISOString(),
		casesFile: typeof args.cases === 'string' ? path.resolve(args.cases) : '(object)',
		plan: plan.module,
		filter: { tier: [...tiers], suite: args.suite && String(args.suite), case: args.case && String(args.case), excludeTags: [...exTags] },
		node: process.version,
	}
	const results = []
	const suites = []
	const bySuite = []
	for (const c of cases) {
		let g = bySuite[bySuite.length - 1]
		if (!g || g.suite !== c.suite) bySuite.push((g = { suite: c.suite, cases: [] }))
		g.cases.push(c)
	}
	log(`running ${cases.length} cases in ${bySuite.length} suites against ${args.host}${mock ? ' (mock)' : ''}`)

	let interrupted = false
	const onSigint = () => {
		interrupted = true
	}
	process.on('SIGINT', onSigint)
	for (const g of bySuite) {
		if (interrupted) break
		const t0 = Date.now()
		const firstResult = results.length
		let h
		const sres = { id: g.suite, cases: g.cases.length }
		try {
			h = await createHarness({ host: args.host, readyTimeout: 8000 })
			const first = h.http[0]
			sres.ready = {
				status: h.status,
				tcp: h.tcpState(),
				initialCheck: first ? { url: first.url, status: first.status, responseText: trim(first.responseText, 500), error: first.error } : null,
			}
			sres.ready.tcp.lastEvent = sres.ready.tcp.lastEvent && sres.ready.tcp.lastEvent.event
			for (const c of g.cases) {
				if (interrupted) break
				const r = await runCase(h, c, { ...args, mock })
				results.push(r)
				if (args.verbose || r.verdict === 'fail') {
					log(`  [${r.verdict}] ${r.id} ${r.fail || ''} ${r.warnings.slice(0, 3).join(' | ')}`)
				}
				if (h.exit) break
			}
		} catch (e) {
			sres.error = e.message
			log(`  suite ${g.suite} error: ${e.message}`)
		} finally {
			if (h) {
				sres.exit = await h.close()
				sres.stderr = h.stderr.filter((l) => l.trim()).slice(-20)
			}
			sres.ms = Date.now() - t0
			suites.push(sres)
		}
		const done = results.slice(firstResult)
		const cnt = (v) => done.filter((r) => r.verdict === v).length
		log(`suite ${g.suite}: ${done.length}/${g.cases.length} cases  pass ${cnt('pass')} warn ${cnt('warn')} fail ${cnt('fail')}  ${sres.ms} ms`)
		if (args.suiteGapMs) await sleep(args.suiteGapMs)
	}
	if (ownMock) await mock.close()

	const summary = { total: results.length, pass: 0, warn: 0, fail: 0, httpStatus: {}, warningKinds: {} }
	for (const r of results) {
		summary[r.verdict]++
		for (const e of r.http) {
			const k = e.status ?? (e.error && (e.error.code || e.error.name)) ?? 'none'
			summary.httpStatus[k] = (summary.httpStatus[k] || 0) + 1
		}
		for (const w of r.warnings) {
			const k = w.replace(/[0-9.,-]+/g, '#').replace(/"[^"]*"/g, '"…"').slice(0, 80)
			summary.warningKinds[k] = (summary.warningKinds[k] || 0) + 1
		}
	}
	meta.finishedAt = new Date().toISOString()
	meta.interrupted = interrupted
	process.removeListener('SIGINT', onSigint)
	const doc = { meta, summary, suites, results }
	if (args.write !== false) fs.writeFileSync(out, JSON.stringify(doc, null, 1))
	log('summary', JSON.stringify({ total: summary.total, pass: summary.pass, warn: summary.warn, fail: summary.fail, httpStatus: summary.httpStatus }))
	const topW = Object.entries(summary.warningKinds).sort((a, b) => b[1] - a[1]).slice(0, 12)
	if (topW.length) log('top warnings:\n' + topW.map(([k, n]) => `  ${n}x ${k}`).join('\n'))
	if (args.write !== false) log('results written to ' + out)
	return { ...doc, out: args.write !== false ? out : null }
}

module.exports = { runPlan, runCase, parseArgs }

if (require.main === module) {
	const args = parseArgs(process.argv.slice(2))
	if (args.help || !args.host) {
		console.log(fs.readFileSync(__filename, 'utf8').split('\n').slice(1, 10).join('\n'))
		process.exit(args.help ? 0 : 2)
	}
	runPlan(args).then(
		(r) => process.exit(r.summary.fail ? 1 : 0),
		(e) => {
			console.error(e)
			process.exit(1)
		},
	)
}
