#!/usr/bin/env python3
"""Runs the robustness scenarios against the emulator and checks their results.

  tests/scenarios/run.py --fast            the quick set (CI, about 6 minutes)
  tests/scenarios/run.py --all             every scenario (about 20 minutes; iptables ones need root)
  tests/scenarios/run.py misc k csrf       by name
  tests/scenarios/run.py --list

Options: --out DIR (results, default tests/out/scenarios), --strict (a scenario skipped for
lack of root/iptables counts as a failure). Configuration through the environment, see lib.py
(EMU_BIN, T_IP, T_VMIX, T_PEER_PREFIX...). Build the emulator first: tests/emulator/build.sh.

Each scenario prints a JSON result; the checks below turn it into PASS/FAIL. Every scenario
except 'j' and 'boot' (Safe Mode and reset causes, which reset on purpose) fails on any emulated
watchdog reset; 'sm' counts its intentional restarts itself.
"""
import argparse, json, os, shutil, signal, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
import lib  # noqa: E402  (configuration only)


def stop_writes(writes, n=2):
    """[(t, [v...])] -> times of writes whose first n values are all zero."""
    return [w[0] for w in writes if all(x == 0 for x in list(w[1])[:n])]


def tally_ok(t, out, tag='tally'):
    if not t or t.get('n', 0) == 0:
        out.append('%s: no tally pushes measured' % tag)
        return
    if t['missing']:
        out.append('%s: %d of %d vMix changes never reached the camera' % (tag, t['missing'], t['n']))
    if t['max'] is not None and t['max'] > 0.5:
        out.append('%s: max latency %.3f s > 0.5 s' % (tag, t['max']))


def expect(out, cond, msg):
    if not cond:
        out.append(msg)


def c_csrf(r):
    o = []
    for k, (st, body) in r['protected'].items():
        expect(o, st == 403 and body.startswith('ERROR forbidden'), 'no header: %s -> %s %r (want 403 ERROR forbidden)' % (k, st, body))
    for k, v in r['nothing_applied'].items():
        if isinstance(v, bool):
            expect(o, v, 'refused request had an effect: %s' % k)
    for k, (st, body) in r['with_header'].items():
        expect(o, st == 200, 'with header: %s -> %s %r' % (k, st, body))
    for k, v in r['applied_with_header'].items():
        expect(o, v, 'with header, not applied: %s' % k)
    for k, (st, body) in r['open'].items():
        expect(o, st == 200, 'no header needed: %s -> %s %r' % (k, st, body))
    expect(o, ' 200' in r['events'], '/events: %r' % r['events'])
    expect(o, not r['cors_headers'], 'Access-Control-Allow-Origin sent by: %s' % r['cors_headers'])
    st, cors = r['options']
    expect(o, not (st and 200 <= st < 300) and not cors, 'OPTIONS answered %s (CORS headers: %s)' % (st, cors))
    return o


def c_misc(r):
    o = []
    for p in ('/favicon.ico', '/adir', '/nothere.js'):
        expect(o, r['nresp ' + p] == 1, '%s: %d responses' % (p, r['nresp ' + p]))
    expect(o, r['listed'] == ['foo.txt', 'foo.txt.gz'], 'listFiles shows system pages or their .gz: %s' % r['listed'])
    expect(o, r['gz_served_before'] == 1, 'foo.txt.gz not served before the upload')
    expect(o, '"success":true' in r['upload'], 'upload: %r' % r['upload'])
    expect(o, r['after_upload_served'] == 'UPLOADED-PLAIN', 'stale .gz after upload: %r' % r['after_upload_served'])
    expect(o, not r['gz_still_on_sd'], 'stale foo.txt.gz left on the SD')
    expect(o, r['lone_zero'] == 400, 'tallyCam0Input=0 alone -> %s (want 400)' % r['lone_zero'])
    expect(o, r['both_zero'] == 200, 'slot clear -> %s' % r['both_zero'])
    expect(o, r['vmix_off_tally_writes'][-1:] == [[0, 0]], 'vmixConnect=0 left the tally on: %s' % r['vmix_off_tally_writes'])
    expect(o, r['vmix_back'] == [[1, 0]], 'vmixConnect=1 did not restore the tally: %s' % r['vmix_back'])
    return o


def c_k(r):
    o = []
    expect(o, '"success":true' in r['save_ok'], 'first save: %r' % r['save_ok'])
    for m in ('close', 'rst', 'stall'):
        expect(o, r[m]['file_unchanged'], '%s: previous preset lost' % m)
        expect(o, not r[m]['leftover_tmp'], '%s: temporary file left' % m)
    expect(o, r['list_name'], 'listPresets shows the interrupted name')
    expect(o, '"success":true' in r['load'], 'load: %r' % r['load'])
    expect(o, r['applied_gain'] == [[6]], 'loaded gain: %s' % r['applied_gain'])
    return o


def c_fh(r):
    o = []
    for x in r['f']:
        expect(o, x['applied'], 'split TALLY line %s (cut at %d) not applied: %s' % (x['state'], x['split_at'], x['writes']))
    s = stop_writes(r['h_web_once']['writes'])
    expect(o, s and 0.2 <= s[0] <= 2.0, 'deadman=1 without refresh: stop at %s' % s)
    expect(o, len(r['h_web_refresh']['stops']) == 1, 'deadman refreshed: stops %s' % r['h_web_refresh']['stops'])
    expect(o, [w for w in r['h_zoom'][1:] if w[1] == 0], 'zoom deadman: no stop %s' % r['h_zoom'])
    comp = [w for w in r['h_companion_style'] if all(x == 0 for x in (w[2][:2] if w[1] == 11 else w[2][:1]))]
    expect(o, not comp, 'Companion-style PTZ (no deadman) was stopped: %s' % comp)
    expect(o, not stop_writes(r['h_disarm']), 'plain command did not disarm the deadman: %s' % r['h_disarm'])
    expect(o, not stop_writes(r['h_deadman0']['w']), 'deadman=0 stopped: %s' % r['h_deadman0'])
    return o


def c_i(r):
    o = []
    for tag in ('absent_at_boot', 'pulled'):
        x = r[tag]
        expect(o, x['loadPreset'][0] == 500, '%s: loadPreset %s' % (tag, x['loadPreset']))
        expect(o, x['index'][0] == 503, '%s: index %s' % (tag, x['index']))
        expect(o, x['ccu'][0] == 200 and x['ccu_echo'], '%s: CCU command or echo failed' % tag)
        expect(o, x['tally_applied'], '%s: tally not applied' % tag)
    for tag in ('present', 'reinserted'):
        x = r[tag]
        for k in ('loadPreset', 'savePreset', 'index', 'ccu'):
            expect(o, x[k][0] == 200, '%s: %s %s' % (tag, k, x[k]))
        expect(o, x['tally_applied'], '%s: tally not applied' % tag)
    expect(o, not r['absent_wdt'], 'watchdog with the SD absent: %s' % r['absent_wdt'])
    return o


def c_d(r):
    o = []
    expect(o, r['idle_not_closed'] == 0, '%d idle connections never closed' % r['idle_not_closed'])
    expect(o, r['web_ok_after_s'] is not None, 'web never answered with 8 idle connections')
    expect(o, r['existing_companion_pong'] and not r['existing_companion_closed'], 'existing Companion link lost')
    expect(o, 'SUBSCRIBED OK' in r['new_companion_lines'], 'new Companion: %s' % r['new_companion_lines'])
    expect(o, r['ccu_cmd'] == 200 and all(r['ccu_echo_to_companions']), 'CCU echo: %s' % r['ccu_echo_to_companions'])
    tally_ok(r['tally'], o)
    return o


def c_g(r):
    o = []
    expect(o, r['events_a'] >= 10 and r['events_b'] >= 10, 'SSE events a=%d b=%d (want >= 10 each)' % (r['events_a'], r['events_b']))
    expect(o, len(r['requestsync_times']) >= 2, 'REQUESTSYNC times %s' % r['requestsync_times'])
    expect(o, r['min_gap_s'] is None or r['min_gap_s'] >= 9.5, 'REQUESTSYNC more often than every 10 s: %s' % r['requestsync_times'])
    expect(o, r['sse_replaced'] == 0, 'SSE client evicted while only two were open')
    expect(o, r['a_still_open'] and r['b_still_open'], 'an SSE client was closed')
    t = r['third_tab']
    expect(o, t['a_closed'] and not t['b_closed'] and not t['c_closed'], 'third tab did not replace the oldest: %s' % t)
    return o


def c_b2(r):
    o = []
    for case in ('vanish', 'release'):
        s = stop_writes(r['writes_' + case])
        expect(o, s and s[0] < 5, 'deadman during a slow transfer (%s): stop at %s' % (case, s))
    return o


def c_b(r):
    o = []
    expect(o, r['dm_status'] == 200, 'deadman request: %s' % r['dm_status'])
    expect(o, all(st == 200 for st, _ in r['requests_during']), 'requests during the slow transfer: %s' % r['requests_during'])
    expect(o, stop_writes(r['deadman_writes_rel_arm']), 'deadman never stopped: %s' % r['deadman_writes_rel_arm'])
    tally_ok(r['tally'], o)
    return o


def c_a(r):
    o = []
    expect(o, all(l['status'] == 200 for l in r['loads']), 'preset loads: %s' % [l['status'] for l in r['loads']])
    expect(o, r['good_sse_events'] > 0 and not r['good_closed'], 'healthy SSE client starved or closed')
    expect(o, r['web_after'] == 200, 'web after: %s' % r['web_after'])
    tally_ok(r['tally'], o)
    return o


def c_c(r):
    o = []
    expect(o, r['dump_keys_seen_in_sse'] == r['dump_keys'], 'CCUSYNC dump: %d of %d keys reached SSE' % (r['dump_keys_seen_in_sse'], r['dump_keys']))
    expect(o, not r['comp_closed'] and r['pong_after'], 'Companion link lost after the cut')
    tally_ok(r['tally'], o)
    return o


def c_e(r):
    o = []
    expect(o, r['tally_off_after_s'] is not None and r['tally_off_after_s'] < 30, 'tally not cleared with vMix half-open: %s' % r['tally_off_after_s'])
    expect(o, r['reconnect_after_restore_s'] is not None, 'no reconnect to vMix')
    expect(o, r['tally_back_on_after_restore_s'] is not None, 'tally not back after the reconnect')
    expect(o, r['web_ok'] == 200, 'web: %s' % r['web_ok'])
    return o


def c_e2(r):
    o = []
    for k in ('baseline', 'vmix_halfopen_30s', 'vmix_unreachable_40s'):
        x = r[k]
        expect(o, x['http_err'] == 0, '%s: %d HTTP errors' % (k, x['http_err']))
        expect(o, x['http_max_ms'] < 2000 and x['pong_max_ms'] < 2000, '%s: http max %d ms, pong max %d ms' % (k, x['http_max_ms'], x['pong_max_ms']))
    expect(o, not r['wdt_a'] and not r['wdt_b'], 'watchdog: %s %s' % (r['wdt_a'], r['wdt_b']))
    return o


def c_j(r):
    o = []
    expect(o, r['A_safe_mode_after_s'] is not None, 'never entered Safe Mode without the shield')
    expect(o, r['A_cmd_in_safe'] == [503, 200], 'Safe Mode answers: %s (want CCU 503, listPresets 200)' % r['A_cmd_in_safe'])
    expect(o, r['A_recover_msg_after_s'] is not None and r['A_boot_completed_after_s'] is not None, 'no automatic recovery')
    expect(o, r['A_normal_cmd'] == 200, 'after recovery: %s' % r['A_normal_cmd'])
    expect(o, r['B_back_in_safe_after_s'] is not None, 'failed recovery did not return to Safe Mode')
    expect(o, r['B_no_quick_recovery_60s'], 'recovery loop: retried within 60 s')
    expect(o, r['C_no_recovery_40s'], 'recovered with the strikes exhausted')
    return o


def c_boot(r):
    o = []

    def only(key, word, n=1):
        v = r[key]
        expect(o, len(v) == n and all(word in x for x in v), '%s: %s (want %d x %r)' % (key, v, n, word))
    expect(o, r['0_map'] == 200, 'tally map: %s' % r['0_map'])
    only('1_poweron', 'Power On')
    only('2_restart', 'Restart')
    only('3_external', 'External')
    only('4_hang_in_operation', 'in operation')
    expect(o, not r['4_boot_failure'], 'a hang in operation was counted as a boot failure')
    expect(o, r['4_wdt_interrupt'], 'the watchdog interrupt did not run before the reset')
    only('5_three_external', 'External', 3)
    expect(o, not r['5_safe_mode'], 'Safe Mode after 3 reset-pin resets in the boot window')
    only('6_three_boot_hangs', 'during boot', 3)
    expect(o, r['6_counts'] == ['Boot failure detected! Count: %d' % i for i in (1, 2, 3)], 'counts: %s' % r['6_counts'])
    expect(o, r['6_safe_mode'] and '"safeMode":true' in r['6_safemode_status'], 'no Safe Mode after 3 boot hangs: %s' % r['6_safemode_status'])
    only('7_after_power_cycle', 'Power On')
    expect(o, not r['7_safe_mode'], 'still in Safe Mode after a power cycle')
    return o


def c_fails(r):
    """Scenarios that check themselves: r['fails'] names the failed checks, r[name] the details."""
    return ['%s: %s' % (k, r.get(k)) for k in r['fails']]


# Scenarios that reset by watchdog on purpose
RESETS_EXPECTED = ('j', 'boot')

# name: (script, args, needs iptables/root, fast, timeout s, check)
SCENARIOS = {
    'csrf': ('s_csrf.py', [], False, True, 120, c_csrf),
    'misc': ('s_misc.py', [], False, True, 120, c_misc),
    'k': ('s_k.py', [], False, True, 120, c_k),
    'fh': ('s_fh.py', [], False, True, 150, c_fh),
    'i': ('s_i.py', [], False, True, 150, c_i),
    'd': ('s_d.py', [], False, True, 150, c_d),
    'g': ('s_g.py', [], False, True, 150, c_g),
    'b': ('s_b.py', [], False, False, 200, c_b),
    'bgz': ('s_b.py', ['gz'], False, False, 200, c_b),
    'b2': ('s_b2.py', [], False, False, 200, c_b2),
    'a': ('s_a.py', [], True, False, 200, c_a),
    'c': ('s_c.py', [], True, False, 200, c_c),
    'e': ('s_e.py', [], True, False, 200, c_e),
    'e2': ('s_e2.py', [], True, False, 300, c_e2),
    'j': ('s_j.py', [], False, False, 600, c_j),
    'boot': ('s_boot.py', [], False, True, 400, c_boot),
    'coop': ('s_coop.py', [], False, True, 300, c_fails),
    'coopgz': ('s_coop.py', ['gz'], False, False, 300, c_fails),
    'coop2': ('s_coop2.py', [], False, True, 200, c_fails),
    'upl': ('s_upl.py', [], False, False, 150, c_fails),
    'sm': ('s_sm.py', [], False, True, 200, c_fails),
}


def can_iptables():
    return hasattr(os, 'geteuid') and os.geteuid() == 0 and shutil.which('iptables') is not None


def port80_ok():
    if hasattr(os, 'geteuid') and os.geteuid() == 0:
        return True
    try:
        return int(open('/proc/sys/net/ipv4/ip_unprivileged_port_start').read()) <= 80
    except (OSError, ValueError):
        return False


def parse_json(text):
    try:
        return json.loads(text)
    except ValueError:
        i = text.find('\n{')
        return json.loads(text[i + 1:]) if i >= 0 else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('names', nargs='*')
    ap.add_argument('--fast', action='store_true')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--strict', action='store_true')
    ap.add_argument('--out', default=os.path.join(lib.ROOT, 'tests', 'out', 'scenarios'))
    a = ap.parse_args()
    if a.list:
        for n, (f, args, root, fast, to, _) in SCENARIOS.items():
            doc = open(os.path.join(HERE, f)).read().split('"""')[1].strip().split('\n')[0]
            print('%-5s %-5s %-5s %s' % (n, 'fast' if fast else '', 'root' if root else '', doc))
        return 0
    names = list(SCENARIOS) if a.all else [n for n in SCENARIOS if SCENARIOS[n][3]] if a.fast else a.names
    unknown = [n for n in names if n not in SCENARIOS]
    if not names or unknown:
        ap.error('unknown scenario(s) %s; use --fast, --all or --list' % unknown if unknown else 'nothing selected')
    if not os.access(lib.BIN, os.X_OK):
        print('emulator binary not found: %s (run tests/emulator/build.sh)' % lib.BIN)
        return 2
    if not port80_ok():
        print('cannot bind port 80: run as root or set net.ipv4.ip_unprivileged_port_start=0')
        return 2
    os.makedirs(os.path.join(a.out, 'runs'), exist_ok=True)
    env = dict(os.environ, T_RUNS=os.path.join(a.out, 'runs'), EMU_BIN=lib.BIN, PYTHONDONTWRITEBYTECODE='1')
    summary = {}
    print('emulator %s, firmware %s, vMix %s' % (lib.BIN, lib.IP, lib.VMIX))
    for n in names:
        f, args, root, fast, timeout, check = SCENARIOS[n]
        if root and not can_iptables():
            summary[n] = {'result': 'SKIP', 'why': 'needs root and iptables'}
            print('SKIP  %-5s (needs root and iptables)' % n)
            continue
        t0 = time.time()
        # own process group: the emulator it starts dies with it, whatever happens
        p = subprocess.Popen([sys.executable, '-u', os.path.join(HERE, f)] + args, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, start_new_session=True)
        try:
            out, err = p.communicate(timeout=timeout)
            rc = p.returncode
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGTERM)  # lib.py removes its iptables rules on SIGTERM
            try:
                out, err = p.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                out, err = p.communicate()
            err, rc = (err or '') + '\ntimeout after %d s' % timeout, -1
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        lib.flush_drops()
        dt = round(time.time() - t0, 1)
        open(os.path.join(a.out, n + '.json'), 'w').write(out)
        open(os.path.join(a.out, n + '.err'), 'w').write(err)
        fails = []
        res = parse_json(out) if rc == 0 else None
        if res is None:
            fails.append('scenario did not finish (rc %s): %s' % (rc, err.strip().splitlines()[-1:] if err.strip() else ''))
        else:
            try:
                fails += check(res)
            except (KeyError, TypeError, IndexError) as e:
                fails.append('unexpected result shape: %r' % e)
            if n not in RESETS_EXPECTED:
                for k, v in res.items():
                    if k.startswith('wdt') and v:
                        fails.append('emulated watchdog reset: %s' % v)
                if res.get('alive') is False:
                    fails.append('emulator process died')
        summary[n] = {'result': 'FAIL' if fails else 'PASS', 's': dt, 'failures': fails}
        print('%s  %-5s %6.1f s' % ('FAIL' if fails else 'PASS', n, dt))
        for x in fails:
            print('        - ' + x)
        sys.stdout.flush()
    json.dump(summary, open(os.path.join(a.out, 'summary.json'), 'w'), indent=1)
    bad = [n for n, s in summary.items() if s['result'] == 'FAIL' or (a.strict and s['result'] == 'SKIP')]
    print('%d passed, %d failed, %d skipped (results in %s)' % (
        sum(s['result'] == 'PASS' for s in summary.values()), sum(s['result'] == 'FAIL' for s in summary.values()),
        sum(s['result'] == 'SKIP' for s in summary.values()), a.out))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
