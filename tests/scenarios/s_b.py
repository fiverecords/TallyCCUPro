"""(b) slow index.html reader (~2 KB/s) + another cancelled midway; tally latency; deadman armed during serveFile."""
import sys, json, time, os, socket, threading; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
gz = len(sys.argv) > 1 and sys.argv[1] == 'gz'
def prep(sd):
    if gz:
        import gzip
        for f in ('index.html', 'tally.html', 'sdcard.html'):
            open(os.path.join(sd, f + '.gz'), 'wb').write(gzip.compress(open(os.path.join(sd, f), 'rb').read(), 9))
res = {'gz': gz}
vm = FakeVmix(); emu = Emu('b' + ('gz' if gz else ''), prep=prep)
assert emu.wait_http(); time.sleep(1.5); setup_tally(); time.sleep(1.5)
pushes, stop = tally_pusher(vm, 0.1)
def slow():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048); s.settimeout(40)
    s.connect((IP, 80)); t0 = time.time()
    s.sendall(b'GET / HTTP/1.1\r\nHost: t\r\nAccept-Encoding: gzip, deflate\r\n\r\n')
    got = 0; err = None
    while True:
        try: d = s.recv(512)
        except socket.timeout: err = 'timeout'; break
        except ConnectionResetError: err = 'reset'; break
        if not d: break
        got += len(d); time.sleep(0.25)
    res['slow'] = {'bytes': got, 's': round(time.time() - t0, 2), 'end': err or 'FIN'}
def cancel():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096); s.settimeout(10)
    s.connect((IP, 80)); s.sendall(b'GET /index.html HTTP/1.1\r\nHost: t\r\n\r\n')
    got = 0
    while got < 30000:
        d = s.recv(1024)
        if not d: break
        got += len(d); time.sleep(0.02)
    s.close(); res['cancel'] = {'bytes_before_close': got}
th = threading.Thread(target=slow); th.start()
time.sleep(0.5)
# deadman armed while the page is being served (the joystick then disappears)
t_dm = time.time(); r_dm = http('/?cameraId=2&pan_tilt_velocity=0.4,0.4&deadman=1', timeout=15)
res['dm_request_ms'] = round(r_dm['t'] * 1000); res['dm_status'] = r_dm['status']
time.sleep(0.5)
tc = threading.Thread(target=cancel); tc.start(); tc.join()
# other requests during the slow transfer
lat = []
for i in range(5):
    r = http('/?listPresets', timeout=15); lat.append((r['status'], round(r['t'] * 1000))); time.sleep(0.5)
res['requests_during'] = lat
th.join()
time.sleep(1)
stop.set(); time.sleep(0.3)
w = [(round(b['wt'] / 1000 - t_dm, 3), [x['f'] for x in b['v']][:2]) for b in emu.bmd() if b.get('group') == 11 and b.get('cam') == 2 and b['wt'] / 1000 >= t_dm - 1]
res['deadman_writes_rel_arm'] = w
res['tally'] = tally_latency(emu, pushes)
res['serial_tail'] = [l for l in emu.serial().splitlines() if 'SD' in l or 'PTZ' in l or 'Web' in l][-8:]
res['wdt'] = emu.wdt(); res['alive'] = emu.alive()
emu.stop(); vm.close()
print(json.dumps(res, indent=1))
