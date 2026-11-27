#pragma once

#include "common/CodedFailure.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace pp {

/**
 * The signed extension area (pp-browser projects/release-compatibility/SIGNED_FORMATS.md § 1.2, DECISIONS RC020):
 * the last field of every signed object's signed bytes, a sorted list of typed entries.
 *
 *   area   = u64 BE byte length of the entries, then the entries     (the form inside signed bytes and binary blobs)
 *   entry  = u16 BE type ‖ u32 BE length ‖ value (`length` bytes)
 *   type   = bit 15 (0x8000): critical (must understand); bits 0–14: the id. 0x0000 and 0x8000 are invalid
 *
 * JSON objects carry the entries alone (no u64 length) as standard base64 in `ext_b64`: `DecodeEntries` /
 * `EncodeEntries` are that form, `Decode` / `DecodeAt` / `Encode` / `AppendTo` the area form.
 *
 * Entries are strictly ascending by the whole u16 type (no duplicates), so one set of entries has one encoding, and
 * the entries take at most 4 KiB (`kMaxEntriesBytes`, the u64 length not counted). Values are opaque bytes here.
 * An `ExtArea` only ever holds a valid set (it is built by a decoder or by `FromEntries`), so encoding cannot fail,
 * and a decoded area encodes back to the bytes it came from: unknown entries are kept, not dropped.
 *
 * A reader refuses as malformed, checking in this order (the frozen vectors, pp-specs' vectors/ext_area_vectors.json, hold
 * one case of each):
 *   1. area form only: `LengthPastEnd` if the input is too short for the u64 length; then `TooLarge` if it says
 *      over 4 KiB; then `LengthPastEnd` if it runs past the end of the input.
 *   2. `TooLarge`: the entries are over 4 KiB.
 *   then for each entry, in order:
 *   3. `BytesLeftOver`: 1–5 bytes are left, too few for an entry's header.
 *   4. `InvalidType`: type 0x0000 or 0x8000.
 *   5. `Duplicate` (the type equals the one before it) / `Unsorted` (it is below it).
 *   6. `LengthPastEnd`: the entry's u32 length runs past the end of the entries.
 *   and, for `Decode` of a whole buffer only:
 *   7. `BytesLeftOver`: bytes after the area.
 *
 * Step 5 of the receiver's order (SIGNED_FORMATS § 1.1), after the signature verifies: an unknown critical entry
 * means "update needed" (`FirstUnknownCritical`); unknown non-critical entries are ignored and kept.
 */
class ExtArea {
public:
  enum class Err : int32_t {
    TooLarge = 1,
    LengthPastEnd = 2,
    BytesLeftOver = 3,
    InvalidType = 4,
    Unsorted = 5,
    Duplicate = 6,
  };

  /** One entry: its type (bit 15 = critical) and value. */
  struct Entry {
    uint16_t type = 0;
    std::vector<uint8_t> value;

    bool IsCritical() const { return (type & kCriticalBit) != 0; }
    /** Bits 0–14. */
    uint16_t Id() const { return static_cast<uint16_t>(type & ~kCriticalBit); }
    bool operator==(const Entry&) const = default;
  };

  static constexpr uint16_t kCriticalBit = 0x8000;
  /** The cap on the entries' bytes (headers and values; the area's u64 length not counted). */
  static constexpr size_t kMaxEntriesBytes = 4096;
  /** u16 type + u32 length. */
  static constexpr size_t kEntryHeaderBytes = 6;
  /** The area form's u64 length. */
  static constexpr size_t kLengthBytes = 8;

  /** The empty area (every object's at release 1). */
  ExtArea() = default;

  /**
   * An area of these entries, sorted by type. Refused: an invalid type, the same type twice (`Duplicate`), or more
   * than 4 KiB of entries (`TooLarge`).
   */
  static CodedRoe<ExtArea, Err> FromEntries(std::vector<Entry> entries);

  /** The entries alone, the whole input (the bytes of `ext_b64`). */
  static CodedRoe<ExtArea, Err> DecodeEntries(std::span<const uint8_t> entries);
  /** The area form (u64 length, then the entries) as the whole input: bytes after it are `BytesLeftOver`. */
  static CodedRoe<ExtArea, Err> Decode(std::span<const uint8_t> area);
  /**
   * The area form starting at `*offset` in `bytes`; on success `*offset` moves past it (and is unchanged on a
   * refusal). What follows the area is the caller's to judge (it is the last signed field: the caller refuses bytes
   * after it).
   */
  static CodedRoe<ExtArea, Err> DecodeAt(std::span<const uint8_t> bytes, size_t* offset);

  /** The entries alone (for `ext_b64`); empty for the empty area. */
  std::vector<uint8_t> EncodeEntries() const;
  /** The area form: u64 BE length, then the entries. */
  std::vector<uint8_t> Encode() const;
  /** Append the area form to `out` (building signed bytes). */
  void AppendTo(std::vector<uint8_t>& out) const;

  const std::vector<Entry>& entries() const { return entries_; }
  bool empty() const { return entries_.empty(); }
  /** The entries' encoded size (what the 4 KiB cap counts). */
  size_t EntriesBytes() const;
  /** The entry of exactly this type (critical bit included), or null. */
  const Entry* Find(uint16_t type) const;

  /**
   * The first critical entry whose type is not in `understood` (whole u16 types, critical bit included), or
   * nullopt: a reader that gets a value here reports "update needed" and does not act on the object.
   */
  std::optional<uint16_t> FirstUnknownCritical(std::span<const uint16_t> understood) const;
  bool HasUnknownCritical(std::span<const uint16_t> understood) const {
    return FirstUnknownCritical(understood).has_value();
  }

  /** The refusal's name in the vector file: `too_large`, `length_past_end`, `bytes_left_over`, `invalid_type`,
   * `unsorted`, `duplicate`. */
  static std::string_view ErrName(Err err);

  bool operator==(const ExtArea&) const = default;

private:
  explicit ExtArea(std::vector<Entry> entries) : entries_(std::move(entries)) {}

  std::vector<Entry> entries_;
};

} // namespace pp
