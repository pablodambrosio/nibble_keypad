const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');
const { WebSocketServer } = require('ws');

const PORT = process.env.PORT || 8080;
const STATIC_DIR = __dirname;
const BRIDGE_EXE = path.join(__dirname, 'bin', 'NibbleBridge.exe');

// MIME types for static files
const MIME_TYPES = {
    '.html': 'text/html; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
    '.svg': 'image/svg+xml',
    '.ico': 'image/x-icon'
};

// Create HTTP server for static files
const server = http.createServer((req, res) => {
    let reqPath = req.url.split('?')[0];
    if (reqPath === '/' || reqPath === '') reqPath = '/index.html';

    const safePath = path.normalize(reqPath).replace(/^(\.\.[\/\\])+/, '');
    const filePath = path.join(STATIC_DIR, safePath);

    fs.stat(filePath, (err, stats) => {
        if (err || !stats.isFile()) {
            res.writeHead(404, { 'Content-Type': 'text/plain' });
            res.end('404 Not Found');
            return;
        }

        const ext = path.extname(filePath).toLowerCase();
        const contentType = MIME_TYPES[ext] || 'application/octet-stream';
        res.writeHead(200, { 'Content-Type': contentType });
        fs.createReadStream(filePath).pipe(res);
    });
});

// Setup WebSocket server
const wss = new WebSocketServer({ server, path: '/ws' });

let latestState = {
    connected: false,
    deviceName: 'Nibble Keypad',
    battery: 100,
    power: null,
    keysHex: null
};

function broadcast(obj) {
    const data = JSON.stringify(obj);
    for (const client of wss.clients) {
        if (client.readyState === 1) { // OPEN
            client.send(data);
        }
    }
}

// Spawn WinRT BLE bridge process
console.log(`[Bridge] Starting NibbleBridge.exe from: ${BRIDGE_EXE}`);
const bridge = spawn(BRIDGE_EXE, [], {
    cwd: path.dirname(BRIDGE_EXE),
    stdio: ['pipe', 'pipe', 'pipe']
});

bridge.stdout.setEncoding('utf8');
let stdoutBuffer = '';

bridge.stdout.on('data', (chunk) => {
    stdoutBuffer += chunk;
    const lines = stdoutBuffer.split(/\r?\n/);
    stdoutBuffer = lines.pop(); // Keep partial line

    for (const line of lines) {
        if (!line.trim()) continue;
        try {
            const msg = JSON.parse(line);
            handleBridgeMessage(msg);
        } catch (e) {
            console.log(`[Bridge RAW] ${line}`);
        }
    }
});

bridge.stderr.on('data', (err) => {
    console.error(`[Bridge ERR] ${err.toString().trim()}`);
});

bridge.on('close', (code) => {
    console.log(`[Bridge] Exited with code ${code}`);
    latestState.connected = false;
    broadcast({ type: 'status', ...latestState, transport: 'ws-bridge' });
});

function handleBridgeMessage(msg) {
    console.log(`[Bridge EVENT]`, msg);

    if (msg.event === 'device_ready') {
        latestState.connected = msg.connected;
        latestState.deviceName = msg.name || 'Nibble Keypad';
        broadcast({
            type: 'status',
            connected: latestState.connected,
            deviceName: latestState.deviceName,
            battery: latestState.battery,
            transport: 'ws-bridge'
        });
    } else if (msg.event === 'connection_changed') {
        latestState.connected = msg.connected;
        broadcast({
            type: 'status',
            connected: latestState.connected,
            deviceName: latestState.deviceName,
            battery: latestState.battery,
            transport: 'ws-bridge'
        });
    } else if (msg.event === 'device_status') {
        latestState.connected = msg.connected;
        broadcast({
            type: 'status',
            connected: latestState.connected,
            deviceName: latestState.deviceName,
            battery: latestState.battery,
            message: msg.message,
            transport: 'ws-bridge'
        });
    } else if (msg.event === 'config') {
        latestState.power = msg.power;
        latestState.keysHex = msg.keysHex;
        broadcast({
            type: 'config',
            power: msg.power,
            keysHex: msg.keysHex
        });
    } else if (msg.event === 'write_result') {
        broadcast({
            type: 'write_result',
            characteristic: msg.characteristic,
            success: msg.success,
            status: msg.status,
            message: msg.message
        });
    } else if (msg.event === 'error') {
        broadcast({
            type: 'error',
            message: msg.message
        });
    }
}

function sendBridgeCommand(cmdObj) {
    if (bridge && bridge.stdin.writable) {
        bridge.stdin.write(JSON.stringify(cmdObj) + '\n');
    }
}

// Client WebSocket connection
wss.on('connection', (ws) => {
    console.log('[WS] Client connected');

    // Send initial status and cached config immediately
    ws.send(JSON.stringify({
        type: 'status',
        connected: latestState.connected,
        deviceName: latestState.deviceName,
        battery: latestState.battery,
        transport: 'ws-bridge'
    }));

    if (latestState.keysHex && latestState.power !== null) {
        ws.send(JSON.stringify({
            type: 'config',
            power: latestState.power,
            keysHex: latestState.keysHex
        }));
    } else {
        // Trigger fresh read
        sendBridgeCommand({ cmd: 'read' });
    }

    ws.on('message', (data) => {
        try {
            const req = JSON.parse(data.toString());
            console.log('[WS REQ]', req);

            if (req.action === 'get_config') {
                sendBridgeCommand({ cmd: 'read' });
            } else if (req.action === 'set_power') {
                sendBridgeCommand({ cmd: 'write_power', value: req.value });
            } else if (req.action === 'set_keys') {
                sendBridgeCommand({ cmd: 'write_keys', keysHex: req.keysHex });
            } else if (req.action === 'factory_reset') {
                sendBridgeCommand({ cmd: 'reset' });
            }
        } catch (e) {
            console.error('[WS] Bad message from client:', e.message);
        }
    });

    ws.on('close', () => {
        console.log('[WS] Client disconnected');
    });
});

server.listen(PORT, () => {
    console.log(`=======================================================`);
    console.log(` Nibble Keypad Configurator Server Running!            `);
    console.log(` Web UI:   http://localhost:${PORT}/                    `);
    console.log(` Bridge:   WinRT GATT <-> WebSocket active             `);
    console.log(`=======================================================`);
});

// Clean shutdown
process.on('SIGINT', () => {
    console.log('\nStopping server and bridge...');
    if (bridge) bridge.kill();
    process.exit(0);
});
process.on('SIGTERM', () => {
    if (bridge) bridge.kill();
    process.exit(0);
});
