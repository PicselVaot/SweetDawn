# Protocole Bluetooth de l'AML005

Référence du dialogue entre SweetDawn et l'AML005, reconstituée à partir de l'application Android d'origine (SunBeam, décompilée), puis vérifiée sur le matériel quand c'est indiqué (✔).

Dans le code, les codes sont définis dans [`src/aml/protocol.h`](../src/aml/protocol.h) et chaque commande est implémentée dans [`src/core/control.c`](../src/core/control.c).

## Liaison

- Bluetooth classique, RFCOMM, canal 1 en général (voir `config/aml.json`).
- **L'AML ne répond à rien tant qu'il n'a pas reçu la requête de statut** (`00 16`) ✔. SweetDawn l'envoie à chaque connexion.
- L'AML n'accepte qu'une connexion à la fois ✔ : quand le service tourne, l'application mobile et `bin/sweetdawn` ne peuvent pas se connecter.

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
- `time` : 0 = auto, 1 = manuel (l'application remet l'AML à l'heure en passant à auto) ;
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

- index : `01` = Alarm 1, `07` = Alarm 2. En lecture, on ajoute `80`.
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

## Messages spontanés

Envoyés par l'AML sans requête (pas encore exploités par SweetDawn) :

| Code | Événement |
|---|---|
| `C6` | début du lever de soleil |
| `C7` | l'alarme sonne |
| `CA` | l'éclairage a changé (à relire) |
| `EB` | fin du minuteur de Light |
| `EC` | fin du minuteur de Mood |
| `ED` | fin du minuteur radio |
| `EE` | fin du minuteur musique |
