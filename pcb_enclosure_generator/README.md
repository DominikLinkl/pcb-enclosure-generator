# PCB Gehäuse-Generator

Webbasierter SaaS-Dienst, der aus hochgeladenen Leiterplatten-Daten (Gerber-ZIP
oder STEP) vollautomatisch passgenaue, 3D-druckbare Gehäuse erzeugt.
Fokus: ESD-sichere FDM-Materialien (PETG, ABS), Heat-Set-Inserts (M3),
ressourcenschonende Bauplan-Vorschau ohne serverseitiges 3D-Rendering.

## Features

- **Gerber-ZIP & STEP Upload** – automatische Outline-, Bohrloch- und I/O-Erkennung
- **Live-Bauplan-Vorschau** – Top-Down-Ansicht mit Außenwand, PCB, Montage-Standoffs, Deckelschrauben und I/O-Cutouts; aktualisiert sich bei jeder Parameter-Änderung
- **Manuelle Platzierung** – Standoffs und Cutouts für Klemmen, USB-Buchsen etc. direkt im Webformular hinzufügen, falls die Auto-Erkennung sie nicht findet
- **FDM-optimiertes CAD** (build123d):
  - Heat-Set-Insert-Kernlöcher (Standard 4.0 mm Ø für M3)
  - 45°-Fasen an I/O-Cutouts für Support-freien Druck
  - ABS/PETG-Schrumpfungsausgleich am Deckel (0,2 %)
  - Dynamische Deckelschrauben: max. alle 30 mm am Umfang, min. 4 gesamt
  - Durchgehende M3-Schraublöcher im Deckel
- **Export**: STL (Druck) + STEP (CAD-editierbar) für Schale und Deckel

## Installation

### Voraussetzungen

- Python ≥ 3.10
- pip + venv

### Setup

```bash
git clone https://github.com/DominikLinkl/pcb-enclosure-generator.git
cd pcb-enclosure-generator

# Virtuelle Umgebung anlegen und aktivieren
python -m venv .venv
source .venv/bin/activate          # Linux / macOS
# .venv\Scripts\activate           # Windows PowerShell

# Abhängigkeiten installieren
pip install -r backend/requirements.txt
```

> **Hinweis zu `build123d`**: Die Bibliothek bringt OpenCASCADE als binäres
> Wheel mit. Auf Linux/macOS/Windows (x86_64, arm64) klappt `pip install`
> direkt. Bei exotischen Plattformen siehe
> [build123d Docs](https://build123d.readthedocs.io/en/latest/installation.html).

## Server starten

```bash
cd backend
python main.py
```

Der Server läuft auf <http://localhost:8000>. Die Web-UI öffnet sich direkt
unter dieser URL — das Frontend wird vom FastAPI-Server als statische Dateien
mit ausgeliefert.

Alternativ explizit über Uvicorn:

```bash
cd backend
uvicorn main:app --host 0.0.0.0 --port 8000 --reload
```

## Workflow im Webinterface

1. **Upload**: Gerber-ZIP oder STEP-Datei per Drag-and-Drop hochladen.
2. **Bauplan & Parameter**: Im Bauplan siehst du das fertige Gehäuse von oben.
   Verstelle die Slider — der Bauplan aktualisiert sich live.
   - Wand- und Bodenstärke
   - Lichte Innenhöhe (Z)
   - Toleranz-Offset (Spaltmaß PCB ↔ Innenwand)
   - PCB-Standoff-Höhe
   - Heat-Set-Kernlochdurchmesser
   - Befestigungslöcher aktivieren/deaktivieren oder manuell hinzufügen
   - I/O-Cutouts (USB, Klemmen, Schalter) mit Wand-Auswahl, Position, Breite, Höhe, Z-Offset
3. **Generieren**: Klick auf "Gehäuse generieren". Der Backend-CAD-Kernel
   berechnet Schale und Deckel.
4. **Download**: STL- und STEP-Dateien für Schale und Deckel herunterladen.

## Architektur

```
pcb_enclosure_generator/
├── backend/
│   ├── main.py                  # FastAPI: Upload, Generate, Download
│   ├── pcb_parser.py            # Gerber- & STEP-Parser
│   ├── enclosure_generator.py   # build123d CAD-Kernel
│   ├── requirements.txt
│   └── uploads/                 # Temp-Verzeichnis pro Job
└── frontend/
    └── index.html               # Single-Page UI mit Bauplan-Canvas
```

| Komponente        | Stack                  | Aufgabe                                                   |
|-------------------|------------------------|-----------------------------------------------------------|
| Backend           | Python 3.10 / FastAPI  | Routing, Job-Verwaltung, JSON-Param-Parsing               |
| CAD-Kernel        | build123d (OpenCASCADE)| Parametrische 3D-Konstruktion, STL- & STEP-Export         |
| Frontend          | HTML + Vanilla JS      | Datei-Upload, Bauplan-Canvas, Parameter-Formular          |
| 2D-Geometrie      | shapely (optional)     | Outline-/Polygon-Operationen                              |

## API-Endpoints

| Methode | Pfad                              | Zweck                                                    |
|---------|-----------------------------------|----------------------------------------------------------|
| POST    | `/api/upload`                     | Gerber-ZIP / STEP hochladen → Analyse-JSON               |
| POST    | `/api/generate`                   | Parameter + Platzierungen senden → CAD generieren        |
| GET     | `/api/download/{job_id}/{file}`   | Generierte STL/STEP-Datei abrufen                        |

Vollständige Schema-Doku unter <http://localhost:8000/docs> (Swagger-UI).

## FDM-Druckempfehlungen

- **Material**: PETG oder ABS für ESD-Anforderungen, geringe Schrumpfung
- **Schichtdicke**: 0,2 mm für Schale, 0,16 mm für Deckel-Passung
- **Infill**: 25–40 % Gyroid
- **Perimeter**: 3 Wände bei 2,4 mm Wandstärke
- **Heat-Set-Inserts**: M3 Standard, mit Lötkolben (≈ 220 °C) in Standoffs setzen
- **Schrauben**: M3 × 10 mm Linsenkopf für Deckel-Verschraubung

## Roadmap

- [ ] User-Account-System für Projektverwaltung
- [ ] Konflikt-Checks (z. B. Cutout sprengt Gehäuse)
- [ ] Payment-Wall vor Download
- [ ] 3D-Preview (Three.js, client-seitig)
- [ ] Bauteilhöhen aus STEP extrahieren für automatische Z-Offsets
- [ ] DXF-Export für 2D-Frästeile

## Lizenz

MIT — siehe `LICENSE`.
