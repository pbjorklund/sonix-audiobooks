#!/usr/bin/env python3
"""Authenticated Audiobookshelf fixture server for the Sonix host test suites.

Serves the checked-in fixture sets from `tools/fixtures/audiobookshelf/<alias>/`
(Bob spec `bob-abs-features-spec.md` §14.6). The alias is selected with the
`SONIX_ABS_FIXTURE` environment variable (default `pinned-3563d49`, so the
pre-0.6.0 §14.1 commands retain their meaning); `--fixture` is an explicit
override.

Strict provenance rules (a violation fails the run, never a silent fallback):

* unknown alias, missing fixture directory, missing/incomplete `source.json`,
  or any file whose SHA-256/size does not match `source.json` → exit code 2;
* the alias/commit pair must match Bob's normative table;
* requests are logged to `<progress-file>.requests` with the selected alias,
  the full normative commit, the exact request path/query, the request origin,
  and a non-secret credential identifier (never the token itself).

Two-credential model (§14.6 two-server scenario): `--token` (credential-a)
and `--token-b` (credential-b) authenticate against the same URLs and see
identical IDs, catalogs, filterdata, corpora, and bytes; only per-item progress
diverges per the fixture seeds. Either credential may patch progress; each
credential's flat progress file mirrors the patch of its legacy primary item
(`mock-book`) and acts as the external override channel for that item, exactly
like the pre-0.6.0 single-profile mock. Other items keep progress in memory,
seeded from the fixture.

This file is also imported by `tools/abs_fixtures_generate.py`, which reuses
`stable_ino`, the audio-track/file builders, and the sort/filter helpers so the
recorded expected values and the serving behavior cannot drift.
"""

import argparse
import base64
import hashlib
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_FIXTURE_ROOT = REPO_ROOT / "tools" / "fixtures" / "audiobookshelf"
DEFAULT_FIXTURE = "pinned-3563d49"
SOURCE_SCHEMA = "sonix-abs-fixture-source-1"
SCENARIO_SCHEMA = "sonix-abs-fixture-scenario-1"

# Normative alias→commit table from §14.6 plus the Hermes-approved third
# fixture (v2.38.0). The mock refuses to serve a fixture whose source.json
# disagrees with this table.
NORMATIVE_COMMITS = {
    "pinned-3563d49": "3563d49424d50170324de9236a24c591b6d965e9",
    "current-5de038d": "5de038dafda294d13ca48861eda7d4405bbd7368",
    "eric-v2.38.0": "1430c9fc642d586ff91c8d4cbed20aab2c47e473",
}

# Deterministic synthetic audio served for fixture item files. These bytes are
# not valid MPEG; suites validate transport, paths, and byte stability, not
# decoding. Recorded so future changes cannot silently alter served bytes.
SYNTHETIC_AUDIO_BYTES = 16384
SYNTHETIC_AUDIO_PREAMBLE = "SONIX-SYNTH-AUDIO-1"


class FixtureError(Exception):
    """Fixture provenance or content failure that must fail the test run."""


def stable_ino(stem, ordinal):
    """Deterministic numeric file identity derived from strings."""
    digest = hashlib.sha256(f"{stem}:{ordinal}".encode("utf-8")).hexdigest()
    return str(int(digest[:12], 16))


def build_audio_track(item_id, ino, filename, duration, index=1, start_offset=0.0,
                      content_url=None):
    """ABS v2.38.0 AudioTrack.toJSON shape (`server/objects/files/AudioTrack.js`)."""
    return {
        "index": index,
        "startOffset": start_offset,
        "duration": duration,
        "title": filename.rsplit(".", 1)[0],
        "contentUrl": content_url if content_url is not None
                      else f"/api/items/{item_id}/file/{ino}",
        "mimeType": "audio/mpeg",
        "codec": None,
        "metadata": {
            "filename": filename,
            "size": int(duration) * 32000,
            "mimeType": "audio/mpeg",
        },
    }


def build_audio_file(item_id, ino, filename, duration):
    """ABS v2.38.0 AudioFile subset used by episode payloads."""
    return {
        "ino": ino,
        "mimeType": "audio/mpeg",
        "duration": duration,
        "metadata": {
            "filename": filename,
            "size": int(duration) * 32000,
            "mimeType": "audio/mpeg",
        },
        "addedAt": 1700000000000,
        "updatedAt": 1700000000000,
    }


def synthetic_audio_bytes(item_id, file_token):
    """Deterministic per-(item, file) bytes for `/api/items/{id}/file/{ino}`."""
    digest = hashlib.sha256(f"{item_id}:{file_token}".encode("utf-8")).digest()
    block = bytes(byte ^ digest[i % len(digest)] for i, byte in
                  enumerate(range(256)))
    header = (f"{SYNTHETIC_AUDIO_PREAMBLE}\nitem={item_id}\nfile={file_token}\n"
              f"sha256={hashlib.sha256(digest).hexdigest()}\n").encode("utf-8")
    body = block * ((SYNTHETIC_AUDIO_BYTES - len(header)) // len(block))
    out = header + body + block[: SYNTHETIC_AUDIO_BYTES - len(header) - len(body)]
    return out


# ---------------------------------------------------------------------------
# Dataset ordering/filter helpers (shared with abs_fixtures_generate.py).
# ---------------------------------------------------------------------------

def order_rows(rows, sort_key, desc):
    """Order dataset rows like the ABS items endpoint (`?sort=&desc=`).

    Deterministic tie-breaks: secondary key is lowercase title, final key is
    the row id. Unknown or missing sort keys fall back to title order.
    """
    def series_sequence(row):
        if not row["series"]:
            return 0.0
        try:
            return float(row["series"][0].get("sequence", 0))
        except (TypeError, ValueError):
            return 0.0

    keys = {
        "media.metadata.title": lambda row: row["title"].lower(),
        "media.metadata.author": lambda row: row["author_name"].lower(),
        "media.metadata.authorName": lambda row: row["author_name"].lower(),
        "addedAt": lambda row: row["added_at"],
        "media.duration": lambda row: row["duration"],
        "media.numTracks": lambda row: row["num_tracks"],
        "media.numEpisodes": lambda row: row["num_episodes"],
        "sequence": series_sequence,
    }
    key_fn = keys.get(sort_key or "", keys["media.metadata.title"])
    ordered = sorted(rows, key=lambda row: (key_fn(row), row["title"].lower(), row["id"]))
    return list(reversed(ordered)) if desc else ordered


def decode_filter(filter_param):
    """Decode `?filter=<group>.<base64(value)>` (ABS `libraryFilters.decode`).

    Returns (group, value), `None` for an absent/empty filter, or the sentinel
    `("invalid", None)` so undecodable filters fail closed to zero results.
    """
    if not filter_param:
        return None
    group, sep, value = filter_param.partition(".")
    if not sep or group not in ("authors", "series", "tags", "genres"):
        return ("invalid", None)
    padded = value + "=" * (-len(value) % 4)
    try:
        return (group, base64.b64decode(padded).decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        return ("invalid", None)


def filter_rows(rows, group, value):
    """Select dataset rows for a decoded filter group/value.

    §10.1 filter value semantics: author/series take the object id, genre/tag
    take the string itself (the NAME), so a genre/tag filter matches the rows
    whose genre/tag list carries that exact string -- a name-based filter
    selects real rows, never zero by id lookup.
    """
    if group == "authors":
        return [row for row in rows if value in row["author_ids"]]
    if group == "series":
        return [row for row in rows
                if any(entry["id"] == value for entry in row["series"])]
    if group == "tags":
        return [row for row in rows if value in row["tags"]]
    if group == "genres":
        return [row for row in rows if value in row["genres"]]
    return []


# ---------------------------------------------------------------------------
# Fixture loading with strict provenance verification.
# ---------------------------------------------------------------------------

class Fixture:
    def __init__(self, alias, fixture_dir):
        self.alias = alias
        self.fixture_dir = fixture_dir
        self.commit = NORMATIVE_COMMITS[alias]
        self._verify_provenance()
        self.libraries = self._load_json(Path("scenario") / "libraries.json")
        self.dataset = self._load_json(Path("scenario") / "dataset.json")
        if self.dataset.get("schema") != SCENARIO_SCHEMA:
            raise FixtureError(
                f"{alias}: dataset.json schema is {self.dataset.get('schema')!r}, "
                f"expected {SCENARIO_SCHEMA!r}")
        self.details = {}
        for item_id, rel_path in sorted(self.dataset["detail_index"].items()):
            self.details[item_id] = self._load_json(Path(rel_path))
        self.rows_by_library = self.dataset["items"]
        self._load()

    def _load_json(self, rel):
        path = self.fixture_dir / rel
        if not path.is_file():
            raise FixtureError(f"{self.alias}: missing fixture file {rel}")
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise FixtureError(f"{self.alias}: unreadable {rel}: {error}") from error

    def _verify_provenance(self):
        source_path = self.fixture_dir / "source.json"
        if not source_path.is_file():
            raise FixtureError(f"{self.alias}: missing source.json (provenance)")
        try:
            source = json.loads(source_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise FixtureError(f"{self.alias}: unreadable source.json: {error}") from error
        if source.get("schema") != SOURCE_SCHEMA:
            raise FixtureError(f"{self.alias}: unexpected source.json schema "
                               f"{source.get('schema')!r}")
        if source.get("alias") != self.alias:
            raise FixtureError(f"{self.alias}: source.json records alias "
                               f"{source.get('alias')!r}")
        commit = source.get("normative_abs_server", {}).get("commit")
        if commit != self.commit:
            raise FixtureError(f"{self.alias}: commit {commit!r} does not match the "
                               f"normative commit {self.commit}")
        derived = source.get("derived_source_paths")
        if not derived or not all(
                entry.get("path") and len(entry.get("git_blob_sha1", "")) == 40
                for entry in derived):
            raise FixtureError(f"{self.alias}: source.json is missing derived "
                               f"route/model source paths or blob hashes")
        files = source.get("files")
        if not files:
            raise FixtureError(f"{self.alias}: source.json lists no fixture files")
        self.verified_files = 0
        for entry in files:
            path = self.fixture_dir / entry["path"]
            if not path.is_file():
                raise FixtureError(f"{self.alias}: fixture file missing: "
                                   f"{entry['path']}")
            data = path.read_bytes()
            if len(data) != entry.get("bytes"):
                raise FixtureError(f"{self.alias}: size mismatch for {entry['path']}")
            if hashlib.sha256(data).hexdigest() != entry["sha256"]:
                raise FixtureError(f"{self.alias}: sha256 mismatch for {entry['path']}")
            self.verified_files += 1

    def _load(self):
        self.libraries_by_id = {lib["id"]: lib for lib in self.libraries["libraries"]}
        self.seeds = self.dataset.get("progress_seeds", {})

    def library(self, library_id):
        return self.libraries_by_id.get(library_id)

    def rows(self, library_id):
        return self.rows_by_library.get(library_id, [])

    def progress_seed(self, credential, key):
        entry = self.seeds.get(key, {})
        return entry.get(credential, {"__status__": 404})


def resolve_alias(cli_value):
    """SONIX_ABS_FIXTURE wins, then --fixture, then the §14.6 default."""
    env_value = (os.environ.get("SONIX_ABS_FIXTURE") or "").strip()
    alias = env_value or cli_value or DEFAULT_FIXTURE
    if alias not in NORMATIVE_COMMITS:
        known = ", ".join(sorted(NORMATIVE_COMMITS))
        raise FixtureError(f"unknown SONIX_ABS_FIXTURE alias {alias!r} "
                           f"(known aliases: {known})")
    return alias


def load_fixture(alias, fixture_root):
    fixture_dir = fixture_root / alias
    if not fixture_dir.is_dir():
        raise FixtureError(f"fixture directory not found: {fixture_dir}")
    return Fixture(alias, fixture_dir)


# ---------------------------------------------------------------------------
# Progress stores (two credentials, identical IDs, diverging progress).
# ---------------------------------------------------------------------------

LEGACY_PRIMARY_ITEM = "mock-book"


class CredentialProgress:
    """Per-credential progress state.

    The flat progress file mirrors the legacy single-book mock for the legacy
    primary item (`.requests`-adjacent tests depend on it) and is the external
    override channel for that item: when the file exists, its contents are
    served for the primary item even if the fixture seed differs. All other
    items keep progress in memory, initialized from the fixture seeds.
    """

    def __init__(self, credential_id, fixture, progress_file):
        self.credential_id = credential_id
        self.fixture = fixture
        self.progress_file = progress_file
        self.offline_file = progress_file.with_suffix(".offline")
        self._store = {}
        self._lock = threading.Lock()

    def seed(self, key):
        return self.fixture.progress_seed(self.credential_id, key)

    def get(self, key):
        with self._lock:
            if key == LEGACY_PRIMARY_ITEM and self.progress_file.is_file():
                return "value", json.loads(self.progress_file.read_text(encoding="utf-8"))
            if key in self._store:
                return "value", self._store[key]
            seed = self.seed(key)
            if "__status__" in seed:
                return "status", seed["__status__"]
            return "value", json.loads(json.dumps(seed))

    def patch(self, key, value):
        with self._lock:
            self._store[key] = json.loads(json.dumps(value))
            if key == LEGACY_PRIMARY_ITEM:
                self.progress_file.write_text(json.dumps(value), encoding="utf-8")
            return self._store[key]


# ---------------------------------------------------------------------------
# HTTP layer
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    fixture: Fixture
    media_dir: Path
    credentials: dict
    log_path: Path
    log_lock: threading.Lock
    throttle: dict  # file URL token -> per-16 KiB delay in ms (default empty)

    def log_message(self, fmt, *args):
        return

    # -- plumbing -----------------------------------------------------------

    def send_bytes(self, status, body, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, value, status=200):
        self.send_bytes(status, json.dumps(value).encode(), "application/json")

    def send_error_json(self, message, status):
        self.send_json({"error": message}, status)

    def origin(self):
        host = self.headers.get("Host") or self.headers.get("host")
        if host:
            return f"http://{host}"
        return f"http://127.0.0.1:{self.server.server_port}"

    def credential(self):
        """Resolve the non-secret credential identifier for this request.

        Returns (credential_id, CredentialProgress|None). Unknown tokens log as
        `credential-none` and are rejected before any route runs.
        """
        header = self.headers.get("Authorization") or ""
        for credential_id, progress in self.credentials.items():
            if header == f"Bearer {progress.token}":
                return credential_id, progress
        return "credential-none", None

    def log_request_line(self, credential_id):
        # Deliberately NOT named log_request: BaseHTTPRequestHandler.log_request
        # takes the HTTP status and is invoked by send_response(), which would
        # log every request twice under its response code.
        line = (f"{self.command} {self.path} | alias={self.fixture.alias} "
                f"commit={self.fixture.commit} | cred={credential_id} "
                f"| origin={self.origin()} | client={self.client_address[0]}\n")
        with self.log_lock:
            with self.log_path.open("a", encoding="utf-8") as log:
                log.write(line)

    # -- route helpers ------------------------------------------------------

    def query_params(self):
        return parse_qs(urlparse(self.path).query)

    def library_id(self):
        parts = urlparse(self.path).path.split("/")
        return parts[3] if len(parts) > 3 else ""

    # -- GET routes ---------------------------------------------------------

    def do_GET(self):
        credential_id, progress = self.credential()
        self.log_request_line(credential_id)
        if progress is None:
            self.send_error_json("unauthorized", 401)
            return
        path = urlparse(self.path).path
        if path == "/api/libraries":
            self.send_json(self.fixture.libraries)
            return
        if path.startswith("/api/libraries/") and path.endswith("/items"):
            self.serve_items(progress)
            return
        if path.startswith("/api/libraries/") and path.endswith("/filterdata"):
            self.serve_filterdata()
            return
        if "/search" in path and "/api/libraries/" in path:
            self.serve_search()
            return
        if path.startswith("/api/podcasts/") and "/episode/" in path:
            self.serve_episode_refresh()
            return
        if path.startswith("/api/items/") and path.endswith("/cover"):
            self.serve_cover()
            return
        if "/file/" in path and path.startswith("/api/items/"):
            self.serve_item_file()
            return
        if path.startswith("/api/items/"):
            self.serve_item_detail(progress)
            return
        if path.startswith("/api/me/progress/"):
            self.serve_progress_get(progress)
            return
        self.send_error_json("not found", 404)

    def int_param(self, params, name, default):
        try:
            return int(params.get(name, [default])[0] or default)
        except ValueError:
            self.send_error_json("invalid query parameter", 400)
            return None

    def serve_items(self, progress):
        library_id = self.library_id()
        library = self.fixture.library(library_id)
        if library is None:
            self.send_error_json("not found", 404)
            return
        params = self.query_params()
        rows = self.fixture.rows(library_id)

        limit = self.int_param(params, "limit", 0)
        page = self.int_param(params, "page", 0)
        if limit is None or page is None:
            return
        sort = params.get("sort", [None])[0]
        desc = params.get("desc", ["0"])[0] == "1"
        decoded = decode_filter(params.get("filter", [""])[0])
        if decoded is not None:
            group, value = decoded
            rows = filter_rows(rows, group, value) if group != "invalid" else []

        ordered = order_rows(rows, sort, desc)
        total = len(ordered)
        if limit:
            ordered = ordered[page * limit:(page + 1) * limit]

        results = []
        for row in ordered:
            wire = self.wire_row(row, progress)
            results.append(wire)
        envelope = {
            "results": results,
            "total": total,
            "limit": limit,
            "page": page,
            "sortBy": sort,
            "sortDesc": desc,
            "filterBy": params.get("filter", [None])[0] or None,
            "mediaType": library["mediaType"],
            "minified": params.get("minified", ["0"])[0] == "1",
            "collapseseries": params.get("collapseseries", ["0"])[0] == "1",
            "include": params.get("include", [""])[0] if "include" in params else "",
        }
        self.send_json(envelope)

    def wire_row(self, row, progress):
        media = {
            "coverPath": row["cover_path"],
            "metadata": {"title": row["title"], "authorName": row["author_name"]},
        }
        if row["kind"] == "book":
            media["duration"] = row["duration"]
            media["numTracks"] = row["num_tracks"]
        else:
            media["numEpisodes"] = row["num_episodes"]
            media["metadata"] = {"title": row["title"], "author": row["author_name"]}
        if row["series"]:
            if "authorName" in media["metadata"]:
                media["metadata"]["seriesName"] = row["series"][0]["name"]
        wire = {"id": row["id"], "media": media}
        seed = progress.seed(row["id"])
        if "__status__" not in seed:
            wire["userMediaProgress"] = {
                "currentTime": seed.get("currentTime", 0),
                "isFinished": seed.get("isFinished", False),
            }
        return wire

    def serve_filterdata(self):
        library_id = self.library_id()
        if self.fixture.library(library_id) is None:
            self.send_error_json("not found", 404)
            return
        filterdata = self.fixture.dataset["filterdata"].get(
            library_id, {"authors": [], "series": []})
        self.send_json(filterdata)

    def serve_search(self):
        parts = urlparse(self.path).path.split("/")
        library_id = parts[3] if len(parts) > 3 else ""
        if self.fixture.library(library_id) is None:
            self.send_error_json("not found", 404)
            return
        params = self.query_params()
        query = (params.get("q", [""])[0] or "").strip().lower()
        limit = self.int_param(params, "limit", 40)
        if limit is None:
            return
        corpus = self.fixture.dataset["search_corpora"].get(library_id, {})
        matches = self.search_matches(library_id, corpus, query)
        matches = matches[:limit]

        book_rows = []
        podcast_rows = []
        for row_id in matches:
            row = self.find_row(row_id)
            if row is None:
                continue
            if row["kind"] == "book":
                book_rows.append({"libraryItem": {
                    "id": row["id"],
                    "media": {"duration": row["duration"],
                              "metadata": {"title": row["title"],
                                           "authorName": row["author_name"]}},
                }})
            else:
                podcast_rows.append({"libraryItem": {
                    "id": row["id"],
                    "media": {"metadata": {"title": row["title"],
                                           "author": row["author_name"]},
                              "numEpisodes": row["num_episodes"]},
                }})
        if corpus.get("response") == "legacy-book-only":
            self.send_json({"book": book_rows})
            return
        self.send_json({"book": book_rows, "narrators": [], "authors": [],
                        "tags": [], "series": [], "podcast": podcast_rows})

    def search_matches(self, library_id, corpus, query):
        matches = []
        seen = set()

        def add(row_id):
            if row_id not in seen:
                seen.add(row_id)
                matches.append(row_id)

        for key, ids in corpus.get("keys", {}).items():
            if query == key:
                for row_id in ids:
                    add(row_id)
        if query:
            for row in self.fixture.rows(library_id):
                if row["title"].lower() == query:
                    add(row["id"])
        # §14.1 legacy rule for the pinned single-book library.
        if corpus.get("legacy_rule") == "legacy-substring":
            target = corpus.get("match_target", "the mock journey")
            if query in target:
                for row in self.fixture.rows(library_id):
                    add(row["id"])
        return matches

    def find_row(self, item_id):
        for rows in self.fixture.rows_by_library.values():
            for row in rows:
                if row["id"] == item_id:
                    return row
        return None

    def serve_item_detail(self, progress):
        item_id = urlparse(self.path).path.split("/")[3]
        detail = self.item_detail(item_id)
        if detail is None:
            self.send_error_json("not found", 404)
            return
        payload = json.loads(json.dumps(detail))
        seed = progress.seed(item_id)
        if "__status__" not in seed:
            payload["userMediaProgress"] = json.loads(json.dumps(seed))
        self.send_json(payload)

    def item_detail(self, item_id):
        if item_id in self.fixture.details:
            return self.fixture.details[item_id]
        row = self.find_row(item_id)
        if row is None:
            return None
        return self.synthesize_detail(row)

    def synthesize_detail(self, row):
        """Expand a plain dataset row deterministically (documented in the
        fixture `virtual_detail` note)."""
        metadata = {"title": row["title"], "authorName": row["author_name"]}
        media = {"coverPath": row["cover_path"], "metadata": metadata}
        if row["kind"] == "book":
            media["duration"] = row["duration"]
            media["numTracks"] = row["num_tracks"]
            tracks = []
            track_duration = row["duration"] / max(row["num_tracks"], 1)
            for k in range(1, row["num_tracks"] + 1):
                ino = stable_ino(row["id"], k)
                tracks.append(build_audio_track(
                    row["id"], ino, f"Part {k}.mp3", round(track_duration, 2),
                    index=k, start_offset=round((k - 1) * track_duration, 2)))
            media["tracks"] = tracks
            media["chapters"] = []
        else:
            media["episodes"] = self.synthesize_episodes(row)
        return {"id": row["id"], "media": media}

    def synthesize_episodes(self, row):
        episodes = []
        for ordinal in range(1, row["num_episodes"] + 1):
            unknown = ordinal % 11 == 0  # deterministic unknown-date coverage
            duration = 0.0 if unknown else 1800.0
            ino = stable_ino(row["id"], ordinal)
            episode = {
                "libraryItemId": row["id"],
                "podcastId": row["id"],
                "id": f"{row['id'].replace('-', '')}-ep-{ordinal:03d}",
                "oldEpisodeId": None,
                "index": ordinal,
                "season": None,
                "episode": None,
                "episodeType": "full",
                "title": f"{row['title']} — Episode {ordinal:03d}",
                "subtitle": f"Synthesized episode {ordinal:03d}",
                "description": f"Episode {ordinal:03d} of {row['title']}.",
                "enclosure": None,
                "guid": f"fixture-guid-{row['id']}-{ordinal:03d}",
                "pubDate": None if unknown else "Tue, 14 Nov 2017 01:00:00 GMT",
                "publishedAt": None if unknown else 1700000000000 + ordinal * 86400000,
                "addedAt": None if unknown else 1700000000000 + ordinal * 1000,
                "chapters": [],
                "audioFile": build_audio_file(row["id"], ino,
                                              f"Episode {ordinal:03d}.mp3", duration),
                "audioTrack": build_audio_track(row["id"], ino,
                                                f"Episode {ordinal:03d}.mp3", duration),
                "size": 0 if unknown else 1800 * 32000,
                "duration": duration,
            }
            episodes.append(episode)
        return episodes

    def serve_item_file(self):
        path = urlparse(self.path).path
        item_id = path.split("/")[3]
        file_token = path.rsplit("/file/", 1)[1]
        if item_id == LEGACY_PRIMARY_ITEM:
            # Legacy byte-compatibility: the smoke test supplies real media.
            legacy_names = {"1": "part1.mp3", "2": "part2.mp3"}
            name = legacy_names.get(file_token)
            if name is None:
                self.send_error_json("not found", 404)
                return
            media_file = self.media_dir / name
            if not media_file.is_file():
                self.send_error_json("not found", 404)
                return
            self.send_media_file(media_file, file_token)
            return
        if self.find_row(item_id) is None and item_id not in self.fixture.details:
            self.send_error_json("not found", 404)
            return
        self.send_media_file(None, file_token, synthetic_bytes=synthetic_audio_bytes(item_id, file_token))

    def send_media_file(self, media_file, file_token, synthetic_bytes=None):
        """serve_item_file's body: plain, or chunked+slept when the launch
        throttled this file's URL token (§8.5 line 595 slow responses)."""
        body = (synthetic_bytes if media_file is None
                else media_file.read_bytes())
        delay_ms = Handler.throttle.get(file_token, 0)
        if delay_ms <= 0:
            self.send_bytes(200, body, "audio/mpeg")
            return
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        chunk_size = 16 * 1024
        for start in range(0, len(body), chunk_size):
            self.wfile.write(body[start:start + chunk_size])
            self.wfile.flush()
            if start + chunk_size < len(body):
                time.sleep(delay_ms / 1000.0)

    def serve_cover(self):
        item_id = urlparse(self.path).path.split("/")[3]
        if item_id == LEGACY_PRIMARY_ITEM:
            media_file = self.media_dir / "cover.jpg"
            if not media_file.is_file():
                self.send_error_json("not found", 404)
                return
            self.send_bytes(200, media_file.read_bytes(), "image/jpeg")
            return
        if self.find_row(item_id) is None and item_id not in self.fixture.details:
            self.send_error_json("not found", 404)
            return
        cover = self.fixture.fixture_dir / "scenario" / "media" / "covers" / "fixture-cover.jpg"
        self.send_bytes(200, cover.read_bytes(), "image/jpeg")

    def serve_episode_refresh(self):
        """GET /api/podcasts/:id/episode/:episodeId (non-expanded shape).

        ABS v2.38.0 PodcastEpisode.toOldJSON: `audioFile`, no top-level
        audioTrack/size/duration.
        """
        parts = urlparse(self.path).path.split("/")
        if len(parts) < 6:
            self.send_error_json("not found", 404)
            return
        show_id, episode_id = parts[3], parts[5]
        detail = self.item_detail(show_id)
        episodes = (detail or {}).get("media", {}).get("episodes", [])
        for episode in episodes:
            if episode["id"] != episode_id:
                continue
            old_json = json.loads(json.dumps(episode))
            for key in ("audioTrack", "size", "duration"):
                old_json.pop(key, None)
            self.send_json(old_json)
            return
        self.send_error_json("not found", 404)

    def serve_progress_get(self, progress):
        parts = urlparse(self.path).path.split("/")
        item_id = parts[4] if len(parts) > 4 else ""
        episode_id = parts[5] if len(parts) > 5 else None
        key = f"{item_id}/{episode_id}" if episode_id else item_id
        if progress.offline_file.exists():
            self.send_error_json("offline test", 503)
            return
        state, value = progress.get(key)
        if state == "status":
            self.send_error_json("no remote progress", value)
            return
        if state == "missing":
            self.send_error_json("no remote progress", 404)
            return
        response = dict(value)
        response["libraryItemId"] = item_id
        if episode_id:
            response["episodeId"] = episode_id
        self.send_json(response)

    # -- PATCH --------------------------------------------------------------

    def do_PATCH(self):
        credential_id, progress = self.credential()
        self.log_request_line(credential_id)
        if progress is None:
            self.send_error_json("unauthorized", 401)
            return
        path = urlparse(self.path).path
        if not path.startswith("/api/me/progress/"):
            self.send_error_json("not found", 404)
            return
        parts = path.split("/")
        item_id = parts[4] if len(parts) > 4 else ""
        episode_id = parts[5] if len(parts) > 5 else None
        key = f"{item_id}/{episode_id}" if episode_id else item_id
        if not item_id:
            self.send_error_json("not found", 404)
            return
        if progress.offline_file.exists():
            self.send_error_json("offline test", 503)
            return
        size = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(size)
        try:
            value = json.loads(body or b"{}")
        except ValueError:
            self.send_error_json("invalid body", 400)
            return
        value.setdefault("lastUpdate", int(time.time() * 1000))
        progress.patch(key, value)
        self.send_json(value)


def log_startup(fixture, log_path, log_lock):
    """Record the selected alias, full commit, and verification in stdout and
    at the head of the request log. Tokens are never logged."""
    banner = [
        f"sonix-abs-fixture: serving alias={fixture.alias} commit={fixture.commit}",
        f"sonix-abs-fixture: fixture_dir={fixture.fixture_dir} "
        f"verified_files={fixture.verified_files}",
        "sonix-abs-fixture: credentials=credential-a,credential-b "
        "(non-secret identifiers; the mock never logs token values)",
    ]
    for line in banner:
        print(line, flush=True)
    with log_lock:
        with log_path.open("a", encoding="utf-8") as log:
            for line in banner:
                log.write(f"# {line}\n")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def validate_startup(fixture, progress_a, progress_b):
    if progress_a.token == progress_b.token:
        raise FixtureError("credential-a and credential-b tokens must differ "
                           "(identical tokens cannot model two servers)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--media-dir", type=Path, required=True)
    parser.add_argument("--port-file", type=Path, required=True)
    parser.add_argument("--progress-file", type=Path, required=True)
    parser.add_argument("--token", default="test-token")
    parser.add_argument("--token-b", default="test-token-b",
                        help="second credential token for the two-server scenario")
    parser.add_argument("--progress-b-file", type=Path, default=None,
                        help="credential-b progress file "
                             "(default: <progress-file stem>-b.json)")
    parser.add_argument("--fixture", default=None,
                        help="fixture alias (SONIX_ABS_FIXTURE wins when set)")
    parser.add_argument("--fixture-dir", type=Path, default=DEFAULT_FIXTURE_ROOT)
    parser.add_argument("--verify-only", action="store_true",
                        help="validate provenance and hashes, then exit")
    parser.add_argument("--throttle", action="append", default=[],
                        metavar="TOKEN:MS",
                        help="slow the media-file response whose URL token is "
                             "TOKEN by MS milliseconds per 16 KiB chunk "
                             "(repeatable), e.g. --throttle 2:1500 — lets a "
                             "test pause a transfer mid-file deterministically")
    args = parser.parse_args()

    try:
        alias = resolve_alias(args.fixture)
        fixture = load_fixture(alias, args.fixture_dir)
        progress_file_b = (args.progress_b_file or
                           args.progress_file.with_name(
                               f"{args.progress_file.stem}-b.json"))
        progress_a = CredentialProgress("credential-a", fixture, args.progress_file)
        progress_a.token = args.token
        progress_b = CredentialProgress("credential-b", fixture, progress_file_b)
        progress_b.token = args.token_b
        validate_startup(fixture, progress_a, progress_b)
    except FixtureError as error:
        print(f"sonix-abs-fixture: FAILED {error}", file=sys.stderr)
        return 2

    if args.verify_only:
        print(f"sonix-abs-fixture: verified alias={alias} commit={fixture.commit} "
              f"files={fixture.verified_files}")
        return 0

    Handler.fixture = fixture
    Handler.media_dir = args.media_dir
    Handler.credentials = {"credential-a": progress_a, "credential-b": progress_b}
    Handler.log_path = args.progress_file.with_suffix(".requests")
    Handler.log_lock = threading.Lock()
    Handler.throttle = {}
    for spec in args.throttle:
        token, sep, ms = spec.partition(":")
        if not sep or not ms.isdigit() or int(ms) <= 0:
            print(f"sonix-abs-mock: invalid --throttle {spec!r} (want TOKEN:MS)", file=sys.stderr)
            return 2
        Handler.throttle[token] = int(ms)
    log_startup(fixture, Handler.log_path, Handler.log_lock)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    args.port_file.write_text(str(server.server_port), encoding="ascii")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
