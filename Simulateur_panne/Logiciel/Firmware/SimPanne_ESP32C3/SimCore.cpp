// =====================================================================
//  SimCore.cpp - Coeur du simulateur de pannes
//  Phoebus du F.L.A.L. - Lycee de la Borde Basse - STI2D / NSI
// =====================================================================
#include "SimCore.h"
#include <stdlib.h>
#include <vector>

namespace sp {

// ---------------------------------------------------------------------
//  Outils texte
// ---------------------------------------------------------------------
std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
  return s.substr(a, b - a);
}

std::string upper(const std::string& s) {
  std::string r = s;
  for (auto& c : r) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
  return r;
}

// Supprime le separateur '|' et les caracteres de controle, puis tronque
// a maxLen octets sans couper un caractere UTF-8 (accents).
std::string sanitize(const std::string& in, size_t maxLen) {
  std::string s;
  for (unsigned char c : in) {
    if (c == '|') s += '/';
    else if (c < 0x20 || c == 0x7F) s += ' ';
    else s += (char)c;
  }
  s = trim(s);
  if (s.size() > maxLen) {
    size_t n = maxLen;
    while (n > 0 && (((unsigned char)s[n]) & 0xC0) == 0x80) n--;  // debut d'un caractere
    s = trim(s.substr(0, n));
  }
  return s;
}

static std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) { out.push_back(cur); cur.clear(); }
    else cur += c;
  }
  out.push_back(cur);
  return out;
}

static bool toInt(const std::string& s, long& v) {
  if (s.empty()) return false;
  char* end = nullptr;
  v = strtol(s.c_str(), &end, 10);
  return end && *end == '\0';
}

static bool validPin(const std::string& p) {
  if (p.size() < 4 || p.size() > 8) return false;
  for (char c : p) if (c < '0' || c > '9') return false;
  return true;
}

// ---------------------------------------------------------------------
//  Initialisation / configuration
// ---------------------------------------------------------------------
void Core::begin(Hardware* hw) {
  hw_ = hw;
  for (int v = 0; v < NB_VOIES; v++) {   // demarrage securise : tout au repos
    mode_[v] = M_NORMAL;
    a_[v] = b_[v] = false;
    hw_->relay(2 * v, false);
    hw_->relay(2 * v + 1, false);
  }
  stateChanged = true;
}

bool Core::parseConfigPayload(const std::string& s, Config& out) const {
  std::vector<std::string> f = split(s, '|');
  if (f.size() != 3 + 3 * NB_VOIES) return false;
  Config c = cfg;                                   // conserve le PIN
  c.name = sanitize(f[0], MAX_NAME);
  c.loc  = sanitize(f[1], MAX_LOC);
  long p = 0;
  if (!toInt(trim(f[2]), p) || p < 200 || p > 60000) return false;
  c.period = (uint32_t)p;
  for (int v = 0; v < NB_VOIES; v++) {
    std::string t = upper(trim(f[3 + 3 * v]));
    if (t != "E" && t != "S" && t != "-") return false;
    c.v[v].type  = t[0];
    c.v[v].id    = sanitize(f[4 + 3 * v], MAX_ID);
    c.v[v].label = sanitize(f[5 + 3 * v], MAX_LABEL);
  }
  out = c;
  return true;
}

std::string Core::configPayload() const {
  std::string s = cfg.name + "|" + cfg.loc + "|" + std::to_string(cfg.period);
  for (int v = 0; v < NB_VOIES; v++)
    s += std::string("|") + cfg.v[v].type + "|" + cfg.v[v].id + "|" + cfg.v[v].label;
  return s;
}

void Core::loadConfig(const std::string& payload, const std::string& pin) {
  Config c;
  if (!payload.empty() && parseConfigPayload(payload, c)) cfg = c;
  if (validPin(pin)) cfg.pin = pin;
}

// CFG|fw|mac|configure(0/1)|nom|lieu|periode|T1|ID1|LB1|...|T4|ID4|LB4
std::string Core::cfgString() const {
  return "CFG|" + fw + "|" + mac + "|" + (configured() ? "1" : "0") + "|" + configPayload();
}

// ST|m1|m2|m3|m4|uptime_s|nb_connexions|configure
std::string Core::stateString(int nbConnexions) const {
  std::string s = "ST";
  for (int v = 0; v < NB_VOIES; v++) s += "|" + std::to_string((int)mode_[v]);
  s += "|" + std::to_string(hw_ ? hw_->nowMs() / 1000 : 0);
  s += "|" + std::to_string(nbConnexions);
  s += configured() ? "|1" : "|0";
  return s;
}

std::string Core::advName() const {
  if (!configured()) {
    // 4 derniers chiffres hexa de l'adresse MAC : SP-NEW-AB12
    std::string m;
    for (char c : mac) if (c != ':') m += c;
    std::string suf = m.size() >= 4 ? m.substr(m.size() - 4) : m;
    return "SP-NEW-" + upper(suf);
  }
  return sanitize("SP-" + cfg.name, 26);
}

// ---------------------------------------------------------------------
//  Authentification par connexion BLE
// ---------------------------------------------------------------------
void Core::onConnect(int conn) { onDisconnect(conn); }   // une nouvelle connexion n'est jamais authentifiee

void Core::onDisconnect(int conn) {
  for (int i = 0; i < MAX_AUTH; i++) if (auth_[i] == conn) auth_[i] = -1;
}

bool Core::isAuth(int conn) const {
  if (conn < 0) return true;  // liaison serie USB = acces physique au boitier
  for (int i = 0; i < MAX_AUTH; i++) if (auth_[i] == conn) return true;
  return false;
}

// ---------------------------------------------------------------------
//  Pilotage des relais
// ---------------------------------------------------------------------
void Core::setA(int v, bool on) { a_[v] = on; hw_->relay(2 * v, on); }
void Core::setB(int v, bool on) { b_[v] = on; hw_->relay(2 * v + 1, on); }

// Ordre de commutation choisi pour ne jamais laisser OUT en l'air inutilement :
//  - B se ferme AVANT A (le noeud X est deja relie a FLT quand A bascule)
//  - A se relache AVANT B
void Core::applyAB(int v, bool a, bool b) {
  if (b && !b_[v]) {
    setB(v, true);
    hw_->waitMs(20);
    setA(v, a);
  } else if (!b && b_[v]) {
    setA(v, a);
    hw_->waitMs(20);
    setB(v, false);
  } else {
    setA(v, a);
    setB(v, b);
  }
}

uint32_t Core::randomDelay() {
  uint32_t maxMs = cfg.period < 200 ? 200 : cfg.period;
  return 100 + hw_->randomU32() % (maxMs - 99);   // 100 ms .. periode
}

bool Core::setMode(int v, int m) {
  if (v < 0 || v >= NB_VOIES || m < 0 || m >= M_NB) return false;
  switch (m) {
    case M_NORMAL:   applyAB(v, false, false); break;
    case M_COUPURE:  applyAB(v, true, false);  break;
    case M_FORCAGE:  applyAB(v, true, true);   break;
    case M_COUP_INT: applyAB(v, true, false);  next_[v] = hw_->nowMs() + randomDelay(); break;
    case M_FORC_INT: applyAB(v, true, true);   next_[v] = hw_->nowMs() + randomDelay(); break;
  }
  if (mode_[v] != m) stateChanged = true;
  mode_[v] = (uint8_t)m;
  return true;
}

void Core::allNormal() {
  for (int v = 0; v < NB_VOIES; v++) setMode(v, M_NORMAL);
}

int Core::activeFaults() const {
  int n = 0;
  for (int v = 0; v < NB_VOIES; v++) if (mode_[v] != M_NORMAL) n++;
  return n;
}

void Core::tick() {
  uint32_t now = hw_->nowMs();
  for (int v = 0; v < NB_VOIES; v++) {
    if (mode_[v] != M_COUP_INT && mode_[v] != M_FORC_INT) continue;
    if ((int32_t)(now - next_[v]) >= 0) {
      setA(v, !a_[v]);                   // le mode (et donc l'etat BLE) ne change pas
      next_[v] = now + randomDelay();
    }
  }
}

// ---------------------------------------------------------------------
//  Commandes texte (BLE caracteristique CMD ou liaison serie)
// ---------------------------------------------------------------------
std::string Core::handleCommand(const std::string& raw, int conn) {
  std::string line = trim(raw);
  std::vector<std::string> a;
  for (auto& w : split(line, ' ')) if (!w.empty()) a.push_back(w);
  if (a.empty()) return "ERR VIDE";
  std::string c = upper(a[0]);

  // --- Commandes sans authentification ---
  if (c == "PING")  return "OK PONG";
  if (c == "IDENT") { hw_->identify(); return "OK IDENT"; }
  if (c == "AUTH") {
    if (a.size() != 2) return "ERR SYNTAXE";
    if (a[1] != cfg.pin) return "ERR AUTH";
    if (conn >= 0 && !isAuth(conn)) {
      for (int i = 0; i < MAX_AUTH; i++) if (auth_[i] < 0) { auth_[i] = conn; break; }
    }
    return "OK AUTH";
  }

  // --- Commandes protegees par le code PIN ---
  if (!isAuth(conn)) return "ERR AUTH REQUISE";

  if (c == "SET") {                                  // SET <voie 1-4> <mode 0-4>
    long v, m;
    if (a.size() != 3 || !toInt(a[1], v) || !toInt(a[2], m)) return "ERR SYNTAXE";
    if (v < 1 || v > NB_VOIES) return "ERR VOIE";
    if (m < 0 || m >= M_NB) return "ERR MODE";
    setMode((int)v - 1, (int)m);
    stateChanged = true;                             // confirme meme si inchange
    return "OK SET " + std::to_string(v) + " " + std::to_string(m);
  }
  if (c == "ALL") {                                  // ALL <mode> (en pratique ALL 0)
    long m;
    if (a.size() != 2 || !toInt(a[1], m) || m < 0 || m >= M_NB) return "ERR SYNTAXE";
    for (int v = 0; v < NB_VOIES; v++)
      if (cfg.v[v].type != '-' || m == M_NORMAL) setMode(v, (int)m);
    stateChanged = true;
    return "OK ALL " + std::to_string(m);
  }
  if (c == "PIN") {                                  // PIN <nouveau code 4 a 8 chiffres>
    if (a.size() != 2 || !validPin(a[1])) return "ERR PIN";
    cfg.pin = a[1];
    hw_->saveConfig(configPayload(), cfg.pin);
    return "OK PIN";
  }
  if (c == "CFG") {                                  // CFG <payload> (liaison serie)
    size_t p = line.find(' ');
    return handleConfigWrite(p == std::string::npos ? "" : line.substr(p + 1), conn);
  }
  if (c == "RESETCFG") {                             // retour configuration usine
    allNormal();
    Config blank;
    cfg = blank;
    hw_->saveConfig("", cfg.pin);
    configChanged = true;
    stateChanged = true;
    return "OK RESETCFG";
  }
  if (c == "REBOOT") { allNormal(); hw_->reboot(); return "OK REBOOT"; }
  return "ERR INCONNUE";
}

std::string Core::handleConfigWrite(const std::string& payload, int conn) {
  if (!isAuth(conn)) return "ERR AUTH REQUISE";
  Config c;
  if (!parseConfigPayload(trim(payload), c)) return "ERR CFG";
  if (c.name.empty()) return "ERR NOM";
  cfg = c;
  // une voie devenue "non utilisee" est remise au repos
  for (int v = 0; v < NB_VOIES; v++)
    if (cfg.v[v].type == '-' && mode_[v] != M_NORMAL) setMode(v, M_NORMAL);
  hw_->saveConfig(configPayload(), cfg.pin);
  configChanged = true;
  stateChanged = true;
  return "OK CFG";
}

}  // namespace sp
