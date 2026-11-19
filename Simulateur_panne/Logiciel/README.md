# Simulateur de pannes BLE : firmware ESP32-C3 et application Android

*Phoebus du F.L.A.L. – Lycée de la Borde Basse – STI2D / NSI – v1.0.0 (14/11/2026)*

Ce dossier contient le logiciel des boîtiers simulateurs (carte KiCad v1.0 : ESP32-C3 SuperMini,
ULN2803A, 8 relais G5V-1, 4 voies) et l'application Android qui permet à l'enseignant de piloter
**tous les simulateurs de l'atelier**.

```
Logiciel/
├── SimuPannes.apk                      ← application Android, prête à installer
├── Firmware/
│   ├── SimPanne_ESP32C3/               ← sketch Arduino (ouvrir le .ino)
│   │   ├── SimPanne_ESP32C3.ino        BLE NimBLE, relais, LED, bouton, liaison série
│   │   ├── SimCore.h / SimCore.cpp     cœur : protocole, modes de panne, PIN
│   ├── bin/SimPanne_ESP32C3_v1.0.0_merged.bin   ← firmware compilé (flash à l'adresse 0x0)
│   └── tests/test_core.cpp             tests unitaires du cœur (PC)
└── Android/                            sources de l'application
    ├── app/src/main/assets/index.html  interface (HTML/JS) – fonctionne aussi en mode démo dans un navigateur
    ├── app/src/main/java/…/MainActivity.java, BleManager.java
    ├── build_apk.sh                    reconstruction de l'APK (sans Android Studio)
    └── simpanne.keystore               clé de signature (mot de passe : simpanne)
```

---

## 1. Fonctionnement général

| Besoin | Solution |
|---|---|
| Plusieurs simulateurs dans l'atelier | Chaque boîtier annonce un **nom unique** en BLE. L'application se connecte à tous les boîtiers de sa liste en même temps. |
| Identifier un boîtier avant la mise en service | Un boîtier neuf s'annonce **`SP-NEW-xxxx`** (xxxx = fin de son adresse MAC) et sa LED clignote vite. Le bouton **Identifier** fait clignoter sa LED pendant 6 s. Le nom et les voies sont ensuite **enregistrés dans le boîtier** (mémoire flash). |
| Ajouter, modifier, supprimer un simulateur | Bouton **＋** (recherche), page **Configurer**, menu **⋮ → Supprimer** (avec option de retour usine du boîtier). |
| Voie interposée sur une entrée ou une sortie | Pour chaque voie : *Entrée automate* (capteur → automate) ou *Sortie automate* (automate → actionneur), avec **identifiant** (ex. `%I0.2`, `%Q0.4`) et **libellé**. |
| Pilotage par appui sur une touche | 5 touches par voie : **Normal, Coupure, Forçage, Coupure ~, Forçage ~** (« ~ » = intermittent aléatoire). |
| Tableau de bord au redémarrage | Au lancement, l'application **se connecte à chaque simulateur et relit sa configuration et l'état de ses voies**. Ensuite le boîtier envoie chaque changement (notification BLE), y compris quand une autre tablette ou le bouton RAZ agit. |

La configuration est stockée **dans le boîtier**. Un second téléphone qui ajoute le même boîtier
retrouve donc les mêmes noms et identifiants de voies.

### Correspondance entre modes et relais (une voie = relais A + relais B)

| Mode | Relais A | Relais B | Effet sur OUT |
|---|:-:|:-:|---|
| 0 Normal | 0 | 0 | OUT = IN |
| 1 Coupure | 1 | 0 | OUT en l'air (fil coupé) |
| 2 Forçage | 1 | 1 | OUT = FLT (0 V, +24 V, résistance…) |
| 3 Coupure ~ | aléatoire | 0 | faux contact (durées de 0,1 s à la période réglée) |
| 4 Forçage ~ | aléatoire | 1 | forçage intermittent |

Ordre de commutation : B se ferme **avant** A, et A s'ouvre **avant** B. Ainsi OUT ne passe jamais
par un état « en l'air » parasite. **Au démarrage du boîtier, toutes les voies sont à l'état normal**
(sécurité) : les pannes ne sont pas mémorisées après une coupure d'alimentation.

---

## 2. Firmware ESP32-C3

### Installation (Arduino IDE 2)
1. Gestionnaire de cartes : **esp32 by Espressif**, version 3.x (testé et compilé avec la 3.3.12).
2. Gestionnaire de bibliothèques : **NimBLE-Arduino** by h2zero, version 2.x (testé avec la 2.5.2).
3. Carte : **ESP32C3 Dev Module**. Dans *Outils*, régler **USB CDC On Boot : Enabled**.
4. Ouvrir `SimPanne_ESP32C3.ino` puis téléverser.
   Si le téléversement échoue : maintenir **BOOT** (SW1), appuyer sur RESET, relâcher BOOT.

**Sans Arduino IDE** : flasher `bin/SimPanne_ESP32C3_v1.0.0_merged.bin` à l'adresse **0x0** avec
<https://espressif.github.io/esptool-js/> (Chrome ou Edge, câble USB-C).

Bilan de compilation : 605 ko de flash (46 %), 24 ko de RAM (7 %), aucun avertissement.

### Brochage utilisé
| GPIO | 0 | 1 | 3 | 4 | 5 | 6 | 7 | 10 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| Rôle | K1 V1-A | K2 V1-B | K3 V2-A | K4 V2-B | K5 V3-A | K6 V3-B | K7 V4-A | K8 V4-B | LED | bouton RAZ |

### LED du module (GPIO8)
- clignotement rapide (10 Hz) : identification demandée ;
- clignotement 2 Hz : boîtier **non configuré** ;
- éclair toutes les 3 s : configuré, aucun téléphone connecté ;
- éclair toutes les secondes : au moins un téléphone connecté.

### Bouton SW1 (GPIO9)
- **appui court** : toutes les voies reviennent à l'état normal ;
- **appui de 8 s** : le code PIN revient à **0000** (en cas d'oubli).

### Configuration par la liaison série (115200 bauds, optionnelle)
`HELP`, `SHOW`, `CFG Poste 3|Ligne de tri|2000|E|%I0.2|Capteur S1|S|%Q0.1|Vérin V1|-|||-||`,
`SET 1 2`, `ALL 0`, `PIN 1234`, `IDENT`, `RESETCFG`, `REBOOT`.

---

## 3. Application Android « Simu Pannes »

### Installation
1. Copier `SimuPannes.apk` sur le téléphone ou la tablette (Android 6 minimum, conçue pour Android 14).
2. L'ouvrir et autoriser l'installation « depuis cette source ». L'application n'étant pas publiée
   sur le Play Store, Play Protect peut afficher un avertissement : choisir *Installer quand même*.
3. Au premier lancement, accepter les autorisations **Appareils à proximité** (Android 12 et plus)
   ou **Localisation** (Android 6 à 11). Sur Android 6 à 11, la localisation doit aussi être activée.

### Mise en service d'un nouveau boîtier
1. Alimenter le boîtier : sa LED clignote à 2 Hz (non configuré).
2. Dans l'application : **＋**. Le boîtier apparaît sous la forme `SP-NEW-xxxx` avec le badge **NEUF**.
3. Le toucher, garder le PIN `0000` puis **Ajouter**. La page de configuration s'ouvre.
4. **Identifier** : vérifier que c'est bien la LED de ce boîtier qui clignote.
5. Saisir le **nom** (ex. *Poste 3*), l'**emplacement**, puis pour chaque voie : *Entrée* ou *Sortie*,
   l'**identifiant** et le **libellé**. Appuyer sur **Enregistrer**. Le boîtier s'annonce désormais `SP-Poste 3`.
6. Conseil : **changer le code PIN** du boîtier. Sans ce code, une autre application BLE (par exemple
   celle d'un élève) ne peut pas déclencher de panne.

### En séance
- Le tableau de bord affiche chaque simulateur, ses voies (ENTRÉE en bleu, SORTIE en orange) et le
  mode en cours. Le cadre devient rouge dès qu'une panne est active.
- La touche du mode reste en surbrillance **uniquement après confirmation du boîtier**.
- Le gros bouton rouge **TOUT NORMAL — ATELIER** supprime les pannes de tous les boîtiers connectés.
- L'écran reste allumé tant que l'application est ouverte.

### Mode démonstration
`Android/app/src/main/assets/index.html` s'ouvre dans Chrome sur PC, avec 3 simulateurs fictifs.
Pratique pour présenter l'outil sans matériel.

### Reconstruire l'APK après une modification
Sous Ubuntu, ou dans WSL sous Windows :
`sudo apt install android-sdk-platform-23 android-sdk-build-tools apksigner zipalign default-jdk`,
puis `./build_apk.sh`. Garder `simpanne.keystore` : la même clé permet d'installer une mise à jour
sans désinstaller l'application.

---

## 4. Protocole BLE (pour les élèves de NSI ou une autre application)

Service `5f6d0001-1d9c-4c7e-a7b1-53494d50414e`. Messages texte UTF-8, champs séparés par `|`.

| Caractéristique | UUID | Accès | Contenu |
|---|---|---|---|
| CONFIG | `5f6d0002-…` | lecture | `CFG|fw|mac|configuré(0/1)|nom|lieu|période_ms|T1|ID1|LB1|…|T4|ID4|LB4` |
| | | écriture (PIN) | `nom|lieu|période_ms|T1|ID1|LB1|…|T4|ID4|LB4` (T = `E`, `S` ou `-`) |
| ETAT | `5f6d0003-…` | lecture, notification | `ST|m1|m2|m3|m4|uptime_s|nb_connexions|configuré` |
| CMD | `5f6d0004-…` | écriture | `AUTH <pin>`, `SET <voie> <mode>`, `ALL 0`, `PIN <code>`, `IDENT`, `PING`, `RESETCFG`, `REBOOT` |
| REPONSE | `5f6d0005-…` | lecture, notification | `OK …` ou `ERR AUTH`, `ERR AUTH REQUISE`, `ERR VOIE`, `ERR MODE`… |

L'authentification (`AUTH <pin>`) est valable **pour une connexion**. Sans elle, seuls `PING`,
`IDENT` et les lectures sont autorisés. Trois téléphones au maximum peuvent être connectés en même temps
au même boîtier. Longueurs maximales (en octets) : nom 20, lieu 24, identifiant 12, libellé 24.

---

## 5. Vérifications effectuées

- **Cœur du firmware** : 58 vérifications automatiques réussies (`tests/test_core.cpp` : PIN, configuration,
  ordre de commutation des relais, intermittence, RAZ, retour usine, accents UTF-8).
- **Firmware complet** : compilé pour ESP32-C3 (Arduino-ESP32 3.3.12 + NimBLE-Arduino 2.5.2), sans avertissement.
- **Interface** : scénario complet testé dans Chrome en mode démo (ajout d'un boîtier neuf, configuration,
  pannes, tout normal, suppression, redémarrage avec relecture de l'état).
- **APK** : construit et signé (schémas v1, v2 et v3), cible Android 14.
- **À valider sur le matériel réel** : liaison BLE téléphone ↔ boîtier (aucun boîtier ni téléphone n'était
  disponible pour ce test).
