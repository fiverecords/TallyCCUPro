'use strict'
// Harness preload: runs inside the forked module process (node --require preload.js main.js)
// BEFORE the module code. It does NOT modify the module or @companion-module/base:
//  - wraps globalThis.fetch to report every request/response to the parent (host)
//  - returns a recording subclass of net.Socket only to `require('net')` calls made from the
//    module's own src/ files (tcp.js), so every line written/received on 8098 is reported
// Telemetry goes to the parent via process.send({__harness: 1, ...}) on the same IPC channel the
// real Companion host uses, so it stays ordered with the module's own IPC messages (log-message,
// executeAction responses...). Parent->child control commands arrive on an extra pipe (fd 4) so the
// module's IPC channel only ever carries genuine Companion host messages.

const Module = require('module')
const path = require('path')
const fs = require('fs')
const net = require('net')
const { performance } = require('perf_hooks')

const MODULE_DIR = fs.realpathSync(process.env.HARNESS_MODULE_DIR || process.cwd())
const MODULE_SRC = path.join(MODULE_DIR, 'src') + path.sep
const CAPTURE_BODIES = process.env.HARNESS_CAPTURE_BODIES !== '0'
const BODY_LIMIT = parseInt(process.env.HARNESS_BODY_LIMIT || '262144', 10)

// The callback matters: without it a failed send (e.g. a late socket 'close' while the host is
// disconnecting the channel) is emitted as an 'error' event on `process` and kills the module.
const ignoreSendError = () => {}
function report(type, data) {
	try {
		if (process.send && process.connected) {
			process.send({ __harness: 1, type, t: Date.now(), hr: performance.now(), ...data }, undefined, undefined, ignoreSendError)
		}
	} catch (_e) {
		// parent gone
	}
}

function serializeErr(err) {
	if (!err) return null
	return {
		name: err.name,
		message: err.message,
		code: err.code || (err.cause && err.cause.code) || undefined,
		cause: err.cause ? { name: err.cause.name, message: err.cause.message, code: err.cause.code } : undefined,
	}
}

function headersToArray(h) {
	if (!h) return []
	try {
		if (typeof h.entries === 'function') return [...h.entries()]
		if (Array.isArray(h)) return h.map((x) => [String(x[0]), String(x[1])])
		return Object.entries(h).map(([k, v]) => [k, String(v)])
	} catch (_e) {
		return []
	}
}

function bodyToString(body) {
	if (body === undefined || body === null) return null
	if (typeof body === 'string') return body
	if (body instanceof URLSearchParams) return body.toString()
	if (Buffer.isBuffer(body)) return body.toString('utf8')
	if (body instanceof ArrayBuffer) return Buffer.from(body).toString('utf8')
	if (ArrayBuffer.isView(body)) return Buffer.from(body.buffer, body.byteOffset, body.byteLength).toString('utf8')
	return '[' + Object.prototype.toString.call(body) + ']'
}

// ---------------------------------------------------------------- fetch
const origFetch = globalThis.fetch
let httpSeq = 0
globalThis.fetch = async function harnessFetch(input, init) {
	const id = ++httpSeq
	const t0 = performance.now()
	let url
	let method = 'GET'
	if (typeof input === 'string') url = input
	else if (input instanceof URL) url = input.href
	else if (input && typeof input === 'object' && 'url' in input) {
		url = input.url
		method = input.method || method
	} else url = String(input)
	if (init && init.method) method = String(init.method).toUpperCase()
	report('http:start', {
		id,
		method,
		url,
		headers: headersToArray(init && init.headers),
		body: bodyToString(init && init.body),
	})
	let res
	try {
		res = await origFetch(input, init)
	} catch (err) {
		report('http:error', { id, ms: performance.now() - t0, error: serializeErr(err) })
		throw err
	}
	report('http:response', {
		id,
		ms: performance.now() - t0,
		status: res.status,
		statusText: res.statusText,
		headers: headersToArray(res.headers),
	})
	if (CAPTURE_BODIES) {
		let clone
		try {
			clone = res.clone()
		} catch (err) {
			report('http:bodyError', { id, ms: performance.now() - t0, error: serializeErr(err) })
		}
		if (clone) {
			clone.arrayBuffer().then(
				(buf) => {
					const b = Buffer.from(buf)
					report('http:body', {
						id,
						ms: performance.now() - t0,
						bytes: b.length,
						text: b.subarray(0, BODY_LIMIT).toString('utf8'),
						truncated: b.length > BODY_LIMIT,
						b64: b.length <= 4096 ? b.toString('base64') : undefined,
					})
				},
				(err) => report('http:bodyError', { id, ms: performance.now() - t0, error: serializeErr(err) }),
			)
		}
	} else {
		report('http:body', { id, ms: performance.now() - t0, bytes: null, text: null, notCaptured: true })
	}
	return res
}

// ---------------------------------------------------------------- net (only for the module's src/)
const RealSocket = net.Socket
let connSeq = 0
let currentSocket = null
let injecting = false

class HarnessSocket extends RealSocket {
	constructor(...args) {
		super(...args)
		this.__harnessConn = ++connSeq
		currentSocket = this
		report('tcp:new', { conn: this.__harnessConn })
	}
	connect(...args) {
		const plain = args.filter((a) => typeof a !== 'function')
		report('tcp:connectCall', { conn: this.__harnessConn, args: plain.map((a) => (typeof a === 'object' ? { ...a } : a)) })
		return super.connect(...args)
	}
	emit(ev, ...args) {
		if (ev === 'data') {
			const chunk = args[0]
			report('tcp:rx', {
				conn: this.__harnessConn,
				text: Buffer.isBuffer(chunk) ? chunk.toString('utf8') : String(chunk),
				injected: injecting,
			})
		} else if (ev === 'connect' || ev === 'close' || ev === 'error' || ev === 'timeout' || ev === 'end') {
			report('tcp:event', {
				conn: this.__harnessConn,
				event: ev,
				error: ev === 'error' ? serializeErr(args[0]) : undefined,
				hadError: ev === 'close' ? !!args[0] : undefined,
				local: ev === 'connect' ? { address: this.localAddress, port: this.localPort } : undefined,
				remote: ev === 'connect' ? { address: this.remoteAddress, port: this.remotePort } : undefined,
			})
		}
		return super.emit(ev, ...args)
	}
}

// write() must be intercepted with an accessor: Socket.prototype.connect() does
// `if (this.write !== Socket.prototype.write) this.write = Socket.prototype.write` (and end() may
// assign writeAfterFIN), which would shadow a plain subclass method with an own property. The
// setter keeps whatever Node assigns and the getter always returns the recording wrapper.
function harnessWrite(data, encoding, cb) {
	const text = Buffer.isBuffer(data)
		? data.toString('utf8')
		: data instanceof Uint8Array
			? Buffer.from(data).toString('utf8')
			: String(data)
	report('tcp:tx', { conn: this.__harnessConn, text, destroyed: this.destroyed, writable: this.writable })
	const real = this.__harnessRealWrite || RealSocket.prototype.write
	return real.call(this, data, encoding, cb)
}
Object.defineProperty(HarnessSocket.prototype, 'write', {
	configurable: true,
	enumerable: false,
	get() {
		return harnessWrite
	},
	set(v) {
		this.__harnessRealWrite = v
	},
})

function wrapNet(realNet) {
	const proxy = Object.create(realNet)
	Object.defineProperty(proxy, 'Socket', { value: HarnessSocket, enumerable: true })
	Object.defineProperty(proxy, 'Stream', { value: HarnessSocket, enumerable: true })
	const connect = (...args) => {
		const normalized = realNet._normalizeArgs ? realNet._normalizeArgs(args) : args
		const opts = normalized[0] || {}
		const s = new HarnessSocket(opts)
		if (opts.timeout) s.setTimeout(opts.timeout)
		return s.connect(...normalized)
	}
	Object.defineProperty(proxy, 'connect', { value: connect, enumerable: true })
	Object.defineProperty(proxy, 'createConnection', { value: connect, enumerable: true })
	return proxy
}
const netProxy = wrapNet(net)

const origLoad = Module._load
Module._load = function harnessLoad(request, parent, isMain) {
	const exp = origLoad.apply(this, arguments)
	if ((request === 'net' || request === 'node:net') && parent && parent.filename) {
		let fname = parent.filename
		try {
			fname = fs.realpathSync(fname)
		} catch (_e) {
			// keep
		}
		if (fname.startsWith(MODULE_SRC)) return netProxy
	}
	return exp
}

// ---------------------------------------------------------------- control channel (fd 4)
function handleCtl(msg) {
	switch (msg.cmd) {
		case 'injectRx': {
			// Deliver bytes to the module's live socket as if the server had sent them
			const s = currentSocket
			if (!s || s.destroyed) {
				report('ctl:reply', { id: msg.id, ok: false, error: 'no live module socket' })
				return
			}
			injecting = true
			try {
				s.emit('data', Buffer.from(msg.text, 'utf8'))
			} finally {
				injecting = false
			}
			report('ctl:reply', { id: msg.id, ok: true })
			return
		}
		case 'tcpDestroy': {
			const s = currentSocket
			if (s && !s.destroyed) s.destroy()
			report('ctl:reply', { id: msg.id, ok: !!s })
			return
		}
		case 'ping':
			report('ctl:reply', { id: msg.id, ok: true })
			return
		default:
			report('ctl:reply', { id: msg.id, ok: false, error: 'unknown cmd ' + msg.cmd })
	}
}

if (process.env.HARNESS_CTL_FD) {
	try {
		const ctl = new RealSocket({ fd: parseInt(process.env.HARNESS_CTL_FD, 10), readable: true, writable: false })
		ctl.unref()
		let buf = ''
		ctl.on('data', (d) => {
			buf += d.toString('utf8')
			let i
			while ((i = buf.indexOf('\n')) >= 0) {
				const line = buf.slice(0, i)
				buf = buf.slice(i + 1)
				if (!line.trim()) continue
				try {
					handleCtl(JSON.parse(line))
				} catch (e) {
					report('ctl:reply', { id: null, ok: false, error: String(e && e.message) })
				}
			}
		})
		ctl.on('error', () => {})
	} catch (e) {
		report('preload:warn', { message: 'control channel unavailable: ' + e.message })
	}
}

// Observe only: a monitor does not change Node's default crash behaviour (also covers unhandled
// rejections, which Node 22 turns into uncaught exceptions). The stack also lands on stderr.
process.on('uncaughtExceptionMonitor', (err, origin) => {
	report('process:uncaughtException', { origin, error: serializeErr(err), stack: err && err.stack })
})

report('preload:ready', { pid: process.pid, moduleDir: MODULE_DIR, node: process.version })
