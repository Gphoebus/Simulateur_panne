// =====================================================================
//  SIMULATEUR DE PANNES BLE - ESP32-C3 SuperMini - 4 voies / 8 relais
//  Firmware v1.0.0
//
//  Carte : "ESP32C3 Dev Module" (Arduino-ESP32 3.x)
//          Outils > USB CDC On Boot : "Enabled"  (moniteur serie par USB-C)
//  Bibliotheque : NimBLE-Arduino 2.x (h2zero) - Gestionnaire de bibliotheques
//
//  Phoebus du F.L.A.L. - Lycee de la Borde Basse - STI2D / NSI
// =====================================================================
//
//  SERVICE BLE  5f6d0001-1d9c-4c7e-a7b1-53494d50414e
//   - CONFIG 5f6d0002 (lecture / ecriture)  configuration du boitier
//   - ETAT   5f6d0003 (lecture / notification) modes des 4 voies
//   - CMD    5f6d0004 (ecriture)            commandes texte
//   - REPONSE 5f6d0005 (lecture / notification) reponse aux commandes
//  Protocole detaille : voir README.md
// =====================================================================
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_random.h>
#include "SimCore.h"

#define FW_VERSION "1.0.0"

// ----------------------- Brochage (schema KiCad v1.0) -----------------
// index : 0=K1 voie1 A, 1=K2 voie1 B, 2=K3 voie2 A, 3=K4 voie2 B ...
static const uint8_t RELAY_PINS[8] = {0, 1, 3, 4, 5, 6, 7, 10};
static const uint8_t LED_PIN = 8;   // LED bleue du SuperMini, active a l'etat bas
static const uint8_t BTN_PIN = 9;   // bouton RAZ (SW1) / BOOT, actif a l'etat bas

// ----------------------- BLE ------------------------------------------
#define UUID_SERVICE "5f6d0001-1d9c-4c7e-a7b1-53494d50414e"
#define UUID_CONFIG  "5f6d0002-1d9c-4c7e-a7b1-53494d50414e"
#define UUID_ETAT    "5f6d0003-1d9c-4c7e-a7b1-53494d50414e"
#define UUID_CMD     "5f6d0004-1d9c-4c7e-a7b1-53494d50414e"
#define UUID_REPONSE "5f6d0005-1d9c-4c7e-a7b1-53494d50414e"
static const uint8_t MAX_CONN = 3;  // 3 telephones simultanes au maximum

sp::Core core;
Preferences prefs;

NimBLEServer*         server  = nullptr;
NimBLECharacteristic* chCfg   = nullptr;
NimBLECharacteristic* chEtat  = nullptr;
NimBLECharacteristic* chCmd   = nullptr;
NimBLECharacteristic* chRep   = nullptr;

// ----------------------- File de messages BLE -> loop() ---------------
// Les callbacks BLE s'executent dans la tache NimBLE : on ne fait que
// deposer les messages, le traitement est fait dans loop().
enum MsgKind : uint8_t { K_CONNECT, K_DISCONNECT, K_CMD, K_CFG };
struct Msg {
  int16_t  conn;
  uint8_t  kind;
  uint16_t len;
  char     data[256];
};
static QueueHandle_t msgQueue;

static void pushMsg(uint16_t conn, uint8_t kind, const uint8_t* d, size_t n) {
  Msg m;
  m.conn = (int16_t)conn;
  m.kind = kind;
  m.len  = (uint16_t)min(n, sizeof(m.data) - 1);
  if (d && m.len) memcpy(m.data, d, m.len);
  m.data[m.len] = 0;
  xQueueSend(msgQueue, &m, 0);
}

// ----------------------- Materiel --------------------------------------
volatile uint32_t identUntil = 0;
uint32_t rebootAt = 0;

class BoardHW : public sp::Hardware {
 public:
  void relay(int i, bool on) override {
    if (i >= 0 && i < 8) digitalWrite(RELAY_PINS[i], on ? HIGH : LOW);
  }
  void     waitMs(uint32_t ms) override { delay(ms); }
  uint32_t nowMs() override { return millis(); }
  uint32_t randomU32() override { return esp_random(); }
  void saveConfig(const std::string& payload, const std::string& pin) override {
    prefs.putString("cfg", payload.c_str());
    prefs.putString("pin", pin.c_str());
  }
  void identify() override { identUntil = millis() + 6000; }
  void reboot() override { rebootAt = millis() + 500; }
} hw;

// ----------------------- Callbacks NimBLE ------------------------------
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& ci) override {
    pushMsg(ci.getConnHandle(), K_CONNECT, nullptr, 0);
    if (s->getConnectedCount() < MAX_CONN) NimBLEDevice::startAdvertising();
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& ci, int reason) override {
    pushMsg(ci.getConnHandle(), K_DISCONNECT, nullptr, 0);
    NimBLEDevice::startAdvertising();
  }
};

class WriteCB : public NimBLECharacteristicCallbacks {
 public:
  explicit WriteCB(uint8_t k) : kind(k) {}
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    NimBLEAttValue v = c->getValue();
    pushMsg(ci.getConnHandle(), kind, v.data(), v.size());
  }
 private:
  uint8_t kind;
};

// ----------------------- Publication des valeurs ----------------------
static void publishConfig() {
  std::string s = core.cfgString();
  chCfg->setValue((const uint8_t*)s.data(), s.size());
}

static void publishState(bool notify) {
  std::string s = core.stateString(server ? server->getConnectedCount() : 0);
  chEtat->setValue((const uint8_t*)s.data(), s.size());
  if (notify) chEtat->notify();
}

static void reply(const std::string& r, int16_t conn) {
  chRep->setValue((const uint8_t*)r.data(), r.size());
  if (conn >= 0) chRep->notify((uint16_t)conn);
  Serial.printf("[BLE %d] %s\n", conn, r.c_str());
}

static void updateAdvertising() {
  std::string name = core.advName();
  NimBLEDevice::setDeviceName(name);
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();
  NimBLEAdvertisementData ad;
  ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  ad.setCompleteServices(NimBLEUUID(UUID_SERVICE));
  adv->setAdvertisementData(ad);
  NimBLEAdvertisementData sr;
  sr.setName(name);
  adv->setScanResponseData(sr);
  if (server->getConnectedCount() < MAX_CONN) adv->start();
  Serial.printf("Annonce BLE : %s\n", name.c_str());
}

// ----------------------- Liaison serie (configuration USB) ------------
static String serialLine;

static void printHelp() {
  Serial.println(F("\n=== Simulateur de pannes - commandes serie ==="));
  Serial.println(F("SHOW                      : afficher configuration et etat"));
  Serial.println(F("CFG nom|lieu|periode|T1|ID1|LB1|T2|ID2|LB2|T3|ID3|LB3|T4|ID4|LB4"));
  Serial.println(F("     T = E (entree), S (sortie) ou - (non utilisee)"));
  Serial.println(F("SET <voie 1-4> <mode 0-4>  : 0 normal 1 coupure 2 forcage 3 coup.interm. 4 forc.interm."));
  Serial.println(F("ALL 0                     : toutes les voies a l'etat normal"));
  Serial.println(F("PIN <code>                : nouveau code PIN (4 a 8 chiffres)"));
  Serial.println(F("IDENT | RESETCFG | REBOOT | HELP"));
}

static void showAll() {
  Serial.printf("\nFirmware %s  MAC %s  Nom BLE : %s\n", FW_VERSION, core.mac.c_str(), core.advName().c_str());
  Serial.printf("Configure : %s  Nom : '%s'  Lieu : '%s'  Periode : %u ms  PIN : %s\n",
                core.configured() ? "oui" : "NON", core.cfg.name.c_str(), core.cfg.loc.c_str(),
                (unsigned)core.cfg.period, core.cfg.pin.c_str());
  static const char* MODES[] = {"normal", "COUPURE", "FORCAGE", "COUPURE INTERM.", "FORCAGE INTERM."};
  for (int v = 0; v < sp::NB_VOIES; v++) {
    const sp::Voie& vo = core.cfg.v[v];
    Serial.printf("  Voie %d [%c] %-12s %-24s -> %s\n", v + 1, vo.type, vo.id.c_str(), vo.label.c_str(),
                  MODES[core.mode(v)]);
  }
  Serial.printf("Connexions BLE : %u\n", server->getConnectedCount());
}

static void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      serialLine.trim();
      if (serialLine.length()) {
        std::string l = serialLine.c_str();
        std::string u = sp::upper(l);
        if (u == "HELP" || u == "?") printHelp();
        else if (u == "SHOW") showAll();
        else Serial.println(core.handleCommand(l, -1).c_str());
      }
      serialLine = "";
    } else if (serialLine.length() < 300) {
      serialLine += c;
    }
  }
}

// ----------------------- Bouton RAZ / LED -----------------------------
static uint32_t btnDownAt = 0;
static bool     btnWasDown = false, pinResetDone = false;

static void handleButton() {
  bool down = digitalRead(BTN_PIN) == LOW;
  uint32_t now = millis();
  if (down && !btnWasDown) { btnDownAt = now; pinResetDone = false; }
  if (down && !pinResetDone && now - btnDownAt > 8000) {   // appui 8 s : PIN usine
    core.cfg.pin = "0000";
    hw.saveConfig(core.configPayload(), core.cfg.pin);
    pinResetDone = true;
    identUntil = now + 3000;
    Serial.println(F("Code PIN reinitialise a 0000"));
  }
  if (!down && btnWasDown) {
    uint32_t d = now - btnDownAt;
    if (d > 40 && d < 2000) {                              // appui court : RAZ des pannes
      core.allNormal();
      core.stateChanged = true;
      Serial.println(F("Bouton : toutes les voies a l'etat normal"));
    }
  }
  btnWasDown = down;
}

static void handleLed() {
  uint32_t now = millis();
  bool on;
  if ((int32_t)(identUntil - now) > 0)            on = (now / 100) % 2;          // identification
  else if (!core.configured())                     on = (now / 250) % 2;          // non configure
  else if (server->getConnectedCount() == 0)       on = (now % 3000) < 60;        // en attente
  else                                             on = (now % 1000) < 60;        // connecte
  digitalWrite(LED_PIN, on ? LOW : HIGH);
}

// =====================================================================
void setup() {
  for (uint8_t p : RELAY_PINS) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }   // relais au repos
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  pinMode(BTN_PIN, INPUT_PULLUP);
  Serial.begin(115200);

  msgQueue = xQueueCreate(8, sizeof(Msg));

  prefs.begin("simpanne", false);
  core.fw = FW_VERSION;
  core.loadConfig(prefs.getString("cfg", "").c_str(), prefs.getString("pin", "0000").c_str());
  core.begin(&hw);

  NimBLEDevice::init("SP");
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(9);   // +9 dBm : portee atelier
  core.mac = sp::upper(NimBLEDevice::getAddress().toString());

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  server->advertiseOnDisconnect(false);   // relance geree dans les callbacks

  NimBLEService* svc = server->createService(UUID_SERVICE);
  chCfg  = svc->createCharacteristic(UUID_CONFIG, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  chEtat = svc->createCharacteristic(UUID_ETAT, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  chCmd  = svc->createCharacteristic(UUID_CMD, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  chRep  = svc->createCharacteristic(UUID_REPONSE, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  chCfg->setCallbacks(new WriteCB(K_CFG));
  chCmd->setCallbacks(new WriteCB(K_CMD));
  server->start();

  publishConfig();
  publishState(false);
  chRep->setValue("PRET");
  updateAdvertising();

  printHelp();
  showAll();
}

void loop() {
  Msg m;
  while (xQueueReceive(msgQueue, &m, 0) == pdTRUE) {
    switch (m.kind) {
      case K_CONNECT:
        core.onConnect(m.conn);
        core.stateChanged = true;
        Serial.printf("Connexion BLE %d\n", m.conn);
        break;
      case K_DISCONNECT:
        core.onDisconnect(m.conn);
        core.stateChanged = true;
        Serial.printf("Deconnexion BLE %d\n", m.conn);
        break;
      case K_CMD:
        reply(core.handleCommand(std::string(m.data, m.len), m.conn), m.conn);
        break;
      case K_CFG:
        reply(core.handleConfigWrite(std::string(m.data, m.len), m.conn), m.conn);
        break;
    }
  }

  handleSerial();
  handleButton();
  core.tick();

  if (core.configChanged) {
    core.configChanged = false;
    publishConfig();
    updateAdvertising();
  }
  static uint32_t lastPub = 0;
  if (core.stateChanged) {
    core.stateChanged = false;
    publishState(true);
    lastPub = millis();
  } else if (millis() - lastPub > 1000) {   // met a jour le temps de fonctionnement (lecture)
    publishState(false);
    lastPub = millis();
  }

  handleLed();
  if (rebootAt && (int32_t)(millis() - rebootAt) > 0) ESP.restart();
  delay(5);
}
