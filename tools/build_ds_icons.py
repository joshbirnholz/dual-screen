#!/usr/bin/env python3
"""Adds every DS game's icon to the icons branch, from DS-Scene's release list.

DS-Scene keeps each scene release's banner icon as a 32x32 PNG at
img.files-ds-scene.net/boxarts/<range>/<release>.png, with every palette
colour drawn -- colour 0 too, which the console shows as transparent -- so
the picture is exactly the ROM's. There are no game codes beside them, so
they are named by the picture itself: Dualscreen reads the same icon out of
the ROM and works out the same name (see ds_icon_key() in the app):

  every pixel, row by row, as BGR555 -- red, green and blue each cut to five
  bits -- packed into a little-endian 16-bit number; SHA-1 of those 2048
  bytes, in lowercase hex.

Release numbers come from ADVANsCEne's NDS dat (the same numbering).
Saved as ds/<sha1>.png, 256x256, colour 0 transparent when the four corners
share it (as good a guess as there is without the palette's order).
"""
import concurrent.futures as futures
import hashlib
import io
import json
import os
import re
import sys
import urllib.request
import zipfile

from PIL import Image

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
DAT = "https://www.advanscene.com/offline/datas/ADVANsCEne_NDS_S.zip"


def get(url):
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def releases():
    data = zipfile.ZipFile(io.BytesIO(get(DAT)))
    xml = data.read(data.namelist()[0]).decode("utf-8", "replace")
    found = {}
    for game in re.findall(r"<game>(.*?)</game>", xml, re.S):
        number = re.search(r"<releaseNumber>(\d+)</releaseNumber>", game)
        title = re.search(r"<title>(.*?)</title>", game)
        if number:
            found[int(number.group(1))] = (title.group(1) if title else "").replace("&amp;", "&")
    return found


def icon_url(number):
    low = (number - 1) // 250 * 250 + 1
    return f"http://img.files-ds-scene.net/boxarts/{low:04d}-{low + 249:04d}/{number:04d}.png"


def key_of(image):
    packed = bytearray()
    for r, g, b in image.getdata():
        value = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
        packed += bytes((value & 0xFF, value >> 8))
    return hashlib.sha1(packed).hexdigest()


def shown(image):
    rgba = image.convert("RGBA")
    corners = {rgba.getpixel(p) for p in ((0, 0), (31, 0), (0, 31), (31, 31))}
    if len(corners) == 1:
        key = corners.pop()
        rgba.putdata([(0, 0, 0, 0) if p == key else p for p in rgba.getdata()])
    return rgba.resize((256, 256), Image.NEAREST)


def fetch(number):
    try:
        image = Image.open(io.BytesIO(get(icon_url(number))))
    except Exception as error:
        return number, None, str(error)
    if image.size != (32, 32):
        return number, None, f"{image.size}"
    return number, image.convert("RGB"), None


def main():
    titles = releases()
    print(f"{len(titles)} releases", flush=True)
    index_path = os.path.join(OUT, "index.json")
    index = json.load(open(index_path)) if os.path.exists(index_path) else {}
    ds = index.setdefault("ds", {})
    os.makedirs(os.path.join(OUT, "ds"), exist_ok=True)
    missing = 0
    with futures.ThreadPoolExecutor(max_workers=8) as pool:
        for number, image, error in pool.map(fetch, sorted(titles)):
            if image is None:
                missing += 1
                print(f"{number}: {error}", flush=True)
                continue
            key = key_of(image)
            if key not in ds:
                ds[key] = titles[number]
                shown(image).save(os.path.join(OUT, "ds", key + ".png"), optimize=True)
    print(f"DS: {len(ds)} icons from {len(titles)} releases, {missing} without one", flush=True)
    with open(index_path, "w") as f:
        json.dump(index, f, indent=1, ensure_ascii=False, sort_keys=True)


if __name__ == "__main__":
    main()
