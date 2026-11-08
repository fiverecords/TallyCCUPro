"""(a) SSE client vanishes (DROP) while a 40-param preset loads repeatedly; vMix tally keeps changing."""
import sys, json, time, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
P = [('focus', '0.5'), ('aperture_f_stop', '2.8'), ('aperture_normalised', '0.4'), ('set_absolute_zoom_normalised', '0.3'),
     ('gain', '2'), ('manual_white_balance', '5600,10'), ('exposure_us', '20000'), ('dynamic_range_mode', '1'),
     ('video_sharpening_level', '1'), ('shutter_angle', '18000'), ('gain_db', '6'), ('iso', '800'), ('nd_filter_stop', '2,0'),
     ('mic_level', '0.5'), ('headphone_level', '0.5'), ('headphone_program_mix', '0.5'), ('speaker_level', '0.5'),
     ('input_type', '1'), ('input_levels', '0.5,0.5'), ('phantom_power', '1'), ('brightness', '0.5'), ('zebra_level', '0.5'),
     ('peaking_level', '0.5'), ('focus_assist', '1,1'), ('program_return_feed_enable', '1'), ('tally_brightness', '0.5'),
     ('front_tally_brightness', '0.5'), ('rear_tally_brightness', '0.5'), ('lift_adjust', '0.1,0.1,0.1,0.1'),
     ('gamma_adjust', '0.1,0.1,0.1,0.1'), ('gain_adjust', '1.2,1.2,1.2,1.2'), ('offset_adjust', '0.1,0.1,0.1,0.1'),
     ('contrast_adjust', '0.5,1.1'), ('luma_mix', '1'), ('color_adjust', '0.1,1.1'), ('display_lut', '1,1'),
     ('exposure_and_focus_tools', '1,1'), ('overlays', '1,1,1,1'), ('aperture_ordinal', '3'), ('exposure_ordinal', '4')]
def prep(sd):
    for p in range(2):
        open(os.path.join(sd, 'preset_1_%d.dat' % p), 'w').write(
            'cameraId:1\r\npresetId:%d\r\nname:Big %d\r\n' % (p, p) + ''.join('%s:%s\r\n' % kv for kv in P))
res = {'nparams': len(P)}
flush_drops()
vm = FakeVmix(); emu = Emu('a', prep=prep)
assert emu.wait_http(); time.sleep(1.5); setup_tally(); time.sleep(1.5)
good = Sse(); bad = Sse(rcvbuf=2048); time.sleep(1.0)
pushes, stop = tally_pusher(vm, 0.1)
time.sleep(1.0)
drop = Drop(bad.port, host=IP)   # rules on the client side port
drop2 = Drop(bad.port, host=LOOPBACK_NET)
bad.reading = False
loads = []
for k in range(8):
    t = time.time(); r = http('/?loadPreset=1,%d' % (k % 2), timeout=30)
    loads.append({'status': r['status'], 'ms': round((r['t']) * 1000), 'body': r['body'][:80].decode('latin1')})
    time.sleep(0.7)
time.sleep(3)
# keep loading until SSE timeout cleans up the dead client
t_unseen = time.time()
for k in range(4):
    r = http('/?loadPreset=1,%d' % (k % 2), timeout=30); loads.append({'status': r['status'], 'ms': round(r['t'] * 1000)}); time.sleep(1)
stop.set(); time.sleep(0.5)
drop.undo(); drop2.undo()
res['loads'] = loads
res['tally'] = tally_latency(emu, pushes)
gt = good.text()
res['good_sse_events'] = gt.count('data: {"cam"')
res['good_closed'] = good.closed
ser = emu.serial()
res['serial_sse'] = [l for l in ser.splitlines() if '[SSE]' in l][-8:]
res['ccu_writes_cam1'] = len([b for b in emu.bmd() if b.get('cam') == 1 and b.get('fn', '').startswith('writeCommand')])
res['web_after'] = http('/?listPresets')['status']
res['wdt'] = emu.wdt(); res['alive'] = emu.alive()
good.close(); bad.close(); emu.stop(); vm.close()
print(json.dumps(res, indent=1))
