# Simulateur de pannes BLE – ESP32-C3 / 4 voies

Carte d'**interposition** qui se place sur une liaison **capteur → automate** ou
**automate → actionneur** d'un système automatisé didactique. Depuis une application
smartphone (BLE), l'enseignant provoque des pannes réalistes (fil coupé, court-circuit,
forçage, dérive) sans toucher au câblage pendant la séance.

*Phoebus du F.L.A.L. – Lycée de la Borde Basse – STI2D / NSI – Rév. 1.0 (13/11/2026)*

---

## 1. Contenu du dossier

| Fichier | Rôle |
|---|---|
| `Simulateur_panne.kicad_pro` | Projet KiCad (ouvrir celui-ci) |
| `Simulateur_panne.kicad_sch` | Schéma complet A3 (format KiCad 7, s'ouvre en KiCad 7 / 8 / 9) |
| `Simulateur_panne.kicad_sym` | Bibliothèque projet : symbole `ESP32-C3_SuperMini` |
| `Simulateur_panne.pretty/` | Bibliothèque projet : empreinte `ESP32-C3_SuperMini_Socket` |
| `sym-lib-table`, `fp-lib-table` | Déclarent ces bibliothèques locales (`${KIPRJMOD}`) |
| `exports/Schema_Simulateur_panne.pdf` / `.png` | Schéma imprimable |
| `exports/BOM_Simulateur_panne.csv` | Nomenclature (séparateur `;`, s'ouvre dans Excel) |

## 2. Principe d'une voie d'interposition

Chaque voie dispose d'un bornier 3 points et de **2 relais** :

```
              K_A (repos)                    Bornier J4..J7
 IN  o────────NC ┐                           1 : IN  = côté source
                 COM ───────────── o OUT      (capteur ou sortie automate)
           ┌──NO ┘                           2 : OUT = côté destination
           │ (nœud X)                         (entrée automate ou actionneur)
           └── COM K_B ── NC : en l'air      3 : FLT = référence de panne
                       └─ NO ──────── o FLT   (0 V, +24 V, résistance…)
```

| K_A | K_B | État de la liaison | Panne simulée |
|:-:|:-:|---|---|
| 0 | x | OUT = IN | **Fonctionnement normal** |
| 1 | 0 | OUT en l'air | **Fil coupé**, capteur débranché, actionneur non commandé |
| 1 | 1 | OUT = FLT | **Forçage / court-circuit** (FLT câblé au 0 V, au +24 V ou via une résistance pour une dérive) |

Pannes « logicielles » gérées par le firmware : **intermittente** (K_A clignote),
**retardée** (déclenchement après N s), **scénarios** enchaînés.

> **Sécurité intrinsèque** : relais au repos = liaison normale. Si l'ESP32 plante,
> redémarre ou perd l'alimentation, le système automatisé fonctionne normalement.

## 3. Architecture (blocs du schéma)

**A – Alimentation 5 V / 2 A**
- Entrée par jack 5,5 × 2,1 (J1) **ou** bornier (J2).
- **Q1 AO3401A** (P-MOS) : protection contre l'inversion de polarité avec une chute
  < 0,1 V. Une diode série ferait perdre 0,4 V : les bobines n'auraient plus que
  ≈ 3,7 V après le ULN2803A, sous le seuil d'enclenchement du relais (3,75 V).
- **F1** PTC réarmable 1,1 A, **D1** TVS P6KE6.8A (surtensions), C1 470 µF + C2 100 nF.
- LED verte D3 « sous tension ».

**B – Module ESP32-C3 SuperMini** (BLE 5, USB-C, régulateur 3,3 V intégré)
- Monté sur 2 barrettes femelles 1×08 (entraxe 15,24 mm) : remplaçable, programmable par USB-C.
- **D2 1N5819** : empêche le 5 V de l'USB de remonter vers la carte (et inversement).
- **SW1** sur GPIO9 : bouton « RAZ des pannes » en façade (pull-up déjà présente sur le module).
- **J3** extension : 3V3, GND, GPIO2, GPIO20 (RX), GPIO21 (TX), +5V → écran OLED I2C, buzzer, debug.

**C – Commande des relais**
- **ULN2803A** : 8 darlingtons, diodes de roue libre intégrées (COM au +5 V). Entrées
  compatibles 3,3 V, résistance de rappel interne → relais au repos pendant le boot.
- 8 LEDs (orange = K_A « coupure », rouge = K_B « forçage »).
- **JP1** : cavalier retiré = LEDs éteintes → pannes invisibles pour les élèves.

**D – 4 voies** : K1…K8 = **Omron G5V-1** (relais de signal, contacts adaptés aux
faibles courants : entrées automate, boucles 4-20 mA, capteurs).

## 4. Affectation des GPIO

| GPIO | Fonction | | GPIO | Fonction |
|---|---|---|---|---|
| 0 | K1 – voie 1 A | | 5 | K5 – voie 3 A |
| 1 | K2 – voie 1 B | | 6 | K6 – voie 3 B |
| 3 | K3 – voie 2 A | | 7 | K7 – voie 4 A |
| 4 | K4 – voie 2 B | | 10 | K8 – voie 4 B |
| 9 | Bouton RAZ (SW1) | | 2 / 20 / 21 | Extension J3 |
| 8 | LED du module (non utilisé) | | 18 / 19 | USB (non accessible) |

GPIO2, GPIO8 et GPIO9 sont des **broches de strapping** : elles ne pilotent pas de
relais, sinon le ULN2803A (qui tire vers 0 V) bloquerait le démarrage.

## 5. Bilan de consommation (sous 5 V)

| Poste | Courant |
|---|---|
| 8 bobines G5V-1 (30 mA) – cas extrême | 240 mA |
| 9 LEDs (≈ 2,5 mA) | 25 mA |
| ESP32-C3 en émission BLE (pic) | ≈ 350 mA |
| Extension J3 (réserve) | 200 mA |
| **Total maxi** | **≈ 0,8 A** → alimentation 2 A largement dimensionnée |

## 6. Exemples de raccordement

- **Détecteur PNP 24 V** : IN = fil noir (sortie) du capteur, OUT = entrée automate.
  FLT au 0 V → « capteur bloqué à 0 » ; FLT au +24 V → « capteur bloqué à 1 ».
- **Boucle 4-20 mA** : interposer sur un fil de la boucle. Coupure → 0 mA (rupture
  détectée par l'automate) ; FLT relié à l'autre fil via 250 Ω → dérive de mesure.
- **Commande d'actionneur 24 V** (bobine de contacteur, distributeur) : IN = sortie
  automate, OUT = actionneur. Coupure → l'actionneur ne répond plus à l'ordre.

## 7. Limites et sécurité – à respecter impérativement

- **TBTS uniquement : ≤ 30 V DC, ≤ 1 A par contact** (G5V-1 : 1 A / 24 V DC).
- **Jamais de 230 V AC** sur les borniers de voie.
- Ne **jamais** s'interposer sur une chaîne de sécurité (arrêt d'urgence, carter,
  barrière immatérielle).
- Pour des actionneurs > 1 A, interposer sur la **commande** (bobine du
  contacteur / relais d'interface), pas sur la puissance.

## 8. Points à vérifier avant le routage du PCB

1. **Brochage du SuperMini** : il existe plusieurs variantes de clones. Vérifier la
   correspondance broches/GPIO de votre module avec l'empreinte (broche 1 = 5 V,
   côté USB-C) avant de lancer la fabrication.
2. **F1** : l'empreinte `Fuse_Bourns_MF-RHT100` (pas 5,1 mm) est compatible MF-R110 ;
   à vérifier selon la référence achetée.
3. Q1 (SOT-23) est le seul composant CMS : se soude facilement au fer.
4. Dans KiCad 8/9 : lancer *Inspecter → Contrôle des règles électriques (ERC)*.
   Les seules broches non connectées (marquées ✕) sont volontaires : NC des relais B
   et GPIO8.

## 9. Étapes suivantes proposées

1. Routage du PCB (format 100 × 80 mm, borniers sur un seul bord).
2. Firmware ESP32-C3 (Arduino / PlatformIO) : service BLE GATT
   *Simulateur_panne* avec une caractéristique d'écriture par voie
   (`0 = normal`, `1 = coupure`, `2 = forçage`, `3 = intermittent`) et une
   caractéristique de notification de l'état.
3. Application smartphone (MIT App Inventor ou appli web Web Bluetooth).
