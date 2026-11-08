# Companion compatibility tests

The firmware must keep working with the **official** Bitfocus Companion module
(`companion-module-fiverecords-tallyccupro` v3.1.0), which cannot be changed. These tools run
that module, unmodified and with its real `@companion-module/base` 1.14.1, against the firmware
(emulated) and record everything it sends and receives.

```
tests/companion/
├── fetch_module.sh      clones the module at a pinned commit into module/ and installs its deps
├── driver/              harness: runs the module as Companion's module host does
│   ├── driver.js        createHarness() -> h.run(action, options), h.http[], h.tcp[], ...
│   ├── preload.js       injected into the module process: records fetch() and the 8098 socket
│   ├── gen-cases.js     writes driver/cases.json (every action of the module, 1473 cases)
│   ├── run-cases.js     runs cases.json against any host -> results file
│   ├── compare-results.js   compares two run-cases.js results
│   ├── mock-server.js   trivial HTTP :80 + TCP :8098 server, to test the harness itself
│   └── test-mock.js     harness self-test against the mock
└── diff/                differential runs against the emulator
    ├── gen-static.js    writes diff/static-cases.json (34 scenario cases: Safe Mode, network cuts, vMix...)
    ├── runner.js        one emulated firmware + fake vMix, runs the plans through the module
    ├── fake-vmix.js     minimal vMix TCP API (port 8099)
    ├── compare.js       joins two runs with the emulator logs, case by case -> diff.json
    ├── report.js        summary of diff.json (Markdown)
    └── show.js          prints the differences of selected cases
```

`module/`, `cases.json` and `static-cases.json` are generated, not versioned. Needs Node 22,
git and npm; the emulator needs port 80 (root or
`sudo sysctl -w net.ipv4.ip_unprivileged_port_start=0`).

## Setup

```bash
tests/companion/fetch_module.sh             # module v3.1.0 at commit c35b1e7, base 1.14.1
node tests/companion/driver/gen-cases.js    # -> driver/cases.json
node tests/companion/diff/gen-static.js     # -> diff/static-cases.json
MOCK_HOST=127.0.82.1 node tests/companion/driver/test-mock.js --quick   # harness self-test (~3 s)
tests/emulator/build.sh                     # -> tests/out/emu/bin/emu
```

## Run the module against the current firmware

```bash
node tests/companion/diff/runner.js --ip 127.0.82.2 --vmix 127.0.82.12 --out tests/out/companion/run-new \
     [--suite REGEX] [--case REGEX] [--tier core,extended,robustness] [--exclude-tags slow]
```

Every case runs one or more module actions (or harness steps: web requests, SSE, vMix tally,
serial commands, network cuts) in a fresh module instance per suite. The runner exits with 1 if
a case **fails** (an action throws, an awaited TCP line never comes, the module dies) or if the
emulator resets by watchdog outside the cases that reboot on purpose. `warn` (HTTP 4xx/5xx, a
module warning) is expected in some cases: a preset that does not exist, a 403 path...
The `static:netcut-*` cases cut the network with iptables and need root
(`sudo -E env "PATH=$PATH" node ...`). The full plan (1507 cases) takes about 10 minutes.

## Compare with an older firmware

Build both versions, run the same plan on each (in parallel, on different addresses), then compare:

```bash
REF=8d554d5                                  # v3.7.1; any commit, tag or branch
tests/emulator/build.sh --ref $REF --name emu-ref
tests/emulator/build.sh --name emu-new
node tests/companion/diff/runner.js --which ref --bin tests/out/emu/bin/emu-ref --sd-ref $REF \
     --ip 127.0.82.2 --vmix 127.0.82.12 --out tests/out/companion/run-ref &
node tests/companion/diff/runner.js --which new --bin tests/out/emu/bin/emu-new \
     --ip 127.0.82.3 --vmix 127.0.82.13 --out tests/out/companion/run-new &
wait
node tests/companion/diff/compare.js tests/out/companion/run-ref tests/out/companion/run-new tests/out/companion/diff.json
node tests/companion/diff/report.js tests/out/companion/diff.json --md tests/out/companion/report.md
node tests/companion/diff/show.js tests/out/companion/diff.json '^preset:load-existing' 'bmd|responseText'
```

`--sd-ref` serves that commit's `sdcard/*.html`, so each firmware gets its own pages. The
comparison is per case and per aspect: the module's requests, HTTP status and body, TCP lines
in both directions, module logs, variables and feedbacks, and from the emulator the BMD commands
that reached the camera, SD operations, network problems and watchdog/boot events. A difference
is not a verdict: classify each group (identical, harmless, expected improvement, regression)
by hand with `show.js`. The same comparison runs on GitHub Actions from the CI workflow
(*Run workflow*, `companion_diff`, `reference`).

## Harness API (driver.js)

```js
const { createHarness } = require('./driver/driver.js')
const h = await createHarness({ host: '127.0.82.2' })   // fork + register + init, waits for SUBSCRIBED OK
const r = await h.run('set_gain_db', { cameraId: 1, value: 6 })
// r = { success, errorMessage, http:[...], tcp:[...], logs:[...], variables:{...} } for this action only
await h.externalHttp('/?cameraId=1&gain_db=10')          // as the web page (not recorded as module traffic)
await h.waitForTcp('CCU 1 gain_db 10', { dir: 'rx' })
await h.close()
```

Other helpers: `h.mark()`/`h.since(mark)`, `h.injectTcpRx(line)`, `h.dropTcp()`,
`h.externalSse('/events')`, `h.addFeedback(id, options)`, `h.updateConfig({host})`.
`run(action, options)` fills unspecified options with the action's defaults, as a new button in
Companion does. The harness does not clamp values to min/max, so out-of-range cases (tier
`robustness`) send what Companion's UI would not.

## What the module does that the firmware must accept

Seen in the module's real requests:
- one parameter per GET: `/?cameraId=N&<key>=<value>`, no custom headers, no `Origin`;
  `/?loadPreset=c,p`, `POST /savePreset` (form body, names with `encodeURIComponent`),
  `/?listPresets`, `/?vmixConnect=1|0`; `change_camera` repeats the key (`/?cameraId=8&cameraId=8`);
- unrounded floats from increments (`0.30000000000000004`, `-7.700000000000001`), arrays as
  `a%2Cb`, `exposure_and_focus_tools=%2C` for its defaults, booleans `1`/`0` (and `true`/`false`
  from defaults inside presets and `CCUSYNC`);
- `load_preset` succeeds if the response **contains** `OK` (or is JSON with `success:true`);
- TCP 8098: `SUBSCRIBE CCU` on connect, `PING` every 30 s, reconnect after 5 s; it answers
  `REQUESTSYNC` with one `CCUSYNC <cam> <key> <value>` per cached value (about 45 per camera
  after a preset save);
- PTZ: the speed once, then `0,0` to stop; no deadman refresh.
