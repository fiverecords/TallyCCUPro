"""(g) two SSE browsers at once: both get events, no eviction loop, REQUESTSYNC <= 1 per 10 s.
Also: an SSE reopened 3 s after a sync, kept open, gets the deferred REQUESTSYNC (~10 s later)."""
import sys, json, time, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
res = {}
vm = FakeVmix(); emu = Emu('g')
assert emu.wait_http(); time.sleep(1.5)
comp = Companion(src=peer(50), dump=['1 gain_db 3', '2 gain_db 4']); time.sleep(1)
t0 = time.time()
a = Sse(); time.sleep(0.5); b = Sse(); time.sleep(1.0)
for i in range(10): http('/?cameraId=1&gain_db=%d' % i); time.sleep(0.1)
time.sleep(0.5)
res['events_a'] = a.text().count('"key":"gain_db"'); res['events_b'] = b.text().count('"key":"gain_db"')
res['a_closed'] = a.closed; res['b_closed'] = b.closed
# both SSE open for 25 s, browsers reconnect one of them every 3 s (tab reload)
for k in range(4):
    b.close(); time.sleep(0.2); b = Sse(); time.sleep(3)
time.sleep(14)
rs = [round(t - t0, 2) for (t, l) in comp.lines if l == 'REQUESTSYNC']
res['requestsync_times'] = rs
res['min_gap_s'] = min([y - x for x, y in zip(rs, rs[1:])] or [None])
ser = emu.serial().splitlines()
res['sse_new'] = len([l for l in ser if '[SSE] New client' in l]); res['sse_replaced'] = len([l for l in ser if 'Replacing oldest' in l])
res['a_still_open'] = not a.closed; res['b_still_open'] = not b.closed
res['a_got_sync_events'] = a.text().count('"key":"gain_db","val":"3"') ; res['b_got_sync_events'] = b.text().count('"key":"gain_db","val":"3"')
# third tab replaces the oldest (a)
c = Sse(); time.sleep(1.0)
res['third_tab'] = {'a_closed': a.closed, 'b_closed': b.closed, 'c_closed': c.closed}
res['wdt'] = emu.wdt(); res['alive'] = emu.alive()
for s in (a, b, c): s.close()
comp.close(); emu.stop(); vm.close()
print(json.dumps(res, indent=1))
