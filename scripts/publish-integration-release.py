#!/usr/bin/env python3
"""Publish only the verified packages from a successful integration CI run."""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import zipfile
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


def pe_file_version(data: bytes) -> str:
    import pefile

    with pefile.PE(data=data) as image:
        if not getattr(image, "VS_FIXEDFILEINFO", None):
            raise RuntimeError("Packaged executable has no FileVersion resource")
        info = image.VS_FIXEDFILEINFO[0]
        return ".".join(str(part) for part in (
            info.FileVersionMS >> 16, info.FileVersionMS & 0xFFFF,
            info.FileVersionLS >> 16, info.FileVersionLS & 0xFFFF,
        ))


def pe_display_version(data: bytes) -> str:
    import pefile

    with pefile.PE(data=data) as image:
        versions = []
        for entries in getattr(image, "FileInfo", []):
            for entry in entries:
                for table in getattr(entry, "StringTable", []):
                    comment = table.entries.get(b"Comments", b"").decode("utf-8").rstrip("\0")
                    if comment.startswith("VERSION_STR="):
                        versions.append(comment.removeprefix("VERSION_STR="))
        if not versions or len(set(versions)) != 1:
            raise RuntimeError("Packaged executable has missing or conflicting display version")
        return versions[0]


def verify_package_versions(assets: list[Path], numeric: str, tag: str) -> None:
    expected = numeric_tag(numeric)
    if numeric_tag(tag) != expected:
        raise RuntimeError(f"release tag {tag} does not match derived version {numeric}")
    for path in assets:
        name = re.fullmatch(
            r"Moonlight-VPlus-(?:Portable-(?:x64|arm64)-|Setup-|SteamLink-)?"
            r"(?P<version>\d+\.\d+\.\d+(?:\.\d+)?(?:[+-]\d+\.g[0-9a-f]+)?)"
            r"(?:-(?:x86_64|aarch64|arm64))?\.(?:zip|exe|dmg|AppImage(?:\.zsync)?)",
            path.name,
        )
        if name is None or name.group("version") != numeric:
            raise RuntimeError(f"asset {path.name} version does not match derived version {numeric}")
        if path.name.startswith("Moonlight-VPlus-Portable-"):
            with zipfile.ZipFile(path) as archive:
                entries = [item for item in archive.infolist()
                           if Path(item.filename).name.lower() == "moonlight.exe"]
                if len(entries) != 1:
                    raise RuntimeError(f"{path.name} must contain exactly one Moonlight.exe")
                data = archive.read(entries[0])
            actual = pe_file_version(data)
        elif path.name.startswith("Moonlight-VPlus-Setup-"):
            actual = pe_file_version(path.read_bytes())
        else:
            continue
        if actual != numeric:
            raise RuntimeError(f"{path.name} FileVersion {actual} != derived version {numeric}")
        if path.name.startswith("Moonlight-VPlus-Portable-"):
            display = pe_display_version(data)
            if display != numeric:
                raise RuntimeError(
                    f"{path.name} display version {display} != derived version {numeric}")


def verify_release_identity(release: dict, tag: str, head: str) -> None:
    if release["tag_name"] != tag or release["target_commitish"] != head:
        raise RuntimeError("Published release tag or target differs from the verified version")


def verify_previous_update(candidate: dict, previous: dict | None) -> None:
    if previous is None:
        raise RuntimeError("Previous published release is missing; updater replay cannot be verified")
    script = Path(__file__).with_name("verify-release-update.py")
    spec = importlib.util.spec_from_file_location("release_update_gate", script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.verify_update(candidate, previous["tag_name"])


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
    environment = dict(os.environ)
    environment.pop("CI_VERSION", None)
    version = subprocess.check_output([sys.executable, str(script), "--release", "--field", "artifact"],
                                      text=True, env=environment).strip()
    numeric = subprocess.check_output([sys.executable, str(script), "--release", "--field", "numeric"],
                                      text=True, env=environment).strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+\.\d+", numeric):
        raise RuntimeError("Invalid numeric release version")
    assets = packages(args.artifacts, version)
    tag = "v" + numeric
    verify_package_versions(assets, numeric, tag)
    existing = optional_release(repo, f"tags/{tag}")
    if existing and not existing["draft"]:
        release = verify_assets(repo, existing["id"], assets)
        verify_release_identity(release, tag, head)
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
                 "Clients without automatic installation, including v6.5.3.204, need one click to upgrade. "
                 "This version checks at startup and every six hours while idle, and installs verified updates "
                 "automatically by default after streaming ends, with a cancellable 15-second restart countdown. "
                 "Automatic installation can be disabled in About settings. Windows Setup upgrades still require "
                 "administrator approval.\n\n"
                 f"Verified integration build {head}.\n\nCI: {os.environ['GITHUB_SERVER_URL']}/{repo}/actions/runs/{os.environ['GITHUB_RUN_ID']}\n\n")
        notes += "\n".join(f"- `{path.name}` SHA256 `{sha256(path)}`" for path in assets)
        release = json.loads(gh("api", f"repos/{repo}/releases", "--method", "POST",
                                "-f", f"tag_name={tag}", "-f", f"target_commitish={head}",
                                "-f", f"name=Moonlight V+ r{run} ({numeric})", "-f", f"body={notes}",
                                "-F", "draft=true", "-F", "prerelease=false"))
    upload_missing_assets(repo, release, assets)
    candidate = verify_assets(repo, release["id"], assets)
    verify_release_identity(candidate, tag, head)
    verify_previous_update(candidate, latest)
    gh("api", f"repos/{repo}/releases/{release['id']}", "--method", "PATCH",
       "-F", "draft=false", "-F", "prerelease=false", "-f", f"make_latest={str(make_latest).lower()}")
    final = verify_assets(repo, release["id"], assets)
    verify_release_identity(final, tag, head)
    print(final["html_url"])


if __name__ == "__main__":
    main()
