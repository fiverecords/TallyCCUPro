'use strict'
// Trivial TallyCCU Pro mock (HTTP :80 + TCP :8098) used ONLY to verify that the harness
// sends and records traffic. It is NOT a firmware emulator: responses are plausible shapes
// taken from the module's parsing code, nothing more.
//
//   const mock = await startMock({ host: '127.0.0.9' })
//   mock.http / mock.tcp   -> what the mock received
//   mock.push('CCU 1 gain_db 4')  -> line to every subscribed TCP client
//   await mock.close()
//
// CLI: node mock-server.js [host] [httpPort] [tcpPort]

const http = require('http')
const net = require('net')

function startMock({ host = '127.0.0.9', httpPort = 80, tcpPort = 8098, echoCcu = true, log = false } = {}) {
	const state = {
		http: [],
		tcp: [],
		presets: new Map(), // "cam,preset" -> {cameraId, presetId, name, data}
		vmixConnect: 0,
		clients: new Set(),
		sse: new Set(),
	}

	const say = (...a) => log && console.log('[mock]', ...a)

	function sendLine(sock, line) {
		state.tcp.push({ t: Date.now(), dir: 'tx', line, client: sock.__id })
		sock.write(line + '\r\n')
	}
	function push(line) {
		let n = 0
		for (const c of state.clients) {
			if (c.__subscribed && !c.destroyed) {
				sendLine(c, line)
				n++
			}
		}
		return n
	}
	function sseSend(obj) {
		for (const res of state.sse) res.write('data: ' + JSON.stringify(obj) + '\n\n')
	}

	// ---------------------------------------------------------------- TCP 8098
	let clientSeq = 0
	const tcpServer = net.createServer((sock) => {
		sock.__id = ++clientSeq
		sock.__subscribed = false
		state.clients.add(sock)
		state.tcp.push({ t: Date.now(), dir: 'event', event: 'connect', client: sock.__id })
		let buf = ''
		sock.on('data', (d) => {
			buf += d.toString('utf8')
			const parts = buf.split(/\r?\n/)
			buf = parts.pop()
			for (const raw of parts) {
				const line = raw.trim()
				if (!line) continue
				state.tcp.push({ t: Date.now(), dir: 'rx', line, client: sock.__id })
				say('tcp rx', line)
				if (line.startsWith('SUBSCRIBE')) {
					sock.__subscribed = true
					sendLine(sock, 'SUBSCRIBED OK')
				} else if (line.startsWith('UNSUBSCRIBE')) {
					sock.__subscribed = false
					sendLine(sock, 'UNSUBSCRIBED OK')
				} else if (line === 'PING') {
					sendLine(sock, 'PONG')
				} else if (line.startsWith('CCUSYNC ')) {
					const m = line.match(/^CCUSYNC (\d+) (\S+) (.*)$/)
					if (m) sseSend({ cameraId: +m[1], param: m[2], value: m[3] })
				} else {
					sendLine(sock, 'ERROR Unknown command')
				}
			}
		})
		const gone = () => {
			state.clients.delete(sock)
			state.tcp.push({ t: Date.now(), dir: 'event', event: 'close', client: sock.__id })
		}
		sock.on('close', gone)
		sock.on('error', () => {})
	})

	// ---------------------------------------------------------------- HTTP 80
	function reply(res, status, body, type = 'text/plain') {
		res.writeHead(status, { 'Content-Type': type, Connection: 'close', 'Content-Length': Buffer.byteLength(body) })
		res.end(body)
	}

	function pairs(str) {
		return String(str || '')
			.split('&')
			.filter(Boolean)
			.map((p) => {
				const i = p.indexOf('=')
				const k = i >= 0 ? p.slice(0, i) : p
				const v = i >= 0 ? p.slice(i + 1) : null
				const dec = (s) => {
					try {
						return decodeURIComponent(s)
					} catch (_e) {
						return s
					}
				}
				return [dec(k), v === null ? null : dec(v)]
			})
	}

	const httpServer = http.createServer((req, res) => {
		let body = ''
		req.setEncoding('utf8')
		req.on('data', (d) => (body += d))
		req.on('end', () => {
			const qi = req.url.indexOf('?')
			const pathname = qi >= 0 ? req.url.slice(0, qi) : req.url
			const query = qi >= 0 ? req.url.slice(qi + 1) : ''
			const entry = { t: Date.now(), method: req.method, url: req.url, path: pathname, query, body: body || null, headers: req.headers }
			state.http.push(entry)
			say('http', req.method, req.url, body ? body.length + 'B' : '')

			if (req.method === 'GET' && pathname === '/events') {
				res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', Connection: 'keep-alive' })
				res.write('event: connected\ndata: {"status":"ok"}\n\n')
				state.sse.add(res)
				// not req 'close': it fires as soon as the GET request has been read
				res.on('close', () => state.sse.delete(res))
				push('REQUESTSYNC')
				return
			}

			if (req.method === 'POST' && pathname === '/savePreset') {
				const f = Object.fromEntries(pairs(body))
				const cameraId = parseInt(f.cameraId, 10)
				const presetId = parseInt(f.presetId, 10)
				if (!(cameraId >= 1 && cameraId <= 8) || !(presetId >= 0 && presetId <= 4)) {
					return reply(res, 400, JSON.stringify({ success: false, error: 'bad ids' }), 'application/json')
				}
				const data = {}
				for (const kv of String(f.data || '').split(';')) {
					if (!kv) continue
					const i = kv.indexOf(':')
					data[kv.slice(0, i)] = kv.slice(i + 1)
				}
				state.presets.set(cameraId + ',' + presetId, { cameraId, presetId, name: f.name || '', data })
				push(`PRESETSAVED ${cameraId} ${presetId} ${f.name || ''}`)
				return reply(res, 200, JSON.stringify({ success: true, params: Object.keys(data).length }), 'application/json')
			}

			if (req.method === 'GET' && pathname === '/') {
				const q = pairs(query)
				const first = q[0] ? q[0][0] : null
				if (first === 'listPresets') {
					const presets = [...state.presets.values()].map((p) => ({ cameraId: p.cameraId, presetId: p.presetId, name: p.name }))
					return reply(res, 200, JSON.stringify({ presets }), 'application/json')
				}
				if (first === 'loadPreset') {
					// Like both firmwares: values come back only with &returnValues= (the module never
					// asks), otherwise the new state reaches the module through the CCU broadcasts.
					const [c, p] = String(q[0][1] || '').split(',').map((x) => parseInt(x, 10))
					const pr = state.presets.get(c + ',' + p)
					if (!pr) return reply(res, 404, JSON.stringify({ success: false, message: 'Preset not found' }), 'application/json')
					for (const [k, v] of Object.entries(pr.data)) if (echoCcu) push(`CCU ${c} ${k} ${v}`)
					push(`PRESET ${c} ${p} ${pr.name}`)
					if (q.some(([k]) => k === 'returnValues')) {
						const parameters = {}
						for (const [k, v] of Object.entries(pr.data)) parameters[k] = v.includes(',') || isNaN(+v) ? v : +v
						return reply(res, 200, JSON.stringify({ success: true, parameters }), 'application/json')
					}
					return reply(res, 200, JSON.stringify({ success: true, message: 'OK' }), 'application/json')
				}
				if (first === 'vmixConnect') {
					state.vmixConnect = parseInt(q[0][1], 10) ? 1 : 0
					return reply(res, 200, 'OK')
				}
				if (first === 'cameraId') {
					const cam = parseInt(q[0][1], 10)
					for (const [k, v] of q.slice(1)) {
						if (echoCcu && k !== 'cameraId') push(`CCU ${cam} ${k} ${v}`)
					}
					return reply(res, 200, 'OK')
				}
			}
			reply(res, 404, 'Not Found')
		})
	})

	return new Promise((resolve, reject) => {
		httpServer.once('error', reject)
		tcpServer.once('error', reject)
		httpServer.listen(httpPort, host, () => {
			tcpServer.listen(tcpPort, host, () => {
				const api = {
					host,
					httpPort,
					tcpPort,
					state,
					get http() {
						return state.http
					},
					get tcp() {
						return state.tcp
					},
					push,
					reset() {
						state.http.length = 0
						state.tcp.length = 0
					},
					close() {
						for (const c of state.clients) c.destroy()
						for (const r of state.sse) r.destroy()
						if (httpServer.closeAllConnections) httpServer.closeAllConnections()
						return Promise.all([
							new Promise((r) => httpServer.close(() => r())),
							new Promise((r) => tcpServer.close(() => r())),
						])
					},
				}
				resolve(api)
			})
		})
	})
}

module.exports = { startMock }

if (require.main === module) {
	const [host = '127.0.0.9', hp = '80', tp = '8098'] = process.argv.slice(2)
	startMock({ host, httpPort: +hp, tcpPort: +tp, log: true }).then((m) => {
		console.log(`mock listening on http://${host}:${hp} and tcp ${host}:${tp} (Ctrl+C to stop)`)
		const stop = () => m.close().then(() => process.exit(0))
		process.on('SIGINT', stop)
		process.on('SIGTERM', stop)
	})
}
