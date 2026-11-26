#!/usr/bin/env python3
"""Tests du décodage IR : trames réelles enregistrées et formats lirc / evdev.

    python3 tests/test_ir_input.py
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import ir_input as ii  # noqa: E402

failures = 0


def check(cond, msg):
    global failures
    failures += not cond
    print(("  OK " if cond else "  ÉCHEC ") + msg)


print("[audio : salves réelles]")
cases = json.loads((HERE / "audio_bursts.json").read_text())["cases"]
for i, case in enumerate(cases):
    codes = [ii.nec_codes(f) for f in ii.decode_nec(case["durations"])
             if f != "REPEAT" and f[2] ^ f[3] == 0xFF]
    if case["code"] is None:
        check(not codes, f"salve {i:2d} : rien attendu, reçu {codes or 'rien'}")
    else:
        check(len(codes) == 1 and case["code"] in codes[0], f"salve {i:2d} : {case['code']} → {codes}")

print("[lirc]")
rec = ii.LIRC_SCANCODE.pack(1, 0, 9, 0, 0x14)
check(ii.parse_lirc(rec) == ii.Event(["nec:0x14"], False), "NEC 0x14")
rec = ii.LIRC_SCANCODE.pack(1, 2, 2, 0, 0x1e0c)
check(ii.parse_lirc(rec) == ii.Event(["rc5:0x1e0c"], True), "RC5 maintenue")

print("[evdev]")
check(ii.parse_evdev(ii.INPUT_EVENT.pack(1, 2, 1, 115, 1)) == ii.Event(["key:115"], False), "appui")
check(ii.parse_evdev(ii.INPUT_EVENT.pack(1, 2, 1, 115, 2)) == ii.Event(["key:115"], True), "maintien")
check(ii.parse_evdev(ii.INPUT_EVENT.pack(1, 2, 1, 115, 0)) is None, "relâchement ignoré")

print("[codes]")
check(ii.normalize("NEC:0x0014") == "nec:0x14" and ii.normalize("key:0x73") == "key:115", "normalisation")

print("\n" + ("Tous les tests sont passés." if not failures else f"{failures} ÉCHEC(S)"))
sys.exit(1 if failures else 0)
