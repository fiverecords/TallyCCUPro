"""(coop2) background transfers vs long requests.
 - a 20 KB/s reader of big.bin while a slow upload (8 KB/s, ~6 s) keeps the server busy: not cut, exact
 - upload replacing the file being sent: that transfer is cut (FIN), the new file is served whole
 - SSE client keeps getting events while 2 transfers run
"""
import sys, json, time, os, socket, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
H = CSRF
res = {}; fails = []
def check(name, cond, info=None):
    res[name] = 'ok' if cond else ('FAIL %s' % (info,))
    if not cond: fails.append(name)
def prep(sd):
    open(os.path.join(sd, 'big.bin'), 'wb').write(os.urandom(150000))
    open(os.path.join(sd, 'swap.bin'), 'wb').write(b'A' * 120000)
vm = FakeVmix(); emu = Emu('coop2', prep=prep)
assert emu.wait_http(); time.sleep(1.5); setup_tally(); time.sleep(0.5)
BIG = open(emu.work + '/sd/big.bin', 'rb').read()
pushes, stop = tally_pusher(vm, 0.1)
T0 = time.time(); marks = []
def mark(n): marks.append((n, round(time.time() - T0, 2)))

def reader(path, delay, chunk, out, rcvbuf=4096):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf); s.settimeout(40)
    s.connect((IP, 80)); t0 = time.time()
    s.sendall(('GET %s HTTP/1.1\r\nHost: t\r\n\r\n' % path).encode())
    data = b''; end = None
    while True:
        try: d = s.recv(chunk)
        except socket.timeout: end = 'timeout'; break
        except ConnectionResetError: end = 'reset'; break
        if not d: end = 'FIN'; break
        data += d
        if delay: time.sleep(delay)
    s.close()
    out.update({'data': data.split(b'\r\n\r\n', 1)[1] if b'\r\n\r\n' in data else b'', 'end': end, 't': round(time.time() - t0, 2)})

def slow_upload(name, payload, rate):
    """multipart upload sent at 'rate' bytes/s"""
    b = '----tccuBoundary12345678'
    body = ('--%s\r\nContent-Disposition: form-data; name="file"; filename="%s"\r\nContent-Type: application/octet-stream\r\n\r\n' % (b, name)).encode() + payload + ('\r\n--%s--\r\n' % b).encode()
    s = socket.socket(); s.settimeout(30); s.connect((IP, 80)); t0 = time.time()
    s.sendall(('POST /upload HTTP/1.1\r\nHost: t\r\nX-TallyCCU: 1\r\nContent-Type: multipart/form-data; boundary=%s\r\nContent-Length: %d\r\n\r\n' % (b, len(body))).encode())
    step = max(1, rate // 20)
    for i in range(0, len(body), step):
        s.sendall(body[i:i + step]); time.sleep(0.05)
    raw = b''
    while True:
        try: d = s.recv(4096)
        except OSError: break
        if not d: break
        raw += d
    s.close()
    return {'t': round(time.time() - t0, 2), 'resp': raw.split(b'\r\n\r\n', 1)[-1].decode('latin1', 'replace').strip()}

# 1) reader at ~20 KB/s during a ~6 s upload of another file
mark('p1')
o = {}
th = threading.Thread(target=reader, args=('/big.bin', 0.05, 1024, o)); th.start()
time.sleep(0.5)
mark('up1 start'); up = slow_upload('other.bin', os.urandom(48000), 8000); mark('up1 end')
th.join(40)
res['upload_during'] = up
res['reader_during_upload'] = {'end': o['end'], 't': o['t'], 'bytes': len(o['data'])}
check('upload ok', '"success":true' in up['resp'], up)
check('reader not cut by the busy server', o['end'] == 'FIN' and o['data'] == BIG, res['reader_during_upload'])

# 2) upload replacing the file being sent
mark('p2')
o2 = {}
th = threading.Thread(target=reader, args=('/swap.bin', 0.25, 512, o2, 2048)); th.start()
time.sleep(1.0)
NEW = b'B' * 50000
mark('up2 start'); up2 = slow_upload('swap.bin', NEW, 400000); mark('up2 end')
th.join(40)
res['replace'] = {'upload': up2, 'reader_end': o2['end'], 'reader_bytes': len(o2['data']), 'all_A': set(o2['data']) <= {ord('A')}}
check('replaced file: old transfer cut, only old bytes', '"success":true' in up2['resp'] and o2['end'] in ('FIN', 'reset') and set(o2['data']) <= {ord('A')} and len(o2['data']) < 120000, res['replace'])
r = http('/swap.bin')
check('new file served whole', r['body'] == NEW, len(r['body']))
cuts = [l for l in emu.serial().splitlines() if 'Transfer cut' in l]
res['cuts'] = cuts

# 3) SSE events during two transfers
mark('p3')
sse = Sse(); time.sleep(0.5)
a = {}; b = {}
ta = threading.Thread(target=reader, args=('/big.bin', 0.05, 1024, a)); tb = threading.Thread(target=reader, args=('/big.bin', 0.05, 1024, b))
ta.start(); tb.start(); time.sleep(0.3)
for i in range(5):
    http('/?cameraId=1&gain_db=%d' % (i * 2)); time.sleep(0.2)
ta.join(40); tb.join(40)
time.sleep(0.3)
txt = sse.text(); sse.close()
got = [l for l in txt.split('\n') if '"gain_db"' in l]
res['sse_events'] = len(got)
check('sse events during transfers', len(got) >= 5, len(got))
check('both transfers exact', a['data'] == BIG and b['data'] == BIG, (len(a['data']), len(b['data'])))

stop.set(); time.sleep(0.3)
res['tally'] = tally_latency(emu, pushes)
bmd = emu.tallies(1); slow = []
for tp, st in pushes:
    want = [1, 0] if st[0] == '1' else [0, 1]
    hit = next((b for b in bmd if b['wt'] / 1000.0 >= tp - 0.003 and list(b.get('v', [])) == want), None)
    if hit and hit['wt'] / 1000.0 - tp > 0.1: slow.append((round(tp - T0, 2), round(hit['wt'] / 1000.0 - tp, 3)))
res['slow_pushes'] = slow; res['marks'] = marks
check('tally bounded', res['tally']['missing'] == 0 and res['tally']['max'] < 0.15, res['tally'])
res['wdt'] = emu.wdt(); res['alive'] = emu.alive()
check('no WDT', not res['wdt'] and res['alive'])
res['fails'] = fails
emu.stop(); vm.close()
print(json.dumps(res, indent=1))
