"""(upl) tally latency around the end of slow multipart uploads (no transfers)."""
import sys, json, time, os, socket, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
vm = FakeVmix(); emu = Emu('upl')
assert emu.wait_http(); time.sleep(1.5); setup_tally(); time.sleep(0.5)
pushes, stop = tally_pusher(vm, 0.1)
T0 = time.time(); marks = []
def slow_upload(name, payload, rate):
    b = '----tccuBoundary12345678'
    body = ('--%s\r\nContent-Disposition: form-data; name="file"; filename="%s"\r\nContent-Type: application/octet-stream\r\n\r\n' % (b, name)).encode() + payload + ('\r\n--%s--\r\n' % b).encode()
    s = socket.socket(); s.settimeout(30); s.connect((IP, 80))
    s.sendall(('POST /upload HTTP/1.1\r\nHost: t\r\nX-TallyCCU: 1\r\nContent-Type: multipart/form-data; boundary=%s\r\nContent-Length: %d\r\n\r\n' % (b, len(body))).encode())
    step = max(1, rate // 20)
    for i in range(0, len(body), step):
        s.sendall(body[i:i + step]); time.sleep(0.05)
    t_sent = time.time() - T0
    raw = b''
    while True:
        try: d = s.recv(4096)
        except OSError: break
        if not d: break
        raw += d
    s.close()
    return {'sent_at': round(t_sent, 2), 'done_at': round(time.time() - T0, 2), 'resp': raw.split(b'\r\n\r\n', 1)[-1].decode('latin1', 'replace').strip()}
time.sleep(0.5)
ups = [slow_upload('other%d.bin' % i, os.urandom(48000), 8000) for i in range(2)]
time.sleep(1)
stop.set(); time.sleep(0.3)
bmd = emu.tallies(1); slow = []
for tp, st in pushes:
    want = [1, 0] if st[0] == '1' else [0, 1]
    hit = next((b for b in bmd if b['wt'] / 1000.0 >= tp - 0.003 and list(b.get('v', [])) == want), None)
    if hit and hit['wt'] / 1000.0 - tp > 0.1: slow.append((round(tp - T0, 2), round(hit['wt'] / 1000.0 - tp, 3)))
res = {'ups': ups, 'tally': tally_latency(emu, pushes), 'slow': slow, 'wdt': emu.wdt(), 'alive': emu.alive()}
fails = ['upload %d: %s' % (i, u['resp']) for i, u in enumerate(ups) if '"success":true' not in u['resp']]
t = res['tally']
if t['missing'] or t['max'] is None or t['max'] >= 0.15:
    fails.append('tally: %s' % t)
res['fails'] = fails
emu.stop(); vm.close()
print(json.dumps(res, indent=1))
