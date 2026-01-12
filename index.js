// TallyCCU Pro - Companion Module V3.0
// For Companion v4.x (compatible with v3.x)
// Blackmagic Design camera control via TallyCCU Pro
// Auto-generated - DO NOT EDIT MANUALLY

const { InstanceBase, Regex, runEntrypoint, InstanceStatus } = require('@companion-module/base');
const axios = require('axios');

class TallyCcuProInstance extends InstanceBase {
    constructor(internal) {
        super(internal);
        
        // Default config
        this.config = {
            host: '192.168.0.100',
            defaultCameraId: 1
        };
        
        // Almacenamiento para los valores actuales de los parametros
        this.paramValues = {};
        
        // Definition of all parameters and their default values
        this.paramDefaults = {};
        
        // Mapa de parametros a grupos
        this.paramGroupMap = {};
        
        // Storage for camera states
        this.cameraStates = {};
        
        // Initialize with empty states for all cameras
        for (let i = 1; i <= 8; i++) {
            this.cameraStates[i] = {};
        }
        
        // Variables for connection monitoring
        this.connectionStatus = 'unknown'; // 'unknown', 'ok', 'error'
        this.connectionTimer = null;
        this.pingInterval = 120000; // 2 minutes between each HTTP check (TCP is the main connection)
        this.reconnectAttempts = 0;
        this.maxReconnectAttempts = 3;
        
        // Variables de Companion
        this.variableDefinitions = [];
        
        // Storage for preset names
        this.presetNames = {};
        // === CLIENTE TCP PARA SINCRONIZACION PUSH ===
        this.tcpSocket = null;
        this.tcpConnected = false;
        this.tcpReconnectTimer = null;
        this.tcpReconnectInterval = 5000;
        this.tcpPingInterval = 30000;
        this.tcpPingTimer = null;
        this.tcpBuffer = '';
        this.ccuBroadcastPort = 8098;

    }
    
    // ========================================================================
    // MODULE CONFIGURATION
    // ========================================================================
    
    getConfigFields() {
        return [
            {
                type: 'static-text',
                id: 'info',
                width: 12,
                label: 'Information',
                value: 'Configure TallyCCU Pro IP to connect to Blackmagic cameras'
            },
            {
                type: 'textinput',
                id: 'host',
                label: 'TallyCCU Pro IP Address',
                width: 8,
                regex: Regex.IP,
                required: true,
                default: '192.168.0.100'
            },
            {
                type: 'number',
                id: 'defaultCameraId',
                label: 'Default Camera ID',
                width: 4,
                min: 1,
                max: 8,
                default: 1,
                required: true
            },
            {
                type: 'static-text',
                id: 'info2',
                width: 12,
                label: 'Note',
                value: 'Module variables update automatically when parameters are modified or presets are loaded.'
            }
        ];
    }
    
    // ========================================================================
    // INITIALIZATION AND LIFECYCLE (COMPANION V4 API)
    // ========================================================================
    
    async init(config, isFirstInit) {
        this.log('info', 'Inicializando modulo TallyCCU Pro v4.0');
        
        if (config) {
            this.config = config;
            this.log('info', 'Configured with IP: ' + (config.host || 'Not configured'));
        }
        
        // Initialize default parameters
        this.initParamDefaults();
        
        // Inicializar sistema de variables
        this.initVariableDefinitions();
        
        // Configurar las acciones disponibles
        this.setActionDefinitions(this.getActions());
        
        // Check initial connection
        if (this.config.host) {
            this.updateStatus(InstanceStatus.Connecting, 'Checking connection...');
            this.startConnectionMonitor();
            this.startTcpConnection();
        } else {
            this.updateStatus(InstanceStatus.BadConfig, 'IP not configured');
        }
    }
    
    async configUpdated(config) {
        this.log('info', 'Configuration updated');
        
        // Save previous config to compare
        const oldHost = this.config.host;
        
        // Update configuration
        this.config = config;
        
        // Actualizar acciones
        this.setActionDefinitions(this.getActions());
        
        // If IP changed or configured for first time, restart monitoring
        if (this.config.host !== oldHost) {
            this.log('info', 'IP address change detected, restarting connection monitoring');
            
            if (this.config.host) {
                this.updateStatus(InstanceStatus.Connecting, 'Checking connection...nueva IP...');
                this.stopTcpConnection();
        this.stopConnectionMonitor();
                this.startConnectionMonitor();
            this.startTcpConnection();
            } else {
                this.updateStatus(InstanceStatus.BadConfig, 'IP not configured');
                this.stopTcpConnection();
        this.stopConnectionMonitor();
            }
        }
    }
    
    async destroy() {
        this.log('info', 'Destruyendo instancia TallyCCU Pro');
        this.stopTcpConnection();
        this.stopConnectionMonitor();
    }
    
    // ========================================================================
    // CONNECTION MONITORING
    // ========================================================================
    
    async checkConnection() {
        if (!this.config.host) {
            this.connectionStatus = 'error';
            this.updateStatus(InstanceStatus.BadConfig, 'No IP configured');
            return false;
        }
        
        // If TCP is connected, use it as connection indicator
        // This prevents HTTP requests from interfering with TCP socket
        if (this.tcpConnected) {
            this.connectionStatus = 'ok';
            this.updateStatus(InstanceStatus.Ok, 'Conectado via TCP');
            return true;
        }

        this.log('debug', 'Checking connection...n TallyCCU Pro en ' + this.config.host);
        
        try {
            const url = 'http://' + this.config.host + '/?listPresets';
            const response = await axios.get(url, { timeout: 3000 });
            
            let validResponse = false;
            let presetsData = null;
            
            if (typeof response.data === 'object') {
                validResponse = response.data && (response.data.presets !== undefined);
                if (validResponse) presetsData = response.data;
            } else if (typeof response.data === 'string') {
                validResponse = response.data.includes('presets') || 
                               response.data.includes('TallyCCU') || 
                               (response.data.includes('{') && response.data.includes('}'));
                
                // Intentar extraer JSON para obtener nombres de presets
                if (validResponse) {
                    const jsonMatch = response.data.match(/\{.*\}/s);
                    if (jsonMatch) {
                        try {
                            presetsData = JSON.parse(jsonMatch[0]);
                        } catch (e) {
                            // No problem if parsing fails
                        }
                    }
                }
            }
            
            if (validResponse) {
                this.connectionStatus = 'ok';
                this.reconnectAttempts = 0;
                this.updateStatus(InstanceStatus.Ok, 'Conectado a TallyCCU Pro');
                this.log('debug', 'Connection verified successfully');
                
                // Load preset names if available
                if (presetsData && presetsData.presets && Array.isArray(presetsData.presets)) {
                    this.log('info', `Loading preset names from SD`);
                    this.updatePresetNames(presetsData.presets);
                }
                
                return true;
            } else {
                this.connectionStatus = 'error';
                this.reconnectAttempts++;
                this.updateStatus(InstanceStatus.ConnectionFailure, 'Respuesta incorrecta - No es un TallyCCU Pro');
                this.log('warn', 'Respuesta recibida pero no parece ser de un TallyCCU Pro');
                return false;
            }
        } catch (error) {
            this.connectionStatus = 'error';
            this.reconnectAttempts++;
            
            let errorMsg = 'Connection error';
            if (error.code === 'ECONNREFUSED') {
                errorMsg = 'Connection refused';
            } else if (error.code === 'ETIMEDOUT' || error.code === 'ECONNABORTED') {
                errorMsg = 'Tiempo de espera agotado';
            } else if (error.code === 'EHOSTUNREACH') {
                errorMsg = 'Host inalcanzable';
            }
            
            this.updateStatus(InstanceStatus.ConnectionFailure, errorMsg + ' (' + this.reconnectAttempts + '/' + this.maxReconnectAttempts + ')');
            this.log('error', 'Error checking connection: ' + error.message);
            return false;
        }
    }

    startConnectionMonitor() {
        this.log('info', 'Starting connection monitoring...');
        
        if (this.connectionTimer) {
            clearInterval(this.connectionTimer);
        }
        
        // Verificacion inicial inmediata
        this.checkConnection().then(connected => {
            if (connected) {
                this.log('info', 'Initial connection established successfully');
            } else {
                this.log('warn', 'Could not establish initial connection');
            }
        });
        
        // Verificacion periodica
        this.connectionTimer = setInterval(async () => {
            const connected = await this.checkConnection();
            
            if (!connected && this.reconnectAttempts >= this.maxReconnectAttempts) {
                clearInterval(this.connectionTimer);
                this.log('warn', 'Multiple connection failures, increasing check interval');
                this.pingInterval = 60000;
                this.connectionTimer = setInterval(() => this.checkConnection(), this.pingInterval);
            }
        }, this.pingInterval);
    }

    stopConnectionMonitor() {
        if (this.connectionTimer) {
            clearInterval(this.connectionTimer);
            this.connectionTimer = null;
        }
        this.log('debug', 'Connection monitoring stopped');
    }
    
    // ========================================================================
    // PARAMETER MANAGEMENT
    // ========================================================================
    
initParamDefaults() {
        // Usar un objeto simple como base
        this.paramDefaults = {};
        
        // Configure basic parameters
        this.paramDefaults['aperture_normalised'] = 0.0;
        this.paramDefaults['optical_image_stabilisation'] = true;
        this.paramDefaults['focus'] = 0.0;
        this.paramDefaults['set_absolute_zoom_normalised'] = 0.0;
        this.paramDefaults['set_continuous_zoom_speed'] = 0.0;
        this.paramDefaults['nd_filter_stop'] = [0, 0];
        this.paramDefaults['shutter_speed'] = 24.0;
        this.paramDefaults['gain_db'] = 0.0;
        this.paramDefaults['manual_white_balance'] = [5600, 10];
        this.paramDefaults['dynamic_range_mode'] = 2.0;
        this.paramDefaults['video_sharpening_level'] = 0.0;
        this.paramDefaults['set_auto_exposure_mode'] = 0.0;
        this.paramDefaults['video_mode'] = [60, 0, 6, 0, 0];
        this.paramDefaults['display_lut'] = [0, 0];
        this.paramDefaults['mic_level'] = 0.7;
        this.paramDefaults['headphone_level'] = 1.0;
        this.paramDefaults['headphone_program_mix'] = 0.0;
        this.paramDefaults['speaker_level'] = 1.0;
        this.paramDefaults['input_type'] = 0.0;
        this.paramDefaults['input_levels'] = [0.5, 0.5];
        this.paramDefaults['phantom_power'] = false;
        this.paramDefaults['overlays'] = [5, 100, 0, 9];
        this.paramDefaults['brightness'] = 1.0;
        this.paramDefaults['exposure_and_focus_tools'] = [0, 0];
        this.paramDefaults['zebra_level'] = 0.5;
        this.paramDefaults['peaking_level'] = 0.9;
        this.paramDefaults['color_bars_display_time_seconds'] = 0.0;
        this.paramDefaults['focus_assist'] = [1, 0];
        this.paramDefaults['program_return_feed_enable'] = 0.0;
        this.paramDefaults['timecode_source_0'] = 0.0;
        this.paramDefaults['tally_brightness'] = 1.0;
        this.paramDefaults['front_tally_brightness'] = 1.0;
        this.paramDefaults['rear_tally_brightness'] = 1.0;
        this.paramDefaults['source'] = 0.0;
        this.paramDefaults['offset'] = 0.0;
        this.paramDefaults['contrast_adjust'] = [0.5, 1];
        this.paramDefaults['color_adjust'] = [0.0, 1];
        this.paramDefaults['lift_adjust'] = [0.0, 0.0, 0.0, 0.0];
        this.paramDefaults['gamma_adjust'] = [0.0, 0.0, 0.0, 0.0];
        this.paramDefaults['gain_adjust'] = [1, 1, 1, 1];
        this.paramDefaults['offset_adjust'] = [0.0, 0.0, 0.0, 0.0];
        this.paramDefaults['luma_mix'] = 1.0;
        this.paramDefaults['pan_tilt_velocity'] = [0.0, 0.0];
        this.paramDefaults['memory_preset'] = [2, 1];

        // Initialize parameter to group map
        this.paramGroupMap = {};
            this.paramGroupMap['aperture_normalised'] = 'lens';
        this.paramGroupMap['instantaneous_auto_aperture'] = 'lens';
        this.paramGroupMap['optical_image_stabilisation'] = 'lens';
        this.paramGroupMap['focus'] = 'lens';
        this.paramGroupMap['instantaneous_autofocus'] = 'lens';
        this.paramGroupMap['set_absolute_zoom_normalised'] = 'lens';
        this.paramGroupMap['set_continuous_zoom_speed'] = 'lens';
        this.paramGroupMap['nd_filter_stop'] = 'video';
        this.paramGroupMap['shutter_speed'] = 'video';
        this.paramGroupMap['gain_db'] = 'video';
        this.paramGroupMap['manual_white_balance'] = 'video';
        this.paramGroupMap['set_auto_wb'] = 'video';
        this.paramGroupMap['restore_auto_wb'] = 'video';
        this.paramGroupMap['dynamic_range_mode'] = 'video';
        this.paramGroupMap['video_sharpening_level'] = 'video';
        this.paramGroupMap['set_auto_exposure_mode'] = 'video';
        this.paramGroupMap['video_mode'] = 'video';
        this.paramGroupMap['display_lut'] = 'video';
        this.paramGroupMap['mic_level'] = 'audio';
        this.paramGroupMap['headphone_level'] = 'audio';
        this.paramGroupMap['headphone_program_mix'] = 'audio';
        this.paramGroupMap['speaker_level'] = 'audio';
        this.paramGroupMap['input_type'] = 'audio';
        this.paramGroupMap['input_levels'] = 'audio';
        this.paramGroupMap['phantom_power'] = 'audio';
        this.paramGroupMap['overlays'] = 'output';
        this.paramGroupMap['brightness'] = 'display';
        this.paramGroupMap['exposure_and_focus_tools'] = 'display';
        this.paramGroupMap['zebra_level'] = 'display';
        this.paramGroupMap['peaking_level'] = 'display';
        this.paramGroupMap['color_bars_display_time_seconds'] = 'display';
        this.paramGroupMap['focus_assist'] = 'display';
        this.paramGroupMap['program_return_feed_enable'] = 'display';
        this.paramGroupMap['timecode_source_0'] = 'display';
        this.paramGroupMap['tally_brightness'] = 'tally';
        this.paramGroupMap['front_tally_brightness'] = 'tally';
        this.paramGroupMap['rear_tally_brightness'] = 'tally';
        this.paramGroupMap['source'] = 'reference';
        this.paramGroupMap['offset'] = 'reference';
        this.paramGroupMap['contrast_adjust'] = 'color_correction';
        this.paramGroupMap['color_adjust'] = 'color_correction';
        this.paramGroupMap['lift_adjust'] = 'color_correction';
        this.paramGroupMap['gamma_adjust'] = 'color_correction';
        this.paramGroupMap['gain_adjust'] = 'color_correction';
        this.paramGroupMap['offset_adjust'] = 'color_correction';
        this.paramGroupMap['luma_mix'] = 'color_correction';
        this.paramGroupMap['correction_reset_default'] = 'color_correction';
        this.paramGroupMap['pan_tilt_velocity'] = 'ptz_control';
        this.paramGroupMap['memory_preset'] = 'ptz_control';
    }
    
    getParamValue(paramKey, defaultValue, cameraId = null) {
        const camId = cameraId !== null ? cameraId : this.config.defaultCameraId;
        const cameraKey = 'cam' + camId + '_' + paramKey;
        
        if (this.paramValues[cameraKey] !== undefined) {
            return this.paramValues[cameraKey];
        }
        
        return defaultValue;
    }
    
    storeParamValue(paramKey, value, cameraId = null) {
        const camId = cameraId !== null ? cameraId : this.config.defaultCameraId;
        const cameraKey = 'cam' + camId + '_' + paramKey;
        this.paramValues[cameraKey] = value;
    }
    
    async sendParam(cameraId, paramKey, val) {
        if (this.connectionStatus === 'error') {
            this.log('debug', 'Connection in error state, trying to reconnect before sending parameter');
            const connected = await this.checkConnection();
            if (!connected) {
                this.log('warn', 'Could not establish connection to send parameter');
                return;
            }
        }
        
        if (!this.config.host) {
            this.log('error', 'TallyCCU Pro IP not configured');
            this.updateStatus(InstanceStatus.BadConfig, 'No IP configured');
            return;
        }
        
        if (!this.cameraStates[cameraId]) {
            this.cameraStates[cameraId] = {};
        }
        
        this.cameraStates[cameraId][paramKey] = val;
        this.log('debug', 'Tracking: Camera ' + cameraId + ', param ' + paramKey + ' = ' + val);
        
        this.storeParamValue(paramKey, val, cameraId);
        
        // Actualizar variables
        this.updateVariablesFromParams(cameraId, paramKey, val);
        
        const url = 'http://' + this.config.host + '/?cameraId=' + cameraId + '&' + paramKey + '=' + encodeURIComponent(val);
        this.log('debug', 'Enviando GET -> ' + url);
        
        try {
            const res = await axios.get(url, { timeout: 3000 });
            this.log('debug', 'Respuesta: ' + res.status + ' ' + res.statusText);
            this.connectionStatus = 'ok';
            this.reconnectAttempts = 0;
            this.updateStatus(InstanceStatus.Ok, 'Conectado');
        } catch (err) {
            this.connectionStatus = 'error';
            this.log('error', 'Error en la solicitud: ' + err.message);
            this.updateStatus(InstanceStatus.ConnectionFailure, 'Connection error');
        }
    }
    
    captureCurrentState(cameraId) {
        const state = this.cameraStates[cameraId] || {};
        state.cameraId = cameraId;
        
        for (const key in this.paramDefaults) {
            if (this.paramDefaults.hasOwnProperty(key)) {
                const defaultValue = this.paramDefaults[key];
                if (state[key] === undefined) {
                    state[key] = defaultValue;
                }
            }
        }
        
        return state;
    }
    
    updateParameterValues(cameraId, parameters) {
        this.log('debug', 'Updating internal values for camera ' + cameraId);
        
        if (!parameters) {
            this.log('warn', 'No parameters received to update');
            return;
        }
        
        for (const paramKey in parameters) {
            if (!parameters.hasOwnProperty(paramKey)) continue;
            const value = parameters[paramKey];
            
            if (paramKey === 'name') continue;
            
            const cameraKey = 'cam' + cameraId + '_' + paramKey;
            
            if (Array.isArray(value)) {
                this.paramValues[cameraKey] = value;
                
                for (let index = 0; index < value.length; index++) {
                    const subValue = value[index];
                    const subKey = 'cam' + cameraId + '_' + paramKey + '_' + index;
                    this.paramValues[subKey] = subValue;
                }
            } else if (typeof value === 'string' && value.indexOf(',') >= 0) {
                const valueArray = value.split(',').map(v => {
                    const numValue = parseFloat(v);
                    return isNaN(numValue) ? v : numValue;
                });
                
                this.paramValues[cameraKey] = valueArray;
                
                for (let index = 0; index < valueArray.length; index++) {
                    const subKey = 'cam' + cameraId + '_' + paramKey + '_' + index;
                    this.paramValues[subKey] = valueArray[index];
                }
            } else {
                this.paramValues[cameraKey] = value;
            }
            
            if (!this.cameraStates[cameraId]) {
                this.cameraStates[cameraId] = {};
            }
            this.cameraStates[cameraId][paramKey] = value;
            
            // Actualizar variables
            this.updateVariablesFromParams(cameraId, paramKey, value);
        }
        
        this.log('info', 'Internal values updated for camera ' + cameraId);
    }
    
    // ========================================================================
    // SISTEMA DE VARIABLES
    // ========================================================================
    
    initVariableDefinitions() {
        this.log('info', 'Inicializando definiciones de variables');
        
        this.variableDefinitions = [];
        
        // Variables for each camera
        for (let camId = 1; camId <= 8; camId++) {
            this.variableDefinitions.push({
                name: `Camera ${camId} - Active Preset Name`,
                variableId: `cam${camId}_active_preset_name`
            });
            
            this.variableDefinitions.push({
                name: `Camera ${camId} - Active Preset ID`,
                variableId: `cam${camId}_active_preset_id`
            });
            
            for (let presetId = 0; presetId <= 4; presetId++) {
                this.variableDefinitions.push({
                    name: `Camera ${camId} - Preset ${presetId} Name`,
                    variableId: `cam${camId}_preset${presetId}_name`
                });
            }
            
            // Variables para parametros
            for (const paramKey in this.paramDefaults) {
                this.variableDefinitions.push({
                    name: `Camera ${camId} - ${paramKey}`,
                    variableId: `cam${camId}_param_${paramKey}`
                });
                
                if (Array.isArray(this.paramDefaults[paramKey])) {
                    for (let i = 0; i < this.paramDefaults[paramKey].length; i++) {
                        this.variableDefinitions.push({
                            name: `Camera ${camId} - ${paramKey} (${i})`,
                            variableId: `cam${camId}_param_${paramKey}_${i}`
                        });
                    }
                }
            }
        }
        
        // Variables de compatibilidad
        this.variableDefinitions.push({
            name: 'Current Preset Name',
            variableId: 'current_preset_name'
        });
        
        this.variableDefinitions.push({
            name: 'Current Preset ID',
            variableId: 'current_preset_id'
        });
        
        for (let presetId = 0; presetId <= 4; presetId++) {
            this.variableDefinitions.push({
                name: `Preset ${presetId} Name`,
                variableId: `preset${presetId}_name`
            });
        }
        
        this.setVariableDefinitions(this.variableDefinitions);
        this.updateAllVariablesToDefaults();
    }
    
    updateAllVariablesToDefaults() {
        const variables = {};
        
        for (let camId = 1; camId <= 8; camId++) {
            variables[`cam${camId}_active_preset_name`] = 'None';
            variables[`cam${camId}_active_preset_id`] = '-';
            
            for (let presetId = 0; presetId <= 4; presetId++) {
                variables[`cam${camId}_preset${presetId}_name`] = `Preset ${presetId}`;
            }
            
            for (const paramKey in this.paramDefaults) {
                const defaultValue = this.paramDefaults[paramKey];
                variables[`cam${camId}_param_${paramKey}`] = this.formatVariableValue(defaultValue);
                
                if (Array.isArray(defaultValue)) {
                    for (let i = 0; i < defaultValue.length; i++) {
                        variables[`cam${camId}_param_${paramKey}_${i}`] = this.formatVariableValue(defaultValue[i]);
                    }
                }
            }
        }
        
        variables['current_preset_name'] = 'None';
        variables['current_preset_id'] = '-';
        
        for (let presetId = 0; presetId <= 4; presetId++) {
            variables[`preset${presetId}_name`] = `Preset ${presetId}`;
        }
        
        this.setVariableValues(variables);
    }
    
    formatVariableValue(value) {
        if (value === null || value === undefined) {
            return 'N/A';
        }
        
        if (typeof value === 'number') {
            if (Number.isInteger(value) || Math.abs(value - Math.round(value)) < 0.001) {
                return Math.round(value).toString();
            } else {
                return value.toFixed(2);
            }
        }
        
        if (Array.isArray(value)) {
            return value.map(v => this.formatVariableValue(v)).join(', ');
        }
        
        return String(value);
    }
    
    updateVariablesFromParams(cameraId, paramKey, value) {
        const variables = {};
        
        // Convertir string con comas a array si es necesario
        let processedValue = value;
        if (typeof value === 'string' && value.indexOf(',') >= 0) {
            processedValue = value.split(',').map(v => {
                const num = parseFloat(v.trim());
                return isNaN(num) ? v.trim() : num;
            });
        }
        
        // Variable principal del parametro
        variables[`cam${cameraId}_param_${paramKey}`] = this.formatVariableValue(processedValue);
        
        if (cameraId == this.config.defaultCameraId) {
            variables[`param_${paramKey}`] = this.formatVariableValue(processedValue);
        }
        
        // If array, also update each individual subindex
        if (Array.isArray(processedValue)) {
            for (let i = 0; i < processedValue.length; i++) {
                const subValue = processedValue[i];
                variables[`cam${cameraId}_param_${paramKey}_${i}`] = this.formatVariableValue(subValue);
                
                if (cameraId == this.config.defaultCameraId) {
                    variables[`param_${paramKey}_${i}`] = this.formatVariableValue(subValue);
                }
                
                // Also update internal storage for subindexes
                const subKey = `cam${cameraId}_${paramKey}_${i}`;
                this.paramValues[subKey] = subValue;
            }
            
            // Store complete array too
            this.paramValues[`cam${cameraId}_${paramKey}`] = processedValue;
        }
        
        this.setVariableValues(variables);
    }
    
    // Metodo auxiliar para actualizar solo un subindice especifico
    updateSubIndexVariable(cameraId, paramKey, subIndex, value) {
        const variables = {};
        
        variables[`cam${cameraId}_param_${paramKey}_${subIndex}`] = this.formatVariableValue(value);
        
        if (cameraId == this.config.defaultCameraId) {
            variables[`param_${paramKey}_${subIndex}`] = this.formatVariableValue(value);
        }
        
        this.setVariableValues(variables);
    }
    
    updatePresetNames(presets) {
        if (!Array.isArray(presets)) {
            this.log('warn', 'updatePresetNames: presets no es un array');
            return;
        }
        
        const variables = {};
        
        for (const preset of presets) {
            if (preset && preset.cameraId !== undefined && preset.presetId !== undefined) {
                const cameraId = parseInt(preset.cameraId);
                const presetId = parseInt(preset.presetId);
                const presetName = preset.name || `Preset ${presetId}`;
                
                variables[`cam${cameraId}_preset${presetId}_name`] = presetName;
                
                if (cameraId == this.config.defaultCameraId) {
                    variables[`preset${presetId}_name`] = presetName;
                }
                
                if (!this.presetNames[cameraId]) this.presetNames[cameraId] = {};
                this.presetNames[cameraId][presetId] = presetName;
            }
        }
        
        if (Object.keys(variables).length > 0) {
            this.setVariableValues(variables);
            this.log('info', `Actualizadas ${Object.keys(variables).length} variables de nombres de presets`);
        }
    }
    

    // ========================================================================
    // CLIENTE TCP PARA SINCRONIZACION PUSH
    // ========================================================================
    
    startTcpConnection() {
        if (!this.config.host) return;
        
        // Cancel any pending reconnection FIRST
        if (this.tcpReconnectTimer) {
            clearTimeout(this.tcpReconnectTimer);
            this.tcpReconnectTimer = null;
        }
        
        // Close existing connection if any
        if (this.tcpSocket) {
            this.log('debug', 'Closing existing TCP connection');
            // IMPORTANT: remove listeners BEFORE destroy to prevent reconnection
            this.tcpSocket.removeAllListeners();
            this.tcpSocket.destroy();
            this.tcpSocket = null;
            this.tcpConnected = false;
        }
        
        const net = require('net');
        this.log('info', `Conectando TCP a ${this.config.host}:${this.ccuBroadcastPort}...`);
        
        this.tcpSocket = new net.Socket();
        this.tcpBuffer = '';
        this.tcpSocket.setTimeout(5000);
        
        this.tcpSocket.on('connect', () => {
            this.log('info', 'TCP conectado al servidor CCU');
            this.tcpConnected = true;
            this.tcpSocket.setTimeout(0);
            this.tcpSocket.write('SUBSCRIBE CCU\r\n');
            this.startTcpPing();
        });
        
        this.tcpSocket.on('data', (data) => {
            this.tcpBuffer += data.toString();
            this.processTcpBuffer();
        });
        
        this.tcpSocket.on('close', () => {
            this.log('info', 'TCP desconectado');
            this.tcpConnected = false;
            this.stopTcpPing();
            // Solo reconectar si este socket sigue siendo el actual
            // (avoids duplicate reconnections if a new socket was already created)
            if (this.tcpSocket === null || !this.tcpSocket.connecting) {
                this.scheduleTcpReconnect();
            }
        });
        
        this.tcpSocket.on('error', (err) => {
            this.log('debug', `TCP error: ${err.message}`);
            this.tcpConnected = false;
        });
        
        this.tcpSocket.on('timeout', () => {
            this.log('warn', 'TCP timeout');
            this.tcpSocket.destroy();
        });
        
        this.tcpSocket.connect(this.ccuBroadcastPort, this.config.host);
    }
    
    stopTcpConnection() {
        this.stopTcpPing();
        if (this.tcpReconnectTimer) { clearTimeout(this.tcpReconnectTimer); this.tcpReconnectTimer = null; }
        if (this.tcpSocket) { this.tcpSocket.destroy(); this.tcpSocket = null; }
        this.tcpConnected = false;
    }
    
    scheduleTcpReconnect() {
        if (this.tcpReconnectTimer) clearTimeout(this.tcpReconnectTimer);
        this.tcpReconnectTimer = setTimeout(() => {
            if (this.config.host && !this.tcpConnected) {
                this.log('debug', 'Reconectando TCP...');
                this.startTcpConnection();
            }
        }, this.tcpReconnectInterval);
    }
    
    startTcpPing() {
        this.stopTcpPing();
        this.tcpPingTimer = setInterval(() => {
            if (this.tcpSocket && this.tcpConnected) this.tcpSocket.write('PING\r\n');
        }, this.tcpPingInterval);
    }
    
    stopTcpPing() {
        if (this.tcpPingTimer) { clearInterval(this.tcpPingTimer); this.tcpPingTimer = null; }
    }
    
    processTcpBuffer() {
        const lines = this.tcpBuffer.split(/\r?\n/);
        this.tcpBuffer = lines.pop() || '';
        for (const line of lines) {
            if (line.trim()) this.processTcpMessage(line.trim());
        }
    }
    
    processTcpMessage(message) {
        this.log('debug', `TCP rx: ${message}`);
        
        if (message.startsWith('CCU ')) {
            const parts = message.substring(4).split(' ');
            if (parts.length >= 3) {
                const cameraId = parseInt(parts[0]);
                const paramKey = parts[1];
                const value = parts.slice(2).join(' ');
                this.handleCcuChange(cameraId, paramKey, value);
            }
            return;
        }
        
        // PRESETSAVED must come BEFORE PRESET (because PRESETSAVED starts with PRESET)
        if (message.startsWith('PRESETSAVED ')) {
            const parts = message.substring(12).split(' ');
            if (parts.length >= 2) {
                const cameraId = parseInt(parts[0]);
                const presetId = parseInt(parts[1]);
                const presetName = parts.slice(2).join(' ') || `Preset ${presetId}`;
                this.handlePresetSaved(cameraId, presetId, presetName);
            }
            return;
        }
        
        if (message.startsWith('PRESET ')) {
            const parts = message.substring(7).split(' ');
            if (parts.length >= 2) {
                const cameraId = parseInt(parts[0]);
                const presetId = parseInt(parts[1]);
                const presetName = parts.slice(2).join(' ') || `Preset ${presetId}`;
                this.handlePresetChange(cameraId, presetId, presetName);
            }
            return;
        }
        
        if (message === 'SUBSCRIBED OK') {
            this.log('info', 'Suscrito a cambios CCU via TCP');
        }
    }
    
    handleCcuChange(cameraId, paramKey, value) {
        this.log('debug', `CCU push: Cam${cameraId} ${paramKey}=${value}`);
        
        let parsedValue = value;
        if (value.includes(',')) {
            parsedValue = value.split(',').map(v => {
                const num = parseFloat(v.trim());
                return isNaN(num) ? v.trim() : num;
            });
        } else {
            const num = parseFloat(value);
            parsedValue = isNaN(num) ? value : num;
        }
        
        const cameraKey = 'cam' + cameraId + '_' + paramKey;
        this.paramValues[cameraKey] = parsedValue;
        
        if (!this.cameraStates[cameraId]) this.cameraStates[cameraId] = {};
        this.cameraStates[cameraId][paramKey] = parsedValue;
        
        this.updateVariablesFromParams(cameraId, paramKey, parsedValue);
    }
    
    handlePresetChange(cameraId, presetId, presetName) {
        this.log('info', `Preset push: Cam${cameraId} P${presetId} "${presetName}"`);
        
        const variables = {};
        variables[`cam${cameraId}_active_preset_name`] = presetName;
        variables[`cam${cameraId}_active_preset_id`] = presetId.toString();
        
        if (cameraId === this.config.defaultCameraId) {
            variables['current_preset_name'] = presetName;
            variables['current_preset_id'] = presetId.toString();
        }
        
        this.setVariableValues(variables);
    }
    
    handlePresetSaved(cameraId, presetId, presetName) {
        this.log('info', `Preset guardado: Cam${cameraId} P${presetId} "${presetName}"`);
        
        // Actualizar cache interno de nombres
        if (!this.presetNames) this.presetNames = {};
        if (!this.presetNames[cameraId]) this.presetNames[cameraId] = {};
        this.presetNames[cameraId][presetId] = presetName;
        
        // Actualizar variables del nombre del preset
        const variables = {};
        variables[`cam${cameraId}_preset${presetId}_name`] = presetName;
        
        if (cameraId === this.config.defaultCameraId) {
            variables[`preset${presetId}_name`] = presetName;
        }
        
        this.setVariableValues(variables);
        this.log('debug', `Variable actualizada: cam${cameraId}_preset${presetId}_name = "${presetName}"`);
    }
    
    // ========================================================================
    // ACCIONES
    // ========================================================================
    
    getActions() {
        const actions = {};

        // Action for Aperture (normalised) (numeric)
        actions['set_aperture_normalised'] = {
            name: 'Set Aperture (normalised)',
            description: 'Group: Lens | Param: Aperture (normalised) | Note: 0.0 = Smallest, 1.0 = Largest',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'aperture_normalised', event.options.value);
            }
        };
        // Action to increment Aperture (normalised)
        actions['set_aperture_normalised_increment'] = {
            name: '⬆️ Increase Aperture (normalised)',
            description: 'Increase the value of Aperture (normalised)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('aperture_normalised', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'aperture_normalised', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('aperture_normalised', newValue, cameraId);
            }
        };
        // Action to decrement Aperture (normalised)
        actions['set_aperture_normalised_decrement'] = {
            name: '⬇️ Decrease Aperture (normalised)',
            description: 'Decrease the value of Aperture (normalised)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('aperture_normalised', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'aperture_normalised', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('aperture_normalised', newValue, cameraId);
            }
        };
        // Action to reset Aperture (normalised) al default value
        actions['set_aperture_normalised_reset'] = {
            name: '🔄 Reset Aperture (normalised)',
            description: 'Reset to default value (0.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'aperture_normalised', 0.0);
                
                // Store default value for this specific camera
                this.storeParamValue('aperture_normalised', 0.0, cameraId);
            }
        };
        // Action for Instantaneous auto aperture (void)
        actions['set_instantaneous_auto_aperture'] = {
            name: 'Trigger Instantaneous auto aperture',
            description: 'Group: Lens | Param: Instantaneous auto aperture | Note: Trigger Instantaneous Auto Aperture',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'instantaneous_auto_aperture', '1');
            }
        };
        // Action for Optical image stabilisation (boolean)
        actions['set_optical_image_stabilisation'] = {
            name: 'Set Optical image stabilisation',
            description: 'Group: Lens | Param: Optical image stabilisation | Note: True = Enabled, False = Disabled',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'checkbox',
                    label: 'Value',
                    id: 'value',
                    default: true
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const value = event.options.value ? 1 : 0;
                this.sendParam(cameraId, 'optical_image_stabilisation', value);
            }
        };
        // Action for Focus (numeric)
        actions['set_focus'] = {
            name: 'Set Focus',
            description: 'Group: Lens | Param: Focus | Note: 0.0 = Near, 1.0 = Far',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'focus', event.options.value);
            }
        };
        // Action to increment Focus
        actions['set_focus_increment'] = {
            name: '⬆️ Increase Focus',
            description: 'Increase the value of Focus',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('focus', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'focus', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('focus', newValue, cameraId);
            }
        };
        // Action to decrement Focus
        actions['set_focus_decrement'] = {
            name: '⬇️ Decrease Focus',
            description: 'Decrease the value of Focus',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('focus', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'focus', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('focus', newValue, cameraId);
            }
        };
        // Action to reset Focus al default value
        actions['set_focus_reset'] = {
            name: '🔄 Reset Focus',
            description: 'Reset to default value (0.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'focus', 0.0);
                
                // Store default value for this specific camera
                this.storeParamValue('focus', 0.0, cameraId);
            }
        };
        // Action for Instantaneous autofocus (void)
        actions['set_instantaneous_autofocus'] = {
            name: 'Trigger Instantaneous autofocus',
            description: 'Group: Lens | Param: Instantaneous autofocus | Note: Trigger Instantaneous Autofocus',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'instantaneous_autofocus', '1');
            }
        };
        // Action for Set absolute zoom (normalised) (numeric)
        actions['set_set_absolute_zoom_normalised'] = {
            name: 'Set Set absolute zoom (normalised)',
            description: 'Group: Lens | Param: Set absolute zoom (normalised) | Note: Move to specified focal length: 0.0 = wide, 1.0 = tele',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'set_absolute_zoom_normalised', event.options.value);
            }
        };
        // Action to increment Set absolute zoom (normalised)
        actions['set_set_absolute_zoom_normalised_increment'] = {
            name: '⬆️ Increase Set absolute zoom (normalised)',
            description: 'Increase the value of Set absolute zoom (normalised)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('set_absolute_zoom_normalised', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'set_absolute_zoom_normalised', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('set_absolute_zoom_normalised', newValue, cameraId);
            }
        };
        // Action to decrement Set absolute zoom (normalised)
        actions['set_set_absolute_zoom_normalised_decrement'] = {
            name: '⬇️ Decrease Set absolute zoom (normalised)',
            description: 'Decrease the value of Set absolute zoom (normalised)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('set_absolute_zoom_normalised', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'set_absolute_zoom_normalised', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('set_absolute_zoom_normalised', newValue, cameraId);
            }
        };
        // Action to reset Set absolute zoom (normalised) al default value
        actions['set_set_absolute_zoom_normalised_reset'] = {
            name: '🔄 Reset Set absolute zoom (normalised)',
            description: 'Reset to default value (0.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'set_absolute_zoom_normalised', 0.0);
                
                // Store default value for this specific camera
                this.storeParamValue('set_absolute_zoom_normalised', 0.0, cameraId);
            }
        };
        // Action for Zoom continuo - Inicio
        actions['zoom_start'] = {
            name: 'Zoom - Iniciar',
            description: 'Start zoom in a direction',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'dropdown',
                    label: 'Direction',
                    id: 'direction',
                    default: 'in',
                    choices: [
                        { id: 'in', label: 'Zoom In (Tele)' },
                        { id: 'out', label: 'Zoom Out (Wide)' }
                    ]
                },
                {
                    type: 'number',
                    label: 'Speed (0-1)',
                    id: 'speed',
                    default: 0.5,
                    min: 0,
                    max: 1,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const direction = event.options.direction;
                const speed = parseFloat(event.options.speed);
                const value = direction === 'in' ? speed : -speed;
                this.sendParam(cameraId, 'set_continuous_zoom_speed', value);
            }
        };
        
        // Action for Zoom continuo - Detener
        actions['zoom_stop'] = {
            name: 'Zoom - Detener',
            description: 'Stop any zoom movement',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'set_continuous_zoom_speed', 0);
            }
        };
        // Action for ND Filter Stop (multiple subindexes)
        actions['set_nd_filter_stop'] = {
            name: 'Set ND Filter Stop',
            description: 'Set values for ND Filter Stop',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Filter power as f-stop',
                    id: 'value0',
                    default: 0.0,
                    min: 0.0,
                    max: 15.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: '0 = Stop, 1 = Density, 2 = Transmitance',
                    id: 'value1',
                    default: 0.0,
                    min: 0.0,
                    max: 2.0,
                    step: 0.1
                },
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const values = [];
                values[0] = event.options.value0;
                values[1] = event.options.value1;
                const valuesString = values.join(",");
                this.sendParam(cameraId, 'nd_filter_stop', valuesString);
                
                // Store individual values for each subindex
                
                this.storeParamValue('nd_filter_stop_0', event.options.value0, cameraId);
                this.storeParamValue('nd_filter_stop_1', event.options.value1, cameraId);
            }
        };
        // Action to set only ND Filter Stop: Filter power as f-stop
        actions['set_nd_filter_stop_0'] = {
            name: 'Set ND Filter Stop: Filter power as f-stop',
            description: 'Set value for ND Filter Stop: Filter power as f-stop',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 15.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For the subindex being modified, use the new value
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('nd_filter_stop_1', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('nd_filter_stop_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'nd_filter_stop', valuesString);
            }
        };
        // Action to increment ND Filter Stop: Filter power as f-stop
        actions['set_nd_filter_stop_0_increment'] = {
            name: '⬆️ Increase ND Filter Stop: Filter power as f-stop',
            description: 'Increase the value of ND Filter Stop: Filter power as f-stop',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 15.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('nd_filter_stop_0', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(15.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value incrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('nd_filter_stop_1', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('nd_filter_stop_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'nd_filter_stop', valuesString);
            }
        };
        // Action to decrement ND Filter Stop: Filter power as f-stop
        actions['set_nd_filter_stop_0_decrement'] = {
            name: '⬇️ Decrease ND Filter Stop: Filter power as f-stop',
            description: 'Decrease the value of ND Filter Stop: Filter power as f-stop',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 15.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('nd_filter_stop_0', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value decrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('nd_filter_stop_1', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('nd_filter_stop_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'nd_filter_stop', valuesString);
            }
        };
        // Action to reset ND Filter Stop: Filter power as f-stop al default value
        actions['set_nd_filter_stop_0_reset'] = {
            name: '🔄 Reset ND Filter Stop: Filter power as f-stop',
            description: 'Reset to default value (0.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 0.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being reset, use its default value
                values[0] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('nd_filter_stop_1', 0.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('nd_filter_stop_0', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'nd_filter_stop', valuesString);
            }
        };
        // Action to set only ND Filter Stop: 0 = Stop, 1 = Density, 2 = Transmitance
        actions['set_nd_filter_stop_1'] = {
            name: 'Set ND Filter Stop: 0 = Stop, 1 = Density, 2 = Transmitance',
            description: 'Set value for ND Filter Stop: 0 = Stop, 1 = Density, 2 = Transmitance',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 2.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('nd_filter_stop_0', 0.0, cameraId);
                // For the subindex being modified, use the new value
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('nd_filter_stop_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'nd_filter_stop', valuesString);
            }
        };
        // Action to increment ND Filter Stop: 0 = Stop, 1 = Density, 2 = Transmitance
        actions['set_nd_filter_stop_1_increment'] = {
            name: '⬆️ Increase ND Filter Stop: 0 = Stop, 1 = Density, 2 = Transmitance',
            description: 'Increase the value of ND Filter Stop: 0 = Stop, 1 = Density, 2 = Transmitance',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 2.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('nd_filter_stop_1', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(2.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('nd_filter_stop_0', 0.0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('nd_filter_stop_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'nd_filter_stop', valuesString);
            }
        };
        // Action to decrement ND Filter Stop: 0 = Stop, 1 = Density, 2 = Transmitance
        actions['set_nd_filter_stop_1_decrement'] = {
            name: '⬇️ Decrease ND Filter Stop: 0 = Stop, 1 = Density, 2 = Transmitance',
            description: 'Decrease the value of ND Filter Stop: 0 = Stop, 1 = Density, 2 = Transmitance',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 2.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('nd_filter_stop_1', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('nd_filter_stop_0', 0.0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('nd_filter_stop_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'nd_filter_stop', valuesString);
            }
        };
        // Action to reset ND Filter Stop: 0 = Stop, 1 = Density, 2 = Transmitance al default value
        actions['set_nd_filter_stop_1_reset'] = {
            name: '🔄 Reset ND Filter Stop: 0 = Stop, 1 = Density, 2 = Transmitance',
            description: 'Reset to default value (0.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 0.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('nd_filter_stop_0', 0.0, cameraId);
                // For the subindex being reset, use its default value
                values[1] = resetValue;
                
                // Store default value specifically for this camera
                this.storeParamValue('nd_filter_stop_1', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'nd_filter_stop', valuesString);
            }
        };
        // Action for Shutter speed (numeric)
        actions['set_shutter_speed'] = {
            name: 'Set Shutter speed',
            description: 'Group: Video | Param: Shutter speed | Note: Shutter speed value as a fraction of 1, so 50 for 1/50th of a second',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 24.0,
                    min: 24.0,
                    max: 2000.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'shutter_speed', event.options.value);
            }
        };
        // Action to increment Shutter speed
        actions['set_shutter_speed_increment'] = {
            name: '⬆️ Increase Shutter speed',
            description: 'Increase the value of Shutter speed',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 1976.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('shutter_speed', 24.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(2000.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'shutter_speed', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('shutter_speed', newValue, cameraId);
            }
        };
        // Action to decrement Shutter speed
        actions['set_shutter_speed_decrement'] = {
            name: '⬇️ Decrease Shutter speed',
            description: 'Decrease the value of Shutter speed',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 1976.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('shutter_speed', 24.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(24.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'shutter_speed', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('shutter_speed', newValue, cameraId);
            }
        };
        // Action to reset Shutter speed al default value
        actions['set_shutter_speed_reset'] = {
            name: '🔄 Reset Shutter speed',
            description: 'Reset to default value (24)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'shutter_speed', 24.0);
                
                // Store default value for this specific camera
                this.storeParamValue('shutter_speed', 24.0, cameraId);
            }
        };
        // Action for Gain(db) (numeric)
        actions['set_gain_db'] = {
            name: 'Set Gain(db)',
            description: 'Group: Video | Param: Gain(db) | Note: Gain in decibel (dB)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: -12.0,
                    max: 36.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'gain_db', event.options.value);
            }
        };
        // Action to increment Gain(db)
        actions['set_gain_db_increment'] = {
            name: '⬆️ Increase Gain(db)',
            description: 'Increase the value of Gain(db)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 48.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gain_db', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(36.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'gain_db', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('gain_db', newValue, cameraId);
            }
        };
        // Action to decrement Gain(db)
        actions['set_gain_db_decrement'] = {
            name: '⬇️ Decrease Gain(db)',
            description: 'Decrease the value of Gain(db)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 48.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gain_db', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(-12.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'gain_db', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('gain_db', newValue, cameraId);
            }
        };
        // Action to reset Gain(db) al default value
        actions['set_gain_db_reset'] = {
            name: '🔄 Reset Gain(db)',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'gain_db', 0.0);
                
                // Store default value for this specific camera
                this.storeParamValue('gain_db', 0.0, cameraId);
            }
        };
        // Action for Manual White Balance (multiple subindexes)
        actions['set_manual_white_balance'] = {
            name: 'Set Manual White Balance',
            description: 'Set values for Manual White Balance',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Color temp (2500-10000 K)',
                    id: 'value0',
                    default: 5600.0,
                    min: 2500.0,
                    max: 10000.0,
                    step: 1
                },
                {
                    type: 'number',
                    label: 'Tint (-50 to 50)',
                    id: 'value1',
                    default: 10.0,
                    min: -50.0,
                    max: 50.0,
                    step: 1
                },
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const values = [];
                values[0] = event.options.value0;
                values[1] = event.options.value1;
                const valuesString = values.join(",");
                this.sendParam(cameraId, 'manual_white_balance', valuesString);
                
                // Store individual values for each subindex
                
                this.storeParamValue('manual_white_balance_0', event.options.value0, cameraId);
                this.storeParamValue('manual_white_balance_1', event.options.value1, cameraId);
            }
        };
        // Action to set onlyr temp (2500-10000 K)
        actions['set_manual_white_balance_0'] = {
            name: 'Set Manual White Balance: Color temp (2500-10000 K)',
            description: 'Set value for Manual White Balance: Color temp (2500-10000 K)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 5600.0,
                    min: 2500.0,
                    max: 10000.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For the subindex being modified, use the new value
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('manual_white_balance_1', 10.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('manual_white_balance_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'manual_white_balance', valuesString);
            }
        };
        // Action to increment Manual White Balance: Color temp (2500-10000 K)
        actions['set_manual_white_balance_0_increment'] = {
            name: '⬆️ Increase Manual White Balance: Color temp (2500-10000 K)',
            description: 'Increase the value of Manual White Balance: Color temp (2500-10000 K)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 7500.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('manual_white_balance_0', 5600.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(10000.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value incrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('manual_white_balance_1', 10.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('manual_white_balance_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'manual_white_balance', valuesString);
            }
        };
        // Action to decrement Manual White Balance: Color temp (2500-10000 K)
        actions['set_manual_white_balance_0_decrement'] = {
            name: '⬇️ Decrease Manual White Balance: Color temp (2500-10000 K)',
            description: 'Decrease the value of Manual White Balance: Color temp (2500-10000 K)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 7500.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('manual_white_balance_0', 5600.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(2500.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value decrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('manual_white_balance_1', 10.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('manual_white_balance_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'manual_white_balance', valuesString);
            }
        };
        // Action to reset Manual White Balance: Color temp (2500-10000 K) al default value
        actions['set_manual_white_balance_0_reset'] = {
            name: '🔄 Reset Manual White Balance: Color temp (2500-10000 K)',
            description: 'Reset to default value (5600)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 5600.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being reset, use its default value
                values[0] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('manual_white_balance_1', 10.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('manual_white_balance_0', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'manual_white_balance', valuesString);
            }
        };
        // Action to set only Manual White Balance: Tint (-50 to 50)
        actions['set_manual_white_balance_1'] = {
            name: 'Set Manual White Balance: Tint (-50 to 50)',
            description: 'Set value for Manual White Balance: Tint (-50 to 50)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 10.0,
                    min: -50.0,
                    max: 50.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('manual_white_balance_0', 5600.0, cameraId);
                // For the subindex being modified, use the new value
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('manual_white_balance_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'manual_white_balance', valuesString);
            }
        };
        // Action to increment Manual White Balance: Tint (-50 to 50)
        actions['set_manual_white_balance_1_increment'] = {
            name: '⬆️ Increase Manual White Balance: Tint (-50 to 50)',
            description: 'Increase the value of Manual White Balance: Tint (-50 to 50)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 100.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('manual_white_balance_1', 10.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(50.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('manual_white_balance_0', 5600.0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('manual_white_balance_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'manual_white_balance', valuesString);
            }
        };
        // Action to decrement Manual White Balance: Tint (-50 to 50)
        actions['set_manual_white_balance_1_decrement'] = {
            name: '⬇️ Decrease Manual White Balance: Tint (-50 to 50)',
            description: 'Decrease the value of Manual White Balance: Tint (-50 to 50)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 100.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('manual_white_balance_1', 10.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(-50.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('manual_white_balance_0', 5600.0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('manual_white_balance_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'manual_white_balance', valuesString);
            }
        };
        // Action to reset Manual White Balance: Tint (-50 to 50) al default value
        actions['set_manual_white_balance_1_reset'] = {
            name: '🔄 Reset Manual White Balance: Tint (-50 to 50)',
            description: 'Reset to default value (10)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 10.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('manual_white_balance_0', 5600.0, cameraId);
                // For the subindex being reset, use its default value
                values[1] = resetValue;
                
                // Store default value specifically for this camera
                this.storeParamValue('manual_white_balance_1', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'manual_white_balance', valuesString);
            }
        };
        // Action for Set auto WB (void)
        actions['set_set_auto_wb'] = {
            name: 'Trigger Set auto WB',
            description: 'Group: Video | Param: Set auto WB | Note: Calculate and set auto white balance',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'set_auto_wb', '1');
            }
        };
        // Action for Restore auto WB (void)
        actions['set_restore_auto_wb'] = {
            name: 'Trigger Restore auto WB',
            description: 'Group: Video | Param: Restore auto WB | Note: Use latest auto white balance setting',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'restore_auto_wb', '1');
            }
        };
        // Action for Dynamic Range Mode (numeric)
        actions['set_dynamic_range_mode'] = {
            name: 'Set Dynamic Range Mode',
            description: 'Group: Video | Param: Dynamic Range Mode | Note: 0 = Film, 1 = Video, 2 = Extended Video',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 2.0,
                    min: 0.0,
                    max: 2.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'dynamic_range_mode', event.options.value);
            }
        };
        // Action to increment Dynamic Range Mode
        actions['set_dynamic_range_mode_increment'] = {
            name: '⬆️ Increase Dynamic Range Mode',
            description: 'Increase the value of Dynamic Range Mode',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 2.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('dynamic_range_mode', 2.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(2.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'dynamic_range_mode', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('dynamic_range_mode', newValue, cameraId);
            }
        };
        // Action to decrement Dynamic Range Mode
        actions['set_dynamic_range_mode_decrement'] = {
            name: '⬇️ Decrease Dynamic Range Mode',
            description: 'Decrease the value of Dynamic Range Mode',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 2.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('dynamic_range_mode', 2.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'dynamic_range_mode', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('dynamic_range_mode', newValue, cameraId);
            }
        };
        // Action to reset Dynamic Range Mode al default value
        actions['set_dynamic_range_mode_reset'] = {
            name: '🔄 Reset Dynamic Range Mode',
            description: 'Reset to default value (2)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'dynamic_range_mode', 2.0);
                
                // Store default value for this specific camera
                this.storeParamValue('dynamic_range_mode', 2.0, cameraId);
            }
        };
        // Action for Video sharpening level (numeric)
        actions['set_video_sharpening_level'] = {
            name: 'Set Video sharpening level',
            description: 'Group: Video | Param: Video sharpening level | Note: 0 = Off, 1 = Low, 2 = Medium, 3 = High',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 3.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'video_sharpening_level', event.options.value);
            }
        };
        // Action to increment Video sharpening level
        actions['set_video_sharpening_level_increment'] = {
            name: '⬆️ Increase Video sharpening level',
            description: 'Increase the value of Video sharpening level',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 3.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('video_sharpening_level', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(3.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'video_sharpening_level', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('video_sharpening_level', newValue, cameraId);
            }
        };
        // Action to decrement Video sharpening level
        actions['set_video_sharpening_level_decrement'] = {
            name: '⬇️ Decrease Video sharpening level',
            description: 'Decrease the value of Video sharpening level',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 3.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('video_sharpening_level', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'video_sharpening_level', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('video_sharpening_level', newValue, cameraId);
            }
        };
        // Action to reset Video sharpening level al default value
        actions['set_video_sharpening_level_reset'] = {
            name: '🔄 Reset Video sharpening level',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'video_sharpening_level', 0.0);
                
                // Store default value for this specific camera
                this.storeParamValue('video_sharpening_level', 0.0, cameraId);
            }
        };
        // Action for Set auto exposure mode (numeric)
        actions['set_set_auto_exposure_mode'] = {
            name: 'Set Set auto exposure mode',
            description: 'Group: Video | Param: Set auto exposure mode | Note: 0 = Manual Trigger, 1 = Iris, 2 = Shutter, 3 = Iris + Shutter, 4 = Shutter + Iris',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 4.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'set_auto_exposure_mode', event.options.value);
            }
        };
        // Action to increment Set auto exposure mode
        actions['set_set_auto_exposure_mode_increment'] = {
            name: '⬆️ Increase Set auto exposure mode',
            description: 'Increase the value of Set auto exposure mode',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 4.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('set_auto_exposure_mode', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(4.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'set_auto_exposure_mode', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('set_auto_exposure_mode', newValue, cameraId);
            }
        };
        // Action to decrement Set auto exposure mode
        actions['set_set_auto_exposure_mode_decrement'] = {
            name: '⬇️ Decrease Set auto exposure mode',
            description: 'Decrease the value of Set auto exposure mode',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 4.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('set_auto_exposure_mode', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'set_auto_exposure_mode', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('set_auto_exposure_mode', newValue, cameraId);
            }
        };
        // Action to reset Set auto exposure mode al default value
        actions['set_set_auto_exposure_mode_reset'] = {
            name: '🔄 Reset Set auto exposure mode',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'set_auto_exposure_mode', 0.0);
                
                // Store default value for this specific camera
                this.storeParamValue('set_auto_exposure_mode', 0.0, cameraId);
            }
        };
        // Action for Video multiple subindexes)
        actions['set_video_mode'] = {
            name: 'Set Video mode',
            description: 'Set values for Video mode',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Frame rate (24, 25, 30, 50, 60)',
                    id: 'value0',
                    default: 60.0,
                    min: 24.0,
                    max: 60.0,
                    step: 1
                },
                {
                    type: 'number',
                    label: 'M-rate (0 = regular, 1 = M-rate)',
                    id: 'value1',
                    default: 0.0,
                    min: 0.0,
                    max: 1.0,
                    step: 1
                },
                {
                    type: 'number',
                    label: 'Dimensions (0 = NTSC, 1 = PAL, 2 = 720, 3 = 1080, 4 = 2kDCI, 5 = 2k16:9, 6 = UHD, 7 = 3k Anamorphic, 8 = 4k DCI, 9 = 4k 16:9, 10 = 4.6k 2.4:1, 11 = 4.6k)',
                    id: 'value2',
                    default: 6.0,
                    min: 0.0,
                    max: 11.0,
                    step: 1
                },
                {
                    type: 'number',
                    label: 'Interlaced (0 = progressive, 1 = interlaced)',
                    id: 'value3',
                    default: 0.0,
                    min: 0.0,
                    max: 1.0,
                    step: 1
                },
                {
                    type: 'number',
                    label: 'Color space (0 = YUV)',
                    id: 'value4',
                    default: 0.0,
                    min: 0.0,
                    max: 1.0,
                    step: 1
                },
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const values = [];
                values[0] = event.options.value0;
                values[1] = event.options.value1;
                values[2] = event.options.value2;
                values[3] = event.options.value3;
                values[4] = event.options.value4;
                const valuesString = values.join(",");
                this.sendParam(cameraId, 'video_mode', valuesString);
                
                // Store individual values for each subindex
                
                this.storeParamValue('video_mode_0', event.options.value0, cameraId);
                this.storeParamValue('video_mode_1', event.options.value1, cameraId);
                this.storeParamValue('video_mode_2', event.options.value2, cameraId);
                this.storeParamValue('video_mode_3', event.options.value3, cameraId);
                this.storeParamValue('video_mode_4', event.options.value4, cameraId);
            }
        };
        // Action to set only Video mode: Frame rate (24, 25, 30, 50, 60)
        actions['set_video_mode_0'] = {
            name: 'Set Video mode: Frame rate (24, 25, 30, 50, 60)',
            description: 'Set value for Video mode: Frame rate (24, 25, 30, 50, 60)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 60.0,
                    min: 24.0,
                    max: 60.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For the subindex being modified, use the new value
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('video_mode_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to increment Video mode: Frame rate (24, 25, 30, 50, 60)
        actions['set_video_mode_0_increment'] = {
            name: '⬆️ Increase Video mode: Frame rate (24, 25, 30, 50, 60)',
            description: 'Increase the value of Video mode: Frame rate (24, 25, 30, 50, 60)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 36.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('video_mode_0', 60.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(60.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value incrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('video_mode_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to decrement Video mode: Frame rate (24, 25, 30, 50, 60)
        actions['set_video_mode_0_decrement'] = {
            name: '⬇️ Decrease Video mode: Frame rate (24, 25, 30, 50, 60)',
            description: 'Decrease the value of Video mode: Frame rate (24, 25, 30, 50, 60)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 36.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('video_mode_0', 60.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(24.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value decrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('video_mode_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to reset Video mode: Frame rate (24, 25, 30, 50, 60) al default value
        actions['set_video_mode_0_reset'] = {
            name: '🔄 Reset Video mode: Frame rate (24, 25, 30, 50, 60)',
            description: 'Reset to default value (60)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 60.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being reset, use its default value
                values[0] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('video_mode_0', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to set only Video mode: M-rate (0 = regular, 1 = M-rate)
        actions['set_video_mode_1'] = {
            name: 'Set Video mode: M-rate (0 = regular, 1 = M-rate)',
            description: 'Set value for Video mode: M-rate (0 = regular, 1 = M-rate)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For the subindex being modified, use the new value
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('video_mode_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to increment Video mode: M-rate (0 = regular, 1 = M-rate)
        actions['set_video_mode_1_increment'] = {
            name: '⬆️ Increase Video mode: M-rate (0 = regular, 1 = M-rate)',
            description: 'Increase the value of Video mode: M-rate (0 = regular, 1 = M-rate)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('video_mode_1', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('video_mode_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to decrement Video mode: M-rate (0 = regular, 1 = M-rate)
        actions['set_video_mode_1_decrement'] = {
            name: '⬇️ Decrease Video mode: M-rate (0 = regular, 1 = M-rate)',
            description: 'Decrease the value of Video mode: M-rate (0 = regular, 1 = M-rate)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('video_mode_1', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('video_mode_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to reset Video mode: M-rate (0 = regular, 1 = M-rate) al default value
        actions['set_video_mode_1_reset'] = {
            name: '🔄 Reset Video mode: M-rate (0 = regular, 1 = M-rate)',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 0.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For the subindex being reset, use its default value
                values[1] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('video_mode_1', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to set only Video mode: Dimensions (0 = NTSC, 1 = PAL, 2 = 720, 3 = 1080, 4 = 2kDCI, 5 = 2k16:9, 6 = UHD, 7 = 3k Anamorphic, 8 = 4k DCI, 9 = 4k 16:9, 10 = 4.6k 2.4:1, 11 = 4.6k)
        actions['set_video_mode_2'] = {
            name: 'Set Video mode: Dimensions (0 = NTSC, 1 = PAL, 2 = 720, 3 = 1080, 4 = 2kDCI, 5 = 2k16:9, 6 = UHD, 7 = 3k Anamorphic, 8 = 4k DCI, 9 = 4k 16:9, 10 = 4.6k 2.4:1, 11 = 4.6k)',
            description: 'Set value for Video mode: Dimensions (0 = NTSC, 1 = PAL, 2 = 720, 3 = 1080, 4 = 2kDCI, 5 = 2k16:9, 6 = UHD, 7 = 3k Anamorphic, 8 = 4k DCI, 9 = 4k 16:9, 10 = 4.6k 2.4:1, 11 = 4.6k)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 6.0,
                    min: 0.0,
                    max: 11.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For the subindex being modified, use the new value
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('video_mode_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to increment Video mode: Dimensions (0 = NTSC, 1 = PAL, 2 = 720, 3 = 1080, 4 = 2kDCI, 5 = 2k16:9, 6 = UHD, 7 = 3k Anamorphic, 8 = 4k DCI, 9 = 4k 16:9, 10 = 4.6k 2.4:1, 11 = 4.6k)
        actions['set_video_mode_2_increment'] = {
            name: '⬆️ Increase Video mode: Dimensions (0 = NTSC, 1 = PAL, 2 = 720, 3 = 1080, 4 = 2kDCI, 5 = 2k16:9, 6 = UHD, 7 = 3k Anamorphic, 8 = 4k DCI, 9 = 4k 16:9, 10 = 4.6k 2.4:1, 11 = 4.6k)',
            description: 'Increase the value of Video mode: Dimensions (0 = NTSC, 1 = PAL, 2 = 720, 3 = 1080, 4 = 2kDCI, 5 = 2k16:9, 6 = UHD, 7 = 3k Anamorphic, 8 = 4k DCI, 9 = 4k 16:9, 10 = 4.6k 2.4:1, 11 = 4.6k)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 11.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('video_mode_2', 6.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(11.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('video_mode_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to decrement Video mode: Dimensions (0 = NTSC, 1 = PAL, 2 = 720, 3 = 1080, 4 = 2kDCI, 5 = 2k16:9, 6 = UHD, 7 = 3k Anamorphic, 8 = 4k DCI, 9 = 4k 16:9, 10 = 4.6k 2.4:1, 11 = 4.6k)
        actions['set_video_mode_2_decrement'] = {
            name: '⬇️ Decrease Video mode: Dimensions (0 = NTSC, 1 = PAL, 2 = 720, 3 = 1080, 4 = 2kDCI, 5 = 2k16:9, 6 = UHD, 7 = 3k Anamorphic, 8 = 4k DCI, 9 = 4k 16:9, 10 = 4.6k 2.4:1, 11 = 4.6k)',
            description: 'Decrease the value of Video mode: Dimensions (0 = NTSC, 1 = PAL, 2 = 720, 3 = 1080, 4 = 2kDCI, 5 = 2k16:9, 6 = UHD, 7 = 3k Anamorphic, 8 = 4k DCI, 9 = 4k 16:9, 10 = 4.6k 2.4:1, 11 = 4.6k)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 11.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('video_mode_2', 6.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('video_mode_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to reset Video mode: Dimensions (0 = NTSC, 1 = PAL, 2 = 720, 3 = 1080, 4 = 2kDCI, 5 = 2k16:9, 6 = UHD, 7 = 3k Anamorphic, 8 = 4k DCI, 9 = 4k 16:9, 10 = 4.6k 2.4:1, 11 = 4.6k) al default value
        actions['set_video_mode_2_reset'] = {
            name: '🔄 Reset Video mode: Dimensions (0 = NTSC, 1 = PAL, 2 = 720, 3 = 1080, 4 = 2kDCI, 5 = 2k16:9, 6 = UHD, 7 = 3k Anamorphic, 8 = 4k DCI, 9 = 4k 16:9, 10 = 4.6k 2.4:1, 11 = 4.6k)',
            description: 'Reset to default value (6)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 6.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For the subindex being reset, use its default value
                values[2] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('video_mode_2', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to set only Video mode: Interlaced (0 = progressive, 1 = interlaced)
        actions['set_video_mode_3'] = {
            name: 'Set Video mode: Interlaced (0 = progressive, 1 = interlaced)',
            description: 'Set value for Video mode: Interlaced (0 = progressive, 1 = interlaced)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For the subindex being modified, use the new value
                values[3] = newValue;
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('video_mode_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to increment Video mode: Interlaced (0 = progressive, 1 = interlaced)
        actions['set_video_mode_3_increment'] = {
            name: '⬆️ Increase Video mode: Interlaced (0 = progressive, 1 = interlaced)',
            description: 'Increase the value of Video mode: Interlaced (0 = progressive, 1 = interlaced)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('video_mode_3', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[3] = newValue;
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('video_mode_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to decrement Video mode: Interlaced (0 = progressive, 1 = interlaced)
        actions['set_video_mode_3_decrement'] = {
            name: '⬇️ Decrease Video mode: Interlaced (0 = progressive, 1 = interlaced)',
            description: 'Decrease the value of Video mode: Interlaced (0 = progressive, 1 = interlaced)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('video_mode_3', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[3] = newValue;
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('video_mode_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to reset Video mode: Interlaced (0 = progressive, 1 = interlaced) al default value
        actions['set_video_mode_3_reset'] = {
            name: '🔄 Reset Video mode: Interlaced (0 = progressive, 1 = interlaced)',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 0.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For the subindex being reset, use its default value
                values[3] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[4] = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('video_mode_3', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to set onlyr space (0 = YUV)
        actions['set_video_mode_4'] = {
            name: 'Set Video mode: Color space (0 = YUV)',
            description: 'Set value for Video mode: Color space (0 = YUV)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For the subindex being modified, use the new value
                values[4] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('video_mode_4', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to increment Video mode: Color space (0 = YUV)
        actions['set_video_mode_4_increment'] = {
            name: '⬆️ Increase Video mode: Color space (0 = YUV)',
            description: 'Increase the value of Video mode: Color space (0 = YUV)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[4] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('video_mode_4', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to decrement Video mode: Color space (0 = YUV)
        actions['set_video_mode_4_decrement'] = {
            name: '⬇️ Decrease Video mode: Color space (0 = YUV)',
            description: 'Decrease the value of Video mode: Color space (0 = YUV)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('video_mode_4', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[4] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('video_mode_4', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action to reset Video mode: Color space (0 = YUV) al default value
        actions['set_video_mode_4_reset'] = {
            name: '🔄 Reset Video mode: Color space (0 = YUV)',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 0.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('video_mode_0', 60.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('video_mode_1', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('video_mode_2', 6.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('video_mode_3', 0.0, cameraId);
                // For the subindex being reset, use its default value
                values[4] = resetValue;
                
                // Store default value specifically for this camera
                this.storeParamValue('video_mode_4', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'video_mode', valuesString);
            }
        };
        // Action for Display LUT (multiple subindexes)
        actions['set_display_lut'] = {
            name: 'Set Display LUT',
            description: 'Set values for Display LUT',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Selected LUT (0 = None, 1 = Custom, 2 = Film to Video, 3 = Film to Extended Video)',
                    id: 'value0',
                    default: 0.0,
                    min: 0.0,
                    max: 3.0,
                    step: 1
                },
                {
                    type: 'number',
                    label: 'LUT Enabled (0 = Not enabled, 1 = Enabled)',
                    id: 'value1',
                    default: 0.0,
                    min: 0.0,
                    max: 1.0,
                    step: 1
                },
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const values = [];
                values[0] = event.options.value0;
                values[1] = event.options.value1;
                const valuesString = values.join(",");
                this.sendParam(cameraId, 'display_lut', valuesString);
                
                // Store individual values for each subindex
                
                this.storeParamValue('display_lut_0', event.options.value0, cameraId);
                this.storeParamValue('display_lut_1', event.options.value1, cameraId);
            }
        };
        // Action to set only Display LUT: Selected LUT (0 = None, 1 = Custom, 2 = Film to Video, 3 = Film to Extended Video)
        actions['set_display_lut_0'] = {
            name: 'Set Display LUT: Selected LUT (0 = None, 1 = Custom, 2 = Film to Video, 3 = Film to Extended Video)',
            description: 'Set value for Display LUT: Selected LUT (0 = None, 1 = Custom, 2 = Film to Video, 3 = Film to Extended Video)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 3.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For the subindex being modified, use the new value
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('display_lut_1', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('display_lut_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'display_lut', valuesString);
            }
        };
        // Action to increment Display LUT: Selected LUT (0 = None, 1 = Custom, 2 = Film to Video, 3 = Film to Extended Video)
        actions['set_display_lut_0_increment'] = {
            name: '⬆️ Increase Display LUT: Selected LUT (0 = None, 1 = Custom, 2 = Film to Video, 3 = Film to Extended Video)',
            description: 'Increase the value of Display LUT: Selected LUT (0 = None, 1 = Custom, 2 = Film to Video, 3 = Film to Extended Video)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 3.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('display_lut_0', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(3.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value incrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('display_lut_1', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('display_lut_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'display_lut', valuesString);
            }
        };
        // Action to decrement Display LUT: Selected LUT (0 = None, 1 = Custom, 2 = Film to Video, 3 = Film to Extended Video)
        actions['set_display_lut_0_decrement'] = {
            name: '⬇️ Decrease Display LUT: Selected LUT (0 = None, 1 = Custom, 2 = Film to Video, 3 = Film to Extended Video)',
            description: 'Decrease the value of Display LUT: Selected LUT (0 = None, 1 = Custom, 2 = Film to Video, 3 = Film to Extended Video)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 3.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('display_lut_0', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value decrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('display_lut_1', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('display_lut_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'display_lut', valuesString);
            }
        };
        // Action to reset Display LUT: Selected LUT (0 = None, 1 = Custom, 2 = Film to Video, 3 = Film to Extended Video) al default value
        actions['set_display_lut_0_reset'] = {
            name: '🔄 Reset Display LUT: Selected LUT (0 = None, 1 = Custom, 2 = Film to Video, 3 = Film to Extended Video)',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 0.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being reset, use its default value
                values[0] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('display_lut_1', 0.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('display_lut_0', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'display_lut', valuesString);
            }
        };
        // Action to set only Display LUT: LUT Enabled (0 = Not enabled, 1 = Enabled)
        actions['set_display_lut_1'] = {
            name: 'Set Display LUT: LUT Enabled (0 = Not enabled, 1 = Enabled)',
            description: 'Set value for Display LUT: LUT Enabled (0 = Not enabled, 1 = Enabled)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('display_lut_0', 0.0, cameraId);
                // For the subindex being modified, use the new value
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('display_lut_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'display_lut', valuesString);
            }
        };
        // Action to increment Display LUT: LUT Enabled (0 = Not enabled, 1 = Enabled)
        actions['set_display_lut_1_increment'] = {
            name: '⬆️ Increase Display LUT: LUT Enabled (0 = Not enabled, 1 = Enabled)',
            description: 'Increase the value of Display LUT: LUT Enabled (0 = Not enabled, 1 = Enabled)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('display_lut_1', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('display_lut_0', 0.0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('display_lut_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'display_lut', valuesString);
            }
        };
        // Action to decrement Display LUT: LUT Enabled (0 = Not enabled, 1 = Enabled)
        actions['set_display_lut_1_decrement'] = {
            name: '⬇️ Decrease Display LUT: LUT Enabled (0 = Not enabled, 1 = Enabled)',
            description: 'Decrease the value of Display LUT: LUT Enabled (0 = Not enabled, 1 = Enabled)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('display_lut_1', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('display_lut_0', 0.0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('display_lut_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'display_lut', valuesString);
            }
        };
        // Action to reset Display LUT: LUT Enabled (0 = Not enabled, 1 = Enabled) al default value
        actions['set_display_lut_1_reset'] = {
            name: '🔄 Reset Display LUT: LUT Enabled (0 = Not enabled, 1 = Enabled)',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 0.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('display_lut_0', 0.0, cameraId);
                // For the subindex being reset, use its default value
                values[1] = resetValue;
                
                // Store default value specifically for this camera
                this.storeParamValue('display_lut_1', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'display_lut', valuesString);
            }
        };
        // Action for Mic level (numeric)
        actions['set_mic_level'] = {
            name: 'Set Mic level',
            description: 'Group: Audio | Param: Mic level | Note: 0.0 = Minimum, 1.0 = Maximum',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.7,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'mic_level', event.options.value);
            }
        };
        // Action to increment Mic level
        actions['set_mic_level_increment'] = {
            name: '⬆️ Increase Mic level',
            description: 'Increase the value of Mic level',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('mic_level', 0.7, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'mic_level', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('mic_level', newValue, cameraId);
            }
        };
        // Action to decrement Mic level
        actions['set_mic_level_decrement'] = {
            name: '⬇️ Decrease Mic level',
            description: 'Decrease the value of Mic level',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('mic_level', 0.7, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'mic_level', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('mic_level', newValue, cameraId);
            }
        };
        // Action to reset Mic level al default value
        actions['set_mic_level_reset'] = {
            name: '🔄 Reset Mic level',
            description: 'Reset to default value (0.70)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'mic_level', 0.7);
                
                // Store default value for this specific camera
                this.storeParamValue('mic_level', 0.7, cameraId);
            }
        };
        // Action for Headphone level (numeric)
        actions['set_headphone_level'] = {
            name: 'Set Headphone level',
            description: 'Group: Audio | Param: Headphone level | Note: 0.0 = Minimum, 1.0 = Maximum',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 1.0,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'headphone_level', event.options.value);
            }
        };
        // Action to increment Headphone level
        actions['set_headphone_level_increment'] = {
            name: '⬆️ Increase Headphone level',
            description: 'Increase the value of Headphone level',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('headphone_level', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'headphone_level', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('headphone_level', newValue, cameraId);
            }
        };
        // Action to decrement Headphone level
        actions['set_headphone_level_decrement'] = {
            name: '⬇️ Decrease Headphone level',
            description: 'Decrease the value of Headphone level',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('headphone_level', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'headphone_level', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('headphone_level', newValue, cameraId);
            }
        };
        // Action to reset Headphone level al default value
        actions['set_headphone_level_reset'] = {
            name: '🔄 Reset Headphone level',
            description: 'Reset to default value (1.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'headphone_level', 1.0);
                
                // Store default value for this specific camera
                this.storeParamValue('headphone_level', 1.0, cameraId);
            }
        };
        // Action for Headphone program mix (numeric)
        actions['set_headphone_program_mix'] = {
            name: 'Set Headphone program mix',
            description: 'Group: Audio | Param: Headphone program mix | Note: 0.0 = Minimum, 1.0 = Maximum',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'headphone_program_mix', event.options.value);
            }
        };
        // Action to increment Headphone program mix
        actions['set_headphone_program_mix_increment'] = {
            name: '⬆️ Increase Headphone program mix',
            description: 'Increase the value of Headphone program mix',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('headphone_program_mix', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'headphone_program_mix', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('headphone_program_mix', newValue, cameraId);
            }
        };
        // Action to decrement Headphone program mix
        actions['set_headphone_program_mix_decrement'] = {
            name: '⬇️ Decrease Headphone program mix',
            description: 'Decrease the value of Headphone program mix',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('headphone_program_mix', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'headphone_program_mix', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('headphone_program_mix', newValue, cameraId);
            }
        };
        // Action to reset Headphone program mix al default value
        actions['set_headphone_program_mix_reset'] = {
            name: '🔄 Reset Headphone program mix',
            description: 'Reset to default value (0.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'headphone_program_mix', 0.0);
                
                // Store default value for this specific camera
                this.storeParamValue('headphone_program_mix', 0.0, cameraId);
            }
        };
        // Action for Speaker level (numeric)
        actions['set_speaker_level'] = {
            name: 'Set Speaker level',
            description: 'Group: Audio | Param: Speaker level | Note: 0.0 = Minimum, 1.0 = Maximum',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 1.0,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'speaker_level', event.options.value);
            }
        };
        // Action to increment Speaker level
        actions['set_speaker_level_increment'] = {
            name: '⬆️ Increase Speaker level',
            description: 'Increase the value of Speaker level',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('speaker_level', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'speaker_level', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('speaker_level', newValue, cameraId);
            }
        };
        // Action to decrement Speaker level
        actions['set_speaker_level_decrement'] = {
            name: '⬇️ Decrease Speaker level',
            description: 'Decrease the value of Speaker level',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('speaker_level', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'speaker_level', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('speaker_level', newValue, cameraId);
            }
        };
        // Action to reset Speaker level al default value
        actions['set_speaker_level_reset'] = {
            name: '🔄 Reset Speaker level',
            description: 'Reset to default value (1.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'speaker_level', 1.0);
                
                // Store default value for this specific camera
                this.storeParamValue('speaker_level', 1.0, cameraId);
            }
        };
        // Action for Input type (numeric)
        actions['set_input_type'] = {
            name: 'Set Input type',
            description: 'Group: Audio | Param: Input type | Note: 0 = Internal Mic, 1 = Line Level Input, 2 = Low Mic Level Input, 3 = High Mic Level Input',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 3.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'input_type', event.options.value);
            }
        };
        // Action to increment Input type
        actions['set_input_type_increment'] = {
            name: '⬆️ Increase Input type',
            description: 'Increase the value of Input type',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 3.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('input_type', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(3.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'input_type', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('input_type', newValue, cameraId);
            }
        };
        // Action to decrement Input type
        actions['set_input_type_decrement'] = {
            name: '⬇️ Decrease Input type',
            description: 'Decrease the value of Input type',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 3.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('input_type', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'input_type', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('input_type', newValue, cameraId);
            }
        };
        // Action to reset Input type al default value
        actions['set_input_type_reset'] = {
            name: '🔄 Reset Input type',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'input_type', 0.0);
                
                // Store default value for this specific camera
                this.storeParamValue('input_type', 0.0, cameraId);
            }
        };
        // Action for Input levels (multiple subindexes)
        actions['set_input_levels'] = {
            name: 'Set Input levels',
            description: 'Set values for Input levels',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Ch1 = 0.0 Minimum, 1.0 Maximum',
                    id: 'value0',
                    default: 0.5,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: 'Ch2 = 0.0 Minimum, 1.0 Maximum',
                    id: 'value1',
                    default: 0.5,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                },
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const values = [];
                values[0] = event.options.value0;
                values[1] = event.options.value1;
                const valuesString = values.join(",");
                this.sendParam(cameraId, 'input_levels', valuesString);
                
                // Store individual values for each subindex
                
                this.storeParamValue('input_levels_0', event.options.value0, cameraId);
                this.storeParamValue('input_levels_1', event.options.value1, cameraId);
            }
        };
        // Action to set only Input levels: Ch1 = 0.0 Minimum, 1.0 Maximum
        actions['set_input_levels_0'] = {
            name: 'Set Input levels: Ch1 = 0.0 Minimum, 1.0 Maximum',
            description: 'Set value for Input levels: Ch1 = 0.0 Minimum, 1.0 Maximum',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.5,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For the subindex being modified, use the new value
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('input_levels_1', 0.5, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('input_levels_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'input_levels', valuesString);
            }
        };
        // Action to increment Input levels: Ch1 = 0.0 Minimum, 1.0 Maximum
        actions['set_input_levels_0_increment'] = {
            name: '⬆️ Increase Input levels: Ch1 = 0.0 Minimum, 1.0 Maximum',
            description: 'Increase the value of Input levels: Ch1 = 0.0 Minimum, 1.0 Maximum',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('input_levels_0', 0.5, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value incrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('input_levels_1', 0.5, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('input_levels_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'input_levels', valuesString);
            }
        };
        // Action to decrement Input levels: Ch1 = 0.0 Minimum, 1.0 Maximum
        actions['set_input_levels_0_decrement'] = {
            name: '⬇️ Decrease Input levels: Ch1 = 0.0 Minimum, 1.0 Maximum',
            description: 'Decrease the value of Input levels: Ch1 = 0.0 Minimum, 1.0 Maximum',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('input_levels_0', 0.5, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value decrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('input_levels_1', 0.5, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('input_levels_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'input_levels', valuesString);
            }
        };
        // Action to reset Input levels: Ch1 = 0.0 Minimum, 1.0 Maximum al default value
        actions['set_input_levels_0_reset'] = {
            name: '🔄 Reset Input levels: Ch1 = 0.0 Minimum, 1.0 Maximum',
            description: 'Reset to default value (0.50)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 0.5;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being reset, use its default value
                values[0] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('input_levels_1', 0.5, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('input_levels_0', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'input_levels', valuesString);
            }
        };
        // Action to set only Input levels: Ch2 = 0.0 Minimum, 1.0 Maximum
        actions['set_input_levels_1'] = {
            name: 'Set Input levels: Ch2 = 0.0 Minimum, 1.0 Maximum',
            description: 'Set value for Input levels: Ch2 = 0.0 Minimum, 1.0 Maximum',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.5,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('input_levels_0', 0.5, cameraId);
                // For the subindex being modified, use the new value
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('input_levels_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'input_levels', valuesString);
            }
        };
        // Action to increment Input levels: Ch2 = 0.0 Minimum, 1.0 Maximum
        actions['set_input_levels_1_increment'] = {
            name: '⬆️ Increase Input levels: Ch2 = 0.0 Minimum, 1.0 Maximum',
            description: 'Increase the value of Input levels: Ch2 = 0.0 Minimum, 1.0 Maximum',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('input_levels_1', 0.5, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('input_levels_0', 0.5, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('input_levels_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'input_levels', valuesString);
            }
        };
        // Action to decrement Input levels: Ch2 = 0.0 Minimum, 1.0 Maximum
        actions['set_input_levels_1_decrement'] = {
            name: '⬇️ Decrease Input levels: Ch2 = 0.0 Minimum, 1.0 Maximum',
            description: 'Decrease the value of Input levels: Ch2 = 0.0 Minimum, 1.0 Maximum',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('input_levels_1', 0.5, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('input_levels_0', 0.5, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('input_levels_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'input_levels', valuesString);
            }
        };
        // Action to reset Input levels: Ch2 = 0.0 Minimum, 1.0 Maximum al default value
        actions['set_input_levels_1_reset'] = {
            name: '🔄 Reset Input levels: Ch2 = 0.0 Minimum, 1.0 Maximum',
            description: 'Reset to default value (0.50)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 0.5;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('input_levels_0', 0.5, cameraId);
                // For the subindex being reset, use its default value
                values[1] = resetValue;
                
                // Store default value specifically for this camera
                this.storeParamValue('input_levels_1', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'input_levels', valuesString);
            }
        };
        // Action for Phantom power (boolean)
        actions['set_phantom_power'] = {
            name: 'Set Phantom power',
            description: 'Group: Audio | Param: Phantom power | Note: True = Powered, False = Not powered',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'checkbox',
                    label: 'Value',
                    id: 'value',
                    default: false
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const value = event.options.value ? 1 : 0;
                this.sendParam(cameraId, 'phantom_power', value);
            }
        };
        // Action for Overlays (multiple subindexes)
        actions['set_overlays'] = {
            name: 'Set Overlays',
            description: 'Set values for Overlays',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Frame guides style (0 = Off, 1 = 2.4:1, 2 = 2.39:1, 3 = 2.35:1, 4 = 1.85:1, 5 = 16:9, 6 = 14:9, 7 = 4:3, 8 = 2:1, 9 = 4:5, 10 = 1:1)',
                    id: 'value0',
                    default: 5.0,
                    min: 0.0,
                    max: 10.0,
                    step: 1
                },
                {
                    type: 'number',
                    label: 'Frame Guide Opacity (0 = Transparent, 100 = Opaque)',
                    id: 'value1',
                    default: 100.0,
                    min: 0.0,
                    max: 100.0,
                    step: 1
                },
                {
                    type: 'number',
                    label: 'Safe Area Percentage (0 means off)',
                    id: 'value2',
                    default: 0.0,
                    min: 0.0,
                    max: 100.0,
                    step: 1
                },
                {
                    type: 'number',
                    label: 'Grid style bit flags (1 = Thirds, 2 = Cross Hairs, 4 = Center Dot, 8 = Horizon)',
                    id: 'value3',
                    default: 9.0,
                    min: 0.0,
                    max: 15.0,
                    step: 1
                },
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const values = [];
                values[0] = event.options.value0;
                values[1] = event.options.value1;
                values[2] = event.options.value2;
                values[3] = event.options.value3;
                const valuesString = values.join(",");
                this.sendParam(cameraId, 'overlays', valuesString);
                
                // Store individual values for each subindex
                
                this.storeParamValue('overlays_0', event.options.value0, cameraId);
                this.storeParamValue('overlays_1', event.options.value1, cameraId);
                this.storeParamValue('overlays_2', event.options.value2, cameraId);
                this.storeParamValue('overlays_3', event.options.value3, cameraId);
            }
        };
        // Action to set only Overlays: Frame guides style (0 = Off, 1 = 2.4:1, 2 = 2.39:1, 3 = 2.35:1, 4 = 1.85:1, 5 = 16:9, 6 = 14:9, 7 = 4:3, 8 = 2:1, 9 = 4:5, 10 = 1:1)
        actions['set_overlays_0'] = {
            name: 'Set Overlays: Frame guides style (0 = Off, 1 = 2.4:1, 2 = 2.39:1, 3 = 2.35:1, 4 = 1.85:1, 5 = 16:9, 6 = 14:9, 7 = 4:3, 8 = 2:1, 9 = 4:5, 10 = 1:1)',
            description: 'Set value for Overlays: Frame guides style (0 = Off, 1 = 2.4:1, 2 = 2.39:1, 3 = 2.35:1, 4 = 1.85:1, 5 = 16:9, 6 = 14:9, 7 = 4:3, 8 = 2:1, 9 = 4:5, 10 = 1:1)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 5.0,
                    min: 0.0,
                    max: 10.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For the subindex being modified, use the new value
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('overlays_1', 100.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('overlays_2', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('overlays_3', 9.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('overlays_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action to increment Overlays: Frame guides style (0 = Off, 1 = 2.4:1, 2 = 2.39:1, 3 = 2.35:1, 4 = 1.85:1, 5 = 16:9, 6 = 14:9, 7 = 4:3, 8 = 2:1, 9 = 4:5, 10 = 1:1)
        actions['set_overlays_0_increment'] = {
            name: '⬆️ Increase Overlays: Frame guides style (0 = Off, 1 = 2.4:1, 2 = 2.39:1, 3 = 2.35:1, 4 = 1.85:1, 5 = 16:9, 6 = 14:9, 7 = 4:3, 8 = 2:1, 9 = 4:5, 10 = 1:1)',
            description: 'Increase the value of Overlays: Frame guides style (0 = Off, 1 = 2.4:1, 2 = 2.39:1, 3 = 2.35:1, 4 = 1.85:1, 5 = 16:9, 6 = 14:9, 7 = 4:3, 8 = 2:1, 9 = 4:5, 10 = 1:1)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 10.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('overlays_0', 5.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(10.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value incrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('overlays_1', 100.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('overlays_2', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('overlays_3', 9.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('overlays_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action to decrement Overlays: Frame guides style (0 = Off, 1 = 2.4:1, 2 = 2.39:1, 3 = 2.35:1, 4 = 1.85:1, 5 = 16:9, 6 = 14:9, 7 = 4:3, 8 = 2:1, 9 = 4:5, 10 = 1:1)
        actions['set_overlays_0_decrement'] = {
            name: '⬇️ Decrease Overlays: Frame guides style (0 = Off, 1 = 2.4:1, 2 = 2.39:1, 3 = 2.35:1, 4 = 1.85:1, 5 = 16:9, 6 = 14:9, 7 = 4:3, 8 = 2:1, 9 = 4:5, 10 = 1:1)',
            description: 'Decrease the value of Overlays: Frame guides style (0 = Off, 1 = 2.4:1, 2 = 2.39:1, 3 = 2.35:1, 4 = 1.85:1, 5 = 16:9, 6 = 14:9, 7 = 4:3, 8 = 2:1, 9 = 4:5, 10 = 1:1)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 10.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('overlays_0', 5.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value decrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('overlays_1', 100.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('overlays_2', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('overlays_3', 9.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('overlays_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action to reset Overlays: Frame guides style (0 = Off, 1 = 2.4:1, 2 = 2.39:1, 3 = 2.35:1, 4 = 1.85:1, 5 = 16:9, 6 = 14:9, 7 = 4:3, 8 = 2:1, 9 = 4:5, 10 = 1:1) al default value
        actions['set_overlays_0_reset'] = {
            name: '🔄 Reset Overlays: Frame guides style (0 = Off, 1 = 2.4:1, 2 = 2.39:1, 3 = 2.35:1, 4 = 1.85:1, 5 = 16:9, 6 = 14:9, 7 = 4:3, 8 = 2:1, 9 = 4:5, 10 = 1:1)',
            description: 'Reset to default value (5)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 5.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being reset, use its default value
                values[0] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('overlays_1', 100.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('overlays_2', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('overlays_3', 9.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('overlays_0', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action to set only Overlays: Frame Guide Opacity (0 = Transparent, 100 = Opaque)
        actions['set_overlays_1'] = {
            name: 'Set Overlays: Frame Guide Opacity (0 = Transparent, 100 = Opaque)',
            description: 'Set value for Overlays: Frame Guide Opacity (0 = Transparent, 100 = Opaque)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 100.0,
                    min: 0.0,
                    max: 100.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('overlays_0', 5.0, cameraId);
                // For the subindex being modified, use the new value
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('overlays_2', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('overlays_3', 9.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('overlays_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action to increment Overlays: Frame Guide Opacity (0 = Transparent, 100 = Opaque)
        actions['set_overlays_1_increment'] = {
            name: '⬆️ Increase Overlays: Frame Guide Opacity (0 = Transparent, 100 = Opaque)',
            description: 'Increase the value of Overlays: Frame Guide Opacity (0 = Transparent, 100 = Opaque)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 100.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('overlays_1', 100.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(100.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('overlays_0', 5.0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('overlays_2', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('overlays_3', 9.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('overlays_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action to decrement Overlays: Frame Guide Opacity (0 = Transparent, 100 = Opaque)
        actions['set_overlays_1_decrement'] = {
            name: '⬇️ Decrease Overlays: Frame Guide Opacity (0 = Transparent, 100 = Opaque)',
            description: 'Decrease the value of Overlays: Frame Guide Opacity (0 = Transparent, 100 = Opaque)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 100.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('overlays_1', 100.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('overlays_0', 5.0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('overlays_2', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('overlays_3', 9.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('overlays_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action to reset Overlays: Frame Guide Opacity (0 = Transparent, 100 = Opaque) al default value
        actions['set_overlays_1_reset'] = {
            name: '🔄 Reset Overlays: Frame Guide Opacity (0 = Transparent, 100 = Opaque)',
            description: 'Reset to default value (100)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 100.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('overlays_0', 5.0, cameraId);
                // For the subindex being reset, use its default value
                values[1] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('overlays_2', 0.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('overlays_3', 9.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('overlays_1', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action to set only Overlays: Safe Area Percentage (0 means off)
        actions['set_overlays_2'] = {
            name: 'Set Overlays: Safe Area Percentage (0 means off)',
            description: 'Set value for Overlays: Safe Area Percentage (0 means off)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 100.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('overlays_0', 5.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('overlays_1', 100.0, cameraId);
                // For the subindex being modified, use the new value
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('overlays_3', 9.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('overlays_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action to increment Overlays: Safe Area Percentage (0 means off)
        actions['set_overlays_2_increment'] = {
            name: '⬆️ Increase Overlays: Safe Area Percentage (0 means off)',
            description: 'Increase the value of Overlays: Safe Area Percentage (0 means off)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 100.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('overlays_2', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(100.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('overlays_0', 5.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('overlays_1', 100.0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('overlays_3', 9.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('overlays_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action to decrement Overlays: Safe Area Percentage (0 means off)
        actions['set_overlays_2_decrement'] = {
            name: '⬇️ Decrease Overlays: Safe Area Percentage (0 means off)',
            description: 'Decrease the value of Overlays: Safe Area Percentage (0 means off)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 100.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('overlays_2', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('overlays_0', 5.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('overlays_1', 100.0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('overlays_3', 9.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('overlays_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action to reset Overlays: Safe Area Percentage (0 means off) al default value
        actions['set_overlays_2_reset'] = {
            name: '🔄 Reset Overlays: Safe Area Percentage (0 means off)',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 0.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('overlays_0', 5.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('overlays_1', 100.0, cameraId);
                // For the subindex being reset, use its default value
                values[2] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('overlays_3', 9.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('overlays_2', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action to set only Overlays: Grid style bit flags (1 = Thirds, 2 = Cross Hairs, 4 = Center Dot, 8 = Horizon)
        actions['set_overlays_3'] = {
            name: 'Set Overlays: Grid style bit flags (1 = Thirds, 2 = Cross Hairs, 4 = Center Dot, 8 = Horizon)',
            description: 'Set value for Overlays: Grid style bit flags (1 = Thirds, 2 = Cross Hairs, 4 = Center Dot, 8 = Horizon)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 9.0,
                    min: 0.0,
                    max: 15.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('overlays_0', 5.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('overlays_1', 100.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('overlays_2', 0.0, cameraId);
                // For the subindex being modified, use the new value
                values[3] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('overlays_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action to increment Overlays: Grid style bit flags (1 = Thirds, 2 = Cross Hairs, 4 = Center Dot, 8 = Horizon)
        actions['set_overlays_3_increment'] = {
            name: '⬆️ Increase Overlays: Grid style bit flags (1 = Thirds, 2 = Cross Hairs, 4 = Center Dot, 8 = Horizon)',
            description: 'Increase the value of Overlays: Grid style bit flags (1 = Thirds, 2 = Cross Hairs, 4 = Center Dot, 8 = Horizon)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 15.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('overlays_3', 9.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(15.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('overlays_0', 5.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('overlays_1', 100.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('overlays_2', 0.0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[3] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('overlays_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action to decrement Overlays: Grid style bit flags (1 = Thirds, 2 = Cross Hairs, 4 = Center Dot, 8 = Horizon)
        actions['set_overlays_3_decrement'] = {
            name: '⬇️ Decrease Overlays: Grid style bit flags (1 = Thirds, 2 = Cross Hairs, 4 = Center Dot, 8 = Horizon)',
            description: 'Decrease the value of Overlays: Grid style bit flags (1 = Thirds, 2 = Cross Hairs, 4 = Center Dot, 8 = Horizon)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 15.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('overlays_3', 9.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('overlays_0', 5.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('overlays_1', 100.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('overlays_2', 0.0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[3] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('overlays_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action to reset Overlays: Grid style bit flags (1 = Thirds, 2 = Cross Hairs, 4 = Center Dot, 8 = Horizon) al default value
        actions['set_overlays_3_reset'] = {
            name: '🔄 Reset Overlays: Grid style bit flags (1 = Thirds, 2 = Cross Hairs, 4 = Center Dot, 8 = Horizon)',
            description: 'Reset to default value (9)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 9.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('overlays_0', 5.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('overlays_1', 100.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('overlays_2', 0.0, cameraId);
                // For the subindex being reset, use its default value
                values[3] = resetValue;
                
                // Store default value specifically for this camera
                this.storeParamValue('overlays_3', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'overlays', valuesString);
            }
        };
        // Action for Brightness (numeric)
        actions['set_brightness'] = {
            name: 'Set Brightness',
            description: 'Group: Display | Param: Brightness | Note: 0.0 = Minimum, 1.0 = Maximum',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 1.0,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'brightness', event.options.value);
            }
        };
        // Action to increment Brightness
        actions['set_brightness_increment'] = {
            name: '⬆️ Increase Brightness',
            description: 'Increase the value of Brightness',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('brightness', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'brightness', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('brightness', newValue, cameraId);
            }
        };
        // Action to decrement Brightness
        actions['set_brightness_decrement'] = {
            name: '⬇️ Decrease Brightness',
            description: 'Decrease the value of Brightness',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('brightness', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'brightness', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('brightness', newValue, cameraId);
            }
        };
        // Action to reset Brightness al default value
        actions['set_brightness_reset'] = {
            name: '🔄 Reset Brightness',
            description: 'Reset to default value (1.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'brightness', 1.0);
                
                // Store default value for this specific camera
                this.storeParamValue('brightness', 1.0, cameraId);
            }
        };
        // Action for Exposure and focus tools (multiple subindexes)
        actions['set_exposure_and_focus_tools'] = {
            name: 'Set Exposure and focus tools',
            description: 'Set values for Exposure and focus tools',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'textinput',
                    label: 'Bit flags: 1 = Zebra, 2 = Focus Assist, 4 = False Color',
                    id: 'value0',
                    default: ''
                },
                {
                    type: 'textinput',
                    label: 'Target displays bit flags: 1 = LCD, 2 = HDMI, 4 = EVF, 8 = Main SDI, 16 = Front SDI',
                    id: 'value1',
                    default: ''
                },
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const values = [];
                values[0] = event.options.value0;
                values[1] = event.options.value1;
                const valuesString = values.join(",");
                this.sendParam(cameraId, 'exposure_and_focus_tools', valuesString);
                
                // Store individual values for each subindex
                
                this.storeParamValue('exposure_and_focus_tools_0', event.options.value0, cameraId);
                this.storeParamValue('exposure_and_focus_tools_1', event.options.value1, cameraId);
            }
        };
        // Action for Zebra level (numeric)
        actions['set_zebra_level'] = {
            name: 'Set Zebra level',
            description: 'Group: Display | Param: Zebra level | Note: 0.0 = Minimum, 1.0 = Maximum',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.5,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'zebra_level', event.options.value);
            }
        };
        // Action to increment Zebra level
        actions['set_zebra_level_increment'] = {
            name: '⬆️ Increase Zebra level',
            description: 'Increase the value of Zebra level',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('zebra_level', 0.5, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'zebra_level', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('zebra_level', newValue, cameraId);
            }
        };
        // Action to decrement Zebra level
        actions['set_zebra_level_decrement'] = {
            name: '⬇️ Decrease Zebra level',
            description: 'Decrease the value of Zebra level',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('zebra_level', 0.5, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'zebra_level', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('zebra_level', newValue, cameraId);
            }
        };
        // Action to reset Zebra level al default value
        actions['set_zebra_level_reset'] = {
            name: '🔄 Reset Zebra level',
            description: 'Reset to default value (0.50)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'zebra_level', 0.5);
                
                // Store default value for this specific camera
                this.storeParamValue('zebra_level', 0.5, cameraId);
            }
        };
        // Action for Peaking level (numeric)
        actions['set_peaking_level'] = {
            name: 'Set Peaking level',
            description: 'Group: Display | Param: Peaking level | Note: 0.0 = Minimum, 1.0 = Maximum',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.9,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'peaking_level', event.options.value);
            }
        };
        // Action to increment Peaking level
        actions['set_peaking_level_increment'] = {
            name: '⬆️ Increase Peaking level',
            description: 'Increase the value of Peaking level',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('peaking_level', 0.9, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'peaking_level', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('peaking_level', newValue, cameraId);
            }
        };
        // Action to decrement Peaking level
        actions['set_peaking_level_decrement'] = {
            name: '⬇️ Decrease Peaking level',
            description: 'Decrease the value of Peaking level',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('peaking_level', 0.9, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'peaking_level', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('peaking_level', newValue, cameraId);
            }
        };
        // Action to reset Peaking level al default value
        actions['set_peaking_level_reset'] = {
            name: '🔄 Reset Peaking level',
            description: 'Reset to default value (0.90)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'peaking_level', 0.9);
                
                // Store default value for this specific camera
                this.storeParamValue('peaking_level', 0.9, cameraId);
            }
        };
        // Action for Color bars display time (seconds) (numeric)
        actions['set_color_bars_display_time_seconds'] = {
            name: 'Set Color bars display time (seconds)',
            description: 'Group: Display | Param: Color bars display time (seconds) | Note: 0 = Disable Bars, 1-30 = Enable Bars With Timeout (s)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 30.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'color_bars_display_time_seconds', event.options.value);
            }
        };
        // Action to increment Color bars display time (seconds)
        actions['set_color_bars_display_time_seconds_increment'] = {
            name: '⬆️ Increase Color bars display time (seconds)',
            description: 'Increase the value of Color bars display time (seconds)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 30.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('color_bars_display_time_seconds', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(30.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'color_bars_display_time_seconds', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('color_bars_display_time_seconds', newValue, cameraId);
            }
        };
        // Action to decrement Color bars display time (seconds)
        actions['set_color_bars_display_time_seconds_decrement'] = {
            name: '⬇️ Decrease Color bars display time (seconds)',
            description: 'Decrease the value of Color bars display time (seconds)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 30.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('color_bars_display_time_seconds', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'color_bars_display_time_seconds', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('color_bars_display_time_seconds', newValue, cameraId);
            }
        };
        // Action to reset Color bars display time (seconds) al default value
        actions['set_color_bars_display_time_seconds_reset'] = {
            name: '🔄 Reset Color bars display time (seconds)',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'color_bars_display_time_seconds', 0.0);
                
                // Store default value for this specific camera
                this.storeParamValue('color_bars_display_time_seconds', 0.0, cameraId);
            }
        };
        // Action for Focus Assist (multiple subindexes)
        actions['set_focus_assist'] = {
            name: 'Set Focus Assist',
            description: 'Set values for Focus Assist',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Focus Assist Method (0 = Peak, 1 = Colored lines)',
                    id: 'value0',
                    default: 1.0,
                    min: 0.0,
                    max: 1.0,
                    step: 1
                },
                {
                    type: 'number',
                    label: 'Focus Line Color (0 = Red, 1 = Green, 2 = Blue, 3 = White, 4 = Black)',
                    id: 'value1',
                    default: 0.0,
                    min: 0.0,
                    max: 4.0,
                    step: 1
                },
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const values = [];
                values[0] = event.options.value0;
                values[1] = event.options.value1;
                const valuesString = values.join(",");
                this.sendParam(cameraId, 'focus_assist', valuesString);
                
                // Store individual values for each subindex
                
                this.storeParamValue('focus_assist_0', event.options.value0, cameraId);
                this.storeParamValue('focus_assist_1', event.options.value1, cameraId);
            }
        };
        // Action to set onlyred lines)
        actions['set_focus_assist_0'] = {
            name: 'Set Focus Assist: Focus Assist Method (0 = Peak, 1 = Colored lines)',
            description: 'Set value for Focus Assist: Focus Assist Method (0 = Peak, 1 = Colored lines)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 1.0,
                    min: 0.0,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For the subindex being modified, use the new value
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('focus_assist_1', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('focus_assist_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'focus_assist', valuesString);
            }
        };
        // Action to increment Focus Assist: Focus Assist Method (0 = Peak, 1 = Colored lines)
        actions['set_focus_assist_0_increment'] = {
            name: '⬆️ Increase Focus Assist: Focus Assist Method (0 = Peak, 1 = Colored lines)',
            description: 'Increase the value of Focus Assist: Focus Assist Method (0 = Peak, 1 = Colored lines)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('focus_assist_0', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value incrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('focus_assist_1', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('focus_assist_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'focus_assist', valuesString);
            }
        };
        // Action to decrement Focus Assist: Focus Assist Method (0 = Peak, 1 = Colored lines)
        actions['set_focus_assist_0_decrement'] = {
            name: '⬇️ Decrease Focus Assist: Focus Assist Method (0 = Peak, 1 = Colored lines)',
            description: 'Decrease the value of Focus Assist: Focus Assist Method (0 = Peak, 1 = Colored lines)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('focus_assist_0', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value decrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('focus_assist_1', 0.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('focus_assist_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'focus_assist', valuesString);
            }
        };
        // Action to reset Focus Assist: Focus Assist Method (0 = Peak, 1 = Colored lines) al default value
        actions['set_focus_assist_0_reset'] = {
            name: '🔄 Reset Focus Assist: Focus Assist Method (0 = Peak, 1 = Colored lines)',
            description: 'Reset to default value (1)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 1.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being reset, use its default value
                values[0] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('focus_assist_1', 0.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('focus_assist_0', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'focus_assist', valuesString);
            }
        };
        // Action to set onlyr (0 = Red, 1 = Green, 2 = Blue, 3 = White, 4 = Black)
        actions['set_focus_assist_1'] = {
            name: 'Set Focus Assist: Focus Line Color (0 = Red, 1 = Green, 2 = Blue, 3 = White, 4 = Black)',
            description: 'Set value for Focus Assist: Focus Line Color (0 = Red, 1 = Green, 2 = Blue, 3 = White, 4 = Black)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 4.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('focus_assist_0', 1.0, cameraId);
                // For the subindex being modified, use the new value
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('focus_assist_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'focus_assist', valuesString);
            }
        };
        // Action to increment Focus Assist: Focus Line Color (0 = Red, 1 = Green, 2 = Blue, 3 = White, 4 = Black)
        actions['set_focus_assist_1_increment'] = {
            name: '⬆️ Increase Focus Assist: Focus Line Color (0 = Red, 1 = Green, 2 = Blue, 3 = White, 4 = Black)',
            description: 'Increase the value of Focus Assist: Focus Line Color (0 = Red, 1 = Green, 2 = Blue, 3 = White, 4 = Black)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 4.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('focus_assist_1', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(4.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('focus_assist_0', 1.0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('focus_assist_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'focus_assist', valuesString);
            }
        };
        // Action to decrement Focus Assist: Focus Line Color (0 = Red, 1 = Green, 2 = Blue, 3 = White, 4 = Black)
        actions['set_focus_assist_1_decrement'] = {
            name: '⬇️ Decrease Focus Assist: Focus Line Color (0 = Red, 1 = Green, 2 = Blue, 3 = White, 4 = Black)',
            description: 'Decrease the value of Focus Assist: Focus Line Color (0 = Red, 1 = Green, 2 = Blue, 3 = White, 4 = Black)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 4.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('focus_assist_1', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('focus_assist_0', 1.0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('focus_assist_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'focus_assist', valuesString);
            }
        };
        // Action to reset Focus Assist: Focus Line Color (0 = Red, 1 = Green, 2 = Blue, 3 = White, 4 = Black) al default value
        actions['set_focus_assist_1_reset'] = {
            name: '🔄 Reset Focus Assist: Focus Line Color (0 = Red, 1 = Green, 2 = Blue, 3 = White, 4 = Black)',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 0.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('focus_assist_0', 1.0, cameraId);
                // For the subindex being reset, use its default value
                values[1] = resetValue;
                
                // Store default value specifically for this camera
                this.storeParamValue('focus_assist_1', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'focus_assist', valuesString);
            }
        };
        // Action for Program return feed enable (numeric)
        actions['set_program_return_feed_enable'] = {
            name: 'Set Program return feed enable',
            description: 'Group: Display | Param: Program return feed enable | Note: 0 = Disable, 1-30 = Enable with timeout (seconds)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 30.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'program_return_feed_enable', event.options.value);
            }
        };
        // Action to increment Program return feed enable
        actions['set_program_return_feed_enable_increment'] = {
            name: '⬆️ Increase Program return feed enable',
            description: 'Increase the value of Program return feed enable',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 30.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('program_return_feed_enable', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(30.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'program_return_feed_enable', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('program_return_feed_enable', newValue, cameraId);
            }
        };
        // Action to decrement Program return feed enable
        actions['set_program_return_feed_enable_decrement'] = {
            name: '⬇️ Decrease Program return feed enable',
            description: 'Decrease the value of Program return feed enable',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 30.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('program_return_feed_enable', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'program_return_feed_enable', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('program_return_feed_enable', newValue, cameraId);
            }
        };
        // Action to reset Program return feed enable al default value
        actions['set_program_return_feed_enable_reset'] = {
            name: '🔄 Reset Program return feed enable',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'program_return_feed_enable', 0.0);
                
                // Store default value for this specific camera
                this.storeParamValue('program_return_feed_enable', 0.0, cameraId);
            }
        };
        // Action for Timecode Source [0] (numeric)
        actions['set_timecode_source_0'] = {
            name: 'Set Timecode Source [0]',
            description: 'Group: Display | Param: Timecode Source | Note: Source (0 = Clip, 1 = Timecode)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'timecode_source_0', event.options.value);
            }
        };
        // Action to increment Timecode Source [0]
        actions['set_timecode_source_0_increment'] = {
            name: '⬆️ Increase Timecode Source [0]',
            description: 'Increase the value of Timecode Source [0]',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('timecode_source_0', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'timecode_source_0', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('timecode_source_0', newValue, cameraId);
            }
        };
        // Action to decrement Timecode Source [0]
        actions['set_timecode_source_0_decrement'] = {
            name: '⬇️ Decrease Timecode Source [0]',
            description: 'Decrease the value of Timecode Source [0]',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 1.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('timecode_source_0', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'timecode_source_0', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('timecode_source_0', newValue, cameraId);
            }
        };
        // Action to reset Timecode Source [0] al default value
        actions['set_timecode_source_0_reset'] = {
            name: '🔄 Reset Timecode Source [0]',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'timecode_source_0', 0.0);
                
                // Store default value for this specific camera
                this.storeParamValue('timecode_source_0', 0.0, cameraId);
            }
        };
        // Action for Tally brightness (numeric)
        actions['set_tally_brightness'] = {
            name: 'Set Tally brightness',
            description: 'Group: Tally | Param: Tally brightness | Note: Sets the tally front and rear brightness. 0.0 = minimum, 1.0 = maximum',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 1.0,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'tally_brightness', event.options.value);
            }
        };
        // Action to increment Tally brightness
        actions['set_tally_brightness_increment'] = {
            name: '⬆️ Increase Tally brightness',
            description: 'Increase the value of Tally brightness',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('tally_brightness', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'tally_brightness', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('tally_brightness', newValue, cameraId);
            }
        };
        // Action to decrement Tally brightness
        actions['set_tally_brightness_decrement'] = {
            name: '⬇️ Decrease Tally brightness',
            description: 'Decrease the value of Tally brightness',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('tally_brightness', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'tally_brightness', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('tally_brightness', newValue, cameraId);
            }
        };
        // Action to reset Tally brightness al default value
        actions['set_tally_brightness_reset'] = {
            name: '🔄 Reset Tally brightness',
            description: 'Reset to default value (1.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'tally_brightness', 1.0);
                
                // Store default value for this specific camera
                this.storeParamValue('tally_brightness', 1.0, cameraId);
            }
        };
        // Action for Front tally brightness (numeric)
        actions['set_front_tally_brightness'] = {
            name: 'Set Front tally brightness',
            description: 'Group: Tally | Param: Front tally brightness | Note: Sets the tally front brightness. 0.0 = minimum, 1.0 = maximum',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 1.0,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'front_tally_brightness', event.options.value);
            }
        };
        // Action to increment Front tally brightness
        actions['set_front_tally_brightness_increment'] = {
            name: '⬆️ Increase Front tally brightness',
            description: 'Increase the value of Front tally brightness',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('front_tally_brightness', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'front_tally_brightness', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('front_tally_brightness', newValue, cameraId);
            }
        };
        // Action to decrement Front tally brightness
        actions['set_front_tally_brightness_decrement'] = {
            name: '⬇️ Decrease Front tally brightness',
            description: 'Decrease the value of Front tally brightness',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('front_tally_brightness', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'front_tally_brightness', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('front_tally_brightness', newValue, cameraId);
            }
        };
        // Action to reset Front tally brightness al default value
        actions['set_front_tally_brightness_reset'] = {
            name: '🔄 Reset Front tally brightness',
            description: 'Reset to default value (1.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'front_tally_brightness', 1.0);
                
                // Store default value for this specific camera
                this.storeParamValue('front_tally_brightness', 1.0, cameraId);
            }
        };
        // Action for Rear tally brightness (numeric)
        actions['set_rear_tally_brightness'] = {
            name: 'Set Rear tally brightness',
            description: 'Group: Tally | Param: Rear tally brightness | Note: Sets the tally rear brightness. 0.0 = minimum, 1.0 = maximum (cannot be turned off)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 1.0,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'rear_tally_brightness', event.options.value);
            }
        };
        // Action to increment Rear tally brightness
        actions['set_rear_tally_brightness_increment'] = {
            name: '⬆️ Increase Rear tally brightness',
            description: 'Increase the value of Rear tally brightness',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('rear_tally_brightness', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'rear_tally_brightness', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('rear_tally_brightness', newValue, cameraId);
            }
        };
        // Action to decrement Rear tally brightness
        actions['set_rear_tally_brightness_decrement'] = {
            name: '⬇️ Decrease Rear tally brightness',
            description: 'Decrease the value of Rear tally brightness',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('rear_tally_brightness', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'rear_tally_brightness', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('rear_tally_brightness', newValue, cameraId);
            }
        };
        // Action to reset Rear tally brightness al default value
        actions['set_rear_tally_brightness_reset'] = {
            name: '🔄 Reset Rear tally brightness',
            description: 'Reset to default value (1.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'rear_tally_brightness', 1.0);
                
                // Store default value for this specific camera
                this.storeParamValue('rear_tally_brightness', 1.0, cameraId);
            }
        };
        // Action for Source (numeric)
        actions['set_source'] = {
            name: 'Set Source',
            description: 'Group: Reference | Param: Source | Note: 0 = Internal, 1 = Program, 2 = External',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.0,
                    min: 0.0,
                    max: 2.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'source', event.options.value);
            }
        };
        // Action to increment Source
        actions['set_source_increment'] = {
            name: '⬆️ Increase Source',
            description: 'Increase the value of Source',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 2.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('source', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(2.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'source', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('source', newValue, cameraId);
            }
        };
        // Action to decrement Source
        actions['set_source_decrement'] = {
            name: '⬇️ Decrease Source',
            description: 'Decrease the value of Source',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 2.0,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('source', 0.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'source', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('source', newValue, cameraId);
            }
        };
        // Action to reset Source al default value
        actions['set_source_reset'] = {
            name: '🔄 Reset Source',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'source', 0.0);
                
                // Store default value for this specific camera
                this.storeParamValue('source', 0.0, cameraId);
            }
        };
        // Action for Offset (numeric)
        actions['set_offset'] = {
            name: 'Set Offset',
            description: 'Group: Reference | Param: Offset | Note: +/- offset in pixels',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0,
                    min: 0,
                    max: 1,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'offset', event.options.value);
            }
        };
        // Action to increment Offset
        actions['set_offset_increment'] = {
            name: '⬆️ Increase Offset',
            description: 'Increase the value of Offset',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 1,
                    min: 1,
                    max: 1,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('offset', 0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'offset', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('offset', newValue, cameraId);
            }
        };
        // Action to decrement Offset
        actions['set_offset_decrement'] = {
            name: '⬇️ Decrease Offset',
            description: 'Decrease the value of Offset',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 1,
                    min: 1,
                    max: 1,
                    step: 1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('offset', 0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'offset', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('offset', newValue, cameraId);
            }
        };
        // Action to reset Offset al default value
        actions['set_offset_reset'] = {
            name: '🔄 Reset Offset',
            description: 'Reset to default value (0)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'offset', 0);
                
                // Store default value for this specific camera
                this.storeParamValue('offset', 0, cameraId);
            }
        };
        // Action for Contrast Adjust (multiple subindexes)
        actions['set_contrast_adjust'] = {
            name: 'Set Contrast Adjust',
            description: 'Set values for Contrast Adjust',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Pivot',
                    id: 'value0',
                    default: 0.5,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: 'Adjust',
                    id: 'value1',
                    default: 1.0,
                    min: 0.0,
                    max: 2.0,
                    step: 0.1
                },
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const values = [];
                values[0] = event.options.value0;
                values[1] = event.options.value1;
                const valuesString = values.join(",");
                this.sendParam(cameraId, 'contrast_adjust', valuesString);
                
                // Store individual values for each subindex
                
                this.storeParamValue('contrast_adjust_0', event.options.value0, cameraId);
                this.storeParamValue('contrast_adjust_1', event.options.value1, cameraId);
            }
        };
        // Action to set only Contrast Adjust: Pivot
        actions['set_contrast_adjust_0'] = {
            name: 'Set Contrast Adjust: Pivot',
            description: 'Set value for Contrast Adjust: Pivot',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 0.5,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For the subindex being modified, use the new value
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('contrast_adjust_1', 1.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('contrast_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'contrast_adjust', valuesString);
            }
        };
        // Action to increment Contrast Adjust: Pivot
        actions['set_contrast_adjust_0_increment'] = {
            name: '⬆️ Increase Contrast Adjust: Pivot',
            description: 'Increase the value of Contrast Adjust: Pivot',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('contrast_adjust_0', 0.5, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value incrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('contrast_adjust_1', 1.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('contrast_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'contrast_adjust', valuesString);
            }
        };
        // Action to decrement Contrast Adjust: Pivot
        actions['set_contrast_adjust_0_decrement'] = {
            name: '⬇️ Decrease Contrast Adjust: Pivot',
            description: 'Decrease the value of Contrast Adjust: Pivot',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('contrast_adjust_0', 0.5, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value decrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('contrast_adjust_1', 1.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('contrast_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'contrast_adjust', valuesString);
            }
        };
        // Action to reset Contrast Adjust: Pivot al default value
        actions['set_contrast_adjust_0_reset'] = {
            name: '🔄 Reset Contrast Adjust: Pivot',
            description: 'Reset to default value (0.50)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 0.5;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being reset, use its default value
                values[0] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('contrast_adjust_1', 1.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('contrast_adjust_0', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'contrast_adjust', valuesString);
            }
        };
        // Action to set only Contrast Adjust: Adjust
        actions['set_contrast_adjust_1'] = {
            name: 'Set Contrast Adjust: Adjust',
            description: 'Set value for Contrast Adjust: Adjust',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 1.0,
                    min: 0.0,
                    max: 2.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('contrast_adjust_0', 0.5, cameraId);
                // For the subindex being modified, use the new value
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('contrast_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'contrast_adjust', valuesString);
            }
        };
        // Action to increment Contrast Adjust: Adjust
        actions['set_contrast_adjust_1_increment'] = {
            name: '⬆️ Increase Contrast Adjust: Adjust',
            description: 'Increase the value of Contrast Adjust: Adjust',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 2.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('contrast_adjust_1', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(2.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('contrast_adjust_0', 0.5, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('contrast_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'contrast_adjust', valuesString);
            }
        };
        // Action to decrement Contrast Adjust: Adjust
        actions['set_contrast_adjust_1_decrement'] = {
            name: '⬇️ Decrease Contrast Adjust: Adjust',
            description: 'Decrease the value of Contrast Adjust: Adjust',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 2.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('contrast_adjust_1', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('contrast_adjust_0', 0.5, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('contrast_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'contrast_adjust', valuesString);
            }
        };
        // Action to reset Contrast Adjust: Adjust al default value
        actions['set_contrast_adjust_1_reset'] = {
            name: '🔄 Reset Contrast Adjust: Adjust',
            description: 'Reset to default value (1.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 1.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('contrast_adjust_0', 0.5, cameraId);
                // For the subindex being reset, use its default value
                values[1] = resetValue;
                
                // Store default value specifically for this camera
                this.storeParamValue('contrast_adjust_1', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'contrast_adjust', valuesString);
            }
        };
        // Action for Color Adjust (multiple subindexes)
        actions['set_color_adjust'] = {
            name: 'Set Color Adjust',
            description: 'Set values for Color Adjust',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Hue',
                    id: 'value0',
                    default: -1.0,
                    min: -1.0,
                    max: 1.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: 'Saturation',
                    id: 'value1',
                    default: 1.0,
                    min: 0.0,
                    max: 2.0,
                    step: 0.1
                },
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const values = [];
                values[0] = event.options.value0;
                values[1] = event.options.value1;
                const valuesString = values.join(",");
                this.sendParam(cameraId, 'color_adjust', valuesString);
                
                // Store individual values for each subindex
                
                this.storeParamValue('color_adjust_0', event.options.value0, cameraId);
                this.storeParamValue('color_adjust_1', event.options.value1, cameraId);
            }
        };
        // Action to set onlyr Adjust: Hue
        actions['set_color_adjust_0'] = {
            name: 'Set Color Adjust: Hue',
            description: 'Set value for Color Adjust: Hue',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: -1.0,
                    min: -1.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For the subindex being modified, use the new value
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('color_adjust_1', 1.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('color_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'color_adjust', valuesString);
            }
        };
        // Action to increment Color Adjust: Hue
        actions['set_color_adjust_0_increment'] = {
            name: '⬆️ Increase Color Adjust: Hue',
            description: 'Increase the value of Color Adjust: Hue',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 2.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('color_adjust_0', -1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value incrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('color_adjust_1', 1.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('color_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'color_adjust', valuesString);
            }
        };
        // Action to decrement Color Adjust: Hue
        actions['set_color_adjust_0_decrement'] = {
            name: '⬇️ Decrease Color Adjust: Hue',
            description: 'Decrease the value of Color Adjust: Hue',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 2.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('color_adjust_0', -1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(-1.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value decrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('color_adjust_1', 1.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('color_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'color_adjust', valuesString);
            }
        };
        // Action to reset Color Adjust: Hue al default value
        actions['set_color_adjust_0_reset'] = {
            name: '🔄 Reset Color Adjust: Hue',
            description: 'Reset to default value (-1.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = -1.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being reset, use its default value
                values[0] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('color_adjust_1', 1.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('color_adjust_0', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'color_adjust', valuesString);
            }
        };
        // Action to set onlyr Adjust: Saturation
        actions['set_color_adjust_1'] = {
            name: 'Set Color Adjust: Saturation',
            description: 'Set value for Color Adjust: Saturation',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 1.0,
                    min: 0.0,
                    max: 2.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('color_adjust_0', 0, cameraId);
                // For the subindex being modified, use the new value
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('color_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'color_adjust', valuesString);
            }
        };
        // Action to increment Color Adjust: Saturation
        actions['set_color_adjust_1_increment'] = {
            name: '⬆️ Increase Color Adjust: Saturation',
            description: 'Increase the value of Color Adjust: Saturation',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 2.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('color_adjust_1', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(2.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('color_adjust_0', 0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('color_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'color_adjust', valuesString);
            }
        };
        // Action to decrement Color Adjust: Saturation
        actions['set_color_adjust_1_decrement'] = {
            name: '⬇️ Decrease Color Adjust: Saturation',
            description: 'Decrease the value of Color Adjust: Saturation',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 2.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('color_adjust_1', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('color_adjust_0', 0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[1] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('color_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'color_adjust', valuesString);
            }
        };
        // Action to reset Color Adjust: Saturation al default value
        actions['set_color_adjust_1_reset'] = {
            name: '🔄 Reset Color Adjust: Saturation',
            description: 'Reset to default value (1.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 1.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('color_adjust_0', 0, cameraId);
                // For the subindex being reset, use its default value
                values[1] = resetValue;
                
                // Store default value specifically for this camera
                this.storeParamValue('color_adjust_1', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'color_adjust', valuesString);
            }
        };
        // Action for Lift Adjust (multiple subindexes)
        actions['set_lift_adjust'] = {
            name: 'Set Lift Adjust',
            description: 'Set values for Lift Adjust',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Red',
                    id: 'value0',
                    default: -2.0,
                    min: -2.0,
                    max: 2.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: 'Green',
                    id: 'value1',
                    default: -2.0,
                    min: -2.0,
                    max: 2.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: 'Blue',
                    id: 'value2',
                    default: -2.0,
                    min: -2.0,
                    max: 2.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: 'Luma',
                    id: 'value3',
                    default: -2.0,
                    min: -2.0,
                    max: 2.0,
                    step: 0.1
                },
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const values = [];
                values[0] = event.options.value0;
                values[1] = event.options.value1;
                values[2] = event.options.value2;
                values[3] = event.options.value3;
                const valuesString = values.join(",");
                this.sendParam(cameraId, 'lift_adjust', valuesString);
                
                // Store individual values for each subindex
                
                this.storeParamValue('lift_adjust_0', event.options.value0, cameraId);
                this.storeParamValue('lift_adjust_1', event.options.value1, cameraId);
                this.storeParamValue('lift_adjust_2', event.options.value2, cameraId);
                this.storeParamValue('lift_adjust_3', event.options.value3, cameraId);
            }
        };
        // Action to set only Lift Adjust: Red
        actions['set_lift_adjust_0'] = {
            name: 'Set Lift Adjust: Red',
            description: 'Set value for Lift Adjust: Red',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: -2.0,
                    min: -2.0,
                    max: 2.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For the subindex being modified, use the new value
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('lift_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('lift_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('lift_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('lift_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action to increment Lift Adjust: Red
        actions['set_lift_adjust_0_increment'] = {
            name: '⬆️ Increase Lift Adjust: Red',
            description: 'Increase the value of Lift Adjust: Red',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 4.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('lift_adjust_0', -2.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(2.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value incrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('lift_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('lift_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('lift_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('lift_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action to decrement Lift Adjust: Red
        actions['set_lift_adjust_0_decrement'] = {
            name: '⬇️ Decrease Lift Adjust: Red',
            description: 'Decrease the value of Lift Adjust: Red',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 4.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('lift_adjust_0', -2.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(-2.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value decrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('lift_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('lift_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('lift_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('lift_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action to reset Lift Adjust: Red al default value
        actions['set_lift_adjust_0_reset'] = {
            name: '🔄 Reset Lift Adjust: Red',
            description: 'Reset to default value (-2.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = -2.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being reset, use its default value
                values[0] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('lift_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('lift_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('lift_adjust_3', 0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('lift_adjust_0', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action to set only Lift Adjust: Green
        actions['set_lift_adjust_1'] = {
            name: 'Set Lift Adjust: Green',
            description: 'Set value for Lift Adjust: Green',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: -2.0,
                    min: -2.0,
                    max: 2.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('lift_adjust_0', 0, cameraId);
                // For the subindex being modified, use the new value
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('lift_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('lift_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('lift_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action to increment Lift Adjust: Green
        actions['set_lift_adjust_1_increment'] = {
            name: '⬆️ Increase Lift Adjust: Green',
            description: 'Increase the value of Lift Adjust: Green',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 4.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('lift_adjust_1', -2.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(2.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('lift_adjust_0', 0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('lift_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('lift_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('lift_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action to decrement Lift Adjust: Green
        actions['set_lift_adjust_1_decrement'] = {
            name: '⬇️ Decrease Lift Adjust: Green',
            description: 'Decrease the value of Lift Adjust: Green',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 4.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('lift_adjust_1', -2.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(-2.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('lift_adjust_0', 0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('lift_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('lift_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('lift_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action to reset Lift Adjust: Green al default value
        actions['set_lift_adjust_1_reset'] = {
            name: '🔄 Reset Lift Adjust: Green',
            description: 'Reset to default value (-2.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = -2.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('lift_adjust_0', 0, cameraId);
                // For the subindex being reset, use its default value
                values[1] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('lift_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('lift_adjust_3', 0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('lift_adjust_1', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action to set only Lift Adjust: Blue
        actions['set_lift_adjust_2'] = {
            name: 'Set Lift Adjust: Blue',
            description: 'Set value for Lift Adjust: Blue',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: -2.0,
                    min: -2.0,
                    max: 2.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('lift_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('lift_adjust_1', 0, cameraId);
                // For the subindex being modified, use the new value
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('lift_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('lift_adjust_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action to increment Lift Adjust: Blue
        actions['set_lift_adjust_2_increment'] = {
            name: '⬆️ Increase Lift Adjust: Blue',
            description: 'Increase the value of Lift Adjust: Blue',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 4.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('lift_adjust_2', -2.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(2.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('lift_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('lift_adjust_1', 0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('lift_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('lift_adjust_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action to decrement Lift Adjust: Blue
        actions['set_lift_adjust_2_decrement'] = {
            name: '⬇️ Decrease Lift Adjust: Blue',
            description: 'Decrease the value of Lift Adjust: Blue',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 4.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('lift_adjust_2', -2.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(-2.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('lift_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('lift_adjust_1', 0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('lift_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('lift_adjust_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action to reset Lift Adjust: Blue al default value
        actions['set_lift_adjust_2_reset'] = {
            name: '🔄 Reset Lift Adjust: Blue',
            description: 'Reset to default value (-2.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = -2.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('lift_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('lift_adjust_1', 0, cameraId);
                // For the subindex being reset, use its default value
                values[2] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('lift_adjust_3', 0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('lift_adjust_2', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action to set only Lift Adjust: Luma
        actions['set_lift_adjust_3'] = {
            name: 'Set Lift Adjust: Luma',
            description: 'Set value for Lift Adjust: Luma',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: -2.0,
                    min: -2.0,
                    max: 2.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('lift_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('lift_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('lift_adjust_2', 0, cameraId);
                // For the subindex being modified, use the new value
                values[3] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('lift_adjust_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action to increment Lift Adjust: Luma
        actions['set_lift_adjust_3_increment'] = {
            name: '⬆️ Increase Lift Adjust: Luma',
            description: 'Increase the value of Lift Adjust: Luma',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 4.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('lift_adjust_3', -2.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(2.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('lift_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('lift_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('lift_adjust_2', 0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[3] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('lift_adjust_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action to decrement Lift Adjust: Luma
        actions['set_lift_adjust_3_decrement'] = {
            name: '⬇️ Decrease Lift Adjust: Luma',
            description: 'Decrease the value of Lift Adjust: Luma',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 4.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('lift_adjust_3', -2.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(-2.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('lift_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('lift_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('lift_adjust_2', 0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[3] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('lift_adjust_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action to reset Lift Adjust: Luma al default value
        actions['set_lift_adjust_3_reset'] = {
            name: '🔄 Reset Lift Adjust: Luma',
            description: 'Reset to default value (-2.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = -2.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('lift_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('lift_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('lift_adjust_2', 0, cameraId);
                // For the subindex being reset, use its default value
                values[3] = resetValue;
                
                // Store default value specifically for this camera
                this.storeParamValue('lift_adjust_3', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'lift_adjust', valuesString);
            }
        };
        // Action for Gamultiple subindexes)
        actions['set_gamma_adjust'] = {
            name: 'Set Gamma Adjust',
            description: 'Set values for Gamma Adjust',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Red',
                    id: 'value0',
                    default: -4.0,
                    min: -4.0,
                    max: 4.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: 'Green',
                    id: 'value1',
                    default: -4.0,
                    min: -4.0,
                    max: 4.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: 'Blue',
                    id: 'value2',
                    default: -4.0,
                    min: -4.0,
                    max: 4.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: 'Luma',
                    id: 'value3',
                    default: -4.0,
                    min: -4.0,
                    max: 4.0,
                    step: 0.1
                },
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const values = [];
                values[0] = event.options.value0;
                values[1] = event.options.value1;
                values[2] = event.options.value2;
                values[3] = event.options.value3;
                const valuesString = values.join(",");
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
                
                // Store individual values for each subindex
                
                this.storeParamValue('gamma_adjust_0', event.options.value0, cameraId);
                this.storeParamValue('gamma_adjust_1', event.options.value1, cameraId);
                this.storeParamValue('gamma_adjust_2', event.options.value2, cameraId);
                this.storeParamValue('gamma_adjust_3', event.options.value3, cameraId);
            }
        };
        // Action to set only Gamma Adjust: Red
        actions['set_gamma_adjust_0'] = {
            name: 'Set Gamma Adjust: Red',
            description: 'Set value for Gamma Adjust: Red',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: -4.0,
                    min: -4.0,
                    max: 4.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For the subindex being modified, use the new value
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gamma_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gamma_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gamma_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gamma_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action to increment Gamma Adjust: Red
        actions['set_gamma_adjust_0_increment'] = {
            name: '⬆️ Increase Gamma Adjust: Red',
            description: 'Increase the value of Gamma Adjust: Red',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 8.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gamma_adjust_0', -4.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(4.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value incrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gamma_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gamma_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gamma_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gamma_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action to decrement Gamma Adjust: Red
        actions['set_gamma_adjust_0_decrement'] = {
            name: '⬇️ Decrease Gamma Adjust: Red',
            description: 'Decrease the value of Gamma Adjust: Red',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 8.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gamma_adjust_0', -4.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(-4.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value decrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gamma_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gamma_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gamma_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gamma_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action to reset Gamma Adjust: Red al default value
        actions['set_gamma_adjust_0_reset'] = {
            name: '🔄 Reset Gamma Adjust: Red',
            description: 'Reset to default value (-4.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = -4.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being reset, use its default value
                values[0] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gamma_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gamma_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gamma_adjust_3', 0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('gamma_adjust_0', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action to set only Gamma Adjust: Green
        actions['set_gamma_adjust_1'] = {
            name: 'Set Gamma Adjust: Green',
            description: 'Set value for Gamma Adjust: Green',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: -4.0,
                    min: -4.0,
                    max: 4.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gamma_adjust_0', 0, cameraId);
                // For the subindex being modified, use the new value
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gamma_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gamma_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gamma_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action to increment Gamma Adjust: Green
        actions['set_gamma_adjust_1_increment'] = {
            name: '⬆️ Increase Gamma Adjust: Green',
            description: 'Increase the value of Gamma Adjust: Green',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 8.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gamma_adjust_1', -4.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(4.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gamma_adjust_0', 0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gamma_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gamma_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gamma_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action to decrement Gamma Adjust: Green
        actions['set_gamma_adjust_1_decrement'] = {
            name: '⬇️ Decrease Gamma Adjust: Green',
            description: 'Decrease the value of Gamma Adjust: Green',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 8.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gamma_adjust_1', -4.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(-4.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gamma_adjust_0', 0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gamma_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gamma_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gamma_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action to reset Gamma Adjust: Green al default value
        actions['set_gamma_adjust_1_reset'] = {
            name: '🔄 Reset Gamma Adjust: Green',
            description: 'Reset to default value (-4.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = -4.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gamma_adjust_0', 0, cameraId);
                // For the subindex being reset, use its default value
                values[1] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gamma_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gamma_adjust_3', 0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('gamma_adjust_1', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action to set only Gamma Adjust: Blue
        actions['set_gamma_adjust_2'] = {
            name: 'Set Gamma Adjust: Blue',
            description: 'Set value for Gamma Adjust: Blue',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: -4.0,
                    min: -4.0,
                    max: 4.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gamma_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gamma_adjust_1', 0, cameraId);
                // For the subindex being modified, use the new value
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gamma_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gamma_adjust_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action to increment Gamma Adjust: Blue
        actions['set_gamma_adjust_2_increment'] = {
            name: '⬆️ Increase Gamma Adjust: Blue',
            description: 'Increase the value of Gamma Adjust: Blue',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 8.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gamma_adjust_2', -4.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(4.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gamma_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gamma_adjust_1', 0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gamma_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gamma_adjust_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action to decrement Gamma Adjust: Blue
        actions['set_gamma_adjust_2_decrement'] = {
            name: '⬇️ Decrease Gamma Adjust: Blue',
            description: 'Decrease the value of Gamma Adjust: Blue',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 8.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gamma_adjust_2', -4.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(-4.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gamma_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gamma_adjust_1', 0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gamma_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gamma_adjust_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action to reset Gamma Adjust: Blue al default value
        actions['set_gamma_adjust_2_reset'] = {
            name: '🔄 Reset Gamma Adjust: Blue',
            description: 'Reset to default value (-4.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = -4.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gamma_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gamma_adjust_1', 0, cameraId);
                // For the subindex being reset, use its default value
                values[2] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gamma_adjust_3', 0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('gamma_adjust_2', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action to set only Gamma Adjust: Luma
        actions['set_gamma_adjust_3'] = {
            name: 'Set Gamma Adjust: Luma',
            description: 'Set value for Gamma Adjust: Luma',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: -4.0,
                    min: -4.0,
                    max: 4.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gamma_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gamma_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gamma_adjust_2', 0, cameraId);
                // For the subindex being modified, use the new value
                values[3] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('gamma_adjust_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action to increment Gamma Adjust: Luma
        actions['set_gamma_adjust_3_increment'] = {
            name: '⬆️ Increase Gamma Adjust: Luma',
            description: 'Increase the value of Gamma Adjust: Luma',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 8.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gamma_adjust_3', -4.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(4.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gamma_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gamma_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gamma_adjust_2', 0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[3] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('gamma_adjust_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action to decrement Gamma Adjust: Luma
        actions['set_gamma_adjust_3_decrement'] = {
            name: '⬇️ Decrease Gamma Adjust: Luma',
            description: 'Decrease the value of Gamma Adjust: Luma',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 8.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gamma_adjust_3', -4.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(-4.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gamma_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gamma_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gamma_adjust_2', 0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[3] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('gamma_adjust_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action to reset Gamma Adjust: Luma al default value
        actions['set_gamma_adjust_3_reset'] = {
            name: '🔄 Reset Gamma Adjust: Luma',
            description: 'Reset to default value (-4.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = -4.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gamma_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gamma_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gamma_adjust_2', 0, cameraId);
                // For the subindex being reset, use its default value
                values[3] = resetValue;
                
                // Store default value specifically for this camera
                this.storeParamValue('gamma_adjust_3', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gamma_adjust', valuesString);
            }
        };
        // Action for Gain Adjust (multiple subindexes)
        actions['set_gain_adjust'] = {
            name: 'Set Gain Adjust',
            description: 'Set values for Gain Adjust',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Red',
                    id: 'value0',
                    default: 1.0,
                    min: 0.0,
                    max: 16.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: 'Green',
                    id: 'value1',
                    default: 1.0,
                    min: 0.0,
                    max: 16.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: 'Blue',
                    id: 'value2',
                    default: 1.0,
                    min: 0.0,
                    max: 16.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: 'Luma',
                    id: 'value3',
                    default: 1.0,
                    min: 0.0,
                    max: 16.0,
                    step: 0.1
                },
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const values = [];
                values[0] = event.options.value0;
                values[1] = event.options.value1;
                values[2] = event.options.value2;
                values[3] = event.options.value3;
                const valuesString = values.join(",");
                this.sendParam(cameraId, 'gain_adjust', valuesString);
                
                // Store individual values for each subindex
                
                this.storeParamValue('gain_adjust_0', event.options.value0, cameraId);
                this.storeParamValue('gain_adjust_1', event.options.value1, cameraId);
                this.storeParamValue('gain_adjust_2', event.options.value2, cameraId);
                this.storeParamValue('gain_adjust_3', event.options.value3, cameraId);
            }
        };
        // Action to set only Gain Adjust: Red
        actions['set_gain_adjust_0'] = {
            name: 'Set Gain Adjust: Red',
            description: 'Set value for Gain Adjust: Red',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 1.0,
                    min: 0.0,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For the subindex being modified, use the new value
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gain_adjust_1', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gain_adjust_2', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gain_adjust_3', 1.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gain_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action to increment Gain Adjust: Red
        actions['set_gain_adjust_0_increment'] = {
            name: '⬆️ Increase Gain Adjust: Red',
            description: 'Increase the value of Gain Adjust: Red',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gain_adjust_0', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(16.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value incrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gain_adjust_1', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gain_adjust_2', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gain_adjust_3', 1.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gain_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action to decrement Gain Adjust: Red
        actions['set_gain_adjust_0_decrement'] = {
            name: '⬇️ Decrease Gain Adjust: Red',
            description: 'Decrease the value of Gain Adjust: Red',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gain_adjust_0', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value decrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gain_adjust_1', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gain_adjust_2', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gain_adjust_3', 1.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gain_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action to reset Gain Adjust: Red al default value
        actions['set_gain_adjust_0_reset'] = {
            name: '🔄 Reset Gain Adjust: Red',
            description: 'Reset to default value (1.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 1.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being reset, use its default value
                values[0] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gain_adjust_1', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gain_adjust_2', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gain_adjust_3', 1.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('gain_adjust_0', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action to set only Gain Adjust: Green
        actions['set_gain_adjust_1'] = {
            name: 'Set Gain Adjust: Green',
            description: 'Set value for Gain Adjust: Green',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 1.0,
                    min: 0.0,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gain_adjust_0', 1.0, cameraId);
                // For the subindex being modified, use the new value
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gain_adjust_2', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gain_adjust_3', 1.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gain_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action to increment Gain Adjust: Green
        actions['set_gain_adjust_1_increment'] = {
            name: '⬆️ Increase Gain Adjust: Green',
            description: 'Increase the value of Gain Adjust: Green',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gain_adjust_1', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(16.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gain_adjust_0', 1.0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gain_adjust_2', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gain_adjust_3', 1.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gain_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action to decrement Gain Adjust: Green
        actions['set_gain_adjust_1_decrement'] = {
            name: '⬇️ Decrease Gain Adjust: Green',
            description: 'Decrease the value of Gain Adjust: Green',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gain_adjust_1', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gain_adjust_0', 1.0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gain_adjust_2', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gain_adjust_3', 1.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gain_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action to reset Gain Adjust: Green al default value
        actions['set_gain_adjust_1_reset'] = {
            name: '🔄 Reset Gain Adjust: Green',
            description: 'Reset to default value (1.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 1.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gain_adjust_0', 1.0, cameraId);
                // For the subindex being reset, use its default value
                values[1] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gain_adjust_2', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gain_adjust_3', 1.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('gain_adjust_1', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action to set only Gain Adjust: Blue
        actions['set_gain_adjust_2'] = {
            name: 'Set Gain Adjust: Blue',
            description: 'Set value for Gain Adjust: Blue',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 1.0,
                    min: 0.0,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gain_adjust_0', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gain_adjust_1', 1.0, cameraId);
                // For the subindex being modified, use the new value
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gain_adjust_3', 1.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gain_adjust_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action to increment Gain Adjust: Blue
        actions['set_gain_adjust_2_increment'] = {
            name: '⬆️ Increase Gain Adjust: Blue',
            description: 'Increase the value of Gain Adjust: Blue',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gain_adjust_2', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(16.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gain_adjust_0', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gain_adjust_1', 1.0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gain_adjust_3', 1.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gain_adjust_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action to decrement Gain Adjust: Blue
        actions['set_gain_adjust_2_decrement'] = {
            name: '⬇️ Decrease Gain Adjust: Blue',
            description: 'Decrease the value of Gain Adjust: Blue',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gain_adjust_2', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gain_adjust_0', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gain_adjust_1', 1.0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gain_adjust_3', 1.0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('gain_adjust_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action to reset Gain Adjust: Blue al default value
        actions['set_gain_adjust_2_reset'] = {
            name: '🔄 Reset Gain Adjust: Blue',
            description: 'Reset to default value (1.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 1.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gain_adjust_0', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gain_adjust_1', 1.0, cameraId);
                // For the subindex being reset, use its default value
                values[2] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('gain_adjust_3', 1.0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('gain_adjust_2', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action to set only Gain Adjust: Luma
        actions['set_gain_adjust_3'] = {
            name: 'Set Gain Adjust: Luma',
            description: 'Set value for Gain Adjust: Luma',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 1.0,
                    min: 0.0,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gain_adjust_0', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gain_adjust_1', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gain_adjust_2', 1.0, cameraId);
                // For the subindex being modified, use the new value
                values[3] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('gain_adjust_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action to increment Gain Adjust: Luma
        actions['set_gain_adjust_3_increment'] = {
            name: '⬆️ Increase Gain Adjust: Luma',
            description: 'Increase the value of Gain Adjust: Luma',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gain_adjust_3', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(16.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gain_adjust_0', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gain_adjust_1', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gain_adjust_2', 1.0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[3] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('gain_adjust_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action to decrement Gain Adjust: Luma
        actions['set_gain_adjust_3_decrement'] = {
            name: '⬇️ Decrease Gain Adjust: Luma',
            description: 'Decrease the value of Gain Adjust: Luma',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('gain_adjust_3', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gain_adjust_0', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gain_adjust_1', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gain_adjust_2', 1.0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[3] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('gain_adjust_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action to reset Gain Adjust: Luma al default value
        actions['set_gain_adjust_3_reset'] = {
            name: '🔄 Reset Gain Adjust: Luma',
            description: 'Reset to default value (1.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = 1.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('gain_adjust_0', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('gain_adjust_1', 1.0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('gain_adjust_2', 1.0, cameraId);
                // For the subindex being reset, use its default value
                values[3] = resetValue;
                
                // Store default value specifically for this camera
                this.storeParamValue('gain_adjust_3', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'gain_adjust', valuesString);
            }
        };
        // Action for Offset Adjust (multiple subindexes)
        actions['set_offset_adjust'] = {
            name: 'Set Offset Adjust',
            description: 'Set values for Offset Adjust',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Red',
                    id: 'value0',
                    default: -8.0,
                    min: -8.0,
                    max: 8.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: 'Green',
                    id: 'value1',
                    default: -8.0,
                    min: -8.0,
                    max: 8.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: 'Blue',
                    id: 'value2',
                    default: -8.0,
                    min: -8.0,
                    max: 8.0,
                    step: 0.1
                },
                {
                    type: 'number',
                    label: 'Luma',
                    id: 'value3',
                    default: -8.0,
                    min: -8.0,
                    max: 8.0,
                    step: 0.1
                },
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const values = [];
                values[0] = event.options.value0;
                values[1] = event.options.value1;
                values[2] = event.options.value2;
                values[3] = event.options.value3;
                const valuesString = values.join(",");
                this.sendParam(cameraId, 'offset_adjust', valuesString);
                
                // Store individual values for each subindex
                
                this.storeParamValue('offset_adjust_0', event.options.value0, cameraId);
                this.storeParamValue('offset_adjust_1', event.options.value1, cameraId);
                this.storeParamValue('offset_adjust_2', event.options.value2, cameraId);
                this.storeParamValue('offset_adjust_3', event.options.value3, cameraId);
            }
        };
        // Action to set only Offset Adjust: Red
        actions['set_offset_adjust_0'] = {
            name: 'Set Offset Adjust: Red',
            description: 'Set value for Offset Adjust: Red',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: -8.0,
                    min: -8.0,
                    max: 8.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For the subindex being modified, use the new value
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('offset_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('offset_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('offset_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('offset_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action to increment Offset Adjust: Red
        actions['set_offset_adjust_0_increment'] = {
            name: '⬆️ Increase Offset Adjust: Red',
            description: 'Increase the value of Offset Adjust: Red',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('offset_adjust_0', -8.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(8.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value incrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('offset_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('offset_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('offset_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('offset_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action to decrement Offset Adjust: Red
        actions['set_offset_adjust_0_decrement'] = {
            name: '⬇️ Decrease Offset Adjust: Red',
            description: 'Decrease the value of Offset Adjust: Red',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('offset_adjust_0', -8.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(-8.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being modified, use the new value decrementado
                values[0] = newValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('offset_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('offset_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('offset_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('offset_adjust_0', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action to reset Offset Adjust: Red al default value
        actions['set_offset_adjust_0_reset'] = {
            name: '🔄 Reset Offset Adjust: Red',
            description: 'Reset to default value (-8.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = -8.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For the subindex being reset, use its default value
                values[0] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('offset_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('offset_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('offset_adjust_3', 0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('offset_adjust_0', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action to set only Offset Adjust: Green
        actions['set_offset_adjust_1'] = {
            name: 'Set Offset Adjust: Green',
            description: 'Set value for Offset Adjust: Green',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: -8.0,
                    min: -8.0,
                    max: 8.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('offset_adjust_0', 0, cameraId);
                // For the subindex being modified, use the new value
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('offset_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('offset_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('offset_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action to increment Offset Adjust: Green
        actions['set_offset_adjust_1_increment'] = {
            name: '⬆️ Increase Offset Adjust: Green',
            description: 'Increase the value of Offset Adjust: Green',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('offset_adjust_1', -8.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(8.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('offset_adjust_0', 0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('offset_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('offset_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('offset_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action to decrement Offset Adjust: Green
        actions['set_offset_adjust_1_decrement'] = {
            name: '⬇️ Decrease Offset Adjust: Green',
            description: 'Decrease the value of Offset Adjust: Green',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('offset_adjust_1', -8.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(-8.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('offset_adjust_0', 0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[1] = newValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('offset_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('offset_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('offset_adjust_1', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action to reset Offset Adjust: Green al default value
        actions['set_offset_adjust_1_reset'] = {
            name: '🔄 Reset Offset Adjust: Green',
            description: 'Reset to default value (-8.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = -8.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('offset_adjust_0', 0, cameraId);
                // For the subindex being reset, use its default value
                values[1] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('offset_adjust_2', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('offset_adjust_3', 0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('offset_adjust_1', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action to set only Offset Adjust: Blue
        actions['set_offset_adjust_2'] = {
            name: 'Set Offset Adjust: Blue',
            description: 'Set value for Offset Adjust: Blue',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: -8.0,
                    min: -8.0,
                    max: 8.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('offset_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('offset_adjust_1', 0, cameraId);
                // For the subindex being modified, use the new value
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('offset_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('offset_adjust_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action to increment Offset Adjust: Blue
        actions['set_offset_adjust_2_increment'] = {
            name: '⬆️ Increase Offset Adjust: Blue',
            description: 'Increase the value of Offset Adjust: Blue',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('offset_adjust_2', -8.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(8.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('offset_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('offset_adjust_1', 0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('offset_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('offset_adjust_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action to decrement Offset Adjust: Blue
        actions['set_offset_adjust_2_decrement'] = {
            name: '⬇️ Decrease Offset Adjust: Blue',
            description: 'Decrease the value of Offset Adjust: Blue',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('offset_adjust_2', -8.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(-8.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('offset_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('offset_adjust_1', 0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[2] = newValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('offset_adjust_3', 0, cameraId);
                
                // Store new value specifically for this camera
                this.storeParamValue('offset_adjust_2', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action to reset Offset Adjust: Blue al default value
        actions['set_offset_adjust_2_reset'] = {
            name: '🔄 Reset Offset Adjust: Blue',
            description: 'Reset to default value (-8.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = -8.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('offset_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('offset_adjust_1', 0, cameraId);
                // For the subindex being reset, use its default value
                values[2] = resetValue;
                // For other subindexes, get current value or use their specific default
                values[3] = this.getParamValue('offset_adjust_3', 0, cameraId);
                
                // Store default value specifically for this camera
                this.storeParamValue('offset_adjust_2', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action to set only Offset Adjust: Luma
        actions['set_offset_adjust_3'] = {
            name: 'Set Offset Adjust: Luma',
            description: 'Set value for Offset Adjust: Luma',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: -8.0,
                    min: -8.0,
                    max: 8.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const newValue = event.options.value;
                
                // Get current values or use SPECIFIC defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('offset_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('offset_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('offset_adjust_2', 0, cameraId);
                // For the subindex being modified, use the new value
                values[3] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('offset_adjust_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action to increment Offset Adjust: Luma
        actions['set_offset_adjust_3_increment'] = {
            name: '⬆️ Increase Offset Adjust: Luma',
            description: 'Increase the value of Offset Adjust: Luma',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('offset_adjust_3', -8.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(8.0, currentValue + increment);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('offset_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('offset_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('offset_adjust_2', 0, cameraId);
                // For the subindex being modified, use the new value incrementado
                values[3] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('offset_adjust_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action to decrement Offset Adjust: Luma
        actions['set_offset_adjust_3_decrement'] = {
            name: '⬇️ Decrease Offset Adjust: Luma',
            description: 'Decrease the value of Offset Adjust: Luma',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 16.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('offset_adjust_3', -8.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(-8.0, currentValue - decrement);
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('offset_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('offset_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('offset_adjust_2', 0, cameraId);
                // For the subindex being modified, use the new value decrementado
                values[3] = newValue;
                
                // Store new value specifically for this camera
                this.storeParamValue('offset_adjust_3', newValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action to reset Offset Adjust: Luma al default value
        actions['set_offset_adjust_3_reset'] = {
            name: '🔄 Reset Offset Adjust: Luma',
            description: 'Reset to default value (-8.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const resetValue = -8.0;
                
                // Get current values for other subindexes with their specific defaults
                const values = [];
                // For other subindexes, get current value or use their specific default
                values[0] = this.getParamValue('offset_adjust_0', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[1] = this.getParamValue('offset_adjust_1', 0, cameraId);
                // For other subindexes, get current value or use their specific default
                values[2] = this.getParamValue('offset_adjust_2', 0, cameraId);
                // For the subindex being reset, use its default value
                values[3] = resetValue;
                
                // Store default value specifically for this camera
                this.storeParamValue('offset_adjust_3', resetValue, cameraId);
                
                // Send all values
                const valuesString = values.join(',');
                this.sendParam(cameraId, 'offset_adjust', valuesString);
            }
        };
        // Action for Luma mix (numeric)
        actions['set_luma_mix'] = {
            name: 'Set Luma mix',
            description: 'Group: Color Correction | Param: Luma mix | Note: -',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Value',
                    id: 'value',
                    default: 1.0,
                    min: 0.0,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'luma_mix', event.options.value);
            }
        };
        // Action to increment Luma mix
        actions['set_luma_mix_increment'] = {
            name: '⬆️ Increase Luma mix',
            description: 'Increase the value of Luma mix',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Increment',
                    id: 'increment',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const increment = event.options.increment;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('luma_mix', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.min(1.0, currentValue + increment);
                
                // Send new value
                this.sendParam(cameraId, 'luma_mix', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('luma_mix', newValue, cameraId);
            }
        };
        // Action to decrement Luma mix
        actions['set_luma_mix_decrement'] = {
            name: '⬇️ Decrease Luma mix',
            description: 'Decrease the value of Luma mix',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Decrement',
                    id: 'decrement',
                    default: 0.1,
                    min: 0.1,
                    max: 1.0,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const decrement = event.options.decrement;
                
                // Get current value or use default for this camera
                let currentValue = this.getParamValue('luma_mix', 1.0, cameraId);
                
                // Calcular nuevo valor
                let newValue = Math.max(0.0, currentValue - decrement);
                
                // Send new value
                this.sendParam(cameraId, 'luma_mix', newValue);
                
                // Store new value specifically for this camera
                this.storeParamValue('luma_mix', newValue, cameraId);
            }
        };
        // Action to reset Luma mix al default value
        actions['set_luma_mix_reset'] = {
            name: '🔄 Reset Luma mix',
            description: 'Reset to default value (1.00)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                
                // Send default value
                this.sendParam(cameraId, 'luma_mix', 1.0);
                
                // Store default value for this specific camera
                this.storeParamValue('luma_mix', 1.0, cameraId);
            }
        };
        // Action for Correction Reset Default (void)
        actions['set_correction_reset_default'] = {
            name: 'Trigger Correction Reset Default',
            description: 'Group: Color Correction | Param: Correction Reset Default | Note: Reset to defaults',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'correction_reset_default', '1');
            }
        };
        // Special action for Pan/Tilt Velocity
        actions['set_pan_tilt'] = {
            name: 'Pan/Tilt - Mover',
            description: 'Controls pan/tilt movement of the camera',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'dropdown',
                    label: 'Direction',
                    id: 'direction',
                    default: 'stop',
                    choices: [
                        { id: 'stop', label: 'Detener' },
                        { id: 'left', label: 'Left' },
                        { id: 'right', label: 'Right' },
                        { id: 'up', label: 'Up' },
                        { id: 'down', label: 'Down' },
                        { id: 'up_left', label: 'Up-Left' },
                        { id: 'up_right', label: 'Up-Right' },
                        { id: 'down_left', label: 'Down-Left' },
                        { id: 'down_right', label: 'Down-Right' }
                    ]
                },
                {
                    type: 'number',
                    label: 'Speed (0-1)',
                    id: 'speed',
                    default: 0.5,
                    min: 0,
                    max: 1,
                    step: 0.1
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const direction = event.options.direction;
                const speed = event.options.speed;
                
                // Calculate pan/tilt values based on direction
                let panValue = 0;
                let tiltValue = 0;
                
                switch (direction) {
                    case 'left': panValue = -speed; break;
                    case 'right': panValue = speed; break;
                    case 'up': tiltValue = speed; break;
                    case 'down': tiltValue = -speed; break;
                    case 'up_left': panValue = -speed; tiltValue = speed; break;
                    case 'up_right': panValue = speed; tiltValue = speed; break;
                    case 'down_left': panValue = -speed; tiltValue = -speed; break;
                    case 'down_right': panValue = speed; tiltValue = -speed; break;
                    case 'stop':
                    default:
                        // Do not change values, they are 0
                }
                
                this.sendParam(cameraId, 'pan_tilt_velocity', `${panValue},${tiltValue}`);
            }
        };
        
        // Action for detener Pan/Tilt
        actions['stop_pan_tilt'] = {
            name: 'Pan/Tilt - Detener',
            description: 'Stop any pan/tilt movement',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'pan_tilt_velocity', '0,0');
            }
        };
        // Special action for Memory Preset - Store location
        actions['store_memory_preset'] = {
            name: 'Memory Preset - Store',
            description: 'Store current position in a memory slot',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Slot (1-5)',
                    id: 'slot',
                    default: 1,
                    min: 1,
                    max: 5
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const slot = event.options.slot - 1; // Adjust to 0-based index
                this.sendParam(cameraId, 'memory_preset', `1,${slot}`);
            }
        };
        
        // Special action for Memory Preset - Recall location
        actions['recall_memory_preset'] = {
            name: 'Memory Preset - Recall',
            description: 'Recall a stored position',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Slot (1-5)',
                    id: 'slot',
                    default: 1,
                    min: 1,
                    max: 5
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const slot = event.options.slot - 1; // Adjust to 0-based index
                this.sendParam(cameraId, 'memory_preset', `2,${slot}`);
            }
        };
        // ====================================================================
        // ACCIONES DE PRESETS
        // ====================================================================
        
        actions['load_preset'] = {
            name: 'Load Preset',
            description: 'Load a preset from TallyCCU Pro SD card',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Preset ID (0-4)',
                    id: 'presetId',
                    default: 0,
                    min: 0,
                    max: 4
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const presetId = event.options.presetId;
                
                const url = 'http://' + this.config.host + '/?loadPreset=' + cameraId + ',' + presetId;
                this.log('info', 'Loading preset ' + presetId + ' for camera ' + cameraId);
                
                try {
                    const response = await axios.get(url, { timeout: 10000 });
                    
                    let success = false;
                    let data = null;
                    
                    // El Arduino puede responder "OK" (texto plano) o JSON con success
                    if (typeof response.data === 'string') {
                        if (response.data.trim() === 'OK' || response.data.includes('OK')) {
                            success = true;
                        } else {
                            // Intentar parsear como JSON
                            const jsonMatch = response.data.match(/\{.*\}/s);
                            if (jsonMatch) {
                                data = JSON.parse(jsonMatch[0]);
                                success = data.success === true;
                            }
                        }
                    } else if (typeof response.data === 'object') {
                        data = response.data;
                        success = data.success === true;
                    }
                    
                    if (success) {
                        this.log('info', 'Preset cargado correctamente');
                        
                        // Actualizar variables de preset activo
                        const presetName = (this.presetNames[cameraId] && this.presetNames[cameraId][presetId]) 
                            ? this.presetNames[cameraId][presetId] 
                            : `Preset ${presetId}`;
                        
                        const variables = {};
                        variables[`cam${cameraId}_active_preset_name`] = presetName;
                        variables[`cam${cameraId}_active_preset_id`] = presetId.toString();
                        
                        if (cameraId == this.config.defaultCameraId) {
                            variables['current_preset_name'] = presetName;
                            variables['current_preset_id'] = presetId.toString();
                        }
                        
                        this.setVariableValues(variables);
                        
                        // Update internal values if response has parameters
                        if (data && data.parameters) {
                            this.updateParameterValues(cameraId, data.parameters);
                        }
                    } else {
                        this.log('warn', 'Preset did not load correctly');
                    }
                } catch (error) {
                    this.log('error', 'Error cargando preset: ' + error.message);
                }
            }
        };
        
        actions['list_presets'] = {
            name: 'Listar Presets',
            description: 'List all presets saved on SD card',
            options: [],
            callback: async () => {
                const url = 'http://' + this.config.host + '/?listPresets';
                this.log('info', 'Listando presets...');
                
                try {
                    const response = await axios.get(url, { timeout: 5000 });
                    
                    let data;
                    if (typeof response.data === 'object') {
                        data = response.data;
                    } else if (typeof response.data === 'string') {
                        const jsonMatch = response.data.match(/\{.*\}/s);
                        if (jsonMatch) {
                            data = JSON.parse(jsonMatch[0]);
                        }
                    }
                    
                    if (data && data.presets && Array.isArray(data.presets)) {
                        this.log('info', 'Presets encontrados: ' + data.presets.length);
                        
                        data.presets.forEach(p => {
                            if (p.cameraId !== undefined && p.presetId !== undefined) {
                                this.log('info', `Camara ${p.cameraId}, Preset ${p.presetId}: "${p.name || 'No name'}"`);
                            }
                        });
                        
                        this.updatePresetNames(data.presets);
                    } else {
                        this.log('warn', 'No presets found');
                    }
                } catch (error) {
                    this.log('error', 'Error listando presets: ' + error.message);
                }
            }
        };
        
        actions['retry_connection'] = {
            name: 'Retry Connection',
            description: 'Force a new connection attempt with TallyCCU Pro',
            options: [],
            callback: async () => {
                this.log('info', 'Retrying connection manually...');
                this.reconnectAttempts = 0;
                
                const connected = await this.checkConnection();
                
                if (connected) {
                    this.log('info', 'Connection restored successfully');
                } else {
                    this.log('warn', 'Could not establish connection');
                }
            }
        };
        
        actions['change_camera'] = {
            name: 'Change Active Camera',
            description: 'Change the camera that will receive the following commands',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: 1,
                    min: 1,
                    max: 8
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                this.sendParam(cameraId, 'cameraId', cameraId);
            }
        };

        // ====================================================================
        // SAVE PRESET - POST METHOD (OPTIMIZED)
        // Un solo request POST con todos los datos - mucho mas rapido y fiable
        // ====================================================================
        
        actions['save_current_as_preset'] = {
            name: 'Save Current Configuration as Preset',
            description: 'Saves current camera configuration as preset (via POST)',
            options: [
                {
                    type: 'number',
                    label: 'Camera ID',
                    id: 'cameraId',
                    default: this.config.defaultCameraId || 1,
                    min: 1,
                    max: 8
                },
                {
                    type: 'number',
                    label: 'Numero de Preset (0-4)',
                    id: 'presetId',
                    default: 0,
                    min: 0,
                    max: 4
                },
                {
                    type: 'textinput',
                    label: 'Nombre del Preset',
                    id: 'presetName',
                    default: 'Mi Preset'
                },
                {
                    type: 'checkbox',
                    label: 'Incluir: Audio',
                    id: 'includeGroupAudio',
                    default: false
                },                {
                    type: 'checkbox',
                    label: 'Incluir: Color Correction',
                    id: 'includeGroupColorCorrection',
                    default: true
                },                {
                    type: 'checkbox',
                    label: 'Incluir: Display',
                    id: 'includeGroupDisplay',
                    default: false
                },                {
                    type: 'checkbox',
                    label: 'Incluir: Lens',
                    id: 'includeGroupLens',
                    default: true
                },                {
                    type: 'checkbox',
                    label: 'Incluir: Output',
                    id: 'includeGroupOutput',
                    default: false
                },                {
                    type: 'checkbox',
                    label: 'Incluir: PTZ Control',
                    id: 'includeGroupPtzControl',
                    default: false
                },                {
                    type: 'checkbox',
                    label: 'Incluir: Reference',
                    id: 'includeGroupReference',
                    default: false
                },                {
                    type: 'checkbox',
                    label: 'Incluir: Tally',
                    id: 'includeGroupTally',
                    default: false
                },                {
                    type: 'checkbox',
                    label: 'Incluir: Video',
                    id: 'includeGroupVideo',
                    default: true
                }
            ],
            callback: async (event) => {
                const cameraId = event.options.cameraId;
                const presetId = event.options.presetId;
                const presetName = event.options.presetName;
                
                const fullState = this.captureCurrentState(cameraId);
                
                // Determinar que grupos incluir
                const includedGroups = [];
                if (event.options.includeGroupAudio) includedGroups.push('audio');
                if (event.options.includeGroupColorCorrection) includedGroups.push('color_correction');
                if (event.options.includeGroupDisplay) includedGroups.push('display');
                if (event.options.includeGroupLens) includedGroups.push('lens');
                if (event.options.includeGroupOutput) includedGroups.push('output');
                if (event.options.includeGroupPtzControl) includedGroups.push('ptz_control');
                if (event.options.includeGroupReference) includedGroups.push('reference');
                if (event.options.includeGroupTally) includedGroups.push('tally');
                if (event.options.includeGroupVideo) includedGroups.push('video');
                
                this.log('info', `Saving preset ${presetId} for camera ${cameraId} with groups: ${includedGroups.join(', ')}`);
                
                // Filtrar parametros por grupos seleccionados
                const filteredData = {};
                
                for (const [key, value] of Object.entries(fullState)) {
                    if (key === 'cameraId') continue;
                    
                    const paramGroup = this.paramGroupMap[key];
                    
                    if (paramGroup && includedGroups.includes(paramGroup)) {
                        filteredData[key] = value;
                    }
                }
                
                const paramKeys = Object.keys(filteredData);
                const totalParams = paramKeys.length;
                
                if (totalParams === 0) {
                    this.log('warn', 'No hay parametros para guardar con los grupos seleccionados');
                    return;
                }
                
                this.log('info', `Preparando ${totalParams} parametros para enviar...`);
                
                try {
                    // Construir string de datos: key1:val1;key2:val2;...
                    let dataString = '';
                    for (const key of paramKeys) {
                        const value = filteredData[key];
                        const valueStr = Array.isArray(value) ? value.join(',') : String(value);
                        dataString += `${key}:${valueStr};`;
                    }
                    
                    // Construir body del POST
                    const bodyData = `cameraId=${cameraId}&presetId=${presetId}&name=${encodeURIComponent(presetName)}&data=${encodeURIComponent(dataString)}`;
                    
                    this.log('debug', `Body length: ${bodyData.length} bytes`);
                    
                    // Enviar POST unico con todos los datos
                    const url = 'http://' + this.config.host + '/savePreset';
                    
                    const response = await axios.post(url, bodyData, {
                        timeout: 15000,
                        headers: {
                            'Content-Type': 'application/x-www-form-urlencoded'
                        }
                    });
                    
                    // Procesar respuesta
                    let result;
                    if (typeof response.data === 'object') {
                        result = response.data;
                    } else if (typeof response.data === 'string') {
                        const jsonMatch = response.data.match(/\{.*\}/s);
                        if (jsonMatch) {
                            result = JSON.parse(jsonMatch[0]);
                        }
                    }
                    
                    if (result && result.success) {
                        this.log('info', `Preset guardado con exito: ${result.params || totalParams} parametros`);
                        
                        // Actualizar nombres de presets en memoria
                        if (!this.presetNames[cameraId]) this.presetNames[cameraId] = {};
                        this.presetNames[cameraId][presetId] = presetName;
                        
                        // Actualizar variables de Companion
                        const variables = {};
                        variables[`cam${cameraId}_preset${presetId}_name`] = presetName;
                        if (cameraId == this.config.defaultCameraId) {
                            variables[`preset${presetId}_name`] = presetName;
                        }
                        this.setVariableValues(variables);
                        
                    } else {
                        const errorMsg = result?.error || 'Respuesta no valida del servidor';
                        this.log('error', `Error guardando preset: ${errorMsg}`);
                    }
                    
                } catch (error) {
                    this.log('error', `Error guardando preset: ${error.message}`);
                }
            }
        };

        // ====================================================================
        // ACCIONES DE VMIX
        // ====================================================================
        
        actions['set_vmix_connect'] = {
            name: 'vMix Connect - Toggle State',
            description: 'Enable or disable automatic vMix connection',
            options: [
                {
                    type: 'dropdown',
                    label: 'Estado',
                    id: 'enabled',
                    default: 'true',
                    choices: [
                        { id: 'true', label: 'Enable' },
                        { id: 'false', label: 'Disable' }
                    ]
                }
            ],
            callback: async (event) => {
                const enabled = event.options.enabled === 'true' ? 1 : 0;
                const url = `http://${this.config.host}/?vmixConnect=${enabled}`;
                
                try {
                    await axios.get(url, { timeout: 3000 });
                    this.log('info', `Conexion vMix ${enabled ? 'activada' : 'desactivada'}`);
                } catch (err) {
                    this.log('error', `Error cambiando estado vMix Connect: ${err.message}`);
                }
            }
        };

        return actions;
    }
}

// Punto de entrada del modulo
runEntrypoint(TallyCcuProInstance, []);
