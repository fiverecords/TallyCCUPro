"""(j) Safe Mode: SDI shield absent -> boot loop -> Safe Mode; shield back -> automatic recovery;
anti-loop: a recovery that fails again counts a strike; at the max strikes it stays in Safe Mode."""
import sys, json, time, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
os.makedirs(RUNS, exist_ok=True)
SH = os.path.join(RUNS, 'shield')
def shield(v): open(SH, 'w').write(v)
def wait_serial(emu, pat, since_len, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        s = emu.serial()
        if pat in s[since_len:]: return round(time.time() - t0, 1)
        time.sleep(0.25)
    return None
def wait_web(timeout=30):
    # a TCP connect can succeed before the firmware listens: wait for a real answer
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = http('/safemode-status', timeout=2)
        if r['status'] == 200: return r
        time.sleep(0.3)
    return r
def eep(emu, a):
    d = open(emu.work + '/eeprom.bin', 'rb').read(); return d[a]
res = {}
vm = FakeVmix()
# A) absent -> Safe Mode -> present -> auto recovery
shield('0')
t0 = time.time()
emu = Emu('jA', env={'EMU_SHIELD_FILE': SH, 'EMU_MAX_BOOTS': '20'})
res['A_safe_mode_after_s'] = wait_serial(emu, 'ENTERING SAFE MODE', 0, 90)
r = wait_web(); res['A_status_in_safe'] = r['body'][:200].decode('latin1')
res['A_cmd_in_safe'] = [http('/?cameraId=1&gain_db=2')['status'], http('/?listPresets')['status']]
shield('1'); tp = time.time(); L = len(emu.serial())
res['A_recover_msg_after_s'] = wait_serial(emu, 'restarting in normal mode', L, 80)
L2 = len(emu.serial())
res['A_boot_completed_after_s'] = wait_serial(emu, 'Boot completed successfully', L2, 60)
res['A_normal_cmd'] = http('/?cameraId=1&gain_db=2')['status']
res['A_strikes_auto'] = [eep(emu, 315), eep(emu, 316)]
res['A_wdt_resets'] = len(emu.wdt())
emu.stop()
# B) recovery that fails again: shield present in Safe Mode, then lost right after the auto-exit
shield('0')
emu = Emu('jB', env={'EMU_SHIELD_FILE': SH, 'EMU_MAX_BOOTS': '30'})
wait_serial(emu, 'ENTERING SAFE MODE', 0, 90); wait_web()
shield('1'); L = len(emu.serial())
wait_serial(emu, 'restarting in normal mode', L, 80)
shield('0'); L = len(emu.serial())       # it dies again during the next boot
res['B_back_in_safe_after_s'] = wait_serial(emu, 'ENTERING SAFE MODE', L, 90)
time.sleep(2)
res['B_strikes_auto'] = [eep(emu, 315), eep(emu, 316)]
res['B_status'] = wait_web()['body'][:250].decode('latin1')
shield('1'); L = len(emu.serial())
res['B_no_quick_recovery_60s'] = wait_serial(emu, 'restarting in normal mode', L, 60) is None
res['B_probe_msgs'] = [l for l in emu.serial()[L:].splitlines() if 'SafeMode' in l][:4]
emu.stop()
# C) at max strikes (EEPROM pre-set: strikes=1, auto flag) a new failure gives strike 2: disarmed
e = bytearray(open(os.path.join(RUNS, 'jB', 'eeprom.bin'), 'rb').read())
e[315] = 1; e[316] = 0x5A
open(os.path.join(RUNS, 'eepC.bin'), 'wb').write(bytes(e))
shield('0')
emu = Emu('jC', env={'EMU_SHIELD_FILE': SH, 'EMU_MAX_BOOTS': '30'}, eeprom=os.path.join(RUNS, 'eepC.bin'))
wait_serial(emu, 'ENTERING SAFE MODE', 0, 90); time.sleep(2)
res['C_strikes_auto'] = [eep(emu, 315), eep(emu, 316)]
res['C_status'] = wait_web()['body'][:250].decode('latin1')
shield('1'); L = len(emu.serial())
res['C_no_recovery_40s'] = wait_serial(emu, 'restarting in normal mode', L, 40) is None
res['C_probe_msgs'] = [l for l in emu.serial()[L:].splitlines() if 'SafeMode' in l][:3]
emu.stop(); vm.close()
print(json.dumps(res, indent=1))
