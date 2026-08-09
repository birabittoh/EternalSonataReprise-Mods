#pragma once
// Shared helper for mods that overwrite a fixed-length string embedded in
// the game's static data -- e.g. the item/enemy name and description
// tables documented in extracted/README.md in the main repo. Generalizes
// the pattern mods_src/xex_patch_potion and mods_src/xex_patch_red_rust
// each implemented separately, handling the gotchas that tripped up the
// mods built against this table so far (see extracted/README.md's
// "Better way" and "Retargeting to item names" sections for the full
// story of each):
//
//   - The target is typically in the image's read-only data section, so
//     an in-process write access-violates (0xC0000005) unless the page is
//     unlocked first.
//   - A shorter replacement must zero-fill the rest of the original
//     field, or leftover bytes of the old string survive past wherever
//     the new text ends.
//   - NAME fields (not description fields) in this table use a "big
//     first letter" font encoding: the first character is stored as
//     `ASCII - 0x20` (e.g. 'P' 0x50 -> 0x30), remaining characters are
//     plain uppercase ASCII. Writing a plain-ASCII replacement into a name
//     field renders as garbage, because the font's first-glyph table is
//     indexed by that shifted range, not raw ASCII -- use
//     ApplyNameFieldPatch, not ApplyTextPatch, for these. Description
//     fields have no such encoding; ApplyTextPatch is correct for them
//     as-is.
//
// Usage (call from a mod's OnModuleLaunched(), once the guest module is
// loaded and its static data is in memory):
//
//   #include <rexmod/text_patch.h>
//
//   // A description field (plain ASCII):
//   rexmod::ApplyTextPatch(runtime_, kSomeDescAddrVanilla, kFieldLength, "New text");
//
//   // A name field (first letter needs the font's encoding):
//   rexmod::ApplyNameFieldPatch(runtime_, kSomeNameAddrVanilla, kFieldLength, "NEW");
//
// The guest address must come from scanning a *live* process
// (scripts/scan_guest_memory.py), not from an offline-decrypted
// default.xex file offset -- this title applies a title-update delta
// patch (assets/default.xexp) over the base image on every launch, which
// can shift addresses after the patched region. See extracted/README.md.
//
// Also, a field's length must be measured from what actually follows it
// in memory, not assumed from the replaced text's own length: some name
// fields have a null terminator before the next field/entry-end marker,
// others butt directly against it with none at all. See
// extracted/README.md's "Retargeting to item names" section.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace rexmod {

// Overwrites the field at `address`, which must be exactly `field_length`
// bytes (including any terminator/padding that follows it), with
// `replacement` verbatim, zero-filling any bytes past it. `replacement`
// must fit within `field_length` (i.e. `replacement.size() <=
// field_length`) -- longer text would run into whatever field follows,
// so this refuses to write at all rather than risk corrupting
// neighboring data.
//
// Returns false if the replacement doesn't fit, the address isn't backed
// by a heap, or the page can't be unlocked for writing; true if the write
// happened.
inline bool ApplyTextPatch(rex::Runtime* runtime, uint32_t address, uint32_t field_length,
                           std::string_view replacement) {
  if (!runtime || replacement.size() > field_length) {
    return false;
  }
  auto* memory = runtime->memory();
  if (!memory) {
    return false;
  }

  auto* heap = memory->LookupHeap(address);
  if (!heap) {
    return false;
  }

  uint32_t old_protect = 0;
  if (!heap->Protect(address, field_length,
                     rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                     &old_protect)) {
    return false;
  }

  uint8_t* host = memory->TranslateVirtual<uint8_t*>(address);
  bool wrote = false;
  if (host) {
    std::memset(host, 0, field_length);
    std::memcpy(host, replacement.data(), replacement.size());
    wrote = true;
  }

  // Best-effort restore; a failure here doesn't undo the write above, and
  // there's nothing more useful to do with it than leave the page
  // writable rather than pretend the patch didn't happen.
  heap->Protect(address, field_length, old_protect);

  return wrote;
}

// Same as ApplyTextPatch, but for NAME fields: encodes `replacement`'s
// first character with this table's "big first letter" font offset
// (ASCII - 0x20) before writing, since that's how every name field in the
// item/enemy tables stores its first character. `replacement` must be
// plain uppercase ASCII (matching how every name in this table is cased);
// passing lowercase or punctuation as the first character will encode to
// some other, likely wrong, glyph.
inline bool ApplyNameFieldPatch(rex::Runtime* runtime, uint32_t address, uint32_t field_length,
                                std::string_view replacement) {
  if (replacement.empty()) {
    return ApplyTextPatch(runtime, address, field_length, replacement);
  }
  std::string encoded(replacement);
  encoded[0] = static_cast<char>(static_cast<unsigned char>(encoded[0]) - 0x20);
  return ApplyTextPatch(runtime, address, field_length, encoded);
}

}  // namespace rexmod
