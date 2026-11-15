#!/usr/bin/env python3

import hashlib
import json
import os
import shutil
import sys
import tarfile
import urllib.request
from pathlib import Path

REPO = "devkitpro/devkita64"
LAYERS = [
    ("sha256:74701f26999d7854982cfa51dafd83beeaf644031b943c4cbd0a245139a742a4", 265076006),
    ("sha256:ea5d3216df1fb580c54c016ee13ff58eb965c0a0b0ef08afd8dfd5a1c03e95a8", 28381973),
    ("sha256:da5be4661c9ac4e389f9e5f939e58bff7d32bc60569fa7c14e849d5246881691", 382982909),
]


def download(digest, size, token, dest):
    if dest.exists() and dest.stat().st_size == size and sha256(dest) == digest:
        return
    req = urllib.request.Request(f"https://registry-1.docker.io/v2/{REPO}/blobs/{digest}",
                                 headers={"Authorization": "Bearer " + token})
    part = dest.with_name(dest.name + ".part")
    with urllib.request.urlopen(req, timeout=120) as src, part.open("wb") as out:
        shutil.copyfileobj(src, out, 1 << 20)
    if part.stat().st_size != size or sha256(part) != digest:
        part.unlink()
        raise SystemExit("fetch_devkitpro: " + digest[7:19] + " does not match its pinned digest")
    part.rename(dest)


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return "sha256:" + h.hexdigest()


def extract(archive, out):
    prefix = "opt/devkitpro/"
    with tarfile.open(archive, "r:gz") as tf:
        members = []
        for m in tf:
            name = m.name.lstrip("./")
            if not name.startswith(prefix) or Path(name).name.startswith(".wh."):
                continue
            if m.issym() and not os.path.normpath(os.path.join(os.path.dirname(name), m.linkname)).startswith(prefix):
                continue
            m.name = name[len(prefix):]
            if m.islnk():
                m.linkname = m.linkname.lstrip("./")[len(prefix):]
            members.append(m)
        try:
            tf.extractall(out, members=members, filter="data")
        except TypeError:
            tf.extractall(out, members=members)


def main():
    out = Path(sys.argv[1])
    if (out / "cmake" / "Switch.cmake").exists():
        return 0
    work = out.with_name(out.name + ".layers")
    work.mkdir(parents=True, exist_ok=True)
    token = json.load(urllib.request.urlopen(
        f"https://auth.docker.io/token?service=registry.docker.io&scope=repository:{REPO}:pull", timeout=60))["token"]
    archives = []
    for digest, size in LAYERS:
        dest = work / (digest[7:] + ".tar.gz")
        download(digest, size, token, dest)
        archives.append(dest)
    staging = out.with_name(out.name + ".part")
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    for archive in archives:
        extract(archive, staging)
    staging.rename(out)
    shutil.rmtree(work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
