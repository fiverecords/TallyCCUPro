"""(sm) Built-in Safe Mode page (BOOT-13): in Safe Mode, / and /ccu answer a page from flash when
safemode.html cannot be read (file missing, SD failing), the SD page again once it is back, and its
exit request restarts in normal mode. Outside Safe Mode a failing SD still gives 503."""
import sys, json, time, os, shutil, tempfile; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *
sys.path.insert(0, os.path.join(ROOT, 'tests', 'ci'))
import check_pages

BUILTIN = b'Built-in page: safemode.html could not be read'
res = {'fails': []}


def check(name, ok, detail):
    res[name] = detail
    if not ok:
        res['fails'].append(name)


def page(path, **kw):
    r = http(path, **kw)
    return {'status': r['status'], 'builtin': BUILTIN in r['body'],
            'sd_page': b'<title>TallyCCU Pro - Safe Mode</title>' in r['body'] and BUILTIN not in r['body'],
            'html': 'Content-Type: text/html' in r['headers'], 'no_store': 'Cache-Control: no-store' in r['headers'],
            'gzip': 'Content-Encoding: gzip' in r['headers'], 'button': b"fetch('/safemode-exit'" in r['body'],
            'firmware': b'Firmware: ' in r['body'], 'len': len(r['body'])}


def wait_status(want, timeout=40):
    # a real answer, not only a TCP connect: right after a connect the single listening
    # socket on port 80 is not open again yet and a second connection would be reset
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = http('/safemode-status', timeout=2)
        if r['status'] == 200 and want in r['body']:
            return True
        time.sleep(0.3)
    return False


def enter_safe_mode(emu):
    m = emu.mark(); emu.send('safemode')
    ok = emu.wait('SAFE MODE ACTIVE', m, 40) >= 0 and emu.wait('System ready.', m, 40) >= 0
    return ok and wait_status(b'"safeMode":true')


def builtin_ok(p):
    return p['status'] == 200 and p['builtin'] and p['html'] and p['no_store'] and not p['gzip'] and p['button'] and p['firmware']


vm = FakeVmix()

# A) SD fine but safemode.html missing
emu = Emu('smA', serial=True, prep=lambda sd: os.remove(os.path.join(sd, 'safemode.html')))
emu.wait('System ready.'); wait_status(b'"safeMode":false')
check('A_safe_mode', enter_safe_mode(emu), 'entered')
p = page('/'); check('A_root', builtin_ok(p), p)
# the page's exit button: its script parses (node --check) and sends what the firmware wants
body = http('/')['body']
check('A_button_request', b"method:'POST'" in body and b"'X-TallyCCU':'1'" in body and b"location.replace('/')" in body,
      'POST, CSRF header, then the main page')
if shutil.which('node'):
    with tempfile.TemporaryDirectory() as tmp:
        f = os.path.join(tmp, 'builtin.html'); open(f, 'wb').write(body)
        errors, n, _ = check_pages.check(f, tmp)
    check('A_script_syntax', n == 1 and not errors, errors or n)
else:
    res['A_script_syntax'] = 'skipped: no node'
p = page('/ccu'); check('A_ccu', builtin_ok(p), p)
p = page('/', headers={'Accept-Encoding': 'gzip'}); check('A_root_gzip', builtin_ok(p), p)
p = page('/index.html'); check('A_index', builtin_ok(p), p)
st = http('/safemode-status'); check('A_status', b'"safeMode":true' in st['body'], st['body'][:120].decode('latin1'))
# the page from the SD again once it is there
shutil.copy(os.path.join(SDSRC, 'safemode.html'), emu.work + '/sd/')
p = page('/'); check('A_sd_page_back', p['status'] == 200 and p['sd_page'], p)
os.remove(emu.work + '/sd/safemode.html')
# what the page's button sends: POST /safemode-exit with the CSRF header
m = emu.mark()
r = http('/safemode-exit', method='POST', body='', headers=CSRF)
check('A_exit', r['status'] == 200 and b'"success":true' in r['body'], r['body'][:80].decode('latin1'))
ok = emu.wait('Boot completed successfully', m, 60) >= 0 and wait_status(b'"safeMode":false')
check('A_normal_after_exit', ok, ok)
p = page('/'); check('A_index_after_exit', p['status'] == 200 and not p['builtin'], p)
# only the two intentional restarts (into Safe Mode, out of it)
n = len(emu.wdt()); check('A_resets', n == 2, n)
res['alive'] = emu.alive()
emu.stop()

# B) SD card failing: normal mode still answers 503, Safe Mode the built-in page
emu = Emu('smB', serial=True, env={'EMU_SD_FAIL': '1'})
emu.wait('System ready.'); wait_status(b'"safeMode":false')
r = http('/'); check('B_normal_root_503', r['status'] == 503, r['status'])
check('B_safe_mode', enter_safe_mode(emu), 'entered')
p = page('/'); check('B_root', builtin_ok(p), p)
time.sleep(11)   # a remount is due again (SD_REMOUNT_MS): the page still comes, after the retry
p = page('/'); check('B_root_after_remount', builtin_ok(p), p)
check('B_remount_tried', emu.serial().count('SD remount') >= 1, emu.serial().count('SD remount'))
n = len(emu.wdt()); check('B_resets', n == 1, n)
res['alive'] = res['alive'] and emu.alive()
emu.stop(); vm.close()
print(json.dumps(res, indent=1))
