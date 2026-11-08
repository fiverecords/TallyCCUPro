"""(f) TALLY line split in 2 segments 100 ms apart; (h) deadman with/without deadman=1."""
import sys, os, json, time; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
res = {}
def ccu(b): return b.get('fn', '').startswith('writeCommand')   # not the periodic tally refresh
vm = FakeVmix(); emu = Emu('fh')
assert emu.wait_http()
time.sleep(1.5); setup_tally(); time.sleep(2)
# (f)
res['f'] = []
for k, (st, split) in enumerate([('1000000000', 12), ('2000000000', 5), ('1000000000', 15), ('0000000000', 9)]):
    t = vm.push(st, split=split); time.sleep(0.6)
    want = {'1': [1, 0], '2': [0, 1], '0': [0, 0]}[st[0]]
    hit = [b for b in emu.tallies() if b['wt'] / 1000 >= t]
    res['f'].append({'state': st, 'split_at': split, 'applied': bool(hit and hit[-1]['v'] == want),
                     'lat_ms': round(hit[0]['wt'] - t * 1000) if hit else None, 'writes': [h['v'] for h in hit]})
ser = emu.serial()
res['f_serial_errors'] = [l for l in ser.splitlines() if 'vMix' in l and ('invalid' in l.lower() or 'bad' in l.lower() or 'too long' in l.lower())][:5]
# (h) deadman
def pt(since):
    return [(round(b['wt'] / 1000 - since, 3), [x['f'] for x in b['v']][:2]) for b in emu.bmd() if b.get('group') == 11 and b.get('param') == 0 and b['wt'] / 1000 >= since]
def zm(since):
    return [(round(b['wt'] / 1000 - since, 3), [x['f'] for x in b['v']][:1]) for b in emu.bmd() if b.get('group') == 0 and b.get('param') == 9 and b['wt'] / 1000 >= since]
t0 = time.time(); r = http('/?cameraId=1&pan_tilt_velocity=0.5,0.25&deadman=1'); time.sleep(2.0)
res['h_web_once'] = {'status': r['status'], 'writes': pt(t0)}
# refreshed every 250 ms for 2 s then stopped
t0 = time.time()
for i in range(8): http('/?cameraId=2&pan_tilt_velocity=0.5,0.25&deadman=1'); time.sleep(0.25)
tl = time.time(); time.sleep(2.0)
w = [(round(b['wt'] / 1000 - tl, 3), [x['f'] for x in b['v']][:2]) for b in emu.bmd() if b.get('group') == 11 and b.get('cam') == 2 and b['wt'] / 1000 >= t0]
res['h_web_refresh'] = {'stops': [x for x in w if x[1] == [0, 0]], 'nwrites': len(w)}
t0 = time.time(); r = http('/?cameraId=3&set_continuous_zoom_speed=0.5&deadman=1'); time.sleep(1.5)
res['h_zoom'] = [(round(b['wt'] / 1000 - t0, 3), b['v'][0]['f']) for b in emu.bmd() if b.get('group') == 0 and b.get('param') == 9 and b['wt'] / 1000 >= t0]
# Companion style: no deadman param -> no stop
t0 = time.time(); http('/?cameraId=4&pan_tilt_velocity=0.5,0.25'); http('/?cameraId=4&set_continuous_zoom_speed=0.3'); time.sleep(4.0)
res['h_companion_style'] = [(round(b['wt'] / 1000 - t0, 3), b['group'], [x['f'] for x in b['v']]) for b in emu.bmd() if b.get('cam') == 4 and ccu(b) and b['wt'] / 1000 >= t0]
# deadman armed, then Companion-style command for same cam disarms
t0 = time.time(); http('/?cameraId=5&pan_tilt_velocity=0.5,0.25&deadman=1'); http('/?cameraId=5&pan_tilt_velocity=0.2,0.2'); time.sleep(1.5)
res['h_disarm'] = [(round(b['wt'] / 1000 - t0, 3), [x['f'] for x in b['v']]) for b in emu.bmd() if b.get('cam') == 5 and ccu(b) and b['wt'] / 1000 >= t0]
# deadman=0 explicit
t0 = time.time(); r = http('/?cameraId=6&pan_tilt_velocity=0.5,0.25&deadman=0'); time.sleep(1.5)
res['h_deadman0'] = {'status': r['status'], 'w': [(round(b['wt'] / 1000 - t0, 3), [x['f'] for x in b['v']]) for b in emu.bmd() if b.get('cam') == 6 and ccu(b) and b['wt'] / 1000 >= t0]}
res['wdt'] = emu.wdt(); res['alive'] = emu.alive()
emu.stop(); vm.close()
print(json.dumps(res, indent=1))
