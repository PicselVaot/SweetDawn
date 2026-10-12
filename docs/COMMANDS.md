# Commandes SweetDawn

Toutes les commandes acceptées par le service, avec `sweetdawn-ctl` ou par IPC (voir le [README](../README.md#ipc-protocol)).

```bash
sweetdawn-ctl <commande> [valeur]
```

Trois sortes de commandes :

- **lecture** : un groupe sans valeur (`sweetdawn-ctl Light`) renvoie son état en JSON ;
- **réglage** : une commande avec une valeur (`sweetdawn-ctl Light/Brightness 75`) ;
- **action** : une commande sans valeur (`sweetdawn-ctl Radio/>>`).

Chaque valeur est vérifiée avant d'être envoyée à l'AML. Une valeur hors limites est refusée avec `INVALID_VALUE`. Les noms qui contiennent un espace s'écrivent entre guillemets : `sweetdawn-ctl "Alarm/1/Tone volume" 5`.

Cette liste vient de l'arbre défini dans [`src/command/aml_commands.c`](../src/command/aml_commands.c). La commande `sweetdawn-ctl commands` renvoie la même chose au format JSON, directement depuis le service.

## Service

| Commande | Valeur | Effet |
|---|---|---|
| `status` | — | État de la liaison, derniers réglages connus, heure de l'AML (`clock`) et état du réveil (`alarm_state`), sans interroger l'AML |
| `commands` | — | Arbre complet des commandes (types, limites, choix) |
| `events` | — | Avec `sweetdawn-ctl` : affiche les événements en direct (Ctrl+C pour arrêter). Par IPC, la commande s'appelle `subscribe` |

## Settings — réglages de l'appareil

| Commande | Valeur | Effet |
|---|---|---|
| `Settings` | — | **Lecture** : `display_brightness`, `display_time`, `time`, `feedback`, `fade` |
| `Settings/Display brightness` | `L`, `H`, `A` | Luminosité de l'affichage : faible, forte, automatique |
| `Settings/Display time` | `24`, `12` | Format de l'heure affichée |
| `Settings/Time` | `auto`, `manual` | Mode de l'heure ; `auto` remet aussi l'AML à l'heure |
| `Settings/Feedback` | `true`, `false` | Bip des boutons |
| `Settings/Fade` | `true`, `false` | Fondu lumineux |
| `Settings/Sync time` | — | **Action** : met l'AML à l'heure du Raspberry Pi |
| `Settings/Set time` | `HH:MM`, `HH:MM:SS` ou `AAAA-MM-JJ HH:MM` | Met l'AML à l'heure choisie ; sans date, garde celle du jour |

**Heure de l'AML.** L'AML ne permet pas de relire son heure : on peut seulement la régler. SweetDawn la connaît quand même, puisque c'est lui qui la règle. Il retient l'écart avec l'heure du Raspberry Pi (`status` → `clock`). En mode `auto`, le service remet l'AML à l'heure tout seul : dès la connexion, puis une fois par jour, en gardant l'écart choisi avec `Set time`. En mode `manual`, l'heure a pu être changée aux boutons : elle est alors inconnue, et `next` aussi.

En mode `manual`, l'heure se règle avec les boutons de l'AML, qui ignore alors toute mise à l'heure venue de l'extérieur. `Sync time` et `Set time` repassent donc d'abord en mode `auto`.

## Alarm — réveils

`<n>` vaut `1` ou `2`.

| Commande | Valeur | Effet |
|---|---|---|
| `Alarm` | — | **Lecture** des deux alarmes |
| `Alarm/<n>` | — | **Lecture** : `enable`, `time`, `days`, `snooze`, `tone`, `tone_volume`, `sunrise`, `sunrise_duration`, `sunrise_brightness`, et le prochain déclenchement : `next`, `in`, `in_s` |
| `Alarm/<n>/Enable` | `true`, `false` | Active ou désactive l'alarme |
| `Alarm/<n>/Time` | `HH:MM` | Heure de l'alarme, ex. `07:30` |
| `Alarm/<n>/Days` | 0 à 127 | Jours actifs (voir ci-dessous) |
| `Alarm/<n>/Snooze` | `1`, `2`, `5`, `10`, `20`, `30` | Durée de répétition, en minutes |
| `Alarm/<n>/Tone` | `Buzzer`, `Radio`, `Melody 1`, `Birds`, `Bells`, `Piano`, `Melody 2` | Sonnerie |
| `Alarm/<n>/Tone volume` | 1 à 10 | Volume de la sonnerie |
| `Alarm/<n>/Sunrise` | `true`, `false` | Lever de soleil avant l'alarme |
| `Alarm/<n>/Sunrise duration` | 2 à 60 | Durée du lever de soleil, en minutes |
| `Alarm/<n>/Sunrise brightness` | 1 à 100 | Luminosité atteinte à la fin du lever de soleil |
| `Alarm/Stop` | — | **Action** : arrête l'alarme qui sonne |
| `Alarm/Snooze` | — | **Action** : répète l'alarme qui sonne plus tard |

**Prochain déclenchement** : `next` est la date et l'heure réelles du prochain réveil (`2026-10-13T07:00:00+02:00`), `in` le délai lisible (`11 h 05 min`) et `in_s` ce délai en secondes. `next` vaut `null` si l'alarme est désactivée, ou si l'heure de l'AML n'est pas connue (voir ci-dessous).

**Jours** : additionner les valeurs des jours voulus.

| Dim | Lun | Mar | Mer | Jeu | Ven | Sam |
|---|---|---|---|---|---|---|
| 1 | 2 | 4 | 8 | 16 | 32 | 64 |

Exemples : du lundi au vendredi = 2 + 4 + 8 + 16 + 32 = **62** ; le week-end = 1 + 64 = **65** ; tous les jours = **127**.

## Light — lampe principale

| Commande | Valeur | Effet |
|---|---|---|
| `Light` | — | **Lecture** : `enable`, `brightness`, `sunset`, `sunset_duration` |
| `Light/Enable` | `true`, `false` | Allume ou éteint |
| `Light/Brightness` | 0 à 100 | Luminosité |
| `Light/Sunset` | `true`, `false` | Coucher de soleil : extinction progressive |
| `Light/Sunset duration` | 1 à 60 | Durée du coucher de soleil, en minutes |

## Mood — éclairage d'ambiance

| Commande | Valeur | Effet |
|---|---|---|
| `Mood` | — | **Lecture** : `enable`, `brightness`, `timer`, `timer_duration`, `color`, `scene` |
| `Mood/Enable` | `true`, `false` | Allume ou éteint |
| `Mood/Color` | `R,G,B` | Couleur, chaque composante de 0 à 255, ex. `255,120,0` |
| `Mood/Brightness` | 0 à 100 | Luminosité |
| `Mood/Scene` | `Off`, `Random`, `Rainbow`, `Rainbow Slow`, `Fusion`, `Pulse`, `Wave`, `Chill`, `Action`, `Forest`, `Summer` | Scène animée |
| `Mood/Timer` | `true`, `false` | Minuteur d'extinction |
| `Mood/Timer duration` | 1 à 120 | Durée du minuteur, en minutes |

À savoir :

- **Allumer l'ambiance lance toujours la scène `Rainbow`**, même si elle est déjà allumée. Pour une autre scène ou une couleur, l'envoyer **après** `Mood/Enable true`.
- La scène `Off` n'éteint pas l'ambiance : elle arrête l'animation et affiche la couleur fixe (`Mood/Color`).

## Radio — radio FM

| Commande | Valeur | Effet |
|---|---|---|
| `Radio` | — | **Lecture** : `enable`, `channel`, `frequency` (MHz), `volume`, `sleep_timer`, `timer_duration` |
| `Radio/Enable` | `true`, `false` | Allume ou éteint la radio |
| `Radio/>` | — | **Action** : +0,1 MHz ; renvoie la nouvelle `frequency` |
| `Radio/<` | — | **Action** : −0,1 MHz |
| `Radio/>>` | — | **Action** : cherche la station suivante |
| `Radio/<<` | — | **Action** : cherche la station précédente |
| `Radio/Channel` | 1 à 10 | Rappelle une présélection |
| `Radio/Volume` | 1 à 10 | Volume |
| `Radio/Sleep timer` | `true`, `false` | Arrêt programmé |
| `Radio/Timer duration` | 1 à 60 | Délai avant l'arrêt, en minutes |

Il n'existe pas de commande pour choisir directement une fréquence : le protocole de l'AML ne le permet pas.

## Media Player — haut-parleur Bluetooth

| Commande | Valeur | Effet |
|---|---|---|
| `Media Player/Enable` | `true`, `false` | Active ou désactive le mode haut-parleur Bluetooth |

La lecture de l'état (`Media Player`) n'est pas encore disponible.

## Commandes sans accusé de réception

L'AML ne confirme pas certaines commandes : allumer, éteindre, luminosité, couleur, scène, minuteurs, volume radio. Pour elles, `OK` veut seulement dire que la commande est partie. Relire le groupe permet de vérifier l'effet. Le détail est dans [PROTOCOL.md](PROTOCOL.md).
