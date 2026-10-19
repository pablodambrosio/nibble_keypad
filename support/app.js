/**
 * Nibble Keypad - Web Bluetooth Configurator Client
 * Communicates directly with ESP32-C3 Custom GATT Service (0xFFF0).
 */

// ============================================================================
// BLE GATT UUID Definitions
// ============================================================================
const UUID_CFG_SVC       = 0xFFF0;
const UUID_CHR_PWR_MODE  = 0xFFF1;
const UUID_CHR_KEY_MAP   = 0xFFF2;
const UUID_CHR_RESET     = 0xFFF3;

const UUID_BATTERY_SVC   = 0x180F;
const UUID_BATTERY_CHR   = 0x2A19;

// ============================================================================
// HID Usage Table & Modifiers
// ============================================================================
const MODIFIERS = [
    { bit: 0x01, name: 'Ctrl (L)', id: 'mod-lctrl' },
    { bit: 0x02, name: 'Shift (L)', id: 'mod-lshift' },
    { bit: 0x04, name: 'Alt (L)', id: 'mod-lalt' },
    { bit: 0x08, name: 'GUI (L)', id: 'mod-lgui' },
    { bit: 0x10, name: 'Ctrl (R)', id: 'mod-rctrl' },
    { bit: 0x20, name: 'Shift (R)', id: 'mod-rshift' },
    { bit: 0x40, name: 'Alt (R)', id: 'mod-ralt' },
    { bit: 0x80, name: 'GUI (R)', id: 'mod-rgui' }
];

const HID_KEYCODES = [
    { code: 0x00, name: '(None / Modifiers Only)', category: 'Control' },
    
    // Letters
    { code: 0x04, name: 'A', category: 'Letters' },
    { code: 0x05, name: 'B', category: 'Letters' },
    { code: 0x06, name: 'C', category: 'Letters' },
    { code: 0x07, name: 'D', category: 'Letters' },
    { code: 0x08, name: 'E', category: 'Letters' },
    { code: 0x09, name: 'F', category: 'Letters' },
    { code: 0x0A, name: 'G', category: 'Letters' },
    { code: 0x0B, name: 'H', category: 'Letters' },
    { code: 0x0C, name: 'I', category: 'Letters' },
    { code: 0x0D, name: 'J', category: 'Letters' },
    { code: 0x0E, name: 'K', category: 'Letters' },
    { code: 0x0F, name: 'L', category: 'Letters' },
    { code: 0x10, name: 'M', category: 'Letters' },
    { code: 0x11, name: 'N', category: 'Letters' },
    { code: 0x12, name: 'O', category: 'Letters' },
    { code: 0x13, name: 'P', category: 'Letters' },
    { code: 0x14, name: 'Q', category: 'Letters' },
    { code: 0x15, name: 'R', category: 'Letters' },
    { code: 0x16, name: 'S', category: 'Letters' },
    { code: 0x17, name: 'T', category: 'Letters' },
    { code: 0x18, name: 'U', category: 'Letters' },
    { code: 0x19, name: 'V', category: 'Letters' },
    { code: 0x1A, name: 'W', category: 'Letters' },
    { code: 0x1B, name: 'X', category: 'Letters' },
    { code: 0x1C, name: 'Y', category: 'Letters' },
    { code: 0x1D, name: 'Z', category: 'Letters' },

    // Numbers
    { code: 0x1E, name: '1 !', category: 'Numbers' },
    { code: 0x1F, name: '2 @', category: 'Numbers' },
    { code: 0x20, name: '3 #', category: 'Numbers' },
    { code: 0x21, name: '4 $', category: 'Numbers' },
    { code: 0x22, name: '5 %', category: 'Numbers' },
    { code: 0x23, name: '6 ^', category: 'Numbers' },
    { code: 0x24, name: '7 &', category: 'Numbers' },
    { code: 0x25, name: '8 *', category: 'Numbers' },
    { code: 0x26, name: '9 (', category: 'Numbers' },
    { code: 0x27, name: '0 )', category: 'Numbers' },

    // Editing & Controls
    { code: 0x28, name: 'Enter / Return', category: 'Editing' },
    { code: 0x29, name: 'Escape', category: 'Editing' },
    { code: 0x2A, name: 'Backspace', category: 'Editing' },
    { code: 0x2B, name: 'Tab', category: 'Editing' },
    { code: 0x2C, name: 'Spacebar', category: 'Editing' },
    { code: 0x2D, name: '- _ (Minus)', category: 'Punctuation' },
    { code: 0x2E, name: '= + (Equal)', category: 'Punctuation' },
    { code: 0x2F, name: '[ { (Left Bracket)', category: 'Punctuation' },
    { code: 0x30, name: '] } (Right Bracket)', category: 'Punctuation' },
    { code: 0x31, name: '\\ | (Backslash)', category: 'Punctuation' },
    { code: 0x33, name: '; : (Semicolon)', category: 'Punctuation' },
    { code: 0x34, name: '\' " (Quote)', category: 'Punctuation' },
    { code: 0x35, name: '` ~ (Tilde)', category: 'Punctuation' },
    { code: 0x36, name: ', < (Comma)', category: 'Punctuation' },
    { code: 0x37, name: '. > (Period)', category: 'Punctuation' },
    { code: 0x38, name: '/ ? (Slash)', category: 'Punctuation' },

    // Function Keys
    { code: 0x3A, name: 'F1', category: 'Function' },
    { code: 0x3B, name: 'F2', category: 'Function' },
    { code: 0x3C, name: 'F3', category: 'Function' },
    { code: 0x3D, name: 'F4', category: 'Function' },
    { code: 0x3E, name: 'F5', category: 'Function' },
    { code: 0x3F, name: 'F6', category: 'Function' },
    { code: 0x40, name: 'F7', category: 'Function' },
    { code: 0x41, name: 'F8', category: 'Function' },
    { code: 0x42, name: 'F9', category: 'Function' },
    { code: 0x43, name: 'F10', category: 'Function' },
    { code: 0x44, name: 'F11', category: 'Function' },
    { code: 0x45, name: 'F12', category: 'Function' },

    // Navigation & Editing
    { code: 0x46, name: 'Print Screen', category: 'Navigation' },
    { code: 0x47, name: 'Scroll Lock', category: 'Navigation' },
    { code: 0x48, name: 'Pause / Break', category: 'Navigation' },
    { code: 0x49, name: 'Insert', category: 'Navigation' },
    { code: 0x4A, name: 'Home', category: 'Navigation' },
    { code: 0x4B, name: 'Page Up', category: 'Navigation' },
    { code: 0x4C, name: 'Delete', category: 'Navigation' },
    { code: 0x4D, name: 'End', category: 'Navigation' },
    { code: 0x4E, name: 'Page Down', category: 'Navigation' },
    { code: 0x4F, name: 'Right Arrow', category: 'Navigation' },
    { code: 0x50, name: 'Left Arrow', category: 'Navigation' },
    { code: 0x51, name: 'Down Arrow', category: 'Navigation' },
    { code: 0x52, name: 'Up Arrow', category: 'Navigation' },

    // Media & Playback Controls
    { code: 0xF0, name: 'Play / Pause (⏯)', category: 'Media' },
    { code: 0xED, name: 'Next Track (⏭)', category: 'Media' },
    { code: 0xEE, name: 'Previous Track (⏮)', category: 'Media' },
    { code: 0xEF, name: 'Stop (⏹)', category: 'Media' },
    { code: 0x7F, name: 'Mute (🔇)', category: 'Media' },
    { code: 0x80, name: 'Volume Up (🔊)', category: 'Media' },
    { code: 0x81, name: 'Volume Down (🔉)', category: 'Media' },
    { code: 0xEB, name: 'Fast Forward (⏩)', category: 'Media' },
    { code: 0xEC, name: 'Rewind (⏪)', category: 'Media' },
    { code: 0xE8, name: 'Play (▶)', category: 'Media' },
    { code: 0xE9, name: 'Pause (⏸)', category: 'Media' }
];

// Mapping from JS KeyboardEvent.code to USB HID Usage ID
const JS_CODE_TO_HID = {
    'KeyA': 0x04, 'KeyB': 0x05, 'KeyC': 0x06, 'KeyD': 0x07, 'KeyE': 0x08, 'KeyF': 0x09,
    'KeyG': 0x0A, 'KeyH': 0x0B, 'KeyI': 0x0C, 'KeyJ': 0x0D, 'KeyK': 0x0E, 'KeyL': 0x0F,
    'KeyM': 0x10, 'KeyN': 0x11, 'KeyO': 0x12, 'KeyP': 0x13, 'KeyQ': 0x14, 'KeyR': 0x15,
    'KeyS': 0x16, 'KeyT': 0x17, 'KeyU': 0x18, 'KeyV': 0x19, 'KeyW': 0x1A, 'KeyX': 0x1B,
    'KeyY': 0x1C, 'KeyZ': 0x1D,
    'Digit1': 0x1E, 'Digit2': 0x1F, 'Digit3': 0x20, 'Digit4': 0x21, 'Digit5': 0x22,
    'Digit6': 0x23, 'Digit7': 0x24, 'Digit8': 0x25, 'Digit9': 0x26, 'Digit0': 0x27,
    'Enter': 0x28, 'Escape': 0x29, 'Backspace': 0x2A, 'Tab': 0x2B, 'Space': 0x2C,
    'Minus': 0x2D, 'Equal': 0x2E, 'BracketLeft': 0x2F, 'BracketRight': 0x30, 'Backslash': 0x31,
    'Semicolon': 0x33, 'Quote': 0x34, 'Backquote': 0x35, 'Comma': 0x36, 'Period': 0x37, 'Slash': 0x38,
    'F1': 0x3A, 'F2': 0x3B, 'F3': 0x3C, 'F4': 0x3D, 'F5': 0x3E, 'F6': 0x3F,
    'F7': 0x40, 'F8': 0x41, 'F9': 0x42, 'F10': 0x43, 'F11': 0x44, 'F12': 0x45,
    'PrintScreen': 0x46, 'ScrollLock': 0x47, 'Pause': 0x48, 'Insert': 0x49, 'Home': 0x4A,
    'PageUp': 0x4B, 'Delete': 0x4C, 'End': 0x4D, 'PageDown': 0x4E,
    'ArrowRight': 0x4F, 'ArrowLeft': 0x50, 'ArrowDown': 0x51, 'ArrowUp': 0x52,
    'MediaPlayPause': 0xF0, 'MediaTrackNext': 0xED, 'MediaTrackPrevious': 0xEE,
    'MediaStop': 0xEF, 'AudioVolumeMute': 0x7F, 'AudioVolumeUp': 0x80, 'AudioVolumeDown': 0x81
};

// ============================================================================
// Application State
// ============================================================================
const state = {
    device: null,
    server: null,
    characteristics: {
        pwrMode: null,
        keyMap: null,
        reset: null,
        battery: null
    },
    connected: false,
    transport: 'none', // 'bridge' | 'ble' | 'none'
    selectedKeyIndex: 0,
    keys: [
        { modifiers: 0x00, keycode: 0x04 }, // Key 1: 'a'
        { modifiers: 0x00, keycode: 0x05 }, // Key 2: 'b'
        { modifiers: 0x00, keycode: 0x06 }, // Key 3: 'c'
        { modifiers: 0x00, keycode: 0x07 }  // Key 4: 'd'
    ],
    powerMode: 0, // 0 = Normal, 1 = Energy Save
    isCapturing: false
};

// WebSocket Bridge Connection
let wsBridge = null;
let wsConnected = false;

// ============================================================================
// DOM Element References
// ============================================================================
const elements = {
    compatWarning: document.getElementById('compat-warning'),
    btnConnect: document.getElementById('btn-connect'),
    btnDisconnect: document.getElementById('btn-disconnect'),
    statusBadge: document.getElementById('connection-status-badge'),
    statusText: document.getElementById('connection-status-text'),
    batteryIndicator: document.getElementById('battery-indicator'),
    batteryLevelText: document.getElementById('battery-level-text'),
    
    connectBanner: document.getElementById('connect-banner'),
    bannerTitle: document.getElementById('banner-title'),
    bannerSubtitle: document.getElementById('banner-subtitle'),
    bannerConnectBtn: document.getElementById('banner-connect-btn'),
    configWorkspace: document.getElementById('config-workspace'),
    settingsWorkspace: document.getElementById('settings-workspace'),

    keycaps: [
        document.getElementById('keycap-0'),
        document.getElementById('keycap-1'),
        document.getElementById('keycap-2'),
        document.getElementById('keycap-3')
    ],
    keyModsDisplays: [
        document.getElementById('key-mods-0'),
        document.getElementById('key-mods-1'),
        document.getElementById('key-mods-2'),
        document.getElementById('key-mods-3')
    ],
    keyCodeDisplays: [
        document.getElementById('key-code-0'),
        document.getElementById('key-code-1'),
        document.getElementById('key-code-2'),
        document.getElementById('key-code-3')
    ],

    editorTitle: document.getElementById('editor-title'),
    editorSubtitle: document.getElementById('editor-subtitle'),
    selectedKeyBadge: document.getElementById('selected-key-badge'),
    btnCaptureKey: document.getElementById('btn-capture-key'),
    captureBtnLabel: document.getElementById('capture-btn-label'),
    keycodeSelect: document.getElementById('keycode-select'),
    summaryComboText: document.getElementById('summary-combo-text'),
    summaryHexCode: document.getElementById('summary-hex-code'),

    pwrNormal: document.getElementById('pwr-normal'),
    pwrEnergy: document.getElementById('pwr-energy'),

    btnReadAll: document.getElementById('btn-read-all'),
    btnSaveAll: document.getElementById('btn-save-all'),
    btnFactoryReset: document.getElementById('btn-factory-reset'),

    modalReset: document.getElementById('modal-reset'),
    btnCancelReset: document.getElementById('btn-cancel-reset'),
    btnConfirmReset: document.getElementById('btn-confirm-reset'),

    logsContainer: document.getElementById('logs-container'),
    btnClearLogs: document.getElementById('btn-clear-logs'),
    toastContainer: document.getElementById('toast-container')
};

// ============================================================================
// Initialization & Keycode Populator
// ============================================================================
function init() {
    initWebSocketBridge();

    // Check Web Bluetooth API availability
    if (!navigator.bluetooth) {
        log('Web Bluetooth is NOT supported by this browser. (Using WebSocket Host Bridge if active).', 'info');
    }

    populateKeycodeSelect();
    bindEvents();
    renderAllKeycaps();
    loadSelectedKeyIntoEditor();
}

function initWebSocketBridge() {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${protocol}//${window.location.host}/ws`;

    try {
        wsBridge = new WebSocket(wsUrl);

        wsBridge.onopen = () => {
            wsConnected = true;
            log('Connected to local Node.js WebSocket Host Bridge.', 'system');
            updateBridgeUiState(false, 'Host Bridge connected. Press any key on Nibble Keypad to wake.');
        };

        wsBridge.onmessage = (event) => {
            try {
                const msg = JSON.parse(event.data);
                handleWsMessage(msg);
            } catch (err) {
                console.error('Error parsing WS message:', err);
            }
        };

        wsBridge.onclose = () => {
            if (wsConnected) {
                log('Disconnected from Node.js WebSocket Host Bridge.', 'system');
                wsConnected = false;
                if (state.transport === 'bridge') {
                    onDisconnected();
                }
            }
            // Retry connecting after 3 seconds
            setTimeout(initWebSocketBridge, 3000);
        };

        wsBridge.onerror = () => {
            // Server might not be running; fall back silently to Web Bluetooth
        };
    } catch (e) {
        // Fall back to Web Bluetooth
    }
}

function updateBridgeUiState(deviceConnected, msgText) {
    if (deviceConnected) {
        state.connected = true;
        state.transport = 'bridge';
        setConnectionStatus('connected', `${state.deviceName || 'Nibble Keypad'} (Host Shared)`);
        elements.batteryIndicator.classList.remove('hidden');
        elements.batteryLevelText.textContent = `${state.battery || 100}%`;

        if (elements.connectBanner) {
            elements.connectBanner.classList.add('connected');
            elements.bannerTitle.textContent = `${state.deviceName || 'Nibble Keypad'} Connected (Host Shared)`;
            elements.bannerSubtitle.textContent = 'Active Bluetooth link shared with Windows. You can type on the keypad and configure it simultaneously!';
            if (elements.bannerConnectBtn) elements.bannerConnectBtn.classList.add('hidden');
        }
        elements.btnConnect.classList.add('hidden');
        if (elements.btnDisconnect) elements.btnDisconnect.classList.add('hidden');
    } else {
        state.connected = false;
        state.transport = 'bridge';
        setConnectionStatus('disconnected', 'Bridge Active (Keypad Asleep / Offline)');
        elements.batteryIndicator.classList.add('hidden');

        if (elements.connectBanner) {
            elements.connectBanner.classList.remove('connected');
            elements.bannerTitle.textContent = 'Node.js Host Bridge Active';
            elements.bannerSubtitle.textContent = msgText || 'Connected to Node.js server via WebSocket. Press any key on your Nibble Keypad to wake it, or pair it in Windows Bluetooth settings.';
            if (elements.bannerConnectBtn) {
                elements.bannerConnectBtn.classList.remove('hidden');
                elements.bannerConnectBtn.innerHTML = `
                    <svg class="btn-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                        <polyline points="23 4 23 10 17 10"></polyline>
                        <path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"></path>
                    </svg>
                    Check / Wake Keypad
                `;
            }
        }
        elements.btnConnect.classList.remove('hidden');
        elements.btnConnect.innerHTML = `
            <svg class="btn-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                <polyline points="23 4 23 10 17 10"></polyline>
                <path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"></path>
            </svg>
            Sync via Bridge
        `;
        if (elements.btnDisconnect) elements.btnDisconnect.classList.add('hidden');
    }
}

function handleWsMessage(msg) {
    if (msg.type === 'status') {
        state.deviceName = msg.deviceName || 'Nibble Keypad';
        state.battery = msg.battery || 100;
        updateBridgeUiState(msg.connected, msg.message);
        if (msg.connected) {
            log(`Nibble Keypad active via Windows Host Bridge. Simultaneous typing & configuring enabled!`, 'success');
        } else if (msg.message) {
            log(msg.message, 'system');
        }
    } else if (msg.type === 'config') {
        if (msg.power !== undefined && msg.power !== null) {
            state.powerMode = msg.power;
            elements.pwrNormal.checked = (state.powerMode === 0);
            elements.pwrEnergy.checked = (state.powerMode === 1);
        }

        if (msg.keysHex && msg.keysHex.length >= 16) {
            for (let i = 0; i < 4; ++i) {
                const mod = parseInt(msg.keysHex.substr(i * 4, 2), 16);
                const code = parseInt(msg.keysHex.substr(i * 4 + 2, 2), 16);
                state.keys[i].modifiers = mod;
                state.keys[i].keycode = code;
            }
            renderAllKeycaps();
            loadSelectedKeyIntoEditor();
            log(`Loaded key mappings from device: [${state.keys.map((k, idx) => `Key ${idx + 1}: ${getComboLabel(k.modifiers, k.keycode)}`).join(' | ')}]`, 'success');
        }
    } else if (msg.type === 'write_result') {
        if (msg.success) {
            log(`Saved ${msg.characteristic} successfully via Host Bridge.`, 'success');
            showToast(`Saved ${msg.characteristic} to device!`, 'success');
        } else {
            log(`Error saving ${msg.characteristic}: ${msg.message || msg.status}`, 'error');
            showToast(`Save error: ${msg.message || msg.status}`, 'error');
        }
    }
}

function onConnectButtonClicked() {
    if (wsConnected) {
        log('Querying Nibble Keypad status via Windows Host Bridge...', 'system');
        showToast('Querying keypad via bridge... Press any key if sleeping.', 'info');
        wsBridge.send(JSON.stringify({ action: 'get_config' }));
    } else {
        connectBleDevice();
    }
}

function populateKeycodeSelect() {
    elements.keycodeSelect.innerHTML = '';
    const groups = {};

    HID_KEYCODES.forEach(item => {
        if (!groups[item.category]) {
            groups[item.category] = [];
        }
        groups[item.category].push(item);
    });

    for (const [category, items] of Object.entries(groups)) {
        const optgroup = document.createElement('optgroup');
        optgroup.label = category;
        items.forEach(item => {
            const opt = document.createElement('option');
            opt.value = item.code;
            opt.textContent = `${item.name} (0x${item.code.toString(16).padStart(2, '0').toUpperCase()})`;
            optgroup.appendChild(opt);
        });
        elements.keycodeSelect.appendChild(optgroup);
    }
}

// ============================================================================
// Event Handlers
// ============================================================================
function bindEvents() {
    elements.btnConnect.addEventListener('click', onConnectButtonClicked);
    if (elements.bannerConnectBtn) {
        elements.bannerConnectBtn.addEventListener('click', onConnectButtonClicked);
    }
    if (elements.btnDisconnect) {
        elements.btnDisconnect.addEventListener('click', disconnectBleDevice);
    }

    elements.keycaps.forEach((keycap, index) => {
        keycap.addEventListener('click', () => selectKey(index));
    });

    // Modifiers checkbox listener
    MODIFIERS.forEach(mod => {
        const el = document.getElementById(mod.id);
        el.addEventListener('change', onModifierChange);
    });

    // Keycode select listener
    elements.keycodeSelect.addEventListener('change', onKeycodeChange);

    // Key capture listener
    elements.btnCaptureKey.addEventListener('click', toggleCaptureMode);
    window.addEventListener('keydown', handleKeyCaptureEvent);

    // Power profile radios
    elements.pwrNormal.addEventListener('change', () => state.powerMode = 0);
    elements.pwrEnergy.addEventListener('change', () => state.powerMode = 1);

    // Sync buttons
    elements.btnReadAll.addEventListener('click', readAllSettingsFromDevice);
    elements.btnSaveAll.addEventListener('click', saveAllSettingsToDevice);

    // Factory reset modal
    elements.btnFactoryReset.addEventListener('click', () => elements.modalReset.classList.remove('hidden'));
    elements.btnCancelReset.addEventListener('click', () => elements.modalReset.classList.add('hidden'));
    elements.btnConfirmReset.addEventListener('click', executeFactoryReset);

    // Presets
    document.getElementById('preset-dev').addEventListener('click', () => applyPreset('developer'));
    document.getElementById('preset-media').addEventListener('click', () => applyPreset('media'));
    document.getElementById('preset-window').addEventListener('click', () => applyPreset('window'));
    document.getElementById('preset-default').addEventListener('click', () => applyPreset('default'));

    elements.btnClearLogs.addEventListener('click', () => {
        elements.logsContainer.innerHTML = '';
        log('Log cleared.', 'system');
    });
}

// ============================================================================
// Web Bluetooth Connection
// ============================================================================
async function connectBleDevice() {
    if (!navigator.bluetooth) {
        showToast('Web Bluetooth is not supported in this browser.', 'error');
        return;
    }

    try {
        setConnectionStatus('connecting', 'Connecting...');
        log('Scanning for Nibble Keypad macropad...', 'system');

        // Request BLE device advertising Nibble Keypad (or Nimble 4)
        state.device = await navigator.bluetooth.requestDevice({
            filters: [
                { name: 'Nibble Keypad' },
                { name: 'Nimble 4' }
            ],
            optionalServices: [
                UUID_CFG_SVC,
                UUID_BATTERY_SVC,
                '0000fff0-0000-1000-8000-00805f9b34fb',
                '0000180f-0000-1000-8000-00805f9b34fb'
            ]
        });

        state.device.addEventListener('gattserverdisconnected', onDisconnected);
        log(`Device selected: ${state.device.name}. Establishing GATT connection...`, 'system');

        state.server = await state.device.gatt.connect();
        log('Connected to GATT Server. Discovering configuration service (0xFFF0)...', 'system');

        // Access Custom Configuration Service
        let cfgService;
        try {
            cfgService = await state.server.getPrimaryService(UUID_CFG_SVC);
        } catch {
            cfgService = await state.server.getPrimaryService('0000fff0-0000-1000-8000-00805f9b34fb');
        }

        // Discover characteristics
        state.characteristics.pwrMode = await cfgService.getCharacteristic(UUID_CHR_PWR_MODE);
        state.characteristics.keyMap = await cfgService.getCharacteristic(UUID_CHR_KEY_MAP);
        state.characteristics.reset = await cfgService.getCharacteristic(UUID_CHR_RESET);
        log('Configuration characteristics (0xFFF1, 0xFFF2, 0xFFF3) discovered.', 'success');

        // Attempt to read Battery Service
        try {
            const batService = await state.server.getPrimaryService(UUID_BATTERY_SVC);
            state.characteristics.battery = await batService.getCharacteristic(UUID_BATTERY_CHR);
            const batVal = await state.characteristics.battery.readValue();
            const level = batVal.getUint8(0);
            elements.batteryLevelText.textContent = `${level}%`;
            elements.batteryIndicator.classList.remove('hidden');
            log(`Battery service reporting: ${level}%`, 'success');
        } catch (e) {
            log('Battery service not available on this connection.', 'system');
        }

        state.connected = true;
        setConnectionStatus('connected', `Connected: ${state.device.name || 'Nibble Keypad'}`);
        showToast(`Successfully connected to ${state.device.name || 'Nibble Keypad'}!`, 'success');

        // Read current configurations from device
        await readAllSettingsFromDevice();

        // Update banner to connected state
        if (elements.connectBanner) {
            elements.connectBanner.classList.add('connected');
            elements.bannerTitle.textContent = `${state.device.name || 'Nibble Keypad'} Connected (Hardware Synced)`;
            elements.bannerSubtitle.textContent = 'Active GATT link established on Service 0xFFF0. Configuration changes will be saved to NVS.';
            if (elements.bannerConnectBtn) elements.bannerConnectBtn.classList.add('hidden');
        }
        elements.btnConnect.classList.add('hidden');
        elements.btnDisconnect.classList.remove('hidden');

    } catch (error) {
        log(`Connection failed or cancelled: ${error.message}`, 'error');
        setConnectionStatus('disconnected', 'Disconnected');
        showToast(error.message, 'error');
    }
}

function disconnectBleDevice() {
    if (state.transport === 'bridge') {
        log('Nibble Keypad is connected via Windows Host Bridge. To completely unpair, use Windows Bluetooth settings.', 'system');
        showToast('Keypad remains connected to Windows for typing.', 'info');
        return;
    }

    if (state.device && state.device.gatt.connected) {
        log('Disconnecting from keyboard...', 'system');
        state.device.gatt.disconnect();
    }
}

function onDisconnected() {
    state.connected = false;
    state.device = null;
    state.server = null;
    state.characteristics = { pwrMode: null, keyMap: null, reset: null, battery: null };

    setConnectionStatus('disconnected', 'Disconnected');
    elements.batteryIndicator.classList.add('hidden');

    if (elements.connectBanner) {
        elements.connectBanner.classList.remove('connected');
        elements.bannerTitle.textContent = 'Interactive Configuration & Preview Mode';
        elements.bannerSubtitle.textContent = 'Configure your 4 keys and power profile below. Click Connect Nibble Keypad to sync live with hardware via Web Bluetooth.';
        if (elements.bannerConnectBtn) elements.bannerConnectBtn.classList.remove('hidden');
    }
    elements.btnConnect.classList.remove('hidden');
    if (elements.btnDisconnect) elements.btnDisconnect.classList.add('hidden');

    log('BLE link disconnected.', 'system');
    showToast('Nibble Keypad disconnected.', 'system');
}

function setConnectionStatus(type, text) {
    elements.statusBadge.className = `status-badge status-${type}`;
    elements.statusText.textContent = text;
}

// ============================================================================
// GATT Read & Write Logic
// ============================================================================
async function readAllSettingsFromDevice() {
    if (wsConnected && state.transport === 'bridge') {
        log('Requesting fresh configuration from device via Host Bridge...', 'ble-rx');
        wsBridge.send(JSON.stringify({ action: 'get_config' }));
        return;
    }

    if (!state.connected || !state.characteristics.keyMap) {
        showToast('Not connected to device.', 'error');
        return;
    }

    try {
        log('Reading current key mappings (0xFFF2)...', 'ble-rx');
        const keyData = await state.characteristics.keyMap.readValue();
        
        if (keyData.byteLength === 8) {
            for (let i = 0; i < 4; ++i) {
                state.keys[i].modifiers = keyData.getUint8(i * 2);
                state.keys[i].keycode = keyData.getUint8(i * 2 + 1);
            }
            log(`Key mappings read successfully: [${Array.from(new Uint8Array(keyData.buffer)).map(b => '0x' + b.toString(16).padStart(2, '0')).join(', ')}]`, 'success');
        }

        log('Reading active power profile (0xFFF1)...', 'ble-rx');
        const pwrData = await state.characteristics.pwrMode.readValue();
        if (pwrData.byteLength >= 1) {
            state.powerMode = pwrData.getUint8(0);
            elements.pwrNormal.checked = (state.powerMode === 0);
            elements.pwrEnergy.checked = (state.powerMode === 1);
            log(`Active power mode read: ${state.powerMode === 1 ? 'Energy Save (10s/5m)' : 'Normal (30s/10m)'}`, 'success');
        }

        renderAllKeycaps();
        loadSelectedKeyIntoEditor();
        showToast('Keyboard settings refreshed from device.', 'success');

    } catch (error) {
        log(`Failed reading settings: ${error.message}`, 'error');
        showToast(`Read error: ${error.message}`, 'error');
    }
}

async function saveAllSettingsToDevice() {
    if (wsConnected && state.transport === 'bridge') {
        let keysHex = '';
        for (let i = 0; i < 4; ++i) {
            keysHex += state.keys[i].modifiers.toString(16).padStart(2, '0');
            keysHex += state.keys[i].keycode.toString(16).padStart(2, '0');
        }
        log(`Saving key mappings via Host Bridge: [${keysHex}]...`, 'ble-tx');
        wsBridge.send(JSON.stringify({ action: 'set_keys', keysHex }));
        log(`Saving power profile (${state.powerMode}) via Host Bridge...`, 'ble-tx');
        wsBridge.send(JSON.stringify({ action: 'set_power', value: state.powerMode }));
        showToast('All settings sent to Nibble Keypad!', 'success');
        return;
    }

    if (!state.connected || !state.characteristics.keyMap) {
        showToast('Not connected to device.', 'error');
        return;
    }

    try {
        // 1. Pack 8-byte key buffer
        const keyBuffer = new Uint8Array(8);
        for (let i = 0; i < 4; ++i) {
            keyBuffer[i * 2] = state.keys[i].modifiers;
            keyBuffer[i * 2 + 1] = state.keys[i].keycode;
        }

        log(`Writing 8-byte key mapping table to 0xFFF2: [${Array.from(keyBuffer).map(b => '0x' + b.toString(16).padStart(2, '0')).join(', ')}]...`, 'ble-tx');
        await state.characteristics.keyMap.writeValue(keyBuffer);
        log('Key mappings saved and committed to NVS.', 'success');

        // 2. Pack 1-byte power profile buffer
        const pwrBuffer = new Uint8Array([state.powerMode]);
        log(`Writing power profile (${state.powerMode}) to 0xFFF1...`, 'ble-tx');
        await state.characteristics.pwrMode.writeValue(pwrBuffer);
        log('Power profile saved and committed to NVS.', 'success');

        showToast('All settings successfully saved to Nibble Keypad!', 'success');

    } catch (error) {
        log(`Failed saving settings: ${error.message}`, 'error');
        showToast(`Save error: ${error.message}`, 'error');
    }
}

async function executeFactoryReset() {
    elements.modalReset.classList.add('hidden');

    if (wsConnected && state.transport === 'bridge') {
        log('Sending factory reset command via Host Bridge...', 'ble-tx');
        wsBridge.send(JSON.stringify({ action: 'factory_reset' }));
        showToast('Factory reset triggered! Device is resetting...', 'success');
        applyPreset('default');
        state.powerMode = 0;
        elements.pwrNormal.checked = true;
        return;
    }

    if (!state.connected || !state.characteristics.reset) {
        showToast('Not connected to device.', 'error');
        return;
    }

    try {
        log('Initiating Factory Reset via GATT 0xFFF3 write...', 'ble-tx');
        const resetCmd = new Uint8Array([0x01]);
        await state.characteristics.reset.writeValue(resetCmd);

        log('Factory reset accepted. The keyboard is erasing NVS, forgetting bonded devices, and rebooting.', 'success');
        showToast('Factory reset triggered! Device is resetting and rebooting...', 'success');

        // Reset local in-memory state
        applyPreset('default');
        state.powerMode = 0;
        elements.pwrNormal.checked = true;

    } catch (error) {
        log(`Factory reset command failed: ${error.message}`, 'error');
        showToast(`Reset error: ${error.message}`, 'error');
    }
}

// ============================================================================
// Keycap Selection & Editor View
// ============================================================================
function selectKey(index) {
    if (state.isCapturing) stopCaptureMode();
    state.selectedKeyIndex = index;

    elements.keycaps.forEach((cap, i) => {
        cap.classList.toggle('active', i === index);
    });

    loadSelectedKeyIntoEditor();
}

function loadSelectedKeyIntoEditor() {
    const key = state.keys[state.selectedKeyIndex];
    elements.editorTitle.textContent = `Configure Key ${state.selectedKeyIndex + 1}`;
    elements.selectedKeyBadge.textContent = `KEY ${state.selectedKeyIndex + 1}`;

    // Set checkboxes
    MODIFIERS.forEach(mod => {
        const checkbox = document.getElementById(mod.id);
        checkbox.checked = (key.modifiers & mod.bit) !== 0;
    });

    // Set keycode dropdown
    elements.keycodeSelect.value = key.keycode;

    updateComboSummary();
}

function onModifierChange() {
    let modMask = 0;
    MODIFIERS.forEach(mod => {
        const checkbox = document.getElementById(mod.id);
        if (checkbox.checked) {
            modMask |= mod.bit;
        }
    });

    state.keys[state.selectedKeyIndex].modifiers = modMask;
    renderKeycap(state.selectedKeyIndex);
    updateComboSummary();
}

function onKeycodeChange() {
    const code = parseInt(elements.keycodeSelect.value, 10);
    state.keys[state.selectedKeyIndex].keycode = code;
    renderKeycap(state.selectedKeyIndex);
    updateComboSummary();
}

function renderKeycap(index) {
    const key = state.keys[index];
    const modsStr = formatModifiers(key.modifiers);
    const keyName = getKeyName(key.keycode);

    elements.keyModsDisplays[index].textContent = modsStr || 'None';
    elements.keyCodeDisplays[index].textContent = keyName;
}

function renderAllKeycaps() {
    for (let i = 0; i < 4; ++i) {
        renderKeycap(i);
    }
}

function updateComboSummary() {
    const key = state.keys[state.selectedKeyIndex];
    const modsStr = formatModifiers(key.modifiers);
    const keyName = getKeyName(key.keycode);

    const combo = modsStr ? `${modsStr} + ${keyName}` : keyName;
    elements.summaryComboText.textContent = combo;
    elements.summaryHexCode.textContent = `[Mod: 0x${key.modifiers.toString(16).padStart(2, '0').toUpperCase()}, Code: 0x${key.keycode.toString(16).padStart(2, '0').toUpperCase()}]`;
}

// ============================================================================
// Live Key Capture Engine
// ============================================================================
function toggleCaptureMode() {
    if (state.isCapturing) {
        stopCaptureMode();
    } else {
        startCaptureMode();
    }
}

function startCaptureMode() {
    state.isCapturing = true;
    elements.btnCaptureKey.classList.add('recording');
    elements.captureBtnLabel.textContent = 'Listening... Press key';
    log('Capture mode active. Press any key on your keyboard...', 'system');
}

function stopCaptureMode() {
    state.isCapturing = false;
    elements.btnCaptureKey.classList.remove('recording');
    elements.captureBtnLabel.textContent = 'Capture Key';
}

function handleKeyCaptureEvent(e) {
    if (!state.isCapturing) return;

    // Prevent default browser shortcuts (e.g. Ctrl+S, Ctrl+P)
    e.preventDefault();
    e.stopPropagation();

    // Map modifiers
    let modMask = 0;
    if (e.ctrlKey) modMask |= 0x01; // Left Ctrl
    if (e.shiftKey) modMask |= 0x02; // Left Shift
    if (e.altKey) modMask |= 0x04; // Left Alt
    if (e.metaKey) modMask |= 0x08; // Left GUI

    // Identify primary keycode
    let hidCode = JS_CODE_TO_HID[e.code] || 0x00;

    // Update selected key state
    state.keys[state.selectedKeyIndex].modifiers = modMask;
    if (hidCode !== 0x00) {
        state.keys[state.selectedKeyIndex].keycode = hidCode;
    }

    renderKeycap(state.selectedKeyIndex);
    loadSelectedKeyIntoEditor();

    log(`Captured: ${elements.summaryComboText.textContent} ${elements.summaryHexCode.textContent}`, 'success');
    showToast(`Captured combo for Key ${state.selectedKeyIndex + 1}!`, 'success');
    stopCaptureMode();
}

// ============================================================================
// Presets
// ============================================================================
function applyPreset(name) {
    switch (name) {
        case 'developer':
            // Key 1: Copy (Ctrl+C), Key 2: Paste (Ctrl+V), Key 3: Undo (Ctrl+Z), Key 4: Save (Ctrl+S)
            state.keys = [
                { modifiers: 0x01, keycode: 0x06 }, // Ctrl + C
                { modifiers: 0x01, keycode: 0x19 }, // Ctrl + V
                { modifiers: 0x01, keycode: 0x1D }, // Ctrl + Z
                { modifiers: 0x01, keycode: 0x16 }  // Ctrl + S
            ];
            log('Applied Developer Preset: [Copy, Paste, Undo, Save]', 'system');
            break;

        case 'media':
            // Key 1 (Top-Left): Previous Track, Key 2 (Top-Right): Next Track
            // Key 3 (Bottom-Left): Play/Pause, Key 4 (Bottom-Right): Mute
            state.keys = [
                { modifiers: 0x00, keycode: 0xEE }, // Previous Track
                { modifiers: 0x00, keycode: 0xED }, // Next Track
                { modifiers: 0x00, keycode: 0xF0 }, // Play / Pause
                { modifiers: 0x00, keycode: 0x7F }  // Mute
            ];
            log('Applied Media Controller Preset: [Prev, Next, Play/Pause, Mute]', 'system');
            break;

        case 'window':
            // Key 1: Win+Left, Key 2: Win+Right, Key 3: Win+Up, Key 4: Win+Down
            state.keys = [
                { modifiers: 0x08, keycode: 0x50 }, // GUI + Left
                { modifiers: 0x08, keycode: 0x4F }, // GUI + Right
                { modifiers: 0x08, keycode: 0x52 }, // GUI + Up
                { modifiers: 0x08, keycode: 0x51 }  // GUI + Down
            ];
            log('Applied Window Snapping Preset: [Win+Left, Win+Right, Win+Up, Win+Down]', 'system');
            break;

        case 'default':
        default:
            // Standard A, B, C, D
            state.keys = [
                { modifiers: 0x00, keycode: 0x04 }, // A
                { modifiers: 0x00, keycode: 0x05 }, // B
                { modifiers: 0x00, keycode: 0x06 }, // C
                { modifiers: 0x00, keycode: 0x07 }  // D
            ];
            log('Applied Factory Default Preset: [A, B, C, D]', 'system');
            break;
    }

    renderAllKeycaps();
    loadSelectedKeyIntoEditor();
    showToast(`Applied ${name} macro preset! Click 'Save & Apply' to commit.`, 'success');
}

// ============================================================================
// Formatting Helpers
// ============================================================================
function formatModifiers(mask) {
    if (!mask) return '';
    const parts = [];
    if (mask & 0x01) parts.push('Ctrl');
    if (mask & 0x02) parts.push('Shift');
    if (mask & 0x04) parts.push('Alt');
    if (mask & 0x08) parts.push('Win');
    if (mask & 0x10) parts.push('RCtrl');
    if (mask & 0x20) parts.push('RShift');
    if (mask & 0x40) parts.push('AltGr');
    if (mask & 0x80) parts.push('RGUI');
    return parts.join(' + ');
}

function getKeyName(code) {
    const item = HID_KEYCODES.find(k => k.code === code);
    return item ? item.name : `0x${code.toString(16).padStart(2, '0').toUpperCase()}`;
}

// ============================================================================
// Logger & Toast Notifications
// ============================================================================
function log(msg, type = 'system') {
    const entry = document.createElement('div');
    const time = new Date().toLocaleTimeString();
    entry.className = `log-entry log-${type}`;
    entry.textContent = `[${time}] ${msg}`;
    elements.logsContainer.appendChild(entry);
    elements.logsContainer.scrollTop = elements.logsContainer.scrollHeight;
}

function showToast(message, type = 'info') {
    const toast = document.createElement('div');
    toast.className = `toast toast-${type}`;
    toast.textContent = message;

    elements.toastContainer.appendChild(toast);

    setTimeout(() => {
        toast.style.opacity = '0';
        toast.style.transform = 'translateY(10px)';
        toast.style.transition = 'all 0.3s ease';
        setTimeout(() => toast.remove(), 300);
    }, 3500);
}

// Start application
window.addEventListener('DOMContentLoaded', init);
