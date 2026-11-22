#!/usr/bin/env python3
"""Deterministic generator for the checked-in Sonix Audiobookshelf API fixtures.

Writes the fixture sets required by Bob spec `.specs/bob-abs-features-spec.md` §14.6
(subset around `tools/fixtures/audiobookshelf/<alias>/`) for three normative
Audiobookshelf server revisions:

  pinned-3563d49  → ABS commit 3563d49424d50170324de9236a24c591b6d965e9
  current-5de038d → ABS commit 5de038dafda294d13ca48861eda7d4405bbd7368
  eric-v2.38.0    → ABS commit 1430c9fc642d586ff91c8d4cbed20aab2c47e473 (v2.38.0)

Every fixture payload is produced from fixed constants (no clock, no RNG, no set
iteration), so reruns produce byte-identical files. The scenario data is identical
across all three aliases; only the provenance (`source.json`) differs per alias.

The sort/filter/ordering semantics used to compute the recorded expected values are
imported from `audiobookshelf_mock.py` so the mock and the expectations cannot drift.

Usage:
  python3 tools/abs_fixtures_generate.py            # regenerate all three alias dirs
  python3 tools/abs_fixtures_generate.py --check    # verify checked-in bytes only
"""

import argparse
import base64
import hashlib
import json
import shutil
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
FIXTURE_ROOT = TOOLS_DIR / "fixtures" / "audiobookshelf"

# ---------------------------------------------------------------------------
# Normative upstream facts (captured 2026-11-22 via `gh api` against
# advplyr/audiobookshelf; kept here so regeneration never needs network access).
# ---------------------------------------------------------------------------

NORMATIVE_COMMITS = {
    "pinned-3563d49": {
        "commit": "3563d49424d50170324de9236a24c591b6d965e9",
        "tag": None,
        "committed_date": "2026-09-29T22:30:00Z",
    },
    "current-5de038d": {
        "commit": "5de038dafda294d13ca48861eda7d4405bbd7368",
        "tag": None,
        "committed_date": "2026-11-19T23:39:34Z",
    },
    "eric-v2.38.0": {
        "commit": "1430c9fc642d586ff91c8d4cbed20aab2c47e473",
        "tag": "v2.38.0",
        "committed_date": "2026-11-17T23:09:20Z",
    },
}

DERIVED_SOURCE_PATHS = [
    "server/controllers/LibraryController.js",
    "server/controllers/LibraryItemController.js",
    "server/controllers/MeController.js",
    "server/controllers/PodcastController.js",
    "server/controllers/SearchController.js",
    "server/models/Library.js",
    "server/models/LibraryItem.js",
    "server/models/Podcast.js",
    "server/models/PodcastEpisode.js",
    "server/models/Book.js",
    "server/models/Author.js",
    "server/models/Series.js",
    "server/models/MediaProgress.js",
    "server/objects/files/AudioFile.js",
    "server/objects/files/AudioTrack.js",
]

# Git blob SHA-1 of every derived source path at each normative commit.
BLOB_HASHES = {
    "pinned-3563d49": {
        "server/controllers/LibraryController.js": "6bf36a7b44cdc3c2125a5d72ae1610fa2b27bc33",
        "server/controllers/LibraryItemController.js": "18b1dc2660dc14f6a45ee48d69c8bf565967977c",
        "server/controllers/MeController.js": "ca600d373e79612553bff79ab7296634f47eb603",
        "server/controllers/PodcastController.js": "5eaa2a64a349428fd469f23a96482746eaa4de46",
        "server/controllers/SearchController.js": "ecd9a41c052a0e284679bc378bc886773e57399a",
        "server/models/Library.js": "708880aad352417e9391edbcf611dea221527c9a",
        "server/models/LibraryItem.js": "4876d52e643d9d7f9977e66fcf9fdcb70b1d7b38",
        "server/models/Podcast.js": "9c222e48a5a669a3ed4e97b11f5468fdd6ef5e65",
        "server/models/PodcastEpisode.js": "996f55f58e1437bcdd323aa0fb8e23c3b6d56943",
        "server/models/Book.js": "19923750ec49776dd8947c5395666b1b68832175",
        "server/models/Author.js": "65561e211a35edff9166a516a44c198274634eb2",
        "server/models/Series.js": "6ca288464f11c9fb16d3f1f291340dd463602c02",
        "server/models/MediaProgress.js": "9c0269a9e939353fc72c5ed2b863b1e933ebcd54",
        "server/objects/files/AudioFile.js": "c0c425ba32ef9c71a46040b1b60f8075fa2e793e",
        "server/objects/files/AudioTrack.js": "a27c2a016a0dba7f415d169260d52cdfb0707c81",
    },
    "current-5de038d": {
        "server/controllers/LibraryController.js": "ac23b683f6526e3a89eadce62aa265beff2634ed",
        "server/controllers/LibraryItemController.js": "18b1dc2660dc14f6a45ee48d69c8bf565967977c",
        "server/controllers/MeController.js": "03841b5cacc76e0c8f78c58b5c6dd59238849fe0",
        "server/controllers/PodcastController.js": "5eaa2a64a349428fd469f23a96482746eaa4de46",
        "server/controllers/SearchController.js": "ecd9a41c052a0e284679bc378bc886773e57399a",
        "server/models/Library.js": "708880aad352417e9391edbcf611dea221527c9a",
        "server/models/LibraryItem.js": "4876d52e643d9d7f9977e66fcf9fdcb70b1d7b38",
        "server/models/Podcast.js": "9c222e48a5a669a3ed4e97b11f5468fdd6ef5e65",
        "server/models/PodcastEpisode.js": "996f55f58e1437bcdd323aa0fb8e23c3b6d56943",
        "server/models/Book.js": "0446b3d9070917dd48936cb1abe861236ba7704a",
        "server/models/Author.js": "525f557a8de16e369807e5affc845828ee9e5564",
        "server/models/Series.js": "6ca10a9ba0ca143236c8d8cd9b1133f87235efe5",
        "server/models/MediaProgress.js": "9c0269a9e939353fc72c5ed2b863b1e933ebcd54",
        "server/objects/files/AudioFile.js": "c0c425ba32ef9c71a46040b1b60f8075fa2e793e",
        "server/objects/files/AudioTrack.js": "a27c2a016a0dba7f415d169260d52cdfb0707c81",
    },
    "eric-v2.38.0": {
        "server/controllers/LibraryController.js": "ac23b683f6526e3a89eadce62aa265beff2634ed",
        "server/controllers/LibraryItemController.js": "18b1dc2660dc14f6a45ee48d69c8bf565967977c",
        "server/controllers/MeController.js": "03841b5cacc76e0c8f78c58b5c6dd59238849fe0",
        "server/controllers/PodcastController.js": "5eaa2a64a349428fd469f23a96482746eaa4de46",
        "server/controllers/SearchController.js": "ecd9a41c052a0e284679bc378bc886773e57399a",
        "server/models/Library.js": "708880aad352417e9391edbcf611dea221527c9a",
        "server/models/LibraryItem.js": "4876d52e643d9d7f9977e66fcf9fdcb70b1d7b38",
        "server/models/Podcast.js": "9c222e48a5a669a3ed4e97b11f5468fdd6ef5e65",
        "server/models/PodcastEpisode.js": "996f55f58e1437bcdd323aa0fb8e23c3b6d56943",
        "server/models/Book.js": "0446b3d9070917dd48936cb1abe861236ba7704a",
        "server/models/Author.js": "65561e211a35edff9166a516a44c198274634eb2",
        "server/models/Series.js": "6ca10a9ba0ca143236c8d8cd9b1133f87235efe5",
        "server/models/MediaProgress.js": "9c0269a9e939353fc72c5ed2b863b1e933ebcd54",
        "server/objects/files/AudioFile.js": "c0c425ba32ef9c71a46040b1b60f8075fa2e793e",
        "server/objects/files/AudioTrack.js": "a27c2a016a0dba7f415d169260d52cdfb0707c81",
    },
}

# ---------------------------------------------------------------------------
# Scenario constants (identical across aliases).
# ---------------------------------------------------------------------------

SCENARIO_SCHEMA = "sonix-abs-fixture-scenario-1"
SOURCE_SCHEMA = "sonix-abs-fixture-source-1"

# §14.6: an expanded show larger than 3 MiB.
BIG_SHOW_MIN_BYTES = 3 * 1024 * 1024
BIG_SHOW_EPISODES = 120
BIG_DESC_SEGMENT = (
    "Fixture Long Show episode description sentence, repeated deterministically "
    "so the expanded show payload exceeds three MiB without any randomness. "
)
# len(BIG_DESC_SEGMENT) ≈ 158 bytes → ~176 repeats ≈ 28 KiB per episode;
# 120 episodes ≈ 3.4 MiB total, a safe margin above the 3 MiB gate.
BIG_DESC_REPEATS = 176

# A valid 1x1 white JPEG (deterministic, decoded by any image stack).
COVER_JPEG_B64 = (
    "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0a"
    "HBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAFAABAAAAAAAA"
    "AAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAD8A0s8g/9k="
)

PAGE_LIMIT = 40

DETAIL_BOOKS = ["bk-full", "bk-minimal", "bk-chapters-0", "bk-chapters-1",
                "bk-chapters-400", "bk-chapters-401"]
DETAIL_SHOWS = ["pod-41", "pod-big"]

# Shared deterministic helpers (identity derivation, audio shapes) come from
# the mock module so fixture files and mock-served synthesis cannot drift.
sys.path.insert(0, str(TOOLS_DIR))
from audiobookshelf_mock import (build_audio_file as audio_file,  # noqa: E402
                                 build_audio_track as audio_track,
                                 stable_ino)

VIRTUAL_DETAIL_NOTE = (
    "Items without a checked-in expanded detail file are expanded at request time "
    "by the mock from the dataset row: books get numTracks virtual tracks with "
    "ino=stable_ino(item_id, k) and chapters=[]; podcast shows get numEpisodes "
    "virtual episodes where every 11th episode has publishedAt=null (unknown date "
    "coverage). The derivation is deterministic in the item id and ordinal."
)

MIXED_FILLER = [
    ("lib-mix-8", "Fixture Mixed Shelf Eight", "book", 2),
    ("lib-mix-9", "Fixture Mixed Shelf Nine", "podcast", 2),
    ("lib-mix-10", "Fixture Mixed Shelf Ten", "book", 3),
    ("lib-mix-11", "Fixture Mixed Shelf Eleven", "podcast", 3),
    ("lib-mix-12", "Fixture Mixed Shelf Twelve", "book", 1),
    ("lib-mix-13", "Fixture Mixed Shelf Thirteen", "podcast", 1),
    ("lib-mix-14", "Fixture Mixed Shelf Fourteen", "book", 2),
    ("lib-mix-15", "Fixture Mixed Shelf Fifteen", "podcast", 2),
    ("lib-mix-16", "Fixture Mixed Shelf Sixteen", "book", 1),
    ("lib-mix-17", "Fixture Mixed Shelf Seventeen", "podcast", 1),
    ("lib-mix-18", "Fixture Mixed Shelf Eighteen", "book", 1),
]


def book_row(item_id, title, author_name, author_ids, series, tag_ids, genre_ids,
             ordinal, duration, num_tracks):
    # §14.1 legacy compatibility: the pinned single-book row keeps the exact
    # pre-0.6.0 coverPath; every other item uses the standard API cover route.
    cover_path = ("/fixture/cover.jpg" if item_id == "mock-book"
                  else f"/api/items/{item_id}/cover")
    return {
        "id": item_id,
        "kind": "book",
        "title": title,
        "author_name": author_name,
        "author_ids": list(author_ids),
        "series": [dict(entry) for entry in series],
        "tag_ids": list(tag_ids),
        "genre_ids": list(genre_ids),
        "added_at": 1700000000000 + ordinal * 1000,
        "duration": duration,
        "num_tracks": num_tracks,
        "num_episodes": 0,
        "cover_path": cover_path,
    }


def podcast_row(item_id, title, author_name, author_ids, tag_ids, genre_ids,
                ordinal, num_episodes):
    return {
        "id": item_id,
        "kind": "podcast",
        "title": title,
        "author_name": author_name,
        "author_ids": list(author_ids),
        "series": [],
        "tag_ids": list(tag_ids),
        "genre_ids": list(genre_ids),
        "added_at": 1700000000000 + ordinal * 1000,
        "duration": 0.0,
        "num_tracks": 0,
        "num_episodes": num_episodes,
        "cover_path": f"/api/items/{item_id}/cover",
    }


def build_libraries():
    """The exact GET /api/libraries payload plus the ordering metadata."""
    libraries = [
        {"id": "mock-library", "name": "Mock Audiobooks", "mediaType": "book"},
        {"id": "lib-search-1", "name": "Fixture Search Range", "mediaType": "book"},
        {"id": "lib-list-1", "name": "Fixture Fiction Shelf", "mediaType": "book"},
        {"id": "lib-pod-1", "name": "Fixture Podcast Rack", "mediaType": "podcast"},
        {"id": "lib-pod-2", "name": "Fixture Talk Shows", "mediaType": "podcast"},
        {"id": "lib-pod-3", "name": "Fixture Long Shows", "mediaType": "podcast"},
        {"id": "lib-detail-1", "name": "Fixture Details Shelf", "mediaType": "book"},
    ]
    for lib_id, name, media_type, _count in MIXED_FILLER:
        libraries.append({"id": lib_id, "name": name, "mediaType": media_type})
    return {"libraries": libraries}


def build_dataset():
    """Rows, corpora, filterdata, seeds, and expected semantics (alias-independent)."""
    items = {}

    # -- legacy compatibility library: exactly one item, byte-compatible values.
    items["mock-library"] = [book_row(
        "mock-book", "The Mock Journey", "Test Author", ["author-1"],
        [{"id": "series-1", "name": "Fixture Series", "sequence": "2"}],
        ["tag-legacy"], ["genre-legacy"], 0, 20.0, 2)]

    # -- search-range library feeding the 0/40/100/101 corpora.
    search_rows = []
    for i in range(1, 106):
        search_rows.append(book_row(
            f"sc-{i:03d}", f"Fixture Search Item {i:03d}", "Fixture Search Author",
            ["author-sr"], [], ["tag-search"], ["genre-search"], i, 600.0, 1))
    items["lib-search-1"] = search_rows

    # -- paginated book library: 75 rows → pages 40 + 35 at the client's limit 40.
    list_rows = []
    for i in range(1, 76):
        author_id = f"author-fa-{(i - 1) % 3 + 1}"
        author_name = {1: "Fixture Author One", 2: "Fixture Author Two",
                       3: "Fixture Author Three"}[(i - 1) % 3 + 1]
        series_id = 2 if i % 2 == 0 else 1
        tag_ids = ["ta-fa-1"] if i <= 40 else ["ta-fa-2"]
        genre_ids = ["ge-fa-1"] if i % 5 == 1 else ["ge-fa-2"]
        list_rows.append(book_row(
            f"pg-bk-{i:03d}", f"Fixture Paginated Book {i:03d}", author_name,
            [author_id],
            [{"id": f"series-fa-{series_id}",
              "name": {1: "Fixture Series One", 2: "Fixture Series Two"}[series_id],
              "sequence": str((i - 1) % 2 + 1)}],
            tag_ids, genre_ids, i, 300.0 + (i % 7) * 10.0, 1 + i % 4))
    items["lib-list-1"] = list_rows

    # -- paginated podcast library: 65 shows → pages 40 + 25.
    rack_rows = []
    for i in range(1, 66):
        tag_ids = ["ta-pod-1"] if i <= 32 else ["ta-pod-2"]
        genre_ids = ["ge-pod-1"] if i % 2 == 0 else ["ge-pod-2"]
        rack_rows.append(podcast_row(
            f"pod-rack-{i:03d}", f"Fixture Rack Podcast {i:03d}",
            "Fixture Podcast Author", [], tag_ids, genre_ids, i,
            12 + (i % 26)))
    items["lib-pod-1"] = rack_rows

    # -- talk-show library hosting the named 41-episode show.
    items["lib-pod-2"] = [
        podcast_row("pod-41", "Fixture Forty-One Podcast", "Fixture Podcast Author",
                    [], ["ta-pod-1"], ["ge-pod-1"], 1, 41),
        podcast_row("pod-talk-1", "Fixture Talk Show One", "Fixture Podcast Author",
                    [], [], ["ge-pod-2"], 2, 8),
        podcast_row("pod-talk-2", "Fixture Talk Show Two", "Fixture Podcast Author",
                    [], [], ["ge-pod-2"], 3, 5),
        podcast_row("pod-talk-3", "Fixture Talk Show Three", "Fixture Podcast Author",
                    [], [], ["ge-pod-2"], 4, 6),
    ]

    # -- long-show library hosting the >3 MiB expanded show.
    items["lib-pod-3"] = [
        podcast_row("pod-big", "Fixture Long Show", "Fixture Podcast Author",
                    [], ["ta-pod-2"], ["ge-pod-2"], 1, BIG_SHOW_EPISODES),
        podcast_row("pod-long-1", "Fixture Long Show One", "Fixture Podcast Author",
                    [], [], ["ge-pod-1"], 2, 30),
        podcast_row("pod-long-2", "Fixture Long Show Two", "Fixture Podcast Author",
                    [], [], ["ge-pod-1"], 3, 34),
    ]

    # -- details shelf: full/minimal book details plus 0/1/400/401 chapter variants.
    detail_rows = []
    for ordinal, item_id in enumerate(DETAIL_BOOKS, start=1):
        chapters = {"bk-chapters-0": 0, "bk-chapters-1": 1, "bk-chapters-400": 400,
                    "bk-chapters-401": 401}.get(item_id, 5)
        detail_rows.append(book_row(
            item_id,
            {"bk-full": "Fixture Full Details Book",
             "bk-minimal": "Fixture Minimal Book",
             "bk-chapters-0": "Fixture Zero Chapter Book",
             "bk-chapters-1": "Fixture One Chapter Book",
             "bk-chapters-400": "Fixture Four Hundred Chapter Book",
             "bk-chapters-401": "Fixture Four Hundred One Chapter Book"}[item_id],
            "Fixture Detail Author", ["author-detail"],
            [{"id": "series-detail", "name": "Fixture Detail Series",
              "sequence": str(ordinal)}] if item_id != "bk-minimal" else [],
            ["ta-detail"], ["ge-detail"], ordinal, float(chapters * 300), 3))
    items["lib-detail-1"] = detail_rows

    # -- mixed filler libraries so every library has browsable content.
    for lib_id, name, media_type, count in MIXED_FILLER:
        rows = []
        for ordinal in range(1, count + 1):
            if media_type == "book":
                rows.append(book_row(
                    f"{lib_id}-bk-{ordinal}", f"{name} Book {ordinal}",
                    "Fixture Mixed Author", [f"author-{lib_id}"], [],
                    [], ["genre-mixed"], ordinal, 240.0, 1))
            else:
                rows.append(podcast_row(
                    f"{lib_id}-pod-{ordinal}", f"{name} Podcast {ordinal}",
                    "Fixture Mixed Author", [f"author-{lib_id}"], [], [],
                    ordinal, 9 + ordinal))
        items[lib_id] = rows

    filterdata = {
        "mock-library": {
            # Exact legacy shape: the pinned fixture must not disturb §14.1 runs.
            "authors": [{"id": "author-1", "name": "Test Author"}],
            "series": [{"id": "series-1", "name": "Fixture Series"}],
        },
        "lib-search-1": {
            "authors": [{"id": "author-sr", "name": "Fixture Search Author"}],
            "series": [],
        },
        "lib-list-1": {
            "authors": [
                {"id": "author-fa-1", "name": "Fixture Author One"},
                {"id": "author-fa-2", "name": "Fixture Author Two"},
                {"id": "author-fa-3", "name": "Fixture Author Three"},
            ],
            "series": [
                {"id": "series-fa-1", "name": "Fixture Series One"},
                {"id": "series-fa-2", "name": "Fixture Series Two"},
            ],
            "tags": [
                {"id": "ta-fa-1", "name": "Fixture Tag Alpha"},
                {"id": "ta-fa-2", "name": "Fixture Tag Beta"},
            ],
            "genres": [
                {"id": "ge-fa-1", "name": "Fiction"},
                {"id": "ge-fa-2", "name": "Nonfiction"},
            ],
        },
        "lib-pod-1": {
            "authors": [], "series": [],
            "tags": [{"id": "ta-pod-1", "name": "Interviews"},
                     {"id": "ta-pod-2", "name": "News"}],
            "genres": [{"id": "ge-pod-1", "name": "Comedy"},
                       {"id": "ge-pod-2", "name": "History"}],
        },
        "lib-detail-1": {
            "authors": [{"id": "author-detail", "name": "Fixture Detail Author"}],
            "series": [{"id": "series-detail", "name": "Fixture Detail Series"}],
        },
    }
    for lib_id, name, _media_type, _count in MIXED_FILLER:
        filterdata[lib_id] = {"authors": [{"id": f"author-{lib_id}",
                                           "name": "Fixture Mixed Author"}]}

    # Declared corpora (match lists), keyed by exact lowercase query.
    search_corpora = {
        "mock-library": {
            "response": "legacy-book-only",
            "legacy_rule": "legacy-substring",
            "match_target": "the mock journey",
            "keys": {"m": ["mock-book"]},
        },
        "lib-search-1": {
            "response": "canonical",
            "keys": {
                "zero": [],
                "fourty": [f"sc-{i:03d}" for i in range(1, 41)],
                "hundred": [f"sc-{i:03d}" for i in range(1, 101)],
                "overflow": [f"sc-{i:03d}" for i in range(1, 102)],
            },
        },
        "lib-pod-1": {
            "response": "canonical",
            "keys": {
                "pod": [f"pod-rack-{i:03d}" for i in range(1, 7)],
                "fresh": [],
            },
        },
        "lib-pod-2": {
            "response": "canonical",
            "keys": {"twoshows": ["pod-41", "pod-talk-1"]},
        },
    }

    # Two-server progress seeds: identical IDs, diverging per credential. The
    # "__status__" marker means the credential sees ABS's 404 no-progress error.
    progress_seeds = {
        "mock-book": {
            "credential-a": {"currentTime": 13.0, "isFinished": False,
                             "lastUpdate": 1700000000000},
            "credential-b": {"currentTime": 17.5, "isFinished": False,
                             "lastUpdate": 1700000001999},
        },
        "pg-bk-001": {
            "credential-a": {"currentTime": 45.0, "duration": 810.0,
                             "isFinished": False, "lastUpdate": 1700000002000},
            "credential-b": {"__status__": 404},
        },
        "bk-full": {
            "credential-a": {"currentTime": 3.0, "duration": 3000.0,
                             "isFinished": False, "lastUpdate": 1700000003000},
            "credential-b": {"currentTime": 2997.0, "duration": 3000.0,
                             "isFinished": False, "lastUpdate": 1700000003001},
        },
        "bk-chapters-400": {
            "credential-a": {"__status__": 404},
            "credential-b": {"currentTime": 60000.0, "duration": 120000.0,
                             "isFinished": False, "lastUpdate": 1700000004000},
        },
        "pod-41/pod41-ep-001": {
            "credential-a": {"currentTime": 123.5, "duration": 1200.0,
                             "isFinished": False, "lastUpdate": 1700000005000},
            "credential-b": {"currentTime": 1200.0, "duration": 1200.0,
                             "isFinished": True, "lastUpdate": 1700000005001},
        },
        "pod-41/pod41-ep-002": {
            "credential-a": {"__status__": 404},
            "credential-b": {"currentTime": 47.0, "duration": 1200.0,
                             "isFinished": False, "lastUpdate": 1700000006000},
        },
        "pod-big/podbig-ep-010": {
            "credential-a": {"currentTime": 5.0, "duration": 1800.0,
                             "isFinished": False, "lastUpdate": 1700000007000},
            "credential-b": {"__status__": 404},
        },
    }

    dataset = {
        "schema": SCENARIO_SCHEMA,
        "two_server_scenario": {
            "identical_ids": True,
            "credentials": ["credential-a", "credential-b"],
            "note": ("Both credentials authenticate against the same fixture "
                     "URLs and see identical library/item/show/episode IDs, "
                     "filterdata, corpora, and bytes; only per-item progress "
                     "diverges per the seeds above. Token values are supplied at "
                     "mock start time and are never stored in these fixtures."),
        },
        "virtual_detail": VIRTUAL_DETAIL_NOTE,
        "detail_index": {
            **{item_id: f"scenario/details/{item_id}.expanded.json"
               for item_id in DETAIL_BOOKS + DETAIL_SHOWS},
            # The legacy item's checked-in file keeps §14.1 byte compatibility.
            "mock-book": "scenario/details/mock-book.expanded.json",
        },
        "items": items,
        "filterdata": filterdata,
        "search_corpora": search_corpora,
        "progress_seeds": progress_seeds,
    }
    return dataset


# ---------------------------------------------------------------------------
# Expanded detail payloads (ABS v2.38.0-verified shapes).
# ---------------------------------------------------------------------------

def book_expanded(item_id, title, description=None, narrator=None, chapters=None,
                  published_year=None, publisher=None, genres=None, tags=None,
                  series=None, track_count=1, duration=3600.0):
    metadata = {"title": title, "authorName": "Fixture Detail Author"}
    if description is not None:
        metadata["description"] = description
    if narrator is not None:
        metadata["narrator"] = narrator
    if published_year is not None:
        metadata["publishedYear"] = str(published_year)
    if publisher is not None:
        metadata["publisher"] = publisher
    if genres is not None:
        metadata["genres"] = genres
    if tags is not None:
        metadata["tags"] = tags
    if series is not None:
        metadata["series"] = series
        metadata["seriesName"] = series[0]["name"]
    chapter_list = []
    for i in range(chapters or 0):
        chapter_list.append({
            "index": i + 1,
            "startOffset": round(i * 300.0, 2),
            "endOffset": round(min((i + 1) * 300.0, max(duration, chapters * 300.0)), 2),
            "title": f"Chapter {i + 1}",
        })
    tracks = []
    for k in range(track_count):
        ino = stable_ino(item_id, k + 1)
        tracks.append(audio_track(item_id, ino, f"Fixture Part {k + 1}.mp3",
                                  round(duration / track_count, 2), index=k + 1,
                                  start_offset=round(k * duration / track_count, 2)))
    return {
        "id": item_id,
        "media": {
            "duration": duration,
            "coverPath": f"/api/items/{item_id}/cover",
            "metadata": metadata,
            "tracks": tracks,
            "chapters": chapter_list,
        },
    }


def podcast_episode(item_id, ordinal, title_prefix, description=None,
                    unknown_metadata=False, with_audio_track=True,
                    null_content_url=False, with_enclosure=False):
    """One expanded episode (PodcastEpisode.toOldJSONExpanded shape).

    Episode IDs follow `{show id without dashes}-ep-{ordinal:03d}` so the show
    and all episodes share one deterministic derivation (pod41-ep-002, …).
    """
    ino = stable_ino(item_id, ordinal)
    af_duration = 0.0 if unknown_metadata else 1800.0
    # ABS derives episode size/duration getters from the audioFile; a zero
    # duration/size plus null dates is the documented unknown-metadata shape
    # (the Sonix cache turns nonpositive into NULL, never a fake zero).
    episode = {
        "libraryItemId": item_id,
        "podcastId": item_id,
        "id": f"{item_id.replace('-', '')}-ep-{ordinal:03d}",
        "oldEpisodeId": None,
        "index": ordinal,
        "season": None,
        "episode": None,
        "episodeType": "full",
        "title": f"{title_prefix} — Episode {ordinal:03d}",
        "subtitle": f"Deterministic episode {ordinal:03d}",
        "description": description if description is not None
                       else f"Episode {ordinal:03d} of {title_prefix}.",
        "enclosure": ({"url": f"http://fixture.invalid/enclosure/{item_id}/{ordinal:03d}.mp3",
                       "type": "audio/mpeg", "length": "32000000"}
                      if with_enclosure else None),
        "guid": f"fixture-guid-{item_id}-{ordinal:03d}",
        "pubDate": None if unknown_metadata else "Tue, 14 Nov 2017 01:00:00 GMT",
        "publishedAt": None if unknown_metadata else 1700000000000 + ordinal * 86400000,
        "addedAt": None if unknown_metadata else 1700000000000 + ordinal * 1000,
        "chapters": [],
        "audioFile": audio_file(item_id, ino, f"Episode {ordinal:03d}.mp3", af_duration),
    }
    if with_audio_track:
        track = audio_track(item_id, ino, f"Episode {ordinal:03d}.mp3", af_duration)
        if null_content_url:
            # §7.1 fallback shape: audioTrack exists but carries no contentUrl;
            # the client derives /api/items/{showId}/file/{ino} from audioFile.ino.
            track["contentUrl"] = None
        episode["audioTrack"] = track
    # else: §7.1 fallback shape B — audioTrack absent entirely; the client
    # derives /api/items/{showId}/file/{ino} from audioFile.ino.
    episode["size"] = 0 if unknown_metadata else 1800 * 32000
    episode["duration"] = af_duration
    return episode


def show_expanded(item_id, title, episodes):
    return {
        "id": item_id,
        "media": {
            "metadata": {
                "title": title,
                "author": "Fixture Podcast Author",
                "description": f"Deterministic expanded payload for {title}.",
                "genres": ["Interviews"],
            },
            "coverPath": f"/api/items/{item_id}/cover",
            "episodes": episodes,
        },
    }


def build_details():
    details = {}
    details["mock-book"] = {
        # Byte-compatible with the legacy single-book mock (§14.1 regressions).
        "id": "mock-book",
        "media": {
            "duration": 20,
            "coverPath": "/fixture/cover.jpg",
            "metadata": {
                "title": "The Mock Journey",
                "authorName": "Test Author",
                "description": "A downloaded fixture used to prove resume import.",
                "seriesName": "Fixture Series",
                "series": [{"name": "Fixture Series", "sequence": "2"}],
            },
            "tracks": [
                {"index": 1, "startOffset": 0, "duration": 10, "title": "Part One",
                 "contentUrl": "/api/items/mock-book/file/1", "mimeType": "audio/mpeg",
                 "metadata": {"filename": "Part One.mp3"}},
                {"index": 2, "startOffset": 10, "duration": 10, "title": "Part Two",
                 "contentUrl": "/api/items/mock-book/file/2", "mimeType": "audio/mpeg",
                 "metadata": {"filename": "Part Two.mp3"}},
            ],
        },
    }
    details["bk-full"] = book_expanded(
        "bk-full", "Fixture Full Details Book",
        description=("A deterministically generated book with every optional "
                     "detail field populated, used by detail-page suites."),
        narrator="Fixture Narrator", chapters=10, published_year=2026,
        publisher="Fixture Press", genres=["Fiction"], tags=["detail-full"],
        series=[{"name": "Fixture Detail Series", "sequence": "1"}],
        track_count=3, duration=3000.0)
    details["bk-minimal"] = book_expanded(
        "bk-minimal", "Fixture Minimal Book", chapters=0, track_count=1,
        duration=45.5)
    details["bk-chapters-0"] = book_expanded(
        "bk-chapters-0", "Fixture Zero Chapter Book", chapters=0,
        track_count=1, duration=900.0)
    details["bk-chapters-1"] = book_expanded(
        "bk-chapters-1", "Fixture One Chapter Book", chapters=1,
        track_count=1, duration=300.0)
    details["bk-chapters-400"] = book_expanded(
        "bk-chapters-400", "Fixture Four Hundred Chapter Book", chapters=400,
        track_count=1, duration=120000.0)
    details["bk-chapters-401"] = book_expanded(
        "bk-chapters-401", "Fixture Four Hundred One Chapter Book", chapters=401,
        track_count=1, duration=120300.0)

    # pod-41: the named 41-episode show carrying the §7.1 shape variants.
    episodes = []
    for ordinal in range(1, 42):
        # ep-002: no audioTrack at all (pure audioFile.ino fallback).
        # ep-003: audioTrack present but contentUrl null.
        # ep-004: unknown metadata (nulls).
        # ep-005: RSS enclosure extras.
        episodes.append(podcast_episode(
            "pod-41", ordinal, "Fixture Forty-One Podcast",
            with_audio_track=(ordinal != 2),
            null_content_url=(ordinal == 3),
            unknown_metadata=(ordinal == 4),
            with_enclosure=(ordinal == 5)))
    details["pod-41"] = show_expanded("pod-41", "Fixture Forty-One Podcast", episodes)

    # pod-big: the >3 MiB expanded show.
    long_desc = BIG_DESC_SEGMENT * BIG_DESC_REPEATS
    episodes = [podcast_episode("pod-big", ordinal, "Fixture Long Show",
                                description=f"Episode {ordinal:03d} narrative. {long_desc}")
                for ordinal in range(1, BIG_SHOW_EPISODES + 1)]
    details["pod-big"] = show_expanded("pod-big", "Fixture Long Show", episodes)
    return details


# ---------------------------------------------------------------------------
# Expected semantics (computed with the mock's own helpers to avoid drift).
# ---------------------------------------------------------------------------

def build_expected(dataset):
    """Compute §14.6 expected values using the mock's own ordering/filter helpers."""
    from audiobookshelf_mock import filter_rows, order_rows

    items = dataset["items"]
    expected_pagination = {
        "default_limit": PAGE_LIMIT,
        "lib-list-1": {
            "total": 75, "limit": PAGE_LIMIT, "pages": 2,
            "page0_first": "pg-bk-001", "page0_last": "pg-bk-040",
            "page1_first": "pg-bk-041", "page1_last": "pg-bk-075",
        },
        "lib-pod-1": {
            "total": 65, "limit": PAGE_LIMIT, "pages": 2,
            "page0_first": "pod-rack-001", "page0_last": "pod-rack-040",
            "page1_first": "pod-rack-041", "page1_last": "pod-rack-065",
        },
    }

    books = items["lib-list-1"]
    sort_inputs = ["media.metadata.title", "media.metadata.author",
                   "addedAt", "media.duration", "media.numTracks"]
    expected_sorts = {}
    for sort in sort_inputs:
        for desc in (0, 1):
            ordered = order_rows(books, sort, bool(desc))
            expected_sorts[f"{sort}|desc={desc}"] = {
                "first": ordered[0]["id"], "last": ordered[-1]["id"]}
    expected_sorts["sequence|filter=series-fa-1|desc=0"] = {
        "first": order_rows(
            filter_rows(books, "series", "series-fa-1"), "sequence", False)[0]["id"],
        "last": order_rows(
            filter_rows(books, "series", "series-fa-1"), "sequence", False)[-1]["id"],
    }

    filter_input_groups = {
        # group → (value, expected count). lib-list-1 rows: author cycles i%3,
        # series id 1 = odd rows (38), series id 2 = even rows (37); tags split
        # 40/35 at index 40; genres: every 5th row starting at i=1 (15) vs rest.
        "authors": [("author-fa-1", 25), ("author-fa-2", 25), ("author-fa-3", 25)],
        "series": [("series-fa-1", 38), ("series-fa-2", 37)],
        "tags": [("ta-fa-1", 40), ("ta-fa-2", 35)],
        "genres": [("ge-fa-1", 15), ("ge-fa-2", 60)],
    }
    expected_filters = {}
    for group, entries in filter_input_groups.items():
        for value, count in entries:
            got = filter_rows(books, group, value)
            assert len(got) == count, (group, value, len(got), count)
            expected_filters[f"{group}.{value}"] = {
                "count": count, "first": got[0]["id"], "last": got[-1]["id"]}

    corpora = dataset["search_corpora"]
    expected_search = {}
    for lib_id, corpus in corpora.items():
        for key, ids in sorted(corpus["keys"].items()):
            expected_search[f"{lib_id}|q={key}"] = {"match_count": len(ids),
                                                    "first": ids[0] if ids else None}
    expected_search["limit_semantics"] = {
        "note": ("served = min(match_count, requested limit); a query that "
                 "requests limit=101 on q=overflow sees all 101 matches, "
                 "limit=40 shows the honest 40-result cap"),
        "overflow_at_limit_40": 40,
        "overflow_at_limit_100": 100,
        "overflow_at_limit_101": 101,
    }

    expected_seeds = {
        "mock-book": {"credential-a": 13.0, "credential-b": 17.5},
        "pg-bk-001": {"credential-a": 45.0, "credential-b": 404},
        "pod-41/pod41-ep-001": {"credential-a": 123.5, "credential-b": "finished"},
        "pod-big/podb-ep-010": {"credential-a": 5.0, "credential-b": 404},
    }

    return {
        "libraries": {
            "count": 18,
            "book_libraries": 10,
            "podcast_libraries": 8,
            "first_library": "mock-library",
            "note": "Array order is served order; the legacy first entry is preserved for §14.1.",
        },
        "pagination": expected_pagination,
        "sorts": {
            "inputs": sort_inputs + ["sequence"],
            "expected": expected_sorts,
        },
        "filters": {
            "groups": ["authors", "series", "tags", "genres"],
            "expected": expected_filters,
        },
        "search": {
            "result_count_variants": {"zero": 0, "fourty": 40, "hundred": 100,
                                      "overflow": 101},
            "expected": expected_search,
        },
        "episodes": {
            "pod-41": {"episode_count": 41,
                       "audio_track_variant": "pod41-ep-001",
                       "fallback_no_audio_track": "pod41-ep-002",
                       "fallback_null_content_url": "pod41-ep-003",
                       "unknown_metadata_variant": "pod41-ep-004",
                       "enclosure_variant": "pod41-ep-005"},
            "pod-big": {"episode_count": BIG_SHOW_EPISODES,
                        "min_expanded_bytes": BIG_SHOW_MIN_BYTES},
        },
        "chapters": {"bk-chapters-0": 0, "bk-chapters-1": 1, "bk-chapters-400": 400,
                     "bk-chapters-401": 401},
        "book_details_optional_fields": {"with_optional": "bk-full",
                                         "without_optional": "bk-minimal"},
        "progress_divergence": expected_seeds,
    }


# ---------------------------------------------------------------------------
# Writing
# ---------------------------------------------------------------------------

def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=1, sort_keys=False) + "\n",
                    encoding="utf-8")


def generate(alias, out_dir):
    libraries = build_libraries()
    dataset = build_dataset()
    details = build_details()
    dataset["expected"] = build_expected(dataset)

    # Regenerate from a clean directory so stale files can never linger next
    # to the provenance manifest.
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    files = []  # (relative path string, Path)

    def record(path):
        files.append((str(path.relative_to(out_dir)), path))

    write_json(out_dir / "scenario" / "libraries.json", libraries)
    record(out_dir / "scenario" / "libraries.json")
    write_json(out_dir / "scenario" / "dataset.json", dataset)
    record(out_dir / "scenario" / "dataset.json")
    for item_id in sorted(details):
        write_json(out_dir / "scenario" / "details" / f"{item_id}.expanded.json",
                   details[item_id])
        record(out_dir / "scenario" / "details" / f"{item_id}.expanded.json")
    cover_path = out_dir / "scenario" / "media" / "covers" / "fixture-cover.jpg"
    cover_path.parent.mkdir(parents=True, exist_ok=True)
    cover_path.write_bytes(base64.b64decode(COVER_JPEG_B64))
    record(cover_path)

    source = {
        "schema": SOURCE_SCHEMA,
        "alias": alias,
        "normative_abs_server": {
            "repo": "advplyr/audiobookshelf",
            "commit": NORMATIVE_COMMITS[alias]["commit"],
            "tag": NORMATIVE_COMMITS[alias]["tag"],
            "committed_date": NORMATIVE_COMMITS[alias]["committed_date"],
            "captured": "2026-11-22 via gh api git/trees + git/blobs",
        },
        "derived_source_paths": [
            {"path": path, "git_blob_sha1": BLOB_HASHES[alias][path]}
            for path in DERIVED_SOURCE_PATHS
        ],
        "scenario_summary": {
            "libraries": 18,
            "items": {lib: len(rows) for lib, rows in sorted(dataset["items"].items())},
            "search_result_variants": [0, 40, 100, 101],
            "chapter_variants": [0, 1, 400, 401],
            "forty_one_episode_show": "pod-41",
            "oversized_expanded_show": "pod-big",
            "oversized_min_bytes": BIG_SHOW_MIN_BYTES,
            "two_server_scenario": ("credential-a/credential-b share IDs; progress "
                                    "diverges per dataset.progress_seeds"),
        },
        "generator": {
            "script": "tools/abs_fixtures_generate.py",
            "deterministic": True,
            "note": ("No clock, RNG, or set iteration; payload content is "
                     "identical across aliases; only this source.json differs. "
                     "Sort/filter expectations are computed with "
                     "audiobookshelf_mock's own helpers to prevent drift."),
        },
        "files": [],
    }
    for rel, path in sorted(files):
        entry = {"path": rel, "sha256": sha256_file(path), "bytes": path.stat().st_size}
        source["files"].append(entry)
    write_json(out_dir / "source.json", source)


def check(root=FIXTURE_ROOT):
    failures = []
    for alias in sorted(NORMATIVE_COMMITS):
        alias_dir = root / alias
        source_path = alias_dir / "source.json"
        if not source_path.is_file():
            failures.append(f"{alias}: missing source.json")
            continue
        source = json.loads(source_path.read_text(encoding="utf-8"))
        if source.get("alias") != alias:
            failures.append(f"{alias}: source.json alias mismatch")
        if source.get("normative_abs_server", {}).get("commit") != \
                NORMATIVE_COMMITS[alias]["commit"]:
            failures.append(f"{alias}: commit mismatch in source.json")
        for entry in source.get("files", []):
            path = alias_dir / entry["path"]
            if not path.is_file():
                failures.append(f"{alias}/{entry['path']}: missing file")
                continue
            digest = sha256_file(path)
            if digest != entry["sha256"]:
                failures.append(f"{alias}/{entry['path']}: sha256 mismatch")
            if path.stat().st_size != entry["bytes"]:
                failures.append(f"{alias}/{entry['path']}: size mismatch")
        big = alias_dir / "scenario" / "details" / "pod-big.expanded.json"
        if big.is_file() and big.stat().st_size < BIG_SHOW_MIN_BYTES:
            failures.append(f"{alias}: pod-big.expanded.json below {BIG_SHOW_MIN_BYTES} bytes")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="verify checked-in fixture bytes and provenance only")
    parser.add_argument("--root", type=Path, default=FIXTURE_ROOT,
                        help="fixture root directory (default: tools/fixtures/audiobookshelf)")
    args = parser.parse_args()
    if args.check:
        failures = check(args.root)
        if failures:
            for failure in failures:
                print(f"FAIL {failure}", file=sys.stderr)
            return 1
        print("abs fixtures: all three aliases verified (provenance, hashes, sizes)")
        return 0
    for alias in sorted(NORMATIVE_COMMITS):
        out_dir = args.root / alias
        out_dir.mkdir(parents=True, exist_ok=True)
        generate(alias, out_dir)
        print(f"wrote fixture set {alias}")
    failures = check(args.root)
    if failures:
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
