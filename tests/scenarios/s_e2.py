"""(e2) HTTP and 8098 responsiveness while vMix is half-open / unreachable (connect retries)."""
import sys, json, time, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
def probe(dur, comp):
    lat = []; pong = []; t0 = time.time(); nxt = t0
    while time.time() - t0 < dur:
        r = http('/?listPresets', timeout=10); lat.append(round(r['t'] * 1000) if r['status'] == 200 else ('ERR', r['err']))
        if time.time() >= nxt:
            n = len(comp.got('PONG')); ts = time.time(); comp.send(b'PING\n')
            while len(comp.got('PONG')) == n and time.time() - ts < 5: time.sleep(0.005)
            pong.append(round((time.time() - ts) * 1000)); nxt += 1
        time.sleep(0.2)
    ok = [x for x in lat if isinstance(x, int)]
    return {'http_n': len(lat), 'http_err': len(lat) - len(ok), 'http_max_ms': max(ok), 'http_p50': sorted(ok)[len(ok)//2],
            'pong_n': len(pong), 'pong_max_ms': max(pong)}
res = {}
vm = FakeVmix(); emu = Emu('e2a')
assert emu.wait_http(); time.sleep(1.5); setup_tally(); time.sleep(1.5)
comp = Companion(src=peer(50)); time.sleep(1)
res['baseline'] = probe(5, comp)
drop = Drop(8099, host=VMIX)
res['vmix_halfopen_30s'] = probe(30, comp)
drop.undo(); time.sleep(12)
res['wdt_a'] = emu.wdt(); res['serial_a'] = [l for l in emu.serial().splitlines() if 'vMix' in l][-6:]
comp.close(); emu.stop(); vm.close()
# unreachable vMix: no remap -> CONNECT never answered (ARP timeout)
emu = Emu('e2b', env={'EMU_REMAP_ALL': ''})
assert emu.wait_http(); time.sleep(1.5)
comp = Companion(src=peer(50)); time.sleep(1)
res['vmix_unreachable_40s'] = probe(40, comp)
res['wdt_b'] = emu.wdt(); res['serial_b'] = [l for l in emu.serial().splitlines() if 'vMix' in l][-6:]
comp.close(); emu.stop()
print(json.dumps(res, indent=1))
