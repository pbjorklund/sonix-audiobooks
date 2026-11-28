#!/usr/bin/env python3
"""Loopback fixtures for Sonix favourites and download-status tests."""
import base64
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

work = Path(sys.argv[1])
album = {"id": 21, "title": "Fixture Album", "numberOfTracks": 2, "artist": {"name": "Fixture Artist"}}
tracks = [dict(id=i, title=f"Fixture Track {i}", trackNumber=i - 20, album=album) for i in (21, 22)]


saved_path = "/userCollectionSaveForLaters/me/relationships/items"
media_type = "application/vnd.api+json"
saved = {"21"}


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        self.change_saved(True)

    def do_DELETE(self):
        self.change_saved(False)

    def change_saved(self, add):
        if self.path != saved_path or self.headers.get("Content-Type") != media_type or self.headers.get("Accept") != media_type:
            self.send_error(400)
            return
        payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if payload != {"data": [{"id": "21", "type": "albums"}]}:
            self.send_error(422)
            return
        if add:
            saved.add("21")
        else:
            saved.discard("21")
        self.send_json({"data": []})

    def do_GET(self):
        request = urlsplit(self.path)
        path = request.path
        if path == saved_path:
            if not request.query.startswith("include=items,items.albums:artists,items.albums:coverArt"):
                self.send_error(400)
            elif self.headers.get_all("Accept") != [media_type]:
                self.send_error(406)
            elif (work / "reject-saved").exists():
                self.send_json({"errors": [{"status": "403"}]}, status=403)
            elif "page%5Bcursor%5D=album%2Bpage" in self.path:
                self.send_json({"data": [{"id": i, "type": "albums"} for i in saved],
                                "links": {"self": path}, "included": [{"id": "21", "type": "albums",
                                    "attributes": {"title": "Account Saved Album", "numberOfItems": 2},
                                    "relationships": {"artists": {"data": [{"id": "12", "type": "artists"}]}}},
                                    {"id": "12", "type": "artists", "attributes": {"name": "Saved Artist"}}]})
            else:
                self.send_json({"data": [{"id": "99", "type": "tracks"}],
                                "links": {"self": path, "next": path + "?page[cursor]=album%2Bpage"}})
        elif path in ("/users/1/favorites/tracks", "/albums/21/tracks"):
            items = tracks
            if path == "/albums/21/tracks" and (work / "large-album").exists():
                items = [dict(tracks[0], id=i + 21, title=f"Fixture Track {i + 21}", trackNumber=i + 1)
                         for i in range(260)]
            query = parse_qs(request.query)
            offset, limit = int(query.get("offset", [0])[0]), int(query.get("limit", [100])[0])
            self.send_json({"items": items[offset:offset + limit], "totalNumberOfItems": len(items)})
        elif path == "/users/1/favorites/albums":
            if (work / "hold-favourites").exists():
                (work / "favourites-refresh-started").touch()
                deadline = time.monotonic() + 12
                while (work / "hold-favourites").exists() and time.monotonic() < deadline:
                    time.sleep(.05)
            if (work / "reject-favourites").exists():
                self.send_json({"userMessage": "Fixture offline"}, status=503)
            elif (work / "bulk-albums").exists():
                self.send_json({"items": [album, dict(album, id=31, title="Second Album", numberOfTracks=1),
                                          dict(album, id=41, title="Missing Album")]})
            else:
                current = dict(album, title="Updated Fixture" if (work / "updated-favourites").exists() else album["title"])
                if (work / "fixture-cover").exists():
                    current["cover"] = "fixture-cover-id"
                self.send_json({"items": [current], "totalNumberOfItems": 1})
        elif path == "/albums/31/tracks" and (work / "bulk-albums").exists():
            self.send_json({"items": [dict(tracks[0], id=31, title="Fixture Track 31",
                                           album=dict(album, id=31, title="Second Album"))]})
        elif path == "/albums/21":
            self.send_json(album)
        elif path in ("/tracks/21/playbackinfopostpaywall", "/tracks/22/playbackinfopostpaywall"):
            track = path.split("/")[2]
            if track == "21" and (work / "hold-playback").exists():
                (work / "playback-requested").touch()
                deadline = time.monotonic() + 12
                while (work / "hold-playback").exists() and time.monotonic() < deadline:
                    time.sleep(.05)
                if (work / "reject-playback").exists():
                    self.send_json({"userMessage": "Fixture playback rejected"}, status=503)
                    return
            aac = (work / "fixture.m4a").is_file() and "audioquality=LOW" in request.query
            mime = "audio/mp4" if aac else "audio/flac"
            suffix = "?aac=1" if aac else ""
            manifest = json.dumps({"mimeType": mime, "urls": [f"http://127.0.0.1:{self.server.server_port}/audio/{track}{suffix}"]})
            self.send_json({"manifest": base64.b64encode(manifest.encode()).decode()})
        elif path in ("/audio/21", "/audio/22"):
            track = path.rsplit("/", 1)[1]
            aac = request.query == "aac=1"
            data = (work / ("fixture.m4a" if aac else "fixture.flac")).read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "audio/mp4" if aac else "audio/flac")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data[:1024])
            (work / f"active-{track}").touch()
            deadline = time.monotonic() + 20
            while not (work / f"release-{track}").exists() and time.monotonic() < deadline:
                time.sleep(.05)
            self.wfile.write(data[1024:])
        else:
            self.send_error(404)

    def send_json(self, payload, status=200):
        data = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
    (work / "port").write_text(str(server.server_port))
    server.serve_forever()
