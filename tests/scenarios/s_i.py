"""(i) SD absent at boot, and SD pulled at runtime: loadPreset 500 'SD not available', the rest keeps working."""
import sys, json, time, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
def prep(sd):
    open(os.path.join(sd, 'preset_1_0.dat'), 'w').write('cameraId:1\r\npresetId:0\r\nname:P\r\ngain_db:6\r\n')
def short(r): return [r['status'], r['body'][:90].decode('latin1').strip()]
def check(emu, vm, comp, tag):
    out = {}
    out['loadPreset'] = short(http('/?loadPreset=1,0'))
    out['listPresets'] = short(http('/?listPresets'))
    out['savePreset'] = short(http('/savePreset', method='POST', body='cameraId=1&presetId=1&name=X&data=gain_db%3A6%3B',
                                    headers={'Content-Type': 'application/x-www-form-urlencoded'}))
    out['index'] = short(http('/'))
    out['ccu'] = short(http('/?cameraId=1&gain_db=2'))
    out['ccu_echo'] = bool(comp.got('CCU 1 gain_db 2'))
    t = vm.push('1000000000' if tag != 'b' else '2000000000'); time.sleep(0.4)
    out['tally_applied'] = bool([b for b in emu.tallies() if b['wt'] / 1000 >= t])
    vm.push('0000000000'); time.sleep(0.2)
    return out
res = {}
# 1) absent at boot
vm = FakeVmix(); emu = Emu('i1', env={'EMU_SD_FAIL': '1'}, prep=prep)
assert emu.wait_http(); time.sleep(1.5); setup_tally(); time.sleep(1)
comp = Companion(src=peer(50)); time.sleep(0.5)
t = time.time(); res['absent_at_boot'] = check(emu, vm, comp, 'a'); res['absent_at_boot']['check_s'] = round(time.time() - t, 2)
res['absent_wdt'] = emu.wdt(); comp.close(); emu.stop()
# 2) pulled at runtime, then reinserted
time.sleep(1); emu = Emu('i2', prep=prep)
assert emu.wait_http(); time.sleep(1.5); setup_tally(); time.sleep(1)
comp = Companion(src=peer(50)); time.sleep(0.5)
res['present'] = check(emu, vm, comp, 'p')
sd = emu.work + '/sd'; os.rename(sd, sd + '.out')
t = time.time(); res['pulled'] = check(emu, vm, comp, 'b'); res['pulled']['check_s'] = round(time.time() - t, 2)
os.rename(sd + '.out', sd); time.sleep(11)
res['reinserted'] = check(emu, vm, comp, 'r')
res['serial_sd'] = [l for l in emu.serial().splitlines() if 'SD' in l][-6:]
res['wdt'] = emu.wdt(); res['alive'] = emu.alive()
comp.close(); emu.stop(); vm.close()
print(json.dumps(res, indent=1))
