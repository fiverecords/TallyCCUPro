"""(k) interrupted savePreset keeps the previous preset."""
import sys, json, time, os, socket; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
res = {}
vm = FakeVmix(); emu = Emu('k')
assert emu.wait_http(); time.sleep(1.5)
H = {'Content-Type': 'application/x-www-form-urlencoded'}
body = 'cameraId=1&presetId=2&name=Original&data=gain_db%3A6%3Biris%3A0.5%3Baperture_normalised%3A0.3%3B'
res['save_ok'] = http('/savePreset', method='POST', body=body, headers=H)['body'][:60].decode()
sd = emu.work + '/sd'; orig = open(sd + '/preset_1_2.dat').read()
newbody = 'cameraId=1&presetId=2&name=Nuevo&data=' + 'gain_db%3A12%3B' + 'focus%3A0.1%3B' * 150
for mode in ('close', 'rst', 'stall'):
    s = socket.socket(); s.connect((IP, 80))
    req = ('POST /savePreset HTTP/1.1\r\nHost: t\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\n\r\n' % len(newbody)).encode()
    s.sendall(req + newbody[: len(newbody) // 2].encode())
    t = time.time()
    if mode == 'close': time.sleep(0.3); s.close(); resp = None
    elif mode == 'rst':
        time.sleep(0.3); import struct; s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0)); s.close(); resp = None
    else:
        s.settimeout(15)
        try: resp = s.recv(4096)
        except socket.timeout: resp = b'<timeout>'
        s.close()
    time.sleep(1.0)
    now = open(sd + '/preset_1_2.dat').read() if os.path.exists(sd + '/preset_1_2.dat') else None
    res[mode] = {'file_unchanged': now == orig, 'leftover_tmp': os.path.exists(sd + '/preset_1_2.tmp'),
                 'resp': (resp or b'')[-80:].decode('latin1'), 'resp_s': round(time.time() - t - 1.0, 2)}
lp = http('/?listPresets')['body'].decode()
res['list_name'] = 'Original' in lp and 'Nuevo' not in lp
t = time.time(); r = http('/?loadPreset=1,2'); time.sleep(0.3)
res['load'] = r['body'][:50].decode()
res['applied_gain'] = [b['v'] for b in emu.bmd() if b['wt'] / 1000 >= t and b.get('group') == 1 and b.get('param') == 13]
res['sd_files'] = sorted(f for f in os.listdir(sd) if f.startswith('preset'))
res['wdt'] = emu.wdt(); res['alive'] = emu.alive()
emu.stop(); vm.close()
print(json.dumps(res, indent=1))
