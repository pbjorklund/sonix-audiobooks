#!/usr/bin/env python3
"""Exercise episode routes against disposable, loopback-only official ABS images."""
import hashlib
import json
import os
import secrets
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path

IMAGES = {
    '2.35.1': 'sha256:54a0f832278d5fbc10a038cfb9da26f227dfc6fa054f645699dbfa0ff0b97502',
    '2.38.0': 'sha256:9bea16b9d1808a117b8a0420182d63961a1792ca4bf07c75114e42106a17a495',
}


def docker(*args):
    return subprocess.check_output(['docker', *args], text=True).strip()


def check(version, digest):
    image = 'ghcr.io/advplyr/audiobookshelf@' + digest
    meta = json.loads(docker('image', 'inspect', image))[0]
    assert meta['Config']['Labels']['org.opencontainers.image.version'] == version
    name = 'sonix-pod-contract-' + uuid.uuid4().hex[:12]
    with tempfile.TemporaryDirectory(prefix='sonix-pod-contract-') as path:
        root = Path(path)
        folders = ['config', 'metadata', 'podcasts']
        for directory in folders:
            (root / directory).mkdir()
        show_path = root / 'podcasts' / 'Synthetic show'
        show_path.mkdir()
        for title in ('Episode A', 'Episode B'):
            subprocess.run(['ffmpeg', '-nostdin', '-loglevel', 'error', '-f', 'lavfi', '-i',
                            'sine=frequency=440:duration=30', '-b:a', '64k', '-metadata',
                            'title=' + title, str(show_path / (title + '.mp3'))], check=True)
        try:
            args = ['run', '-d', '--name', name, '--user', f'{os.getuid()}:{os.getgid()}',
                    '--cap-drop=ALL', '--security-opt=no-new-privileges', '--publish', '127.0.0.1::80']
            for folder in folders:
                args += ['-v', f'{root / folder}:/{folder}']
            docker(*args, image)
            base = 'http://' + docker('port', name, '80/tcp').splitlines()[0]
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            token = None

            def request(method, route, body=None, *, accepted=(200,), extra=None):
                headers = dict(extra or {})
                if token:
                    headers['Authorization'] = 'Bearer ' + token
                if body is not None:
                    headers['Content-Type'] = 'application/json'
                req = urllib.request.Request(base + route, method=method, headers=headers,
                                             data=None if body is None else json.dumps(body).encode())
                try:
                    response = opener.open(req, timeout=8)
                except urllib.error.HTTPError as error:
                    response = error
                with response:
                    assert response.code in accepted, (version, method, route, response.code)
                    if response.headers.get_content_type() == 'application/json':
                        return json.load(response)
                    return response.read()

            deadline = time.monotonic() + 45
            while True:
                try:
                    status = request('GET', '/status')
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(.2)
            assert status['serverVersion'] == version
            credentials = {'username': 'synthetic-root', 'password': secrets.token_urlsafe(32)}
            request('POST', '/init', {'newRoot': credentials})
            user = request('POST', '/login', credentials, extra={'x-return-tokens': 'true'})['user']
            token = user['accessToken']
            assert request('GET', '/api/me')['id'] == user['id']
            media = request('POST', '/api/libraries', {'name': 'Synthetic podcasts', 'mediaType': 'podcast',
                                                      'folders': [{'fullPath': '/podcasts'}]})
            lib = '/api/libraries/' + media['id']
            request('POST', lib + '/scan', {}, accepted=(200, 204))
            deadline = time.monotonic() + 45
            while True:
                listing = request('GET', lib + '/items?limit=40&page=0&minified=1')
                if listing['total'] == 1 and listing['results'][0]['media']['numEpisodes'] == 2:
                    break
                assert time.monotonic() < deadline, 'episode scan timed out'
                time.sleep(.2)
            item = listing['results'][0]
            assert item['mediaType'] == 'podcast'
            show_id = item['id']
            show = request('GET', '/api/items/' + show_id + '?expanded=1')
            a, b = sorted(show['media']['episodes'], key=lambda ep: ep['title'])
            assert a['title'] == 'Episode A' and b['title'] == 'Episode B'
            ep = request('GET', f"/api/podcasts/{show_id}/episode/{a['id']}")
            assert ep['libraryItemId'] == show_id and ep['id'] == a['id']
            assert 'audioTrack' not in ep and 'duration' not in ep
            route = f"/api/items/{show_id}/file/{ep['audioFile']['ino']}"
            assert a['audioTrack']['contentUrl'] == route
            expected = hashlib.sha256((show_path / 'Episode A.mp3').read_bytes()).digest()
            assert hashlib.sha256(request('GET', route)).digest() == expected
            assert hashlib.sha256(request('GET', route + '/download')).digest() == expected
            latest = request('GET', lib + '/recent-episodes?limit=40&page=0')['episodes']
            assert {row['id'] for row in latest} == {a['id'], b['id']}
            progress = f"/api/me/progress/{show_id}/{a['id']}"
            request('PATCH', progress, {'currentTime': 11.5, 'duration': 30, 'isFinished': False}, accepted=(200, 204))
            saved = request('GET', progress)
            assert saved['currentTime'] == 11.5 and saved['episodeId'] == a['id']
            session = {'id': str(uuid.uuid4()), 'libraryItemId': show_id, 'episodeId': a['id'], 'mediaType': 'podcast',
                       'playMethod': 3, 'startedAt': saved['lastUpdate'] - 12000, 'updatedAt': saved['lastUpdate'] - 1,
                       'timeListening': 3.5, 'currentTime': 9, 'duration': 30, 'mediaPlayer': 'Sonix contract test'}
            sessions_route = f"/api/me/item/listening-sessions/{show_id}/{a['id']}"
            before = request('GET', '/api/me/listening-stats')
            for _ in range(2):
                result = request('POST', '/api/session/local-all', {'sessions': [session]})['results']
                assert len(result) == 1 and result[0]['id'] == session['id'] and result[0]['success']
            stored = request('GET', sessions_route)
            assert stored['total'] == 1 and float(stored['sessions'][0]['timeListening']) == 3.5, stored
            after = request('GET', '/api/me/listening-stats')
            assert after['totalTime'] - before['totalTime'] == 3.5, after
            assert sum(after['days'].values()) - sum(before['days'].values()) == 3.5, after
            assert request('GET', progress)['currentTime'] == 11.5
            request('GET', f"/api/me/progress/{show_id}/{b['id']}", accepted=(404,))
            request('PATCH', progress, {'currentTime': 29, 'duration': 30, 'isFinished': False,
                                       'markAsFinishedTimeRemaining': 0}, accepted=(200, 204))
            assert not request('GET', progress)['isFinished']
            request('PATCH', progress, {'currentTime': 30, 'duration': 30, 'isFinished': True}, accepted=(200, 204))
            assert request('GET', progress)['isFinished']
            assert a['id'] not in {ep['id'] for ep in request('GET', lib + '/recent-episodes?limit=40&page=0')['episodes']}
            request('PATCH', progress, {'currentTime': 0, 'duration': 30, 'isFinished': False}, accepted=(200, 204))
            reset = request('GET', progress)
            assert not reset['isFinished'] and reset['currentTime'] == 0
            print(f'ABS {version}: show, episode, stream/download, progress and retry contracts passed', flush=True)
        finally:
            subprocess.run(['docker', 'rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


if __name__ == '__main__':
    for version, digest in IMAGES.items():
        check(version, digest)
