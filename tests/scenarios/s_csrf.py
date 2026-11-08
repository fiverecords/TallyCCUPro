"""(csrf) SEC-01/SEC-06 contract: state-changing endpoints need 'X-TallyCCU: 1' (403 'ERROR forbidden'
without it, nothing applied); the Companion and read-only endpoints do not; no CORS header anywhere;
OPTIONS never succeeds."""
import sys, os, json, time; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *

def prep(sd):
    open(os.path.join(sd, 'preset_1_0.dat'), 'w').write('cameraId:1\r\npresetId:0\r\nname:P\r\ngain_db:6\r\n')
    for f in ('del.txt', 'ren.txt'):
        open(os.path.join(sd, f), 'w').write('x')

FORM = {'Content-Type': 'application/x-www-form-urlencoded'}
B = '----b0undary'
UPLOAD = ('--%s\r\nContent-Disposition: form-data; name="file"; filename="up.txt"\r\nContent-Type: text/plain\r\n\r\n'
          'UP\r\n--%s--\r\n' % (B, B))
MP = {'Content-Type': 'multipart/form-data; boundary=' + B}
SYNC = '1:gain_db:4\n'

# (name, method, path, body, extra headers) that must be refused without the header
PROTECTED = [
    ('deleteFile', 'GET', '/?deleteFile=del.txt', None, {}),
    ('renameFile', 'GET', '/?renameFile=ren.txt&to=ren2.txt', None, {}),
    ('upload', 'POST', '/upload', UPLOAD, MP),
    ('overrideTally', 'GET', '/?overrideTally=0', None, {}),
    ('overrideCCU', 'GET', '/?overrideCCU=0', None, {}),
    ('vmixIP', 'GET', '/?vmixIP=10.9.8.7', None, {}),
    ('tallyMapInput', 'GET', '/?tallyCam3Input=7', None, {}),
    ('tallyMapCam', 'GET', '/?cam3ID=7', None, {}),
    ('tallyMapBoth', 'GET', '/?tallyCam3Input=7&cam3ID=7', None, {}),
    ('mixedQuery', 'GET', '/?cameraId=7&gain_db=7&overrideTally=0', None, {}),
    ('syncState', 'POST', '/syncState', SYNC, {'Content-Type': 'text/plain'}),
    # last: on a firmware without the check these restart it
    ('safemode-exit', 'POST', '/safemode-exit', '', FORM),
    ('reboot', 'GET', '/reboot', None, {}),
]
# with the header these must work (reboot and safemode-exit are left out: they restart)
WITH_HEADER = ['deleteFile', 'renameFile', 'upload', 'overrideTally', 'overrideCCU', 'tallyMapBoth', 'syncState']
# no header needed (Companion module, reads, SSE, static files)
OPEN = [
    ('ccu', 'GET', '/?cameraId=1&gain_db=2', None, {}),
    ('loadPreset', 'GET', '/?loadPreset=1,0', None, {}),
    ('savePreset', 'POST', '/savePreset', 'cameraId=1&presetId=1&name=X&data=gain_db%3A6%3B', FORM),
    ('listPresets', 'GET', '/?listPresets', None, {}),
    ('vmixConnect', 'GET', '/?vmixConnect=1', None, {}),
    ('getOverrides', 'GET', '/?getOverrides', None, {}),
    ('getTallyMap', 'GET', '/?getTallyMap', None, {}),
    ('getVmixConnect', 'GET', '/?getVmixConnect', None, {}),
    ('getParams', 'GET', '/?getParams', None, {}),
    ('listFiles', 'GET', '/?listFiles', None, {}),
    ('download', 'GET', '/?download=tally.html', None, {}),
    ('fileExists', 'GET', '/?fileExists=tally.html', None, {}),
    ('safemode-status', 'GET', '/safemode-status', None, {}),
    ('index', 'GET', '/', None, {}),
    ('tally.html', 'GET', '/tally.html', None, {}),
]

def req(item, headers=None):
    name, method, path, body, extra = item
    h = dict(extra); h.update(headers or {})
    return http(path, method=method, body=body, headers=h, timeout=10)

res = {'protected': {}, 'with_header': {}, 'open': {}, 'cors_headers': [], 'wdt': [], 'alive': None}
vm = FakeVmix(); emu = Emu('csrf', prep=prep)
assert emu.wait_http(); time.sleep(1.5)
http('/?overrideCCU=1&overrideTally=1', headers=CSRF)
setup_tally(); time.sleep(0.5)
t0 = time.time()
for item in PROTECTED:
    r = req(item)
    res['protected'][item[0]] = [r['status'], r['body'][:40].decode('latin1').strip()]
    if 'access-control-allow-origin' in r['headers'].lower(): res['cors_headers'].append(item[0])
time.sleep(0.5)
rebooted = bool(emu.wdt())
if rebooted: emu.wait_http(); time.sleep(1.5)
sd = emu.work + '/sd'
res['nothing_applied'] = {
    'del.txt_kept': os.path.exists(sd + '/del.txt'),
    'ren.txt_kept': os.path.exists(sd + '/ren.txt') and not os.path.exists(sd + '/ren2.txt'),
    'no_upload': not os.path.exists(sd + '/up.txt'),
    'no_cam7_write': not [b for b in emu.bmd() if b['wt'] / 1000 >= t0 and b.get('cam') == 7
                          and b.get('fn', '').startswith('writeCommand')],
    'overrides': http('/?getOverrides')['body'][:120].decode('latin1').strip(),
    'tally_map': http('/?getTallyMap')['body'][:200].decode('latin1').strip(),
    'no_restart': not rebooted,
}
for item in PROTECTED:
    if item[0] in WITH_HEADER:
        r = req(item, CSRF)
        res['with_header'][item[0]] = [r['status'], r['body'][:60].decode('latin1').strip()]
res['applied_with_header'] = {
    'del.txt_gone': not os.path.exists(sd + '/del.txt'),
    'renamed': os.path.exists(sd + '/ren2.txt'),
    'uploaded': os.path.exists(sd + '/up.txt'),
}
http('/?overrideCCU=1&overrideTally=1', headers=CSRF)
for item in OPEN:
    r = req(item)
    res['open'][item[0]] = [r['status'], r['body'][:40].decode('latin1').strip()]
    if 'access-control-allow-origin' in r['headers'].lower(): res['cors_headers'].append(item[0])
try:
    sse = Sse(); time.sleep(1.0)
    head = sse.text().split('\r\n\r\n', 1)[0]
    sse.close()
except OSError as e:
    head = 'error %s' % e
res['events'] = head.split('\r\n')[0]
if 'access-control-allow-origin' in head.lower(): res['cors_headers'].append('events')
r = http('/', method='OPTIONS', headers={'Origin': 'http://evil.example', 'Access-Control-Request-Method': 'GET',
                                          'Access-Control-Request-Headers': 'x-tallyccu'})
res['options'] = [r['status'], 'access-control-allow' in r['headers'].lower()]
res['wdt'] = emu.wdt()[1:] if rebooted else emu.wdt()  # the refused /reboot is reported above
res['alive'] = emu.alive()
emu.stop(); vm.close()
print(json.dumps(res, indent=1))
