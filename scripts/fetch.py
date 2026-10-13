#!/usr/bin/env python3
"""Download everything the build needs into one cache directory.

    python3 scripts/fetch.py                 download all files of scripts/sources.txt that are missing
    python3 scripts/fetch.py --skip rust     leave out files whose name contains "rust" (repeatable)
    X45_SKIP="spotify bluetooth" python3 scripts/fetch.py   leave out everything only those optional parts need
    python3 scripts/fetch.py --dir DIR       use DIR as the cache (default: $X45_DL, else .build/dl)

Every file is checked against the SHA256 in scripts/sources.txt. A file that is already in the cache with the right
sum is not downloaded again, so the script can be run as often as you like. After the Spotify sources are there, the
Rust crates they need are downloaded too (into DIR/cargo-vendor, each one checked against Cargo.lock), so the
build itself can run without network access.

Only the Python standard library is used, so it works the same on Linux, macOS and Windows.
"""
import hashlib
import io
import json
import os
import re
import sys
import tarfile
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
UA = "x45-firmware-fetch/1.0"

# optional parts (names as in X45_SKIP) and the downloads that only they need
GROUPS = {
    "airplay": ["shairport", "libconfig", "popt", "alac"],
    "spotify": ["librespot", "rust"],
    "upnp": ["upmpdcli", "libupnpp", "libnpupnp", "libmicrohttpd", "jsoncpp", "libmpdclient"],
    "wifi": ["libnl", "iw-", "wpa_supplicant"],
    "bluetooth": ["btstack", "libusb", "aic-"],
}


def default_dir():
    if os.environ.get("X45_DL"):
        return os.environ["X45_DL"]
    build = os.environ.get("X45_BUILD")
    if not build:
        if ROOT.replace("\\", "/").startswith("/mnt/"):
            build = os.path.join(os.path.expanduser("~"), "x45-build")
        else:
            build = os.path.join(ROOT, ".build")
    return os.path.join(build, "dl")


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_sources(path):
    items = []
    with open(path, encoding="ascii") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) != 3 or not re.fullmatch(r"[0-9a-f]{64}", parts[0]):
                sys.exit("bad line in sources.txt: %s" % line)
            items.append({"sha": parts[0], "name": parts[1], "url": parts[2]})
    return items


def download(url, dst, tries=4):
    last = None
    for attempt in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=120) as r, open(dst, "wb") as o:
                while True:
                    chunk = r.read(1 << 20)
                    if not chunk:
                        break
                    o.write(chunk)
            return None
        except Exception as e:  # network errors of every kind: try again
            last = e
            time.sleep(2 + 3 * attempt)
    return str(last)


def fetch_one(item, cache):
    dst = os.path.join(cache, item["name"])
    if os.path.isfile(dst) and sha256_of(dst) == item["sha"]:
        return item["name"], "have", ""
    part = dst + ".part"
    err = download(item["url"], part)
    if err:
        if os.path.exists(part):
            os.remove(part)
        return item["name"], "FAILED", err
    got = sha256_of(part)
    if got != item["sha"]:
        if os.environ.get("X45_ACCEPT_HASH_MISMATCH") == "1":
            os.replace(part, dst)
            return item["name"], "changed upstream (accepted)", got
        os.remove(part)
        return item["name"], "FAILED", "checksum differs: expected %s, got %s" % (item["sha"], got)
    os.replace(part, dst)
    return item["name"], "ok", "%d bytes" % os.path.getsize(dst)


def vendor_crates(cache):
    """Download the registry crates named by librespot's Cargo.lock into cache/cargo-vendor."""
    tb = os.path.join(cache, "librespot-0.8.0.tar.gz")
    if not os.path.isfile(tb):
        return 0
    with tarfile.open(tb) as t:
        lock = t.extractfile("librespot-0.8.0/Cargo.lock").read().decode("utf-8")
    out = os.path.join(cache, "cargo-vendor")
    os.makedirs(out, exist_ok=True)
    pk = []
    for blk in lock.split("[[package]]")[1:]:
        d = dict(re.findall(r'^(\w+) = "([^"]*)"', blk, re.M))
        if d.get("source", "").startswith("registry+"):
            pk.append(d)

    def one(d):
        name, ver, ck = d["name"], d["version"], d["checksum"]
        dst = os.path.join(out, "%s-%s" % (name, ver))
        if os.path.exists(os.path.join(dst, ".cargo-checksum.json")):
            return None
        part = os.path.join(out, "%s-%s.crate.part" % (name, ver))
        err = download("https://static.crates.io/crates/%s/%s-%s.crate" % (name, name, ver), part)
        if err:
            return "%s-%s: %s" % (name, ver, err)
        with open(part, "rb") as f:
            data = f.read()
        os.remove(part)
        if hashlib.sha256(data).hexdigest() != ck:
            return "%s-%s: checksum differs from Cargo.lock" % (name, ver)
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as t:
            t.extractall(out)
        with open(os.path.join(dst, ".cargo-checksum.json"), "w") as f:
            json.dump({"files": {}, "package": ck}, f)
        return None

    with ThreadPoolExecutor(8) as ex:
        errs = [e for e in ex.map(one, pk) if e]
    print("rust crates: %d, problems: %d" % (len(pk), len(errs)))
    for e in errs:
        print("  " + e)
    return len(errs)


def main(argv):
    cache = default_dir()
    skip = []
    i = 1
    while i < len(argv):
        if argv[i] == "--dir":
            cache = argv[i + 1]
            i += 2
        elif argv[i] == "--skip":
            skip.append(argv[i + 1])
            i += 2
        else:
            print(__doc__)
            return 2
    for part in os.environ.get("X45_SKIP", "").split():
        skip.extend(GROUPS.get(part, []))
    os.makedirs(cache, exist_ok=True)
    items = [it for it in read_sources(os.path.join(HERE, "sources.txt")) if not any(s in it["name"] for s in skip)]
    print("cache: %s (%d files)" % (cache, len(items)))
    bad = 0
    with ThreadPoolExecutor(4) as ex:
        for name, state, info in ex.map(lambda it: fetch_one(it, cache), items):
            print("  %-12s %s %s" % (state, name, info))
            if state == "FAILED":
                bad += 1
    if not any("librespot" in s or "rust" in s for s in skip):
        bad += vendor_crates(cache)
    if bad:
        print("%d problem(s). Run the script again; if one file keeps failing, its address in sources.txt may have moved." % bad)
        return 1
    print("all downloads are in place")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
