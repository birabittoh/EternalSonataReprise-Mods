#!/usr/bin/env python3
"""Writes catalog.json, the static mod catalog the game falls back to when
its Firestore backend is unavailable (see rex::system::ModCatalog's
mod_catalog_fallback_url in the SDK).

Runs in the release workflow after every mod has been packaged, against the
zips about to become release assets: only the ids listed in catalog.toml's
`published` array are emitted, each with the metadata from its src/<mod>/
mod.toml, the deterministic release-asset URL of its zip, that zip's SHA-256
and the platforms it actually ships binaries for. Icons point at the mod's
icon.png in this repo, pinned to the release tag.
"""
import argparse
import hashlib
import json
import os
import re
import sys
import tomllib
import zipfile

VERSION_PATTERN = re.compile(r"^\s*version\s*=")


def split_list(value):
    if isinstance(value, list):
        return [str(v).strip() for v in value if str(v).strip()]
    return [v.strip() for v in str(value).split(",") if v.strip()]


def zip_platforms(zip_path, mod_id):
    plats = set()
    with zipfile.ZipFile(zip_path) as zf:
        for name in zf.namelist():
            parts = name.split("/")
            if len(parts) >= 4 and parts[0] == mod_id and parts[1] == "code":
                plats.add(parts[2])
    return sorted(plats)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build_entry(mod_id, src_dir, assets_dir, repo, tag, featured):
    manifest_path = os.path.join(src_dir, mod_id, "mod.toml")
    with open(manifest_path, "rb") as f:
        manifest = tomllib.load(f)

    version = str(manifest.get("version", "")).strip()
    if not version:
        raise SystemExit(f"error: {manifest_path} has no version")
    tagged = version if version.startswith("v") else f"v{version}"
    zip_name = f"{mod_id}-{tagged}.zip"
    zip_path = os.path.join(assets_dir, zip_name)
    if not os.path.isfile(zip_path):
        raise SystemExit(f"error: release asset {zip_name} not found in {assets_dir}")

    # mod.toml accepts "1.0.0" and ">= 1.0.0" alike; the catalog wants the bare
    # minimum version.
    game_version = str(manifest.get("game_version", "")).strip()
    game_version = re.sub(r"^>=\s*", "", game_version)

    icon_url = ""
    if os.path.isfile(os.path.join(src_dir, mod_id, "icon.png")):
        icon_url = f"https://raw.githubusercontent.com/{repo}/{tag}/src/{mod_id}/icon.png"

    return {
        "modId": mod_id,
        "name": str(manifest.get("name", mod_id)),
        "author": str(manifest.get("author", "")),
        "description": str(manifest.get("description", "")),
        "version": version.lstrip("v"),
        "gameVersion": game_version,
        "assetUrl": f"https://github.com/{repo}/releases/download/{tag}/{zip_name}",
        "checksum": sha256_file(zip_path),
        "platform": zip_platforms(zip_path, mod_id),
        "requires": split_list(manifest.get("requires", "")),
        "iconUrl": icon_url,
        "status": "featured" if mod_id in featured else "approved",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--assets", required=True, help="Directory holding the <mod>-<version>.zip "
                                                        "release assets")
    parser.add_argument("--repo", required=True, help="owner/name of this GitHub repository")
    parser.add_argument("--tag", required=True, help="Release tag the assets are published under")
    parser.add_argument("--config", default="catalog.toml")
    parser.add_argument("--src", default="src")
    parser.add_argument("--output", default="catalog.json")
    args = parser.parse_args()

    with open(args.config, "rb") as f:
        config = tomllib.load(f)
    published = split_list(config.get("published", []))
    featured = set(split_list(config.get("featured", [])))
    unknown = featured - set(published)
    if unknown:
        raise SystemExit(f"error: featured but not published: {', '.join(sorted(unknown))}")
    if not published:
        raise SystemExit("error: catalog.toml publishes no mods")

    mods = [build_entry(m, args.src, args.assets, args.repo, args.tag, featured)
            for m in published]
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump({"tag": args.tag, "mods": mods}, f, indent=2)
        f.write("\n")
    print(f"wrote {args.output} with {len(mods)} mod(s): {', '.join(published)}")


if __name__ == "__main__":
    main()
