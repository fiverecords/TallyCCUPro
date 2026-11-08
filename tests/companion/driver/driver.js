'use strict'
// TallyCCU Pro / Companion compatibility harness.
//
// Runs the REAL, unmodified Companion module (copy in ../module) with the REAL
// @companion-module/base it depends on, in a child process forked exactly the way Companion's
// module host does it (MODULE_MANIFEST / CONNECTION_ID / VERIFICATION_TOKEN env + Node IPC).
// This file plays the role of the Companion host: it answers `register`, asks for the config
// fields, sends `init` with a configured connection, and triggers actions with `executeAction`
// (the module receives event.options and the real context object built by the base library).
// preload.js (injected with --require) records every fetch and every 8098 TCP line.

const { fork } = require('child_process')
const path = require('path')
const fs = require('fs')
const crypto = require('crypto')
const { createRequire } = require('module')
const EventEmitter = require('events')

const DEFAULT_MODULE_DIR = path.resolve(__dirname, '..', 'module')
const PRELOAD = path.join(__dirname, 'preload.js')

function safeDecode(s) {
	try {
		return decodeURIComponent(s)
	} catch (_e) {
		return s
	}
}

// Parse "a=1&b=x%20y" WITHOUT URLSearchParams ('+' is NOT a space for the firmware parser)
function parsePairs(str) {
	if (str === null || str === undefined || str === '') return []
	return String(str)
		.split('&')
		.map((part) => {
			const i = part.indexOf('=')
			const rawKey = i >= 0 ? part.slice(0, i) : part
			const rawValue = i >= 0 ? part.slice(i + 1) : null
			return { key: safeDecode(rawKey), rawKey, value: rawValue === null ? null : safeDecode(rawValue), rawValue }
		})
}

function optionDefaults(def) {
	const out = {}
	for (const opt of (def && def.options) || []) {
		if (opt && opt.id && 'default' in opt && opt.type !== 'static-text') out[opt.id] = clone(opt.default)
	}
	return out
}

function clone(v) {
	return v === undefined ? undefined : JSON.parse(JSON.stringify(v))
}

function sleep(ms) {
	return new Promise((r) => setTimeout(r, ms))
}

class Harness extends EventEmitter {
	constructor(opts) {
		super()
		this.setMaxListeners(0)
		this.opts = { ...opts }
		this.moduleDir = path.resolve(opts.moduleDir || DEFAULT_MODULE_DIR)
		this.label = opts.label || 'tallyccu'
		this.connectionId = opts.connectionId || 'harness-' + crypto.randomBytes(4).toString('hex')
		this.verificationToken = crypto.randomBytes(12).toString('hex')

		// Records (public)
		this.http = [] // one entry per fetch()
		this.tcp = [] // one entry per TCP line (tx/rx) or socket event
		this.tcpRaw = [] // raw chunks as written/received
		this.logs = [] // module log() calls
		this.status = null // last updateStatus()
		this.statusHistory = []
		this.variables = {} // variableId -> value (as the module set it)
		this.variableChanges = [] // {t, id, value}
		this.variableDefinitions = []
		this.actionDefinitions = []
		this.feedbackDefinitions = []
		this.feedbackValues = {}
		this.presetDefinitions = []
		this.savedConfigs = []
		this.customVariables = []
		this.stdout = []
		this.stderr = []
		this.timeline = []
		this.crashes = []
		this.configFields = null
		this.config = null
		this.initResponse = null
		this.exit = null

		this._httpById = new Map()
		this._rxBuf = new Map()
		this._pending = new Map()
		this._ctlPending = new Map()
		this._nextCb = 1
		this._nextCtl = 1
		this._nextAction = 1
		this._lastActivity = Date.now()
		this._actionMap = new Map()
		this._feedbackMap = new Map()
	}

	// ------------------------------------------------------------------ lifecycle
	async start() {
		const manifestPath = path.join(this.moduleDir, 'companion', 'manifest.json')
		const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'))
		this.manifest = manifest
		const entry = path.resolve(path.dirname(manifestPath), manifest.runtime.entrypoint)
		const modRequire = createRequire(path.join(this.moduleDir, 'package.json'))
		this._ejson = modRequire('ejson')
		this.baseVersion = JSON.parse(
			fs.readFileSync(modRequire.resolve('@companion-module/base/package.json'), 'utf8'),
		).version

		const env = {
			...process.env,
			...(this.opts.env || {}),
			MODULE_MANIFEST: manifestPath,
			CONNECTION_ID: this.connectionId,
			VERIFICATION_TOKEN: this.verificationToken,
			HARNESS_MODULE_DIR: this.moduleDir,
			HARNESS_CTL_FD: '4',
			HARNESS_CAPTURE_BODIES: this.opts.captureBodies === false ? '0' : '1',
		}
		this.child = fork(entry, [], {
			cwd: this.moduleDir,
			env,
			execArgv: [...(this.opts.execArgv || []), '--require', PRELOAD],
			stdio: ['ignore', 'pipe', 'pipe', 'ipc', 'pipe'],
			serialization: 'json',
		})
		this._ctl = this.child.stdio[4]
		this._ctl.on('error', () => {})
		this._lines(this.child.stdout, this.stdout, 'stdout')
		this._lines(this.child.stderr, this.stderr, 'stderr')
		this.child.on('message', (msg) => this._onMessage(msg))
		this.exited = new Promise((resolve) => {
			this.child.on('exit', (code, signal) => {
				this.exit = { code, signal, t: Date.now() }
				this._push('process:exit', { code, signal })
				for (const [, p] of this._pending) p.reject(new Error(`module process exited (${code ?? signal})`))
				this._pending.clear()
				for (const [, p] of this._ctlPending) p.reject(new Error('module process exited'))
				this._ctlPending.clear()
				this.emit('exit', this.exit)
				resolve(this.exit)
			})
		})

		const startTimeout = this.opts.startTimeout || 15000
		await this._withTimeout(
			new Promise((resolve, reject) => {
				this._onRegister = resolve
				this.exited.then((e) => reject(new Error('module exited before register: ' + JSON.stringify(e) + '\n' + this.stderr.join('\n'))))
			}),
			startTimeout,
			'module did not register',
		)

		// Companion asks for the config fields to render the connection editor
		const cf = await this.call('getConfigFields', {})
		this.configFields = cf.fields

		// Build the config as Companion stores it after the user configured the connection
		const config = {}
		for (const f of this.configFields) {
			if (f && f.id && 'default' in f && !String(f.type).startsWith('secret')) config[f.id] = clone(f.default)
		}
		Object.assign(config, this.opts.config || {})
		if (this.opts.host !== undefined) config.host = this.opts.host
		if (this.opts.defaultCameraId !== undefined) config.defaultCameraId = this.opts.defaultCameraId
		this.config = config

		this.initResponse = await this.call(
			'init',
			{
				label: this.label,
				isFirstInit: !!this.opts.isFirstInit,
				config,
				secrets: {},
				lastUpgradeIndex: this.opts.lastUpgradeIndex ?? -1,
				feedbacks: {},
				actions: {},
			},
			this.opts.initTimeout || 15000,
		)
		if (this.initResponse && this.initResponse.updatedConfig) this.config = this.initResponse.updatedConfig

		if (this.opts.waitReady !== false) await this.waitReady(this.opts.readyTimeout || 8000)
		return this
	}

	// Wait for the initial HTTP connection check and the first TCP outcome (or SUBSCRIBED OK)
	async waitReady(timeout = 8000) {
		if (!this.config || !this.config.host) return { http: null, tcp: null }
		const wantSub = this.opts.waitSubscribed !== false
		const ok = await this.waitFor(() => {
			const httpDone = this.http.length > 0 && this.http[0].done
			const tcpOutcome = this.tcp.some(
				(e) =>
					(e.dir === 'rx' && e.line === 'SUBSCRIBED OK') ||
					(e.dir === 'event' && (e.event === 'error' || e.event === 'close' || e.event === 'timeout')) ||
					(!wantSub && e.dir === 'event' && e.event === 'connect'),
			)
			return httpDone && tcpOutcome
		}, timeout)
		return { ready: ok, http: this.http[0] || null, tcp: this.tcpState() }
	}

	async close() {
		if (!this.child) return this.exit
		if (!this.exit) {
			try {
				await this.call('destroy', {}, 3000)
			} catch (e) {
				this._push('harness:destroyError', { message: e.message })
			}
			// let late socket events ('close' of the destroyed 8098 socket) reach the records
			await sleep(50)
			try {
				this._ctl.end()
			} catch (_e) {
				// ignore
			}
			try {
				if (this.child.connected) this.child.disconnect()
			} catch (_e) {
				// ignore
			}
			const exited = await Promise.race([this.exited.then(() => true), sleep(1000).then(() => false)])
			if (!exited) {
				this.child.kill('SIGTERM')
				const ex2 = await Promise.race([this.exited.then(() => true), sleep(1500).then(() => false)])
				if (!ex2) this.child.kill('SIGKILL')
			}
		}
		return this.exited
	}

	// ------------------------------------------------------------------ public API
	actions() {
		return this.actionDefinitions.map((a) => ({
			id: a.id,
			name: a.name,
			description: a.description,
			options: a.options,
		}))
	}

	action(id) {
		return this._actionMap.get(id) || null
	}

	feedbacks() {
		return this.feedbackDefinitions
	}

	optionDefaults(actionId) {
		return optionDefaults(this._actionMap.get(actionId))
	}

	mark() {
		return {
			t: Date.now(),
			http: this.http.length,
			tcp: this.tcp.length,
			logs: this.logs.length,
			status: this.statusHistory.length,
			vars: this.variableChanges.length,
			timeline: this.timeline.length,
		}
	}

	since(mark) {
		const vars = {}
		for (const c of this.variableChanges.slice(mark.vars)) vars[c.id] = c.value
		return {
			http: this.http.slice(mark.http),
			tcp: this.tcp.slice(mark.tcp),
			logs: this.logs.slice(mark.logs),
			status: this.statusHistory.slice(mark.status),
			variables: vars,
		}
	}

	// Invoke an action exactly like Companion does (executeAction over IPC).
	// options are merged over the definition defaults (like a freshly created button action),
	// unless runOpts.fillDefaults === false.
	async run(actionId, options = {}, runOpts = {}) {
		const def = this._actionMap.get(actionId)
		if (!def && !runOpts.allowUnknown) throw new Error('Unknown action: ' + actionId)
		const opts = runOpts.fillDefaults === false ? { ...options } : { ...optionDefaults(def), ...options }
		const id = 'harness-action-' + this._nextAction++
		const mark = this.mark()
		const t0 = Date.now()
		let resp
		let ipcError = null
		try {
			resp = await this.call(
				'executeAction',
				{
					action: {
						id,
						controlId: runOpts.controlId || 'bank:harness-1-1-1',
						actionId,
						options: opts,
						upgradeIndex: null,
						disabled: false,
					},
					surfaceId: runOpts.surfaceId,
				},
				runOpts.timeout || 30000,
			)
		} catch (e) {
			ipcError = e.message
		}
		const callbackMs = Date.now() - t0
		await this.settle(mark, runOpts)
		const s = this.since(mark)
		return {
			actionId,
			options: opts,
			success: resp ? resp.success : false,
			errorMessage: resp ? resp.errorMessage : ipcError,
			callbackMs,
			ms: Date.now() - t0,
			...s,
		}
	}

	// Wait until every fetch started since `mark` completed (body or error) and the module has
	// been quiet for quietMs. Bounded by settleMs.
	async settle(mark, { settleMs = 4000, quietMs = 60 } = {}) {
		const deadline = Date.now() + settleMs
		const allDone = () => this.http.slice(mark.http).every((e) => e.done)
		await this.waitFor(allDone, Math.max(0, deadline - Date.now()))
		while (Date.now() < deadline) {
			const idle = Date.now() - this._lastActivity
			if (idle >= quietMs && allDone()) break
			await sleep(Math.min(quietMs, Math.max(5, quietMs - idle)))
		}
	}

	// Resolve true when pred() becomes true (checked on every module activity), false on timeout
	waitFor(pred, timeout = 5000) {
		if (pred()) return Promise.resolve(true)
		return new Promise((resolve) => {
			let timer
			const check = () => {
				let ok = false
				try {
					ok = pred()
				} catch (_e) {
					ok = false
				}
				if (ok) {
					clearTimeout(timer)
					this.off('activity', check)
					resolve(true)
				}
			}
			timer = setTimeout(() => {
				this.off('activity', check)
				resolve(pred())
			}, timeout)
			this.on('activity', check)
		})
	}

	// Wait for a TCP line (dir 'rx' or 'tx') matching a string prefix / RegExp, after `mark`
	async waitForTcp(match, { dir = 'rx', mark = null, timeout = 3000 } = {}) {
		const from = mark ? mark.tcp : 0
		const test = (line) => (match instanceof RegExp ? match.test(line) : line.startsWith(match))
		const find = () => this.tcp.slice(from).find((e) => e.dir === dir && e.line !== undefined && test(e.line))
		await this.waitFor(() => !!find(), timeout)
		return find() || null
	}

	tcpState() {
		const ev = this.tcp.filter((e) => e.dir === 'event')
		const last = ev[ev.length - 1]
		return {
			connections: new Set(this.tcp.map((e) => e.conn)).size,
			connected: !!last && last.event === 'connect',
			subscribed: this.tcp.some((e) => e.dir === 'rx' && e.line === 'SUBSCRIBED OK'),
			lastEvent: last || null,
		}
	}

	// Deliver a line to the module's live 8098 socket as if the server had sent it
	// (local injection: for servers that cannot push on demand). '\r\n' appended if missing.
	injectTcpRx(text, { raw = false } = {}) {
		const payload = raw || /\n$/.test(text) ? text : text + '\r\n'
		return this._ctlCall({ cmd: 'injectRx', text: payload })
	}

	// Destroy the module's TCP socket from the client side (tests the module's reconnect logic)
	dropTcp() {
		return this._ctlCall({ cmd: 'tcpDestroy' })
	}

	async updateConfig(partial) {
		this.config = { ...this.config, ...partial }
		await this.call('updateConfigAndLabel', { label: this.label, config: this.config, secrets: {} })
	}

	// Place a feedback on a virtual button so the module reports its value (updateFeedbackValues)
	async addFeedback(feedbackId, options = {}, { controlId = 'bank:harness-fb' } = {}) {
		const def = this._feedbackMap.get(feedbackId)
		if (!def) throw new Error('Unknown feedback: ' + feedbackId)
		const id = 'harness-fb-' + this._nextAction++
		await this.call('updateFeedbacks', {
			feedbacks: {
				[id]: {
					id,
					controlId,
					feedbackId,
					options: { ...optionDefaults(def), ...options },
					isInverted: false,
					upgradeIndex: null,
					disabled: false,
				},
			},
		})
		await sleep(20)
		return id
	}

	// HTTP request made by the HARNESS itself (not by the module), e.g. to emulate the web UI
	// changing a value so that the firmware broadcasts "CCU ..." to the module.
	async externalHttp(pathOrUrl, { method = 'GET', body, headers, timeout = 5000 } = {}) {
		const url = /^https?:/.test(pathOrUrl) ? pathOrUrl : 'http://' + this.config.host + pathOrUrl
		const t0 = Date.now()
		try {
			const res = await fetch(url, { method, body, headers, signal: AbortSignal.timeout(timeout) })
			const text = await res.text()
			const r = { url, method, body: body ?? null, status: res.status, text, ms: Date.now() - t0 }
			this._push('external:http', r)
			return r
		} catch (e) {
			const r = { url, method, body: body ?? null, error: { name: e.name, message: e.message, code: e.cause && e.cause.code }, ms: Date.now() - t0 }
			this._push('external:http', r)
			return r
		}
	}

	// Open an SSE stream (GET /events) for holdMs and return what arrived (firmware sends
	// REQUESTSYNC to TCP subscribers when an SSE client connects).
	async externalSse(pathname = '/events', { holdMs = 1500 } = {}) {
		const url = 'http://' + this.config.host + pathname
		const ac = new AbortController()
		const t0 = Date.now()
		let text = ''
		let status = null
		let error = null
		const timer = setTimeout(() => ac.abort(), holdMs)
		try {
			const res = await fetch(url, { signal: ac.signal })
			status = res.status
			const reader = res.body.getReader()
			const dec = new TextDecoder()
			for (;;) {
				const { value, done } = await reader.read()
				if (done) break
				text += dec.decode(value, { stream: true })
			}
		} catch (e) {
			if (e.name !== 'AbortError') error = { name: e.name, message: e.message }
		} finally {
			clearTimeout(timer)
		}
		const r = { url, status, text, error, ms: Date.now() - t0 }
		this._push('external:sse', r)
		return r
	}

	// Compact, JSON-friendly dump of everything recorded
	snapshot() {
		return {
			connectionId: this.connectionId,
			baseVersion: this.baseVersion,
			moduleVersion: this.manifest && this.manifest.version,
			config: this.config,
			status: this.status,
			statusHistory: this.statusHistory,
			http: this.http,
			tcp: this.tcp,
			logs: this.logs,
			variables: this.variables,
			crashes: this.crashes,
			exit: this.exit,
			stderr: this.stderr,
		}
	}

	// ------------------------------------------------------------------ host IPC
	call(name, payload, timeout = 10000) {
		if (!this.child || this.exit) return Promise.reject(new Error('module process not running'))
		const callbackId = this._nextCb++
		return new Promise((resolve, reject) => {
			const timer = setTimeout(() => {
				this._pending.delete(callbackId)
				reject(new Error(`IPC call ${name} timed out after ${timeout} ms`))
			}, timeout)
			this._pending.set(callbackId, {
				name,
				resolve: (v) => {
					clearTimeout(timer)
					resolve(v)
				},
				reject: (e) => {
					clearTimeout(timer)
					reject(e)
				},
			})
			this.child.send({ direction: 'call', name, payload: this._ejson.stringify(payload), callbackId })
		})
	}

	_reply(callbackId, success, payload) {
		if (!callbackId || !this.child || !this.child.connected) return
		this.child.send({
			direction: 'response',
			callbackId,
			success,
			payload: this._ejson.stringify(payload === undefined ? null : payload),
		})
	}

	_onMessage(msg) {
		if (!msg || typeof msg !== 'object') return
		if (msg.__harness) return this._onTelemetry(msg)
		this._lastActivity = Date.now()
		if (msg.direction === 'response') {
			const p = this._pending.get(msg.callbackId)
			if (!p) return
			this._pending.delete(msg.callbackId)
			let data
			try {
				data = msg.payload ? this._ejson.parse(msg.payload) : undefined
			} catch (_e) {
				data = msg.payload
			}
			if (msg.success) p.resolve(data)
			else p.reject(new Error((data && data.message) || JSON.stringify(data)))
			this.emit('activity')
			return
		}
		if (msg.direction !== 'call') return
		let data
		try {
			data = msg.payload ? this._ejson.parse(msg.payload) : undefined
		} catch (_e) {
			data = undefined
		}
		try {
			const res = this._handleModuleCall(msg.name, data)
			if (msg.callbackId) {
				Promise.resolve(res).then(
					(v) => this._reply(msg.callbackId, true, v),
					(e) => this._reply(msg.callbackId, false, { message: e.message }),
				)
			}
		} catch (e) {
			if (msg.callbackId) this._reply(msg.callbackId, false, { message: e.message })
		}
		this.emit('activity')
	}

	_handleModuleCall(name, data) {
		const t = Date.now()
		switch (name) {
			case 'register': {
				if (data.connectionId !== this.connectionId || data.verificationToken !== this.verificationToken)
					throw new Error('bad registration')
				this.apiVersion = data.apiVersion
				this._push('ipc:register', { apiVersion: data.apiVersion })
				if (this._onRegister) setImmediate(this._onRegister)
				return {}
			}
			case 'log-message': {
				const e = { t, level: data.level, message: data.message }
				this.logs.push(e)
				this._push('log', e)
				this.emit('log', e)
				if (this.opts.echoLogs) console.log(`[module ${data.level}] ${data.message}`)
				return
			}
			case 'set-status': {
				this.status = { t, status: data.status, message: data.message }
				this.statusHistory.push(this.status)
				this._push('status', this.status)
				this.emit('status', this.status)
				return
			}
			case 'setActionDefinitions': {
				this.actionDefinitions = data.actions || []
				this._actionMap = new Map(this.actionDefinitions.map((a) => [a.id, a]))
				this._push('ipc:setActionDefinitions', { count: this.actionDefinitions.length })
				return
			}
			case 'setFeedbackDefinitions': {
				this.feedbackDefinitions = data.feedbacks || []
				this._feedbackMap = new Map(this.feedbackDefinitions.map((f) => [f.id, f]))
				this._push('ipc:setFeedbackDefinitions', { count: this.feedbackDefinitions.length })
				return
			}
			case 'setVariableDefinitions': {
				this.variableDefinitions = data.variables || []
				for (const v of data.newValues || []) this._setVar(t, v.id, v.value, true)
				this._push('ipc:setVariableDefinitions', { count: this.variableDefinitions.length })
				return
			}
			case 'setVariableValues': {
				for (const v of data.newValues || []) this._setVar(t, v.id, v.value, false)
				this.emit('variables', data.newValues)
				return
			}
			case 'setPresetDefinitions':
				this.presetDefinitions = data.presets || []
				return
			case 'updateFeedbackValues':
				for (const v of data.values || []) this.feedbackValues[v.id] = v.value
				this._push('ipc:updateFeedbackValues', { values: data.values })
				return
			case 'saveConfig':
				this.savedConfigs.push({ t, ...data })
				this._push('ipc:saveConfig', data)
				return
			case 'setCustomVariable':
				this.customVariables.push({ t, ...data })
				return
			case 'parseVariablesInString': {
				const variableIds = []
				const text = String(data.text).replace(/\$\(([^:)]+):([^)]+)\)/g, (m, label, id) => {
					variableIds.push(`${label}:${id}`)
					if (label === this.label && id in this.variables) return String(this.variables[id])
					return '$NA'
				})
				return { text, variableIds }
			}
			case 'sharedUdpSocketJoin':
			case 'sharedUdpSocketLeave':
			case 'sharedUdpSocketSend':
				throw new Error('shared UDP sockets are not emulated by the harness')
			default:
				this._push('ipc:unhandled', { name, data })
				return
		}
	}

	_setVar(t, id, value, fromDefinition) {
		if (value === undefined) delete this.variables[id]
		else this.variables[id] = value
		if (!fromDefinition) this.variableChanges.push({ t, id, value })
	}

	// ------------------------------------------------------------------ telemetry from preload
	_onTelemetry(m) {
		this._lastActivity = Date.now()
		const t = m.t
		switch (m.type) {
			case 'http:start': {
				let wireUrl = m.url
				let pathname = null
				let search = ''
				try {
					const u = new URL(m.url)
					wireUrl = u.href
					pathname = u.pathname
					search = u.search
				} catch (_e) {
					// keep raw
				}
				const rawSearch = (() => {
					const i = m.url.indexOf('?')
					return i >= 0 ? m.url.slice(i + 1) : ''
				})()
				const ctype = (m.headers || []).find((h) => String(h[0]).toLowerCase() === 'content-type')
				const e = {
					id: m.id,
					index: this.http.length,
					t,
					method: m.method,
					url: m.url, // exactly what the module passed to fetch()
					wireUrl, // after WHATWG URL normalization (what undici puts on the wire)
					path: pathname,
					search,
					query: parsePairs(rawSearch),
					headers: m.headers,
					body: m.body,
					form:
						m.body && ctype && /x-www-form-urlencoded/i.test(ctype[1])
							? parsePairs(m.body)
							: undefined,
					status: null,
					statusText: null,
					responseHeaders: null,
					responseText: null,
					responseBytes: null,
					headersMs: null,
					ms: null,
					error: null,
					done: false,
				}
				this.http.push(e)
				this._httpById.set(m.id, e)
				this._push('http:start', { id: m.id, method: m.method, url: m.url })
				this.emit('http', e)
				break
			}
			case 'http:response': {
				const e = this._httpById.get(m.id)
				if (e) {
					e.status = m.status
					e.statusText = m.statusText
					e.responseHeaders = m.headers
					e.headersMs = m.ms
				}
				break
			}
			case 'http:body': {
				const e = this._httpById.get(m.id)
				if (e) {
					e.responseText = m.text
					e.responseBytes = m.bytes
					e.responseB64 = m.b64
					e.truncated = m.truncated
					e.ms = m.ms
					e.done = true
					this._push('http:done', { id: m.id, status: e.status, bytes: m.bytes, ms: Math.round(m.ms) })
				}
				break
			}
			case 'http:error':
			case 'http:bodyError': {
				const e = this._httpById.get(m.id)
				if (e) {
					if (m.type === 'http:error') e.error = m.error
					else e.bodyError = m.error
					e.ms = m.ms
					e.done = true
					this._push(m.type, { id: m.id, error: m.error, ms: Math.round(m.ms) })
				}
				break
			}
			case 'tcp:new':
			case 'tcp:connectCall':
				this._push(m.type, { conn: m.conn, args: m.args })
				break
			case 'tcp:tx': {
				this.tcpRaw.push({ t, conn: m.conn, dir: 'tx', text: m.text })
				const parts = m.text.split(/\r?\n/)
				const tail = parts.pop()
				for (const line of parts) this._tcpLine({ t, conn: m.conn, dir: 'tx', line, raw: m.text })
				if (tail) this._tcpLine({ t, conn: m.conn, dir: 'tx', line: tail, raw: m.text, unterminated: true })
				break
			}
			case 'tcp:rx': {
				this.tcpRaw.push({ t, conn: m.conn, dir: 'rx', text: m.text, injected: m.injected })
				const buf = (this._rxBuf.get(m.conn) || '') + m.text
				const parts = buf.split(/\r?\n/)
				this._rxBuf.set(m.conn, parts.pop() || '')
				for (const line of parts) this._tcpLine({ t, conn: m.conn, dir: 'rx', line, injected: m.injected || undefined })
				break
			}
			case 'tcp:event': {
				const e = { t, conn: m.conn, dir: 'event', event: m.event }
				if (m.error) e.error = m.error
				if (m.hadError !== undefined) e.hadError = m.hadError
				if (m.local) e.local = m.local
				if (m.remote) e.remote = m.remote
				this._tcpLine(e)
				break
			}
			case 'ctl:reply': {
				const p = this._ctlPending.get(m.id)
				if (p) {
					this._ctlPending.delete(m.id)
					if (m.ok) p.resolve(true)
					else p.reject(new Error(m.error || 'ctl failed'))
				}
				break
			}
			case 'process:uncaughtException':
				this.crashes.push(m)
				this._push(m.type, { origin: m.origin, error: m.error })
				break
			default:
				this._push(m.type, m)
		}
		this.emit('activity')
	}

	_tcpLine(e) {
		this.tcp.push(e)
		this._push('tcp', e)
		this.emit('tcp', e)
	}

	_push(type, data) {
		this.timeline.push({ t: Date.now(), type, ...data })
		this._lastActivity = Date.now()
	}

	_ctlCall(msg) {
		if (!this._ctl || this.exit) return Promise.reject(new Error('module process not running'))
		const id = this._nextCtl++
		return new Promise((resolve, reject) => {
			const timer = setTimeout(() => {
				this._ctlPending.delete(id)
				reject(new Error('ctl timeout'))
			}, 3000)
			this._ctlPending.set(id, {
				resolve: (v) => {
					clearTimeout(timer)
					resolve(v)
				},
				reject: (e) => {
					clearTimeout(timer)
					reject(e)
				},
			})
			this._ctl.write(JSON.stringify({ ...msg, id }) + '\n')
		})
	}

	_lines(stream, arr, name) {
		let buf = ''
		stream.setEncoding('utf8')
		stream.on('data', (d) => {
			buf += d
			let i
			while ((i = buf.indexOf('\n')) >= 0) {
				const line = buf.slice(0, i)
				buf = buf.slice(i + 1)
				arr.push(line)
				this._push(name, { line })
				if (this.opts.echoStdio) console.log(`[module ${name}] ${line}`)
			}
		})
	}

	async _withTimeout(p, ms, what) {
		let timer
		try {
			return await Promise.race([
				p,
				new Promise((_, rej) => {
					timer = setTimeout(() => rej(new Error(`${what} within ${ms} ms`)), ms)
				}),
			])
		} finally {
			clearTimeout(timer)
		}
	}
}

async function createHarness(opts = {}) {
	const h = new Harness(opts)
	try {
		await h.start()
	} catch (e) {
		await h.close().catch(() => {})
		throw e
	}
	return h
}

module.exports = { createHarness, Harness, optionDefaults, parsePairs, DEFAULT_MODULE_DIR }
