#!/usr/bin/env python3
"""Writes rekordbox ANLZ .DAT files for the scenarios that need a rekordbox grid.

beatgrid_90.DAT is a PQTZ beat grid at 90 BPM over the first 30 s, from
0.5 s: far enough from whatever Bite DJ's analyzer finds in ../sine-30.wav
that the two grids' bars land in visibly different places.

beatgrid_90_bar3.DAT is the same grid, but rekordbox starts it on the third
beat of a bar, so its first bar line is the grid's third beat, not its first.

The layout follows lib/rekordbox-metadata/rekordbox_anlz.ksy (big endian): a
0x1c-byte PMAI header, then one section per tag. Run from this directory to
regenerate the files.
"""

import struct


def section(fourcc, body):
    return fourcc + struct.pack(">II", 12, 12 + len(body)) + body


def beat_grid(bpm, first_ms, length_ms, first_beat_number=1):
    beat_ms = 60000.0 / bpm
    times = []
    t = float(first_ms)
    while t < length_ms:
        times.append(round(t))
        t += beat_ms
    body = struct.pack(">III", 0, 0x80000, len(times))
    for i, time_ms in enumerate(times):
        body += struct.pack(">HHI", (i + first_beat_number - 1) % 4 + 1, round(bpm * 100), time_ms)
    return section(b"PQTZ", body)


def anlz(*sections):
    data = b"".join(sections)
    header = b"PMAI" + struct.pack(">II", 0x1C, 0x1C + len(data))
    return header.ljust(0x1C, b"\0") + data


with open("beatgrid_90.DAT", "wb") as f:
    f.write(anlz(beat_grid(90, 500, 30000)))

with open("beatgrid_90_bar3.DAT", "wb") as f:
    f.write(anlz(beat_grid(90, 500, 30000, first_beat_number=3)))
