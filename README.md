# EternalSonataReprise-Mods

Source for mods built against
[EternalSonataReprise](https://github.com/birabittoh/EternalSonataReprise)'s
mod loader. See that repo's
[`docs/making-mods.md`](https://github.com/birabittoh/EternalSonataReprise/blob/main/docs/making-mods.md)
for the mod-authoring guide (plugin ABI, shared mod registry, `mod.toml`
format, etc.).

## Layout

```
src/<name>/       mod source
src/common/       shared CMake helper + header-only include/ used by mods
scripts/          build tooling
```

## Building

```
python scripts/download-sdk.py --pinned
python scripts/make_mods.py
```

`make_mods.py` builds every `src/<name>/` project for the host's own
platform by default (`--target` to pick one of `win-amd64`, `linux-amd64`,
`linux-arm64`, `mac-amd64`, `mac-arm64`, or `android-arm64` explicitly, repeatable)
and assembles the result into
`mods/<name>/code/<platform>/`. Pass `--mod <name>` (repeatable) to build a
subset, `--package` to also zip each mod individually.

Binaries live under `code/<platform>/` (`code/win-amd64/<name>.dll`,
`code/linux-amd64/lib<name>.so`, `code/linux-arm64/lib<name>.so`,
`code/mac-amd64/lib<name>.dylib`, `code/mac-arm64/lib<name>.dylib`,
`code/android-arm64/lib<name>.so`) rather than
flat in `code/`, so one mod folder can carry every platform's binaries
at once without the two `.so` builds colliding on the same filename.
The mod loader checks `code/<platform>/<stem>...` (matching the running
host) before falling back to a flat `code/<stem>...`.

## Publishing to the in-game catalog

Tagging `v*` builds every mod, attaches one zip per mod to the GitHub
Release and generates `catalog.json` next to them
(`scripts/gen_catalog.py`). The game's mod manager fetches
`releases/latest/download/catalog.json` whenever its primary catalog backend
is unavailable. Only the ids listed in `catalog.toml` are included, so add a
mod there to publish it; `featured` ids sort to the top. An `icon.png` in the
mod's source folder becomes its catalog icon.
