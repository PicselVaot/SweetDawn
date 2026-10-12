#!/usr/bin/env python3
"""
sweetdawn_mqtt.py — Pont entre Home Assistant (MQTT) et le service SweetDawn

    Home Assistant <--MQTT--> ce pont <--socket IPC--> sweetdawn-service <--Bluetooth--> AML005

Le pont ne parle jamais au Bluetooth : il envoie au service les mêmes
commandes que `sweetdawn-ctl` (voir docs/COMMANDS.md).

Ce qu'il fait :
  - annonce chaque fonction de l'AML à Home Assistant (MQTT discovery) :
    lampe, ambiance, radio, alarmes, réglages ;
  - exécute les commandes reçues de Home Assistant ;
  - relit l'état de l'AML toutes les `poll_seconds` secondes, et une
    seconde après chaque commande (l'AML ne confirme pas toutes ses
    commandes : relire est le seul moyen d'afficher l'état réel).

Configuration : config/mqtt.json (voir config/mqtt.example.json).
Lancement : par systemd (systemd/sweetdawn-mqtt.service), ou à la main
depuis la racine du projet : python3 mqtt/sweetdawn_mqtt.py

Organisation du fichier :
  1. Configuration
  2. Client du service SweetDawn (socket IPC)
  3. Description des entités Home Assistant
  4. Pont : MQTT, file de commandes, relecture périodique
"""

import json
import logging
import os
import queue
import signal
import socket
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from pathlib import Path
from typing import Callable, Optional

import paho.mqtt.client as mqtt

log = logging.getLogger("sweetdawn-mqtt")


# ============================================================================
# 1. Configuration
# ============================================================================

PROJECT_DIR = Path(__file__).resolve().parent.parent
CONFIG_PATH = PROJECT_DIR / "config" / "mqtt.json"

# Même socket par défaut que le service (src/ipc/ipc_server.h).
DEFAULT_SOCKET = "/run/sweetdawn/sweetdawn.sock"

# Préfixe des topics de ce pont, et préfixe de découverte de Home Assistant.
BASE = "sweetdawn/aml005"
DISCOVERY_PREFIX = "homeassistant"

# Topics de disponibilité :
#   BRIDGE : ce pont est-il en ligne ? (message de testament si il meurt)
#   AML    : le service est-il joignable ET connecté à l'AML ?
BRIDGE_TOPIC = f"{BASE}/bridge"
AML_TOPIC = f"{BASE}/status"

# Délai entre une commande et la relecture de son groupe.
REFRESH_DELAY_S = 1.0

# Appareil Home Assistant. Les identifiants sont ceux de l'ancien pont
# (~/MQTT/aml.py), pour que Home Assistant garde les mêmes entités.
DEVICE = {
    "identifiers": ["raspberry_aml005"],
    "name": "AML005",
    "manufacturer": "AML",
    "model": "AML005",
    "via_device": "raspberry_gateway",
}


def load_config():
    """Lit config/mqtt.json et le fichier d'identifiants qu'il désigne."""
    with open(CONFIG_PATH, encoding="utf-8") as fp:
        config = json.load(fp)

    with open(os.path.expanduser(config["credentials"]), encoding="utf-8") as fp:
        credentials = json.load(fp)

    return {
        "broker": config["broker"],
        "port": int(config.get("port", 1883)),
        "user": credentials["user"],
        "password": credentials["passw"],
        "poll_seconds": float(config.get("poll_seconds", 30)),
        "socket": os.environ.get("SWEETDAWN_SOCKET") or DEFAULT_SOCKET,
    }


# ============================================================================
# 2. Client du service SweetDawn
# ============================================================================

class ServiceError(Exception):
    """Le service a refusé la commande ou n'a pas pu l'exécuter."""

    def __init__(self, code, message):
        super().__init__(f"{code} : {message}")
        self.code = code


class Service:
    """Envoie une commande au service et renvoie son résultat (un dict)."""

    def __init__(self, path):
        self.path = path
        self.next_id = 0

    def request(self, command, value=None, timeout=20):
        self.next_id += 1
        message = {
            "type": "request",
            "id": self.next_id,
            "command": command,
            "args": [] if value is None else [value],
        }

        # Une connexion par commande : simple, et sans état à resynchroniser
        # si le service redémarre.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(timeout)
            sock.connect(self.path)
            sock.sendall((json.dumps(message) + "\n").encode())

            data = b""
            while not data.endswith(b"\n"):
                chunk = sock.recv(65536)
                if not chunk:
                    raise ConnectionError("le service a fermé la connexion")
                data += chunk

        response = json.loads(data)

        if not response.get("ok"):
            error = response.get("error", {})
            raise ServiceError(error.get("code", "?"), error.get("message", ""))

        return response.get("result", {})


# ============================================================================
# 3. Description des entités Home Assistant
# ============================================================================
#
# Une entité = une fonction de l'AML vue par Home Assistant (une lampe, un
# interrupteur, un curseur...). Elle a un ou plusieurs canaux :
#
#   canal ""           -> state_topic / command_topic
#   canal "brightness" -> brightness_state_topic / brightness_command_topic
#   ...
#
# Chaque canal sait :
#   get(cache)          lire sa valeur dans le dernier état connu de l'AML
#                       (cache = {"Light": {...}, "Alarm": {...}, ...}) ;
#   set(payload, cache) traduire un message de Home Assistant en commandes
#                       SweetDawn : liste de (commande, valeur).

@dataclass
class Channel:
    get: Optional[Callable[[dict], Optional[str]]] = None
    set: Optional[Callable[[str, dict], list]] = None


@dataclass
class Entity:
    component: str           # light, switch, number, select, text, button...
    key: str                 # identifiant court, unique
    name: str                # nom affiché dans Home Assistant
    group: Optional[str]     # groupe à relire après une commande
    channels: dict
    config: dict = field(default_factory=dict)
    unique_id: Optional[str] = None
    discovery_topic: Optional[str] = None
    needs_aml: bool = True   # indisponible quand l'AML est déconnecté
    live: bool = False       # valeur recalculée à chaque tour, sans relecture


def value_in(cache, *path):
    """cache["Light"]["enable"]... ou None si un niveau manque."""
    node = cache
    for key in path:
        if not isinstance(node, dict) or key not in node:
            return None
        node = node[key]
    return node


def on_off(value):
    return None if value is None else ("ON" if value else "OFF")


def text(value):
    return None if value is None else str(value)


def or_none(value):
    """
    Valeur d'un capteur pouvant être inconnu : Home Assistant comprend
    "None" comme « inconnu » (publier rien laisserait l'ancienne valeur).
    """
    return "None" if value is None else str(value)


def aml_clock(cache):
    """
    Heure actuelle de l'AML (HH:MM) : heure du Raspberry Pi plus l'écart
    connu par le service, ou None si cet écart est inconnu.
    """
    if not value_in(cache, "status", "clock", "known"):
        return None
    offset = value_in(cache, "status", "clock", "offset_s") or 0
    return (datetime.now() + timedelta(seconds=offset)).strftime("%H:%M")


def next_alarm(cache):
    """Prochain déclenchement parmi les deux alarmes (ISO 8601), ou None."""
    alarms = [value_in(cache, "Alarm", n) or {} for n in ("1", "2")]
    upcoming = [a for a in alarms if a.get("next") is not None]
    if not upcoming:
        return None
    return min(upcoming, key=lambda a: a["in_s"])["next"]


def bool_payload(payload):
    if payload not in ("ON", "OFF"):
        raise ValueError(f"ON ou OFF attendu, reçu {payload!r}")
    return "true" if payload == "ON" else "false"


def int_payload(payload):
    # Home Assistant peut envoyer "12" ou "12.0".
    return str(int(float(payload)))


# --- Fabriques des entités courantes ---------------------------------------

def switch(key, name, group, path, field_path, **config):
    return Entity("switch", key, name, group, {
        "": Channel(
            get=lambda c: on_off(value_in(c, group, *field_path)),
            set=lambda p, c: [(path, bool_payload(p))],
        ),
    }, {"payload_on": "ON", "payload_off": "OFF", **config})


def number(key, name, group, path, field_path, minimum, maximum, **config):
    return Entity("number", key, name, group, {
        "": Channel(
            get=lambda c: text(value_in(c, group, *field_path)),
            set=lambda p, c: [(path, int_payload(p))],
        ),
    }, {"min": minimum, "max": maximum, "step": 1, "mode": "slider", **config})


def select(key, name, group, path, field_path, options, **config):
    return Entity("select", key, name, group, {
        "": Channel(
            get=lambda c: text(value_in(c, group, *field_path)),
            set=lambda p, c: [(path, p)],
        ),
    }, {"options": options, **config})


def button(key, name, path, group=None, **config):
    return Entity("button", key, name, group, {
        "": Channel(set=lambda p, c: [(path, None)]),
    }, {"payload_press": "PRESS", **config})


def day_switch(number_, bit, day):
    """Un jour d'une alarme : un bit du masque Days (bit 0 = dimanche)."""
    def get(cache):
        days = value_in(cache, "Alarm", str(number_), "days")
        return None if days is None else on_off(days & (1 << bit))

    def set_(payload, cache):
        days = value_in(cache, "Alarm", str(number_), "days")
        if days is None:
            raise ValueError("jours de l'alarme encore inconnus")
        days = days | (1 << bit) if payload == "ON" else days & ~(1 << bit)
        return [(f"Alarm/{number_}/Days", str(days))]

    return Entity("switch", f"alarm{number_}_day{bit}",
                  f"Alarme {number_} {day}", "Alarm",
                  {"": Channel(get=get, set=set_)},
                  {"payload_on": "ON", "payload_off": "OFF",
                   "entity_category": "config"})


# Choix possibles : mêmes listes que l'arbre des commandes
# (src/command/aml_commands.c). Ils sont relus au démarrage depuis le
# service (commande "commands") ; ces valeurs ne servent que de secours.
CHOICES = {
    "Settings/Display brightness": ["L", "H", "A"],
    "Settings/Display time": ["24", "12"],
    "Settings/Time": ["auto", "manual"],
    "Alarm/1/Snooze": ["1", "2", "5", "10", "20", "30"],
    "Alarm/1/Tone": ["Buzzer", "Radio", "Melody 1", "Birds", "Bells",
                     "Piano", "Melody 2"],
    "Mood/Scene": ["Off", "Random", "Rainbow", "Rainbow Slow", "Fusion",
                   "Pulse", "Wave", "Chill", "Action", "Forest", "Summer"],
}

DAYS = ["dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"]


def build_entities():
    config = {"entity_category": "config"}
    entities = []

    # --- Lampe principale --------------------------------------------------
    # Même identifiant et même topic de découverte que l'ancien pont.
    light = Entity("light", "light", "Lumière", "Light", {
        "": Channel(
            get=lambda c: on_off(value_in(c, "Light", "enable")),
            set=lambda p, c: [("Light/Enable", bool_payload(p))],
        ),
        "brightness": Channel(
            get=lambda c: text(value_in(c, "Light", "brightness")),
            set=lambda p, c: [("Light/Brightness", int_payload(p))],
        ),
    }, {"brightness_scale": 100, "payload_on": "ON", "payload_off": "OFF",
        "on_command_type": "first"},
        unique_id="aml005_light",
        discovery_topic=f"{DISCOVERY_PREFIX}/light/aml005/config")
    entities.append(light)
    entities.append(switch("light_sunset", "Coucher de soleil", "Light",
                           "Light/Sunset", ["sunset"], **config))
    entities.append(number("light_sunset_duration", "Durée coucher de soleil",
                           "Light", "Light/Sunset duration", ["sunset_duration"],
                           1, 60, unit_of_measurement="min", **config))

    # --- Ambiance ------------------------------------------------------------
    entities.append(Entity("light", "mood", "Ambiance", "Mood", {
        "": Channel(
            get=lambda c: on_off(value_in(c, "Mood", "enable")),
            set=lambda p, c: [("Mood/Enable", bool_payload(p))],
        ),
        "brightness": Channel(
            get=lambda c: text(value_in(c, "Mood", "brightness")),
            set=lambda p, c: [("Mood/Brightness", int_payload(p))],
        ),
        # Même format des deux côtés : "R,G,B".
        "rgb": Channel(
            get=lambda c: text(value_in(c, "Mood", "color")),
            set=lambda p, c: [("Mood/Color", p)],
        ),
        "effect": Channel(
            get=lambda c: text(value_in(c, "Mood", "scene")),
            set=lambda p, c: [("Mood/Scene", p)],
        ),
    }, {"brightness_scale": 100, "payload_on": "ON", "payload_off": "OFF",
        "on_command_type": "first", "effect_list": CHOICES["Mood/Scene"]}))
    entities.append(switch("mood_timer", "Minuteur ambiance", "Mood",
                           "Mood/Timer", ["timer"], **config))
    entities.append(number("mood_timer_duration", "Durée minuteur ambiance",
                           "Mood", "Mood/Timer duration", ["timer_duration"],
                           1, 120, unit_of_measurement="min", **config))

    # --- Radio ---------------------------------------------------------------
    entities.append(switch("radio", "Radio", "Radio", "Radio/Enable",
                           ["enable"], icon="mdi:radio"))
    entities.append(number("radio_volume", "Volume radio", "Radio",
                           "Radio/Volume", ["volume"], 1, 10,
                           icon="mdi:volume-high"))
    entities.append(number("radio_channel", "Présélection radio", "Radio",
                           "Radio/Channel", ["channel"], 1, 10, mode="box",
                           icon="mdi:numeric"))
    entities.append(Entity("sensor", "radio_frequency", "Fréquence radio",
                           "Radio", {"": Channel(
                               get=lambda c: text(value_in(c, "Radio", "frequency")))},
                           {"unit_of_measurement": "MHz",
                            "icon": "mdi:radio-tower"}))
    for key, name, path, icon in (
            ("radio_seek_up", "Station suivante", "Radio/>>", "mdi:skip-next"),
            ("radio_seek_down", "Station précédente", "Radio/<<", "mdi:skip-previous"),
            ("radio_tune_up", "Fréquence +0,1", "Radio/>", "mdi:chevron-right"),
            ("radio_tune_down", "Fréquence -0,1", "Radio/<", "mdi:chevron-left")):
        entities.append(button(key, name, path, group="Radio", icon=icon))
    entities.append(switch("radio_sleep", "Arrêt programmé radio", "Radio",
                           "Radio/Sleep timer", ["sleep_timer"], **config))
    entities.append(number("radio_sleep_duration", "Durée arrêt radio", "Radio",
                           "Radio/Timer duration", ["timer_duration"], 1, 60,
                           unit_of_measurement="min", **config))

    # --- Alarmes -------------------------------------------------------------
    for n in (1, 2):
        a = str(n)
        entities.append(switch(f"alarm{n}", f"Alarme {n}", "Alarm",
                               f"Alarm/{n}/Enable", [a, "enable"],
                               icon="mdi:alarm"))
        entities.append(Entity("text", f"alarm{n}_time", f"Alarme {n} heure",
                               "Alarm", {"": Channel(
                                   get=lambda c, a=a: text(value_in(c, "Alarm", a, "time")),
                                   set=lambda p, c, n=n: [(f"Alarm/{n}/Time", p)])},
                               {"pattern": r"^([01]\d|2[0-3]):[0-5]\d$",
                                "min": 5, "max": 5, "icon": "mdi:clock-outline"}))
        for bit, day in enumerate(DAYS):
            entities.append(day_switch(n, bit, day))
        entities.append(select(f"alarm{n}_snooze", f"Alarme {n} snooze",
                               "Alarm", f"Alarm/{n}/Snooze", [a, "snooze"],
                               CHOICES["Alarm/1/Snooze"], **config))
        entities.append(select(f"alarm{n}_tone", f"Alarme {n} sonnerie",
                               "Alarm", f"Alarm/{n}/Tone", [a, "tone"],
                               CHOICES["Alarm/1/Tone"], **config))
        entities.append(number(f"alarm{n}_volume", f"Alarme {n} volume",
                               "Alarm", f"Alarm/{n}/Tone volume",
                               [a, "tone_volume"], 1, 10, **config))
        entities.append(switch(f"alarm{n}_sunrise", f"Alarme {n} lever de soleil",
                               "Alarm", f"Alarm/{n}/Sunrise", [a, "sunrise"],
                               **config))
        entities.append(number(f"alarm{n}_sunrise_duration",
                               f"Alarme {n} durée lever de soleil", "Alarm",
                               f"Alarm/{n}/Sunrise duration",
                               [a, "sunrise_duration"], 2, 60,
                               unit_of_measurement="min", **config))
        entities.append(number(f"alarm{n}_sunrise_brightness",
                               f"Alarme {n} luminosité lever de soleil", "Alarm",
                               f"Alarm/{n}/Sunrise brightness",
                               [a, "sunrise_brightness"], 1, 100,
                               unit_of_measurement="%", **config))
    # Prochains déclenchements : Home Assistant affiche « dans 11 heures ».
    # Inconnus tant que SweetDawn n'a pas réglé l'heure de l'AML.
    timestamp = {"device_class": "timestamp", "icon": "mdi:alarm"}
    entities.append(Entity("sensor", "next_alarm", "Prochaine alarme", "Alarm",
                           {"": Channel(get=lambda c: or_none(next_alarm(c)))},
                           dict(timestamp)))
    for n in ("1", "2"):
        entities.append(Entity("sensor", f"alarm{n}_next",
                               f"Alarme {n} prochain déclenchement", "Alarm",
                               {"": Channel(get=lambda c, n=n: or_none(
                                   value_in(c, "Alarm", n, "next")))},
                               dict(timestamp)))

    entities.append(button("alarm_stop", "Arrêter l'alarme", "Alarm/Stop",
                           icon="mdi:alarm-off"))
    entities.append(button("alarm_snooze", "Snooze", "Alarm/Snooze",
                           icon="mdi:alarm-snooze"))

    # --- Réglages ------------------------------------------------------------
    # Lus dans "status" : aucune requête Bluetooth.
    entities.append(select("display_brightness", "Luminosité affichage",
                           "status", "Settings/Display brightness",
                           ["settings", "display_brightness"],
                           CHOICES["Settings/Display brightness"], **config))
    entities.append(select("display_time", "Format heure", "status",
                           "Settings/Display time", ["settings", "display_time"],
                           CHOICES["Settings/Display time"], **config))
    entities.append(select("time_mode", "Mode heure", "status",
                           "Settings/Time", ["settings", "time"],
                           CHOICES["Settings/Time"], **config))
    entities.append(switch("feedback", "Bip des boutons", "status",
                           "Settings/Feedback", ["settings", "feedback"], **config))
    entities.append(switch("fade", "Fondu lumineux", "status",
                           "Settings/Fade", ["settings", "fade"], **config))
    entities.append(button("sync_time", "Mettre à l'heure", "Settings/Sync time",
                           entity_category="config", icon="mdi:clock-check"))
    # Heure choisie à la main. L'AML ne permet pas de relire son heure :
    # sans topic d'état, Home Assistant affiche la dernière valeur envoyée.
    entities.append(Entity("text", "set_time", "Régler l'heure", "status", {
        "": Channel(set=lambda p, c: [("Settings/Set time", p)]),
    }, {"pattern": r"^(\d{4}-\d{2}-\d{2} )?([01]\d|2[0-3]):[0-5]\d(:[0-5]\d)?$",
        "min": 5, "max": 19, "icon": "mdi:clock-edit-outline"}))

    # Heure de l'AML, recalculée chaque seconde et publiée à chaque
    # changement de minute (live) : aucune requête vers l'AML.
    entities.append(Entity("sensor", "aml_time", "Heure de l'AML", "status",
                           {"": Channel(get=lambda c: or_none(aml_clock(c)))},
                           {"icon": "mdi:clock-digital"}, live=True))

    # Écart entre l'horloge de l'AML et celle du Raspberry Pi (0 = à
    # l'heure) ; inconnu en mode manuel ou avant la première mise à l'heure.
    entities.append(Entity("sensor", "clock_offset", "Écart horloge AML", "status",
                           {"": Channel(get=lambda c: or_none(
                               value_in(c, "status", "clock", "offset_s")))},
                           {"unit_of_measurement": "s",
                            "device_class": "duration",
                            "entity_category": "diagnostic",
                            "icon": "mdi:clock-alert-outline"}))

    # --- Haut-parleur Bluetooth ----------------------------------------------
    # L'état n'est pas lisible : Home Assistant le suppose (mode optimiste).
    entities.append(Entity("switch", "speaker", "Haut-parleur Bluetooth", None, {
        "": Channel(set=lambda p, c: [("Media Player/Enable", bool_payload(p))]),
    }, {"payload_on": "ON", "payload_off": "OFF", "optimistic": True,
        "icon": "mdi:speaker-bluetooth"}))

    # --- Liaison -------------------------------------------------------------
    # Disponible même quand l'AML est déconnecté : c'est elle qui le dit.
    entities.append(Entity("binary_sensor", "connected", "Connexion Bluetooth",
                           "status", {"": Channel(
                               get=lambda c: on_off(value_in(c, "status", "connected")))},
                           {"device_class": "connectivity",
                            "payload_on": "ON", "payload_off": "OFF",
                            "entity_category": "diagnostic"},
                           needs_aml=False))

    return entities


def apply_service_choices(tree):
    """Met CHOICES à jour avec les listes réelles du service (commande "commands")."""
    def walk(node, path):
        for child in node.get("children", []):
            child_path = f"{path}/{child['name']}" if path else child["name"]
            if child_path in CHOICES and child.get("choices"):
                CHOICES[child_path] = child["choices"]
            walk(child, child_path)

    walk(tree, "")


# ============================================================================
# 4. Pont
# ============================================================================

class Bridge:
    """
    Un seul fil parle au service : le fil principal (run). Les messages
    MQTT, reçus dans le fil de paho, sont simplement mis en file.
    """

    GROUPS = ("status", "Light", "Mood", "Radio", "Alarm")

    def __init__(self, config):
        self.config = config
        self.service = Service(config["socket"])
        self.entities = []
        self.routes = {}          # topic de commande -> (entité, canal)
        self.cache = {}           # dernier état lu, par groupe
        self.published = {}       # topic -> dernier payload publié
        self.commands = queue.Queue()
        self.refresh_due = {}     # groupe -> heure de relecture prévue
        self.running = True

        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                  client_id="sweetdawn_aml005")
        self.client.username_pw_set(config["user"], config["password"])
        self.client.will_set(BRIDGE_TOPIC, "offline", retain=True)
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message

    # --- Topics --------------------------------------------------------------

    @staticmethod
    def state_topic(entity, channel):
        return f"{BASE}/{entity.key}/{channel or 'state'}"

    @staticmethod
    def command_topic(entity, channel):
        return f"{BASE}/{entity.key}/{channel + '/' if channel else ''}set"

    def discovery(self, entity):
        availability = [{"topic": BRIDGE_TOPIC}]
        if entity.needs_aml:
            availability.append({"topic": AML_TOPIC})

        payload = {
            "name": entity.name,
            "unique_id": entity.unique_id or f"aml005_{entity.key}",
            "device": DEVICE,
            "availability": availability,
            "availability_mode": "all",
            **entity.config,
        }

        for name, channel in entity.channels.items():
            prefix = f"{name}_" if name else ""
            if channel.get is not None:
                payload[prefix + "state_topic"] = self.state_topic(entity, name)
            if channel.set is not None:
                payload[prefix + "command_topic"] = self.command_topic(entity, name)

        topic = (entity.discovery_topic or
                 f"{DISCOVERY_PREFIX}/{entity.component}/aml005_{entity.key}/config")
        return topic, payload

    # --- MQTT (fil de paho) --------------------------------------------------

    def announce(self):
        """Annonce le pont et toutes ses entités à Home Assistant."""
        self.client.publish(BRIDGE_TOPIC, "online", retain=True)

        for entity in self.entities:
            topic, payload = self.discovery(entity)
            self.client.publish(topic, json.dumps(payload), retain=True)

        # Le fil principal republiera tous les états.
        self.commands.put(("republish", None))

    def on_connect(self, client, userdata, flags, reason_code, properties=None):
        if reason_code.is_failure:
            log.error("Connexion MQTT refusée : %s", reason_code)
            return

        log.info("Connecté au broker MQTT")
        client.subscribe(f"{BASE}/+/set")
        client.subscribe(f"{BASE}/+/+/set")
        # Home Assistant redémarré : il faut lui réannoncer les entités.
        client.subscribe(f"{DISCOVERY_PREFIX}/status")
        self.announce()

    def on_message(self, client, userdata, message):
        payload = message.payload.decode(errors="replace").strip()

        if message.topic == f"{DISCOVERY_PREFIX}/status":
            if payload == "online":
                log.info("Home Assistant redémarré : nouvelle annonce")
                self.announce()
            return

        route = self.routes.get(message.topic)
        if route is not None:
            self.commands.put((route, payload))

    # --- Service (fil principal) ---------------------------------------------

    def already_on(self, entity):
        """
        Une lampe est-elle déjà allumée ? (relue à l'instant, pas en cache :
        elle a pu être éteinte au bouton depuis la dernière relecture.)
        """
        self.refresh(entity.group)
        return value_in(self.cache, entity.group, "enable") is True

    def execute(self, route, payload):
        entity, channel_name = route
        channel = entity.channels[channel_name]

        # Rallumer une lampe déjà allumée n'est pas sans effet : l'ambiance
        # repart sur la scène Rainbow, ce qui écraserait la scène, la
        # couleur ou la luminosité que Home Assistant envoie juste après
        # son « ON » (on_command_type: first).
        if (entity.component == "light" and channel_name == "" and
                payload == "ON" and self.already_on(entity)):
            return

        try:
            for command, value in channel.set(payload, self.cache):
                log.info("Commande %s %s", command, "" if value is None else value)
                self.service.request(command, value)
        except (ValueError, ServiceError) as error:
            log.warning("%s (%s) refusé : %s", entity.key, payload, error)
        except OSError as error:
            log.warning("Service injoignable : %s", error)

        if entity.group is not None:
            self.refresh_due[entity.group] = time.monotonic() + REFRESH_DELAY_S

    def refresh(self, group):
        """Relit un groupe sur le service, puis publie les états concernés."""
        try:
            self.cache[group] = self.service.request(group)
        except ServiceError as error:
            log.warning("Lecture %s impossible : %s", group, error)
            return
        except OSError as error:
            log.warning("Service injoignable : %s", error)
            self.cache.pop("status", None)
            self.publish(AML_TOPIC, "offline")
            return

        if group == "status":
            connected = self.cache["status"].get("connected", False)
            self.publish(AML_TOPIC, "online" if connected else "offline")

        for entity in self.entities:
            if entity.group != group:
                continue
            for name, channel in entity.channels.items():
                if channel.get is not None:
                    value = channel.get(self.cache)
                    if value is not None:
                        self.publish(self.state_topic(entity, name), value)

    def refresh_all(self):
        self.refresh("status")

        # Inutile d'interroger l'AML s'il n'est pas connecté.
        if value_in(self.cache, "status", "connected"):
            for group in self.GROUPS[1:]:
                self.refresh(group)

    def publish_live(self):
        """Republie les valeurs calculées localement (publish ignore l'inchangé)."""
        for entity in self.entities:
            if entity.live:
                for name, channel in entity.channels.items():
                    self.publish(self.state_topic(entity, name),
                                 channel.get(self.cache))

    def publish(self, topic, payload):
        """Publie seulement si la valeur a changé (états conservés : retain)."""
        if self.published.get(topic) != payload:
            self.client.publish(topic, payload, retain=True)
            self.published[topic] = payload

    # --- Boucle principale ---------------------------------------------------

    def start(self):
        # Les choix réels (scènes, sonneries...) viennent du service.
        try:
            apply_service_choices(self.service.request("commands"))
        except (OSError, ServiceError) as error:
            log.warning("Liste des commandes indisponible (%s) : "
                        "choix par défaut utilisés", error)

        self.entities = build_entities()
        for entity in self.entities:
            for name, channel in entity.channels.items():
                if channel.set is not None:
                    self.routes[self.command_topic(entity, name)] = (entity, name)

        self.client.connect_async(self.config["broker"], self.config["port"], 60)
        self.client.loop_start()

    def run(self):
        next_poll = 0.0

        while self.running:
            try:
                item, payload = self.commands.get(timeout=0.5)
                if item == "republish":
                    # Tout republier, même ce qui n'a pas changé.
                    self.published.clear()
                    next_poll = 0.0
                else:
                    self.execute(item, payload)
            except queue.Empty:
                pass

            now = time.monotonic()

            for group, due in list(self.refresh_due.items()):
                if now >= due:
                    del self.refresh_due[group]
                    self.refresh(group)

            if now >= next_poll:
                self.refresh_all()
                next_poll = now + self.config["poll_seconds"]

            self.publish_live()

    def stop(self, *_):
        self.running = False

    def close(self):
        try:
            self.client.publish(BRIDGE_TOPIC, "offline",
                                retain=True).wait_for_publish(2)
        except (RuntimeError, ValueError):
            pass  # pas connecté au broker : le testament s'en chargera
        self.client.loop_stop()
        self.client.disconnect()


def main():
    logging.basicConfig(level=logging.INFO, stream=sys.stdout,
                        format="%(levelname)s %(message)s")

    try:
        config = load_config()
    except (OSError, KeyError, ValueError) as error:
        log.error("Configuration illisible (%s) : %s", CONFIG_PATH, error)
        return 1

    bridge = Bridge(config)
    signal.signal(signal.SIGTERM, bridge.stop)
    signal.signal(signal.SIGINT, bridge.stop)

    bridge.start()
    log.info("Pont démarré (%d entités)", len(bridge.entities))

    try:
        bridge.run()
    finally:
        bridge.close()
        log.info("Pont arrêté")

    return 0


if __name__ == "__main__":
    sys.exit(main())
