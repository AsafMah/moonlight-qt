#!/usr/bin/env python3
"""Resolve the repository used by the app's in-app and AppImage update channels."""

import argparse
import os
import re
import sys

DEFAULT_REPOSITORY = "qiin2333/moonlight-qt"
REPOSITORY_PATTERN = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")


def resolve_repository(environment):
    repository = DEFAULT_REPOSITORY
    if environment.get("GITHUB_ACTIONS") == "true":
        repository = environment.get("GITHUB_REPOSITORY") or DEFAULT_REPOSITORY

    if not REPOSITORY_PATTERN.fullmatch(repository):
        raise ValueError("GITHUB_REPOSITORY must be a GitHub owner/repository")
    return repository


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--appimage-arch", choices=("x86_64", "aarch64"))
    parser.add_argument("--appimage-tag", default="latest")
    args = parser.parse_args()

    try:
        repository = resolve_repository(os.environ)
    except ValueError as error:
        parser.error(str(error))

    if args.appimage_arch:
        owner, name = repository.split("/", 1)
        print(
            f"gh-releases-zsync|{owner}|{name}|{args.appimage_tag}|"
            f"Moonlight-VPlus-*-{args.appimage_arch}.AppImage.zsync"
        )
    else:
        print(repository)


if __name__ == "__main__":
    main()
