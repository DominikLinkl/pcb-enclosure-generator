/*
 * web_ui.h - Modernes Web-Interface für PSU-Steuerung
 * 
 * Features:
 * - Echtzeit-Anzeige von Spannung, Strom, Leistung
 * - Slider zur Spannungseinstellung (38V - 250V)
 * - Automatische PWM-Regelung (nicht-linear, ±3V Toleranz)
 * - WebSocket für Live-Updates
 */

#ifndef WEB_UI_H
#define WEB_UI_H

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

// ===================== WLAN-Konfiguration =====================
// Access Point Modus (eigenes WLAN)
#define WIFI_AP_MODE true
#define WIFI_AP_SSID "PSU-Control"
#define WIFI_AP_PASS "12345678"

// Station Modus (bestehendes WLAN) - nur wenn AP_MODE = false
#define WIFI_STA_SSID "DeinWLAN"
#define WIFI_STA_PASS "DeinPasswort"

// ===================== Regelungs-Parameter =====================
static float g_targetVoltage = 0.0f;      // Zielspannung (0 = aus, oder 38-250V)
static bool  g_regulationActive = false;  // Regelung aktiv?
static const float VOLTAGE_TOLERANCE = 3.0f;  // ±3V Toleranz

// PWM-Grenzen für Spannungsbereich (aus bestehendem Code)
extern volatile float g_psuDutyPctFine;
extern volatile int   g_psuDutyPct;
extern const uint8_t PSU_DUTY_MIN;  // 10%
extern const uint8_t PSU_DUTY_MAX;  // 86%

// Messwerte aus Main
extern volatile float inputVoltage;
extern volatile float inputCurrent;
extern volatile float inputPower;

// PSU Ein/Aus Status
extern bool g_remoteState;

// Web-Kontroll-Modus - immer aktiv (Web + Hardware Encoder funktionieren beide)
bool g_webControlMode = true;

// Forward declarations
void applyOutputAndLog(const char* reason, bool clampToLimits);
void setPsuState(bool state);

// ===================== Web Server & WebSocket =====================
WebServer webServer(80);
WebSocketsServer webSocket(81);

// ===================== HTML/CSS/JS Web-Interface =====================
static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>PSU Control</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 50%, #0f3460 100%);
            min-height: 100vh;
            color: #fff;
            padding: 20px;
        }
        .container {
            max-width: 600px;
            margin: 0 auto;
        }
        h1 {
            text-align: center;
            margin-bottom: 30px;
            font-size: 2em;
            text-shadow: 0 0 20px rgba(0, 255, 200, 0.5);
            color: #00ffc8;
        }
        .card {
            background: rgba(255, 255, 255, 0.05);
            backdrop-filter: blur(10px);
            border-radius: 20px;
            padding: 25px;
            margin-bottom: 20px;
            border: 1px solid rgba(255, 255, 255, 0.1);
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
        }
        .measurement {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 15px 0;
            border-bottom: 1px solid rgba(255, 255, 255, 0.1);
        }
        .measurement:last-child {
            border-bottom: none;
        }
        .measurement-label {
            font-size: 1.1em;
            color: #aaa;
        }
        .measurement-value {
            font-size: 2em;
            font-weight: bold;
            font-family: 'Courier New', monospace;
        }
        .voltage { color: #00ffc8; }
        .current { color: #ff6b6b; }
        .power { color: #ffd93d; }
        .duty { color: #6bcfff; }
        
        .slider-container {
            padding: 20px 0;
        }
        .slider-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 15px;
        }
        .slider-title {
            font-size: 1.2em;
            color: #aaa;
        }
        .target-value {
            font-size: 1.8em;
            font-weight: bold;
            color: #00ffc8;
            font-family: 'Courier New', monospace;
        }
        
        input[type="range"] {
            width: 100%;
            height: 20px;
            -webkit-appearance: none;
            background: linear-gradient(90deg, #0f3460 0%, #00ffc8 100%);
            border-radius: 10px;
            outline: none;
            margin: 10px 0;
        }
        input[type="range"]::-webkit-slider-thumb {
            -webkit-appearance: none;
            width: 35px;
            height: 35px;
            background: #fff;
            border-radius: 50%;
            cursor: pointer;
            box-shadow: 0 0 15px rgba(0, 255, 200, 0.8);
            transition: transform 0.2s;
        }
        input[type="range"]::-webkit-slider-thumb:hover {
            transform: scale(1.1);
        }
        input[type="range"]::-moz-range-thumb {
            width: 35px;
            height: 35px;
            background: #fff;
            border-radius: 50%;
            cursor: pointer;
            border: none;
            box-shadow: 0 0 15px rgba(0, 255, 200, 0.8);
        }
        
        .slider-labels {
            display: flex;
            justify-content: space-between;
            color: #666;
            font-size: 0.9em;
        }
        
        .status {
            display: flex;
            align-items: center;
            gap: 10px;
            margin-top: 15px;
        }
        .status-dot {
            width: 12px;
            height: 12px;
            border-radius: 50%;
            background: #666;
            animation: pulse 2s infinite;
        }
        .status-dot.active {
            background: #00ffc8;
        }
        .status-dot.regulating {
            background: #ffd93d;
            animation: pulse 0.5s infinite;
        }
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        
        .btn {
            width: 100%;
            padding: 15px;
            font-size: 1.2em;
            border: none;
            border-radius: 10px;
            cursor: pointer;
            transition: all 0.3s;
            margin-top: 15px;
        }
        .btn-off {
            background: linear-gradient(135deg, #ff6b6b, #ee5a5a);
            color: white;
        }
        .btn-off:hover {
            transform: translateY(-2px);
            box-shadow: 0 5px 20px rgba(255, 107, 107, 0.4);
        }
        .btn-on {
            background: linear-gradient(135deg, #00ffc8, #00cc9e);
            color: #1a1a2e;
            font-weight: bold;
        }
        .btn-on:hover {
            transform: translateY(-2px);
            box-shadow: 0 5px 20px rgba(0, 255, 200, 0.4);
        }
        .psu-status {
            text-align: center;
            margin: 10px 0;
            font-size: 1.1em;
        }
        .psu-status.on {
            color: #00ffc8;
        }
        .psu-status.off {
            color: #ff6b6b;
        }
        .btn-group {
            display: flex;
            gap: 10px;
            margin-top: 15px;
        }
        .btn-group .btn {
            flex: 1;
            margin-top: 0;
        }
        .btn-mode {
            background: linear-gradient(135deg, #667eea, #764ba2);
            color: white;
        }
        .btn-mode:hover {
            transform: translateY(-2px);
            box-shadow: 0 5px 20px rgba(102, 126, 234, 0.4);
        }
        /* Mode-Styles entfernt */
        
        .voltage-input-container {
            display: flex;
            gap: 10px;
            margin-top: 15px;
            align-items: center;
        }
        .voltage-input {
            flex: 1;
            padding: 12px 15px;
            font-size: 1.2em;
            border: 2px solid #333;
            border-radius: 10px;
            background: #1a1a2e;
            color: #00ffc8;
            text-align: center;
            outline: none;
        }
        .voltage-input:focus {
            border-color: #00ffc8;
            box-shadow: 0 0 10px rgba(0, 255, 200, 0.3);
        }
        .voltage-input::placeholder {
            color: #666;
        }
        .btn-set {
            padding: 12px 20px;
            font-size: 1.1em;
            background: linear-gradient(135deg, #667eea, #764ba2);
            color: white;
            border: none;
            border-radius: 10px;
            cursor: pointer;
            transition: all 0.3s;
        }
        .btn-set:hover {
            transform: translateY(-2px);
            box-shadow: 0 5px 20px rgba(102, 126, 234, 0.4);
        }
        
        .connection-status {
            text-align: center;
            padding: 10px;
            font-size: 0.9em;
            color: #666;
        }
        .connection-status.connected {
            color: #00ffc8;
        }
        .connection-status.disconnected {
            color: #ff6b6b;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>⚡ PSU Control</h1>
        
        <div class="card">
            <div class="measurement">
                <span class="measurement-label">Spannung</span>
                <span class="measurement-value voltage" id="voltage">---.- V</span>
            </div>
            <div class="measurement">
                <span class="measurement-label">Strom</span>
                <span class="measurement-value current" id="current">---.- A</span>
            </div>
            <div class="measurement">
                <span class="measurement-label">Leistung</span>
                <span class="measurement-value power" id="power">---.- W</span>
            </div>
            <div class="measurement">
                <span class="measurement-label">PWM Duty</span>
                <span class="measurement-value duty" id="duty">--.- %</span>
            </div>
        </div>
        
        <div class="card">
            <div class="slider-container">
                <div class="slider-header">
                    <span class="slider-title">Zielspannung</span>
                    <span class="target-value" id="targetDisplay">--- V</span>
                </div>
                <input type="range" id="voltageSlider" min="38" max="250" value="38" step="1">
                <div class="slider-labels">
                    <span>38 V</span>
                    <span>144 V</span>
                    <span>250 V</span>
                </div>
                <div class="voltage-input-container">
                    <input type="number" id="voltageInput" class="voltage-input" 
                           min="38" max="250" step="0.1" placeholder="Spannung eingeben...">
                    <button class="btn-set" onclick="setVoltageFromInput()">Setzen</button>
                </div>
                <div class="status">
                    <div class="status-dot" id="statusDot"></div>
                    <span id="statusText">Regelung inaktiv</span>
                </div>
            </div>
            <button class="btn btn-off" id="btnOff" onclick="setOff()">Regelung AUS</button>
        </div>
        
        <div class="card">
            <h2 style="text-align:center; margin-bottom:15px;">🔌 Netzteil</h2>
            <div class="psu-status" id="psuStatus">Status: ---</div>
            <div class="btn-group">
                <button class="btn btn-on" onclick="setPsuOn()">EIN</button>
                <button class="btn btn-off" onclick="setPsuOff()">AUS</button>
            </div>
        </div>
        
        <div class="card" style="text-align:center;">
            <a href="/graph" style="display:inline-block; padding:15px 30px; background:linear-gradient(135deg, #667eea, #764ba2); color:white; text-decoration:none; border-radius:10px; font-size:1.1em;">📊 Daten-Graph anzeigen</a>
        </div>
        
        <div class="connection-status" id="connStatus">Verbinde...</div>
    </div>

    <script>
        let ws;
        let reconnectInterval;
        
        function connect() {
            const host = window.location.hostname;
            ws = new WebSocket('ws://' + host + ':81/');
            
            ws.onopen = function() {
                document.getElementById('connStatus').textContent = '● Verbunden';
                document.getElementById('connStatus').className = 'connection-status connected';
                clearInterval(reconnectInterval);
            };
            
            ws.onclose = function() {
                document.getElementById('connStatus').textContent = '○ Getrennt - Reconnecting...';
                document.getElementById('connStatus').className = 'connection-status disconnected';
                reconnectInterval = setInterval(connect, 3000);
            };
            
            ws.onerror = function(err) {
                console.error('WebSocket Error:', err);
                ws.close();
            };
            
            ws.onmessage = function(event) {
                try {
                    const data = JSON.parse(event.data);
                    
                    document.getElementById('voltage').textContent = data.v.toFixed(1) + ' V';
                    document.getElementById('current').textContent = data.i.toFixed(2) + ' A';
                    document.getElementById('power').textContent = data.p.toFixed(1) + ' W';
                    document.getElementById('duty').textContent = data.d.toFixed(1) + ' %';
                    
                    // PSU Status aktualisieren
                    if (data.psu !== undefined) {
                        updatePsuStatus(data.psu);
                    }
                    
                    // Control Mode aktualisieren
                    if (data.webMode !== undefined) {
                        updateModeStatus(data.webMode);
                    }
                    
                    // Status aktualisieren
                    const statusDot = document.getElementById('statusDot');
                    const statusText = document.getElementById('statusText');
                    
                    if (data.reg) {
                        const diff = Math.abs(data.v - data.tv);
                        if (diff <= 3) {
                            statusDot.className = 'status-dot active';
                            statusText.textContent = 'Ziel erreicht (' + data.tv.toFixed(0) + ' V)';
                        } else {
                            statusDot.className = 'status-dot regulating';
                            statusText.textContent = 'Regelt... (Ziel: ' + data.tv.toFixed(0) + ' V)';
                        }
                    } else {
                        statusDot.className = 'status-dot';
                        statusText.textContent = 'Regelung inaktiv';
                    }
                } catch (e) {
                    console.error('Parse error:', e);
                }
            };
        }
        
        function sendTarget(voltage) {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({cmd: 'setV', val: voltage}));
            }
        }
        
        function setOff() {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({cmd: 'off'}));
                document.getElementById('voltageSlider').value = 38;
                document.getElementById('targetDisplay').textContent = '--- V';
            }
        }
        
        function setPsuOn() {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({cmd: 'psuOn'}));
            }
        }
        
        function setPsuOff() {
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(JSON.stringify({cmd: 'psuOff'}));
            }
        }
        
        function updatePsuStatus(isOn) {
            const statusEl = document.getElementById('psuStatus');
            if (isOn) {
                statusEl.textContent = 'Status: EIN';
                statusEl.className = 'psu-status on';
            } else {
                statusEl.textContent = 'Status: AUS';
                statusEl.className = 'psu-status off';
            }
        }
        
        // Mode-Funktionen entfernt - Web + Hardware laufen immer parallel
        
        // Slider Event
        const slider = document.getElementById('voltageSlider');
        const targetDisplay = document.getElementById('targetDisplay');
        const voltageInput = document.getElementById('voltageInput');
        
        slider.addEventListener('input', function() {
            targetDisplay.textContent = this.value + ' V';
            voltageInput.value = this.value;
        });
        
        slider.addEventListener('change', function() {
            sendTarget(parseInt(this.value));
        });
        
        function setVoltageFromInput() {
            const value = parseFloat(voltageInput.value);
            if (value >= 38 && value <= 250) {
                slider.value = Math.round(value);
                targetDisplay.textContent = value.toFixed(1) + ' V';
                sendTarget(value);
            } else {
                alert('Bitte einen Wert zwischen 38 und 250 V eingeben!');
            }
        }
        
        // Enter-Taste im Eingabefeld
        voltageInput.addEventListener('keypress', function(e) {
            if (e.key === 'Enter') {
                setVoltageFromInput();
            }
        });
        
        // Initialisierung
        connect();
    </script>
</body>
</html>
)rawliteral";

// ===================== Regelungs-Algorithmus =====================
// Nicht-lineare PWM-zu-Spannung Beziehung berücksichtigen
// Einfacher P-Regler mit adaptivem Gain

void regulateVoltage() {
    // Nur regeln wenn: Regelung aktiv, Web-Modus aktiv, und gültige Zielspannung
    if (!g_regulationActive || !g_webControlMode || g_targetVoltage < 38.0f) {
        return;
    }
    
    float error = g_targetVoltage - inputVoltage;
    
    // Wenn innerhalb Toleranz, nichts tun
    if (fabsf(error) <= VOLTAGE_TOLERANCE) {
        return;
    }
    
    // Adaptiver Regelschritt basierend auf Fehler
    // Größerer Fehler = größerer Schritt, aber begrenzt
    float stepSize;
    float absError = fabsf(error);
    
    if (absError > 50.0f) {
        stepSize = 2.0f;      // Großer Fehler: schnell regeln
    } else if (absError > 20.0f) {
        stepSize = 1.0f;      // Mittlerer Fehler
    } else if (absError > 10.0f) {
        stepSize = 0.5f;      // Kleiner Fehler
    } else {
        stepSize = 0.25f;     // Feinregelung nahe Ziel
    }
    
    // Richtung bestimmen (höherer Duty = höhere Spannung)
    if (error > 0) {
        // Spannung zu niedrig -> PWM erhöhen
        g_psuDutyPctFine += stepSize;
    } else {
        // Spannung zu hoch -> PWM senken
        g_psuDutyPctFine -= stepSize;
    }
    
    // Grenzen einhalten
    if (g_psuDutyPctFine < PSU_DUTY_MIN) g_psuDutyPctFine = PSU_DUTY_MIN;
    if (g_psuDutyPctFine > PSU_DUTY_MAX) g_psuDutyPctFine = PSU_DUTY_MAX;
    
    applyOutputAndLog("WEB-REG", true);  // Mit Clamping!
}

// ===================== WebSocket Event Handler =====================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[WS] Client %u disconnected\n", num);
            break;
            
        case WStype_CONNECTED: {
            IPAddress ip = webSocket.remoteIP(num);
            Serial.printf("[WS] Client %u connected from %s\n", num, ip.toString().c_str());
            break;
        }
        
        case WStype_TEXT: {
            // JSON parsen (einfach ohne Library)
            String msg = String((char*)payload);
            Serial.printf("[WS] Received: %s\n", msg.c_str());
            
            if (msg.indexOf("setV") >= 0) {
                // Zielspannung setzen
                int valIdx = msg.indexOf("\"val\":");
                if (valIdx >= 0) {
                    String valStr = msg.substring(valIdx + 6);
                    int endIdx = valStr.indexOf('}');
                    if (endIdx >= 0) valStr = valStr.substring(0, endIdx);
                    float newTarget = valStr.toFloat();
                    
                    if (newTarget >= 38.0f && newTarget <= 250.0f) {
                        g_targetVoltage = newTarget;
                        g_regulationActive = true;
                        
                        // Duty aus Formel berechnen: U = 2.889983*PWM - 0.860324
                        // Umgestellt: PWM = (U + 0.860324) / 2.889983
                        // Bei invertiertem Signal: höherer Duty = niedrigere Spannung
                        float estimatedDuty = (newTarget + 0.860324f) / 2.889983f;
                        if (estimatedDuty < PSU_DUTY_MIN) estimatedDuty = PSU_DUTY_MIN;
                        if (estimatedDuty > PSU_DUTY_MAX) estimatedDuty = PSU_DUTY_MAX;
                        g_psuDutyPctFine = estimatedDuty;
                        applyOutputAndLog("WEB-INIT", true);
                        
                        Serial.printf("[WEB] Target voltage set to %.1f V, initial duty: %.1f%%\n", g_targetVoltage, g_psuDutyPctFine);
                    }
                }
            }
            else if (msg.indexOf("psuOn") >= 0) {
                // PSU einschalten
                Serial.println("[WS] psuOn command detected!");
                setPsuState(true);
            }
            else if (msg.indexOf("psuOff") >= 0) {
                // PSU ausschalten
                Serial.println("[WS] psuOff command detected!");
                setPsuState(false);
            }
            else if (msg.indexOf("\"off\"") >= 0) {
                // Regelung deaktivieren
                g_regulationActive = false;
                g_targetVoltage = 0.0f;
                Serial.println("[WEB] Regulation OFF");
            }
            // toggleMode entfernt - beide Modi laufen parallel
            break;
        }
        
        default:
            break;
    }
}

// ===================== Web Server Handler =====================
void handleRoot() {
    webServer.send_P(200, "text/html", INDEX_HTML);
}

void handleNotFound() {
    webServer.send(404, "text/plain", "Not found");
}

// ===================== Initialisierung =====================
void webUISetup() {
    Serial.println("\n[WiFi] Starting...");
    
#if WIFI_AP_MODE
    // Access Point Modus
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[WiFi] AP Mode - SSID: %s\n", WIFI_AP_SSID);
    Serial.printf("[WiFi] IP Address: %s\n", ip.toString().c_str());
#else
    // Station Modus
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
    Serial.printf("[WiFi] Connecting to %s", WIFI_STA_SSID);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n[WiFi] Connection failed! Starting AP mode...");
        WiFi.mode(WIFI_AP);
        WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
        Serial.printf("[WiFi] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    }
#endif
    
    // Web Server Setup
    webServer.on("/", handleRoot);
    webServer.onNotFound(handleNotFound);
    webServer.begin();
    Serial.println("[HTTP] Server started on port 80");
    
    // WebSocket Setup
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    Serial.println("[WS] WebSocket server started on port 81");
}

// ===================== Loop Task =====================
static uint32_t lastWsBroadcast = 0;

void webUILoop() {
    webServer.handleClient();
    webSocket.loop();
    
    // Messwerte alle 100ms an alle Clients senden
    uint32_t now = millis();
    if (now - lastWsBroadcast >= 100) {
        lastWsBroadcast = now;
        
        // JSON zusammenbauen (mit PSU Status und Control Mode)
        char json[200];
        snprintf(json, sizeof(json), 
            "{\"v\":%.2f,\"i\":%.3f,\"p\":%.1f,\"d\":%.1f,\"tv\":%.0f,\"reg\":%s,\"psu\":%s,\"webMode\":%s}",
            inputVoltage,
            inputCurrent,
            inputPower,
            g_psuDutyPctFine,
            g_targetVoltage,
            g_regulationActive ? "true" : "false",
            g_remoteState ? "true" : "false",
            g_webControlMode ? "true" : "false"
        );
        
        webSocket.broadcastTXT(json);
    }
    
    // Regelung aufrufen (wird intern begrenzt)
    static uint32_t lastRegulate = 0;
    if (now - lastRegulate >= 50) {  // Alle 50ms regeln
        lastRegulate = now;
        regulateVoltage();
    }
}

#endif // WEB_UI_H
