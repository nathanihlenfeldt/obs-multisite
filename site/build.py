#!/usr/bin/env python3
"""Build the OBS Multisite site into _site/.

The pages are hand-written. This script only fills in the handful of facts
that would otherwise go stale: the current release tag, its download links,
and the build date. Everything else about the project — the gaps, the
roadmap, the protocol — stays in the repository, where it belongs.

Run it with no arguments. It works offline, falling back to the version in
CMakeLists.txt and to the Releases page for downloads.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import sys
import urllib.error
import urllib.request
from datetime import date
from pathlib import Path

REPO = "stageaudioworks/obs-multisite"
HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
OUT = ROOT / "_site"

RELEASES = f"https://github.com/{REPO}/releases"

# Which release asset satisfies which placeholder.
PLATFORMS = {
    "WINDOWS": "windows-x64.zip",
    "MACOS": "macos-arm64.zip",
    "LINUX": "linux-x86_64.tar.gz",
}


def cmake_version() -> str:
    """The version the source tree claims, e.g. '0.1.13'."""
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"^project\([^)]*?VERSION\s+([0-9][0-9.]*)", text,
                  re.MULTILINE | re.DOTALL)
    if not m:
        sys.exit("build.py: no VERSION found in CMakeLists.txt project()")
    return m.group(1)


def latest_release() -> dict | None:
    """The most recent release, pre-releases included, or None if unreachable.

    GitHub's /releases/latest skips pre-releases, and every release so far is
    one — so take the first entry of the full list instead.
    """
    req = urllib.request.Request(
        f"https://api.github.com/repos/{REPO}/releases?per_page=1",
        headers={"Accept": "application/vnd.github+json",
                 "User-Agent": "obs-multisite-site-build"},
    )
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        req.add_header("Authorization", f"Bearer {token}")

    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            releases = json.load(r)
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError) as e:
        print(f"build.py: could not reach the releases API ({e}); "
              f"falling back to CMakeLists.txt", file=sys.stderr)
        return None

    return releases[0] if releases else None


def substitutions() -> dict[str, str]:
    version = cmake_version()
    rel = latest_release()

    if rel:
        tag = rel["tag_name"]
        release_url = rel["html_url"]
        assets = {a["name"]: a["browser_download_url"] for a in rel.get("assets", [])}
    else:
        tag = f"v{version}-alpha"
        release_url = RELEASES
        assets = {}

    subs = {
        "VERSION": version,
        "TAG": tag,
        "RELEASE_URL": release_url,
        "BUILD_DATE": date.today().isoformat(),
    }

    for key, suffix in PLATFORMS.items():
        name = next((n for n in assets if n.endswith(suffix)), None)
        if name:
            subs[f"DL_{key}"] = assets[name]
            subs[f"FN_{key}"] = name
        else:
            # No published asset for this platform in this release. Send the
            # reader to the releases page rather than to a link that 404s.
            if rel:
                print(f"build.py: {tag} has no *{suffix} asset; "
                      f"linking the releases page instead", file=sys.stderr)
            subs[f"DL_{key}"] = release_url
            subs[f"FN_{key}"] = f"the {suffix} build"

    return subs


def render(text: str, subs: dict[str, str]) -> str:
    out = re.sub(r"\{\{(\w+)\}\}",
                 lambda m: subs.get(m.group(1), m.group(0)), text)
    left = set(re.findall(r"\{\{(\w+)\}\}", out))
    if left:
        sys.exit(f"build.py: unsubstituted placeholders: {', '.join(sorted(left))}")
    return out


def main() -> None:
    subs = substitutions()

    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)

    for page in sorted(HERE.glob("*.html")):
        (OUT / page.name).write_text(
            render(page.read_text(encoding="utf-8"), subs), encoding="utf-8")
        print(f"  {page.name}")

    shutil.copytree(HERE / "assets", OUT / "assets")

    # Pages would otherwise run the output through Jekyll, which drops any
    # file or directory whose name begins with an underscore.
    (OUT / ".nojekyll").write_text("")

    cname = HERE / "CNAME"
    if cname.exists():
        shutil.copy2(cname, OUT / "CNAME")
        print(f"  CNAME ({cname.read_text().strip()})")

    print(f"built {OUT.relative_to(ROOT)} for {subs['TAG']} "
          f"({subs['BUILD_DATE']})")


if __name__ == "__main__":
    main()
