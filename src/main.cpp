/*
 * ╔══════════════════════════════════════════════════════════╗
 * ║    SMART PARKING v4.2 — ESP32 + HC-SR04 + NeoPixel       ║
 * ║  Correction Timers Servos & Mise à jour LEDs temps réel  ║
 * ╚══════════════════════════════════════════════════════════╝
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
#define BARRIER_OPEN_MS 3500
#define US_DETECT_CM 20
#define US_TIMEOUT_US 15000 // Optimisation du timeout pour éviter les lags

#define TRIG_ENTRY 5
#define ECHO_ENTRY 18
#define TRIG_EXIT 19
#define ECHO_EXIT 23

#define PIN_SRV_ENTRY 32
#define PIN_SRV_EXIT 33
#define SERVO_CLOSED 0
#define SERVO_OPEN 90

#define PIN_RGB_ENTRY 13
#define PIN_RGB_EXIT 14
#define PIN_RGB_STATUS 27
#define NLED_GATE 3
#define NLED_SPOTS 4

#define PIN_BUZZER 26
#define BUZZER_CHANNEL 0 // Canal LEDC dédié au buzzer
#define PIN_BTN_RESET 4

const char *WIFI_SSID = "SmartParking";
const char *WIFI_PASS = "parking123";

// ════════════════════════════════════════════════════════════
//  COULEURS
// ════════════════════════════════════════════════════════════

#define COL_OFF 0x000000
#define COL_GREEN 0x00E040
#define COL_RED 0xFF1020
#define COL_ORANGE 0xFF5500
#define COL_BLUE 0x0055FF
#define COL_CYAN 0x00CCFF
#define COL_WHITE 0x303030
#define COL_AMBER 0xFF8800
#define COL_MINT 0x00FF88

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
//  ETAT GLOBAL
// ════════════════════════════════════════════════════════════

int placesLibres = MAX_PLACES;
int totalEntrees = 0;
int totalSorties = 0;

bool barrEntryOpen = false;
bool barrExitOpen = false;
unsigned long timerEntry = 0;
unsigned long timerExit = 0;

bool lastBtnReset = false;

// ════════════════════════════════════════════════════════════
//  BUZZER NON-BLOQUANT (Remplacé par LEDC pour protéger les Servos)
// ════════════════════════════════════════════════════════════

void sysTone(uint16_t freq)
{
  ledcWriteTone(BUZZER_CHANNEL, freq);
}
void sysNoTone()
{
  ledcWrite(BUZZER_CHANNEL, 0);
}

struct BuzzNote
{
  uint16_t freq;
  uint16_t dur;
  uint16_t gap;
};

#define MAX_NOTES 6
BuzzNote buzzQueue[MAX_NOTES];
uint8_t buzzLen = 0;
uint8_t buzzIdx = 0;
bool buzzActive = false;
bool buzzInGap = false;
unsigned long buzzTimer = 0;

void buzzPlay(BuzzNote *notes, uint8_t n)
{
  if (n > MAX_NOTES)
    n = MAX_NOTES;
  memcpy(buzzQueue, notes, n * sizeof(BuzzNote));
  buzzLen = n;
  buzzIdx = 0;
  buzzActive = true;
  buzzInGap = false;
  sysNoTone();
  sysTone(buzzQueue[0].freq);
  buzzTimer = millis();
}

void buzzTick()
{
  if (!buzzActive)
    return;
  unsigned long now = millis();
  BuzzNote &cur = buzzQueue[buzzIdx];

  if (!buzzInGap && now - buzzTimer >= cur.dur)
  {
    sysNoTone();
    buzzInGap = true;
  }
  if (buzzInGap && now - buzzTimer >= (unsigned long)(cur.dur + cur.gap))
  {
    buzzIdx++;
    if (buzzIdx >= buzzLen)
    {
      buzzActive = false;
      return;
    }
    buzzInGap = false;
    buzzTimer = millis();
    sysTone(buzzQueue[buzzIdx].freq);
  }
}

void beepWelcome()
{
  static BuzzNote s[] = {{1047, 70, 50}, {1319, 90, 0}};
  buzzPlay(s, 2);
}
void beepFull()
{
  static BuzzNote s[] = {{500, 80, 80}, {500, 80, 80}, {500, 80, 0}};
  buzzPlay(s, 3);
}
void beepExit()
{
  static BuzzNote s[] = {{880, 70, 0}};
  buzzPlay(s, 1);
}
void beepReset()
{
  static BuzzNote s[] = {{1200, 60, 50}, {1400, 80, 0}};
  buzzPlay(s, 2);
}

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
  return dur == 0 ? 999 : dur / 58L;
}

// ════════════════════════════════════════════════════════════
//  NEOPIXEL HELPERS & ANIMATIONS
// ════════════════════════════════════════════════════════════

uint8_t gamma8(uint8_t v) { return (uint8_t)((v * v) >> 8); }

void setGate(Adafruit_NeoPixel &strip, uint32_t col)
{
  for (int i = 0; i < NLED_GATE; i++)
    strip.setPixelColor(i, col);
  strip.show();
}

unsigned long breathTimer = 0;
float breathPhase = 0.0f;

void tickBreath(unsigned long now)
{
  if (now - breathTimer < 16)
    return;
  breathTimer = now;
  breathPhase += 0.06f;
  if (breathPhase > 6.2832f)
    breathPhase -= 6.2832f;

  float bri = 0.5f + 0.5f * sinf(breathPhase);
  int occupees = MAX_PLACES - placesLibres;

  // Mise à jour temps réel des places
  for (int i = 0; i < NLED_SPOTS; i++)
  {
    if (i < occupees)
    {
      rgbSpots.setPixelColor(i, COL_RED); // DEVIENT ROUGE IMMÉDIATEMENT
    }
    else if (i == occupees && placesLibres == 1)
    {
      uint8_t lv = (uint8_t)(bri * 210);
      rgbSpots.setPixelColor(i, rgbSpots.Color(lv, lv >> 2, 0));
    }
    else
    {
      uint8_t lv = (uint8_t)(170 + bri * 50);
      rgbSpots.setPixelColor(i, rgbSpots.Color(0, gamma8(lv), gamma8(lv >> 3)));
    }
  }
  rgbSpots.show();
}

struct ChaseState
{
  bool active = false;
  uint8_t step = 0;
  bool isEntry = false;
  unsigned long timer = 0;
};
ChaseState chaseEntry, chaseExit;

void startChase(ChaseState &cs, bool entry)
{
  cs.active = true;
  cs.isEntry = entry;
  cs.step = 0;
  cs.timer = millis();
}

void tickChase(ChaseState &cs, unsigned long now)
{
  if (!cs.active || now - cs.timer < 80)
    return;
  cs.timer = now;
  Adafruit_NeoPixel &strip = cs.isEntry ? rgbEntry : rgbExit;
  uint32_t head = cs.isEntry ? COL_CYAN : COL_MINT;
  uint32_t tail = cs.isEntry ? 0x003040 : 0x003020;

  for (int i = 0; i < NLED_GATE; i++)
  {
    int pos = cs.step % NLED_GATE;
    if (i == pos)
      strip.setPixelColor(i, head);
    else if (i == (pos - 1 + NLED_GATE) % NLED_GATE)
      strip.setPixelColor(i, tail);
    else
      strip.setPixelColor(i, COL_OFF);
  }
  strip.show();
  cs.step++;
}

struct FlashState
{
  bool active = false;
  uint8_t count = 0, total = 0;
  uint32_t col = COL_CYAN;
  bool phase = false;
  unsigned long timer = 0;
};
FlashState flashSt;

void startFlash(uint32_t col, uint8_t times)
{
  flashSt.active = true;
  flashSt.count = 0;
  flashSt.total = times;
  flashSt.col = col;
  flashSt.phase = true;
  flashSt.timer = millis();
  setGate(rgbEntry, col);
  setGate(rgbExit, col);
  for (int i = 0; i < NLED_SPOTS; i++)
    rgbSpots.setPixelColor(i, col);
  rgbSpots.show();
}

void tickFlash(unsigned long now)
{
  if (!flashSt.active)
    return;
  uint16_t dur = flashSt.phase ? 130 : 90;
  if (now - flashSt.timer < dur)
    return;

  flashSt.timer = now;
  flashSt.phase = !flashSt.phase;
  uint32_t c = flashSt.phase ? flashSt.col : COL_OFF;

  setGate(rgbEntry, c);
  setGate(rgbExit, c);
  for (int i = 0; i < NLED_SPOTS; i++)
    rgbSpots.setPixelColor(i, c);
  rgbSpots.show();

  if (!flashSt.phase && ++flashSt.count >= flashSt.total)
    flashSt.active = false;
}

void gateStatusRGB()
{
  if (!chaseEntry.active)
    setGate(rgbEntry, (placesLibres > 0) ? COL_GREEN : COL_RED);
  if (!chaseExit.active)
    setGate(rgbExit, COL_BLUE);
}

// ── Fonction d'attente non-bloquante ───────────────────────
void waitSmooth(unsigned long ms)
{
  unsigned long start = millis();
  while (millis() - start < ms)
  {
    unsigned long now = millis();
    buzzTick();
    tickFlash(now);
    tickChase(chaseEntry, now);
    tickChase(chaseExit, now);
    if (!flashSt.active)
      tickBreath(now);
    delay(15);
  }
}

// ════════════════════════════════════════════════════════════
//  LCD & BARRIERES
// ════════════════════════════════════════════════════════════

void updateLCD()
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Places: ");
  lcd.print(placesLibres);
  lcd.print("/");
  lcd.print(MAX_PLACES);
  lcd.setCursor(0, 1);
  if (placesLibres == 0)
    lcd.print("  >> COMPLET << ");
  else if (placesLibres == 1)
    lcd.print(" Presque plein  ");
  else
    lcd.print("  Bienvenue !   ");
}

void openBarrierEntry()
{
  srvEntry.write(SERVO_OPEN);
  barrEntryOpen = true;
  timerEntry = millis();
  startChase(chaseEntry, true);
}
void openBarrierExit()
{
  srvExit.write(SERVO_OPEN);
  barrExitOpen = true;
  timerExit = millis();
  startChase(chaseExit, false);
}
void closeBarrierEntry()
{
  srvEntry.write(SERVO_CLOSED);
  barrEntryOpen = false;
  chaseEntry.active = false;
  gateStatusRGB();
}
void closeBarrierExit()
{
  srvExit.write(SERVO_CLOSED);
  barrExitOpen = false;
  chaseExit.active = false;
  gateStatusRGB();
}

void handleReset()
{
  Serial.println("[RESET] Remise a zero");
  placesLibres = MAX_PLACES;
  closeBarrierEntry();
  closeBarrierExit();
  beepReset();
  startFlash(COL_CYAN, 3);
  updateLCD();
}

// ════════════════════════════════════════════════════════════
//  DASHBOARD WEB (Inchangé)
// ════════════════════════════════════════════════════════════

String buildPage()
{
  int occ = MAX_PLACES - placesLibres;
  float pct = (float)occ / MAX_PLACES * 100.0f;
  String c = (placesLibres == 0)   ? "#ff2244"
             : (placesLibres == 1) ? "#ff8800"
                                   : "#00e676";
  String h = R"rawhtml(<!DOCTYPE html><html lang="fr">
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="3"><title>Smart Parking v4</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Rajdhani:wght@400;600;700&family=Share+Tech+Mono&display=swap');
  *{margin:0;padding:0;box-sizing:border-box}
  body{background:#080c14;color:#e0e8f0;font-family:'Rajdhani',sans-serif;min-height:100vh;padding:24px;
       background-image:radial-gradient(ellipse at 20% 50%,#0d1a2e 0%,#080c14 60%)}
  h1{font-size:1.9rem;font-weight:700;letter-spacing:4px;text-transform:uppercase;
     color:#00cfff;text-shadow:0 0 18px #00cfff55;margin-bottom:4px}
  .sub{color:#3a5070;font-family:'Share Tech Mono',monospace;font-size:.75rem;margin-bottom:28px;letter-spacing:2px}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:14px;max-width:680px;margin:0 auto}
  .card{background:#0d1622;border:1px solid #1a2840;border-radius:12px;padding:18px}
  .card.full{grid-column:1/-1}
  .label{font-size:.68rem;letter-spacing:3px;text-transform:uppercase;color:#3a5070;margin-bottom:8px}
  .big{font-size:2.8rem;font-weight:700;font-family:'Share Tech Mono',monospace;line-height:1}
  .bar-wrap{background:#0a1020;border-radius:6px;height:8px;overflow:hidden;margin-top:14px}
  .bar-fill{height:100%;border-radius:6px;transition:width .6s cubic-bezier(.4,0,.2,1)}
  .spots{display:flex;gap:10px;margin-top:10px}
  .spot{flex:1;height:48px;border-radius:10px;display:flex;align-items:center;justify-content:center;
        font-weight:700;font-size:.85rem;letter-spacing:1px;transition:background .4s}
  .spot.free{background:#003322;border:1px solid #00e67644;color:#00e676}
  .spot.taken{background:#220010;border:1px solid #ff224444;color:#ff2244}
  .stat{font-family:'Share Tech Mono',monospace;font-size:1.5rem;color:#00cfff}
  .dot{width:7px;height:7px;border-radius:50%;display:inline-block;margin-right:6px}
  .status-row{display:flex;align-items:center;margin-top:6px;font-size:1rem}
  footer{text-align:center;margin-top:22px;color:#182840;font-size:.68rem;letter-spacing:2px;
         font-family:'Share Tech Mono',monospace}
</style></head><body>
<div style="max-width:680px;margin:0 auto">
<h1>&#x1F17F; Smart Parking</h1>
<div class="sub">ESP32 &middot; HC-SR04 &middot; NEOPIXEL &middot; LIVE v4</div>
<div class="grid">
  <div class="card">
    <div class="label">Places libres</div>
    <div class="big" style="color:)rawhtml";
  h += c + "\">" + String(placesLibres);
  h += "<span style=\"font-size:1.1rem;color:#3a5070\"> / " + String(MAX_PLACES) + "</span></div>";
  h += "<div class=\"bar-wrap\"><div class=\"bar-fill\" style=\"width:" + String((int)pct) + "%;background:" + c + "\"></div></div></div>";
  h += R"rawhtml(
  <div class="card">
    <div class="label">Etat</div>
    <div class="status-row"><span class="dot" style="background:)rawhtml";
  h += c + ";box-shadow:0 0 6px " + c + "\"></span>";
  h += "<span style=\"font-size:1.05rem;font-weight:600\">";
  h += (placesLibres == 0) ? "COMPLET" : (placesLibres == 1) ? "PRESQUE PLEIN"
                                                             : "DISPONIBLE";
  h += "</span></div><div style=\"margin-top:14px;display:flex;gap:20px\">";
  h += "<div><div class=\"label\">Entrees</div><div class=\"stat\">" + String(totalEntrees) + "</div></div>";
  h += "<div><div class=\"label\">Sorties</div><div class=\"stat\">" + String(totalSorties) + "</div></div>";
  h += R"rawhtml(</div></div>
  <div class="card full">
    <div class="label">Places P1 &rarr; P4</div>
    <div class="spots">)rawhtml";
  int occ2 = MAX_PLACES - placesLibres;
  for (int i = 0; i < MAX_PLACES; i++)
  {
    bool taken = i < occ2;
    h += "<div class='spot " + String(taken ? "taken" : "free") + "'>P";
    h += String(i + 1) + (taken ? " &#9679;" : " &#9675;") + "</div>";
  }
  h += R"rawhtml(</div></div>
</div>
<footer>AUTO-REFRESH 3s &middot; SMART PARKING v4</footer>
</div></body></html>)rawhtml";
  return h;
}

void startWifi()
{
  WiFi.begin("Wokwi-GUEST", "");
  Serial.print("[WiFi] Connexion...");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000)
  {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.println("\n[WiFi] IP: " + WiFi.localIP().toString());
  else
    Serial.println("\n[WiFi] TIMEOUT");
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

  // Initialisation Buzzer LEDC (Évite le conflit matériel avec les Servos)
  ledcSetup(BUZZER_CHANNEL, 2000, 8);
  ledcAttachPin(PIN_BUZZER, BUZZER_CHANNEL);

  // Allocation de Timers indépendants pour les Servomoteurs
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  pinMode(TRIG_ENTRY, OUTPUT);
  pinMode(ECHO_ENTRY, INPUT);
  pinMode(TRIG_EXIT, OUTPUT);
  pinMode(ECHO_EXIT, INPUT);
  pinMode(PIN_BTN_RESET, INPUT_PULLDOWN);

  srvEntry.setPeriodHertz(50);
  srvEntry.attach(PIN_SRV_ENTRY, 500, 2400);
  srvEntry.write(SERVO_CLOSED);

  srvExit.setPeriodHertz(50);
  srvExit.attach(PIN_SRV_EXIT, 500, 2400);
  srvExit.write(SERVO_CLOSED);

  rgbEntry.begin();
  rgbEntry.setBrightness(160);
  rgbEntry.clear();
  rgbEntry.show();
  rgbExit.begin();
  rgbExit.setBrightness(160);
  rgbExit.clear();
  rgbExit.show();
  rgbSpots.begin();
  rgbSpots.setBrightness(140);
  rgbSpots.clear();
  rgbSpots.show();

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  SMART PARKING ");
  lcd.setCursor(0, 1);
  lcd.print("   Demarrage... ");

  startFlash(COL_CYAN, 2);
  startWifi();
  setupServer();

  updateLCD();
  gateStatusRGB();
  Serial.println("[SYSTEM] Parking pret !");
}

// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════

void loop()
{
  unsigned long now = millis();

  // Animations non-bloquantes (Maintenant séparées et indépendantes !)
  buzzTick();
  tickFlash(now);
  tickChase(chaseEntry, now);
  tickChase(chaseExit, now);

  if (!flashSt.active)
  {
    tickBreath(now); // Les places sont mises à jour en permanence
  }

  // Mesures ultrason
  long distEntry = measureCm(TRIG_ENTRY, ECHO_ENTRY);
  long distExit = measureCm(TRIG_EXIT, ECHO_EXIT);

  // Sécurisation de la mesure : on ignore les 0 accidentels du capteur
  bool vehicleEntry = (distEntry > 0 && distEntry < US_DETECT_CM);
  bool vehicleExit = (distExit > 0 && distExit < US_DETECT_CM);

  // ── VOIE ENTREE ──
  static bool prevEntry = false;
  if (vehicleEntry)
  {
    if (barrEntryOpen)
    {
      timerEntry = now; // Maintien d'ouverture
    }
    if (!prevEntry)
    {
      if (placesLibres > 0)
      {
        placesLibres--;
        totalEntrees++;
        openBarrierEntry();
        beepWelcome();
        Serial.printf("[ENTREE] dist=%ldcm libres=%d\n", distEntry, placesLibres);
        updateLCD();
        gateStatusRGB();
      }
      else
      {
        setGate(rgbEntry, COL_RED);
        beepFull();
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(" PARKING COMPLET");
        lcd.setCursor(0, 1);
        lcd.print("  Acces refuse  ");
        Serial.println("[ENTREE] Refuse — complet");

        waitSmooth(1800);

        updateLCD();
        gateStatusRGB();
      }
    }
  }
  prevEntry = vehicleEntry;

  // ── VOIE SORTIE ──
  static bool prevExit = false;
  if (vehicleExit)
  {
    if (barrExitOpen)
    {
      timerExit = now; // Maintien d'ouverture
    }
    if (!prevExit)
    {
      if (placesLibres < MAX_PLACES)
      {
        placesLibres++;
        totalSorties++;
        openBarrierExit();
        beepExit();
        Serial.printf("[SORTIE] dist=%ldcm libres=%d\n", distExit, placesLibres);
        updateLCD();
        gateStatusRGB();
      }
    }
  }
  prevExit = vehicleExit;

  // ── FERMETURE AUTO ──
  if (barrEntryOpen && !vehicleEntry && now - timerEntry > BARRIER_OPEN_MS)
  {
    closeBarrierEntry();
  }
  if (barrExitOpen && !vehicleExit && now - timerExit > BARRIER_OPEN_MS)
  {
    closeBarrierExit();
  }

  // ── BOUTON RESET ──
  bool btn = digitalRead(PIN_BTN_RESET);
  if (btn && !lastBtnReset)
    handleReset();
  lastBtnReset = btn;

  server.handleClient();
  delay(20);
}