'use strict'
// Minimal vMix TCP API (port 8099) for the tally path: SUBSCRIBE TALLY -> "SUBSCRIBE OK TALLY" + "TALLY OK <state>".
// Also answers the TALLY request with "TALLY OK <state>", as vMix does.
// Fault injection, all off by default (existing runs behave as before):
//   freeze(true)   stop answering and sending on every connection (new ones too). The TCP
//                  connections stay open and the kernel keeps ACKing: to the firmware this looks
//                  like a vMix that hung, or a PC that died without FIN/RST.
//   setTallySplit(state, at, gapMs)  send one TALLY line as two TCP segments, gapMs apart.
//   opts.version   send "VERSION OK <v>" on connect, like the real vMix.
const net = require('net')
class FakeVmix {
	constructor(host, port = 8099, opts = {}) {
		this.host = host; this.port = port; this.state = '0000000000'; this.clients = new Set(); this.events = []; this.server = null
		this.frozen = false; this.opts = opts || {}
	}
	log(ev) { this.events.push({ wt: Date.now(), ...ev }) }
	start() {
		return new Promise((res, rej) => {
			if (this.server) return res()
			const srv = net.createServer((s) => {
				this.clients.add(s); this.log({ ev: 'connect', peer: s.remoteAddress + ':' + s.remotePort, frozen: this.frozen })
				if (this.opts.version && !this.frozen) s.write('VERSION OK ' + this.opts.version + '\r\n')
				let buf = ''
				s.on('data', (d) => {
					buf += d.toString('latin1')
					let i
					while ((i = buf.indexOf('\n')) >= 0) {
						const line = buf.slice(0, i).replace(/\r$/, ''); buf = buf.slice(i + 1)
						this.log({ ev: 'rx', line, frozen: this.frozen })
						if (this.frozen) continue
						if (/^SUBSCRIBE TALLY/i.test(line)) { s.write('SUBSCRIBE OK TALLY\r\n'); s.write('TALLY OK ' + this.state + '\r\n') }
						else if (/^TALLY\s*$/i.test(line)) s.write('TALLY OK ' + this.state + '\r\n')
					}
				})
				s.on('close', () => { this.clients.delete(s); this.log({ ev: 'close' }) })
				s.on('error', (e) => this.log({ ev: 'error', code: e.code }))
			})
			srv.on('error', rej)
			srv.listen(this.port, this.host, () => { this.server = srv; this.log({ ev: 'listen' }); res() })
		})
	}
	stop() {
		return new Promise((res) => {
			if (!this.server) return res()
			for (const c of this.clients) c.destroy()
			this.server.close(() => res()); this.server = null; this.log({ ev: 'stop' })
		})
	}
	freeze(on = true) { this.frozen = !!on; this.log({ ev: 'freeze', on: this.frozen, clients: this.clients.size }) }
	setTally(state) {
		this.state = state
		if (!this.frozen) for (const c of this.clients) c.write('TALLY OK ' + state + '\r\n')
		this.log({ ev: 'tally', state, clients: this.clients.size, frozen: this.frozen })
	}
	sendRaw(text) {
		for (const c of this.clients) c.write(text)
		this.log({ ev: 'raw', text, clients: this.clients.size })
	}
	setTallySplit(state, at, gapMs = 100) {
		this.state = state
		const line = 'TALLY OK ' + state + '\r\n'
		const a = line.slice(0, at), b = line.slice(at)
		for (const c of this.clients) { c.setNoDelay(true); c.write(a); setTimeout(() => c.write(b), gapMs) }
		this.log({ ev: 'tally-split', state, at, gapMs, clients: this.clients.size })
	}
}
module.exports = { FakeVmix }
