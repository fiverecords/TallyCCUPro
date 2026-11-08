"""(d) 8 idle TCP connections to ports 80 and 8098 from other hosts; web, Companion and vMix must recover."""
import sys, json, time, os, socket; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
res = {}
vm = FakeVmix(); emu = Emu('d')
assert emu.wait_http(); time.sleep(1.5); setup_tally(); time.sleep(1.0)
comp = Companion(src=peer(50)); time.sleep(1.0)
pushes, stop = tally_pusher(vm, 0.1)
idle = []; errs = []
t0 = time.time()
for i in range(8):
    port = 80 if i % 2 == 0 else 8098
    s = socket.socket(); s.settimeout(3)
    s.bind((peer(100 + i), 0))
    try: s.connect((IP, port)); idle.append((port, s))
    except OSError as e: errs.append((port, str(e)))
res['idle_connect_errors'] = errs
# probe web every 0.5 s until OK
probe = []
t_ok = None
for k in range(40):
    r = http('/?listPresets', timeout=2)
    probe.append(r['status'])
    if r['status'] == 200 and t_ok is None and k > 0: t_ok = time.time() - t0
    if r['status'] == 200 and k > 3: break
    time.sleep(0.5)
res['web_probe'] = probe; res['web_ok_after_s'] = round(t_ok, 2) if t_ok else None
# when were idle sockets closed by the firmware
closed = {}
for port, s in idle:
    s.settimeout(0.1)
for k in range(150):
    for i, (port, s) in enumerate(idle):
        if i in closed: continue
        try:
            d = s.recv(100)
            if d == b'': closed[i] = (port, round(time.time() - t0, 1), 'FIN')
        except socket.timeout: pass
        except OSError as e: closed[i] = (port, round(time.time() - t0, 1), type(e).__name__)
    if len(closed) == len(idle): break
    time.sleep(0.1)
res['idle_closed'] = sorted(closed.values())
res['idle_not_closed'] = len(idle) - len(closed)
# Companion (existing) still alive?
comp.send(b'PING\n'); time.sleep(0.5)
res['existing_companion_pong'] = bool(comp.got('PONG')); res['existing_companion_closed'] = comp.closed
# new Companion from another IP
c2 = Companion(src=peer(51)); time.sleep(1.0)
res['new_companion_lines'] = [l for (t, l) in c2.lines][:3]
r = http('/?cameraId=1&gain_db=4'); time.sleep(0.3)
res['ccu_cmd'] = r['status']; res['ccu_echo_to_companions'] = [bool(comp.got('CCU 1 gain_db 4')), bool(c2.got('CCU 1 gain_db 4'))]
stop.set(); time.sleep(0.3)
res['tally'] = tally_latency(emu, pushes)
res['vmix_connects'] = [e for e in vm.events if e[1] != 'push' and not str(e[1]).startswith('push')]
res['wdt'] = emu.wdt(); res['alive'] = emu.alive()
res['serial'] = [l for l in emu.serial().splitlines() if 'CCUBcast' in l or 'Web' in l or 'idle' in l.lower()][-10:]
for p, s in idle: s.close()
comp.close(); c2.close(); emu.stop(); vm.close()
print(json.dumps(res, indent=1))
