// Tests unitaires du coeur du simulateur (s'executent sur PC, sans ESP32)
// Compilation : g++ -std=c++17 test_core.cpp ../SimPanne_ESP32C3/SimCore.cpp -o test_core
#include "../SimPanne_ESP32C3/SimCore.h"
#include <cstdio>
#include <vector>
using namespace sp;

struct FakeHW : Hardware {
  bool r[8] = {};
  std::vector<std::string> log;
  uint32_t t = 0, seed = 1;
  std::string saved, savedPin;
  int ident = 0, reb = 0;
  void relay(int i, bool on) override { r[i] = on; log.push_back(std::to_string(i) + (on ? "+" : "-")); }
  void waitMs(uint32_t ms) override { t += ms; log.push_back("w"); }
  uint32_t nowMs() override { return t; }
  uint32_t randomU32() override { seed = seed * 1103515245u + 12345u; return seed >> 8; }
  void saveConfig(const std::string& p, const std::string& pin) override { saved = p; savedPin = pin; }
  void identify() override { ident++; }
  void reboot() override { reb++; }
};

#define CHECK(c) do { if (!(c)) { printf("ECHEC ligne %d : %s\n", __LINE__, #c); return 1; } n++; } while (0)

int main() {
  int n = 0;
  FakeHW hw;
  Core c;
  c.mac = "A0:B1:C2:D3:E4:F5";
  c.begin(&hw);
  CHECK(!c.configured());
  CHECK(c.advName() == "SP-NEW-E4F5");
  CHECK(c.stateString(0) == "ST|0|0|0|0|0|0|0");

  // --- Authentification ---
  CHECK(c.handleCommand("SET 1 1", 5) == "ERR AUTH REQUISE");
  CHECK(c.handleCommand("AUTH 1234", 5) == "ERR AUTH");
  CHECK(c.handleCommand("auth 0000", 5) == "OK AUTH");
  CHECK(c.isAuth(5) && !c.isAuth(6));
  CHECK(c.handleCommand("PING", 6) == "OK PONG");
  CHECK(c.handleCommand("IDENT", 6) == "OK IDENT" && hw.ident == 1);

  // --- Configuration ---
  std::string p = "Poste 3|Ligne de tri|1500|E|%I0.2|Capteur présence S1|S|%Q0.1|Vérin V1+|-|||E|%IW64|Débit 4-20mA";
  CHECK(c.handleConfigWrite(p, 6) == "ERR AUTH REQUISE");
  CHECK(c.handleConfigWrite(p, 5) == "OK CFG");
  CHECK(c.configured() && c.cfg.v[0].type == 'E' && c.cfg.v[1].type == 'S' && c.cfg.v[2].type == '-');
  CHECK(c.cfg.period == 1500);
  CHECK(c.advName() == "SP-Poste 3");
  CHECK(hw.saved == p);
  CHECK(c.cfgString() == "CFG|1.0.0|A0:B1:C2:D3:E4:F5|1|" + p);
  CHECK(c.handleConfigWrite("a|b|c", 5) == "ERR CFG");
  CHECK(c.handleConfigWrite("X|b|100|E|||E|||E|||E||", 5) == "ERR CFG");   // periode trop courte
  CHECK(c.handleConfigWrite("X|b|1000|Z|||E|||E|||E||", 5) == "ERR CFG");  // type invalide
  CHECK(c.handleConfigWrite("|b|1000|E|||E|||E|||E||", 5) == "ERR NOM");
  CHECK(sanitize("ab|cd", 10) == "ab/cd");
  CHECK(sanitize("éééééé", 5) == "éé");          // ne coupe pas un caractere accentue
  CHECK(sanitize("  x\n", 5) == "x");

  // --- Modes et ordre de commutation des relais ---
  hw.log.clear();
  CHECK(c.handleCommand("SET 1 2", 5) == "OK SET 1 2");
  CHECK(hw.r[0] && hw.r[1]);
  CHECK(hw.log.size() == 3 && hw.log[0] == "1+" && hw.log[1] == "w" && hw.log[2] == "0+");  // B avant A
  hw.log.clear();
  c.handleCommand("SET 1 0", 5);
  CHECK(!hw.r[0] && !hw.r[1] && hw.log[0] == "0-" && hw.log[2] == "1-");                    // A avant B
  c.handleCommand("SET 2 1", 5);
  CHECK(hw.r[2] && !hw.r[3]);
  CHECK(c.stateString(1).rfind("ST|0|1|0|0|", 0) == 0);
  CHECK(c.handleCommand("SET 5 1", 5) == "ERR VOIE");
  CHECK(c.handleCommand("SET 1 9", 5) == "ERR MODE");
  CHECK(c.handleCommand("SET 1", 5) == "ERR SYNTAXE");

  // --- Intermittence ---
  c.handleCommand("SET 4 3", 5);
  int toggles = 0;
  bool last = hw.r[6];
  for (int i = 0; i < 20000; i++) {
    hw.t += 1;
    c.tick();
    if (hw.r[6] != last) { toggles++; last = hw.r[6]; }
  }
  CHECK(toggles >= 10 && toggles <= 200);
  CHECK(!hw.r[7]);
  CHECK(c.mode(3) == M_COUP_INT && c.activeFaults() == 2);
  c.handleCommand("SET 4 4", 5);
  CHECK(hw.r[7]);

  // --- RAZ ---
  CHECK(c.handleCommand("ALL 0", 5) == "OK ALL 0");
  for (int i = 0; i < 8; i++) CHECK(!hw.r[i]);
  CHECK(c.activeFaults() == 0);

  // --- Code PIN ---
  CHECK(c.handleCommand("PIN 12", 5) == "ERR PIN");
  CHECK(c.handleCommand("PIN 4321", 5) == "OK PIN" && hw.savedPin == "4321");
  c.onDisconnect(5);
  CHECK(!c.isAuth(5));
  CHECK(c.handleCommand("AUTH 0000", 5) == "ERR AUTH");
  CHECK(c.handleCommand("AUTH 4321", 5) == "OK AUTH");

  // --- Liaison serie + voie devenue non utilisee remise au repos ---
  c.handleCommand("SET 3 1", -1);
  CHECK(hw.r[4]);
  CHECK(c.handleCommand("CFG Poste 3|Ligne|2000|E|a|b|S|c|d|-|||-||", -1) == "OK CFG");
  CHECK(!hw.r[4] && c.mode(2) == 0);

  // --- Rechargement depuis la memoire flash ---
  Core c2;
  c2.mac = c.mac;
  c2.loadConfig(c.configPayload(), "4321");
  c2.begin(&hw);
  CHECK(c2.cfg.name == "Poste 3" && c2.cfg.pin == "4321" && c2.activeFaults() == 0);

  CHECK(c.handleCommand("RESETCFG", 5) == "OK RESETCFG" && !c.configured());
  CHECK(c.handleCommand("FOO", 5) == "ERR INCONNUE");
  CHECK(c.handleCommand("   ", 5) == "ERR VIDE");

  printf("OK : %d verifications du coeur reussies\n", n);
  return 0;
}
