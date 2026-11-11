"""(ser) Serial console: a garbage byte before a command (the USB-serial chip can deliver one
when the configurator opens the port without a reset) must not lose the command; the leading
newline the configurator sends is ignored."""
import sys, json, time, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
res = {'fails': []}
vm = FakeVmix()
emu = Emu('ser', serial=True)
emu.wait('System ready.'); time.sleep(1)
for name, junk, cmd in (('nul', b'\x00', b'status\n'), ('ff', b'\xff', b'status\n'), ('f0', b'\xf0', b'status\n'),
                        ('crlf_junk', b'\xfe\r\n', b'status\r\n'), ('lead_newline', b'', b'\nstatus\n')):
    m = emu.mark()
    os.write(emu.wfd, junk); time.sleep(0.2); os.write(emu.wfd, cmd)
    i = emu.wait('Local IP:', m, 5)
    out = emu.serial()[m:]
    res[name] = {'answered': i >= 0, 'unknown': 'Unknown command' in out}
    if i < 0 or 'Unknown command' in out:
        res['fails'].append(name)
res['alive'] = emu.alive()
emu.stop(); vm.close()
print(json.dumps(res, indent=1))
