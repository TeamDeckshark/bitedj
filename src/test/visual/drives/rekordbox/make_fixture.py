#!/usr/bin/env python3
"""Writes a Rekordbox export.pdb for the fake sticks the Rekordbox scenarios mount.

The default (no arguments) is the rekordbox_duplicates fixture: a collection in
which three tracks are the same audio file, the way rekordbox exports a file
the DJ imported more than once. Each copy has its own track ID and a playlist
may name any of them, so the "Set" playlist below lists five entries that are
only three files, and every one of them must show up:

  Set  -> Opener (1), Anthem (2), Anthem (5), Closer (3), Anthem (6)

`--tracks N` instead writes N distinct tracks (with an all-tracks "Big" playlist
and a 50-track "Short" one) for measuring how a large stick loads; nothing
holds the audio, which the library list does not need.

The layout follows lib/rekordbox-metadata/rekordbox_pdb.ksy: 4 KiB pages, page
0 a header listing one page chain per table, each data page a 0x28-byte header,
a heap of rows growing up and a row index of 16-row groups growing down from
the end of the page. Run from this directory to regenerate export.pdb.
"""

import argparse
import struct

LEN_PAGE = 4096
HEAP_POS = 0x28
GROUP_SIZE = 0x24

TRACKS, GENRES, ARTISTS, ALBUMS, KEYS, PLAYLIST_TREE, PLAYLIST_ENTRIES = 0, 1, 2, 3, 5, 7, 8


def device_string(text):
    """A device_sql_string: short ASCII, long ASCII or long UTF-16LE."""
    try:
        data = text.encode("ascii")
    except UnicodeEncodeError:
        data = text.encode("utf-16-le")
        return struct.pack("<BHB", 0x90, len(data) + 4, 0) + data
    if len(data) <= 126:
        return bytes([((len(data) + 1) << 1) | 1]) + data
    return struct.pack("<BHB", 0x40, len(data) + 4, 0) + data


def with_strings(head, strings):
    """`head` followed by `strings`, padded to 4 bytes; returns the row and
    each string's offset from the start of the row."""
    row = bytearray(head)
    offsets = []
    for text in strings:
        offsets.append(len(row))
        row += device_string(text)
    return row, offsets


def key_row(id_, name):
    return bytes(with_strings(struct.pack("<II", id_, id_), [name])[0])


def genre_row(id_, name):
    return bytes(with_strings(struct.pack("<I", id_), [name])[0])


def artist_row(id_, name):
    # subtype 0x60: the name's offset is the one-byte ofs_name_near.
    return struct.pack("<HHIBB", 0x60, 0, id_, 3, 10) + device_string(name)


def album_row(id_, artist_id, name):
    return struct.pack("<HHIIIIBB", 0x80, 0, 0, artist_id, id_, 0, 3, 22) + device_string(name)


def playlist_tree_row(id_, parent_id, sort_order, is_folder, name):
    return struct.pack("<IIIII", parent_id, 0, sort_order, id_, is_folder) + device_string(name)


def playlist_entry_row(entry_index, track_id, playlist_id):
    return struct.pack("<III", entry_index, track_id, playlist_id)


def track_row(t):
    head = struct.pack(
        "<HHIIIII HH IIIIIIIIIIII HHHHHH BB HH",
        0x24, 0, 0, 44100, 0, 1_000_000, 0,
        19048, 30967,
        0, t["key_id"], 0, 0, 0, 320, t["number"], t["bpm"] * 100,
        t["genre_id"], t["album_id"], t["artist_id"], t["id"],
        0, 0, 2024, 16, t["duration"], 41,
        0, 0,
        1, 3)
    strings = [""] * 21
    strings[10] = "2026-10-01"
    strings[17] = t["title"]
    strings[19] = t["path"].rsplit("/", 1)[-1]
    strings[20] = t["path"]
    # The 21 string offsets come straight after the fixed part of the row.
    row, offsets = with_strings(head + bytes(42), strings)
    row[len(head):len(head) + 42] = struct.pack("<21H", *offsets)
    return bytes(row)


def pages_for(rows):
    """Packs `rows` into as many pages' worth of (heap, offsets) as needed."""
    pages = []
    heap, offsets = bytearray(), []
    for row in rows:
        row = row + bytes(-len(row) % 4)
        groups = len(offsets) // 16 + 1
        if offsets and HEAP_POS + len(heap) + len(row) + groups * GROUP_SIZE > LEN_PAGE:
            pages.append((heap, offsets))
            heap, offsets = bytearray(), []
        offsets.append(len(heap))
        heap += row
    if offsets:
        pages.append((heap, offsets))
    return pages


def page_bytes(index, type_, next_page, heap, offsets):
    page = bytearray(LEN_PAGE)
    num_rows = len(offsets)
    # An empty table is a single non-data page (flag 0x40): its row index is
    # never parsed, where an empty data page's would underflow to 2^60 groups.
    flags = 0x34 if num_rows else 0x64
    used = len(heap)
    free = LEN_PAGE - HEAP_POS - used - ((num_rows + 15) // 16) * GROUP_SIZE
    counts = num_rows | (num_rows << 13)  # num_rows:13, num_rows_valid:11
    struct.pack_into("<IIIII4s", page, 0, 0, index, type_, next_page, 1, b"\0" * 4)
    page[0x18:0x1B] = counts.to_bytes(3, "little")
    struct.pack_into("<BHHHHHH", page, 0x1B, flags, free, used, 0, num_rows, 0, 0)
    page[HEAP_POS:HEAP_POS + used] = heap
    for group in range(0, num_rows, 16):
        base = LEN_PAGE - (group // 16) * GROUP_SIZE
        present = 0
        for i, offset in enumerate(offsets[group:group + 16]):
            struct.pack_into("<H", page, base - (6 + 2 * i), offset)
            present |= 1 << i
        struct.pack_into("<H", page, base - 4, present)
    return bytes(page)


def write_pdb(path, tables):
    """`tables`: (page type, [row bytes]) in the order to lay them out."""
    pages = [None]
    entries = []
    for type_, rows in tables:
        chunks = pages_for(rows) or [(bytearray(), [])]
        first = len(pages)
        for n, (heap, offsets) in enumerate(chunks):
            index = first + n
            last = n == len(chunks) - 1
            # The last page's next_page points past the end of the file.
            next_page = 0x03FFFFFF if last else index + 1
            pages.append(page_bytes(index, type_, next_page, heap, offsets))
        entries.append((type_, first, len(pages) - 1))

    header = bytearray(LEN_PAGE)
    struct.pack_into("<IIIIIII", header, 0, 0, LEN_PAGE, len(entries), len(pages), 5, 1, 0)
    for n, (type_, first, last) in enumerate(entries):
        struct.pack_into("<IIII", header, 28 + 16 * n, type_, last + 1, first, last)
    pages[0] = bytes(header)
    with open(path, "wb") as pdb:
        pdb.write(b"".join(pages))


def build(tracks, playlists):
    """`playlists`: (id, name, [track ids]); returns the tables to write."""
    artists = sorted({t["artist"] for t in tracks})
    albums = sorted({t["album"] for t in tracks})
    genres = sorted({t["genre"] for t in tracks})
    keys = sorted({t["key"] for t in tracks})
    for t in tracks:
        t["artist_id"] = artists.index(t["artist"]) + 1
        t["album_id"] = albums.index(t["album"]) + 1
        t["genre_id"] = genres.index(t["genre"]) + 1
        t["key_id"] = keys.index(t["key"]) + 1
    return [
        (KEYS, [key_row(n + 1, k) for n, k in enumerate(keys)]),
        (GENRES, [genre_row(n + 1, g) for n, g in enumerate(genres)]),
        (ARTISTS, [artist_row(n + 1, a) for n, a in enumerate(artists)]),
        (ALBUMS, [album_row(n + 1, 1, a) for n, a in enumerate(albums)]),
        (TRACKS, [track_row(t) for t in tracks]),
        (PLAYLIST_TREE, [playlist_tree_row(pid, 0, n, 0, name)
                         for n, (pid, name, _) in enumerate(playlists)]),
        (PLAYLIST_ENTRIES, [playlist_entry_row(i + 1, tid, pid)
                            for pid, _, ids in playlists for i, tid in enumerate(ids)]),
    ]


def track(id_, title, artist, path, key="8A", bpm=124, genre="House", album="Night Bus"):
    return dict(id=id_, title=title, artist=artist, album=album, genre=genre, key=key,
                bpm=bpm, path=path, number=id_, duration=8)


def duplicates_fixture():
    anthem = "/Contents/Kalo/Night Bus/Anthem.flac"
    tracks = [
        track(1, "Opener", "Kalo", "/Contents/Kalo/Night Bus/Opener.flac", "7A", 120),
        track(2, "Anthem", "Kalo", anthem),
        track(3, "Closer", "Mira", "/Contents/Mira/Lanterns/Closer.flac", "9A", 126, "Techno",
              "Lanterns"),
        track(4, "Interlude", "Mira", "/Contents/Mira/Lanterns/Interlude.flac", "8B", 122,
              "Techno", "Lanterns"),
        # The same file imported twice more into the rekordbox collection.
        track(5, "Anthem", "Kalo", anthem),
        track(6, "Anthem", "Kalo", anthem),
    ]
    return tracks, [(1, "Set", [1, 5, 3, 6, 2])]


def large_fixture(count):
    tracks = [
        track(n, "Track %05d" % n, "Artist %03d" % (n % 211),
              "/Contents/Artist %03d/Album %03d/Track %05d.mp3" % (n % 211, n % 97, n),
              "%d%s" % (n % 12 + 1, "AB"[n % 2]), 100 + n % 60, "Genre %02d" % (n % 23),
              "Album %03d" % (n % 97))
        for n in range(1, count + 1)
    ]
    ids = [t["id"] for t in tracks]
    return tracks, [(1, "Big", ids), (2, "Short", ids[:50])]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--tracks", type=int, help="write N distinct tracks instead")
    parser.add_argument("--out", default="export.pdb")
    args = parser.parse_args()
    tracks, playlists = large_fixture(args.tracks) if args.tracks else duplicates_fixture()
    write_pdb(args.out, build(tracks, playlists))


if __name__ == "__main__":
    main()
