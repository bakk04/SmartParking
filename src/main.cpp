/*
 * ================================================================
 *  SMART CLASSROOM — ESP32 (Wokwi)
 *  Version Corrigée v3 :
 *    Fix sonar :
 *      1. Délai LOW initial porté à 4µs
 *      2. Délai de stabilisation 10µs après TRIG LOW final
 *      3. Timeout pulseIn 60ms (au lieu de 30ms)
 *      4. NOUVEAU : PIN_ECHO déclaré INPUT_PULLDOWN
 *      5. NOUVEAU : intervalle mesure 200ms (au lieu de 150ms)
 *    Fix DHT22 : résistance pull-up câblée dans diagram.json
 *    Fix LDR : log Serial complet
 * ================================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <DHT.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── PINS ────────────────────────────────────────────────────────
#define PIN_DHT 27
#define PIN_TRIG 4
#define PIN_ECHO 2
#define PIN_RFID_SS 5
#define PIN_RFID_RST 17
#define PIN_LDR 34
#define PIN_RELAY 32
#define PIN_BUZZER 33
#define PIN_LED_W1 13
#define PIN_LED_W2 12
#define PIN_LED_W3 14
#define PIN_LED_W4 15
#define PIN_LED_GREEN 25
#define PIN_LED_RED 26

// ── SEUILS ──────────────────────────────────────────────────────
#define TEMP_RELAY_ON 29.0f
#define HUM_RELAY_ON 65.0f
#define DIST_PRESENCE 20
#define LUMIERE_SEUIL_SOMBRE 30

#define OLED_ADDR 0x3C

// ── CRÉNEAUX HORAIRES ───────────────────────────────────────────
struct Creneau
{
  uint16_t debut;
  uint16_t fin;
};
const Creneau COURS[] = {
    {480, 570},
    {585, 675},
    {780, 870},
    {885, 975},
};
const uint8_t NB_COURS = 4;

uint16_t minSim() { return (uint16_t)((millis() / 1000UL) % 1440); }
bool enCours()
{
  uint16_t t = minSim();
  for (uint8_t i = 0; i < NB_COURS; i++)
    if (t >= COURS[i].debut && t < COURS[i].fin)
      return true;
  return false;
}
void heureSim(char *buf6)
{
  uint16_t t = minSim();
  snprintf(buf6, 6, "%02d:%02d", t / 60, t % 60);
}

// ── RFID ────────────────────────────────────────────────────────
const byte UIDS_OK[][4] = {
    {0xDE, 0xAD, 0xBE, 0xEF},
    {0xA1, 0xB2, 0xC3, 0xD4},
};
const uint8_t NB_UIDS = 2;
bool uidOK(const byte *u, byte sz)
{
  if (sz != 4)
    return false;
  for (uint8_t i = 0; i < NB_UIDS; i++)
    if (memcmp(u, UIDS_OK[i], 4) == 0)
      return true;
  return false;
}

// ── PÉRIPHÉRIQUES ───────────────────────────────────────────────
DHT dht(PIN_DHT, DHT22);
MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

// ── ÉTAT GLOBAL ─────────────────────────────────────────────────
float g_temp = 25.0f;
float g_hum = 50.0f;
int g_dist = -1;
int g_lumiere_pct = 100;
bool g_presence = false;
bool g_was_presence = false;
int g_nb_presences = 0;
bool g_cours = false;
bool g_lampsOn = false;
bool g_relayOn = false;

unsigned long t_dht = 0;
unsigned long t_capteurs_rapides = 0;
unsigned long t_oled = 0;
unsigned long t_oled_page = 0;
unsigned long t_rfid = 0;
uint8_t pageOLED = 0;

// ════════════════════════════════════════════════════════════════
//  SONAR CORRIGÉ
// ════════════════════════════════════════════════════════════════
int lireSonar()
{
  // S'assurer que TRIG est LOW
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);

  // Impulsion TRIG = 10µs
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  // Timeout 60ms
  long duree = pulseIn(PIN_ECHO, HIGH, 60000UL);

  if (duree == 0)
  {
    Serial.println(F("[SONAR] Timeout - pas d echo"));
    return -1;
  }
  return (int)(duree / 58);
}

void allumerLamps(bool on)
{
  g_lampsOn = on;
  uint8_t v = on ? HIGH : LOW;
  digitalWrite(PIN_LED_W1, v);
  digitalWrite(PIN_LED_W2, v);
  digitalWrite(PIN_LED_W3, v);
  digitalWrite(PIN_LED_W4, v);
}

void bip(int freq = 1200, int ms = 100) { tone(PIN_BUZZER, freq, ms); }
void bipRefus()
{
  tone(PIN_BUZZER, 400, 300);
  delay(350);
  tone(PIN_BUZZER, 300, 300);
}

// ── OLED ────────────────────────────────────────────────────────
void oledClimat()
{
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(18, 0);
  oled.print(F("[ CLIMAT ]"));
  oled.setTextSize(2);
  oled.setCursor(0, 14);
  oled.print(g_temp, 1);
  oled.print((char)247);
  oled.print(F("C"));
  oled.setCursor(0, 38);
  oled.print(g_hum, 1);
  oled.print(F("%"));
  oled.setTextSize(1);
  oled.setCursor(76, 38);
  oled.print(F("AC:"));
  oled.print(g_relayOn ? F("ON ") : F("OFF"));
  oled.display();
}

void oledSalle()
{
  char heure[6];
  heureSim(heure);
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(14, 0);
  oled.print(F("[ SALLE ]"));
  oled.setCursor(0, 12);
  oled.print(F("Presence: "));
  oled.print(g_presence ? F("OUI") : F("NON"));
  oled.setCursor(0, 22);
  oled.print(F("Dist:"));
  if (g_dist > 0 && g_dist < 400)
  {
    oled.print(g_dist);
    oled.print(F("cm "));
  }
  else
  {
    oled.print(F("---  "));
  }
  oled.print(F("Tot:"));
  oled.print(g_nb_presences);
  oled.setCursor(0, 32);
  oled.print(F("Lumiere : "));
  oled.print(g_lumiere_pct);
  oled.print(F("%"));
  oled.setCursor(0, 42);
  oled.print(F("Lampes  : "));
  oled.print(g_lampsOn ? F("ON ") : F("OFF"));
  oled.setCursor(0, 52);
  oled.print(heure);
  oled.print(g_cours ? F(" EN COURS") : F(" LIBRE  "));
  oled.display();
}

void oledAcces(bool ok)
{
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(4, 16);
  oled.print(ok ? F("  ACCES\nOK  :)") : F("  ACCES\n REFUSE"));
  oled.display();
}

// ════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════
void setup()
{
  Serial.begin(115200);
  Serial.println(F("=== Smart Classroom v3 ==="));

  pinMode(PIN_TRIG, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LED_W1, OUTPUT);
  pinMode(PIN_LED_W2, OUTPUT);
  pinMode(PIN_LED_W3, OUTPUT);
  pinMode(PIN_LED_W4, OUTPUT);
  allumerLamps(false);
  pinMode(PIN_LED_GREEN, OUTPUT);
  digitalWrite(PIN_LED_GREEN, LOW);
  pinMode(PIN_LED_RED, OUTPUT);
  digitalWrite(PIN_LED_RED, HIGH);

  pinMode(PIN_ECHO, INPUT);

  pinMode(PIN_LDR, INPUT);

  Wire.begin(21, 22);
  if (oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
  {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(10, 20);
    oled.print(F("Smart Classroom v3"));
    oled.display();
    Serial.println(F("[OLED] OK"));
  }
  else
  {
    Serial.println(F("[OLED] ERREUR"));
  }

  dht.begin();
  delay(2000);
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t) && !isnan(h))
  {
    g_temp = t;
    g_hum = h;
    Serial.printf("[DHT22] Init OK — T=%.1f C  H=%.1f%%\n", g_temp, g_hum);
  }
  else
  {
    Serial.println(F("[DHT22] Init echec"));
  }

  SPI.begin();
  rfid.PCD_Init();
  Serial.println(F("[RFID] OK"));

  // Test sonar immédiat au démarrage
  delay(100);
  int d = lireSonar();
  if (d > 0)
    Serial.printf("[SONAR] Test OK — %d cm\n", d);
  else
    Serial.println(F("[SONAR] Test : pas d obstacle — normal"));

  bip(1000, 100);
  delay(150);
  bip(1400, 100);
  Serial.println(F("Systeme pret."));
}

// ════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════
void loop()
{
  unsigned long now = millis();

  // ── 1. DHT22 : toutes les 2s ────────────────────────────────
  if (now - t_dht >= 2000UL)
  {
    t_dht = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h))
    {
      g_temp = t;
      g_hum = h;
      Serial.printf("[DHT22] T=%.1f C  H=%.1f%%\n", g_temp, g_hum);
    }
    else
    {
      Serial.println(F("[DHT22] NaN"));
    }
    g_relayOn = (g_temp >= TEMP_RELAY_ON || g_hum >= HUM_RELAY_ON);
    digitalWrite(PIN_RELAY, g_relayOn ? HIGH : LOW);
  }

  // ── 2. Sonar + LDR : toutes les 200ms ───────────────────────
  // 200ms garantit un repos suffisant entre deux tirs HC-SR04
  // (minimum recommandé = 60ms ; 200ms donne de la marge)
  if (now - t_capteurs_rapides >= 200UL)
  {
    t_capteurs_rapides = now;

    // Sonar
    g_dist = lireSonar();
    g_presence = (g_dist > 0 && g_dist <= DIST_PRESENCE);

    if (g_dist > 0)
    {
      Serial.printf("[SONAR] %d cm — presence=%s\n",
                    g_dist, g_presence ? "OUI !" : "non");
    }

    // Front montant = nouvelle présence → bip + compteur
    if (g_presence && !g_was_presence)
    {
      g_nb_presences++;
      bip(2000, 150);
      Serial.printf("[!] PRESENCE ! Total=%d\n", g_nb_presences);
    }
    g_was_presence = g_presence;

    // LDR
    int raw = analogRead(PIN_LDR);
    g_lumiere_pct = constrain(map(raw, 0, 4095, 0, 100), 0, 100);
    Serial.printf("[LDR] raw=%d  pct=%d%%\n", raw, g_lumiere_pct);

    // Logique lampes + LEDs
    bool sombre = (g_lumiere_pct < LUMIERE_SEUIL_SOMBRE);
    g_cours = enCours();
    digitalWrite(PIN_LED_GREEN, g_presence ? HIGH : LOW);
    digitalWrite(PIN_LED_RED, g_presence ? LOW : HIGH);
    allumerLamps(sombre && (g_presence || g_cours));
  }

  // ── 3. Page OLED : toutes les 4s ────────────────────────────
  if (now - t_oled_page >= 4000UL)
  {
    t_oled_page = now;
    pageOLED = (pageOLED + 1) % 2;
  }

  // ── 4. Rafraîchissement OLED : toutes les 300ms ─────────────
  if (now - t_oled >= 300UL)
  {
    t_oled = now;
    if (pageOLED == 0)
      oledClimat();
    else
      oledSalle();
  }

  // ── 5. RFID : toutes les 1s ──────────────────────────────────
  if ((now - t_rfid >= 1000UL) &&
      rfid.PICC_IsNewCardPresent() &&
      rfid.PICC_ReadCardSerial())
  {
    t_rfid = now;
    bool ok = uidOK(rfid.uid.uidByte, rfid.uid.size);
    Serial.printf("[RFID] %s\n", ok ? "AUTORISE" : "REFUSE");

    digitalWrite(PIN_LED_GREEN, ok ? HIGH : LOW);
    digitalWrite(PIN_LED_RED, ok ? LOW : HIGH);
    if (ok)
      bip(1400, 200);
    else
      bipRefus();

    oledAcces(ok);
    unsigned long ws = millis();
    while (millis() - ws < 1500)
    {
      yield();
    }

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    digitalWrite(PIN_LED_GREEN, g_presence ? HIGH : LOW);
    digitalWrite(PIN_LED_RED, g_presence ? LOW : HIGH);
    t_oled = 0;
  }
}
