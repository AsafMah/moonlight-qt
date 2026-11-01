#!/usr/bin/env python3
"""Publish only the verified packages from a successful integration CI run."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import quote


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


def verify_assets(repo: str, release_id: int, assets: list[Path]):
    release = json.loads(gh("api", f"repos/{repo}/releases/{release_id}"))
    uploaded = {asset["name"]: asset.get("digest", "") for asset in release["assets"]}
    expected = {path.name: "sha256:" + sha256(path) for path in assets}
    if uploaded != expected:
        raise RuntimeError("Release assets do not match this CI run; refusing publication or replacement")
    return release


def find_draft(repo: str, tag: str, head: str):
    drafts = []
    page = 1
    while True:
        releases = json.loads(gh("api", f"repos/{repo}/releases?per_page=100&page={page}"))
        for release in releases:
            if release["tag_name"] == tag and release["draft"]:
                if release["target_commitish"] != head:
                    raise RuntimeError("Existing draft belongs to another commit")
                drafts.append(release)
        if len(releases) < 100:
            break
        page += 1
    if len(drafts) > 1:
        raise RuntimeError("Multiple matching drafts require manual reconciliation; refusing duplication")
    return drafts[0] if drafts else None


def upload_missing_assets(repo: str, release: dict, assets: list[Path]):
    expected = {path.name: "sha256:" + sha256(path) for path in assets}
    uploaded = {asset["name"]: asset.get("digest", "") for asset in release["assets"]}
    if any(name not in expected or digest != expected[name] for name, digest in uploaded.items()):
        raise RuntimeError("Existing draft has conflicting assets; refusing replacement")
    for path in assets:
        if path.name not in uploaded:
            gh("api", f"https://uploads.github.com/repos/{repo}/releases/{release['id']}/assets?name={quote(path.name)}",
               "--method", "POST",
               "-H", "Content-Type: application/octet-stream", "--input", str(path))


def workflow_tree(repo: str, ref: str):
    commit = json.loads(gh("api", f"repos/{repo}/commits/{quote(ref, safe='')}"))
    tree_id = commit["commit"]["tree"]["sha"]
    for name in (".github", "workflows"):
        tree = json.loads(gh("api", f"repos/{repo}/git/trees/{tree_id}"))
        if tree.get("truncated"):
            raise RuntimeError("Cannot verify workflow tree permissions: GitHub truncated the tree")
        entry = next((item for item in tree["tree"] if item["path"] == name), None)
        if entry is None:
            return None
        if entry["type"] != "tree":
            raise RuntimeError(f"Cannot verify workflow directory {name}")
        tree_id = entry["sha"]
    return tree_id


def preflight_token(repo: str, head: str):
    if os.environ.get("GH_RELEASE_TOKEN"):
        return
    default = json.loads(gh("api", f"repos/{repo}"))["default_branch"]
    if not re.fullmatch(r"[\w./-]+", default) or default.startswith("-"):
        raise RuntimeError("Invalid default branch from GitHub")
    if workflow_tree(repo, default) != workflow_tree(repo, head):
        raise RuntimeError(
            f"Release target changes workflows relative to default branch {default}. GITHUB_TOKEN cannot create "
            "this release tag: configure GH_RELEASE_TOKEN with Contents and Workflows write, "
            "or make integration the default branch before publishing.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--preflight", action="store_true")
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
    if subprocess.check_output(["git", "status", "--porcelain"], text=True).strip():
        raise RuntimeError("Refusing to publish a dirty source checkout")
    preflight_token(repo, head)
    if args.preflight:
        return
    if args.artifacts is None:
        parser.error("--artifacts is required for publication")
    script = Path(__file__).with_name("derive-version.py")
    version = subprocess.check_output(["python3", str(script), "--field", "artifact"], text=True).strip()
    numeric = subprocess.check_output(["python3", str(script), "--field", "numeric"], text=True).strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+\.\d+", numeric):
        raise RuntimeError("Invalid numeric release version")
    assets = packages(args.artifacts, version)
    tag = "v" + numeric
    existing = optional_release(repo, f"tags/{tag}")
    if existing and not existing["draft"]:
        release = verify_assets(repo, existing["id"], assets)
        reference = json.loads(gh("api", f"repos/{repo}/git/ref/tags/{tag}"))
        if reference["object"]["type"] != "commit" or reference["object"]["sha"] != head:
            raise RuntimeError("Existing release tag belongs to another commit")
        print(f"Verified existing release {release['html_url']}; no assets replaced")
        return
    latest = optional_release(repo, "latest")
    make_latest = not latest
    if latest:
        try:
            make_latest = numeric_tag(tag) >= numeric_tag(latest["tag_name"])
        except ValueError:
            # An uncomparable incumbent must not be displaced automatically.
            print(f"Warning: keeping nonnumeric latest release {latest['tag_name']}", file=sys.stderr)
    release = find_draft(repo, tag, head)
    if release is None:
        notes = ("Old r43 builds check upstream: manually upgrade once to this fork-aware build. "
                 "Future releases are checked automatically; download and installation require confirmation. "
                 "Finish streaming before installing or restarting.\n\n"
                 f"Verified integration build {head}.\n\nCI: {os.environ['GITHUB_SERVER_URL']}/{repo}/actions/runs/{os.environ['GITHUB_RUN_ID']}\n\n")
        notes += "\n".join(f"- `{path.name}` SHA256 `{sha256(path)}`" for path in assets)
        release = json.loads(gh("api", f"repos/{repo}/releases", "--method", "POST",
                                "-f", f"tag_name={tag}", "-f", f"target_commitish={head}",
                                "-f", f"name=Moonlight V+ r{run} ({numeric})", "-f", f"body={notes}",
                                "-F", "draft=true", "-F", "prerelease=false"))
    upload_missing_assets(repo, release, assets)
    verify_assets(repo, release["id"], assets)
    gh("api", f"repos/{repo}/releases/{release['id']}", "--method", "PATCH",
       "-F", "draft=false", "-F", "prerelease=false", "-f", f"make_latest={str(make_latest).lower()}")
    final = verify_assets(repo, release["id"], assets)
    print(final["html_url"])


if __name__ == "__main__":
    main()
