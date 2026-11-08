'use strict'
// Cases derived from the static analysis (one suite per scenario, fresh module instance each).
const fs = require('fs')
const cases = []
let n = 0
const A = (action, options, extra = {}) => ({ do: 'action', action, options, ...extra })
function seq(suite, id, steps, notes, tags) {
	cases.push({ id: `static:${id}`, suite: `static:${suite}`, kind: 'sequence', mode: 'static', tier: 'core', action: null, options: null, steps, notes, tags })
	n++
}
const defGroups = { includeGroupAudio: false, includeGroupColorCorrection: true, includeGroupDisplay: false, includeGroupLens: true, includeGroupOutput: false, includeGroupPtzControl: false, includeGroupReference: false, includeGroupTally: false, includeGroupVideo: true }
const save = (cam, p, name, extra = {}) => A('save_current_as_preset', { cameraId: cam, presetId: p, presetName: name, ...defGroups, ...extra })

// 1 exposure_and_focus_tools with empty text fields
seq('eft', 'eft-empty-empty', [A('set_exposure_and_focus_tools', { cameraId: 1, value0: '', value1: '' })], "value0='' value1='' -> %2C")
seq('eft', 'eft-1-empty', [A('set_exposure_and_focus_tools', { cameraId: 1, value0: '1', value1: '' })], "'1,'")
seq('eft', 'eft-empty-4', [A('set_exposure_and_focus_tools', { cameraId: 1, value0: '', value1: '4' })], "',4'")
seq('eft', 'eft-1-4', [A('set_exposure_and_focus_tools', { cameraId: 1, value0: '1', value1: '4' })], "'1,4'")
// 2 exponent in a list component (CCU-01)
seq('contrast-exp', 'contrast-dec-x10', Array.from({ length: 10 }, () => A('set_contrast_adjust_1_decrement', { cameraId: 1, decrement: 0.1 })), '10x Decrease Contrast -> 1.3877787807814457e-16')
seq('contrast-exp', 'contrast-exp-web', [{ do: 'externalHttp', path: '/?cameraId=1&contrast_adjust=0.5%2C1.3877787807814457e-16' }, { do: 'waitTcp', dir: 'rx', match: 'CCU 1 contrast_adjust', timeout: 2000, optional: true }], 'direct GET with exponent')
// 3 FIXED16 at 16.0
seq('gain16', 'gain-adjust-16', [A('set_gain_adjust_0', { cameraId: 1, value: 16 })], 'gain_adjust=16,1,1,1')
seq('gain16', 'gain-adjust-16-web', [{ do: 'externalHttp', path: '/?cameraId=1&gain_adjust=16%2C1%2C1%2C1' }], 'direct GET')
// 4 VOID from the web: echo to the module
seq('void-web', 'void-web-auto-wb', [{ do: 'externalHttp', path: '/?cameraId=1&set_auto_wb=1' }, { do: 'waitTcp', dir: 'rx', match: 'CCU 1 set_auto_wb', timeout: 1500, optional: true }], 'web presses Auto WB')
seq('void-web', 'void-module-auto-wb', [A('set_set_auto_wb', { cameraId: 1 })], 'module presses Auto WB')
// 5 OIS true in a Companion preset (CCU-04)
seq('ois-preset', 'ois-save-load', [save(1, 0, 'OIS test'), A('load_preset', { cameraId: 1, presetId: 0 }), { do: 'feedback', feedback: 'ois_enabled', options: { cameraId: 1 } }], 'save defaults (OIS:true) then load')
// 6 VOID lines inside a preset (CCU-02)
seq('void-preset', 'void-save-load', [A('set_set_auto_wb', { cameraId: 1 }), A('set_correction_reset_default', { cameraId: 1 }), A('set_lift_adjust_0', { cameraId: 1, value: -0.5 }), save(1, 1, 'VOID test'), A('load_preset', { cameraId: 1, presetId: 1 })], 'Auto WB + reset + lift, save, load')
// 7 video_mode in a preset
seq('video-mode-preset', 'video-mode-deliberate', [A('set_video_mode', { cameraId: 2, value0: 25, value1: 0, value2: 3, value3: 0, value4: 0 }), save(2, 0, 'Formato 25p'), A('set_video_mode', { cameraId: 2, value0: 50, value1: 0, value2: 3, value3: 0, value4: 0 }), A('load_preset', { cameraId: 2, presetId: 0 })], 'deliberate video mode preset')
seq('video-mode-default', 'video-mode-untouched', [save(2, 1, 'Sin tocar formato'), A('load_preset', { cameraId: 2, presetId: 1 })], 'preset with filler video_mode 60,0,6,0,0')
// 8 memory_preset with PTZ group
seq('memory-preset', 'memory-recall-in-preset', [A('recall_memory_preset', { cameraId: 1, slot: 3 }), save(1, 2, 'PTZ recall', { includeGroupPtzControl: true }), A('load_preset', { cameraId: 1, presetId: 2 })], 'recall slot 3 stored in preset')
seq('memory-preset', 'memory-store-in-preset', [A('store_memory_preset', { cameraId: 1, slot: 2 }), save(1, 3, 'PTZ store', { includeGroupPtzControl: true }), A('load_preset', { cameraId: 1, presetId: 3 })], 'store slot 2 stored in preset')
// 9 zoom speed in a preset
seq('zoom-preset', 'zoom-speed-in-preset', [A('zoom_start', { cameraId: 1, direction: 'in', speed: 0.5 }), save(1, 4, 'Zoom test'), A('zoom_stop', { cameraId: 1 }), A('load_preset', { cameraId: 1, presetId: 4 })], 'continuous zoom stored')
// 10 long UTF-8 name with &
seq('savepreset-name', 'save-long-utf8-name', [save(1, 0, 'Escena ñandú cámara principal & plano general'), A('list_presets', {})], 'name > 62 encoded bytes')
// 11 long line + reload
const g = '0.30000000000000004'
seq('savepreset-longline', 'save-long-line-web', [
	{ do: 'externalHttp', method: 'POST', path: '/savePreset', body: `cameraId=4&presetId=0&name=T&data=${encodeURIComponent(`gain_adjust:${g},${g},${g},${g};`)}` },
	A('load_preset', { cameraId: 4, presetId: 0 }),
], 'gain_adjust line of 91 chars')
seq('savepreset-longline', 'save-long-line-module', [
	A('set_gain_adjust_0_increment', { cameraId: 4, increment: 0.1 }), A('set_gain_adjust_0_increment', { cameraId: 4, increment: 0.1 }), A('set_gain_adjust_0_increment', { cameraId: 4, increment: 0.1 }),
	A('set_gain_adjust_3_increment', { cameraId: 4, increment: 0.1 }), A('set_gain_adjust_3_increment', { cameraId: 4, increment: 0.1 }), A('set_gain_adjust_3_increment', { cameraId: 4, increment: 0.1 }),
	save(4, 1, 'Long floats'), A('load_preset', { cameraId: 4, presetId: 1 }),
], 'module produces unrounded floats, saves and reloads')
// 12 missing preset
seq('loadpreset-missing', 'load-missing-4-4', [A('load_preset', { cameraId: 4, presetId: 4 })], 'preset_4_4 does not exist')
// 13 timecode_source_0
seq('timecode', 'timecode-source-0', [A('set_timecode_source_0', { cameraId: 1, value: 1 })], 'unknown key')
// 14 vMix tally + vMix off
seq('vmix-tally', 'vmix-tally-then-off', [
	A('set_vmix_connect', { enabled: 'true' }), { do: 'wait', ms: 6000 },
	{ do: 'vmixTally', state: '1200000000', ms: 800 },
	A('set_vmix_connect', { enabled: 'false' }), { do: 'wait', ms: 1500 },
	A('set_vmix_connect', { enabled: 'true' }), { do: 'wait', ms: 6000 },
	{ do: 'vmixTally', state: '0000000000', ms: 600 },
], 'program on cam1, preview cam2, then vMix disabled from Companion')
// 15 vMix refusing connections (vMix PC up, vMix closed)
seq('vmix-refused', 'vmix-refused-actions', [
	{ do: 'vmixDown' }, A('set_vmix_connect', { enabled: 'false' }), { do: 'wait', ms: 300 }, A('set_vmix_connect', { enabled: 'true' }),
	...Array.from({ length: 6 }, (_, i) => A('set_gain_db', { cameraId: 1, value: i })),
	A('list_presets', {}), A('load_preset', { cameraId: 1, presetId: 0 }), { do: 'wait', ms: 6000 },
	...Array.from({ length: 4 }, (_, i) => A('set_focus', { cameraId: 1, value: i / 10 })),
	{ do: 'vmixUp' }, { do: 'wait', ms: 8000 }, A('set_gain_db', { cameraId: 1, value: 2 }),
], 'Companion works while the firmware retries a refusing vMix')
// 16/17 network cut on 8098 while the firmware has something to send
for (const [sec, tags] of [[10, undefined], [29, ['slow']], [45, ['slow']]]) {
	seq(`netcut-${sec}s`, `netcut-${sec}s-focus`, [
		A('set_focus', { cameraId: 1, value: 0.1 }),
		{ do: 'netCut', port: 8098 },
		{ do: 'wait', ms: 300 },
		{ do: 'externalHttp', path: '/?cameraId=1&focus=0.3' },
		{ do: 'wait', ms: sec * 1000 },
		{ do: 'netRestore' },
		{ do: 'waitTcp', dir: 'rx', match: 'CCU 1 focus 0.3', timeout: 4000, optional: true },
		{ do: 'wait', ms: 40000 },
		{ do: 'externalHttp', path: '/?cameraId=1&focus=0.4' },
		{ do: 'waitTcp', dir: 'rx', match: 'CCU 1 focus 0.4', timeout: 3000, optional: true },
		A('set_focus_increment', { cameraId: 1, increment: 0.1 }),
	], `${sec} s blackhole on TCP 8098 while the web changes focus`, tags)
}
// 18 Safe Mode
seq('safemode-enter', 'safemode-enter', [{ do: 'serial', line: 'safemode', ms: 200 }, { do: 'waitBoot', timeout: 15000 }], 'serial safemode -> reboot')
seq('safemode', 'safemode-set-focus', [A('set_focus', { cameraId: 1, value: 0.5 })], 'param in Safe Mode')
seq('safemode', 'safemode-load-preset', [A('load_preset', { cameraId: 1, presetId: 0 })], 'loadPreset in Safe Mode')
seq('safemode', 'safemode-list', [A('list_presets', {})], 'listPresets in Safe Mode')
seq('safemode', 'safemode-save', [save(1, 0, 'Safe')], 'savePreset in Safe Mode')
seq('safemode-exit', 'safemode-exit', [{ do: 'serial', line: 'normalboot', ms: 200 }, { do: 'waitBoot', timeout: 15000 }, { do: 'wait', ms: 2000 }], 'serial normalboot -> reboot')
seq('after-safemode', 'after-safemode-gain', [A('set_gain_db', { cameraId: 1, value: 4 })], 'back to normal')
fs.writeFileSync(__dirname + '/static-cases.json', JSON.stringify({ format: 'static-1', cases }, null, 1))
console.log('static cases:', n)
