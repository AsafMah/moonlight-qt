#!/usr/bin/env python3
"""Publish only the verified packages from a successful integration CI run."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def packages(root: Path, version: str) -> list[Path]:
    if "dirty" in version.lower() or not re.fullmatch(r"\d+\.\d+\.\d+(?:\.\d+)?(?:[+-]\d+\.g[0-9a-f]+)?", version):
        raise ValueError("Refusing a dirty, prerelease or invalid artifact version")
    result = sorted(path for path in root.rglob("*") if path.is_file()
                    and path.name.startswith("Moonlight-VPlus-")
                    and path.name.endswith((".exe", ".zip", ".dmg", ".AppImage", ".AppImage.zsync")))
    expected = [
        f"Moonlight-VPlus-Portable-x64-{version}.zip",
        f"Moonlight-VPlus-Portable-arm64-{version}.zip",
        f"Moonlight-VPlus-Setup-{version}.exe",
    ]
    names = [path.name for path in result]
    if len(names) != len(set(names)) or not all(name in names for name in expected):
        raise ValueError("Missing or duplicate required Windows update assets")
    for path in result:
        if version not in path.name or path.stat().st_size == 0 or "dirty" in path.name.lower():
            raise ValueError(f"Invalid release asset: {path.name}")
    return result


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def gh(*arguments: str):
    result = subprocess.run(["gh", *arguments], text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    return result.stdout


def optional_release(repo: str, suffix: str):
    result = subprocess.run(["gh", "api", f"repos/{repo}/releases/{suffix}"],
                            capture_output=True, text=True)
    if result.returncode == 0:
        return json.loads(result.stdout)
    if "(HTTP 404)" not in result.stderr:
        raise RuntimeError(result.stderr.strip())
    return None


def numeric_tag(tag: str) -> tuple[int, ...]:
    if not re.fullmatch(r"v?\d+\.\d+\.\d+(?:\.\d+)?", tag):
        raise ValueError(f"Cannot compare release tag: {tag}")
    parts = tuple(int(part) for part in tag.removeprefix("v").split("."))
    return parts if len(parts) == 4 else parts + (0,)


def verify_assets(repo: str, tag: str, assets: list[Path]):
    release = json.loads(gh("api", f"repos/{repo}/releases/tags/{tag}"))
    uploaded = {asset["name"]: asset.get("digest", "") for asset in release["assets"]}
    expected = {path.name: "sha256:" + sha256(path) for path in assets}
    if uploaded != expected:
        raise RuntimeError("Release assets do not match this CI run; refusing publication or replacement")
    return release


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, required=True)
    args = parser.parse_args()
    if os.environ.get("GITHUB_ACTIONS") != "true" or os.environ.get("GITHUB_REF") != "refs/heads/integration":
        raise RuntimeError("Only integration GitHub Actions runs may publish")
    if os.environ.get("GITHUB_EVENT_NAME") not in ("push", "workflow_dispatch"):
        raise RuntimeError("This event may not publish a release")
    repo = os.environ["GITHUB_REPOSITORY"]
    head = os.environ["GITHUB_SHA"]
    run = os.environ["GITHUB_RUN_NUMBER"]
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repo) or not re.fullmatch(r"[0-9a-f]{40}", head) or not run.isdecimal():
        raise RuntimeError("Invalid CI identity")
    if subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], text=True).strip():
        raise RuntimeError("Refusing to publish a dirty source checkout")
    script = Path(__file__).with_name("derive-version.py")
    version = subprocess.check_output(["python3", str(script), "--field", "artifact"], text=True).strip()
    numeric = subprocess.check_output(["python3", str(script), "--field", "numeric"], text=True).strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+\.\d+", numeric):
        raise RuntimeError("Invalid numeric release version")
    assets = packages(args.artifacts, version)
    tag = "v" + numeric
    existing = optional_release(repo, f"tags/{tag}")
    if existing:
        release = verify_assets(repo, tag, assets)
        if not release["draft"]:
            reference = json.loads(gh("api", f"repos/{repo}/git/ref/tags/{tag}"))
            if reference["object"]["type"] != "commit" or reference["object"]["sha"] != head:
                raise RuntimeError("Existing release tag belongs to another commit")
            print(f"Verified existing release {release['html_url']}; no assets replaced")
            return
        if release["target_commitish"] != head:
            raise RuntimeError("Existing draft belongs to another commit")
    else:
        notes = ("Old r43 builds check upstream: manually upgrade once to this fork-aware build. "
                 "Future releases are checked automatically; download and installation require confirmation. "
                 "Finish streaming before installing or restarting.\n\n"
                 f"Verified integration build {head}.\n\nCI: {os.environ['GITHUB_SERVER_URL']}/{repo}/actions/runs/{os.environ['GITHUB_RUN_ID']}\n\n")
        notes += "\n".join(f"- `{path.name}` SHA256 `{sha256(path)}`" for path in assets)
        gh("release", "create", tag, *(str(path) for path in assets), "--repo", repo,
           "--target", head, "--title", f"Moonlight V+ r{run} ({numeric})",
           "--notes", notes, "--draft")
        verify_assets(repo, tag, assets)
    latest = optional_release(repo, "latest")
    latest_flag = "--latest" if not latest or numeric_tag(tag) >= numeric_tag(latest["tag_name"]) else "--latest=false"
    gh("release", "edit", tag, "--repo", repo, "--draft=false", "--prerelease=false", latest_flag)
    final = verify_assets(repo, tag, assets)
    print(final["html_url"])


if __name__ == "__main__":
    main()
