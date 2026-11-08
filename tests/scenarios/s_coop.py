"""(coop) cooperative HTTP server: file transfers in the background.
 - 2 KB/s reader of index.html + Companion-style PTZ (speed, then 0,0; no header, no deadman):
   HTTP time and camera write < 100 ms; API requests < 150 ms
 - 2 simultaneous full downloads (gz and raw), byte exact
 - both transfer slots held by slow readers: a third page waits for a slot and gets a whole
   200 within WEB_DEFER_MAX_MS while a socket is free; with the sockets short (two SSE streams)
   it gets 503 + Retry-After after WEB_DEFER_MS
 - stalled reader (zero window): cut, slot freed; slow reader end: FIN, not a hang
 - /?download= headers and body; delete of a file being sent cuts it
usage: s_coop.py [gz]"""
import sys, json, time, os, socket, threading, gzip as gzmod
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
GZ = len(sys.argv) > 1 and sys.argv[1] == 'gz'
H = CSRF
# webserver.h / SdUtils.h
WEB_DEFER_S = 10.0
WEB_DEFER_MAX_S = 30.0 + 3.0 + 2.0   # SD_SEND_MAX_MS + SD_SEND_FIN_MS + 2000
res = {'gz': GZ}
fails = []

def check(name, cond, info=None):
    res[name] = 'ok' if cond else ('FAIL %s' % (info,))
    if not cond: fails.append(name)

def prep(sd):
    if GZ:
        for f in ('index.html', 'tally.html', 'sdcard.html'):
            open(os.path.join(sd, f + '.gz'), 'wb').write(gzmod.compress(open(os.path.join(sd, f), 'rb').read(), 9))
    open(os.path.join(sd, 'big.bin'), 'wb').write(os.urandom(120000))
    open(os.path.join(sd, 'keep.txt'), 'w').write('KEEP')

vm = FakeVmix(); emu = Emu('coop' + ('gz' if GZ else ''), prep=prep)
assert emu.wait_http(); time.sleep(1.5); setup_tally(); time.sleep(1.0)
SD = emu.work + '/sd'
RAW = open(SD + '/index.html', 'rb').read()
GZB = open(SD + '/index.html.gz', 'rb').read() if GZ else None
BIG = open(SD + '/big.bin', 'rb').read()
pushes, stop = tally_pusher(vm, 0.1)

class Reader(threading.Thread):
    """GET path; reads chunk bytes every delay s (None: as fast as possible); stall: never reads."""
    def __init__(self, path, gz=False, delay=None, chunk=512, rcvbuf=None, stall=False, timeout=60):
        super().__init__(daemon=True)
        self.path, self.gz, self.delay, self.chunk, self.rcvbuf, self.stall, self.timeout = path, gz, delay, chunk, rcvbuf, stall, timeout
        self.data = b''; self.end = None; self.t = None; self.t_first = None
    def run(self):
        s = socket.socket()
        if self.rcvbuf: s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, self.rcvbuf)
        s.settimeout(self.timeout); t0 = time.time()
        s.connect((IP, 80)); self.port = s.getsockname()[1]
        req = 'GET %s HTTP/1.1\r\nHost: t\r\n%s\r\n' % (self.path, 'Accept-Encoding: gzip, deflate\r\n' if self.gz else '')
        s.sendall(req.encode())
        if self.stall:
            time.sleep(self.timeout); s.close(); self.end = 'stalled'; self.t = time.time() - t0; return
        while True:
            try: d = s.recv(self.chunk)
            except socket.timeout: self.end = 'timeout'; break
            except ConnectionResetError: self.end = 'reset'; break
            if not d: self.end = 'FIN'; break
            if self.t_first is None: self.t_first = time.time() - t0
            self.data += d
            if self.delay: time.sleep(self.delay)
        self.t = time.time() - t0
        s.close()
    def split(self):
        if b'\r\n\r\n' not in self.data: return None, b''
        h, b = self.data.split(b'\r\n\r\n', 1)
        return h.decode('latin1'), b

def close_path(port):
    """Firmware commands on the socket that accepted local port 'port': 'fin' (DISCON first), 'close' or None."""
    ev = [json.loads(l) for l in open(emu.work + '/net.jsonl')]
    sock = None
    for e in ev:
        if sock is None:
            if e.get('ev') == 'accept' and e.get('port') == 80 and e.get('peer', '').endswith(':%d' % port): sock = e['s']
            continue
        if e.get('s') != sock: continue
        if e.get('ev') == 'cmd' and e.get('cmd') in (8, 16): return 'fin' if e['cmd'] == 8 else 'close'
        if e.get('ev') == 'sr' and e.get('to') == 0: return 'peer:' + e.get('why', '')
    return None

def ptz(val, cam=1):
    t0 = time.time()
    r = http('/?cameraId=%d&pan_tilt_velocity=%s' % (cam, val), timeout=15)
    return t0, r

def ptz_latency(t0, cam, want):
    for b in emu.bmd():
        if b.get('group') == 11 and b.get('cam') == cam and b['wt'] / 1000.0 >= t0 - 0.002:
            v = [x['f'] for x in b['v']][:2]
            if all(abs(a - w) < 0.01 for a, w in zip(v, want)):
                return round(b['wt'] / 1000.0 - t0, 3)
    return None

# 1) 2 KB/s reader + PTZ requests (Companion style: no header, speed then 0,0)
slow = Reader('/', gz=True, delay=0.25, chunk=512, rcvbuf=2048, timeout=45); slow.start()
time.sleep(1.0)
samples = []
for i in range(10):
    val, want = ('0.3,0.3', [0.3, 0.3]) if i % 2 == 0 else ('0,0', [0, 0])
    t0, r = ptz(val)
    samples.append((val, r['status'], round(r['t'] * 1000), t0, want))
    time.sleep(0.3)
time.sleep(0.3)
ptzres = []
for val, st, ms, t0, want in samples:
    ptzres.append({'val': val, 'status': st, 'http_ms': ms, 'bmd_ms': (lambda l: None if l is None else round(l * 1000))(ptz_latency(t0, 1, want))})
res['ptz_during_slow'] = ptzres
stops = [p for p in ptzres if p['val'] == '0,0']
check('ptz stop http < 100 ms', all(p['status'] == 200 and p['http_ms'] < 100 for p in stops), stops)
check('ptz stop at camera < 100 ms', all(p['bmd_ms'] is not None and p['bmd_ms'] < 100 for p in stops), stops)
check('ptz move < 100 ms', all(p['status'] == 200 and p['http_ms'] < 100 and p['bmd_ms'] is not None and p['bmd_ms'] < 100 for p in ptzres), ptzres)

# other requests meanwhile
lat = []
for path in ('/?listPresets', '/?getOverrides', '/?getTallyMap', '/?listFiles', '/safemode-status'):
    r = http(path, timeout=15); lat.append((path, r['status'], round(r['t'] * 1000)))
res['api_during_slow'] = lat
check('api during slow < 150 ms', all(s == 200 and ms < 150 for _, s, ms in lat), lat)

# 2) two simultaneous full-speed page loads while the slow one still runs?
#    (slots: slow + 1 free) -> second waits; do it after the slow one ends instead
slow.join(60)
h, body = slow.split()
res['slow'] = {'bytes': len(body), 'of': len(GZB if GZ else RAW), 's': round(slow.t, 2), 'end': slow.end,
               'first_byte_s': slow.t_first, 'fw_close': close_path(slow.port)}
# the client may still see a reset: the emulator's host socket buffers ~7 KB more than a
# W5500 and Linux aborts its zero-window persist after TCP_USER_TIMEOUT
check('slow reader: firmware ends it with FIN', res['slow']['fw_close'] == 'fin', res['slow'])
want = GZB if GZ else RAW
check('slow reader bytes are a prefix', want.startswith(body), len(body))

a = Reader('/', gz=True); b = Reader('/index.html', gz=False)
a.start(); b.start(); a.join(30); b.join(30)
ha, ba = a.split(); hb, bb = b.split()
res['two_fast'] = {'a_s': round(a.t, 2), 'b_s': round(b.t, 2), 'a_end': a.end, 'b_end': b.end,
                   'a_head': ha, 'b_head': hb}
check('fast gz-capable load exact', ba == (GZB if GZ else RAW) and a.end == 'FIN', (len(ba), a.end))
check('fast raw load exact', bb == RAW and b.end == 'FIN', (len(bb), b.end))
if GZ: check('gz header', 'Content-Encoding: gzip' in ha and 'Content-Length: %d' % len(GZB) in ha, ha)
check('raw header', 'Content-Encoding' not in hb and 'Content-Length: %d' % len(RAW) in hb, hb)

a = Reader('/', gz=GZ); b = Reader('/', gz=GZ)
a.start(); b.start(); a.join(30); b.join(30)
check('two identical loads exact', a.split()[1] == want and b.split()[1] == want, (len(a.split()[1]), len(b.split()[1])))

# 3) both slots held by slow readers, a socket still free: a third page waits for the next
#    slot (the readers are cut after SD_SEND_BASE_MS + size at 8 KB/s) and comes whole;
#    the API and the PTZ stop are answered meanwhile
TALLY = open(SD + '/tally.html', 'rb').read()
TALLY_GZ = open(SD + '/tally.html.gz', 'rb').read() if GZ else None
s1 = Reader('/big.bin', delay=0.25, chunk=512, rcvbuf=2048, timeout=60)
s2 = Reader('/big.bin', delay=0.25, chunk=512, rcvbuf=2048, timeout=60)
s1.start(); s2.start(); time.sleep(1.0)
third = Reader('/tally.html', gz=GZ, timeout=WEB_DEFER_MAX_S + 10); third.start()
time.sleep(0.5)
t0, r = ptz('0,0')
res['ptz_while_deferred'] = {'status': r['status'], 'http_ms': round(r['t'] * 1000), 'bmd_ms': ptz_latency(t0, 1, [0, 0])}
check('ptz while a page is deferred', r['status'] == 200 and r['t'] < 0.1, res['ptz_while_deferred'])
r = http('/?listPresets', timeout=15)
check('api while a page is deferred', r['status'] == 200 and r['t'] < 0.15, (r['status'], round(r['t'] * 1000)))
third.join(WEB_DEFER_MAX_S + 15)
h3, b3 = third.split()
res['third'] = {'s': round(third.t or -1, 2), 'end': third.end, 'status': h3.split('\r\n')[0] if h3 else None, 'bytes': len(b3)}
check('third page: waits for a slot, then a whole 200 before WEB_DEFER_MAX_MS',
      h3 and ' 200 ' in h3.split('\r\n')[0] + ' ' and b3 == (TALLY_GZ if GZ else TALLY) and third.end == 'FIN'
      and 3 < (third.t or 0) < WEB_DEFER_MAX_S, res['third'])
s1.join(70); s2.join(70)
res['big_slow'] = [(len(x.split()[1]), x.end, round(x.t, 1)) for x in (s1, s2)]
res['big_slow_fw'] = [close_path(x.port) for x in (s1, s2)]
check('big slow readers cut with FIN', all(c == 'fin' for c in res['big_slow_fw']), (res['big_slow'], res['big_slow_fw']))
check('big slow bytes prefix', all(BIG.startswith(x.split()[1]) for x in (s1, s2)))

# 3b) the same with the sockets short (two SSE streams open): the waiting page gets
#     503 + Retry-After after WEB_DEFER_MS, so vMix can always reconnect
e1 = Sse(); e2 = Sse(); time.sleep(0.5)
s3 = Reader('/big.bin', delay=0.25, chunk=512, rcvbuf=2048, timeout=60)
s4 = Reader('/big.bin', delay=0.25, chunk=512, rcvbuf=2048, timeout=60)
s3.start(); s4.start(); time.sleep(1.0)
fourth = Reader('/tally.html', gz=GZ, timeout=30); fourth.start()
fourth.join(35)
h4, b4 = fourth.split()
res['short'] = {'s': round(fourth.t or -1, 2), 'end': fourth.end, 'head': h4, 'body': b4.decode('latin1')}
check('sockets short: 503 + Retry-After after WEB_DEFER_MS',
      h4 and ' 503 ' in h4.split('\r\n')[0] + ' ' and 'Retry-After: 2' in h4 and WEB_DEFER_S - 1 < (fourth.t or 0) < WEB_DEFER_S + 2.5,
      res['short'])
tev = [l for l in e1.text().split('\n') if '"type":"tally"' in l]
check('SSE tally events during the transfers', len(tev) >= 10, len(tev))
check('SSE streams kept open', not e1.closed and not e2.closed, (e1.closed, e2.closed))
e1.close(); e2.close()
s3.join(70); s4.join(70)
check('slow readers with the sockets short cut with FIN', all(close_path(x.port) == 'fin' for x in (s3, s4)),
      [(len(x.split()[1]), x.end, round(x.t, 1)) for x in (s3, s4)])
time.sleep(1.0)

# 4) stalled reader: zero window, never reads
st = Reader('/big.bin', rcvbuf=1024, stall=True, timeout=12); st.start()
time.sleep(0.5)
t_st = time.time()
# a page request right away is served (one slot still free)
x = Reader('/tally.html', gz=GZ); x.start(); x.join(20)
check('page while one slot stalled', x.end == 'FIN' and len(x.split()[1]) > 1000, (x.end, len(x.split()[1])))
st.join(20)
res['stalled_fw'] = close_path(st.port)
check('stalled reader closed by firmware', res['stalled_fw'] in ('fin', 'close'), res['stalled_fw'])
cuts = [l for l in emu.serial().splitlines() if 'Transfer cut' in l]
res['serial_cuts'] = cuts[-6:]
# after the stall cut the slots are free again: two fast loads at once
a = Reader('/', gz=GZ); b = Reader('/big.bin'); a.start(); b.start(); a.join(30); b.join(30)
check('slots free after stall cut', a.split()[1] == want and b.split()[1] == BIG, (len(a.split()[1]), len(b.split()[1])))

# 5) download headers and body
r = http('/?download=keep.txt')
res['download_head'] = r['raw'].split(b'\r\n\r\n')[0].decode('latin1')
check('download', r['status'] == 200 and r['body'] == b'KEEP' and 'Content-Disposition: attachment; filename="keep.txt"' in res['download_head']
      and 'application/octet-stream' in res['download_head'], res['download_head'])
r = http('/?download=nothere.txt'); check('download missing 404', r['status'] == 404, r['status'])
r = http('/?download=big.bin'); check('download big exact', r['body'] == BIG, len(r['body']))

# 6) delete while being sent: transfer cut, file gone
d = Reader('/big.bin', delay=0.25, chunk=512, rcvbuf=2048, timeout=30); d.start(); time.sleep(1.0)
r = http('/?deleteFile=big.bin', headers=H)
d.join(40)
res['delete_during'] = {'del': r['body'].decode('latin1', 'replace').strip(), 'reader_end': d.end, 'bytes': len(d.split()[1]), 's': round(d.t, 1)}
check('delete during transfer cuts it', b'"success":true' in r['body'] and d.end == 'FIN' and d.t < 10, res['delete_during'])

stop.set(); time.sleep(0.3)
res['tally'] = tally_latency(emu, pushes)
check('tally bounded', res['tally']['missing'] == 0 and res['tally']['max'] is not None and res['tally']['max'] < 0.15, res['tally'])
res['wdt'] = emu.wdt(); res['alive'] = emu.alive()
check('no WDT', not res['wdt'] and res['alive'])
res['fails'] = fails
emu.stop(); vm.close()
print(json.dumps(res, indent=1))
