/*
 * ╔══════════════════════════════════════════════════════════╗
 * ║    SMART PARKING v3 — ESP32 + HC-SR04 + NeoPixel         ║
 * ║  2 voies (entrée / sortie), RGB WS2812, dashboard WiFi   ║
 * ╚══════════════════════════════════════════════════════════╝
 *
 * Librairies requises (PlatformIO / Arduino) :
 * - Adafruit NeoPixel
 * - LiquidCrystal I2C
 * - ESP32Servo
 * - WebServer (built-in ESP32)
 */

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WebServer.h>

// ════════════════════════════════════════════════════════════
//  CONFIGURATION
// ════════════════════════════════════════════════════════════

#define MAX_PLACES 4
#define BARRIER_OPEN_MS 3500 // durée ouverture barrière
#define US_DETECT_CM 20      // distance détection véhicule (cm)
#define US_TIMEOUT_US 25000  // timeout ultrason µs

// Pins ultrasoniques
#define TRIG_ENTRY 5
#define ECHO_ENTRY 18
#define TRIG_EXIT 19
#define ECHO_EXIT 23

// Servos
#define PIN_SRV_ENTRY 32
#define PIN_SRV_EXIT 33
#define SERVO_CLOSED 10 // degrés barrière fermée
#define SERVO_OPEN 90   // degrés barrière ouverte

// NeoPixel
#define PIN_RGB_ENTRY 13  // 3 LEDs RGB côté entrée
#define PIN_RGB_EXIT 14   // 3 LEDs RGB côté sortie
#define PIN_RGB_STATUS 27 // 4 LEDs RGB places P1-P4
#define NLED_GATE 3
#define NLED_SPOTS 4

// Autres
#define PIN_BUZZER 26
#define PIN_BTN_RESET 4

// WiFi AP
const char *WIFI_SSID = "SmartParking";
const char *WIFI_PASS = "parking123";

// ════════════════════════════════════════════════════════════
//  COULEURS NeoPixel
// ════════════════════════════════════════════════════════════

#define COL_OFF 0x000000
#define COL_GREEN 0x00FF40
#define COL_RED 0xFF1020
#define COL_ORANGE 0xFF6000
#define COL_BLUE 0x0040FF
#define COL_WHITE 0x404040
#define COL_CYAN 0x00FFFF
#define COL_MAGENTA 0xFF00AA

// ════════════════════════════════════════════════════════════
//  OBJETS
// ════════════════════════════════════════════════════════════

Adafruit_NeoPixel rgbEntry(NLED_GATE, PIN_RGB_ENTRY, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel rgbExit(NLED_GATE, PIN_RGB_EXIT, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel rgbSpots(NLED_SPOTS, PIN_RGB_STATUS, NEO_GRB + NEO_KHZ800);

Servo srvEntry;
Servo srvExit;

LiquidCrystal_I2C lcd(0x27, 16, 2);
WebServer server(80);

// ════════════════════════════════════════════════════════════
//  ÉTAT GLOBAL
// ════════════════════════════════════════════════════════════

int placesLibres = MAX_PLACES;
int totalEntrees = 0;
int totalSorties = 0;

bool barrEntryOpen = false;
bool barrExitOpen = false;
unsigned long timerEntry = 0;
unsigned long timerExit = 0;

bool lastBtnReset = false;

// Animation RGB (non-bloquante)
unsigned long animTimer = 0;
uint8_t animStep = 0;
bool animRunning = false;
bool animIsEntry = false;

// ════════════════════════════════════════════════════════════
//  ULTRASON
// ════════════════════════════════════════════════════════════

long measureCm(uint8_t trig, uint8_t echo)
{
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  long dur = pulseIn(echo, HIGH, US_TIMEOUT_US);
  if (dur == 0)
    return 999; // timeout = rien détecté
  return dur / 58L;
}

// ════════════════════════════════════════════════════════════
//  NEOPIXEL HELPERS
// ════════════════════════════════════════════════════════════

void setGate(Adafruit_NeoPixel &strip, uint32_t col)
{
  for (int i = 0; i < NLED_GATE; i++)
    strip.setPixelColor(i, col);
  strip.show();
}

void setGateAnim(Adafruit_NeoPixel &strip, uint8_t step)
{
  // Effet "balayage" non-bloquant
  for (int i = 0; i < NLED_GATE; i++)
  {
    strip.setPixelColor(i, i == (step % NLED_GATE) ? COL_CYAN : COL_BLUE);
  }
  strip.show();
}

void updateStatusRGB()
{
  int occupees = MAX_PLACES - placesLibres;
  for (int i = 0; i < NLED_SPOTS; i++)
  {
    if (i < occupees)
    {
      rgbSpots.setPixelColor(i, COL_RED); // Place occupée
    }
    else
    {
      rgbSpots.setPixelColor(i, COL_GREEN); // Place libre
    }
  }
  rgbSpots.show();
}

void gateStatusRGB()
{
  // Entrée : vert si places dispo, rouge si plein
  uint32_t entryCol = (placesLibres > 0) ? COL_GREEN : COL_RED;
  setGate(rgbEntry, entryCol);
  // Sortie : toujours bleu (ouvert à la sortie)
  setGate(rgbExit, COL_BLUE);
}

void flashAll(uint32_t col, int times)
{
  for (int t = 0; t < times; t++)
  {
    setGate(rgbEntry, col);
    setGate(rgbExit, col);
    for (int i = 0; i < NLED_SPOTS; i++)
      rgbSpots.setPixelColor(i, col);
    rgbSpots.show();
    delay(120);
    setGate(rgbEntry, COL_OFF);
    setGate(rgbExit, COL_OFF);
    for (int i = 0; i < NLED_SPOTS; i++)
      rgbSpots.setPixelColor(i, COL_OFF);
    rgbSpots.show();
    delay(100);
  }
}

// ════════════════════════════════════════════════════════════
//  LCD
// ════════════════════════════════════════════════════════════

void updateLCD()
{
  lcd.clear();
  // Ligne 0 : places libres
  lcd.setCursor(0, 0);
  lcd.print("Places: ");
  lcd.print(placesLibres);
  lcd.print("/");
  lcd.print(MAX_PLACES);
  lcd.print("       ");

  // Ligne 1 : état
  lcd.setCursor(0, 1);
  if (placesLibres == 0)
  {
    lcd.print("  >> COMPLET << ");
  }
  else if (placesLibres == 1)
  {
    lcd.print(" Presque plein  ");
  }
  else
  {
    lcd.print("  Bienvenue !   ");
  }
}

// ════════════════════════════════════════════════════════════
//  BARRIÈRES
// ════════════════════════════════════════════════════════════

void openBarrierEntry()
{
  srvEntry.write(SERVO_OPEN);
  barrEntryOpen = true;
  timerEntry = millis();
}

void openBarrierExit()
{
  srvExit.write(SERVO_OPEN);
  barrExitOpen = true;
  timerExit = millis();
}

void closeBarrierEntry()
{
  srvEntry.write(SERVO_CLOSED);
  barrEntryOpen = false;
}

void closeBarrierExit()
{
  srvExit.write(SERVO_CLOSED);
  barrExitOpen = false;
}

// ════════════════════════════════════════════════════════════
//  BUZZER (non-bloquant via tone)
// ════════════════════════════════════════════════════════════

void beep(int freq, int dur)
{
  tone(PIN_BUZZER, freq, dur);
}

void beepWelcome()
{
  tone(PIN_BUZZER, 1047, 80);
  delay(100);
  tone(PIN_BUZZER, 1319, 120);
}

void beepFull()
{
  for (int i = 0; i < 3; i++)
  {
    tone(PIN_BUZZER, 600, 100);
    delay(160);
  }
}

void beepExit()
{
  tone(PIN_BUZZER, 880, 100);
}

void beepReset()
{
  tone(PIN_BUZZER, 1200, 80);
  delay(120);
  tone(PIN_BUZZER, 1400, 100);
}

// ════════════════════════════════════════════════════════════
//  BOUTON RESET
// ════════════════════════════════════════════════════════════

void handleReset()
{
  Serial.println("[RESET] Remise à zéro");
  placesLibres = MAX_PLACES;
  closeBarrierEntry();
  closeBarrierExit();
  flashAll(COL_CYAN, 3);
  beepReset();
  updateLCD();
  updateStatusRGB();
  gateStatusRGB();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  RESET OK      ");
  lcd.setCursor(0, 1);
  lcd.print("  Parking libre ");
  delay(1500);
  updateLCD();
}

// ════════════════════════════════════════════════════════════
//  WIFI + DASHBOARD WEB
// ════════════════════════════════════════════════════════════

String buildPage()
{
  int occ = MAX_PLACES - placesLibres;
  float pct = (float)occ / MAX_PLACES * 100.0;
  String c = (placesLibres == 0)   ? "#ff2244"
             : (placesLibres == 1) ? "#ff8800"
                                   : "#00e676";

  String h = R"rawhtml(<!DOCTYPE html><html lang="fr">
<head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="refresh" content="3">
<title>Smart Parking</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Rajdhani:wght@400;600;700&family=Share+Tech+Mono&display=swap');
  *{margin:0;padding:0;box-sizing:border-box}
  body{background:#080c14;color:#e0e8f0;font-family:'Rajdhani',sans-serif;min-height:100vh;
       padding:24px;background-image:radial-gradient(ellipse at 20% 50%,#0d1a2e 0%,#080c14 60%)}
  h1{font-size:2rem;font-weight:700;letter-spacing:4px;text-transform:uppercase;
     color:#00cfff;text-shadow:0 0 20px #00cfff66;margin-bottom:4px}
  .sub{color:#4a6080;font-family:'Share Tech Mono',monospace;font-size:0.78rem;margin-bottom:32px;letter-spacing:2px}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;max-width:680px;margin:0 auto}
  .card{background:#0d1622;border:1px solid #1a2840;border-radius:12px;padding:20px}
  .card.full{grid-column:1/-1}
  .label{font-size:0.7rem;letter-spacing:3px;text-transform:uppercase;color:#4a6080;margin-bottom:8px}
  .big{font-size:3rem;font-weight:700;font-family:'Share Tech Mono',monospace}
  .bar-wrap{background:#0a1020;border-radius:6px;height:10px;overflow:hidden;margin-top:12px}
  .bar-fill{height:100%;border-radius:6px;transition:width .5s}
  .spots{display:flex;gap:10px;margin-top:8px}
  .spot{flex:1;height:44px;border-radius:8px;display:flex;align-items:center;justify-content:center;
        font-weight:700;font-size:0.85rem;letter-spacing:1px}
  .spot.free{background:#003322;border:1px solid #00e67666;color:#00e676}
  .spot.taken{background:#220010;border:1px solid #ff224466;color:#ff2244}
  .stat{font-family:'Share Tech Mono',monospace;font-size:1.6rem;color:#00cfff}
  .dot{width:8px;height:8px;border-radius:50%;display:inline-block;margin-right:6px}
  .status-row{display:flex;align-items:center;margin-top:6px;font-size:0.9rem}
  footer{text-align:center;margin-top:24px;color:#1e3050;font-size:0.7rem;letter-spacing:2px;font-family:'Share Tech Mono',monospace}
</style>
</head><body>
<div style="max-width:680px;margin:0 auto">
<h1>&#x1F17F; Smart Parking</h1>
<div class="sub">ESP32 · HC-SR04 · NEOPIXEL · DASHBOARD LIVE</div>
<div class="grid">
  <div class="card">
    <div class="label">Places libres</div>
    <div class="big" style="color:)rawhtml";
  h += c + R"rawhtml(">)rawhtml";
  h += String(placesLibres);
  h += R"rawhtml( <span style="font-size:1.2rem;color:#4a6080">/ )rawhtml";
  h += String(MAX_PLACES);
  h += R"rawhtml(</span></div>
    <div class="bar-wrap"><div class="bar-fill" style="width:)rawhtml";
  h += String((int)pct);
  h += R"rawhtml(%;background:)rawhtml";
  h += c + R"rawhtml("></div></div>
  </div>
  <div class="card">
    <div class="label">État</div>
    <div class="status-row">
      <span class="dot" style="background:)rawhtml";
  h += c + R"rawhtml(";box-shadow:0 0 8px )rawhtml";
  h += c + R"rawhtml("></span>
      <span style="font-size:1.1rem;font-weight:600">)rawhtml";
  h += (placesLibres == 0) ? "COMPLET" : (placesLibres == 1) ? "PRESQUE PLEIN"
                                                             : "DISPONIBLE";
  h += R"rawhtml(</span></div>
    <div style="margin-top:14px;display:flex;gap:16px">
      <div><div class="label">Entrées</div><div class="stat">)rawhtml";
  h += String(totalEntrees);
  h += R"rawhtml(</div></div>
      <div><div class="label">Sorties</div><div class="stat">)rawhtml";
  h += String(totalSorties);
  h += R"rawhtml(</div></div>
    </div>
  </div>
  <div class="card full">
    <div class="label">Places P1 → P4</div>
    <div class="spots">)rawhtml";

  int occ2 = MAX_PLACES - placesLibres;
  for (int i = 0; i < MAX_PLACES; i++)
  {
    bool taken = i < occ2;
    h += "<div class='spot " + String(taken ? "taken" : "free") + "'>P";
    h += String(i + 1);
    h += taken ? " ●" : " ○";
    h += "</div>";
  }
  h += R"rawhtml(</div>
  </div>
</div>
<footer>AUTO-REFRESH · 3s · SMART PARKING v3</footer>
</div></body></html>)rawhtml";
  return h;
}

void startWifi()
{
  WiFi.begin("Wokwi-GUEST", "");
  Serial.print("[WiFi] Connexion en cours...");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("[WiFi] Connecte ! IP Web : ");
  Serial.println(WiFi.localIP());
}

void setupServer()
{
  server.on("/", []()
            { server.send(200, "text/html", buildPage()); });
  server.begin();
}

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════

void setup()
{
  Serial.begin(115200);

  // Pins ultrason
  pinMode(TRIG_ENTRY, OUTPUT);
  pinMode(ECHO_ENTRY, INPUT);
  pinMode(TRIG_EXIT, OUTPUT);
  pinMode(ECHO_EXIT, INPUT);

  // Bouton
  pinMode(PIN_BTN_RESET, INPUT_PULLDOWN);

  // Servos
  srvEntry.attach(PIN_SRV_ENTRY);
  srvExit.attach(PIN_SRV_EXIT);
  srvEntry.write(SERVO_CLOSED);
  srvExit.write(SERVO_CLOSED);

  // NeoPixels
  rgbEntry.begin();
  rgbEntry.setBrightness(180);
  rgbEntry.clear();
  rgbEntry.show();
  rgbExit.begin();
  rgbExit.setBrightness(180);
  rgbExit.clear();
  rgbExit.show();
  rgbSpots.begin();
  rgbSpots.setBrightness(160);
  rgbSpots.clear();
  rgbSpots.show();

  // LCD
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  SMART PARKING ");
  lcd.setCursor(0, 1);
  lcd.print("   Demarrage... ");

  // Séquence de démarrage LED
  flashAll(COL_CYAN, 2);
  delay(300);
  updateStatusRGB();
  gateStatusRGB();
  beep(1000, 150);

  // WiFi + server
  startWifi();
  setupServer();

  updateLCD();
  Serial.println("[SYSTEM] Parking prêt !");
}

// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════

void loop()
{
  unsigned long now = millis();

  // ── Mesures ultrason ──────────────────────────────────────
  long distEntry = measureCm(TRIG_ENTRY, ECHO_ENTRY);
  long distExit = measureCm(TRIG_EXIT, ECHO_EXIT);

  bool vehicleAtEntry = (distEntry < US_DETECT_CM);
  bool vehicleAtExit = (distExit < US_DETECT_CM);

  // ── Voie ENTRÉE ───────────────────────────────────────────
  static bool prevEntry = false;
  if (vehicleAtEntry && !prevEntry)
  {
    // Front montant : véhicule vient d'arriver
    if (placesLibres > 0)
    {
      placesLibres--;
      totalEntrees++;
      openBarrierEntry();
      beepWelcome();
      // LEDs entrée : flash vert
      setGate(rgbEntry, COL_GREEN);
      Serial.printf("[ENTREE] dist=%ldcm  libres=%d\n", distEntry, placesLibres);
    }
    else
    {
      // Plein : LED rouge + bips
      setGate(rgbEntry, COL_RED);
      beepFull();
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(" PARKING COMPLET");
      lcd.setCursor(0, 1);
      lcd.print("  Acces refuse  ");
      delay(2000);
      Serial.println("[ENTREE] Refus — parking complet");
    }
    updateLCD();
    updateStatusRGB();
    gateStatusRGB();
  }
  prevEntry = vehicleAtEntry;

  // ── Voie SORTIE ───────────────────────────────────────────
  static bool prevExit = false;
  if (vehicleAtExit && !prevExit)
  {
    if (placesLibres < MAX_PLACES)
    {
      placesLibres++;
      totalSorties++;
      openBarrierExit();
      beepExit();
      setGate(rgbExit, COL_CYAN);
      Serial.printf("[SORTIE] dist=%ldcm  libres=%d\n", distExit, placesLibres);
      updateLCD();
      updateStatusRGB();
      gateStatusRGB();
    }
  }
  prevExit = vehicleAtExit;

  // ── Fermeture automatique des barrières ───────────────────
  if (barrEntryOpen && now - timerEntry > BARRIER_OPEN_MS)
  {
    closeBarrierEntry();
    gateStatusRGB(); // Remet la couleur d'état
    Serial.println("[BARRIERE] Entree fermee");
  }
  if (barrExitOpen && now - timerExit > BARRIER_OPEN_MS)
  {
    closeBarrierExit();
    gateStatusRGB();
    Serial.println("[BARRIERE] Sortie fermee");
  }

  // ── Bouton reset ──────────────────────────────────────────
  bool btnState = digitalRead(PIN_BTN_RESET);
  if (btnState && !lastBtnReset)
  {
    handleReset();
  }
  lastBtnReset = btnState;

  // ── Animation douce RGB places (clignotement si 1 place) ─
  if (placesLibres == 1 && now - animTimer > 700)
  {
    animTimer = now;
    animStep ^= 1;
    // Fait clignoter la dernière place libre
    int idxLibre = MAX_PLACES - 1;
    rgbSpots.setPixelColor(idxLibre, animStep ? COL_ORANGE : COL_GREEN);
    rgbSpots.show();
  }

  server.handleClient();
  delay(50); // ~20 Hz suffisant pour ultrason
}