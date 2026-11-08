"""Contract checks: HTTP-12 double response, stale .gz after upload/delete, slot clear, vmixConnect=0 with tally on."""
import sys, json, time, os, gzip; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
def prep(sd):
    open(os.path.join(sd, 'foo.txt'), 'w').write('NEW-PLAIN')
    open(os.path.join(sd, 'foo.txt.gz'), 'wb').write(gzip.compress(b'OLD-GZ'))
    os.makedirs(os.path.join(sd, 'adir'))
res = {}
vm = FakeVmix(); emu = Emu('misc', prep=prep)
assert emu.wait_http(); time.sleep(1.5)
for p in ('/favicon.ico', '/adir', '/nothere.js'):
    r = http(p); res['nresp ' + p] = r['raw'].count(b'HTTP/1.1 ')
r = http('/foo.txt', headers={'Accept-Encoding': 'gzip'}); res['gz_served_before'] = r['raw'].count(b'Content-Encoding: gzip')
# upload a new foo.txt through /upload (multipart)
B = '----b0undary'
body = ('--%s\r\nContent-Disposition: form-data; name="file"; filename="foo.txt"\r\nContent-Type: text/plain\r\n\r\nUPLOADED-PLAIN\r\n--%s--\r\n' % (B, B))
r = http('/upload', method='POST', body=body, headers=dict(CSRF, **{'Content-Type': 'multipart/form-data; boundary=' + B}))
res['upload'] = r['body'][:80].decode('latin1')
r = http('/foo.txt', headers={'Accept-Encoding': 'gzip'})
res['after_upload_served'] = 'gzip(stale)' if b'Content-Encoding: gzip' in r['raw'] else r['body'][:20].decode('latin1')
res['gz_still_on_sd'] = os.path.exists(emu.work + '/sd/foo.txt.gz')
# tally slot clear
setup_tally(); time.sleep(0.3)
res['lone_zero'] = http('/?tallyCam0Input=0', headers=CSRF)['status']
res['both_zero'] = http('/?tallyCam0Input=0&cam0ID=0', headers=CSRF)['status']
res['map_after'] = http('/?getTallyMap')['body'][:120].decode('latin1')
setup_tally(); time.sleep(0.5)
vm.push('1000000000'); time.sleep(0.5)
t = time.time(); r = http('/?vmixConnect=0'); time.sleep(0.8)
res['vmix_off_tally_writes'] = [b['v'] for b in emu.tallies() if b['wt'] / 1000 >= t]
http('/?vmixConnect=1'); time.sleep(3)
res['vmix_back'] = [b['v'] for b in emu.tallies() if b['wt'] / 1000 >= t][-1:]
res['wdt'] = emu.wdt()
emu.stop(); vm.close()
print(json.dumps(res, indent=1))
