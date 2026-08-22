#!/usr/bin/env python3
"""Builds every mod under src/ and assembles it into mods/<name>/.

A src/<name>/ directory with no CMakeLists.txt is an asset-only mod (game
file/texture/shader replacements, no native code) -- see
EternalSonataReprise's docs/making-mods.md. It's copied into mods/<name>/ as-is, once, with no
per-platform build step and no `platform` key written into its mod.toml.

Each src/<name>/ directory that *does* have a CMakeLists.txt is a standalone
CMake project (its own
CMakeLists.txt including src/common/mod_cmake/rexmod.cmake) that links
against the SDK's rex::runtime, found via CMAKE_PREFIX_PATH pointing at an
SDK directory. The resulting binary is copied to
mods/<name>/code/<platform>/<name><postfix>.dll (or lib<name><postfix>.so),
one subdirectory per platform, alongside mod.toml and icon.png at the mod
root.

Mods build Release by default (bare name, e.g. <name>.dll importing
rexruntime.dll). Pass --config RelWithDebInfo to build against a
RelWithDebInfo host instead, which resolves <name>rd.dll importing
rexruntimerd.dll (see CONFIG_POSTFIXES below).

The code/<platform>/ nesting (rather than a flat code/ like the mod
loader expects -- see rex::system::LoadModPlugin) is deliberate: it lets
one mod folder, and therefore one release zip (see
scripts/merge_and_package.py), carry all three platforms' binaries side by
side without linux-x64 and linux-arm64 colliding on the same lib<name>.so
filename. Installing a mod into an actual game copy means flattening the
one platform subdirectory you need into code/ directly (see README.md).

This repo tracks three platforms: windows-x64, linux-x64, linux-arm64. By
default every mod is built for whichever platform(s) you pass via --target
(default: just the host's own platform, matching how CI's three-runner
matrix builds one platform per job, then merges all three into per-mod
release zips -- see .github/workflows/_build.yml).

Each target's SDK lives in its own subdirectory (<sdk-root>/win-amd64,
<sdk-root>/linux-amd64, <sdk-root>/linux-arm64), fetched via
scripts/download-sdk.py --pinned --platform ...

Building a target that doesn't match the host: windows-x64 and linux-x64 can
be cross-built via Docker (see scripts/docker/). linux-arm64 has no cross-build path here -- build
it on an arm64 host (CI uses a native ubuntu-24.04-arm runner for exactly
this reason).

After building, each mod's `platform` key in mods/<name>/mod.toml (the built
copy, not the one under src/) is (re)written to record which platform(s)
mods/<name>/code/ actually ships a binary for right now -- this key is
script-managed, not something a mod author sets by hand, and never touches
the src/ manifest.
"""
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys


PLATFORM_INFO = {
    "windows-x64": {"ext": ".dll", "prefix": "", "sdk_subdir": "win-amd64", "manifest_key": "windows-x64"},
    "linux-x64": {"ext": ".so", "prefix": "lib", "sdk_subdir": "linux-amd64", "manifest_key": "linux-x64"},
    "linux-arm64": {"ext": ".so", "prefix": "lib", "sdk_subdir": "linux-arm64", "manifest_key": "linux-arm64"},
}

# Mods build Release (plain name) by default.
# Pass --config RelWithDebInfo to build against a host built RelWithDebInfo
# instead -- the mod loader (rex::system::LoadModPlugin) resolves
# <stem><postfix>.dll for the host's own config before falling back to a
# bare name, and CMake targets pick up the matching postfix automatically
# via CMAKE_<CONFIG>_POSTFIX set in src/common/mod_cmake/rexmod.cmake.
DEFAULT_MOD_BUILD_TYPE = "Release"

CONFIG_POSTFIXES = {"Release": "", "RelWithDebInfo": "rd", "Debug": "d"}

LINUX_DOCKER_IMAGE = "nocturnerecomp-mods-linux-builder"
WINDOWS_CROSS_DOCKER_IMAGE = "nocturnerecomp-mods-windows-cross-builder"
XWIN_CACHE_DIR = "/opt/xwin-cache"


def host_plat():
    system = platform.system()
    arch = platform.machine().lower()
    if system == "Windows":
        return "windows-x64"
    if system == "Linux":
        if arch in ("x86_64", "amd64"):
            return "linux-x64"
        if arch in ("aarch64", "arm64"):
            return "linux-arm64"
    return None  # macOS or anything else: no target can be built natively


def can_cross_build_windows_locally():
    return (
        host_plat() != "windows-x64"
        and shutil.which("clang-cl") is not None
        and os.path.isdir(XWIN_CACHE_DIR)
    )


def find_clangxx():
    if platform.system() != "Windows":
        for version in range(30, 17, -1):
            if shutil.which(f"clang++-{version}"):
                return f"clang++-{version}"
    if shutil.which("clang++"):
        return "clang++"
    print("error: no clang++ compiler found in PATH", file=sys.stderr)
    sys.exit(1)


def run(args, **kwargs):
    print(f"+ {' '.join(str(a) for a in args)}")
    result = subprocess.run(args, **kwargs)
    if result.returncode != 0:
        sys.exit(result.returncode)
    return result


def find_built_binary(build_dir, name, plat, config):
    info = PLATFORM_INFO[plat]
    postfix = CONFIG_POSTFIXES[config]
    fname = f"{info['prefix']}{name}{postfix}{info['ext']}"
    candidates = [
        os.path.join(build_dir, fname),
        os.path.join(build_dir, config, fname),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return path
    return None


def built_platforms_for_mod(root, name, postfix):
    code_dir = os.path.join(root, "mods", name, "code")
    built = []
    for plat, info in PLATFORM_INFO.items():
        binary = os.path.join(code_dir, info["manifest_key"],
                              f"{info['prefix']}{name}{postfix}{info['ext']}")
        if os.path.isfile(binary):
            built.append(info["manifest_key"])
    return built


def update_mod_platform_field(src_dir, name, manifest_keys):
    manifest_path = os.path.join(src_dir, name, "mod.toml")
    if not os.path.isfile(manifest_path):
        return

    value = ",".join(manifest_keys)
    with open(manifest_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    pattern = re.compile(r'^\s*platform\s*=')
    new_line = f'platform = "{value}"\n'
    replaced = False
    for i, line in enumerate(lines):
        if pattern.match(line):
            lines[i] = new_line
            replaced = True
            break
    if not replaced:
        if lines and not lines[-1].endswith("\n"):
            lines[-1] += "\n"
        lines.append(new_line)

    with open(manifest_path, "w", encoding="utf-8") as f:
        f.writelines(lines)


def is_valid_sdk(sdk_dir):
    return os.path.exists(os.path.join(sdk_dir, "lib", "cmake", "rexglue"))


def resolve_native_sdk_dir(sdk_root, sdk_dir, plat):
    if is_valid_sdk(sdk_dir):
        return sdk_dir
    if is_valid_sdk(sdk_root):
        return sdk_root
    return sdk_dir


def require_sdk(sdk_dir, plat):
    if not is_valid_sdk(sdk_dir):
        print(f"error: {plat} SDK not found at '{sdk_dir}' — run "
              f"'python scripts/download-sdk.py {sdk_dir} --pinned --platform "
              f"{PLATFORM_INFO[plat]['sdk_subdir']}' first", file=sys.stderr)
        sys.exit(1)


def build_mod_native(mod_src_dir, name, sdk_dir, cxx_compiler, root, plat, config):
    build_dir = os.path.join(root, "out", "build", "mods", plat, name)
    os.makedirs(build_dir, exist_ok=True)

    configure_args = [
        "cmake",
        "-S", mod_src_dir,
        "-B", build_dir,
        "-G", "Ninja",
        f"-DCMAKE_BUILD_TYPE={config}",
        f"-DCMAKE_PREFIX_PATH={os.path.abspath(sdk_dir)}",
        f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
    ]
    run(configure_args)
    run(["cmake", "--build", build_dir, "--parallel", str(os.cpu_count() or 1)])

    binary = find_built_binary(build_dir, name, plat, config)
    if not binary:
        print(f"error: couldn't find built binary for mod '{name}' under {build_dir}",
              file=sys.stderr)
        sys.exit(1)
    return binary


def build_mod_windows_cross(mod_src_dir, name, sdk_dir, root, config):
    build_dir = os.path.join(root, "out", "build", "mods", "windows-cross", name)
    os.makedirs(build_dir, exist_ok=True)
    toolchain_file = os.path.join(root, "scripts", "docker", "windows-msvc-cross-toolchain.cmake")

    configure_args = [
        "cmake",
        "-S", mod_src_dir,
        "-B", build_dir,
        "-G", "Ninja",
        f"-DCMAKE_BUILD_TYPE={config}",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain_file}",
        f"-DCMAKE_PREFIX_PATH={os.path.abspath(sdk_dir)}",
    ]
    run(configure_args)
    run(["cmake", "--build", build_dir, "--parallel", str(os.cpu_count() or 1)])

    binary = find_built_binary(build_dir, name, "windows-x64", config)
    if not binary:
        print(f"error: couldn't find built binary for mod '{name}' under {build_dir}",
              file=sys.stderr)
        sys.exit(1)
    return binary


def assemble_mod(mod_src_dir, name, binary_path, plat, root, postfix):
    # Binaries land under code/<platform>/ rather than flat code/, since
    # linux-x64 and linux-arm64 both produce lib<name>.so and would
    # otherwise collide -- this also lets a single mod folder (and
    # therefore a single release zip, see merge_and_package.py) carry all
    # three platforms' binaries at once. Installing into an actual game
    # copy means flattening the platform you want into code/ directly
    # (see README).
    info = PLATFORM_INFO[plat]
    dest_dir = os.path.join(root, "mods", name)

    # Copy the whole mod_src_dir tree first, not just mod.toml/icon.png --
    # mods can load arbitrary runtime assets (e.g. foxy_jumpscare's
    # assets/audio.wav and assets/frames/*.png) relative to their mod root,
    # so anything short of the full tree risks silently dropping files a
    # mod needs at runtime.
    shutil.copytree(mod_src_dir, dest_dir, dirs_exist_ok=True)

    code_dir = os.path.join(dest_dir, "code", info["manifest_key"])
    os.makedirs(code_dir, exist_ok=True)

    dest_binary = os.path.join(code_dir, f"{info['prefix']}{name}{postfix}{info['ext']}")
    print(f"+ cp {binary_path} {dest_binary}")
    shutil.copy2(binary_path, dest_binary)


def build_targets_via_docker(plat, names, root, sdk_dir, config):
    if shutil.which("docker") is None:
        print(f"error: can't build '{plat}' natively on this host and no `docker` found in "
              f"PATH to cross-build it instead", file=sys.stderr)
        sys.exit(1)

    if plat == "linux-x64":
        dockerfile = os.path.join(root, "scripts", "docker", "linux-mod-build.Dockerfile")
        image = LINUX_DOCKER_IMAGE
    elif plat == "windows-x64":
        dockerfile = os.path.join(root, "scripts", "docker", "windows-mod-build.Dockerfile")
        image = WINDOWS_CROSS_DOCKER_IMAGE
    else:
        print(f"error: '{plat}' has no Docker cross-build path -- build it on a native "
              f"linux-arm64 host instead", file=sys.stderr)
        sys.exit(1)

    run(["docker", "build", "-t", image, "-f", dockerfile, root])

    mod_args = []
    for name in names:
        mod_args += ["--mod", name]
    sdk_subdir = PLATFORM_INFO[plat]["sdk_subdir"]
    inner_cmd = (
        f"python3 scripts/download-sdk.py {sdk_dir} --pinned --platform {sdk_subdir} && "
        f"python3 scripts/make_mods.py --target {plat} --sdk-dir {sdk_dir} --config {config} "
        + " ".join(mod_args)
    )
    run([
        "docker", "run", "--rm",
        "-v", f"{root}:/workspace",
        "-w", "/workspace",
        image,
        "bash", "-c", inner_cmd,
    ])


def copy_asset_mod(mod_src_dir, name, root):
    dest_dir = os.path.join(root, "mods", name)
    print(f"+ cp -r {mod_src_dir} {dest_dir}")
    shutil.copytree(mod_src_dir, dest_dir, dirs_exist_ok=True)


def package_mod(name, root):
    import zipfile

    mod_dir = os.path.join(root, "mods", name)
    archive_path = os.path.join(root, "mods", f"{name}.zip")
    print(f"+ zip {archive_path}")
    with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for dirpath, _dirnames, filenames in os.walk(mod_dir):
            for fname in filenames:
                full = os.path.join(dirpath, fname)
                arcname = os.path.join(name, os.path.relpath(full, mod_dir))
                zf.write(full, arcname)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sdk-dir", default="sdk",
                        help="SDK root; each platform's SDK is expected at "
                             "<sdk-dir>/win-amd64, <sdk-dir>/linux-amd64, or "
                             "<sdk-dir>/linux-arm64 (default: sdk)")
    parser.add_argument("--mod", action="append", metavar="NAME",
                        help="Only build this mod (repeatable); default: all of src/*")
    parser.add_argument("--target", action="append", dest="targets",
                        choices=list(PLATFORM_INFO.keys()),
                        metavar="{windows-x64,linux-x64,linux-arm64}",
                        help="Only build for this platform (repeatable); default: host's own platform")
    parser.add_argument("--package", action="store_true",
                        help="Also zip each built mod to mods/<name>.zip")
    parser.add_argument("--config", choices=list(CONFIG_POSTFIXES.keys()),
                        default=DEFAULT_MOD_BUILD_TYPE,
                        help=f"CMake build config mods are built with, and whose postfix "
                             f"they ship (default: {DEFAULT_MOD_BUILD_TYPE})")
    args = parser.parse_args()
    postfix = CONFIG_POSTFIXES[args.config]

    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(script_dir, ".."))
    os.chdir(root)

    src_dir = os.path.join(root, "src")
    if not os.path.isdir(src_dir):
        print(f"error: {src_dir} not found", file=sys.stderr)
        sys.exit(1)

    all_src_dirs = sorted(
        name for name in os.listdir(src_dir)
        if name != "common" and os.path.isdir(os.path.join(src_dir, name))
    )
    code_mods = [name for name in all_src_dirs
                 if os.path.isfile(os.path.join(src_dir, name, "CMakeLists.txt"))]
    asset_mods = [name for name in all_src_dirs
                  if name not in code_mods and os.path.isfile(os.path.join(src_dir, name, "mod.toml"))]
    all_mods = sorted(code_mods + asset_mods)

    mod_names = args.mod or all_mods
    unknown = [name for name in mod_names if name not in all_mods]
    if unknown:
        print(f"error: unknown mod(s) {unknown}; available: {all_mods}", file=sys.stderr)
        sys.exit(1)
    code_names = [name for name in mod_names if name in code_mods]
    asset_names = [name for name in mod_names if name in asset_mods]

    for name in asset_names:
        copy_asset_mod(os.path.join(src_dir, name), name, root)

    host = host_plat()
    platforms = args.targets or ([host] if host else [])
    if code_names and not platforms:
        print("error: couldn't detect a native platform for this host -- pass --target explicitly",
              file=sys.stderr)
        sys.exit(1)

    if code_names:
        for plat in platforms:
            print(f"\n=== Platform: {plat} ===")

            sdk_root = args.sdk_dir.replace("\\", "/").rstrip("/")
            sdk_dir = f"{sdk_root}/{PLATFORM_INFO[plat]['sdk_subdir']}"

            if plat == host:
                sdk_dir = resolve_native_sdk_dir(sdk_root, sdk_dir, plat)
                require_sdk(sdk_dir, plat)
                cxx_compiler = find_clangxx()
                for name in code_names:
                    mod_src_dir = os.path.join(src_dir, name)
                    print(f"\n--- Building mod '{name}' natively for {plat} ---")
                    binary = build_mod_native(mod_src_dir, name, sdk_dir, cxx_compiler, root, plat, args.config)
                    assemble_mod(mod_src_dir, name, binary, plat, root, postfix)

            elif plat == "windows-x64" and can_cross_build_windows_locally():
                require_sdk(sdk_dir, plat)
                for name in code_names:
                    mod_src_dir = os.path.join(src_dir, name)
                    print(f"\n--- Cross-building mod '{name}' for windows-x64 (clang-cl + xwin) ---")
                    binary = build_mod_windows_cross(mod_src_dir, name, sdk_dir, root, args.config)
                    assemble_mod(mod_src_dir, name, binary, plat, root, postfix)

            else:
                print(f"host can't build '{plat}' directly -- falling back to Docker")
                build_targets_via_docker(plat, code_names, root, sdk_dir, args.config)

    for name in code_names:
        mod_src_dir = os.path.join(src_dir, name)
        built = built_platforms_for_mod(root, name, postfix)
        manifest_src = os.path.join(mod_src_dir, "mod.toml")
        if os.path.isfile(manifest_src):
            manifest_dest_dir = os.path.join(root, "mods", name)
            shutil.copy2(manifest_src, os.path.join(manifest_dest_dir, "mod.toml"))
            update_mod_platform_field(os.path.join(root, "mods"), name, built)

    if args.package:
        for name in mod_names:
            package_mod(name, root)

    print(f"\nBuilt {len(mod_names)} mod(s) for {', '.join(platforms)}: {', '.join(mod_names)}")


if __name__ == "__main__":
    main()
