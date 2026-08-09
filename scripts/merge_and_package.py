#!/usr/bin/env python3
"""Merges per-platform mods/ trees (as built by make_mods.py --target
<platform> on separate CI runners) into one zip per mod, each zip carrying
every platform's binary under its own code/<platform>/ subdirectory.

Each --input is a directory containing mod folders directly at its root
(i.e. the contents of one platform's mods/ dir, as produced by a single
make_mods.py --target <platform> run -- see .github/workflows/_build.yml,
which downloads each matrix leg's raw build as a separate artifact and
passes each one here as an --input). Mods present in only some inputs still
get packaged, just with fewer platform subdirectories.
"""
import argparse
import json
import os
import re
import shutil
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_mods import PLATFORM_INFO, update_mod_platform_field  # noqa: E402

VERSION_PATTERN = re.compile(r'^\s*version\s*=\s*"([^"]*)"')


def mod_version(mod_dir):
    manifest_path = os.path.join(mod_dir, "mod.toml")
    if not os.path.isfile(manifest_path):
        return None
    with open(manifest_path, "r", encoding="utf-8") as f:
        for line in f:
            m = VERSION_PATTERN.match(line)
            if m:
                value = m.group(1).strip()
                return value if value.startswith("v") else f"v{value}"
    return None


def merge_mod(mod_name, input_dirs, merge_root):
    dest = os.path.join(merge_root, mod_name)
    for input_dir in input_dirs:
        src = os.path.join(input_dir, mod_name)
        if not os.path.isdir(src):
            continue
        for dirpath, _dirnames, filenames in os.walk(src):
            rel = os.path.relpath(dirpath, src)
            dest_dir = os.path.join(dest, rel) if rel != "." else dest
            os.makedirs(dest_dir, exist_ok=True)
            for fname in filenames:
                shutil.copy2(os.path.join(dirpath, fname), os.path.join(dest_dir, fname))

    # Each input's mod.toml only lists the one platform that CI leg built, so
    # whichever --input was merged in last just clobbered the others' platform
    # value. Recompute it from the union of platform subdirs actually present
    # under the merged code/ dir. Asset-only mods have no code/ dir at all and
    # don't carry a `platform` key -- leave those alone.
    code_dir = os.path.join(dest, "code")
    if os.path.isdir(code_dir):
        built = [info["manifest_key"] for info in PLATFORM_INFO.values()
                 if os.path.isdir(os.path.join(code_dir, info["manifest_key"]))]
        update_mod_platform_field(merge_root, mod_name, built)
    return dest


def zip_mod(mod_dir, mod_name, output_dir):
    os.makedirs(output_dir, exist_ok=True)
    version = mod_version(mod_dir)
    stem = f"{mod_name}-{version}" if version else mod_name
    archive_path = os.path.join(output_dir, f"{stem}.zip")
    with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for dirpath, _dirnames, filenames in os.walk(mod_dir):
            for fname in filenames:
                full = os.path.join(dirpath, fname)
                arcname = os.path.join(mod_name, os.path.relpath(full, mod_dir))
                zf.write(full, arcname)
    print(f"wrote {archive_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", action="append", required=True, dest="inputs",
                        help="A platform's built mods/ dir (repeatable, one per platform)")
    parser.add_argument("--output", required=True, help="Directory to write <mod>-<version>.zip "
                                                          "files to")
    parser.add_argument("--write-mod-list", help="Also write a JSON array of built mod names to "
                                                   "this path (e.g. for a CI matrix step)")
    parser.add_argument("--merged-output", help="Also copy each mod's merged, unzipped directory "
                                                  "tree here as <merged-output>/<mod>/ (e.g. for "
                                                  "uploading raw, not pre-zipped, CI artifacts)")
    args = parser.parse_args()

    for d in args.inputs:
        if not os.path.isdir(d):
            print(f"error: input dir '{d}' not found", file=sys.stderr)
            sys.exit(1)

    mod_names = sorted({
        name for input_dir in args.inputs
        for name in os.listdir(input_dir)
        if os.path.isdir(os.path.join(input_dir, name))
    })
    if not mod_names:
        print("error: no mod folders found in any --input dir", file=sys.stderr)
        sys.exit(1)

    with tempfile.TemporaryDirectory() as merge_root:
        for name in mod_names:
            mod_dir = merge_mod(name, args.inputs, merge_root)
            zip_mod(mod_dir, name, args.output)
            if args.merged_output:
                shutil.copytree(mod_dir, os.path.join(args.merged_output, name))

    if args.write_mod_list:
        with open(args.write_mod_list, "w", encoding="utf-8") as f:
            json.dump(mod_names, f)

    print(f"\nPackaged {len(mod_names)} mod(s): {', '.join(mod_names)}")


if __name__ == "__main__":
    main()
