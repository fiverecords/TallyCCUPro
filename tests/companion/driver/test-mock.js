'use strict'
// Smoke test of the harness against the trivial mock (127.0.0.9:80 + :8098).
// Checks that requests leave the real module, reach the server and are recorded.
//
//   node test-mock.js            quick checks + full cases.json (slow cases excluded)
//   node test-mock.js --quick    only the quick checks
//   node test-mock.js --slow     also the slow cases (TCP reconnect 5 s, PING 30 s)

const assert = require('assert/strict')
const path = require('path')
const { createHarness } = require('./driver')
const { startMock } = require('./mock-server')
const { runPlan } = require('./run-cases')

const HOST = process.env.MOCK_HOST || '127.0.0.9'
const QUICK = process.argv.includes('--quick')
const SLOW = process.argv.includes('--slow')

let passed = 0
function ok(cond, msg) {
	assert.ok(cond, msg)
	passed++
	console.log('  ok  ' + msg)
}
const wirePath = (u) => {
	const x = new URL(u)
	return x.pathname + x.search
}
function pidAlive(pid) {
	try {
		process.kill(pid, 0)
		return true
	} catch (_e) {
		return false
	}
}

async function quickChecks(mock) {
	console.log('quick checks against mock ' + HOST)
	const h = await createHarness({ host: HOST })
	const pid = h.child.pid
	try {
		ok(h.baseVersion === '1.14.1', `real @companion-module/base ${h.baseVersion} loaded`)
		ok(h.apiVersion === '1.14.1', 'module registered with apiVersion 1.14.1 over Node IPC')
		ok(h.configFields.some((f) => f.id === 'host') && h.configFields.some((f) => f.id === 'defaultCameraId'), 'getConfigFields returned host + defaultCameraId')
		ok(h.config.host === HOST && h.config.defaultCameraId === 1, 'init config = field defaults + host')
		ok(h.actions().length === 289, `h.actions() lists ${h.actions().length} action definitions`)
		ok(h.actions().every((a) => a.id && Array.isArray(a.options)), 'every definition has id and options')
		ok(h.feedbacks().length === 7 && h.variableDefinitions.length === 775, '7 feedbacks and 775 variables defined')
		ok(h.status && h.status.status === 'ok', 'status ok after the initial check: ' + JSON.stringify(h.status && h.status.message))
		const first = h.http[0]
		ok(first && first.method === 'GET' && first.url === `http://${HOST}/?listPresets` && first.status === 200, 'initial GET /?listPresets recorded with status')
		ok(first.responseText === '{"presets":[]}', 'response body recorded')
		ok(h.tcp.some((e) => e.dir === 'tx' && e.line === 'SUBSCRIBE CCU'), 'TCP tx SUBSCRIBE CCU recorded')
		ok(h.tcp.some((e) => e.dir === 'rx' && e.line === 'SUBSCRIBED OK'), 'TCP rx SUBSCRIBED OK recorded')
		ok(mock.tcp.some((e) => e.dir === 'rx' && e.line === 'SUBSCRIBE CCU'), 'mock received SUBSCRIBE CCU')

		// --- one action, end to end
		let m0 = mock.http.length
		let r = await h.run('set_gain_db', { value: 6 })
		ok(r.success && r.http.length === 1, 'run(set_gain_db) -> one fetch')
		ok(r.http[0].url === `http://${HOST}/?cameraId=1&gain_db=6` && r.http[0].status === 200 && r.http[0].responseText === 'OK', 'URL, status and body recorded')
		ok(mock.http.length === m0 + 1 && mock.http[m0].url === '/?cameraId=1&gain_db=6', 'mock received exactly that request')
		ok(r.tcp.some((e) => e.dir === 'rx' && e.line === 'CCU 1 gain_db 6'), 'TCP echo CCU 1 gain_db 6 recorded')
		ok(r.variables.cam1_param_gain_db === '6' && h.variables.cam1_param_gain_db === '6', 'variable cam1_param_gain_db = "6"')
		ok(r.logs.some((l) => l.message.includes('Sending GET')), 'module logs captured')

		// --- defaults are filled like a freshly created Companion action
		r = await h.run('set_manual_white_balance', {})
		ok(r.http[0].url.endsWith('manual_white_balance=5600%2C10'), 'option defaults filled (5600,10 -> 5600%2C10)')
		r = await h.run('set_gain_db', { cameraId: 8 }, { fillDefaults: false })
		ok(r.http[0].url.endsWith('cameraId=8&gain_db=undefined'), 'fillDefaults:false sends options as given (value undefined)')

		// --- POST body
		m0 = mock.http.length
		const name = "Cámara & 100% Joaquin's ABCDEFGHIJKLMN"
		r = await h.run('save_current_as_preset', { cameraId: 2, presetId: 3, presetName: name })
		const post = r.http[0]
		ok(post.method === 'POST' && post.url === `http://${HOST}/savePreset`, 'POST /savePreset recorded')
		ok(post.form.find((p) => p.key === 'name').value === name, 'form field name decoded = original name')
		ok(mock.http[m0].body === post.body, 'mock received the same body byte for byte')
		ok(r.tcp.some((e) => e.line === `PRESETSAVED 2 3 ${name}`) && h.variables.cam2_preset3_name === name, 'PRESETSAVED push -> variable cam2_preset3_name')

		// --- presets: existing / nonexistent / list
		r = await h.run('load_preset', { cameraId: 2, presetId: 3 })
		ok(r.http[0].status === 200 && r.logs.some((l) => l.message === 'Preset loaded successfully'), 'load existing preset')
		ok(h.variables.cam2_active_preset_id === '3', 'active preset variable updated')
		r = await h.run('load_preset', { cameraId: 7, presetId: 4 })
		ok(r.http[0].status === 404 && r.logs.some((l) => l.level === 'warn'), 'load nonexistent preset -> 404 + module warning')
		r = await h.run('list_presets', {})
		ok(r.http[0].url.endsWith('/?listPresets') && r.http[0].responseText.includes(name), 'list_presets response recorded')

		// --- vmix
		r = await h.run('set_vmix_connect', { enabled: 'false' })
		ok(r.http[0].url.endsWith('/?vmixConnect=0') && mock.state.vmixConnect === 0, 'vmixConnect=0')
		r = await h.run('set_vmix_connect', { enabled: 'true' })
		ok(r.http[0].url.endsWith('/?vmixConnect=1') && mock.state.vmixConnect === 1, 'vmixConnect=1')

		// --- server push, injection, REQUESTSYNC -> CCUSYNC
		let mk = h.mark()
		mock.push('CCU 1 gain_db 20')
		ok(!!(await h.waitForTcp('CCU 1 gain_db 20', { mark: mk })), 'server push received and recorded')
		await h.settle(mk)
		r = await h.run('set_gain_db_increment', {})
		ok(r.http[0].url.endsWith('gain_db=21'), 'increment after TCP value -> 21')
		mk = h.mark()
		await h.injectTcpRx('CCU 1 gain_db 30')
		await h.settle(mk)
		ok(h.since(mk).tcp.some((e) => e.dir === 'rx' && e.line === 'CCU 1 gain_db 30' && e.injected), 'local injection marked injected')
		mk = h.mark()
		const mt = mock.tcp.length
		mock.push('REQUESTSYNC')
		const sync = await h.waitForTcp('CCUSYNC 1 gain_db', { dir: 'tx', mark: mk })
		await h.settle(mk)
		ok(sync && sync.line === 'CCUSYNC 1 gain_db 30', 'REQUESTSYNC -> module sends CCUSYNC (tx recorded)')
		const txLines = h.since(mk).tcp.filter((e) => e.dir === 'tx').map((e) => e.line)
		const mockRx = mock.tcp.slice(mt).filter((e) => e.dir === 'rx').map((e) => e.line)
		ok(txLines.length > 3 && JSON.stringify(txLines) === JSON.stringify(mockRx), `all ${txLines.length} CCUSYNC lines recorded == received by mock`)

		// --- feedback through the real FeedbackManager
		await h.run('set_optical_image_stabilisation', { value: false })
		const fb = await h.addFeedback('ois_enabled', { cameraId: 1 })
		ok(h.feedbackValues[fb] === false, 'feedback ois_enabled = false')

		// --- external HTTP (as web UI) -> broadcast -> module state
		mk = h.mark()
		const extIndex = mock.http.length
		const ext = await h.externalHttp('/?cameraId=1&gain_db=10')
		ok(mock.http[extIndex].url === '/?cameraId=1&gain_db=10', 'external request reached the mock')
		ok(ext.status === 200 && h.http.length === mk.http, 'externalHttp is not recorded as module traffic')
		ok(!!(await h.waitForTcp('CCU 1 gain_db 10', { mark: mk })), 'broadcast after external change reaches the module')

		// --- cross-check: every module request the harness saw reached the mock, in order
		const harnessPaths = h.http.map((e) => e.method + ' ' + wirePath(e.wireUrl))
		const mockPaths = mock.http.filter((_, i) => i !== extIndex).map((e) => e.method + ' ' + e.url)
		ok(JSON.stringify(harnessPaths) === JSON.stringify(mockPaths), `harness h.http (${harnessPaths.length}) == mock received (${mockPaths.length}), same order`)

		// --- config update path (configUpdated) with another harness started without host
		const h2 = await createHarness({ host: '' })
		ok(h2.status.status === 'bad_config' && h2.http.length === 0, 'no host -> bad_config, no traffic')
		await h2.updateConfig({ host: HOST })
		await h2.waitReady()
		ok(h2.http.length >= 1 && h2.tcpState().subscribed, 'updateConfig(host) -> module connects (HTTP + TCP)')
		await h2.close()
		ok(h2.exit && h2.exit.code === 0, 'second harness closed cleanly')

		// --- captureBodies:false
		const h3 = await createHarness({ host: HOST, captureBodies: false })
		r = await h3.run('set_focus', { value: 0.5 })
		ok(r.http[0].status === 200 && r.http[0].responseText === null && r.http[0].done, 'captureBodies:false still records status, no body')
		await h3.close()

		if (SLOW) {
			mk = h.mark()
			await h.dropTcp()
			const resub = await h.waitForTcp('SUBSCRIBE CCU', { dir: 'tx', mark: mk, timeout: 9000 })
			ok(!!resub, 'dropTcp -> module reconnects after 5 s and resubscribes')
			mk = h.mark()
			const ping = await h.waitForTcp('PING', { dir: 'tx', mark: mk, timeout: 33000 })
			const pong = await h.waitForTcp('PONG', { dir: 'rx', mark: mk, timeout: 3000 })
			ok(!!ping && !!pong, 'PING every 30 s answered with PONG')
		}
	} finally {
		await h.close()
	}
	ok(h.exit && h.exit.code === 0 && !pidAlive(pid), `close(): module process ${pid} exited (code ${h.exit && h.exit.code})`)
	ok(h.crashes.length === 0 && h.stderr.filter((l) => l.trim()).length === 0, 'no crashes, nothing on stderr')
}

async function fullPlan(mock) {
	console.log('\nfull cases.json against mock (slow cases ' + (SLOW ? 'included' : 'excluded') + ')')
	const before = mock.http.length
	const res = await runPlan({
		host: HOST,
		cases: path.join(__dirname, 'cases.json'),
		tier: 'core,extended,robustness',
		excludeTags: SLOW ? '' : 'slow',
		mockInstance: mock,
		out: path.join(__dirname, 'results', 'mock-full.json'),
		silent: true,
	})
	const s = res.summary
	console.log(`  ${s.total} cases: pass ${s.pass} warn ${s.warn} fail ${s.fail}; http ${JSON.stringify(s.httpStatus)}`)
	ok(s.fail === 0, 'no case failed')
	const reqs = res.results.reduce((n, r) => n + r.http.length, 0)
	ok(reqs > 1500 && reqs <= mock.http.length - before, `${reqs} module requests recorded in results, all of them reached the mock`)
	ok(res.results.every((r) => r.http.every((e) => e.status !== null)), 'every recorded request has a response status')
	const exp = res.results.flatMap((r) => r.expectations)
	const bad = exp.filter((x) => !x.ok)
	ok(bad.length === 0, `${exp.length} module-side expectations hold against the mock` + (bad.length ? ': ' + JSON.stringify(bad.slice(0, 3)) : ''))
	const noreq = res.results.filter((r) => r.kind === 'single' && r.http.length === 0).map((r) => r.id)
	ok(JSON.stringify(noreq) === JSON.stringify(['util:retry-connection', 'preset:save-no-groups']), 'only retry_connection (TCP up) and save with no groups send nothing: ' + noreq.join(', '))
	ok(res.suites.every((x) => x.exit && x.exit.code === 0), `${res.suites.length} module instances started and exited cleanly`)
	console.log('  results: ' + res.out)
}

;(async () => {
	const mock = await startMock({ host: HOST })
	try {
		await quickChecks(mock)
		if (!QUICK) await fullPlan(mock)
		console.log(`\nALL ${passed} CHECKS PASSED`)
	} finally {
		await mock.close()
	}
})().catch((e) => {
	console.error('\nFAILED:', e.message)
	process.exitCode = 1
})
