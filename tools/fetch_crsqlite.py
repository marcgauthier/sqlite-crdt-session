"""Download an official cr-sqlite release binary for the current OS and CPU.

Only the benchmark uses this third-party binary; it is NOT a dependency
of the sqlite-crdt-session C extension. Uses the GitHub releases API.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import platform
import shutil
import sys
import urllib.error
import urllib.request
import zipfile
from io import BytesIO
from pathlib import Path

DEFAULT_VERSION = 'v0.16.3'
BASE = 'https://api.github.com/repos/vlcn-io/cr-sqlite/releases/tags/'


def request(url: str) -> bytes:
    req = urllib.request.Request(url, headers={'User-Agent': 'sqlite-crdt-session-benchmark/1.0',
                                               'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(req, timeout=60) as response:
        return response.read()


def asset_match(name: str, system: str, machine: str) -> bool:
    lower = name.lower()
    if not lower.endswith('.zip'):
        return False
    os_tokens = {'linux': ('linux',), 'darwin': ('mac', 'darwin', 'osx'),
                 'windows': ('win', 'windows')}
    cpu_tokens = {'x86_64': ('x64', 'x86_64', 'amd64', 'x86-64'),
                  'aarch64': ('arm64', 'aarch64')}
    return any(token in lower for token in os_tokens.get(system, (system,))) and any(
        token in lower for token in cpu_tokens.get(machine, (machine,)))


def fetch(version: str, output: Path, asset_name: str | None = None) -> tuple[str, str]:
    release = json.loads(request(BASE + version))
    osname = platform.system().lower()
    arch = platform.machine().lower()
    if arch in ('amd64', 'x64'):
        arch = 'x86_64'
    if arch == 'arm64':
        arch = 'aarch64'
    assets = release.get('assets', [])
    matches = [a for a in assets if a['name'] == asset_name] if asset_name else [
        a for a in assets if asset_match(a['name'], osname, arch)]
    if len(matches) != 1:
        names = ', '.join(a['name'] for a in assets)
        raise RuntimeError(f'Expected one {osname}/{arch} asset, got {len(matches)}. '
                           f'Available: {names}. Use --asset-name if ambiguous.')
    asset = matches[0]
    archive = request(asset['browser_download_url'])
    expected_name = {'linux': 'crsqlite.so', 'darwin': 'crsqlite.dylib',
                     'windows': 'crsqlite.dll'}[osname]
    with zipfile.ZipFile(BytesIO(archive)) as z:
        candidates = [n for n in z.namelist() if Path(n).name == expected_name]
        if len(candidates) != 1:
            raise RuntimeError(f'Cannot locate {expected_name} in release ZIP: {z.namelist()}')
        content = z.read(candidates[0])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(content)
    return asset['name'], hashlib.sha256(content).hexdigest()


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--version', default=DEFAULT_VERSION)
    p.add_argument('--output', type=Path, default=Path('build/benchmark') / {
        'Linux': 'crsqlite.so', 'Darwin': 'crsqlite.dylib',
        'Windows': 'crsqlite.dll'}.get(platform.system(), 'crsqlite.so'))
    p.add_argument('--asset-name', help='Override GitHub release asset selection')
    args = p.parse_args()
    try:
        name, sha256 = fetch(args.version, args.output, args.asset_name)
    except (OSError, ValueError, RuntimeError, zipfile.BadZipFile, urllib.error.URLError) as exc:
        print(f'Could not download cr-sqlite: {exc}', file=sys.stderr)
        return 1
    print(f'{args.version} / {name} -> {args.output}\nSHA256: {sha256}')
    return 0


if __name__ == '__main__':
    sys.exit(main())