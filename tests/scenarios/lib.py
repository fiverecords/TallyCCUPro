"""Helpers for the robustness scenarios: emulator process, fake vMix, raw HTTP/SSE/8098
clients and iptables DROP rules (network cuts).

Configuration (environment):
  EMU_BIN        emulator binary            (default tests/out/emu/bin/emu)
  T_IP           firmware IP (ports 80/8098) (default 127.0.80.1)
  T_VMIX         fake vMix IP (port 8099)    (default 127.0.80.11; may equal T_IP)
  T_PEER_PREFIX  source addresses of extra clients, prefix + number (default 127.0.81.)
  T_RUNS         per-scenario work directories (default tests/out/scenarios/runs)
  T_SD_SRC       pages copied to each emulated SD card (default sdcard/)
  T_FW_SRC       firmware sources, for the parameter table (default firmware/TallyCCUPro)
Every address must be in 127.0.0.0/8. Port 80 needs root or
net.ipv4.ip_unprivileged_port_start=0; network cuts need iptables (root).
"""
import atexit, json, os, shutil, signal, socket, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
IP = os.environ.get('T_IP', '127.0.80.1')
VMIX = os.environ.get('T_VMIX', '127.0.80.11')
PEER_PREFIX = os.environ.get('T_PEER_PREFIX', '127.0.81.')
SDSRC = os.environ.get('T_SD_SRC', os.path.join(ROOT, 'sdcard'))
FW_SRC = os.environ.get('T_FW_SRC', os.path.join(ROOT, 'firmware', 'TallyCCUPro'))
BIN = os.environ.get('EMU_BIN', os.path.join(ROOT, 'tests', 'out', 'emu', 'bin', 'emu'))
RUNS = os.environ.get('T_RUNS', os.path.join(ROOT, 'tests', 'out', 'scenarios', 'runs'))
LOOPBACK_NET = '127.0.0.0/8'
# SEC-01: header the web pages send on state-changing requests
CSRF = {'X-TallyCCU': '1'}


def peer(n):
    """Source address for an extra client (another host on the LAN)."""
    return '%s%d' % (PEER_PREFIX, n)


# ------------------------------------------------------------------ iptables
_rules = []


def ipt(rule, add=True):
    args = ['iptables', '-I' if add else '-D', 'OUTPUT'] + rule.split() + ['-j', 'DROP']
    r = subprocess.run(args, check=False, capture_output=True)
    if add:
        if r.returncode != 0:
            raise RuntimeError('iptables failed (root needed): %s' % r.stderr.decode().strip())
        _rules.append(rule)
    elif rule in _rules:
        _rules.remove(rule)


@atexit.register
def _undo_rules():
    for rule in list(_rules):
        ipt(rule, False)


# SIGTERM (run.py timeout) still runs the atexit cleanup
signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))


class Drop:
    """DROP both directions of a TCP port on a host (or of one local port)."""
    def __init__(self, port, host=None):
        h = host or IP
        self.rules = ['-s %s -p tcp --sport %d' % (h, port), '-d %s -p tcp --dport %d' % (h, port)]
        for r in self.rules:
            ipt(r)

    def undo(self):
        for r in self.rules:
            ipt(r, False)


def flush_drops():
    """Removes DROP rules left by an earlier, interrupted run on our addresses."""
    try:
        out = subprocess.run(['iptables', '-S', 'OUTPUT'], capture_output=True, text=True).stdout
    except OSError:
        return
    mine = ('%s/32' % IP, '%s/32' % VMIX)
    for l in out.splitlines():
        if l.startswith('-A OUTPUT') and '-j DROP' in l and any(m in l for m in mine):
            subprocess.run(['iptables', '-D'] + l.split()[1:], capture_output=True)


# ------------------------------------------------------------------ fake vMix
class FakeVmix:
    def __init__(self, ip=VMIX, port=8099):
        self.state = '0000000000'; self.clients = []; self.alive = True; self.events = []
        self.srv = socket.socket(); self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind((ip, port)); self.srv.listen(8)
        self.rx = []
        threading.Thread(target=self._acc, daemon=True).start()

    def _acc(self):
        while self.alive:
            try:
                c, a = self.srv.accept()
            except OSError:
                return
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.clients.append(c); self.events.append((time.time(), 'connect', a[1]))
            threading.Thread(target=self._rx, args=(c,), daemon=True).start()

    def _send(self, c, b):
        try:
            c.sendall(b)
        except OSError:
            pass

    def _rx(self, c):
        buf = b''
        while True:
            try:
                d = c.recv(1024)
            except OSError:
                break
            if not d:
                break
            buf += d
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                l = line.strip().upper(); self.rx.append((time.time(), l))
                if l.startswith(b'SUBSCRIBE TALLY'):
                    self._send(c, b'SUBSCRIBE OK TALLY\r\nTALLY OK ' + self.state.encode() + b'\r\n')
                elif l == b'TALLY':
                    self._send(c, b'TALLY OK ' + self.state.encode() + b'\r\n')
        self.events.append((time.time(), 'closed'))
        if c in self.clients:
            self.clients.remove(c)

    def push(self, state, split=None):
        self.state = state; t = time.time()
        msg = b'TALLY OK ' + state.encode() + b'\r\n'
        for c in list(self.clients):
            if split:
                self._send(c, msg[:split]); time.sleep(0.1); self._send(c, msg[split:])
            else:
                self._send(c, msg)
        self.events.append((t, 'push ' + state))
        return t

    def close(self):
        self.alive = False
        for c in self.clients:
            try:
                c.close()
            except OSError:
                pass
        self.srv.close()


# ------------------------------------------------------------------ emulator
class Emu:
    """One emulated board. serial=True: commands through send() (a FIFO); keep=True reuses the
    work directory of an earlier Emu with the same name (SD, EEPROM, logs): a power cycle."""
    def __init__(self, name, env=None, sd=None, prep=None, eeprom=None, serial=False, keep=False):
        if not os.access(BIN, os.X_OK):
            raise RuntimeError('emulator binary not found: %s (run tests/emulator/build.sh)' % BIN)
        self.work = os.path.join(RUNS, name)
        if not keep:
            if os.path.exists(self.work):
                shutil.rmtree(self.work)
            os.makedirs(self.work + '/sd')
            for f in os.listdir(SDSRC):
                if f.endswith('.html') and os.path.isfile(os.path.join(SDSRC, f)):
                    shutil.copy(os.path.join(SDSRC, f), self.work + '/sd/')
            if prep:
                prep(self.work + '/sd')
            if eeprom:
                shutil.copy(eeprom, self.work + '/eeprom.bin')
        self.wfd = None
        serial_in = 'none'
        if serial:
            serial_in = self.work + '/serial.fifo'
            if not os.path.exists(serial_in):
                os.mkfifo(serial_in)
            self.wfd = os.open(serial_in, os.O_RDWR | os.O_NONBLOCK)
        e = dict(os.environ, EMU_BIND_IP=IP, EMU_SD_DIR=self.work + '/sd', EMU_EEPROM=self.work + '/eeprom.bin',
                 EMU_SERIAL_IN=serial_in, EMU_REMAP_ALL=VMIX, EMU_NET_LOG=self.work + '/net.jsonl',
                 EMU_SD_LOG=self.work + '/sdops.jsonl', EMU_BMD_LOG=self.work + '/bmd.jsonl',
                 EMU_EXIT_AFTER_MS='900000', EMU_MAX_BOOTS='6')
        e.update(env or {})
        mode = 'ab' if keep else 'wb'
        self.so = open(self.work + '/serial.log', mode); self.se = open(self.work + '/emu.log', mode)
        self.p = subprocess.Popen([BIN], env=e, stdout=self.so, stderr=self.se)

    def send(self, line):
        """A serial command (needs serial=True)."""
        os.write(self.wfd, (line + '\n').encode())

    def ext_reset(self):
        """Reset pin: RAM (.noinit) is kept, as with the button or a serial DTR pulse."""
        self.p.send_signal(signal.SIGUSR1)

    def mark(self):
        return len(self.serial())

    def wait(self, pat, since=0, timeout=40):
        """Offset of pat in the serial log after since, or -1 after timeout s."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            i = self.serial().find(pat, since)
            if i >= 0:
                return i
            time.sleep(0.1)
        return -1

    def wait_http(self, timeout=40):
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                s = socket.create_connection((IP, 80), timeout=1); s.close(); return True
            except OSError:
                time.sleep(0.2)
        return False

    def alive(self):
        return self.p.poll() is None

    def stop(self):
        if self.p.poll() is None:
            self.p.send_signal(signal.SIGTERM)
            try:
                self.p.wait(3)
            except subprocess.TimeoutExpired:
                self.p.kill(); self.p.wait()
        self.so.close(); self.se.close()
        if self.wfd is not None:
            os.close(self.wfd)
            self.wfd = None

    def emu_log(self):
        return open(self.work + '/emu.log', 'rb').read().decode('latin1')

    def serial(self):
        return open(self.work + '/serial.log', 'rb').read().decode('latin1')

    def wdt(self):
        return [l for l in self.emu_log().splitlines() if 'WATCHDOG' in l]

    def bmd(self):
        out = []
        try:
            for l in open(self.work + '/bmd.jsonl'):
                try:
                    out.append(json.loads(l))
                except ValueError:
                    pass
        except OSError:
            pass
        return out

    def tallies(self, cam=1):
        return [b for b in self.bmd() if b.get('fn') == 'setCameraTally' and b.get('cam') == cam]


# ------------------------------------------------------------------ clients
def http(path, method='GET', body=None, headers=None, rcvbuf=None, timeout=20, read_delay=None, read_chunk=None,
         raw_req=None, ip=None):
    s = socket.socket()
    if rcvbuf:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.settimeout(timeout); t0 = time.time(); raw = b''; err = None
    try:
        s.connect((ip or IP, 80))
        if raw_req is None:
            h = {'Host': 't'}
            if body is not None:
                h['Content-Length'] = str(len(body))
            h.update(headers or {})
            req = ('%s %s HTTP/1.1\r\n' % (method, path) + ''.join('%s: %s\r\n' % kv for kv in h.items()) + '\r\n').encode('latin1')
            if body is not None:
                req += body if isinstance(body, bytes) else body.encode('latin1')
        else:
            req = raw_req
        s.sendall(req)
        while True:
            try:
                d = s.recv(read_chunk or 65536)
            except socket.timeout:
                err = 'timeout'; break
            except ConnectionResetError:
                err = 'reset'; break
            if not d:
                break
            raw += d
            if read_delay:
                time.sleep(read_delay)
    except OSError as e:
        err = str(e)
    s.close()
    r = {'raw': raw, 't': time.time() - t0, 'err': err, 'status': None, 'body': b'', 'headers': ''}
    if b'\r\n\r\n' in raw:
        head, body = raw.split(b'\r\n\r\n', 1)
        p = head.decode('latin1').split('\r\n')[0].split()
        r['status'] = int(p[1]) if len(p) > 1 and p[1].isdigit() else None
        r['body'] = body
        r['headers'] = head.decode('latin1')
    return r


class Sse:
    def __init__(self, rcvbuf=None, name='sse'):
        self.s = socket.socket()
        if rcvbuf:
            self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.s.settimeout(5); self.s.connect((IP, 80))
        self.port = self.s.getsockname()[1]
        self.s.sendall(('GET /events HTTP/1.1\r\nHost: t\r\nAccept: text/event-stream\r\n\r\n').encode())
        self.buf = b''; self.closed = False; self.reading = True; self.lock = threading.Lock()
        threading.Thread(target=self._rd, daemon=True).start()

    def _rd(self):
        self.s.settimeout(0.05)
        while not self.closed:
            if not self.reading:
                time.sleep(0.02); continue
            try:
                d = self.s.recv(65536)
                if not d:
                    self.closed = True; break
                with self.lock:
                    self.buf += d
            except socket.timeout:
                pass
            except OSError:
                self.closed = True; break

    def text(self):
        with self.lock:
            return self.buf.decode('latin1')

    def close(self):
        self.closed = True
        try:
            self.s.close()
        except OSError:
            pass


class Companion:
    """Raw 8098 client mimicking the module: SUBSCRIBE CCU, answers REQUESTSYNC with a dump."""
    def __init__(self, dump=None, name='comp', src=None):
        self.s = socket.create_connection((IP, 8098), timeout=5, source_address=(src, 0) if src else None)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.port = self.s.getsockname()[1]
        self.lines = []; self.closed = False; self.dump = dump; self.reading = True
        self.s.sendall(b'SUBSCRIBE CCU\n')
        threading.Thread(target=self._rd, daemon=True).start()

    def _rd(self):
        buf = b''; self.s.settimeout(0.05)
        while not self.closed:
            if not self.reading:
                time.sleep(0.02); continue
            try:
                d = self.s.recv(65536)
                if not d:
                    self.closed = True; break
                buf += d
                while b'\n' in buf:
                    l, buf = buf.split(b'\n', 1); l = l.strip().decode('latin1')
                    self.lines.append((time.time(), l))
                    if l == 'REQUESTSYNC' and self.dump:
                        threading.Thread(target=self.send_dump, daemon=True).start()
            except socket.timeout:
                pass
            except OSError:
                self.closed = True; break

    def send_dump(self):
        data = ''.join('CCUSYNC %s\n' % x for x in self.dump).encode()
        try:
            self.s.sendall(data)
        except OSError:
            pass

    def send(self, b):
        self.s.sendall(b)

    def got(self, pat):
        return [(t, l) for (t, l) in self.lines if pat in l]

    def close(self):
        self.closed = True
        try:
            self.s.close()
        except OSError:
            pass


# ------------------------------------------------------------------ tally
def tally_pusher(vm, period=0.1, split=None):
    """Alternates cam1 PGM/PVW. Returns (pushes list, stop event)."""
    pushes = []; stop = threading.Event()

    def run():
        i = 0
        while not stop.is_set():
            st = ['1200000000', '2100000000'][i % 2]; i += 1
            pushes.append((vm.push(st, split), st)); stop.wait(period)
    threading.Thread(target=run, daemon=True).start()
    return pushes, stop


def tally_latency(emu, pushes, cam=1):
    bmd = emu.tallies(cam); lat = []
    for tp, st in pushes:
        want = [1, 0] if st[0] == '1' else [0, 1]
        # first matching write at or after the push
        hit = next((b for b in bmd if b['wt'] / 1000.0 >= tp - 0.003 and list(b.get('v', [])) == want), None)
        lat.append(round(hit['wt'] / 1000.0 - tp, 3) if hit else None)
    ok = [l for l in lat if l is not None]
    return {'n': len(lat), 'missing': lat.count(None), 'max': max(ok) if ok else None,
            'p50': sorted(ok)[len(ok)//2] if ok else None, 'top5': sorted(ok)[-5:]}


def setup_tally():
    """Tally slot 0: vMix input 1 -> camera 1 (a tally map edit: CSRF header)."""
    return http('/?tallyCam0Input=1&cam0ID=1', headers=CSRF)
