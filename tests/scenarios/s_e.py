"""(e) vMix half-open: DROP all traffic of the fake vMix; detect, tally off, reconnect after restore."""
import sys, json, time, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
res = {}
flush_drops()
vm = FakeVmix(); emu = Emu('e')
assert emu.wait_http(); time.sleep(1.5); setup_tally(); time.sleep(1.5)
vm.push('1000000000'); time.sleep(1.0)
res['tally_before'] = emu.tallies()[-1]['v']
td = time.time()
drop = Drop(8099, host=VMIX)
t_off = None
while time.time() - td < 40:
    t = [b for b in emu.tallies() if b['wt'] / 1000 > td]
    if t and t[-1]['v'] == [0, 0]: t_off = t[-1]['wt'] / 1000 - td; break
    time.sleep(0.1)
res['tally_off_after_s'] = round(t_off, 2) if t_off else None
res['serial_loss'] = [l for l in emu.serial().splitlines() if 'vMix' in l][-4:]
time.sleep(max(0, 25 - (time.time() - td)))
drop.undo(); tr = time.time()
nconn = len([e for e in vm.events if e[1] == 'connect'])
t_rec = None; t_on = None
while time.time() - tr < 45:
    if t_rec is None and len([e for e in vm.events if e[1] == 'connect']) > nconn: t_rec = time.time() - tr
    t = [b for b in emu.tallies() if b['wt'] / 1000 > tr]
    if t and t[-1]['v'] == [1, 0]: t_on = t[-1]['wt'] / 1000 - tr; break
    time.sleep(0.1)
res['reconnect_after_restore_s'] = round(t_rec, 2) if t_rec else None
res['tally_back_on_after_restore_s'] = round(t_on, 2) if t_on else None
res['serial_vmix'] = [l for l in emu.serial().splitlines() if 'vMix' in l][-8:]
res['web_ok'] = http('/?listPresets')['status']
res['wdt'] = emu.wdt(); res['alive'] = emu.alive()
emu.stop(); vm.close()
print(json.dumps(res, indent=1))
