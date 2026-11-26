#!/usr/bin/env python3
"""Entrées infrarouges de SweetDawn IR.

Chaque entrée lit un récepteur et produit des événements Event(codes, repeat) :
  codes  : codes candidats de la touche, au format du noyau Linux
           ("nec:0x14", "rc5:0x1e0c"...) ou "key:115" pour evdev ;
  repeat : True si la touche est maintenue (code de répétition).

Trois entrées, choisies par "input" dans le fichier de télécommande :

  {"type": "audio", "device": "plughw:CARD=Device,DEV=0"}
      Capteur IR démodulé branché sur une entrée micro (carte son USB...).
      Décodage fait ici : protocole NEC seulement. Option "debug_file" : chemin
      d'un fichier où noter chaque salve reçue (durées brutes et décodage).

  {"type": "lirc", "device": "/dev/lirc0"}
      Récepteur géré par le noyau : capteur sur GPIO (dtoverlay=gpio-ir),
      récepteur USB de type MCE... Le noyau décode NEC, RC5, RC6, Sony,
      JVC, Sanyo, Sharp... (activer les protocoles : ir-keytable -p all).

  {"type": "evdev", "device": "/dev/input/by-id/...-event-kbd"}
      Récepteur qui se présente comme un clavier (FLIRC, dongles USB...) :
      chaque touche arrive comme un code de touche Linux ("key:115").
"""
import collections
import fcntl
import json
import time
import os
import struct
import subprocess
import sys

try:
    import numpy as np   # entrée audio seulement
except ImportError:
    np = None

Event = collections.namedtuple("Event", "codes repeat")


def default_config():
    """Fichier de télécommande par défaut : config/ir.json du dépôt SweetDawn
    (code dans ir/), sinon sweetdawn_ir.json à côté du code."""
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [os.path.join(here, "..", "config", "ir.json"),
                  os.path.join(here, "sweetdawn_ir.json")]
    return os.path.normpath(next((c for c in candidates if os.path.exists(c)), candidates[0]))


def normalize(code):
    """Forme canonique d'un code : "NEC:0x0014" -> "nec:0x14", "key:115"."""
    proto, sep, value = str(code).strip().lower().partition(":")
    if not sep:
        raise ValueError(f"code sans protocole : {code!r} (ex. \"nec:0x14\")")
    if proto == "key":
        return f"key:{int(value, 0)}"
    return f"{proto}:0x{int(value, 16):x}"


# ============================================================================
# audio : capteur sur une entrée micro, décodage NEC
# ============================================================================
#
# Le capteur sort un signal démodulé (créneaux). Via l'entrée micro, il
# arrive filtré (couplage AC) et déformé : impulsion d'en-tête hachée,
# impulsions rallongées au détriment des silences, début de trame instable.
# On détecte donc les fronts (pics de la dérivée) et on ne se fie qu'à la
# durée totale de chaque bit (~1125 µs = 0, ~2250 µs = 1), lue en partant
# de la fin de trame, qui est propre.

RATE = 48000
CHUNK = 2048
SILENCE_MS = 60          # fin de salve après ce silence
MAX_BURST_S = 1.5        # au-delà : parasite continu (soleil...), pas un appui
FULL_SCALE = 65535.0     # écart max entre deux échantillons 16 bits


def _us(n_samples):
    return n_samples * 1e6 / RATE


def bursts(device, sensitivity=6.0):
    """Générateur de salves (tableaux numpy) détectées dans le flux audio."""
    proc = subprocess.Popen(["arecord", "-q", "-D", device, "-f", "S16_LE", "-c", "1",
                             "-r", str(RATE), "-t", "raw"], stdout=subprocess.PIPE)
    noise = None
    buf, levels = [], []
    quiet = 0
    silence_chunks = max(1, int(SILENCE_MS * RATE / 1000 / CHUNK))
    max_chunks = int(MAX_BURST_S * RATE / CHUNK)
    try:
        while True:
            raw = proc.stdout.read(CHUNK * 2)
            if not raw:
                sys.exit(f"arecord s'est arrêté (code {proc.wait()}) : carte son absente ?")
            x = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
            d = np.abs(np.diff(x))
            level = d.max() if len(d) else 0
            if noise is None:
                noise = max(level, 50.0)
            # plafond : les vrais signaux IR sautent de ~65000 (pleine échelle)
            active = level > min(noise * sensitivity, FULL_SCALE * 0.5)
            if active:
                buf.append(x)
                levels.append(level)
                quiet = 0
                if len(buf) > max_chunks:
                    # Parasite continu : abandon, seuil placé juste au-dessus.
                    # Le suivi du bruit le fera redescendre ensuite.
                    parasite = float(np.median(levels))
                    noise = min(parasite * 1.3, FULL_SCALE * 0.5) / sensitivity
                    print(f"parasite continu (niveau {parasite:.0f}) : seuil relevé à "
                          f"{noise * sensitivity:.0f}", file=sys.stderr, flush=True)
                    buf, levels, quiet = [], [], 0
            elif buf:
                buf.append(x)
                quiet += 1
                if quiet >= silence_chunks:
                    yield np.concatenate(buf)
                    buf, levels, quiet = [], [], 0
            else:
                noise = 0.95 * noise + 0.05 * max(level, 50.0)   # suivi lent du bruit
    finally:
        proc.kill()


def edges(sig):
    """Positions (en échantillons) des fronts d'une salve."""
    d = np.diff(sig)
    thr = 0.35 * np.abs(d).max()
    out = []  # (position, signe)
    for i in np.flatnonzero(np.abs(d) > thr):
        s = 1 if d[i] > 0 else -1
        if out and (i - out[-1][0] < 6 or out[-1][1] == s):   # même front ou bruit
            continue
        out.append((i, s))
    return [p for p, _ in out]


def durations(sig):
    e = edges(sig)
    return [_us(b - a) for a, b in zip(e, e[1:])]


FRAME_SPLIT_US = 6000    # aucune durée d'une trame NEC ne dépasse ~5 ms


def _read_bits(seg):
    """32 bits lus depuis la fin (bit de stop = dernier élément) -> 4 octets."""
    val = 0
    for m in range(32):
        if seg[-3 - 2 * m] + seg[-2 - 2 * m] > 1700:
            val |= 1 << (31 - m)
    return tuple((val >> (8 * n)) & 0xFF for n in range(4))


MARK_US = (350, 1350)            # impulsion : ~560 µs, jusqu'à ~1100 si rallongée
BIT_US = (800, 2800)             # impulsion + silence : ~1125 (0) ou ~2250 (1)


def _well_formed(seg):
    """Fin de trame bien formée : bit de stop + 16 bits de commande.

    Le bit de stop est une impulsion, chaque bit a une durée totale valide et
    au moins 14 des 16 « impulsions » en sont (un parasite peut déplacer un
    front au milieu d'un bit). Un alignement décalé d'une durée met des
    silences à la place des impulsions et échoue.
    """
    if not MARK_US[0] <= seg[-1] <= MARK_US[1]:
        return False
    marks = 0
    for m in range(16):
        mark, space = seg[-3 - 2 * m], seg[-2 - 2 * m]
        if not BIT_US[0] <= mark + space <= BIT_US[1]:
            return False
        marks += MARK_US[0] <= mark <= MARK_US[1]
    return marks >= 14


def _frame(seg, max_drop=12):
    """Trame NEC d'un segment, ou None.

    1. Calée sur ses deux bouts : silence d'en-tête (~4,5 ms) puis 32 bits
       et le bit de stop, soit 66 durées ; ce qui suit est du parasite.
    2. Sinon, on retire 0 à max_drop durées parasites en fin de trame et on
       garde le premier alignement dont la fin est bien formée.
    Dans les deux cas la commande doit se vérifier ; l'adresse peut être
    faussée (début de trame instable), voir nec_codes.
    """
    for i, v in enumerate(seg):
        if 3200 < v < 6000 and len(seg) >= i + 66:
            framed = seg[:i + 66]
            b = _read_bits(framed)
            if b[2] ^ b[3] == 0xFF and _well_formed(framed):
                return b
            break
    for drop in range(0, min(max_drop, len(seg) - 65) + 1):
        framed = seg[:len(seg) - drop]
        if _well_formed(framed):
            b = _read_bits(framed)
            return b if b[2] ^ b[3] == 0xFF else None
    return None


def decode_nec(durs):
    """Trames NEC d'une salve : liste de (b0, b1, b2, b3) ou "REPEAT".

    Les trames sont séparées par plus de 6 ms de silence ; des parasites
    (éclairage LED, soleil) dans ce silence forment de petits segments ignorés.
    """
    segments, cur, before = [], [], None
    for v in durs:
        if v > FRAME_SPLIT_US:
            segments.append((before, cur))
            cur, before = [], v
        else:
            cur.append(v)
    segments.append((before, cur))

    frames = []
    for before, seg in segments:
        if len(seg) >= 65:
            b = _frame(seg)
            if b:
                frames.append(b)
        elif len(seg) >= 2 and 1700 < seg[-2] < 2900 and (
                6000 < sum(seg[:-2]) < 11500                    # en-tête haché (~9 ms)
                or (len(seg) == 2 and before and 7000 < before < 11500)):  # en-tête d'un bloc
            frames.append("REPEAT")
    return frames


def nec_codes(b):
    """Codes candidats (format noyau) d'une trame NEC dont la commande est valide.

    Le premier octet d'adresse est parfois faussé (début de trame instable) :
    si l'adresse ne se vérifie pas, on propose aussi l'adresse déduite de son
    inverse (2e octet, intact), en plus de la lecture NEC étendue (16 bits).
    """
    b0, b1, cmd, _ = b
    if b0 ^ b1 == 0xFF:
        return [f"nec:0x{(b0 << 8) | cmd:x}"]
    return [f"necx:0x{(b0 << 16) | (b1 << 8) | cmd:x}",
            f"nec:0x{((b1 ^ 0xFF) << 8) | cmd:x}"]


class AudioInput:
    def __init__(self, device, sensitivity=6.0, debug_file=None):
        if np is None:
            sys.exit("L'entrée audio demande numpy (Debian : apt install python3-numpy)")
        self.device, self.sensitivity, self.debug_file = device, sensitivity, debug_file

    def describe(self):
        return f"audio {self.device} (NEC)"

    def events(self):
        for sig in bursts(self.device, self.sensitivity):
            durs = durations(sig)
            frames = decode_nec(durs)
            codes = [nec_codes(f) for f in frames if f != "REPEAT" and f[2] ^ f[3] == 0xFF]
            if self.debug_file:   # diagnostic : chaque salve, brute et décodée
                with open(self.debug_file, "a") as f:
                    f.write(json.dumps({"t": time.strftime("%H:%M:%S"), "codes": codes,
                                        "frames": [x if x == "REPEAT" else list(x) for x in frames],
                                        "durations": [round(v) for v in durs]}) + "\n")
            if codes:
                yield Event(codes[0], False)     # les répétitions de la même salve suivent l'appui
            elif "REPEAT" in frames:
                yield Event([], True)


# ============================================================================
# lirc : décodage par le noyau (/dev/lircN en mode scancode)
# ============================================================================

LIRC_SET_REC_MODE = 0x40046912          # _IOW('i', 0x12, __u32)
LIRC_MODE_SCANCODE = 0x00000008
LIRC_SCANCODE_FLAG_REPEAT = 2
LIRC_SCANCODE = struct.Struct("=QHHIQ")  # timestamp, flags, rc_proto, keycode, scancode

# enum rc_proto (include/uapi/linux/lirc.h)
RC_PROTO = ["unknown", "other", "rc5", "rc5x_20", "rc5_sz", "jvc", "sony12", "sony15",
            "sony20", "nec", "necx", "nec32", "sanyo", "mcir2_kbd", "mcir2_mse", "rc6_0",
            "rc6_6a_20", "rc6_6a_24", "rc6_6a_32", "rc6_mce", "sharp", "xmp", "cec", "imon",
            "rc_mm_12", "rc_mm_24", "rc_mm_32", "xbox_dvd"]


def parse_lirc(record):
    _, flags, proto, _, scancode = LIRC_SCANCODE.unpack(record)
    name = RC_PROTO[proto] if proto < len(RC_PROTO) else f"proto{proto}"
    return Event([f"{name}:0x{scancode:x}"], bool(flags & LIRC_SCANCODE_FLAG_REPEAT))


class LircInput:
    def __init__(self, device="/dev/lirc0"):
        self.device = device

    def describe(self):
        return f"lirc {self.device} (décodage noyau)"

    def events(self):
        fd = os.open(self.device, os.O_RDONLY)
        try:
            fcntl.ioctl(fd, LIRC_SET_REC_MODE, struct.pack("I", LIRC_MODE_SCANCODE))
            size = LIRC_SCANCODE.size
            while True:
                data = os.read(fd, size * 16)
                for i in range(0, len(data) - size + 1, size):
                    yield parse_lirc(data[i:i + size])
        finally:
            os.close(fd)


# ============================================================================
# evdev : récepteur vu comme un clavier (/dev/input/eventN)
# ============================================================================

INPUT_EVENT = struct.Struct("@llHHi")    # timeval (2 × long), type, code, value
EV_KEY = 0x01
EVIOCGRAB = 0x40044590                   # _IOW('E', 0x90, int)


def parse_evdev(record):
    """Event pour un appui (1) ou une répétition (2) ; None sinon."""
    _, _, etype, code, value = INPUT_EVENT.unpack(record)
    if etype == EV_KEY and value in (1, 2):
        return Event([f"key:{code}"], value == 2)
    return None


class EvdevInput:
    def __init__(self, device, grab=True):
        self.device, self.grab = device, grab

    def describe(self):
        return f"evdev {self.device}"

    def events(self):
        fd = os.open(self.device, os.O_RDONLY)
        try:
            if self.grab:   # les touches ne partent plus vers la console
                fcntl.ioctl(fd, EVIOCGRAB, 1)
            size = INPUT_EVENT.size
            while True:
                data = os.read(fd, size * 16)
                for i in range(0, len(data) - size + 1, size):
                    ev = parse_evdev(data[i:i + size])
                    if ev:
                        yield ev
        finally:
            os.close(fd)


# ============================================================================

def open_input(cfg):
    """Entrée décrite par le bloc "input" du fichier de télécommande."""
    kind = cfg.get("type")
    if kind == "audio":
        return AudioInput(cfg["device"], cfg.get("sensitivity", 6.0), cfg.get("debug_file"))
    if kind == "lirc":
        return LircInput(cfg.get("device", "/dev/lirc0"))
    if kind == "evdev":
        return EvdevInput(cfg["device"], cfg.get("grab", True))
    raise ValueError(f"type d'entrée inconnu : {kind!r} (audio, lirc ou evdev)")
