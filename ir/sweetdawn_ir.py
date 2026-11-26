#!/usr/bin/env python3
"""SweetDawn IR — télécommande infrarouge pour l'AML005.

    télécommande ─IR─► récepteur ─► ir_input.py ─► ce service
                     ─socket JSON─► sweetdawn-service ─Bluetooth─► AML005

Client du service SweetDawn, comme sweetdawn-ctl et le pont MQTT : il ne
touche jamais au Bluetooth. Tout ce qui dépend de la télécommande est dans
un fichier JSON (config/ir.json, voir config/ir.example.json) : l'entrée à lire ("input" : audio, lirc ou evdev, voir
ir_input.py) et, pour chaque touche, son code et son action. Les codes se
relèvent avec ir_learn.py.

Le service retient :
  - la lumière active  (<LightType>) : "Light" (lampe) ou "Mood" (ambiance) ;
  - la musique active  (<MusicType>) : "Radio" ou "Media Player" (haut-parleur BT).
Elles sont sauvegardées dans ~/.local/state/sweetdawn-ir.json.

Usage :
  sweetdawn_ir.py                     écoute la télécommande
  sweetdawn_ir.py --dry-run           affiche les commandes au lieu de les envoyer
  sweetdawn_ir.py --press "☼+"        exécute une touche puis quitte (test sans télécommande)
"""
import argparse
import json
import logging
import os
import queue
import socket
import threading
import time
from pathlib import Path

import ir_input

HERE = Path(__file__).resolve().parent
STATE_FILE = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "sweetdawn-ir.json"

SCENES = ["Off", "Random", "Rainbow", "Rainbow Slow", "Fusion", "Pulse",
          "Wave", "Chill", "Action", "Forest", "Summer"]
# Durée maximale des minuteurs de l'AML, en minutes
TIMER_MAX = {"Light": 60, "Mood": 120, "Radio": 60}
LONG_TIMER_FINAL = 60      # au-delà du max : le Pi attend, puis lance 60 min sur l'AML
CACHE_S = 3.0              # durée de validité d'un état relu (appuis répétés)

log = logging.getLogger("sweetdawn-ir")


# ============================================================================
# Client du service SweetDawn
# ============================================================================

class ServiceError(Exception):
    pass


class Service:
    """Une connexion par commande, comme le pont MQTT."""

    def __init__(self, path):
        self.path = path
        self.next_id = 0

    def request(self, command, value=None, timeout=20):
        self.next_id += 1
        msg = {"type": "request", "id": self.next_id, "command": command,
               "args": [] if value is None else [value]}
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(timeout)
            sock.connect(self.path)
            sock.sendall((json.dumps(msg) + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                chunk = sock.recv(65536)
                if not chunk:
                    raise ConnectionError("le service a fermé la connexion")
                data += chunk
        resp = json.loads(data)
        if not resp.get("ok"):
            err = resp.get("error", {})
            raise ServiceError(f"{err.get('code', '?')} : {err.get('message', '')}")
        return resp.get("result", {})


class FakeService:
    """--dry-run : affiche les commandes, simule un état plausible."""

    def __init__(self):
        self.state = {
            "Light": {"enable": False, "brightness": 50, "sunset": False, "sunset_duration": 10},
            "Mood": {"enable": False, "brightness": 100, "timer": False, "timer_duration": 120,
                     "color": "0,131,255", "scene": "Off"},
            "Radio": {"enable": False, "channel": 2, "frequency": 89.2, "volume": 5,
                      "sleep_timer": False, "timer_duration": 10},
        }

    def request(self, command, value=None, timeout=20):
        if command in self.state:
            return dict(self.state[command])
        print(f"  → {command}" + ("" if value is None else f" {value}"))
        return {}


# ============================================================================
# Actions
# ============================================================================

FIELDS = {  # commande de réglage -> champ de l'état relu
    "Enable": "enable", "Brightness": "brightness", "Volume": "volume",
    "Channel": "channel", "Scene": "scene", "Sunset": "sunset",
    "Sunset duration": "sunset_duration", "Timer": "timer",
    "Timer duration": "timer_duration", "Sleep timer": "sleep_timer",
}


class Remote:
    def __init__(self, service, dry_run=False):
        self.svc = service
        self.dry_run = dry_run
        self.cache = {}           # groupe -> (instant, état)
        self.timers = []          # minuteurs longs gérés par le Pi
        self.lock = threading.RLock()
        self.state = {"light_type": "Light", "music_type": "Radio", "bt_on": False}
        if STATE_FILE.exists():
            try:
                self.state.update(json.loads(STATE_FILE.read_text()))
            except (OSError, ValueError):
                log.warning("état illisible, valeurs par défaut")

    # -- état -----------------------------------------------------------------

    def save(self):
        if self.dry_run:
            return
        STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
        STATE_FILE.write_text(json.dumps(self.state))

    def read(self, group):
        t, data = self.cache.get(group, (0, None))
        if data is None or time.monotonic() - t > CACHE_S:
            data = self.svc.request(group)
            self.cache[group] = (time.monotonic(), data)
        return data

    def send(self, command, value=None):
        log.info("%s %s", command, "" if value is None else value)
        result = self.svc.request(command, value)
        group, _, name = command.partition("/")
        if group in self.cache and name in FIELDS and value is not None:
            self.cache[group][1][FIELDS[name]] = value
        if group in ("Radio",) and isinstance(result, dict):
            # >, <, >> renvoient la nouvelle fréquence
            for k, v in result.items():
                if group in self.cache:
                    self.cache[group][1][k] = v
        return result

    def enable(self, group, on=True):
        """Allume/éteint seulement si nécessaire (allumer l'ambiance relance Rainbow)."""
        if group == "Media Player":
            self.send("Media Player/Enable", on)
            self.state["bt_on"] = on
            return
        if self.read(group)["enable"] != on:
            self.send(f"{group}/Enable", on)

    @property
    def light(self):
        return self.state["light_type"]

    @property
    def music(self):
        return self.state["music_type"]

    # -- actions des touches ---------------------------------------------------

    def act_none(self, k):
        pass

    def act_command(self, k):
        if k["command"].startswith("Radio/"):  # recherche, réglage fin : allume la radio
            self.switch("music", "Radio", force=True)
        self.send(k["command"])

    def act_volume(self, k):
        # Volume commun à la radio et au haut-parleur Bluetooth sur l'AML :
        # agit sur la source active, sans en allumer aucune.
        v = self.read("Radio")["volume"]
        new = max(1, min(10, v + k["step"]))
        if new != v:
            self.send("Radio/Volume", new)

    # Changer de lumière ou de musique active : on éteint toujours celle d'où
    # l'on vient si elle était allumée, et la nouvelle prend alors le relais.
    # force=True : la touche demande en plus que la nouvelle soit allumée.

    def is_on(self, target):
        if target == "Media Player":
            return self.state["bt_on"]
        return self.read(target)["enable"]

    def switch(self, kind, target, force=False):
        key = kind + "_type"
        old = self.state[key]
        was_on = False
        if old != target:
            was_on = self.is_on(old)
            if was_on:
                self.enable(old, False)
            self.state[key] = target
            log.info("%s active : %s", "lumière" if kind == "light" else "musique", target)
        if force or was_on:
            self.enable(target, True)

    def act_music_switch(self, k):
        self.switch("music", "Media Player" if self.music == "Radio" else "Radio")

    def act_music_power(self, k):
        self.enable(self.music, not self.is_on(self.music))

    def act_light_preset(self, k):
        self.switch("light", "Light", force=True)
        self.send("Light/Brightness", k["percent"])

    def act_light_mode(self, k):
        self.switch("light", "Light", force=True)

    def act_mood_mode(self, k):
        self.switch("light", "Mood", force=True)

    def act_light_on(self, k):
        self.enable(self.light, True)

    def act_light_off(self, k):
        self.enable(self.light, False)

    def act_light_step(self, k):
        b = self.read(self.light)["brightness"]
        new = max(0, min(100, b + k["step"]))
        if new != b:
            self.send(f"{self.light}/Brightness", new)

    def act_scene(self, k, scene=None):
        self.switch("light", "Mood", force=True)
        self.send("Mood/Scene", scene or k["scene"])

    def act_scene_step(self, k):
        cur = self.read("Mood").get("scene", "Off")
        i = SCENES.index(cur) if cur in SCENES else 0
        self.act_scene(k, SCENES[(i + k["step"]) % len(SCENES)])

    def act_radio_preset(self, k):
        self.switch("music", "Radio", force=True)
        self.send("Radio/Channel", k["channel"])

    def act_radio_preset_step(self, k):
        self.switch("music", "Radio", force=True)
        c = self.read("Radio")["channel"]
        self.send("Radio/Channel", (c - 1 + k["step"]) % 10 + 1)

    # -- minuteurs -------------------------------------------------------------

    def act_timer(self, k):
        minutes = k["minutes"]
        for t in self.timers:
            t.cancel()
        self.timers = []
        for target in (self.light, self.music):
            if target == "Media Player":
                # pas de minuteur sur l'AML pour le haut-parleur : le Pi éteint lui-même
                if self.state["bt_on"]:
                    self.later(minutes * 60, lambda: self.enable("Media Player", False),
                               f"arrêt haut-parleur dans {minutes} min")
            elif self.read(target)["enable"]:
                if minutes <= TIMER_MAX[target]:
                    self.aml_timer(target, minutes)
                else:
                    wait = minutes - LONG_TIMER_FINAL
                    self.later(wait * 60, lambda t=target: self.aml_timer(t, LONG_TIMER_FINAL, check=True),
                               f"{target} : minuteur {LONG_TIMER_FINAL} min lancé dans {wait} min")

    def aml_timer(self, target, minutes, check=False):
        if check:  # minuteur différé : seulement si c'est encore allumé
            self.cache.pop(target, None)
            if not self.read(target)["enable"]:
                log.info("%s éteint entre-temps : minuteur annulé", target)
                return
        if target == "Light":
            self.send("Light/Sunset duration", minutes)
            self.send("Light/Sunset", True)
        elif target == "Mood":
            self.send("Mood/Timer duration", minutes)
            self.send("Mood/Timer", True)
        elif target == "Radio":
            self.send("Radio/Timer duration", minutes)
            self.send("Radio/Sleep timer", True)

    def later(self, seconds, fn, label):
        log.info("minuteur Pi : %s", label)

        def run():
            with self.lock:
                try:
                    fn()
                    self.save()
                except Exception as e:  # noqa: BLE001 - un minuteur ne doit pas tuer le service
                    log.error("minuteur : %s", e)

        t = threading.Timer(seconds, run)
        t.daemon = True
        t.start()
        self.timers.append(t)

    # -- point d'entrée --------------------------------------------------------

    def press(self, name, key):
        with self.lock:
            log.info("touche %s → %s", name, key["action"])
            try:
                getattr(self, "act_" + key["action"])(key)
            except (ServiceError, OSError, KeyError, ValueError) as e:
                log.error("%s : %s", name, e)
                self.cache.clear()
            self.save()


# ============================================================================
# Réception IR
# ============================================================================

def key_table(cfg):
    """Code normalisé -> (nom, touche). Un code peut aussi être une liste."""
    table = {}
    for name, key in cfg["keys"].items():
        codes = key.get("code") or []
        for code in codes if isinstance(codes, list) else [codes]:
            table[ir_input.normalize(code)] = (name, key)
    return table


def listen(cfg, remote, source):
    table = key_table(cfg)
    repeat_s = cfg.get("repeat_interval_ms", 250) / 1000
    jobs = queue.Queue()

    def worker():
        while True:
            name, key = jobs.get()
            remote.press(name, key)

    threading.Thread(target=worker, daemon=True).start()

    last = None            # (nom, touche) du dernier appui
    last_seen = 0.0        # dernier signe de vie de cette touche
    last_sent = 0.0
    log.info("écoute : %s, %d touches", source.describe(), len(cfg["keys"]))
    for ev in source.events():
        now = time.monotonic()
        hit = next((table[c] for c in ev.codes if c in table), None)
        if not ev.repeat:
            if hit is None:
                log.info("code inconnu %s", " / ".join(ev.codes))
                last = None
                continue
            last, last_seen, last_sent = hit, now, now
            jobs.put(hit)
        elif last and now - last_seen < 0.4 and (hit is None or hit is last):
            # touche maintenue : seulement pour les touches répétables, à débit limité
            last_seen = now
            if last[1].get("repeat") and now - last_sent >= repeat_s and jobs.empty():
                last_sent = now
                jobs.put(last)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--config", default=ir_input.default_config())
    p.add_argument("--dry-run", action="store_true", help="affiche les commandes sans les envoyer")
    p.add_argument("--press", metavar="TOUCHE", help="exécute une touche puis quitte")
    args = p.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    cfg = json.loads(Path(args.config).read_text())
    sock = os.environ.get("SWEETDAWN_SOCKET") or cfg["socket"]
    remote = Remote(FakeService() if args.dry_run else Service(sock), args.dry_run)

    if args.press:
        if args.press not in cfg["keys"]:
            p.error(f"touche inconnue. Touches : {' '.join(cfg['keys'])}")
        remote.press(args.press, cfg["keys"][args.press])
        return
    listen(cfg, remote, ir_input.open_input(cfg["input"]))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
