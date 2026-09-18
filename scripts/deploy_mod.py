#!/usr/bin/env python3
"""Builds a mod via make_mods.py and copies its win-amd64 DLL into a game
repo's mods/ directory, so it's picked up by that game's mod loader.

Usage:
    python scripts/deploy_mod.py party_overlay
    python scripts/deploy_mod.py party_overlay --game-dir ../EternalSonataReprise
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_mods import CONFIG_POSTFIXES, DEFAULT_MOD_BUILD_TYPE  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_GAME_DIR = REPO_ROOT.parent / "EternalSonataReprise"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mod", help="Mod name under src/ (e.g. party_overlay)")
    parser.add_argument(
        "--game-dir",
        type=Path,
        default=DEFAULT_GAME_DIR,
        help=f"Game repo to deploy into (default: {DEFAULT_GAME_DIR})",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Copy the existing build output instead of rebuilding",
    )
    parser.add_argument(
        "--config", choices=list(CONFIG_POSTFIXES.keys()), default=DEFAULT_MOD_BUILD_TYPE,
        help=f"CMake build config to deploy (default: {DEFAULT_MOD_BUILD_TYPE})",
    )
    args = parser.parse_args()
    postfix = CONFIG_POSTFIXES[args.config]

    if not args.skip_build:
        subprocess.run(
            [sys.executable, str(REPO_ROOT / "scripts" / "make_mods.py"),
             "--mod", args.mod, "--config", args.config],
            cwd=REPO_ROOT,
            check=True,
        )

    dll_name = f"{args.mod}{postfix}.dll"
    src = REPO_ROOT / "mods" / args.mod / "code" / "win-amd64" / dll_name
    if not src.is_file():
        sys.exit(f"error: built DLL not found at {src}")

    dest_dir = args.game_dir / "mods" / args.mod / "code" / "win-amd64"
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / dll_name
    shutil.copy2(src, dest)
    print(f"Copied {src} -> {dest}")

    # The mod loader also needs the metadata alongside code/, not just the DLL.
    metadata_src = REPO_ROOT / "src" / args.mod / "mod.toml"
    if not metadata_src.is_file():
        sys.exit(f"error: mod.toml not found at {metadata_src}")
    metadata_dest_dir = args.game_dir / "mods" / args.mod
    shutil.copy2(metadata_src, metadata_dest_dir / "mod.toml")
    print(f"Copied {metadata_src} -> {metadata_dest_dir / 'mod.toml'}")

    icon_src = REPO_ROOT / "src" / args.mod / "icon.png"
    if icon_src.is_file():
        shutil.copy2(icon_src, metadata_dest_dir / "icon.png")
        print(f"Copied {icon_src} -> {metadata_dest_dir / 'icon.png'}")


if __name__ == "__main__":
    main()
