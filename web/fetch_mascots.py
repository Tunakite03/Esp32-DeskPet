"""Downloads the page-mascot characters for the customizer's gallery.

Writes web/mascots/ (git-ignored):
  <id>-directions.webp, <id>-reactions.webp   the sprite sheets, unchanged
  thumbs/<id>.webp                            a small picture of the front pose
  index.json                                  groups, names and file names
  LICENSE                                     page-mascot's MIT license

The sheets come from a pinned commit of github.com/nilbuild/page-mascot, so
the gallery only changes when PAGE_MASCOT_COMMIT does. The deploy workflow
runs this script; run it once yourself before serving the page locally:

  pip install pillow
  python web/fetch_mascots.py
"""

import concurrent.futures
import io
import json
import pathlib
import sys
import urllib.request

from PIL import Image

PAGE_MASCOT_COMMIT = "76a44ed9180063bf4852b9dd724b656f4a828ac7"
RAW = f"https://raw.githubusercontent.com/nilbuild/page-mascot/{PAGE_MASCOT_COMMIT}"
OUT = pathlib.Path(__file__).resolve().parent / "mascots"
THUMB_SIDE = 112
ALPHA_THRESHOLD = 24  # same as build_pet_assets.py and app.js

# The order and groups of site/mascots.ts at PAGE_MASCOT_COMMIT; "Styles" are
# its STYLES variants of the fox.
GROUPS = {
    "Animals": ["bear", "bunny", "cat", "deer", "dino", "fox", "frog", "hamster", "hedgehog", "koala", "mouse",
                "otter", "owl", "panda", "penguin", "pug", "raccoon", "redpanda", "sheep", "sloth", "tiger"],
    "People": ["afro", "astronaut", "bald", "ballerina", "beard", "builder", "cap", "chef", "glasses", "grandpa",
               "granny", "hijabi", "kamran", "nurse", "pirate", "scientist", "sikh", "skater", "wizard"],
    "Robots": ["clockwork", "crt", "cube", "drone", "gearbot", "knight", "lantern", "postbot", "radio", "rocket",
               "scout", "toaster", "tv"],
    "Styles": ["fox-ink", "fox-sketch", "fox-riso", "fox-paper", "fox-pixel"],
}
# Shown on the board, so at most 12 ASCII characters.
NAMES = {"redpanda": "Red Panda", "crt": "CRT", "tv": "TV", "afro": "Afro"}


def display_name(mascot_id: str) -> str:
    name = NAMES.get(mascot_id) or " ".join(part.capitalize() for part in mascot_id.split("-"))
    assert 1 <= len(name) <= 12 and name.isascii(), name
    return name


def download(path: str) -> bytes:
    with urllib.request.urlopen(f"{RAW}/{path}", timeout=60) as response:
        return response.read()


def fetch_sheets(mascot_id: str) -> None:
    for sheet in ("directions", "reactions"):
        target = OUT / f"{mascot_id}-{sheet}.webp"
        if not target.exists():
            data = download(f"public/mascots/{mascot_id}-{sheet}.webp")
            Image.open(io.BytesIO(data)).verify()  # refuse an error page saved as an image
            target.write_bytes(data)
    make_thumb(mascot_id)


def make_thumb(mascot_id: str) -> None:
    """The front pose (centre cell of the directions sheet), trimmed to the artwork."""
    sheet = Image.open(OUT / f"{mascot_id}-directions.webp").convert("RGBA")
    if sheet.width != sheet.height:
        raise ValueError(f"{mascot_id}: the directions sheet is not square")
    cell = sheet.width // 3
    front = sheet.crop((cell, cell, 2 * cell, 2 * cell))
    box = front.getchannel("A").point(lambda alpha: 255 if alpha > ALPHA_THRESHOLD else 0).getbbox()
    if box:
        left, top, right, bottom = box
        side = max(right - left, bottom - top) + cell // 20
        centre_x, centre_y = (left + right) // 2, (top + bottom) // 2
        front = front.crop((centre_x - side // 2, centre_y - side // 2,
                            centre_x - side // 2 + side, centre_y - side // 2 + side))
    front = front.resize((THUMB_SIDE, THUMB_SIDE), Image.LANCZOS)
    front.save(OUT / "thumbs" / f"{mascot_id}.webp", "WEBP", quality=82, method=6)


def main() -> int:
    (OUT / "thumbs").mkdir(parents=True, exist_ok=True)
    ids = [mascot_id for group in GROUPS.values() for mascot_id in group]
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        futures = {pool.submit(fetch_sheets, mascot_id): mascot_id for mascot_id in ids}
        for done, future in enumerate(concurrent.futures.as_completed(futures), 1):
            future.result()
            print(f"\r{done}/{len(ids)} {futures[future]:<12}", end="", flush=True)
    print()
    (OUT / "LICENSE").write_bytes(download("LICENSE"))
    index = {
        "source": "https://github.com/nilbuild/page-mascot",
        "commit": PAGE_MASCOT_COMMIT,
        "license": "MIT",
        "groups": [
            {"name": group, "mascots": [{"id": mascot_id, "name": display_name(mascot_id)} for mascot_id in members]}
            for group, members in GROUPS.items()
        ],
    }
    (OUT / "index.json").write_text(json.dumps(index, indent=1) + "\n", encoding="utf-8")
    size = sum(path.stat().st_size for path in OUT.rglob("*") if path.is_file())
    print(f"{len(ids)} characters, {size / 1048576:.1f} MB in {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
