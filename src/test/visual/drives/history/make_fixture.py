#!/usr/bin/env python3
"""Writes the .bitedj/history.sqlite the history_loading scenario's stick carries.

Two sets, as FsHistoryStore stores them: paths relative to the drive root,
positions in play order. The newer one is what tapping the drive opens.

  2026-10-24 -> Music/reference.flac, Music/same.flac, Music/far.flac
  2026-10-25 -> Music/up1.flac, Music/down1.flac, Music/relative.flac,
                Music/reference.flac

None of the tracks is in the fresh library the scenario starts with, so every
one of them is added on this unit while the set is on screen. Run from this
directory to regenerate history.sqlite.
"""

import os
import sqlite3

SESSIONS = {
    "2026-10-24": ["Music/reference.flac", "Music/same.flac", "Music/far.flac"],
    "2026-10-25": [
        "Music/up1.flac",
        "Music/down1.flac",
        "Music/relative.flac",
        "Music/reference.flac",
    ],
}

PATH = "history.sqlite"

if os.path.exists(PATH):
    os.remove(PATH)
db = sqlite3.connect(PATH)
# Same DDL as kCreateTableDdl in src/library/dao/fshistorystore.cpp.
db.execute(
    "CREATE TABLE IF NOT EXISTS history ("
    "session TEXT NOT NULL, "
    "position INTEGER NOT NULL, "
    "location TEXT NOT NULL, "
    "duration_seconds INTEGER, "
    "played_at TEXT, "
    "PRIMARY KEY (session, position))"
)
for session, locations in SESSIONS.items():
    for position, location in enumerate(locations, start=1):
        db.execute(
            "INSERT INTO history VALUES (?, ?, ?, ?, ?)",
            (session, position, location, 30, f"{session}T21:{position:02d}:00Z"),
        )
db.commit()
db.close()
