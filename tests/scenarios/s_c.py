"""(c) Companion link DROPped in the middle of its CCUSYNC dump; then restored."""
import sys, json, time, os, re; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
rows = re.findall(r'\{\s*(\d+),\s*(\d+),\s*(TYPE_\w+),\s*"(\w+)",\s*(true|false),\s*(\d+)\s*\}', open(os.path.join(FW_SRC, 'bmd_params.cpp')).read())
dump = []
for cam in range(1, 9):
    for g, p, t, k, sub, n in rows:
        if t == 'TYPE_VOID': continue
        n = int(n)
        v = {'TYPE_FIXED16': '0.25', 'TYPE_BOOLEAN': 'true', 'TYPE_STRING': 'en'}.get(t, '1')
        dump.append('%d %s %s' % (cam, k, ','.join([v] * n)))
    dump.append('%d system_language %s' % (cam, 'L' * 100))     # line > 100 chars
res = {'dump_lines': len(dump), 'long_lines': sum(1 for d in dump if len('CCUSYNC ' + d) > 100)}
flush_drops()
vm = FakeVmix(); emu = Emu('c')
assert emu.wait_http(); time.sleep(1.5); setup_tally(); time.sleep(1.0)
comp = Companion(dump=None); time.sleep(1.0); comp_dump = dump
pushes, stop = tally_pusher(vm, 0.1)
sse = Sse(); time.sleep(0.3)
# wait for REQUESTSYNC, then dump with a DROP after ~0.3 s
t0 = time.time()
while not comp.got('REQUESTSYNC') and time.time() - t0 < 5: time.sleep(0.02)
res['requestsync_after_sse_ms'] = round((comp.got('REQUESTSYNC')[0][0] - t0) * 1000) if comp.got('REQUESTSYNC') else None
import threading
comp.dump = dump; threading.Thread(target=comp.send_dump, daemon=True).start()
time.sleep(0.3)
comp.reading = False
drop = Drop(comp.port, host=LOOPBACK_NET)
td = time.time()
for i in range(60): http('/?cameraId=%d&gain_db=%d' % (1 + i % 8, i % 12)); time.sleep(0.05)
time.sleep(8 - (time.time() - td))
drop.undo(); comp.reading = True
tr = time.time()
# wait until the SSE event count stops growing
last = -1
for i in range(60):
    n = sse.text().count('data: {"cam"')
    if n == last and i > 10: break
    last = n; time.sleep(0.5)
stop.set(); time.sleep(0.3)
ev = sse.text()
keys = re.findall(r'data: \{"cam":(\d+),"key":"(\w+)","val":"([^"]*)"\}', ev)
dumpkeys = set((d.split()[0], d.split()[1]) for d in dump)
seen = set(k[:2] for k in keys)
res['sse_events_total'] = len(keys)
res['dump_keys_seen_in_sse'] = len(dumpkeys & seen); res['dump_keys'] = len(dumpkeys)
res['missing_sample'] = sorted(dumpkeys - seen)[:6]
res['drain_after_restore_s'] = round(time.time() - tr, 1)
res['comp_closed'] = comp.closed
res['comp_rx_after_restore'] = [l for (t, l) in comp.lines if t >= tr][:5]
res['comp_ccu_lines'] = len([l for (t, l) in comp.lines if l.startswith('CCU ')])
comp.send(b'PING\n'); time.sleep(0.5)
res['pong_after'] = bool([l for (t, l) in comp.lines if l == 'PONG'])
res['serial'] = [l for l in emu.serial().splitlines() if 'CCUBcast' in l][-8:]
res['tally'] = tally_latency(emu, pushes)
res['wdt'] = emu.wdt(); res['alive'] = emu.alive()
sse.close(); comp.close(); emu.stop(); vm.close()
print(json.dumps(res, indent=1))
