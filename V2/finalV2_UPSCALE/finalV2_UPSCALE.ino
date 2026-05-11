#include <Arduino.h>
#include <SPI.h>

// Forward declarations für web_ui.h
void applyOutputAndLog(const char* reason, bool clampToLimits);

#include "web_ui.h"
#include "data_logger.h"

/* ===================== Gemeinsamer SPI-Bus  ===================== */
#define PIN_SCK   12 // SCK (vorher 21)
#define PIN_MOSI  11 // SDI
#define PIN_MISO  13 // SDO (vorher 20)

/* ===================== ILI9488 (TFT) Pins ===================== */
#define TFT_CS     3
#define TFT_RST    9
#define TFT_DC     10   

// max ampere meter
static const int   ADC_PIN = 6;  

/*======================= Remote-Button (EIN/AUS) ===============================*/
#define PIN_BTN 5    // Taster: nach 3.3 V (aktiv HIGH)
#define PIN_OUT 15   // Ausgang: schaltet Netzteil ein/aus
#define PIN_LED 7    // LED: leuchtet parallel zu OUT

bool g_remoteState = false;  // merkt aktuellen Zustand (nicht static für extern Zugriff)

void remoteButtonSetup() {
  pinMode(PIN_BTN, INPUT_PULLDOWN);  // aktiv HIGH
  pinMode(PIN_OUT, OUTPUT);
  pinMode(PIN_LED, OUTPUT);

  digitalWrite(PIN_OUT, LOW);
  digitalWrite(PIN_LED, LOW);
  g_remoteState = false;

  Serial.println("[REMOTE] Init ok. Ausgang=LOW");
}


void remoteButtonTask() {
  if (digitalRead(PIN_BTN) == HIGH) {
    for (int i = 1; i <= 2; i++) {
      if (i == 2) {
        g_remoteState = !g_remoteState;
        digitalWrite(PIN_OUT, g_remoteState ? HIGH : LOW);
        digitalWrite(PIN_LED, g_remoteState ? HIGH : LOW);

        Serial.print("[REMOTE] Ausgang ist jetzt: ");
        Serial.println(g_remoteState ? "HIGH" : "LOW");
      }
      delay(30); 
    }
    while (digitalRead(PIN_BTN) == HIGH) delay(5); // warten bis losgelassen
    delay(50);
  }
}

// Funktion zum Setzen des PSU-Zustands (für Web-UI)
void setPsuState(bool state) {
  g_remoteState = state;
  digitalWrite(PIN_OUT, g_remoteState ? HIGH : LOW);
  digitalWrite(PIN_LED, g_remoteState ? HIGH : LOW);
  Serial.print("[WEB] PSU Ausgang: ");
  Serial.println(g_remoteState ? "EIN" : "AUS");
}

/* ===================== MAX22530 (SPI-ADC)  ===================== */
#define CS_PIN    4     // Chip Select
#define MOSI_PIN  PIN_MOSI
#define MISO_PIN  PIN_MISO
#define SCK_PIN   PIN_SCK

/* ===================== Rotary-Encoder + PWM ===================== */
constexpr int PIN_PWM    = 35;   // PWM-Ausgang zum 10V-Level-Shifter
constexpr int PIN_ENC_A  = 36;   // Rotary CLK (A)
constexpr int PIN_ENC_B  = 37;   // Rotary DT  (B)
constexpr int PIN_ENC_SW = 38;   // Rotary Button (aktiv LOW)

constexpr uint32_t PWM_FREQ_HZ  = 1000;
constexpr uint8_t  PWM_RES_BITS = 12;     // 0..4095
constexpr uint32_t PWM_MAX_DUTY = (1UL << PWM_RES_BITS) - 1;

bool g_outputInverted = true;  // PWM Invertierung (fest auf invertiert)
constexpr uint8_t BTN_PSU_DUTY_PERCENT  = 100;

const uint8_t PSU_DUTY_MIN = 13;   // Minimum 13% (Netzteil geht nicht auf 0V)
const uint8_t PSU_DUTY_MAX = 86;

// Encoder-Schrittweite
constexpr float    DUTY_STEP_PERCENT = 0.50f;
constexpr uint32_t ENC_SAMPLE_US     = 1000; // 1 ms

// Feinwert (float) + gerundeter Int-Wert für Anzeige/Logs
volatile float g_psuDutyPctFine = 50.0f; // interner feiner Duty-Wert
volatile int   g_psuDutyPct     = 50;    // gerundeter Duty-Wert

uint32_t       lastEncSampleUs = 0;
int32_t        encAccum = 0;
uint8_t        prevAB = 0;

// Gray-Code LUT (Index = (prevAB<<2)|currAB), Werte -1/0/+1
const int8_t encLut[16] = {
  0, -1, +1,  0,
  +1, 0,  0, -1,
  -1, 0,  0, +1,
   0, +1, -1, 0
};

/* ===================== MAX22530 Register ===================== */
#define REG_CONFIG      0x00
#define REG_CHANNEL     0x01
#define REG_STATUS      0x02
#define REG_DATA_H      0x03
#define REG_DATA_L      0x04

/* ===================== Anzeige / State ===================== */
volatile float inputVoltage = 0.0f;  // aus SPI-ADC (0..250V)
volatile float inputCurrent = 0.0f;  // aus ACS712 (A)
volatile float inputPower   = 0.0f;  // U*I (W)

/* --------- ILI9488-Minimaltreiber --------- */
static inline void tftCsLow()  { digitalWrite(TFT_CS, LOW); }
static inline void tftCsHigh() { digitalWrite(TFT_CS, HIGH); }
static inline void dcCmd()     { digitalWrite(TFT_DC, LOW); }
static inline void dcData()    { digitalWrite(TFT_DC, HIGH); }

static inline void startWrite(){ tftCsLow(); }
static inline void endWrite()  { tftCsHigh(); }

static inline void wr8(uint8_t d)       { SPI.transfer(d); }
static inline void writeCmd(uint8_t c)  { dcCmd();  wr8(c); }
static inline void writeData8(uint8_t d){ dcData(); wr8(d); }

static inline uint8_t _r6(uint8_t r){ return r & 0xFC; }
static inline uint8_t _g6(uint8_t g){ return g & 0xFC; }
static inline uint8_t _b6(uint8_t b){ return b & 0xFC; }

static uint8_t g_rotation = 1; // 1 = Landscape (480x320)
#define TFT_RAW_W 320
#define TFT_RAW_H 480
static inline int16_t screenW(){ return (g_rotation & 1) ? TFT_RAW_H : TFT_RAW_W; }
static inline int16_t screenH(){ return (g_rotation & 1) ? TFT_RAW_W : TFT_RAW_H; }

static inline void setAddrWindow(uint16_t x0,uint16_t y0,uint16_t x1,uint16_t y1){
  writeCmd(0x2A); dcData(); wr8(x0>>8); wr8(x0); wr8(x1>>8); wr8(x1);
  writeCmd(0x2B); dcData(); wr8(y0>>8); wr8(y0); wr8(y1>>8); wr8(y1);
  writeCmd(0x2C); dcData();
}

void setRotation(uint8_t r){
  g_rotation = (r & 3);
  startWrite();
  writeCmd(0x36);
  const uint8_t mad[4] = { 0x48, 0x28, 0x88, 0xE8 };
  writeData8(mad[g_rotation]);
  endWrite();
}

void ili9488_init(){
#if (TFT_RST >= 0)
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, HIGH); delay(10);
  digitalWrite(TFT_RST, LOW);  delay(20);
  digitalWrite(TFT_RST, HIGH); delay(120);
#endif
  startWrite(); writeCmd(0x01); endWrite(); delay(150); // SW Reset
  startWrite();
  writeCmd(0x28);                // OFF
  writeCmd(0x3A); writeData8(0x66); // 18-bit
  writeCmd(0x11);                // Sleep OUT
  endWrite(); delay(120);
  setRotation(3);                // Landscape
  startWrite(); writeCmd(0x29); endWrite(); delay(20); // ON
}

// einfache Füll- und Textfunktionen
static uint8_t lineBuf[3*480];

void fillRect(int x,int y,int w,int h, uint8_t r,uint8_t g,uint8_t b){
  if (w<=0||h<=0) return;
  int W=screenW(), H=screenH();
  if (x<0){w+=x; x=0;} if (y<0){h+=y; y=0;}
  if (x>=W||y>=H) return;
  if (x+w>W) w=W-x; if (y+h>H) h=H-y;

  uint8_t R=_r6(r), G=_g6(g), B=_b6(b);
  int rowBytes = w*3;
  for(int i=0;i<w;i++){
    int j=i*3; lineBuf[j]=R; lineBuf[j+1]=G; lineBuf[j+2]=B;
  }

  startWrite(); setAddrWindow(x,y,x+w-1,y+h-1);
  for(int yy=0; yy<h; ++yy) SPI.writeBytes(lineBuf, rowBytes);
  endWrite();
}

void fillScreen(uint8_t r,uint8_t g,uint8_t b){
  fillRect(0,0,screenW(),screenH(), r,g,b);
}

/* 5x7-Zeichensatz: Ziffern, Punkt, V, W, A, Leerzeichen */
static const uint8_t GLYPH_SPACE[5] = {0,0,0,0,0};
static const uint8_t GLYPH_DOT[5]   = {0,0,0,0,0x40};
static const uint8_t GLYPH_V[5]     = {0x03,0x1C,0x60,0x1C,0x03};
static const uint8_t GLYPH_W[5]     = {0x1F,0x60,0x1C,0x60,0x1F};
static const uint8_t GLYPH_A[5]     = {0x7E,0x11,0x11,0x11,0x7E};
static const uint8_t DIGIT[10][5] = {
  {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
  {0x62,0x51,0x49,0x49,0x46},{0x22,0x49,0x49,0x49,0x36},
  {0x18,0x14,0x12,0x7F,0x10},{0x2F,0x49,0x49,0x49,0x31},
  {0x3E,0x49,0x49,0x49,0x32},{0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36},{0x26,0x49,0x49,0x49,0x3E}
};
const uint8_t* glyphCols(char c){
  if (c>='0' && c<='9') return DIGIT[c-'0'];
  switch(c){
    case ' ': return GLYPH_SPACE;
    case '.': return GLYPH_DOT;
    case 'V': return GLYPH_V;
    case 'W': return GLYPH_W;
    case 'A': return GLYPH_A;
    default:  return GLYPH_SPACE;
  }
}
void drawChar5x7(int x,int y,char c,int scale,
                 uint8_t fr,uint8_t fg,uint8_t fb,
                 uint8_t br,uint8_t bg,uint8_t bb){
  const uint8_t* cols = glyphCols(c);
  uint8_t FR=_r6(fr), FG=_g6(fg), FB=_b6(fb);
  uint8_t BKr=_r6(br), BKg=_g6(bg), BKb=_b6(bb);
  startWrite();
  for (int row=0; row<7; ++row){
    for (int sy=0; sy<scale; ++sy){
      int idx=0;
      for (int col=0; col<5; ++col){
        bool on = (cols[col] >> row) & 0x01;
        uint8_t rr = on?FR:BKr, gg=on?FG:BKg, bb2=on?FB:BKb;
        for (int sx=0; sx<scale; ++sx){
          lineBuf[idx++]=rr; lineBuf[idx++]=gg; lineBuf[idx++]=bb2;
        }
      }
      lineBuf[idx++]=BKr; lineBuf[idx++]=BKg; lineBuf[idx++]=BKb; // 1px Spalt
      setAddrWindow(x, y + row*scale + sy, x + (5*scale), y + row*scale + sy);
      SPI.writeBytes(lineBuf, idx);
    }
  }
  endWrite();
}
int textWidth5x7(const char* s, int scale){
  int n=0; for(const char* p=s; *p; ++p) n++;
  return n*(5*scale + 1);
}
void drawText5x7(int x,int y,const char* s,int scale,
                 uint8_t fr,uint8_t fg,uint8_t fb,
                 uint8_t br,uint8_t bg,uint8_t bb){
  for (const char* p=s; *p; ++p){
    drawChar5x7(x,y,*p,scale,fr,fg,fb,br,bg,bb);
    x += 5*scale + 1;
  }
}

/* ===================== MAX22530 (SPI-ADC) ===================== */
void writeRegister(uint8_t reg, uint8_t value) {
  digitalWrite(CS_PIN, LOW);
  SPI.transfer((reg << 1) | 0x80);  // Write command
  SPI.transfer(value);
  digitalWrite(CS_PIN, HIGH);
}
uint8_t readRegister(uint8_t reg) {
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(reg << 1);  // Read command
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(CS_PIN, HIGH);
  return value;
}
uint16_t readADC() {
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(REG_DATA_L << 1);  // Read command
  uint16_t highByte = SPI.transfer(0x00);
  uint16_t lowByte  = SPI.transfer(0x00);
  digitalWrite(CS_PIN, HIGH);
  return (highByte << 8) | lowByte;
}

/* ===================== Rotary/PWM ===================== */
void applyOutputAndLog(const char* reason, bool clampToLimits = true) {
  // mit feinem Float-Wert arbeiten
  float psuFine = g_psuDutyPctFine;

  if (clampToLimits) {
    if (psuFine < PSU_DUTY_MIN) psuFine = PSU_DUTY_MIN;
    if (psuFine > PSU_DUTY_MAX) psuFine = PSU_DUTY_MAX;
  }

  g_psuDutyPctFine = psuFine;
  g_psuDutyPct     = (int)lroundf(psuFine);  // gerundet für Anzeige/Status

  float dutyPSU = psuFine;  // in %
  float dutyESP = g_outputInverted ? (100.0f - dutyPSU) : dutyPSU;

  if (dutyESP < 0.0f)   dutyESP = 0.0f;
  if (dutyESP > 100.0f) dutyESP = 100.0f;

  uint32_t dutyCntOut =
      (uint32_t)lroundf((dutyESP / 100.0f) * PWM_MAX_DUTY);

  analogWrite(PIN_PWM, dutyCntOut);

  Serial.printf("[%s] Duty@PSU=%.2f %%  (ESP=%.2f %%, counts=%u)\n",
                reason, dutyPSU, dutyESP, dutyCntOut);
}

/* ===================== Gemeinsame 10er-Mittelung für DISPLAY-Takt ===================== */
constexpr int NUM_SAMPLES = 10;

// Spannung
float vbuf[NUM_SAMPLES] = {0};
uint8_t vidx = 0;
bool vfull = false;
float vsum = 0.0f;

// Strom (neu: gleicher 10er-Puffer wie Spannung)
float ibuf[NUM_SAMPLES] = {0};
float isum = 0.0f;

// Letztgezeigte gerundete Werte
float lastShownVolt = -999.0f;
float lastShownPow  = -999.0f;
float lastShownAmp  = -999.0f;

/* ===================== ACS712-ANALOG-ADC (Strom) ===================== */
static const int   PRINT_HZ               = 10;
static const int   STARTUP_CAL_SAMPLES    = 500;
static const int   BLOCK_AVG_SAMPLES      = 20;
static const float EMA_ALPHA              = 0.2f;
static const bool  USE_MILLIVOLTS_API     = true;
static const int   ADC_ATTENUATION        = 3;      // 11dB ~ 0..3.3V

// Spannungsteiler (Sensor->ADC): Rt oben, Rb unten
static const float TEILER_RT_OHM = 9974.0f;   // oben (Sensor->ADC)
static const float TEILER_RB_OHM = 5087.0f;   // unten (ADC->GND)
static const float TEILER_FAKTOR = (TEILER_RB_OHM / (TEILER_RT_OHM + TEILER_RB_OHM)); // ≈ 0.3377598

#ifndef USE_CAL_GAIN
static const float SENSITIVITY_mV_PER_A_RAW = 100.0f;  // 20A-Variante
static const float SENSITIVITY_mV_PER_A     = SENSITIVITY_mV_PER_A_RAW * TEILER_FAKTOR;
// ≈ 33.776 mV/A am ADC
#else
static const float SENSITIVITY_mV_PER_A_RAW = 66.0f;
static const float CAL_GAIN                 = 0.631f;
static const float SENSITIVITY_mV_PER_A     = (SENSITIVITY_mV_PER_A_RAW * TEILER_FAKTOR) / CAL_GAIN;
#endif

float zero_mV = 1650.0f;   // erwarteter Offset (~2.5V*Teiler ≈ 1.65V)
float ema_A   = 0.0f;
unsigned long lastPrintMs = 0;

// ---------- Helpers ----------
void setAdcAttenuation(int pin, int att){
  switch (att) {
    case 0: analogSetPinAttenuation(pin, ADC_0db);   break;
    case 1: analogSetPinAttenuation(pin, ADC_2_5db); break;
    case 2: analogSetPinAttenuation(pin, ADC_6db);   break;
    default:analogSetPinAttenuation(pin, ADC_11db);  break;
  }
}
int readMilliVolts(int pin){
  if (USE_MILLIVOLTS_API) return analogReadMilliVolts(pin);
  const float FULL_SCALE_mV = 3300.0f;
  int raw = analogRead(pin);
  return (int)((raw * FULL_SCALE_mV) / 4095.0f);
}
float calibrateZero_mV(int pin, int samples){
  long sum = 0;
  for (int i = 0; i < samples; ++i) {
    sum += readMilliVolts(pin);
    delayMicroseconds(500);
  }
  return (float)sum / (float)samples;
}

/* ===================== UI: große 7-Segment-Ziffern + kleine 5x7-Texte ===================== */

// große Spannungsziffern: echte Display-Pixel, keine 5x7-Skalierung
const int BIGDIGIT_W        = 60;  // Breite einer Ziffer in Pixel
const int BIGDIGIT_H        = 100; // Höhe einer Ziffer in Pixel
const int BIGSEG_THICK      = 10;  // Segmentdicke
const int BIGSEG_MARGIN     = 6;   // Abstand innen
const int BIGDIGIT_SPACING  = 8;   // Abstand zwischen Ziffern

// kleine Texte (P, I) weiter mit 5x7-Font
const int SCALE_SMALL = 7;  // Power/Current
const int MARGIN_X    = 16;
const int MARGIN_TOP  = 14;
const int GAP_AFTER_V = 10;   // Abstand nach Spannungszeile
const int BAR_H       = 28;
const int GAP_AFTER_BAR = 10; // Abstand nach Balken
const int GAP_LINES   = 8;    // Abstand zwischen Power/Current

void drawBar(float v); // Vorwärtsdeklaration

// Breite des Zahlenteils (Ziffern + Punkt) in Pixel
int textWidthBigDigits(const char* s){
  int w = 0;
  const char* p = s;
  while (*p) {
    char ch = *p;
    int cw = 0;
    if (ch >= '0' && ch <= '9') {
      cw = BIGDIGIT_W;
    } else if (ch == '.') {
      cw = BIGSEG_THICK * 2; // Punktbreite
    } else {
      ++p;
      continue;
    }
    w += cw;
    if (*(p+1)) w += BIGDIGIT_SPACING; // Abstand außer nach letzter
    ++p;
  }
  return w;
}

// 7-Segment Mapping für eine Ziffer, mit Spezialfall für '1' (durchgehender Balken)
void drawBigDigit7Seg(int x, int y, char ch,
                      uint8_t fr,uint8_t fg,uint8_t fb,
                      uint8_t br,uint8_t bg,uint8_t bb)
{
  // kompletten Bereich zuerst löschen
  fillRect(x, y, BIGDIGIT_W, BIGDIGIT_H, br,bg,bb);

  // Spezialfall '1' -> durchgehender rechter Balken
  if (ch == '1') {
    uint8_t R = fr, G = fg, B = fb;
    fillRect(
      x + BIGDIGIT_W - BIGSEG_THICK,
      y + BIGSEG_MARGIN,
      BIGSEG_THICK,
      BIGDIGIT_H - 2 * BIGSEG_MARGIN,
      R, G, B
    );
    return;
  }

  bool sa=false,sb=false,sc=false,sd=false,se=false,sf=false,sg=false;
  // a=oben, b=oben rechts, c=unten rechts, d=unten, e=unten links,
  // f=oben links, g=mitte
  switch (ch) {
    case '0': sa=true; sb=true; sc=true; sd=true; se=true; sf=true; sg=false; break;
    case '2': sa=true; sb=true; sc=false;sd=true; se=true; sf=false;sg=true;  break;
    case '3': sa=true; sb=true; sc=true; sd=true; se=false;sf=false;sg=true;  break;
    case '4': sa=false;sb=true; sc=true; sd=false;se=false;sf=true; sg=true;  break;
    case '5': sa=true; sb=false;sc=true; sd=true; se=false;sf=true; sg=true;  break;
    case '6': sa=true; sb=false;sc=true; sd=true; se=true; sf=true; sg=true;  break;
    case '7': sa=true; sb=true; sc=true; sd=false;se=false;sf=false;sg=false; break;
    case '8': sa=true; sb=true; sc=true; sd=true; se=true; sf=true; sg=true;  break;
    case '9': sa=true; sb=true; sc=true; sd=true; se=false;sf=true; sg=true;  break;
    default: return;
  }

  uint8_t R = fr, G = fg, B = fb;

  int innerW = BIGDIGIT_W - 2*BIGSEG_MARGIN;
  int halfH  = BIGDIGIT_H / 2;

  // Segment a (oben)
  if (sa) {
    fillRect(x + BIGSEG_MARGIN,
             y,
             innerW,
             BIGSEG_THICK,
             R,G,B);
  }
  // Segment d (unten)
  if (sd) {
    fillRect(x + BIGSEG_MARGIN,
             y + BIGDIGIT_H - BIGSEG_THICK,
             innerW,
             BIGSEG_THICK,
             R,G,B);
  }
  // Segment g (Mitte)
  if (sg) {
    fillRect(x + BIGSEG_MARGIN,
             y + halfH - BIGSEG_THICK/2,
             innerW,
             BIGSEG_THICK,
             R,G,B);
  }
  int vertH_top  = halfH - BIGSEG_MARGIN*2;
  int vertH_bot  = BIGDIGIT_H - halfH - BIGSEG_MARGIN*2;

  // Segment f (oben links)
  if (sf) {
    fillRect(x,
             y + BIGSEG_MARGIN,
             BIGSEG_THICK,
             vertH_top,
             R,G,B);
  }
  // Segment e (unten links)
  if (se) {
    fillRect(x,
             y + halfH + BIGSEG_MARGIN,
             BIGSEG_THICK,
             vertH_bot,
             R,G,B);
  }
  // Segment b (oben rechts)
  if (sb) {
    fillRect(x + BIGDIGIT_W - BIGSEG_THICK,
             y + BIGSEG_MARGIN,
             BIGSEG_THICK,
             vertH_top,
             R,G,B);
  }
  // Segment c (unten rechts)
  if (sc) {
    fillRect(x + BIGDIGIT_W - BIGSEG_THICK,
             y + halfH + BIGSEG_MARGIN,
             BIGSEG_THICK,
             vertH_bot,
             R,G,B);
  }
}

void drawBigVoltage(float v){
  char buf[24];

  // jetzt 0.1 V Auflösung
  float rounded = floorf(v*10.0f + 0.5f)/10.0f;
  if (fabsf(rounded - lastShownVolt) < 0.1f) return;
  lastShownVolt = rounded;

  dtostrf(rounded, 0, 1, buf);  // 1 Nachkommastelle

  uint8_t fr = 0, fg = 255, fb = 180;

  int twDigits = textWidthBigDigits(buf);
  int unitScale = 4;
  int twUnit   = textWidth5x7(" V", unitScale);
  int gapNumUnit = 8;
  int totalW  = twDigits + gapNumUnit + twUnit;

  int x  = (screenW() - totalW) / 2;
  int y  = MARGIN_TOP;

  // Bereich für die große Zahl komplett löschen
  fillRect(0, y-4, screenW(), BIGDIGIT_H + 20, 0,0,0);

  int cx = x;

  // Zahl (Ziffern + Punkt)
  for (const char* p = buf; *p; ++p) {
    char ch = *p;
    if (ch >= '0' && ch <= '9') {
      drawBigDigit7Seg(cx, y, ch, fr,fg,fb, 0,0,0);
      cx += BIGDIGIT_W;
      if (*(p+1)) cx += BIGDIGIT_SPACING;
    } else if (ch == '.') {
      int dotSize = BIGSEG_THICK;
      int dotX = cx;
      int dotY = y + BIGDIGIT_H - dotSize - 2;
      fillRect(dotX, dotY, dotSize, dotSize, fr,fg,fb);
      cx += dotSize;
      if (*(p+1)) cx += BIGDIGIT_SPACING;
    }
  }

  // etwas Abstand zur Einheit
  cx += gapNumUnit;

  // Einheit " V" mit 5x7-Font
  int unitY = y + BIGDIGIT_H - 7*unitScale - 4;
  drawText5x7(cx, unitY, " V", unitScale, fr,fg,fb, 0,0,0);

  drawBar(v);
}

void drawBar(float v){
  int yTop = MARGIN_TOP + BIGDIGIT_H + GAP_AFTER_V;
  const int barW = screenW() - 2*MARGIN_X;
  const int barX = MARGIN_X;
  const int barY = yTop;

  fillRect(barX, barY, barW, BAR_H, 0,0,0);

  fillRect(barX,           barY,            barW, 1,   80,80,80);
  fillRect(barX,           barY+BAR_H-1,    barW, 1,   80,80,80);
  fillRect(barX,           barY,            1,    BAR_H,80,80,80);
  fillRect(barX+barW-1,    barY,            1,    BAR_H,80,80,80);

  float frac = v / 250.0f;
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  int fillW = (int)lroundf(frac * (barW - 2));
  if (fillW > 0) {
    fillRect(barX+1, barY+1, fillW, BAR_H-2, 0,200,120);
  }
}

void drawSmallCentered(const char* label, const char* unit, float value, int y, float &lastShownRef){
  char num[24];

  // jetzt: Spannung & Strom -> 1 Nachkommastelle (hier betrifft es Strom "A")
  int decimals;
  if (unit[0] == 'W') {
    decimals = 1;      // Leistung 0.1 W
  } else if (unit[0] == 'A') {
    decimals = 1;      // Strom 0.1 A
  } else {
    decimals = 2;      // Fallback
  }

  float factor = (decimals == 1) ? 10.0f :
                 (decimals == 2) ? 100.0f : 1000.0f;

  float rounded = floorf(value*factor + 0.5f)/factor;
  float thresh  = 1.0f / factor;

  if (fabsf(rounded - lastShownRef) < thresh) return;
  lastShownRef = rounded;

  dtostrf(rounded, 0, decimals, num);

  char line[64];
  snprintf(line, sizeof(line), "%s: %s %s", label, num, unit);

  int tw = textWidth5x7(line, SCALE_SMALL);
  int x  = (screenW() - tw)/2;

  fillRect(0, y-2, screenW(), 7*SCALE_SMALL + 6, 0,0,0);
  drawText5x7(x, y, line, SCALE_SMALL, 180,220,255, 0,0,0);
}

void drawUI_averaged(float vAvg, float iAvg){
  drawBigVoltage(vAvg);
  float p = vAvg * iAvg;

  int yBarTop = MARGIN_TOP + BIGDIGIT_H + GAP_AFTER_V;
  int yPow    = yBarTop + BAR_H + GAP_AFTER_BAR;
  int yCur    = yPow + (7*SCALE_SMALL) + GAP_LINES;

  drawSmallCentered("P", "W", p,   yPow, lastShownPow);
  drawSmallCentered("I", "A", iAvg,yCur, lastShownAmp);

  inputVoltage = vAvg;
  inputCurrent = iAvg;
  inputPower   = p;
}


/* ===================== Setup ===================== */
void setup() {
  Serial.begin(115200);
  delay(50);

  // Remote-Button initialisieren
  remoteButtonSetup();

  pinMode(TFT_CS, OUTPUT);  digitalWrite(TFT_CS, HIGH);
  pinMode(TFT_DC, OUTPUT);  digitalWrite(TFT_DC, HIGH);
#if (TFT_RST >= 0)
  pinMode(TFT_RST, OUTPUT);
#endif

  pinMode(CS_PIN, OUTPUT);  digitalWrite(CS_PIN, HIGH);

  // Rotary + PWM
  pinMode(PIN_PWM,   OUTPUT);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);

  analogWriteResolution(PIN_PWM, PWM_RES_BITS);
  analogWriteFrequency(PIN_PWM, PWM_FREQ_HZ);
  analogWrite(PIN_PWM, 0);

  uint8_t a = digitalRead(PIN_ENC_A);
  uint8_t b = digitalRead(PIN_ENC_B);
  prevAB = (b << 1) | a;

  // Gemeinsamer SPI-Bus
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, TFT_CS);
  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));

  // MAX22530 init
  delay(100);
  writeRegister(REG_CONFIG, 0x80);
  delay(10);
  writeRegister(REG_CONFIG, 0x00);
  writeRegister(REG_CHANNEL, 0x02); // wie gehabt

  // TFT init
  ili9488_init();
  fillScreen(0,0,0);

  applyOutputAndLog("INIT", true);

  // Web-UI initialisieren (WiFi + WebServer + WebSocket)
  webUISetup();
  
  // Daten-Logger initialisieren und Routen registrieren
  dataLoggerInit();
  dataLoggerSetupRoutes();

  /* ====== ACS712-Analog-ADC Initialisierung ====== */
  analogReadResolution(12);
  setAdcAttenuation(ADC_PIN, ADC_ATTENUATION);

  Serial.print(F("ACS712 Offset-Kalibrierung (Samples="));
  Serial.print(STARTUP_CAL_SAMPLES);
  Serial.println(F(")..."));
  zero_mV = calibrateZero_mV(ADC_PIN, STARTUP_CAL_SAMPLES);
  Serial.print(F("Offset (mV) = "));
  Serial.println(zero_mV, 3);

  lastPrintMs = millis();
}

/* ===================== Loop ===================== */
void loop() {
  // ==== WEB-UI (WebSocket + Regelung) ====
  webUILoop();
  
  // ==== DATEN-LOGGER (Spannung/Leistung aufzeichnen) ====
  dataLoggerAddSample();

  // ==== REMOTE-BUTTON (unabhängig vom Rest) ====
  remoteButtonTask();

  /* ==== 1) ENCODER (nur wenn NICHT im Web-Modus) ==== */
  if (!g_webControlMode) {
    uint32_t nowUs = micros();
    if ((nowUs - lastEncSampleUs) >= ENC_SAMPLE_US) {
      lastEncSampleUs = nowUs;
      uint8_t a = digitalRead(PIN_ENC_A);
      uint8_t b = digitalRead(PIN_ENC_B);
      uint8_t currAB = (b << 1) | a;

      uint8_t idx  = (prevAB << 2) | currAB;
      int8_t  step = encLut[idx];

      if (step != 0) {
        encAccum += step;
        if (encAccum >= 2) {
          encAccum = 0;
          g_psuDutyPctFine += DUTY_STEP_PERCENT;  // feiner Schritt nach oben (invertiert)
          applyOutputAndLog("ENC +step", true);
        } else if (encAccum <= -2) {
          encAccum = 0;
          g_psuDutyPctFine -= DUTY_STEP_PERCENT;  // feiner Schritt nach unten (invertiert)
          applyOutputAndLog("ENC -step", true);
        }
      }
      prevAB = currAB;
    }
  }

  /* ==== 2) BUTTON (Encoder-Klick) - nur wenn NICHT im Web-Modus ==== */
  if (!g_webControlMode) {
    static uint32_t lastBtnMs = 0;
    if (millis() - lastBtnMs > 20) {
      lastBtnMs = millis();
      if (digitalRead(PIN_ENC_SW) == LOW) {
        while (digitalRead(PIN_ENC_SW) == LOW) delay(5);
        g_psuDutyPctFine = BTN_PSU_DUTY_PERCENT;
        applyOutputAndLog("BTN->PRESET", false);
      }
    }
  }

  /* ==== 3) SPI-ADC Spannung + ACS712 Strom sammeln (alle 10 ms) ==== */
  static uint32_t lastTickMs = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastTickMs >= 10) {  //aktualisierungsrate
    lastTickMs = nowMs;

    // --- Spannung (SPI-ADC) ---
    uint16_t adcValue = readADC();
    float measuredVoltage = (float)adcValue * 1.8f / 4096.0f; // LSB-Skala wie gehabt
    float volts = measuredVoltage * 139.8668f;                // dein Faktor

    if (vfull) vsum -= vbuf[vidx];
    vbuf[vidx] = volts;
    vsum += volts;

    // --- Strom (ACS712): Blockmittel zur Rauschminderung, dann 10er-Puffer ---
    long blockSum_mV = 0;
    for (int i = 0; i < BLOCK_AVG_SAMPLES; ++i) {
      blockSum_mV += readMilliVolts(ADC_PIN);
      delayMicroseconds(200);
    }
    float v_mV = (float)blockSum_mV / (float)BLOCK_AVG_SAMPLES;
    float inst_A = (v_mV - zero_mV) / SENSITIVITY_mV_PER_A;
    ema_A = (EMA_ALPHA * inst_A) + (1.0f - EMA_ALPHA) * ema_A;

    // 10er-Puffer für I (gleicher Index wie Spannung)
    if (vfull) isum -= ibuf[vidx];
    ibuf[vidx] = ema_A;
    isum += ema_A;

    // Index fortschalten
    vidx++;
    if (vidx >= NUM_SAMPLES) {
      vidx = 0;
      vfull = true;

      // --- Gemeinsame 10er-Mittel berechnen ---
      float vAvg = vsum / (float)NUM_SAMPLES;
      float iAvg = isum / (float)NUM_SAMPLES;
      iAvg *= 0.5f; //dirty fix, um strom nur halb anzuzeigen
      // --- DISPLAY-UPDATE NUR JETZT und NUR BEI SICHTBARER ÄNDERUNG ---
      drawUI_averaged(vAvg, iAvg);
    }
  }

  /* ==== 4) Serielle Kommandos (Offset neu) ==== */
  if (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'z' || c == 'Z') {
      Serial.println(F("Rekalibriere Offset... (bei 0A)"));
      zero_mV = calibrateZero_mV(ADC_PIN, STARTUP_CAL_SAMPLES);
      Serial.print(F("Neuer Offset (mV) = "));
      Serial.println(zero_mV, 3);
    }
  }

  /* ==== 5) Status-Log (optional) ==== */
  static uint32_t lastStatusMs = 0;
  if (millis() - lastStatusMs >= 500) {
    lastStatusMs = millis();
    float dutyPSU = g_psuDutyPctFine; // feiner Wert ins Status-Log
    float dutyESP = g_outputInverted ? (100.0f - dutyPSU) : dutyPSU;
    float dutyESP_clamped = max(0.0f, min(100.0f, dutyESP));
    uint32_t dutyCntOut =
        (uint32_t)lroundf((dutyESP_clamped / 100.0f) * PWM_MAX_DUTY);
    Serial.printf("[STAT] U=%.2f V  I=%.3f A  P=%.1f W  Duty@PSU=%.2f %% (ESP=%.2f %%, counts=%u)\\n",
                  inputVoltage, inputCurrent, inputPower,
                  dutyPSU, dutyESP_clamped, dutyCntOut);
  }
}
