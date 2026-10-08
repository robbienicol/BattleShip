#!/usr/bin/env python3
"""Writes a synthetic VS replay (.ssb64r) with scripted, deterministic inputs
so rollback/sync tests can run without anyone playing through the menus.

Usage: make_replay.py OUT [--frames N] [--seed S] [--stage K] [--fighters A,B]
"""
import argparse
import random
import struct

MAGIC = 0x53534E52  # SSNR
VERSION = 1
PLAYERS = 4  # MAXCONTROLLERS

A, B, Z = 0x8000, 0x4000, 0x2000
R_TRIG = 0x0010
C_UP = 0x0008

PKIND_MAN, PKIND_NOT = 0, 2
RULE_STOCK = 0x2
TIME_INFINITE = 100


def fnv(checksum, value):
    checksum ^= value & 0xFFFFFFFF
    return (checksum * 16777619) & 0xFFFFFFFF


def script(rng, frames, toward):
    """One player's (buttons, stick_x, stick_y) per tick: walk, dash, jump, attack, shield."""
    out = []
    while len(out) < frames:
        action = rng.choice(["walk", "walk", "dash", "jump", "jab", "special", "smash", "air", "shield", "idle"])
        length = rng.randint(6, 30)
        x = toward * rng.choice([40, 70, 80])
        for i in range(length):
            buttons, sx, sy = 0, 0, 0
            if action == "walk":
                sx = toward * 40
            elif action == "dash":
                sx = x
            elif action == "jump":
                buttons = C_UP if i < 3 else 0
                sx = x // 2
            elif action == "jab":
                buttons = A if (i % 8) < 3 else 0
            elif action == "special":
                buttons = B if i < 3 else 0
                sy = rng.choice([0, 0, 70, -70]) if i == 0 else 0
            elif action == "smash":
                sx = x if i < 3 else 0
                buttons = A if i < 3 else 0
            elif action == "air":
                buttons = (C_UP if i < 3 else 0) | (A if 10 <= i < 13 else 0)
                sx = x // 2
            elif action == "shield":
                buttons = Z if i < 20 else 0
            out.append((buttons, sx, sy))
    return out[:frames]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--frames", type=int, default=3600)
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--stage", type=int, default=6)  # Dream Land
    ap.add_argument("--fighters", default="0,1")  # Mario, Fox
    args = ap.parse_args()

    rng = random.Random(args.seed)
    fighters = [int(f) for f in args.fighters.split(",")]
    count = len(fighters)

    inputs = [script(rng, args.frames, 1 if p == 0 else -1) for p in range(count)]

    metadata = struct.pack(
        "<10I9B" + "4B" * 7,
        MAGIC, VERSION, 22, count, args.stage, 3, TIME_INFINITE, 0, 0, args.seed,
        1, RULE_STOCK, 0, 0, 0, 0, 100, 0, 0,  # game_type .. is_not_teamshadows
        *[PKIND_MAN if p < count else PKIND_NOT for p in range(PLAYERS)],
        *[fighters[p] if p < count else 0 for p in range(PLAYERS)],
        *[0] * 4,                      # costumes
        *list(range(PLAYERS)),         # teams
        *[9] * 4,                      # handicaps (neutral)
        *[1] * 4,                      # levels
        *[0] * 4,                      # shades
    )
    metadata += b"\0" * ((-len(metadata)) % 4)

    frames = bytearray()
    checksum = 2166136261
    for tick in range(args.frames):
        for p in range(PLAYERS):
            buttons, sx, sy = inputs[p][tick] if p < count else (0, 0, 0)
            frames += struct.pack("<IHbbBBBx", tick, buttons, sx, sy, 0, 0, 1)
            for v in (p, tick, buttons, sx & 0xFF, sy & 0xFF):
                checksum = fnv(checksum, v)

    header = struct.pack("<7I", MAGIC, VERSION, len(metadata), 12, args.frames, PLAYERS, checksum)
    with open(args.out, "wb") as f:
        f.write(header + metadata + frames)
    print(f"wrote {args.out}: {args.frames} frames, fighters={fighters}, stage={args.stage}, checksum=0x{checksum:08X}")


if __name__ == "__main__":
    main()
