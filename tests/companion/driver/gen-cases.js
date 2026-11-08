'use strict'
// Generates cases.json: a plan that walks EVERY action of the Companion module with
// representative options, plus stateful sequences. Action definitions are taken from the real
// module (through the harness, with no host configured, so nothing goes on the network).
//
//   node gen-cases.js [--out cases.json]

const fs = require('fs')
const path = require('path')
const { createHarness, optionDefaults, DEFAULT_MODULE_DIR } = require('./driver')

const outArg = process.argv.indexOf('--out')
const OUT = outArg > 0 ? path.resolve(process.argv[outArg + 1]) : path.join(__dirname, 'cases.json')

// ------------------------------------------------------------------ helpers
const decimals = (step) => {
	if (!step || Number.isInteger(step)) return 0
	const s = String(step)
	return s.includes('.') ? s.split('.')[1].length : 0
}
const snap = (v, o) => Number(Number(v).toFixed(decimals(o.step)))
const midOf = (o) => {
	const step = o.step || 1
	const n = Math.round((o.max - o.min) / 2 / step)
	return snap(o.min + n * step, o)
}
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b)

function modeOf(id) {
	if (/_increment$/.test(id)) return 'increment'
	if (/_decrement$/.test(id)) return 'decrement'
	if (/_reset$/.test(id)) return 'reset'
	if (/^(zoom_start|set_pan_tilt)$/.test(id)) return 'start'
	if (/^(zoom_stop|stop_pan_tilt)$/.test(id)) return 'stop'
	if (/^(store_memory_preset|recall_memory_preset)$/.test(id)) return 'memory'
	if (/^set_(instantaneous_auto_aperture|instantaneous_autofocus|set_auto_wb|restore_auto_wb|correction_reset_default)$/.test(id))
		return 'trigger'
	// v3.1.0 has no real toggle action: booleans are set explicitly (checkbox true/false)
	if (id === 'set_optical_image_stabilisation' || id === 'set_phantom_power') return 'boolean'
	if (id === 'load_preset' || id === 'save_current_as_preset' || id === 'list_presets') return 'preset'
	if (id === 'set_vmix_connect') return 'vmix'
	if (id === 'retry_connection' || id === 'change_camera') return 'utility'
	return 'set'
}

// Which src/actions/<file>.js defines each action (read-only: the files are required with a
// minimal fake `self` only to collect the ids; callbacks are never executed here).
function actionFiles() {
	const dir = path.join(DEFAULT_MODULE_DIR, 'src', 'actions')
	const map = {}
	for (const f of fs.readdirSync(dir)) {
		if (!f.endsWith('.js') || f === 'index.js') continue
		const acts = {}
		require(path.join(dir, f))({ config: { defaultCameraId: 1 }, log() {} }, acts)
		for (const id of Object.keys(acts)) map[id] = f.replace(/\.js$/, '')
	}
	return map
}

// ------------------------------------------------------------------ single-action cases
function singleCases(defs, fileOf) {
	const cases = []
	const order = ['lens', 'video', 'audio', 'output', 'display', 'tally', 'reference', 'color', 'ptz', 'presets']
	const byFile = {}
	for (const d of defs) (byFile[fileOf[d.id] || 'other'] = byFile[fileOf[d.id] || 'other'] || []).push(d)

	for (const file of order.concat(Object.keys(byFile).filter((k) => !order.includes(k)))) {
		if (file === 'presets') continue // handled by presetCases / utilityCases
		for (const d of byFile[file] || []) {
			const suite = 'actions:' + file
			const mode = modeOf(d.id)
			const defaults = optionDefaults(d)
			const seen = []
			const add = (suffix, options, notes, tier = 'core', extra = {}) => {
				const full = { ...defaults, ...options }
				if (seen.some((s) => same(s, full))) return
				seen.push(full)
				cases.push({
					id: `${d.id}@${suffix}`,
					suite,
					kind: 'single',
					mode,
					tier,
					action: d.id,
					options: full,
					notes,
					expect: { requests: 1 },
					...extra,
				})
			}
			const hasCam = d.options.some((o) => o.id === 'cameraId')
			add('default', {}, `${mode}: todas las opciones por defecto (cámara ${defaults.cameraId ?? '-'})`)
			const valueOpts = d.options.filter((o) => o.id !== 'cameraId')
			const numOpts = valueOpts.filter((o) => o.type === 'number')

			for (const o of valueOpts) {
				if (o.type === 'number') {
					const label = o.id === 'increment' || o.id === 'decrement' ? `paso ${o.id}` : o.id
					add(`${o.id}=min`, { [o.id]: o.min }, `${mode}: ${label} = mínimo (${o.min})`)
					add(`${o.id}=max`, { [o.id]: o.max }, `${mode}: ${label} = máximo (${o.max})`)
					add(`${o.id}=mid`, { [o.id]: midOf(o) }, `${mode}: ${label} = intermedio (${midOf(o)})`, 'extended')
				} else if (o.type === 'checkbox') {
					add(`${o.id}=true`, { [o.id]: true }, `${mode}: ${o.id} = true`)
					add(`${o.id}=false`, { [o.id]: false }, `${mode}: ${o.id} = false`)
				} else if (o.type === 'dropdown') {
					for (const c of o.choices || []) add(`${o.id}=${c.id}`, { [o.id]: c.id }, `${mode}: ${o.id} = ${JSON.stringify(c.id)} (${c.label})`)
				} else if (o.type === 'textinput') {
					// exposure_and_focus_tools: bit flags as free text (the default '' sends ",")
					for (const [v, n] of [
						['0', 'cero'],
						['1', 'bit 1'],
						['7', 'todos los bits de función'],
						['31', 'todos los displays'],
					])
						add(`${o.id}=${v}`, { [o.id]: v }, `${mode}: ${o.id} = '${v}' (${n})`)
				}
			}
			// multi-value params: all-min / all-max / all-mid together
			if (numOpts.length > 1) {
				const pick = (fn) => Object.fromEntries(numOpts.map((o) => [o.id, fn(o)]))
				add('all=min', pick((o) => o.min), `${mode}: todos los subvalores al mínimo`)
				add('all=max', pick((o) => o.max), `${mode}: todos los subvalores al máximo`)
				add('all=mid', pick(midOf), `${mode}: todos los subvalores intermedios`, 'extended')
			}
			if (d.id === 'set_exposure_and_focus_tools') {
				add('flags=7,31', { value0: '7', value1: '31' }, 'set: todas las herramientas en todos los displays')
				add('flags=1,1', { value0: '1', value1: '1' }, 'set: zebra en LCD')
			}
			if (d.id === 'zoom_start' || d.id === 'set_pan_tilt') {
				const dirs = d.options.find((o) => o.id === 'direction').choices.map((c) => c.id)
				for (const dir of dirs) {
					add(`direction=${dir},speed=max`, { direction: dir, speed: 1 }, `start: ${dir} a velocidad máxima`, 'extended')
					add(`direction=${dir},speed=min`, { direction: dir, speed: 0 }, `start: ${dir} a velocidad 0 (envía -0 → "0")`, 'extended')
				}
			}
			if (hasCam) {
				add('cam8', { cameraId: 8 }, `${mode}: valores por defecto en la cámara 8`)
				if (numOpts.length) {
					add(
						'cam8-max',
						{ cameraId: 8, ...Object.fromEntries(numOpts.map((o) => [o.id, o.max])) },
						`${mode}: cámara 8 con valores máximos`,
						'extended',
					)
				}
			}
		}
	}
	return cases
}

// Values outside the option range (the Companion UI normally prevents them): firmware
// robustness only, never counted as a compatibility requirement.
function robustnessCases() {
	const C = (id, action, options, notes) => ({
		id: 'robust:' + id,
		suite: 'robustness',
		kind: 'single',
		mode: modeOf(action),
		tier: 'robustness',
		action,
		options,
		notes: notes + ' [fuera de rango: la UI de Companion no lo permite; solo robustez del firmware]',
		expect: { requests: 1 },
	})
	return [
		C('gain-below-min', 'set_gain_db', { cameraId: 1, value: -13 }, 'gain_db = -13 (< -12)'),
		C('gain-above-max', 'set_gain_db', { cameraId: 1, value: 37 }, 'gain_db = 37 (> 36)'),
		C('aperture-above-1', 'set_aperture_normalised', { cameraId: 1, value: 1.5 }, 'aperture = 1.5'),
		C('aperture-negative', 'set_aperture_normalised', { cameraId: 1, value: -0.1 }, 'aperture = -0.1'),
		C('wb-out', 'set_manual_white_balance', { cameraId: 1, value0: 2499, value1: 51 }, 'white balance 2499 K, tint 51'),
		C('cam0', 'set_gain_db', { cameraId: 0, value: 2 }, 'cameraId = 0'),
		C('cam9', 'set_gain_db', { cameraId: 9, value: 2 }, 'cameraId = 9'),
		C('cam255', 'set_gain_db', { cameraId: 255, value: 2 }, 'cameraId = 255'),
		C('load-preset5', 'load_preset', { cameraId: 1, presetId: 5 }, 'loadPreset=1,5 (presetId > 4)'),
		C('load-cam9', 'load_preset', { cameraId: 9, presetId: 0 }, 'loadPreset=9,0 (cameraId > 8)'),
		C('ois-cam0', 'set_optical_image_stabilisation', { cameraId: 0, value: true }, 'OIS en cameraId 0'),
		C('shutter-0', 'set_shutter_speed', { cameraId: 1, value: 0 }, 'shutter_speed = 0'),
		C('nd-dropdown-3', 'set_nd_filter_stop_0', { cameraId: 1, value: 3 }, 'ND 3 (no es una opción del desplegable)'),
		C('memory-slot0', 'recall_memory_preset', { cameraId: 1, slot: 0 }, 'slot 0 → envía memory_preset=2,-1'),
	]
}

// ------------------------------------------------------------------ presets & utilities
const NAMES = {
	spaces: 'Escena Uno Dos',
	accents: 'Cámara Ñandú Acción',
	amp: 'Rock & Roll',
	pct: '100% Gain',
	apos: "Joaquin's Look",
	mixed: "A&B 50% l'été",
	len31: 'ABCDEFGHIJKLMNOPQRSTUVWXYZ01234',
	len32: 'ABCDEFGHIJKLMNOPQRSTUVWXYZ012345',
	len33: 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456',
	len40: 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcd',
	len31accents: 'Cámara añadida en la ópera ññññ', // 31 caracteres, >31 bytes UTF-8
}
for (const k of ['len31', 'len32', 'len33', 'len40']) {
	if (NAMES[k].length !== parseInt(k.slice(3), 10)) throw new Error('bad name length ' + k)
}
if ([...NAMES.len31accents].length !== 31) throw new Error('bad len31accents length ' + [...NAMES.len31accents].length)

const EXTRA_NAMES = {
	plus: 'Plus+Sign',
	separators: 'Wide; f2.8: ND=2, gain',
	empty: '',
	emoji: 'Señal 📷',
	slash: 'a/b\\c',
}

function presetCases() {
	const S = 'presets'
	const cases = []
	const P = (id, action, options, notes, extra = {}) =>
		cases.push({ id: 'preset:' + id, suite: S, kind: 'single', mode: 'preset', tier: 'core', action, options, notes, ...extra })
	const save = (cameraId, presetId, presetName, groups = {}) => ({
		cameraId,
		presetId,
		presetName,
		includeGroupAudio: false,
		includeGroupColorCorrection: true,
		includeGroupDisplay: false,
		includeGroupLens: true,
		includeGroupOutput: false,
		includeGroupPtzControl: false,
		includeGroupReference: false,
		includeGroupTally: false,
		includeGroupVideo: true,
		...groups,
	})
	const ALL = {
		includeGroupAudio: true,
		includeGroupColorCorrection: true,
		includeGroupDisplay: true,
		includeGroupLens: true,
		includeGroupOutput: true,
		includeGroupPtzControl: true,
		includeGroupReference: true,
		includeGroupTally: true,
		includeGroupVideo: true,
	}
	const NONE = Object.fromEntries(Object.keys(ALL).map((k) => [k, false]))

	P('list-initial', 'list_presets', {}, 'listar presets al principio (estado de la SD desconocido)', { expect: { requests: 1 } })
	P('save-default', 'save_current_as_preset', save(1, 0, 'My Preset'), 'guardar con opciones por defecto (cam 1, preset 0, grupos por defecto)', {
		expect: { requests: 1 },
	})
	P('save-all-groups', 'save_current_as_preset', save(1, 1, 'Todos los grupos', ALL), 'guardar con TODOS los grupos (cuerpo POST más largo: 49 parámetros)', {
		expect: { requests: 1 },
	})
	P('save-no-groups', 'save_current_as_preset', save(1, 2, 'Sin grupos', NONE), 'ningún grupo: el módulo no envía nada ("No parameters to save")', {
		expect: { requests: 0 },
	})
	for (const [g, label] of [
		['includeGroupAudio', 'audio'],
		['includeGroupPtzControl', 'ptz_control'],
		['includeGroupTally', 'tally'],
	]) {
		P(`save-only-${label}`, 'save_current_as_preset', save(1, 3, `Solo ${label}`, { ...NONE, [g]: true }), `guardar solo el grupo ${label}`, {
			expect: { requests: 1 },
		})
	}
	// names: cam 2, presets 0..4 cycling; each save followed by a listing to see what came back
	let slot = 0
	for (const [k, name] of Object.entries(NAMES)) {
		const presetId = slot++ % 5
		P(`save-name-${k}`, 'save_current_as_preset', save(2, presetId, name), `guardar nombre ${k}: ${JSON.stringify(name)} (${[...name].length} car., ${Buffer.byteLength(name)} bytes UTF-8) en cam 2 preset ${presetId}`, {
			expect: { requests: 1, form: { name } },
			tags: ['names'],
		})
		P(`list-after-${k}`, 'list_presets', {}, `listar tras guardar ${k}; variable cam2_preset${presetId}_name debería reflejar el nombre devuelto por el firmware`, {
			expect: { requests: 1, variable: { [`cam2_preset${presetId}_name`]: name } },
			tags: ['names'],
		})
	}
	for (const [k, name] of Object.entries(EXTRA_NAMES)) {
		P(`save-name-${k}`, 'save_current_as_preset', save(3, 0, name), `nombre extra ${k}: ${JSON.stringify(name)} (cam 3 preset 0)`, {
			tier: 'robustness',
			expect: { requests: 1, form: { name } },
			tags: ['names'],
		})
		P(`list-after-${k}`, 'list_presets', {}, `listar tras guardar ${k}`, { tier: 'robustness', expect: { requests: 1 }, tags: ['names'] })
	}
	P('save-cam8', 'save_current_as_preset', save(8, 4, 'Cam 8 slot 4'), 'guardar en cámara 8, preset 4 (máximos)', { expect: { requests: 1 } })
	P('list-final', 'list_presets', {}, 'listar todos los presets guardados', { expect: { requests: 1 } })
	P('load-existing-1-0', 'load_preset', { cameraId: 1, presetId: 0 }, 'cargar preset existente (guardado en preset:save-default)', {
		expect: { requests: 1, log: { level: 'info', match: 'Preset loaded successfully' } },
	})
	P('load-existing-1-1', 'load_preset', { cameraId: 1, presetId: 1 }, 'cargar preset existente con todos los grupos', {
		expect: { requests: 1, log: { level: 'info', match: 'Preset loaded successfully' } },
	})
	P('load-existing-8-4', 'load_preset', { cameraId: 8, presetId: 4 }, 'cargar preset existente de la cámara 8', {
		expect: { requests: 1, log: { level: 'info', match: 'Preset loaded successfully' } },
	})
	P('load-existing-2-0', 'load_preset', { cameraId: 2, presetId: 0 }, 'cargar preset con nombre especial (último guardado en cam 2 preset 0)', {
		expect: { requests: 1, log: { level: 'info', match: 'Preset loaded successfully' } },
	})
	P('load-nonexistent-7-4', 'load_preset', { cameraId: 7, presetId: 4 }, 'cargar preset INEXISTENTE (ningún caso guarda en cam 7; asume SD sin ese preset)', {
		expect: { requests: 1, log: { level: 'warn', match: 'Preset did not load correctly' } },
	})
	P('load-nonexistent-6-3', 'load_preset', { cameraId: 6, presetId: 3 }, 'cargar otro preset INEXISTENTE (cam 6 preset 3)', {
		expect: { requests: 1, log: { level: 'warn', match: 'Preset did not load correctly' } },
	})
	return cases
}

function utilityCases() {
	const S = 'utility'
	const U = (id, action, options, notes, extra = {}) => ({ id: 'util:' + id, suite: S, kind: 'single', mode: modeOf(action), tier: 'core', action, options, notes, ...extra })
	return [
		U('vmix-on', 'set_vmix_connect', { enabled: 'true' }, 'vmixConnect=1 (activar)', { expect: { requests: 1, query: { vmixConnect: '1' } } }),
		U('vmix-off', 'set_vmix_connect', { enabled: 'false' }, 'vmixConnect=0 (desactivar)', { expect: { requests: 1, query: { vmixConnect: '0' } } }),
		U('vmix-on-again', 'set_vmix_connect', { enabled: 'true' }, 'vmixConnect=1 otra vez (idempotente)', { expect: { requests: 1, query: { vmixConnect: '1' } } }),
		U('vmix-off-final', 'set_vmix_connect', { enabled: 'false' }, 'vmixConnect=0 (deja vMix desactivado)', { expect: { requests: 1, query: { vmixConnect: '0' } } }),
		U('retry-connection', 'retry_connection', {}, 'reintento manual: con TCP conectado NO hace HTTP; sin TCP hace GET /?listPresets', {
			expect: { requests: null },
		}),
		U('change-camera-1', 'change_camera', { cameraId: 1 }, 'envía /?cameraId=1&cameraId=1 (clave repetida)', { expect: { requests: 1 } }),
		U('change-camera-8', 'change_camera', { cameraId: 8 }, 'envía /?cameraId=8&cameraId=8', { expect: { requests: 1 } }),
		U('change-camera-4', 'change_camera', { cameraId: 4 }, 'cámara intermedia', { tier: 'extended', expect: { requests: 1 } }),
	]
}

// ------------------------------------------------------------------ stateful sequences
function A(action, options = {}, expect) {
	const s = { do: 'action', action, options }
	if (expect) s.expect = expect
	return s
}
function sequenceCases(defs) {
	const out = []
	const SEQ = (id, notes, steps, extra = {}) => {
		const first = steps.find((s) => s.do === 'action')
		out.push({
			id: 'seq:' + id,
			suite: 'seq:' + id,
			fresh: true,
			kind: 'sequence',
			mode: extra.mode || (first ? modeOf(first.action) : 'sequence'),
			tier: extra.tier || 'core',
			action: first ? first.action : null,
			options: first ? first.options : null,
			notes,
			steps,
			...(extra.tags ? { tags: extra.tags } : {}),
		})
	}
	const q = (k, v) => ({ query: { [k]: v } })

	SEQ('inc-unknown-aperture', 'increment repetido SIN valor conocido (módulo recién arrancado): parte del default 0.0; el 3er paso envía 0.30000000000000004 (aritmética JS, sin redondeo)', [
		A('set_aperture_normalised_increment', { cameraId: 1, increment: 0.1 }, q('aperture_normalised', '0.1')),
		A('set_aperture_normalised_increment', { cameraId: 1, increment: 0.1 }, q('aperture_normalised', '0.2')),
		A('set_aperture_normalised_increment', { cameraId: 1, increment: 0.1 }, q('aperture_normalised', '0.30000000000000004')),
	])
	SEQ('inc-unknown-focus-x11', 'focus +0.1 once veces sin valor conocido: genera 0.7999999999999999, 0.8999999999999999, 0.9999999999999999 y satura en 1', Array.from({ length: 11 }, (_, i) => {
		let v = 0
		for (let k = 0; k <= i; k++) v = Math.min(1.0, v + 0.1)
		return A('set_focus_increment', { cameraId: 1, increment: 0.1 }, q('focus', String(v)))
	}), { tags: ['float'] })
	SEQ('inc-dec-unknown-gain', 'gain +1 x3 y -1 x5 sin valor conocido: 1,2,3,2,1,0,-1,-2', [
		...[1, 2, 3].map((v) => A('set_gain_db_increment', { cameraId: 1, increment: 1 }, q('gain_db', String(v)))),
		...[2, 1, 0, -1, -2].map((v) => A('set_gain_db_decrement', { cameraId: 1, decrement: 1 }, q('gain_db', String(v)))),
	])
	SEQ('dec-unknown-mic', 'mic_level -0.1 sin valor conocido parte del default 0.7 (no de 0)', [
		A('set_mic_level_decrement', { cameraId: 1, decrement: 0.1 }, q('mic_level', String(0.7 - 0.1))),
		A('set_mic_level_decrement', { cameraId: 1, decrement: 0.1 }, q('mic_level', String(0.7 - 0.1 - 0.1))),
	])
	SEQ('clamp-max-aperture', 'saturación en el máximo: set 0.9 y +0.1 dos veces → 1, 1', [
		A('set_aperture_normalised', { cameraId: 1, value: 0.9 }, q('aperture_normalised', '0.9')),
		A('set_aperture_normalised_increment', { cameraId: 1, increment: 0.1 }, q('aperture_normalised', '1')),
		A('set_aperture_normalised_increment', { cameraId: 1, increment: 0.1 }, q('aperture_normalised', '1')),
	])
	SEQ('clamp-min-gain', 'saturación en el mínimo: set -11 y -1 dos veces → -12, -12', [
		A('set_gain_db', { cameraId: 1, value: -11 }, q('gain_db', '-11')),
		A('set_gain_db_decrement', { cameraId: 1, decrement: 1 }, q('gain_db', '-12')),
		A('set_gain_db_decrement', { cameraId: 1, decrement: 1 }, q('gain_db', '-12')),
	])
	SEQ('inc-big-step', 'paso máximo: gain +48 desde 0 satura en 36; wb +7500 desde 5600 satura en 10000', [
		A('set_gain_db_increment', { cameraId: 1, increment: 48 }, q('gain_db', '36')),
		A('set_manual_white_balance_0_increment', { cameraId: 1, increment: 7500 }, q('manual_white_balance', '10000,10')),
	])
	SEQ('inc-after-tcp-gain', 'increment DESPUÉS de recibir el valor por TCP: otro cliente HTTP (la web) pone gain=10, el firmware lo difunde como "CCU 1 gain_db 10" al módulo, y +1 debe enviar 11 (si el firmware no difunde, el módulo enviará 1)', [
		{ do: 'externalHttp', path: '/?cameraId=1&gain_db=10', note: 'emula la UI web' },
		{ do: 'waitTcp', dir: 'rx', match: 'CCU 1 gain_db', timeout: 3000 },
		A('set_gain_db_increment', { cameraId: 1, increment: 1 }, q('gain_db', '11')),
	])
	SEQ('inc-after-tcp-wb-array', 'increment de subíndice tras recibir un array por TCP: la web pone white balance 3200,5; +100 K debe enviar 3300,5', [
		{ do: 'externalHttp', path: '/?cameraId=1&manual_white_balance=3200%2C5', note: 'emula la UI web' },
		{ do: 'waitTcp', dir: 'rx', match: 'CCU 1 manual_white_balance', timeout: 3000 },
		A('set_manual_white_balance_0_increment', { cameraId: 1, increment: 100 }, q('manual_white_balance', '3300,5')),
		A('set_manual_white_balance_1_decrement', { cameraId: 1, decrement: 10 }, q('manual_white_balance', '3300,-5')),
	])
	SEQ('inc-after-push-gain', 'línea CCU empujada por el servidor (mock: push real; firmware: inyección local en el socket del módulo) y luego +1', [
		{ do: 'serverPush', line: 'CCU 1 gain_db 20' },
		A('set_gain_db_increment', { cameraId: 1, increment: 1 }, q('gain_db', '21')),
	])
	SEQ('inc-after-push-float', 'valor flotante formateado por el firmware ("0.50") y luego +0.1', [
		{ do: 'serverPush', line: 'CCU 1 aperture_normalised 0.50' },
		A('set_aperture_normalised_increment', { cameraId: 1, increment: 0.1 }, q('aperture_normalised', '0.6')),
	])
	SEQ('inc-after-own-echo', 'el eco TCP de un set propio fija el estado: set gain 5 (eco CCU), luego +1 → 6', [
		A('set_gain_db', { cameraId: 1, value: 5 }, q('gain_db', '5')),
		{ do: 'waitTcp', dir: 'rx', match: 'CCU 1 gain_db', timeout: 2000, optional: true },
		A('set_gain_db_increment', { cameraId: 1, increment: 1 }, q('gain_db', '6')),
	])
	SEQ('shutter-fps', 'shutter por múltiplos del frame rate: video_mode_0=24 → 48, 96, 192; luego decrement → 96', [
		A('set_video_mode_0', { cameraId: 1, value: 24 }, q('video_mode', '24,0,6,0,0')),
		A('set_shutter_speed_increment', { cameraId: 1 }, q('shutter_speed', '48')),
		A('set_shutter_speed_increment', { cameraId: 1 }, q('shutter_speed', '96')),
		A('set_shutter_speed_increment', { cameraId: 1 }, q('shutter_speed', '192')),
		A('set_shutter_speed_decrement', { cameraId: 1 }),
	])
	SEQ('shutter-unknown', 'shutter increment sin video_mode conocido: fps por defecto 60 → 120', [
		A('set_shutter_speed_increment', { cameraId: 2 }, q('shutter_speed', '120')),
		A('set_shutter_speed_decrement', { cameraId: 2 }),
	])
	SEQ('nd-steps', 'ND en pasos de 2: +2 x4 → 2,4,6,6; -2 → 4', [
		...['2,0', '4,0', '6,0', '6,0'].map((v) => A('set_nd_filter_stop_0_increment', { cameraId: 1 }, q('nd_filter_stop', v))),
		A('set_nd_filter_stop_0_decrement', { cameraId: 1 }, q('nd_filter_stop', '4,0')),
	])
	SEQ('subindex-mix', 'subíndices: set lift_adjust_0=-1.5 y lift_adjust_3=1.2; el array conserva los otros subvalores', [
		A('set_lift_adjust_0', { cameraId: 1, value: -1.5 }),
		A('set_lift_adjust_3', { cameraId: 1, value: 1.2 }),
		A('set_lift_adjust_2_increment', { cameraId: 1 }),
		A('set_lift_adjust_1_reset', { cameraId: 1 }),
	])
	SEQ('cameras-independent', 'el estado es por cámara: +1 en cam 8 y en cam 1 parten ambos de 0', [
		A('set_gain_db_increment', { cameraId: 8, increment: 1 }, q('gain_db', '1')),
		A('set_gain_db_increment', { cameraId: 8, increment: 1 }, q('gain_db', '2')),
		A('set_gain_db_increment', { cameraId: 1, increment: 1 }, q('gain_db', '1')),
	])
	SEQ('requestsync-sse', 'REQUESTSYNC real: tras fijar valores, un cliente SSE (/events) se conecta; el firmware envía REQUESTSYNC al módulo, que responde con CCUSYNC por cada parámetro cacheado; el firmware los reenvía al SSE', [
		A('set_gain_db', { cameraId: 1, value: 6 }),
		A('set_manual_white_balance', { cameraId: 1, value0: 4500, value1: -5 }),
		A('set_optical_image_stabilisation', { cameraId: 1, value: false }),
		A('set_instantaneous_autofocus', { cameraId: 1 }),
		{ do: 'sse', path: '/events', holdMs: 2500 },
		{ do: 'waitTcp', dir: 'tx', match: 'CCUSYNC', timeout: 3000 },
	])
	SEQ('requestsync-push', 'REQUESTSYNC empujado por el servidor (mock: push; firmware: inyección local) → el módulo envía CCUSYNC por TCP al firmware', [
		A('set_gain_db', { cameraId: 3, value: -4 }),
		A('set_lift_adjust', { cameraId: 3, value0: -0.5, value1: 0, value2: 0.25, value3: 1 }),
		{ do: 'serverPush', line: 'REQUESTSYNC' },
		{ do: 'waitTcp', dir: 'tx', match: 'CCUSYNC 3 gain_db', timeout: 2000 },
	])
	SEQ('requestsync-after-save', 'tras guardar un preset el módulo cachea los 44 defaults + cameraId: REQUESTSYNC (vía SSE) devuelve ~45 CCUSYNC con booleanos "true"/"false" y "CCUSYNC 5 cameraId 5"', [
		A('set_gain_db', { cameraId: 5, value: 6 }),
		A('change_camera', { cameraId: 5 }),
		A('save_current_as_preset', {
			cameraId: 5,
			presetId: 0,
			presetName: 'Sync tras guardar',
			includeGroupAudio: true,
			includeGroupColorCorrection: true,
			includeGroupDisplay: true,
			includeGroupLens: true,
			includeGroupOutput: true,
			includeGroupPtzControl: true,
			includeGroupReference: true,
			includeGroupTally: true,
			includeGroupVideo: true,
		}, { requests: 1 }),
		{ do: 'sse', path: '/events', holdMs: 3000 },
		{ do: 'waitTcp', dir: 'tx', match: 'CCUSYNC 5 optical_image_stabilisation', timeout: 3000 },
	])
	SEQ('preset-roundtrip', 'ida y vuelta: fija valores, guarda (cam 1 preset 2 vídeo+lente), lista, cambia gain, carga el preset y luego +1 (el estado debe venir del preset)', [
		A('set_gain_db', { cameraId: 1, value: 12 }),
		A('set_manual_white_balance', { cameraId: 1, value0: 3200, value1: 0 }),
		A('set_aperture_normalised', { cameraId: 1, value: 0.5 }),
		A('save_current_as_preset', {
			cameraId: 1,
			presetId: 2,
			presetName: 'Roundtrip Ñ & 100%',
			includeGroupAudio: false,
			includeGroupColorCorrection: false,
			includeGroupDisplay: false,
			includeGroupLens: true,
			includeGroupOutput: false,
			includeGroupPtzControl: false,
			includeGroupReference: false,
			includeGroupTally: false,
			includeGroupVideo: true,
		}, { requests: 1, form: { name: 'Roundtrip Ñ & 100%' } }),
		{ do: 'waitTcp', dir: 'rx', match: 'PRESETSAVED 1 2', timeout: 2000, optional: true },
		A('list_presets', {}, { requests: 1, variable: { cam1_preset2_name: 'Roundtrip Ñ & 100%' } }),
		A('set_gain_db', { cameraId: 1, value: 0 }),
		A('load_preset', { cameraId: 1, presetId: 2 }, { requests: 1, log: { level: 'info', match: 'Preset loaded successfully' }, variable: { cam1_active_preset_id: '2' } }),
		{ do: 'waitTcp', dir: 'rx', match: 'PRESET 1 2', timeout: 3000, optional: true },
		{ do: 'wait', ms: 300 },
		A('set_gain_db_increment', { cameraId: 1, increment: 1 }, q('gain_db', '13')),
	])
	SEQ('preset-load-nonexistent', 'cargar un preset inexistente no cambia las variables de preset activo', [
		A('load_preset', { cameraId: 7, presetId: 4 }, { requests: 1, log: { level: 'warn', match: 'Preset did not load correctly' }, variable: { cam7_active_preset_id: '-' } }),
	])
	SEQ('ois-feedback', 'feedback ois_enabled sigue al valor booleano enviado (1/0) y a su eco TCP', [
		A('set_optical_image_stabilisation', { cameraId: 1, value: false }, q('optical_image_stabilisation', '0')),
		{ do: 'feedback', feedback: 'ois_enabled', options: { cameraId: 1 }, expect: false },
		A('set_optical_image_stabilisation', { cameraId: 1, value: true }, q('optical_image_stabilisation', '1')),
		{ do: 'feedback', feedback: 'ois_enabled', options: { cameraId: 1 }, expect: true },
	])
	SEQ('ptz-start-stop', 'PTZ: pan derecha y parar; zoom in y parar', [
		A('set_pan_tilt', { cameraId: 1, direction: 'right', speed: 0.5 }, q('pan_tilt_velocity', '0.5,0')),
		A('stop_pan_tilt', { cameraId: 1 }, q('pan_tilt_velocity', '0,0')),
		A('zoom_start', { cameraId: 1, direction: 'in', speed: 0.5 }, q('set_continuous_zoom_speed', '0.5')),
		A('zoom_stop', { cameraId: 1 }, q('set_continuous_zoom_speed', '0')),
	])
	SEQ('memory-store-recall', 'memoria PTZ: guarda slot 1..5 y los recupera (0-based en el firmware)', [
		...[1, 2, 3, 4, 5].map((s) => A('store_memory_preset', { cameraId: 1, slot: s }, q('memory_preset', `1,${s - 1}`))),
		...[1, 2, 3, 4, 5].map((s) => A('recall_memory_preset', { cameraId: 1, slot: s }, q('memory_preset', `2,${s - 1}`))),
	])
	SEQ('vmix-toggle', 'vMix connect on/off/on/off', [
		A('set_vmix_connect', { enabled: 'true' }, q('vmixConnect', '1')),
		A('set_vmix_connect', { enabled: 'false' }, q('vmixConnect', '0')),
		A('set_vmix_connect', { enabled: 'true' }, q('vmixConnect', '1')),
		A('set_vmix_connect', { enabled: 'false' }, q('vmixConnect', '0')),
	], { mode: 'vmix' })
	SEQ('burst', '20 acciones seguidas sin pausa (como pulsar rápido un botón de +): todas deben recibir respuesta', Array.from({ length: 20 }, () => A('set_zebra_level_increment', { cameraId: 1, increment: 0.1 })), {
		tier: 'extended',
	})
	SEQ('tcp-reconnect', 'el cliente corta el TCP; el módulo reconecta a los 5 s y vuelve a suscribirse (el firmware debe liberar el hueco)', [
		{ do: 'dropTcp' },
		{ do: 'waitTcp', dir: 'tx', match: 'SUBSCRIBE CCU', timeout: 9000 },
		{ do: 'waitTcp', dir: 'rx', match: 'SUBSCRIBED OK', timeout: 3000 },
		A('set_gain_db', { cameraId: 1, value: 3 }),
	], { tier: 'extended', tags: ['slow'], mode: 'tcp' })
	SEQ('tcp-ping', 'keepalive: el módulo envía PING cada 30 s y el firmware responde PONG', [
		{ do: 'waitTcp', dir: 'tx', match: 'PING', timeout: 33000 },
		{ do: 'waitTcp', dir: 'rx', match: 'PONG', timeout: 3000 },
	], { tier: 'extended', tags: ['slow'], mode: 'tcp' })

	// every increment / decrement action 3 times from an unknown state (cam 3 / cam 4)
	const incs = defs.filter((d) => /_increment$/.test(d.id))
	const decs = defs.filter((d) => /_decrement$/.test(d.id))
	for (const [list, cam, label] of [
		[incs, 3, 'inc'],
		[decs, 4, 'dec'],
	]) {
		for (const d of list) {
			const o = optionDefaults(d)
			o.cameraId = cam
			out.push({
				id: `${label}x3:${d.id}`,
				suite: `seq:${label}x3-unknown`,
				fresh: true,
				kind: 'sequence',
				mode: label === 'inc' ? 'increment' : 'decrement',
				tier: 'extended',
				action: d.id,
				options: o,
				notes: `${label === 'inc' ? 'increment' : 'decrement'} repetido 3 veces sin valor previo conocido (cámara ${cam}, módulo recién arrancado al empezar la suite)`,
				steps: [A(d.id, o), A(d.id, o), A(d.id, o)],
			})
		}
	}
	return out
}

// ------------------------------------------------------------------ main
;(async () => {
	const h = await createHarness({ host: '' })
	const defs = h.actionDefinitions
	const meta = {
		moduleVersion: h.manifest.version,
		baseVersion: h.baseVersion,
		actionCount: defs.length,
		feedbackCount: h.feedbackDefinitions.length,
	}
	await h.close()

	const fileOf = actionFiles()
	const cases = [...singleCases(defs, fileOf), ...utilityCases(), ...presetCases(), ...robustnessCases(), ...sequenceCases(defs)]

	// sanity: ids unique, every action covered, every referenced action exists
	const ids = new Set()
	for (const c of cases) {
		if (ids.has(c.id)) throw new Error('duplicate case id ' + c.id)
		ids.add(c.id)
	}
	const known = new Set(defs.map((d) => d.id))
	const used = new Set()
	for (const c of cases) {
		for (const a of c.steps ? c.steps.filter((s) => s.do === 'action').map((s) => s.action) : [c.action]) {
			if (!known.has(a)) throw new Error(`case ${c.id} uses unknown action ${a}`)
			used.add(a)
		}
	}
	const missing = [...known].filter((a) => !used.has(a))
	if (missing.length) throw new Error('actions without cases: ' + missing.join(', '))

	const suites = []
	for (const c of cases) if (!suites.find((s) => s.id === c.suite)) suites.push({ id: c.suite, fresh: true })

	const stats = {}
	for (const c of cases) {
		stats[c.tier] = (stats[c.tier] || 0) + 1
	}
	const doc = {
		format: 'tallyccu-companion-cases/1',
		generatedBy: 'gen-cases.js',
		generatedAt: new Date().toISOString(),
		module: meta,
		conventions: {
			suite: 'Cada suite se ejecuta en una instancia NUEVA del módulo (estado interno vacío). Dentro de una suite los casos se ejecutan en orden y el estado del módulo se acumula.',
			kind: "'single' = una acción; 'sequence' = varios pasos con estado en 'steps' (action/options = primer paso de acción).",
			options: 'Opciones COMPLETAS tal y como Companion las enviaría en event.options (por defecto + cambios). Números como number, checkbox como boolean, dropdown como el id de la opción.',
			tier: "'core' = obligatorio; 'extended' = más cobertura; 'robustness' = fuera de rango / la UI de Companion no lo permite (no es requisito de compatibilidad).",
			steps: {
				action: '{do:"action", action, options, expect?}',
				externalHttp: '{do:"externalHttp", path} petición del propio arnés (emula la UI web), no del módulo',
				waitTcp: '{do:"waitTcp", dir:"rx"|"tx", match:"prefijo", timeout, optional?}',
				serverPush: '{do:"serverPush", line} el servidor envía la línea; con el firmware real se inyecta localmente en el socket del módulo',
				sse: '{do:"sse", path:"/events", holdMs} abre un cliente SSE',
				feedback: '{do:"feedback", feedback, options, expect} coloca un feedback y lee su valor',
				dropTcp: '{do:"dropTcp"} corta el socket TCP del módulo desde el cliente',
				wait: '{do:"wait", ms}',
			},
			expect: "Lo que el MÓDULO v3.1.0 hace por sí mismo (independiente del firmware), salvo 'variable'/'log' que dependen de la respuesta del firmware: requests = nº de fetch; query = pares clave/valor en la URL; form = campos del cuerpo POST; variable = valor de variable Companion; log = mensaje del módulo.",
		},
		stats: { total: cases.length, byTier: stats, suites: suites.length },
		suites,
		cases,
	}
	fs.writeFileSync(OUT, JSON.stringify(doc, null, '\t') + '\n')
	console.log(`wrote ${OUT}: ${cases.length} cases, ${suites.length} suites`, stats)
})().catch((e) => {
	console.error(e)
	process.exit(1)
})
