"""(b2) deadman armed BEFORE a slow page transfer starts; joystick vanishes / is released during it."""
import sys, json, time, os, socket, threading; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
res = {}
vm = FakeVmix(); emu = Emu('b2')
assert emu.wait_http(); time.sleep(1.5); setup_tally(); time.sleep(1.5)
def slow(tag):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048); s.settimeout(40)
    s.connect((IP, 80)); t0 = time.time()
    s.sendall(b'GET / HTTP/1.1\r\nHost: t\r\n\r\n')
    got = 0
    while True:
        try: d = s.recv(512)
        except (socket.timeout, ConnectionResetError): break
        if not d: break
        got += len(d); time.sleep(0.25)
    res[tag] = {'bytes': got, 's': round(time.time() - t0, 2)}
for case in ('vanish', 'release'):
    cam = 2 if case == 'vanish' else 3
    t_arm = time.time(); http('/?cameraId=%d&pan_tilt_velocity=0.4,0.4&deadman=1' % cam)
    th = threading.Thread(target=slow, args=('slow_' + case,)); th.start()
    time.sleep(0.3)
    if case == 'release':
        t_rel = time.time(); r = http('/?cameraId=%d&pan_tilt_velocity=0,0' % cam, timeout=20)
        res['release_request_ms'] = round(r['t'] * 1000)
    th.join(); time.sleep(3)
    w = [(round(b['wt'] / 1000 - t_arm, 3), [x['f'] for x in b['v']][:2]) for b in emu.bmd() if b.get('group') == 11 and b.get('cam') == cam and b['wt'] / 1000 >= t_arm - 0.5]
    res['writes_' + case] = w
res['wdt'] = emu.wdt()
emu.stop(); vm.close()
print(json.dumps(res))
