/*
 * data_logger.h - Persistenter Daten-Logger für PSU
 * Speichert Spannung, Leistung und Zeit dauerhaft im Flash (LittleFS)
 * WICHTIG: Muss NACH web_ui.h eingebunden werden!
 */

#ifndef DATA_LOGGER_H
#define DATA_LOGGER_H

#include <LittleFS.h>
#include <vector>

// ===================== Konfiguration =====================
#define LOG_FILE_PATH "/psu_log.csv"
#define LOG_FILE_MAX_SIZE (800 * 1024)  // 800KB max, dann ältere Hälfte löschen
#define SAMPLE_INTERVAL_MS 1000         // Abtastintervall 1 Sekunde
#define MAX_DISPLAY_POINTS 500          // Max Punkte für Graph-Anzeige

// ===================== Globale Variablen =====================
static uint32_t g_lastSampleTime = 0;
static bool g_loggingActive = true;     // Immer aktiv (standardmäßig)
static bool g_loggingPaused = false;    // Pausiert?
static uint32_t g_logStartTime = 0;

// Externe Variablen aus main
extern volatile float inputVoltage;
extern volatile float inputPower;

// Externe WebSocket für Broadcasts
extern WebSocketsServer webSocket;
extern WebServer webServer;

// ===================== LittleFS Funktionen =====================

bool dataLoggerInitFS() {
    if (!LittleFS.begin(true)) {  // true = formatOnFail
        Serial.println("[LOGGER] LittleFS Mount FAILED!");
        return false;
    }
    Serial.println("[LOGGER] LittleFS mounted successfully");
    
    // Log-Datei Info anzeigen
    if (LittleFS.exists(LOG_FILE_PATH)) {
        File f = LittleFS.open(LOG_FILE_PATH, "r");
        if (f) {
            Serial.printf("[LOGGER] Log file size: %d bytes\n", f.size());
            f.close();
        }
    } else {
        // Neue Datei mit Header erstellen
        File f = LittleFS.open(LOG_FILE_PATH, "w");
        if (f) {
            f.println("timestamp,voltage,power");
            f.close();
            Serial.println("[LOGGER] New log file created");
        }
    }
    return true;
}

void dataLoggerRotateIfNeeded() {
    if (!LittleFS.exists(LOG_FILE_PATH)) return;
    
    File f = LittleFS.open(LOG_FILE_PATH, "r");
    if (!f) return;
    
    size_t fileSize = f.size();
    f.close();
    
    if (fileSize < LOG_FILE_MAX_SIZE) return;
    
    Serial.printf("[LOGGER] Rotating log file (size: %d)\n", fileSize);
    
    // Letzte Hälfte behalten
    File src = LittleFS.open(LOG_FILE_PATH, "r");
    if (!src) return;
    
    // Zur Mitte springen
    src.seek(fileSize / 2);
    // Bis zum nächsten Zeilenende lesen
    src.readStringUntil('\n');
    
    // Rest in temporäre Datei kopieren
    File dst = LittleFS.open("/psu_log_new.csv", "w");
    if (!dst) {
        src.close();
        return;
    }
    
    dst.println("timestamp,voltage,power");
    while (src.available()) {
        String line = src.readStringUntil('\n');
        if (line.length() > 0) {
            dst.println(line);
        }
    }
    
    src.close();
    dst.close();
    
    // Alte Datei löschen und neue umbenennen
    LittleFS.remove(LOG_FILE_PATH);
    LittleFS.rename("/psu_log_new.csv", LOG_FILE_PATH);
    
    Serial.println("[LOGGER] Log file rotated");
}

// ===================== Logger Funktionen =====================

void dataLoggerInit() {
    g_lastSampleTime = 0;
    g_loggingActive = true;
    g_loggingPaused = false;
    g_logStartTime = millis();
    
    if (!dataLoggerInitFS()) {
        Serial.println("[LOGGER] WARNING: Filesystem init failed!");
    }
    
    Serial.println("[LOGGER] Continuous logging active");
}

void dataLoggerClear() {
    if (LittleFS.exists(LOG_FILE_PATH)) {
        LittleFS.remove(LOG_FILE_PATH);
    }
    File f = LittleFS.open(LOG_FILE_PATH, "w");
    if (f) {
        f.println("timestamp,voltage,power");
        f.close();
    }
    g_logStartTime = millis();
    Serial.println("[LOGGER] All data cleared");
}

void dataLoggerSetPaused(bool paused) {
    g_loggingPaused = paused;
    Serial.printf("[LOGGER] Logging %s\n", paused ? "PAUSED" : "RESUMED");
}

void dataLoggerAddSample() {
    if (!g_loggingActive || g_loggingPaused) return;
    
    uint32_t now = millis();
    if (now - g_lastSampleTime < SAMPLE_INTERVAL_MS) return;
    
    g_lastSampleTime = now;
    
    // Rotation prüfen (nur alle 100 Samples)
    static int rotateCounter = 0;
    if (++rotateCounter >= 100) {
        rotateCounter = 0;
        dataLoggerRotateIfNeeded();
    }
    
    // Datenpunkt in Datei schreiben
    File f = LittleFS.open(LOG_FILE_PATH, "a");
    if (f) {
        f.printf("%lu,%.2f,%.2f\n", now, inputVoltage, inputPower);
        f.close();
    }
}

// Datenpunkte für einen Zeitbereich laden (in ms von jetzt zurück, 0 = alle)
String dataLoggerGetJSON(uint32_t timeRangeMs) {
    String json = "{\"points\":[";
    
    if (!LittleFS.exists(LOG_FILE_PATH)) {
        json += "],\"count\":0,\"paused\":" + String(g_loggingPaused ? "true" : "false") + "}";
        return json;
    }
    
    File f = LittleFS.open(LOG_FILE_PATH, "r");
    if (!f) {
        json += "],\"count\":0,\"paused\":" + String(g_loggingPaused ? "true" : "false") + "}";
        return json;
    }
    
    // Header überspringen
    f.readStringUntil('\n');
    
    uint32_t now = millis();
    uint32_t minTime = (timeRangeMs > 0) ? (now - timeRangeMs) : 0;
    
    // Alle Zeilen lesen und filtern
    std::vector<String> lines;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        if (line.length() < 5) continue;
        
        // Timestamp extrahieren
        int comma = line.indexOf(',');
        if (comma < 0) continue;
        uint32_t ts = line.substring(0, comma).toInt();
        
        if (ts >= minTime) {
            lines.push_back(line);
        }
    }
    f.close();
    
    // Ausdünnen wenn zu viele Punkte
    int count = lines.size();
    int step = 1;
    if (count > MAX_DISPLAY_POINTS) {
        step = count / MAX_DISPLAY_POINTS;
    }
    
    int outputCount = 0;
    uint32_t firstTs = 0;
    
    for (int i = 0; i < count; i += step) {
        String& line = lines[i];
        int c1 = line.indexOf(',');
        int c2 = line.indexOf(',', c1 + 1);
        if (c1 < 0 || c2 < 0) continue;
        
        uint32_t ts = line.substring(0, c1).toInt();
        float v = line.substring(c1 + 1, c2).toFloat();
        float p = line.substring(c2 + 1).toFloat();
        
        if (firstTs == 0) firstTs = ts;
        
        if (outputCount > 0) json += ",";
        json += "{\"t\":" + String(ts - firstTs);
        json += ",\"v\":" + String(v, 1);
        json += ",\"p\":" + String(p, 1) + "}";
        outputCount++;
    }
    
    json += "],\"count\":" + String(count);
    json += ",\"displayed\":" + String(outputCount);
    json += ",\"paused\":" + String(g_loggingPaused ? "true" : "false") + "}";
    
    return json;
}

// CSV-Export - Daten direkt aus Datei streamen
void handleGraphCSV() {
    if (!LittleFS.exists(LOG_FILE_PATH)) {
        webServer.send(404, "text/plain", "No data");
        return;
    }
    
    File f = LittleFS.open(LOG_FILE_PATH, "r");
    if (!f) {
        webServer.send(500, "text/plain", "Error opening file");
        return;
    }
    
    webServer.sendHeader("Content-Disposition", "attachment; filename=\"psu_data.csv\"");
    webServer.streamFile(f, "text/csv");
    f.close();
}

// ===================== Graph Web-Seite =====================
static const char PROGMEM GRAPH_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>PSU Graph</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        body {
            font-family: 'Segoe UI', Arial, sans-serif;
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
            min-height: 100vh;
            color: #fff;
            padding: 15px;
        }
        .container {
            max-width: 900px;
            margin: 0 auto;
        }
        h1 {
            text-align: center;
            margin-bottom: 20px;
            font-size: 1.8em;
        }
        .card {
            background: rgba(255,255,255,0.05);
            border-radius: 15px;
            padding: 20px;
            margin-bottom: 15px;
            backdrop-filter: blur(10px);
        }
        .graph-container {
            position: relative;
            width: 100%;
            height: 300px;
            background: #0a0a15;
            border-radius: 10px;
            overflow: hidden;
        }
        #graphCanvas {
            width: 100%;
            height: 100%;
        }
        .legend {
            display: flex;
            justify-content: center;
            gap: 30px;
            margin-top: 15px;
            flex-wrap: wrap;
        }
        .legend-item {
            display: flex;
            align-items: center;
            gap: 8px;
        }
        .legend-color {
            width: 20px;
            height: 4px;
            border-radius: 2px;
        }
        .legend-voltage { background: #00ffc8; }
        .legend-power { background: #ff6b6b; }
        
        .time-range {
            display: flex;
            gap: 8px;
            flex-wrap: wrap;
            justify-content: center;
            margin-bottom: 15px;
        }
        .time-btn {
            padding: 8px 16px;
            font-size: 0.9em;
            border: 2px solid #444;
            border-radius: 8px;
            background: transparent;
            color: #fff;
            cursor: pointer;
            transition: all 0.3s;
        }
        .time-btn:hover {
            border-color: #00ffc8;
        }
        .time-btn.active {
            background: #00ffc8;
            color: #1a1a2e;
            border-color: #00ffc8;
        }
        
        .controls {
            display: flex;
            gap: 10px;
            flex-wrap: wrap;
            justify-content: center;
            margin-top: 15px;
        }
        .btn {
            padding: 12px 20px;
            font-size: 1em;
            border: none;
            border-radius: 10px;
            cursor: pointer;
            transition: all 0.3s;
        }
        .btn-pause {
            background: linear-gradient(135deg, #f9a825, #ff8f00);
            color: #1a1a2e;
        }
        .btn-resume {
            background: linear-gradient(135deg, #00ffc8, #00cc9e);
            color: #1a1a2e;
        }
        .btn-clear {
            background: linear-gradient(135deg, #ff6b6b, #ee5a5a);
            color: white;
        }
        .btn-export {
            background: linear-gradient(135deg, #667eea, #764ba2);
            color: white;
        }
        .btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 5px 20px rgba(0,0,0,0.3);
        }
        
        .stats {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(120px, 1fr));
            gap: 15px;
            margin-top: 15px;
        }
        .stat-item {
            text-align: center;
            padding: 10px;
            background: rgba(0,0,0,0.2);
            border-radius: 8px;
        }
        .stat-label {
            font-size: 0.8em;
            color: #888;
        }
        .stat-value {
            font-size: 1.3em;
            font-weight: bold;
            margin-top: 5px;
        }
        .stat-voltage { color: #00ffc8; }
        .stat-power { color: #ff6b6b; }
        
        .status {
            text-align: center;
            padding: 10px;
            font-size: 0.9em;
            color: #888;
        }
        .status.recording {
            color: #00ffc8;
        }
        .status.paused {
            color: #f9a825;
        }
        
        .back-link {
            display: block;
            text-align: center;
            margin-top: 15px;
            color: #00ffc8;
            text-decoration: none;
        }
        .back-link:hover {
            text-decoration: underline;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>📊 PSU Daten-Graph</h1>
        
        <div class="card">
            <div class="time-range">
                <button class="time-btn" data-range="60000" onclick="setTimeRange(60000)">1 Min</button>
                <button class="time-btn" data-range="300000" onclick="setTimeRange(300000)">5 Min</button>
                <button class="time-btn" data-range="600000" onclick="setTimeRange(600000)">10 Min</button>
                <button class="time-btn" data-range="1800000" onclick="setTimeRange(1800000)">30 Min</button>
                <button class="time-btn" data-range="3600000" onclick="setTimeRange(3600000)">1 Std</button>
                <button class="time-btn active" data-range="0" onclick="setTimeRange(0)">Alles</button>
            </div>
            <div class="graph-container">
                <canvas id="graphCanvas"></canvas>
            </div>
            <div class="legend">
                <div class="legend-item">
                    <div class="legend-color legend-voltage"></div>
                    <span>Spannung (V)</span>
                </div>
                <div class="legend-item">
                    <div class="legend-color legend-power"></div>
                    <span>Leistung (W)</span>
                </div>
            </div>
        </div>
        
        <div class="card">
            <div class="controls">
                <button class="btn btn-pause" id="pauseBtn" onclick="togglePause()">⏸ Pause</button>
                <button class="btn btn-clear" onclick="clearData()">🗑 Alles Löschen</button>
                <button class="btn btn-export" onclick="exportCSV()">📥 CSV Export</button>
            </div>
            <div class="status" id="status">Laden...</div>
        </div>
        
        <div class="card">
            <div class="stats">
                <div class="stat-item">
                    <div class="stat-label">Akt. Spannung</div>
                    <div class="stat-value stat-voltage" id="curVoltage">--- V</div>
                </div>
                <div class="stat-item">
                    <div class="stat-label">Akt. Leistung</div>
                    <div class="stat-value stat-power" id="curPower">--- W</div>
                </div>
                <div class="stat-item">
                    <div class="stat-label">Max Spannung</div>
                    <div class="stat-value stat-voltage" id="maxVoltage">--- V</div>
                </div>
                <div class="stat-item">
                    <div class="stat-label">Max Leistung</div>
                    <div class="stat-value stat-power" id="maxPower">--- W</div>
                </div>
            </div>
        </div>
        
        <a href="/" class="back-link">← Zurück zur Steuerung</a>
    </div>

    <script>
        const canvas = document.getElementById('graphCanvas');
        const ctx = canvas.getContext('2d');
        let dataPoints = [];
        let isPaused = false;
        let currentRange = 0;  // 0 = alle
        
        function resizeCanvas() {
            const container = canvas.parentElement;
            canvas.width = container.clientWidth;
            canvas.height = container.clientHeight;
            drawGraph();
        }
        
        window.addEventListener('resize', resizeCanvas);
        resizeCanvas();
        
        function setTimeRange(ms) {
            currentRange = ms;
            document.querySelectorAll('.time-btn').forEach(btn => {
                btn.classList.toggle('active', parseInt(btn.dataset.range) === ms);
            });
            fetchData();
        }
        
        function drawGraph() {
            const w = canvas.width;
            const h = canvas.height;
            const padding = { top: 20, right: 60, bottom: 30, left: 60 };
            const graphW = w - padding.left - padding.right;
            const graphH = h - padding.top - padding.bottom;
            
            // Hintergrund
            ctx.fillStyle = '#0a0a15';
            ctx.fillRect(0, 0, w, h);
            
            // Keine Daten?
            if (dataPoints.length < 2) {
                ctx.fillStyle = '#444';
                ctx.font = '14px Arial';
                ctx.textAlign = 'center';
                ctx.fillText('Keine Daten im gewählten Zeitraum', w/2, h/2);
                return;
            }
            
            // Min/Max finden
            let maxV = 0, maxP = 0, maxT = 0;
            dataPoints.forEach(p => {
                if (p.v > maxV) maxV = p.v;
                if (p.p > maxP) maxP = p.p;
                if (p.t > maxT) maxT = p.t;
            });
            
            // Skalierung mit etwas Puffer (dynamisch basierend auf gemessenen Werten)
            maxV = Math.max(maxV * 1.1, 10);   // Min 10V Skala
            maxP = Math.max(maxP * 1.1, 10);   // Min 10W Skala, skaliert dynamisch bis 3000W+
            maxT = Math.max(maxT, 1000);
            
            // Gitterlinien
            ctx.strokeStyle = '#222';
            ctx.lineWidth = 1;
            for (let i = 0; i <= 5; i++) {
                const y = padding.top + (graphH / 5) * i;
                ctx.beginPath();
                ctx.moveTo(padding.left, y);
                ctx.lineTo(w - padding.right, y);
                ctx.stroke();
            }
            
            // Y-Achsen Labels (Spannung links, Leistung rechts)
            ctx.font = '11px Arial';
            ctx.textAlign = 'right';
            ctx.fillStyle = '#00ffc8';
            for (let i = 0; i <= 5; i++) {
                const y = padding.top + (graphH / 5) * i;
                const val = maxV - (maxV / 5) * i;
                ctx.fillText(val.toFixed(0) + 'V', padding.left - 5, y + 4);
            }
            
            ctx.textAlign = 'left';
            ctx.fillStyle = '#ff6b6b';
            for (let i = 0; i <= 5; i++) {
                const y = padding.top + (graphH / 5) * i;
                const val = maxP - (maxP / 5) * i;
                ctx.fillText(val.toFixed(0) + 'W', w - padding.right + 5, y + 4);
            }
            
            // X-Achse Labels (Zeit)
            ctx.fillStyle = '#666';
            ctx.textAlign = 'center';
            for (let i = 0; i <= 4; i++) {
                const x = padding.left + (graphW / 4) * i;
                const tSec = (maxT / 4) * i / 1000;
                let label;
                if (tSec < 60) {
                    label = tSec.toFixed(0) + 's';
                } else if (tSec < 3600) {
                    label = (tSec / 60).toFixed(1) + 'm';
                } else {
                    label = (tSec / 3600).toFixed(1) + 'h';
                }
                ctx.fillText(label, x, h - 8);
            }
            
            // Spannungs-Linie zeichnen
            ctx.strokeStyle = '#00ffc8';
            ctx.lineWidth = 2;
            ctx.beginPath();
            dataPoints.forEach((p, i) => {
                const x = padding.left + (p.t / maxT) * graphW;
                const y = padding.top + graphH - (p.v / maxV) * graphH;
                if (i === 0) ctx.moveTo(x, y);
                else ctx.lineTo(x, y);
            });
            ctx.stroke();
            
            // Leistungs-Linie zeichnen
            ctx.strokeStyle = '#ff6b6b';
            ctx.lineWidth = 2;
            ctx.beginPath();
            dataPoints.forEach((p, i) => {
                const x = padding.left + (p.t / maxT) * graphW;
                const y = padding.top + graphH - (p.p / maxP) * graphH;
                if (i === 0) ctx.moveTo(x, y);
                else ctx.lineTo(x, y);
            });
            ctx.stroke();
        }
        
        function updateStats() {
            if (dataPoints.length === 0) return;
            
            const last = dataPoints[dataPoints.length - 1];
            document.getElementById('curVoltage').textContent = last.v.toFixed(1) + ' V';
            document.getElementById('curPower').textContent = last.p.toFixed(1) + ' W';
            
            let maxV = 0, maxP = 0;
            dataPoints.forEach(p => {
                if (p.v > maxV) maxV = p.v;
                if (p.p > maxP) maxP = p.p;
            });
            document.getElementById('maxVoltage').textContent = maxV.toFixed(1) + ' V';
            document.getElementById('maxPower').textContent = maxP.toFixed(1) + ' W';
        }
        
        function fetchData() {
            fetch('/graph/data?range=' + currentRange)
                .then(r => r.json())
                .then(data => {
                    dataPoints = data.points || [];
                    isPaused = data.paused;
                    
                    const statusEl = document.getElementById('status');
                    const displayed = data.displayed || dataPoints.length;
                    statusEl.textContent = 'Datenpunkte: ' + data.count + ' (zeige ' + displayed + ')';
                    
                    if (isPaused) {
                        statusEl.textContent += ' - PAUSIERT';
                        statusEl.className = 'status paused';
                    } else {
                        statusEl.textContent += ' - Aufzeichnung läuft';
                        statusEl.className = 'status recording';
                    }
                    
                    // Pause-Button aktualisieren
                    const pauseBtn = document.getElementById('pauseBtn');
                    if (isPaused) {
                        pauseBtn.textContent = '▶ Fortsetzen';
                        pauseBtn.className = 'btn btn-resume';
                    } else {
                        pauseBtn.textContent = '⏸ Pause';
                        pauseBtn.className = 'btn btn-pause';
                    }
                    
                    drawGraph();
                    updateStats();
                })
                .catch(e => console.error('Fetch error:', e));
        }
        
        function togglePause() {
            const endpoint = isPaused ? '/graph/resume' : '/graph/pause';
            fetch(endpoint).then(() => fetchData());
        }
        
        function clearData() {
            if (confirm('ALLE Daten dauerhaft löschen?')) {
                fetch('/graph/clear').then(() => fetchData());
            }
        }
        
        function exportCSV() {
            window.location.href = '/graph/csv';
        }
        
        // Regelmäßig Daten abrufen
        fetchData();
        setInterval(fetchData, 2000);
    </script>
</body>
</html>
)rawliteral";

// ===================== Web Handler =====================

void handleGraphPage() {
    webServer.send_P(200, "text/html", GRAPH_HTML);
}

void handleGraphData() {
    uint32_t range = 0;
    if (webServer.hasArg("range")) {
        range = webServer.arg("range").toInt();
    }
    String json = dataLoggerGetJSON(range);
    webServer.send(200, "application/json", json);
}

void handleGraphPause() {
    dataLoggerSetPaused(true);
    webServer.send(200, "text/plain", "OK");
}

void handleGraphResume() {
    dataLoggerSetPaused(false);
    webServer.send(200, "text/plain", "OK");
}

void handleGraphClear() {
    dataLoggerClear();
    webServer.send(200, "text/plain", "OK");
}

void dataLoggerSetupRoutes() {
    webServer.on("/graph", handleGraphPage);
    webServer.on("/graph/data", handleGraphData);
    webServer.on("/graph/pause", handleGraphPause);
    webServer.on("/graph/resume", handleGraphResume);
    webServer.on("/graph/clear", handleGraphClear);
    webServer.on("/graph/csv", handleGraphCSV);
    Serial.println("[LOGGER] Routes registered - /graph (with LittleFS storage)");
}

#endif // DATA_LOGGER_H
