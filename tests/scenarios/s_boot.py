"""(boot) Reset causes and boot-failure counting: restart, reset pin, hang in operation and in the
boot window (RAM record kept across resets, watchdog interrupt), Safe Mode after 3, power cycle."""
import sys, json, re, time, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
os.makedirs(RUNS, exist_ok=True)
HANG = os.path.join(RUNS, 'boot-hang')   # '1': every tally write hangs (shield stuck)
SH = os.path.join(RUNS, 'boot-shield')
open(HANG, 'w').write('0'); open(SH, 'w').write('1')
ENV = {'EMU_HANG_FILE': HANG, 'EMU_SHIELD_FILE': SH, 'EMU_MAX_BOOTS': '30'}


def resets(s):
    return re.findall(r'\[SafeMode\] Last reset: [^\r\n]*', s)


def hang_once(emu, since):
    """Hang until the watchdog resets, then release the shield for the next boot."""
    open(HANG, 'w').write('1')
    emu.wait('[SafeMode] Last reset', since + 1, 30)
    open(HANG, 'w').write('0')
    emu.wait('System ready.', since + 1, 30)


res = {}
vm = FakeVmix()
emu = Emu('boot', env=ENV, serial=True)
emu.wait('System ready.'); emu.wait_http()
res['1_poweron'] = resets(emu.serial())
# a mapped camera: its tally is re-sent every round, so a stuck shield hangs (kept in EEPROM)
res['0_map'] = setup_tally()['status']
m = emu.mark(); emu.send('reset'); emu.wait('System ready.', m + 1, 30)
res['2_restart'] = resets(emu.serial()[m:])
m = emu.mark(); emu.ext_reset(); emu.wait('System ready.', m + 1, 30)
res['3_external'] = resets(emu.serial()[m:])
# hang after the 20 s boot window: a crash in operation, not a boot failure
emu.wait('Boot completed successfully', m, 30)
m = emu.mark(); hang_once(emu, m)
res['4_hang_in_operation'] = resets(emu.serial()[m:])
res['4_boot_failure'] = 'Boot failure detected' in emu.serial()[m:]
res['4_wdt_interrupt'] = 'WDT interrupt' in emu.emu_log()
# reset pin 3 times inside the boot window: no Safe Mode (BOOT-06)
m = emu.mark()
for _ in range(3):
    mm = emu.mark(); emu.ext_reset(); emu.wait('System ready.', mm + 1, 30); time.sleep(1)
res['5_three_external'] = resets(emu.serial()[m:])
res['5_safe_mode'] = 'ENTERING SAFE MODE' in emu.serial()[m:]
# 3 hangs inside the boot window: Safe Mode
m = emu.mark()
for _ in range(3):
    hang_once(emu, emu.mark()); time.sleep(1)
s = emu.serial()[m:]
res['6_three_boot_hangs'] = resets(s)
res['6_counts'] = re.findall(r'Boot failure detected! Count: \d', s)
res['6_safe_mode'] = 'ENTERING SAFE MODE' in s
emu.wait_http()
res['6_safemode_status'] = http('/safemode-status')['body'][:200].decode('latin1')
# power cycle (new process, EEPROM kept, RAM lost): a normal boot again
emu.stop()
emu = Emu('boot', env=ENV, serial=True, keep=True)
k0 = len(emu.serial())
emu.wait('System ready.', k0, 30); time.sleep(1)
s = emu.serial()[k0:]
res['7_after_power_cycle'] = resets(s)
res['7_safe_mode'] = 'SAFE MODE ACTIVE' in s
res['resets_total'] = len(emu.wdt())
res['alive'] = emu.alive()
emu.stop(); vm.close()
print(json.dumps(res, indent=1))
