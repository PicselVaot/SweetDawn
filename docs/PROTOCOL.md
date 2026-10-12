# Protocole Bluetooth de l'AML005

Référence du dialogue entre SweetDawn et l'AML005, reconstituée à partir de l'application Android d'origine (SunBeam, décompilée), puis vérifiée sur le matériel quand c'est indiqué (✔).

Dans le code, les codes sont définis dans [`src/aml/protocol.h`](../src/aml/protocol.h) et chaque commande est implémentée dans [`src/core/control.c`](../src/core/control.c).

## Liaison

- Bluetooth classique, RFCOMM, canal 1 en général (voir `config/aml.json`).
- **L'AML ne répond à rien tant qu'il n'a pas reçu la requête de statut** (`00 16`) ✔. SweetDawn l'envoie à chaque connexion.
- L'AML n'accepte qu'une connexion à la fois ✔ : quand le service tourne, l'application mobile et `bin/sweetdawn` ne peuvent pas se connecter. Si l'application mobile se connecte, elle coupe la liaison en cours ✔.
- L'AML émet un bip à chaque nouvelle connexion ✔.

## Format d'une trame

```
octet :  0  1  2  3  4  5  6  7  8 ...        n-3 n-2 n-1
        FE EF TT LL AB KK IL CC [données]  XX  55  0D  0A
```

| Champ   | Rôle                                                              |
|---------|-------------------------------------------------------------------|
| `FE EF` | début de trame                                                    |
| `TT`    | `0A` requête, `0C` réponse                                        |
| `LL`    | longueur totale de la trame moins 4                               |
| `AB KK` | `AB AA` requête, `AB BB` réponse                                  |
| `IL`    | longueur interne : code + données + checksum + `55`               |
| `CC`    | code de la commande ou de la réponse                              |
| `XX`    | checksum : XOR de `IL` jusqu'à la dernière donnée incluse         |
| `55`    | marqueur de fin interne                                           |
| `0D 0A` | fin de trame                                                      |

Exemple ✔ : statut `FE EF 0A 09 AB AA 04 00 16 12 55 0D 0A`, réponse `FE EF 0C 09 AB BB 04 F0 16 E2 55 0D 0A`.

Dans les tableaux ci-dessous, seules les **données** sont indiquées.

## Commandes

Une requête qui a une réponse reçoit un code différent (colonne *Réponse*). « — » : l'AML ne répond pas, l'application d'origine n'attend rien.

### Appareil et réglages

| Requête | Réponse | Données envoyées | Données reçues |
|---|---|---|---|
| `00` statut ✔ | `F0` | `16` | autorisation : `16` (22) = contrôle accordé |
| `12` lire réglages ✔ | `E2` | — | display, date, time, feedback, fade |
| `02` écrire réglages ✔ | `F2` | display, date, time, feedback, fade | — |
| `01` mise à l'heure | `F1` | s, min, h, jour sem. (0 = dim.), jour, mois, année − 2000, `02` | — |

Valeurs des réglages :
- `display` (luminosité de l'affichage) : 0 = L, 1 = H, 2 = auto ;
- `date` (format d'affichage) : 0 = 24 h, 1 = 12 h ;
- `time` : 0 = auto, 1 = manuel (l'application remet l'AML à l'heure en passant à auto). **En manuel, l'AML ne répond pas à la mise à l'heure `01`** ✔ ;
- `feedback`, `fade` : **inversés**, 0 = activé, 1 = désactivé.

Les cinq réglages s'écrivent toujours ensemble.

### Alarmes

| Requête | Réponse | Données envoyées | Données reçues |
|---|---|---|---|
| `03` lire ✔ | `F3` | index \| `80`, `02` | voir ci-dessous |
| `03` écrire ✔ | `F3` | index, enable, min, h, jours, sonnerie, volume, snooze, sunrise, durée, luminosité | — |
| `25` état | `C5` | — | sunrise en cours, alarme en cours |
| `28` snooze | `C8` | — | résultat |
| `29` arrêt | `C9` | — | résultat |

- index : `01` = alarme 1 (`Alarm/1`), `07` = alarme 2 (`Alarm/2`). En lecture, on ajoute `80`.
- Données reçues en lecture : index \| `80`, enable, **minute, heure** (dans cet ordre), jours, sonnerie, volume, snooze, sunrise, durée du lever de soleil, luminosité.
- jours : un bit par jour, bit 0 = dimanche, bit 1 = lundi … bit 6 = samedi.
- snooze : index dans 1, 2, 5, 10, 20, 30 minutes.
- sonnerie : index (0 = Buzzer, 1 = Radio, 2 et suivants = mélodies).

### Éclairage

Premier octet des commandes : la cible, `01` = Light (lampe principale), `02` = Mood (ambiance).

| Requête | Réponse | Données envoyées | Données reçues |
|---|---|---|---|
| `30` lire ✔ | `D0` | cible | voir ci-dessous |
| `37` allumer | — | cible | |
| `35` éteindre | — | cible | |
| `31` luminosité | — | cible, 0–100 | |
| `38` minuteur actif | — | cible | |
| `36` minuteur inactif | — | cible | |
| `33` durée du minuteur | — | cible, minutes | |
| `32` couleur (Mood) | — | R, G, B | |
| `34` scène (Mood) | — | index de scène | |

- Réponse de lecture ✔ : cible, enable, luminosité, minuteur actif, durée. Pour Mood seulement, elle continue avec R, G, B, scène (9 octets en tout, 5 pour Light).
- Pour Light, le minuteur est le « coucher de soleil » (extinction progressive).
- Allumer Mood (`37 02`) relance la scène 2 (Rainbow), même s'il était déjà allumé ✔. L'application d'origine en tient compte.
- Scène 0 (Off) = couleur fixe, sans animation.
- Scènes : 0 Off, 1 Random, 2 Rainbow, 3 Rainbow Slow, 4 Fusion, 5 Pulse, 6 Wave, 7 Chill, 8 Action, 9 Forest, 10 Summer.

### Radio FM

| Requête | Réponse | Données envoyées | Données reçues |
|---|---|---|---|
| `07` lire ✔ | `F7` | `02` | enable, présélection (1–10), fréquence (2 octets), volume, minuteur actif, durée |
| `08` marche/arrêt | `F8` | 1 / 0 | |
| `0A` changer la fréquence | `FA` | recherche (0 pas fin, 1 recherche), sens (0 haut, 1 bas) | rappel des 2 octets, fréquence (2 octets) |
| `09` présélection | `F9` | numéro 1–10 | |
| `0B` volume | — | 1–10 | |
| `0C` minuteur actif | — | 1 / 0 | |
| `0D` durée du minuteur | — | minutes | |
| `04` liste des présélections | `F4` | `01 02` | 10 fréquences, volume, minuteur |
| `0E` enregistrer présélection | `FE` | — | |

Fréquence : entier sur 2 octets, poids fort d'abord, en dixièmes de MHz (`03 F7` = 1015 = 101,5 MHz). Il n'existe pas de commande pour choisir directement une fréquence.

### Haut-parleur Bluetooth (Media Player)

| Requête | Réponse | Données envoyées | Données reçues |
|---|---|---|---|
| `10` activer | `E0` | — | type |
| `24` désactiver | — | — | |
| `0F` état A2DP | `FF` | — | type, adresse (12 caractères) |
| `17` lire | `E7` | — | volume, minuteur actif, durée |
| `14` volume | — | volume | |
| `15` minuteur actif | `E5` | 1 / 0 | succès |
| `16` durée du minuteur | — | minutes | |

## Correspondance requête / réponse

Le code de réponse se déduit du code de requête :

| Requêtes | Réponse |
|---|---|
| `0X` | `FX` |
| `1X` | `EX` |
| `2X` | `CX` |
| `3X` | `DX` |

Les messages spontanés suivent la même règle : `C7` (alarme) est la réponse de `27`, `EB` celle de `1B`, etc.

## Codes trouvés par exploration

Le 12/10/2026, tous les codes que l'application n'utilise pas, de `00` à `7F`, ont été envoyés sans données au matériel. Après chacun, l'état complet a été relu. Seuls ceux-ci ont répondu ✔ :

| Requête | Réponse | Données reçues | Interprétation |
|---|---|---|---|
| `05` | `F5` | `01` | inconnue |
| `06` | `F6` | `00` | inconnue |
| `11` | `E1` | `01` | inconnue (ce n'est pas l'heure) |
| `20` | `C0` | comme `07` | état de la radio |
| `21` | `C1` | comme `17` | état de la musique ; **a changé la durée du minuteur musique** (1 → 10) |
| `22` | `C2` | comme `30 01` | état de Light |
| `23` | `C3` | comme `30 02`, mais premier octet `01` | état de Mood |

Aucune autre réponse, et aucun autre changement d'état, entre `00` et `7F`.

Les codes inconnus de `00` à `3B` ont ensuite été réessayés avec 1 à 6 octets `00` en paramètre : les réponses sont strictement identiques à celles sans paramètre, et aucun autre code ne répond. `01` sans données (mise à l'heure vide) est seulement acquitté (`F1`, sans données).

Le 12/10/2026 encore, dans le noir et lumières éteintes, chaque code inconnu de `00` à `3F` (et le champ `display` des réglages) a reçu un octet `0A`, puis `64`, puis `FF`, à 8 s d'intervalle, en observant l'affichage : aucun effet visible. `05`, `06` et `11` renvoient toujours la même valeur, quel que soit le paramètre : ce sont des lectures. Le champ `display` accepte et conserve une valeur hors de 0–2 (relue telle quelle), sans effet visible sur l'affichage.

**Aucune commande connue ne permet de relire l'heure de l'AML, ni de régler la luminosité de l'affichage autrement que par L / H / A.**

## Messages spontanés

Envoyés par l'AML sans requête, une fois la poignée de main faite. SweetDawn les transforme en événements pour ses clients (`src/app/handlers.c`, README section « Events ») :

| Code | Données | Événement |
|---|---|---|
| `C6` | n° d'alarme | début du lever de soleil ✔ |
| `C7` | n° d'alarme | l'alarme sonne ✔ |
| `CA` | — | l'éclairage a changé : le relire avec `30` ✔ |
| `EB` | ? | fin du minuteur de Light |
| `EC` | ? | fin du minuteur de Mood |
| `ED` | ? | fin du minuteur radio |
| `EE` | ? | fin du minuteur musique |

Observé sur le matériel le 12/10/2026 :

- `C6 02` à 19:42:00 et `C7 02` à 19:44:00 pour l'alarme 2 réglée à 19:44 avec un lever de soleil de 2 min : la donnée est le numéro de l'alarme (1 ou 2), pas son index de protocole.
- `CA` arrive à chaque changement d'éclairage fait sur l'appareil, y compris par le **capteur de geste** (un mouvement devant l'AML change la lumière), souvent en rafale. L'application d'origine y répond en relisant la lampe.
- Taper l'AML pendant qu'il sonne déclenche un **snooze**, mais **n'envoie aucun message** : la sonnerie s'arrête et le lever de soleil repart de zéro pour la durée du snooze. Seule la lecture d'état `25` le révèle.
- État de l'alarme (`25` → `C5`, deux octets) ✔ :
  1. n° de l'alarme qui **tient la lampe** (lever de soleil, puis sonnerie ; 0 = aucune) ;
  2. n° de l'alarme qui **sonne** (0 = aucune).

  Pendant le snooze : `02 00` ; après `Alarm/Stop` : `00 00`. Pendant que l'alarme 1 sonne : `01 01`.
- Pendant le lever de soleil, la **luminosité de la lampe** (lecture `30 01`) monte de 0 à la luminosité de l'alarme : 4, 14, 24… 100 en 2 minutes ✔. La lampe reste « éteinte » (`enable` = 0).
- **Chevauchement des deux alarmes** (alarme 1 à 20:47, alarme 2 à 20:48, levers de 2 min) ✔ :
  - un seul lever de soleil à la fois : celui de l'alarme 2, qui aurait dû commencer pendant celui de l'alarme 1, est ignoré (aucun `C6 02`) ;
  - quand l'alarme 2 sonne (`C7 02`), elle prend la main : l'état passe de `01 01` à `01 02`, et l'alarme 1 ne resonne pas (encore rien 7 minutes plus tard, pour un snooze de 5 min) ;
  - une alarme peut sonner plus de 6 minutes sans s'arrêter d'elle-même.
- Arrêt (`29` → `C9`) : répond `01` quand une alarme était en cours ✔.
