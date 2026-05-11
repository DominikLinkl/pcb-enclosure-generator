# PSU Control - ESP32 Netzteil-Steuerung

Ein ESP32-basiertes Steuerungssystem für ein regelbares Hochspannungs-Netzteil (38V - 250V) mit Web-Interface, Hardware-Encoder und Datenaufzeichnung.

## 🎯 Anwendungszweck

**Hochspannungs-Leistungsnetzteil für High-Efficiency Class-D Push-Pull RF-Verstärker**

Dieses Netzteil wurde speziell entwickelt für:
- **13.56 MHz ISM-Band** RF-Verstärker
- **Class-D Push-Pull** Topologie für hohe Effizienz
- **Leistungsbereich**: Bis zu 3000W
- **Anwendungen**: Industrielle HF-Anwendungen, Plasma-Generatoren, RF-Heizung

## 🔧 Features

### Spannungsregelung
- **Spannungsbereich**: 38V - 250V
- **PWM-Steuerung**: 12-Bit Auflösung (0-4095), invertiertes Signal
- **Duty Cycle**: 13% - 86%
- **Regelformel**: `U = 2.889983 × PWM - 0.860324`
- **Toleranz**: ±3V automatische Regelung

### Bedienung
- **Web-Interface**: Modernes responsives Design mit Echtzeit-Updates
- **Hardware-Encoder**: Rotary-Encoder für direkte Spannungseinstellung
- **Parallelbetrieb**: Web und Hardware-Encoder funktionieren gleichzeitig

### Web-Interface Features
- 📊 Echtzeit-Anzeige von Spannung, Strom und Leistung
- 🎚️ Slider zur Spannungseinstellung
- ⌨️ Direkteingabe der Zielspannung
- 🔌 PSU Ein/Aus-Steuerung
- 📈 Daten-Graph mit Verlaufsanzeige

### Datenaufzeichnung
- **Speicherung**: Persistent im Flash (LittleFS)
- **Kontinuierlich**: Läuft automatisch im Hintergrund
- **Zeitbereiche**: 1 Min, 5 Min, 10 Min, 30 Min, 1 Std, Alle
- **Export**: CSV-Download
- **Max. Dateigröße**: 800KB mit automatischer Rotation

## 🔌 Hardware-Komponenten

### Mikrocontroller: ESP32-S3

| Eigenschaft | Wert |
|-------------|------|
| **Chip** | ESP32-S3 |
| **CPU** | Dual-Core Xtensa LX7, bis 240 MHz |
| **RAM** | 512 KB SRAM |
| **Flash** | 4-16 MB (je nach Modul) |
| **WiFi** | 802.11 b/g/n |
| **USB** | Native USB OTG |

### Hauptnetzteil: Mean Well CSP-3000-250

| Eigenschaft | Wert |
|-------------|------|
| **Hersteller** | Mean Well |
| **Modell** | CSP-3000-250 |
| **Ausgangsleistung** | 3000W |
| **Ausgangsspannung** | 250V DC (einstellbar) |
| **Ausgangsstrom** | Max. 12A |
| **Eingangsspannung** | 180-264V AC |
| **Wirkungsgrad** | >92% |
| **Steuerung** | 0-10V Analog / PWM |
| **Besonderheit** | Programmierbare Ausgangsspannung via Steuersignal |

### Steuerungsnetzteil: Mean Well 12V

| Eigenschaft | Wert |
|-------------|------|
| **Hersteller** | Mean Well |
| **Ausgangsspannung** | 12V DC |
| **Ausgangsstrom** | 10A |
| **Verwendung** | Versorgung der Steuerungselektronik |
| **Hinweis** | Im Gehäuse verbaut, unabhängig von der Software |

### ADC: MAX22530

| Eigenschaft | Wert |
|-------------|------|
| **Typ** | 4-Kanal SPI ADC |
| **Auflösung** | 12-Bit |
| **Eingangsspannung** | 0-10V (intern skaliert) |
| **Interface** | SPI (bis 5 MHz) |
| **Verwendung** | Spannungsmessung am Netzteil-Ausgang |
| **Besonderheit** | Galvanisch isoliert, für industrielle Anwendungen |

### Stromsensor: ACS712

| Eigenschaft | Wert |
|-------------|------|
| **Typ** | Hall-Effekt Stromsensor |
| **Messbereich** | ±20A / ±30A (je nach Variante) |
| **Empfindlichkeit** | 66-100 mV/A |
| **Ausgangsspannung** | 0.5 × VCC bei 0A (Mittelwert) |
| **Versorgung** | 5V |
| **Verwendung** | Strommessung am Netzteil-Ausgang |

### Display: ILI9488 TFT

| Eigenschaft | Wert |
|-------------|------|
| **Typ** | TFT LCD |
| **Controller** | ILI9488 |
| **Auflösung** | 480 × 320 Pixel |
| **Farbtiefe** | 18-Bit (262k Farben) |
| **Interface** | SPI |
| **Verwendung** | Lokale Anzeige der Messwerte |

## 📁 Projektstruktur

```
finalV2_UPSCALE/
├── finalV2_UPSCALE.ino   # Hauptprogramm
├── web_ui.h              # Web-Interface & WebSocket
├── data_logger.h         # Datenaufzeichnung & Graph
└── README.md             # Diese Datei
```

## 🔌 Hardware-Anschlüsse

### ESP32 Pin-Belegung

| Funktion | Pin | Beschreibung |
|----------|-----|--------------|
| **SPI Bus** | | |
| SCK | 12 | SPI Clock |
| MOSI | 11 | SPI Data Out |
| MISO | 13 | SPI Data In |
| **TFT Display (ILI9488)** | | |
| TFT_CS | 3 | Chip Select |
| TFT_RST | 9 | Reset |
| TFT_DC | 10 | Data/Command |
| **MAX22530 ADC** | | |
| CS | 4 | Chip Select |
| **Rotary Encoder** | | |
| CLK (A) | 36 | Encoder A |
| DT (B) | 37 | Encoder B |
| SW | 38 | Button (aktiv LOW) |
| **PWM Ausgang** | | |
| PWM | 35 | Zum 10V Level-Shifter |
| **Strommmessung** | | |
| ADC | 6 | ACS712 Analog |
| **Remote-Button** | | |
| BTN | 5 | Taster (aktiv HIGH) |
| OUT | 15 | Netzteil Ein/Aus |
| LED | 7 | Status-LED |

## 📡 WLAN-Konfiguration

Das System startet standardmäßig als **Access Point**:

- **SSID**: `PSU-Control`
- **Passwort**: `12345678`
- **Web-Interface**: `http://192.168.4.1`
- **WebSocket**: `ws://192.168.4.1:81`

### Ändern der WLAN-Einstellungen

In `web_ui.h`:
```cpp
// Access Point Modus
#define WIFI_AP_MODE true
#define WIFI_AP_SSID "PSU-Control"
#define WIFI_AP_PASS "12345678"

// Station Modus (mit Router verbinden)
#define WIFI_AP_MODE false
#define WIFI_STA_SSID "DeinWLAN"
#define WIFI_STA_PASS "DeinPasswort"
```

## 🖥️ Web-Interface

### Hauptseite (`/`)
- Aktuelle Messwerte (Spannung, Strom, Leistung, Duty Cycle)
- Spannungs-Slider (38V - 250V)
- Direkteingabe-Feld
- PSU Ein/Aus-Buttons
- Link zum Daten-Graph

### Graph-Seite (`/graph`)
- Echtzeit-Diagramm für Spannung und Leistung
- Zeitbereich-Auswahl
- Pause/Fortsetzen
- CSV-Export
- Statistiken (aktuell, maximal)

## 📊 API-Endpunkte

| Endpunkt | Beschreibung |
|----------|--------------|
| `GET /` | Hauptseite |
| `GET /graph` | Graph-Seite |
| `GET /graph/data?range=X` | JSON-Daten (X in ms, 0=alle) |
| `GET /graph/pause` | Aufzeichnung pausieren |
| `GET /graph/resume` | Aufzeichnung fortsetzen |
| `GET /graph/clear` | Alle Daten löschen |
| `GET /graph/csv` | CSV-Download |

### WebSocket-Kommandos (JSON)

```json
{"cmd": "target", "value": 120}    // Zielspannung setzen
{"cmd": "off"}                      // Regelung aus
{"cmd": "psuOn"}                    // Netzteil ein
{"cmd": "psuOff"}                   // Netzteil aus
```

## 🛠️ Benötigte Bibliotheken

- `WiFi.h` (ESP32 Standard)
- `WebServer.h` (ESP32 Standard)
- `WebSocketsServer.h` ([Links2004/arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets))
- `LittleFS.h` (ESP32 Standard)
- `SPI.h` (Arduino Standard)

### Installation WebSocketsServer

In Arduino IDE:
1. Sketch → Bibliothek einbinden → Bibliotheken verwalten
2. Suche: "WebSockets"
3. Installieren: "WebSockets by Markus Sattler"

## ⚡ Kompilieren & Hochladen

1. **Board auswählen**: ESP32S3 Dev Module (oder dein ESP32-Board)
2. **Partition Scheme**: Default 4MB with spiffs (oder größer für mehr Log-Speicher)
3. **Upload Speed**: 921600
4. Kompilieren und hochladen

## 📈 Spannungsregelung

Die Regelung verwendet eine lineare Formel zur PWM-Berechnung:

```
PWM_Duty = (Zielspannung + 0.860324) / 2.889983
```

Die Regelung ist aktiv wenn:
- Eine Zielspannung ≥ 38V gesetzt wurde
- Der Duty-Wert wird automatisch angepasst bis die Spannung erreicht ist
- Toleranz: ±3V

## 🔄 Datenfluss

```
[MAX22530 ADC] → Spannung
[ACS712]       → Strom
                 ↓
            [ESP32 Core]
                 ↓
    ┌────────────┼────────────┐
    ↓            ↓            ↓
[TFT Display] [WebSocket] [LittleFS]
                 ↓
           [Web Browser]
```

## 📝 Lizenz

Dieses Projekt steht unter der **Creative Commons BY-NC-SA 4.0** Lizenz.

[![License: CC BY-NC-SA 4.0](https://img.shields.io/badge/License-CC%20BY--NC--SA%204.0-lightgrey.svg)](https://creativecommons.org/licenses/by-nc-sa/4.0/)

**Das bedeutet:**
- ✅ **Teilen** — Kopieren und Weiterverteilen erlaubt
- ✅ **Bearbeiten** — Verändern und darauf aufbauen erlaubt
- ✅ **Private Nutzung** — Für persönliche Projekte frei nutzbar
- ❌ **Keine kommerzielle Nutzung** — Nicht für gewerbliche Zwecke
- 📋 **Namensnennung** — Originalautor muss genannt werden
- 🔄 **Weitergabe unter gleichen Bedingungen**

Vollständiger Lizenztext: https://creativecommons.org/licenses/by-nc-sa/4.0/legalcode.de

## ⚠️ Sicherheitshinweis

**ACHTUNG**: Dieses Projekt arbeitet mit Hochspannung (bis 250V) und hoher Leistung (bis 3000W)! 

- ⚡ **Lebensgefahr** durch Hochspannung - nur von qualifizierten Personen verwenden
- 🔥 Hohe Ströme können Brände verursachen - angemessene Kabelquerschnitte verwenden
- 📡 **RF-Strahlung** bei 13.56 MHz - entsprechende Abschirmung und EMV-Maßnahmen beachten
- 🛡️ Niemals an spannungsführenden Teilen arbeiten
- 🔌 Vor Arbeiten am Gerät immer vom Netz trennen und Kondensatoren entladen lassen

## 📻 Anwendungsbereich

Dieses Netzteil ist optimiert für:

```
┌─────────────────────────────────────────────────────────────┐
│                    RF-Verstärker-System                     │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────┐     ┌─────────────┐     ┌─────────────┐    │
│  │  CSP-3000   │────▶│  Class-D    │────▶│  Matching  │    │
│  │  250V PSU   │     │  Push-Pull  │     │  Network    │    │
│  └─────────────┘     │  Amplifier  │     └──────┬──────┘    │
│        ▲             └─────────────┘            │           │
│        │                   ▲                    ▼           │
│  ┌─────┴─────┐       ┌─────┴─────┐     ┌─────────────┐      │
│  │  ESP32-S3 │       │  13.56MHz │     │    Load     │      │
│  │  Control  │       │  Driver   │     │  (Plasma,   │      │
│  └───────────┘       └───────────┘     │   Heater)   │      │
│                                        └─────────────┘      │
└─────────────────────────────────────────────────────────────┘
```

**13.56 MHz ISM-Band Spezifikationen:**
- Frequenz: 13.56 MHz ± 7 kHz
- Lizenzfreies ISM-Band (Industrial, Scientific, Medical)
- Typische Anwendungen: Plasma-Erzeugung, RF-Schweißen, Induktionsheizung

