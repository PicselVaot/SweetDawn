#!/usr/bin/env python3
"""Relever les codes d'une télécommande, quelle que soit l'entrée.

L'entrée lue est le bloc "input" du fichier de télécommande (audio, lirc ou
evdev, voir ir_input.py).

  ir_learn.py listen          affiche le code de chaque touche appuyée
  ir_learn.py learn           pour chaque touche : taper son nom, appuyer
                              quelques fois ; le code est écrit dans le
                              fichier (action conservée, "none" si nouvelle)

Options : --config FICHIER (défaut : config/ir.json), --presses N
(appuis par touche en mode learn, défaut 3).
"""
import argparse
import json
import queue
import re
import threading
import time
from collections import Counter
from pathlib import Path

import ir_input

HERE = Path(__file__).resolve().parent


def reader(source, q):
    for ev in source.events():
        q.put((time.monotonic(), ev))


def save(path, cfg):
    """JSON lisible : une touche par ligne."""
    keys = cfg.pop("keys")
    head = json.dumps(cfg, ensure_ascii=False, indent=2)[:-2]
    width = max((len(json.dumps(k, ensure_ascii=False)) for k in keys), default=0) + 1
    lines = [f"    {json.dumps(k, ensure_ascii=False) + ':':<{width}} "
             f"{json.dumps(v, ensure_ascii=False)}" for k, v in keys.items()]
    head = re.sub(r'\{\n\s+("type"[^}]*)\n\s+\}',
                  lambda m: "{" + re.sub(r",\n\s+", ", ", m.group(1)) + "}", head)
    path.write_text(head + ',\n\n  "keys": {\n' + ",\n".join(lines) + "\n  }\n}\n")
    cfg["keys"] = keys


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=["listen", "learn"])
    p.add_argument("--config", default=ir_input.default_config())
    p.add_argument("--presses", type=int, default=3)
    args = p.parse_args()

    path = Path(args.config)
    cfg = json.loads(path.read_text())
    source = ir_input.open_input(cfg["input"])
    q = queue.Queue()
    threading.Thread(target=reader, args=(source, q), daemon=True).start()
    known = {}
    for name, key in cfg.get("keys", {}).items():
        codes = key.get("code") or []
        for c in codes if isinstance(codes, list) else [codes]:
            known[ir_input.normalize(c)] = name

    print(f"Entrée : {source.describe()}")
    if args.mode == "listen":
        print("Appuyez sur des touches (Ctrl+C pour quitter).")
        while True:
            _, ev = q.get()
            name = next((known[c] for c in ev.codes if c in known), None)
            print(("  (maintenue) " if ev.repeat else "  ") + " / ".join(ev.codes or ["répétition"])
                  + (f"   → {name}" if name else ""))

    cfg.setdefault("keys", {})
    print(f"Relevé dans {path.name}. Nom vide pour terminer.")
    while True:
        name = input("\nNom de la touche : ").strip()
        if not name:
            break
        asked = time.monotonic()
        print(f"  Appuyez {args.presses} fois, brièvement…")
        votes, got = Counter(), 0
        while got < args.presses:
            t, ev = q.get()
            if t < asked or ev.repeat or not ev.codes:
                continue          # appui ancien, ou touche maintenue
            got += 1
            votes.update(ev.codes)
            print(f"  {got}/{args.presses}  {' / '.join(ev.codes)}")
        code, n = votes.most_common(1)[0]
        other = known.get(code)
        if other and other != name:
            print(f"  ⚠ {code} est déjà la touche « {other} »")
        key = cfg["keys"].get(name, {"action": "none"})
        cfg["keys"][name] = {"code": code, **{k: v for k, v in key.items() if k != "code"}}
        known[code] = name
        save(path, cfg)
        print(f"  ✓ {name} = {code} ({n}/{args.presses} appuis concordants)")
    print(f"\n{len(cfg['keys'])} touches dans {path}")


if __name__ == "__main__":
    try:
        main()
    except (KeyboardInterrupt, EOFError):
        print()
