#!/usr/bin/env python3
"""Fills the icons branch: every 3DS and Wii U game's own icon, as Nintendo
serves it to the consoles' HOME Menus, saved as a PNG named by title ID.

Run once, by .github/workflows on this branch; nothing writes here after.
Dualscreen only reads: <console>/<TITLE ID>.png, e.g.
wiiu/0005000010101D00.png, 3ds/0004000000030800.png.

Sources:
  - Title IDs: 3DS from ghost-land/3dsdb's folders; Wii U by asking for every
    ID in the range retail and eShop games use (00050000-1010xx00 to
    -101Fxx00), since no full list exists.
  - Icons: Nintendo's icon server (idbe-*.cdn.nintendo.net), the files the
    consoles themselves download. AES-CBC with keys that are public; see
    https://nintendo-wiki.pretendo.network/docs/idbe. Each file carries a
    SHA-256 of its contents, which is checked.
"""
import concurrent.futures as futures
import hashlib
import io
import json
import os
import subprocess
import sys
import threading

import ssl

import requests
from Crypto.Cipher import AES
from PIL import Image

IV = bytes.fromhex("a46987ae47d82bb4fa8abc0450285fa4")
KEYS = [bytes.fromhex(k) for k in (
    "4ab9a40e146975a84bb1b4f3ecefc47b",
    "90a0bb1e0e864ae87d13a6a03d28c9b8",
    "ffbb57c14e98ec6975b384fcf40786b5",
    "80923799b41f36a6a75fb8b48c95f66f",
)]
SIZE = 256  # what is saved: Discord shows it at about 80-120 px

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
CA = os.environ.get("NINTENDO_CDN_CERT")  # the server's own certificate, pinned
local = threading.local()


class PinnedAdapter(requests.adapters.HTTPAdapter):
    # Nintendo's CDN is signed by Nintendo's own CA, which no system trusts;
    # its certificate is trusted directly instead, and checked as usual.
    def init_poolmanager(self, *args, **kwargs):
        context = ssl.create_default_context(cafile=CA)
        context.verify_flags |= ssl.VERIFY_X509_PARTIAL_CHAIN
        kwargs["ssl_context"] = context
        return super().init_poolmanager(*args, **kwargs)


def session():
    if not hasattr(local, "s"):
        local.s = requests.Session()
        adapter = (PinnedAdapter if CA else requests.adapters.HTTPAdapter)(
            pool_connections=4, pool_maxsize=4, max_retries=3)
        local.s.mount("https://", adapter)
    return local.s


def fetch_idbe(host, tid):
    url = f"https://{host}/icondata/{(tid >> 8) & 0xFF:02X}/{tid:016X}.idbe"
    r = session().get(url, timeout=30)
    if r.status_code != 200 or len(r.content) < 0x40:
        return None
    data = r.content
    if data[0] != 0 or data[1] > 3:
        return None
    plain = AES.new(KEYS[data[1]], AES.MODE_CBC, IV).decrypt(data[2:])
    if hashlib.sha256(plain[0x20:]).digest() != plain[:0x20]:
        raise ValueError(f"{tid:016X}: checksum does not match")
    return plain


def name_of(plain, big_endian, language=1):
    # 16 blocks of 0x200 from 0x50: short name 0x80, long name 0x100,
    # publisher 0x80, UTF-16. Language 1 is English.
    block = plain[0x50 + language * 0x200: 0x50 + (language + 1) * 0x200]
    raw = block[0x80:0x180] if any(block[0x80:0x180]) else block[:0x80]
    text = raw.decode("utf-16-be" if big_endian else "utf-16-le", "ignore")
    return text.split("\0", 1)[0].replace("\n", " ").strip()


def untile_3ds(raw, size=48):
    # RGB565, in 8x8 tiles, each in Morton (Z) order.
    image = Image.new("RGB", (size, size))
    px = image.load()
    i = 0
    for ty in range(0, size, 8):
        for tx in range(0, size, 8):
            for n in range(64):
                x = (n & 1) | ((n >> 1) & 2) | ((n >> 2) & 4)
                y = ((n >> 1) & 1) | ((n >> 2) & 2) | ((n >> 3) & 4)
                v = raw[i] | (raw[i + 1] << 8)
                i += 2
                r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
                px[tx + x, ty + y] = (r * 255 // 31, g * 255 // 63, b * 255 // 31)
    return image


def save(image, path):
    image = image.convert("RGBA").resize((SIZE, SIZE), Image.LANCZOS)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    image.save(path, optimize=True)


def wii_u(tid):
    plain = fetch_idbe("idbe-wup.cdn.nintendo.net", tid)
    if plain is None:
        return None
    if int.from_bytes(plain[0x20:0x28], "big") != tid:
        return None
    image = Image.open(io.BytesIO(plain[0x2050:0x2050 + 0x1002C]))
    save(image, os.path.join(OUT, "wiiu", f"{tid:016X}.png"))
    return name_of(plain, True)


def three_ds(tid):
    plain = fetch_idbe("idbe-ctr.cdn.nintendo.net", tid)
    if plain is None:
        return None
    save(untile_3ds(plain[0x24D0:0x24D0 + 0x1200]), os.path.join(OUT, "3ds", f"{tid:016X}.png"))
    return name_of(plain, False)


def three_ds_title_ids():
    if not os.path.isdir("3dsdb"):
        subprocess.run(["git", "clone", "-q", "--depth", "1", "--filter=blob:none", "--no-checkout",
                        "https://github.com/ghost-land/3dsdb", "3dsdb"], check=True)
    ids = set()
    for category in ("base", "virtual-console", "region-free", "unknown-other"):
        listing = subprocess.run(["git", "-C", "3dsdb", "ls-tree", "-d", "--name-only", f"HEAD:db/3ds/{category}"],
                                 capture_output=True, text=True).stdout
        for line in listing.split():
            try:
                tid = int(line, 16)
            except ValueError:
                continue
            if tid >> 32 == 0x00040000:
                ids.add(tid)
    return sorted(ids)


def run(label, ids, work):
    names = {}
    done = 0
    with futures.ThreadPoolExecutor(max_workers=24) as pool:
        for tid, result in zip(ids, pool.map(lambda t: (t, safe(work, t)), ids)):
            done += 1
            if result[1]:
                names[f"{tid:016X}"] = result[1]
            if done % 2000 == 0:
                print(f"{label}: {done}/{len(ids)} asked, {len(names)} found", flush=True)
    print(f"{label}: {len(names)} icons from {len(ids)} title IDs", flush=True)
    return names


def safe(work, tid):
    try:
        return work(tid)
    except Exception as error:  # one bad file is not the whole run
        print(f"{tid:016X}: {error}", flush=True)
        return None


def main():
    which = os.environ.get("CONSOLES", "3ds,wiiu").split(",")
    index_path = os.path.join(OUT, "index.json")
    index = json.load(open(index_path)) if os.path.exists(index_path) else {}
    if "3ds" in which:
        ids = three_ds_title_ids()
        if os.environ.get("LIMIT"):
            ids = ids[: int(os.environ["LIMIT"])]
        index.setdefault("3ds", {}).update(run("3DS", ids, three_ds))
    if "wiiu" in which:
        first = int(os.environ.get("WIIU_FIRST", "101000"), 16)
        last = int(os.environ.get("WIIU_LAST", "101FFF"), 16)
        ids = [(0x00050000 << 32) | (n << 8) for n in range(first, last + 1)]
        if os.environ.get("WIIU_IDS"):  # asking again for ones that timed out
            ids = [int(t, 16) for t in os.environ["WIIU_IDS"].split(",")]
        index.setdefault("wiiu", {}).update(run("Wii U", ids, wii_u))
    with open(index_path, "w") as f:
        json.dump(index, f, indent=1, ensure_ascii=False, sort_keys=True)


if __name__ == "__main__":
    main()
