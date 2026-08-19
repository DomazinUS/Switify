#!/usr/bin/env python3
"""Resolve Spotify Pathfinder persisted-query hashes from the web player.

This is a developer/build helper. The resulting operation/hash table is small
enough to compile into Switify, while keeping it refreshable whenever Spotify
publishes a new web-player bundle.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import gzip
import json
import re
import sys
from urllib.parse import urljoin
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


DEFAULT_OPERATIONS = (
    "profileAttributes",
    "searchDesktop",
    "libraryV3",
    "fetchLibraryTracks",
    "fetchPlaylist",
    "getAlbum",
    "queryHomeSection",
    "queryArtistOverview",
    "queryPodcastEpisodes",
    "userTopContent",
    "fetchEntitiesForRecentlyPlayed",
)
USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " \
             "(KHTML, like Gecko) Chrome/140 Safari/537.36"
MOBILE_USER_AGENT = "Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) " \
                    "AppleWebKit/605.1.15 Mobile/15E148 Safari/604.1"


def fetch(url: str, user_agent: str = USER_AGENT) -> str:
    request = Request(url, headers={"User-Agent": user_agent,
                                    "Accept-Encoding": "gzip"})
    with urlopen(request, timeout=25) as response:
        body = response.read()
        if response.headers.get("Content-Encoding", "").lower() == "gzip":
            body = gzip.decompress(body)
        return body.decode("utf-8", errors="replace")


def find_hashes(body: str, operations: set[str]) -> dict[str, str]:
    found: dict[str, str] = {}
    for operation in operations:
        escaped = re.escape(operation)
        patterns = (
            rf"{escaped}.{{0,400}}?sha256Hash\\?\"?:\\?\"([a-f0-9]{{64}})",
            rf'"{escaped}","(?:query|mutation)","([a-f0-9]{{64}})"',
        )
        for pattern in patterns:
            match = re.search(pattern, body, re.DOTALL)
            if match:
                found[operation] = match.group(1)
                break
    return found


def parse_map_literals(body: str) -> tuple[dict[int, str], dict[int, str]]:
    candidates: list[dict[int, str]] = []
    for raw in re.findall(r'\{(?:\d+:"[^"]+",?)+\}', body):
        quoted = re.sub(r"(\d+):", r'"\1":', raw)
        try:
            parsed = {int(key): value for key, value in json.loads(quoted).items()}
        except (TypeError, ValueError, json.JSONDecodeError):
            continue
        if parsed:
            candidates.append(parsed)

    def hash_score(item: dict[int, str]) -> float:
        return sum(bool(re.fullmatch(r"[a-f0-9]{6,12}", value))
                   for value in item.values()) / len(item)

    def name_score(item: dict[int, str]) -> float:
        return sum("-" in value or "/" in value
                   for value in item.values()) / len(item)

    hash_maps = sorted((item for item in candidates if hash_score(item) > 0.4),
                       key=hash_score, reverse=True)
    name_maps = sorted((item for item in candidates if name_score(item) > 0.4),
                       key=name_score, reverse=True)
    if not hash_maps or not name_maps:
        raise RuntimeError("web-player chunk maps were not found")
    return name_maps[0], hash_maps[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("operations", nargs="*", default=DEFAULT_OPERATIONS)
    args = parser.parse_args()
    needed = set(args.operations)

    homes = (fetch("https://open.spotify.com/"),
             fetch("https://open.spotify.com/", MOBILE_USER_AGENT))
    scripts = {
        urljoin("https://open.spotify.com/", src)
        for home in homes
        for src in re.findall(r'<script[^>]+src="([^"]+)"', home)
    }
    mains = sorted(
        url for url in scripts
        if re.search(r"/(?:mobile-)?web-player/(?:mobile-)?web-player\.[^.]+\.js$",
                     url)
    )
    if not mains:
        raise RuntimeError("web-player main bundle was not found")
    if args.verbose:
        print("Bundles:\n  " + "\n  ".join(mains), file=sys.stderr)

    found: dict[str, str] = {}
    for main_url in mains:
        if not needed:
            break
        main_body = fetch(main_url)
        result = find_hashes(main_body, needed)
        if args.verbose:
            locations = {operation: main_body.find(operation)
                         for operation in sorted(needed)}
            print(f"Scanned {main_url} ({len(main_body)} bytes): "
                  f"{sorted(result)}; locations={locations}", file=sys.stderr)
        found.update(result)
        needed.difference_update(result)
        if not needed:
            break

        try:
            name_map, hash_map = parse_map_literals(main_body)
        except RuntimeError:
            continue
        base = main_url.rsplit("/", 1)[0] + "/"
        chunks = [urljoin(base, f"{name_map[key]}.{hash_map[key]}.js")
                  for key in sorted(name_map) if key in hash_map]

        def inspect(url: str) -> dict[str, str]:
            try:
                return find_hashes(fetch(url), needed)
            except (HTTPError, URLError, TimeoutError):
                return {}

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            for result in executor.map(inspect, chunks):
                found.update(result)
                needed.difference_update(result)
                if not needed:
                    break

    print(json.dumps({key: found[key] for key in sorted(found)}, indent=2))
    if needed:
        print("Missing: " + ", ".join(sorted(needed)), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
