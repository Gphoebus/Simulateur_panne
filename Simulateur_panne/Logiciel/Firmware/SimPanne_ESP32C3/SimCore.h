// =====================================================================
//  SimCore.h - Coeur du simulateur de pannes (protocole + etats relais)
//  Code independant d'Arduino : teste aussi sur PC (dossier tests/).
//  Phoebus du F.L.A.L. - Lycee de la Borde Basse - STI2D / NSI
// =====================================================================
#pragma once
#include <stdint.h>
#include <string>

namespace sp {

static const int NB_VOIES = 4;

// Modes de panne d'une voie
enum Mode : uint8_t {
  M_NORMAL   = 0,  // relais A=0 B=x : OUT = IN
  M_COUPURE  = 1,  // relais A=1 B=0 : OUT en l'air (fil coupe)
  M_FORCAGE  = 2,  // relais A=1 B=1 : OUT = FLT (court-circuit / forcage)
  M_COUP_INT = 3,  // coupure intermittente (A bascule aleatoirement, B=0)
  M_FORC_INT = 4,  // forcage intermittent  (A bascule aleatoirement, B=1)
  M_NB       = 5
};

// Limites des champs de configuration (tenir dans un MTU de 247 octets)
static const size_t MAX_NAME  = 20;
static const size_t MAX_LOC   = 24;
static const size_t MAX_ID    = 12;
static const size_t MAX_LABEL = 24;

struct Voie {
  char type = '-';        // 'E' = entree automate, 'S' = sortie automate, '-' = non utilisee
  std::string id;         // identifiant donne par l'enseignant (ex : %I0.3)
  std::string label;      // libelle (ex : Capteur fin de course S2)
};

struct Config {
  std::string name;       // vide = simulateur non configure
  std::string loc;        // emplacement dans l'atelier
  std::string pin = "0000";
  uint32_t period = 2000; // periode max de l'intermittence (ms)
  Voie v[NB_VOIES];
};

// Interface materielle fournie par le sketch Arduino (ou par les tests)
class Hardware {
 public:
  virtual ~Hardware() {}
  virtual void     relay(int index, bool on) = 0;   // index 0..7 : 2*voie (A), 2*voie+1 (B)
  virtual void     waitMs(uint32_t ms) = 0;
  virtual uint32_t nowMs() = 0;
  virtual uint32_t randomU32() = 0;
  virtual void     saveConfig(const std::string& payload, const std::string& pin) = 0;
  virtual void     identify() = 0;                  // clignotement LED du boitier
  virtual void     reboot() = 0;
};

class Core {
 public:
  Config      cfg;
  std::string fw  = "1.0.0";
  std::string mac = "00:00:00:00:00:00";

  void begin(Hardware* hw);

  // Configuration : "nom|lieu|periode|T1|ID1|LB1|T2|ID2|LB2|T3|ID3|LB3|T4|ID4|LB4"
  bool        parseConfigPayload(const std::string& s, Config& out) const;
  std::string configPayload() const;
  void        loadConfig(const std::string& payload, const std::string& pin);

  // Messages exposes en BLE
  std::string cfgString() const;                         // caracteristique CONFIG (lecture)
  std::string stateString(int nbConnexions) const;       // caracteristique ETAT
  std::string advName() const;                           // nom annonce en BLE

  // Traitement d'une commande texte. conn = -1 : liaison serie USB (toujours autorisee)
  std::string handleCommand(const std::string& cmd, int conn);
  std::string handleConfigWrite(const std::string& payload, int conn);

  void onConnect(int conn);
  void onDisconnect(int conn);
  bool isAuth(int conn) const;

  bool setMode(int voie, int mode);   // voie 0..3
  void allNormal();
  void tick();                        // a appeler dans loop()

  bool    configured() const { return !cfg.name.empty(); }
  uint8_t mode(int v) const { return mode_[v]; }
  int     activeFaults() const;

  // Drapeaux lus par le sketch
  bool stateChanged = false;   // envoyer une notification ETAT
  bool configChanged = false;  // mettre a jour le nom annonce

 private:
  Hardware* hw_ = nullptr;
  uint8_t   mode_[NB_VOIES] = {0, 0, 0, 0};
  bool      a_[NB_VOIES] = {false, false, false, false};
  bool      b_[NB_VOIES] = {false, false, false, false};
  uint32_t  next_[NB_VOIES] = {0, 0, 0, 0};
  static const int MAX_AUTH = 8;
  int       auth_[MAX_AUTH] = {-1, -1, -1, -1, -1, -1, -1, -1};

  void     applyAB(int v, bool a, bool b);
  void     setA(int v, bool on);
  void     setB(int v, bool on);
  uint32_t randomDelay();
};

// Outils texte
std::string sanitize(const std::string& s, size_t maxLen);
std::string upper(const std::string& s);
std::string trim(const std::string& s);

}  // namespace sp
