#!/usr/bin/env python3
"""Writes the Serato library the related_search scenario mounts as a fake stick.

Six tracks on the stick, two of them in the "Warmup" crate, so the drive's All
Tracks shows the four that are in no crate. Keys, BPMs, artists and genres are
chosen so each related search picks out a different set:

  KEY 8A    -> 8A, 9A, 7A and the relative 8B; not 2A
  ARTIST    -> the three Kalo tracks
  GENRE     -> the three House tracks

The audio is the key-tagged fixtures in ../../media, copied onto the stick by
the scenario's "drives" entry. Run from this directory to regenerate
"database V2" and "Warmup.crate".
"""

import struct

TRACKS = [
    # path on the stick, title, artist, album, genre, key, bpm
    ("Music/reference.flac", "Reference", "Kalo", "Night Bus", "House", "8A", "124.00"),
    ("Music/same.flac", "Same Key", "Kalo", "Night Bus", "Techno", "8A", "128.00"),
    ("Music/up1.flac", "Up One", "Mira", "Lanterns", "House", "9A", "125.00"),
    ("Music/relative.flac", "Relative", "Mira", "Lanterns", "House", "8B", "122.00"),
    ("Music/far.flac", "Far Away", "Kalo", "Coastline", "Techno", "2A", "140.00"),
    ("Music/down1.flac", "Down One", "Odd Lot", "Coastline", "Disco", "7A", "118.00"),
]
CRATE = ["Music/same.flac", "Music/far.flac"]


def text(value):
    return value.encode("utf-16-be")


def field(tag, data):
    return tag.encode("ascii") + struct.pack(">I", len(data)) + data


def track(path, title, artist, album, genre, key, bpm):
    return field(
        "otrk",
        field("ttyp", text("flac"))
        + field("pfil", text(path))
        + field("tsng", text(title))
        + field("tart", text(artist))
        + field("talb", text(album))
        + field("tgen", text(genre))
        + field("tkey", text(key))
        + field("tbpm", text(bpm))
        + field("tlen", text("00:08")),
    )


def main():
    version = field("vrsn", text("2.0/Serato Scratch LIVE Database"))
    with open("database V2", "wb") as database:
        database.write(version + b"".join(track(*t) for t in TRACKS))
    crate_version = field("vrsn", text("1.0/Serato ScratchLive Crate"))
    with open("Warmup.crate", "wb") as crate:
        crate.write(crate_version + b"".join(field("otrk", field("ptrk", text(p))) for p in CRATE))


if __name__ == "__main__":
    main()
