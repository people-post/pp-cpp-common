#include "common/ExtArea.h"

#include <algorithm>
#include <string>

namespace pp {

namespace {

using Failure = CodedFailure<ExtArea::Err>;

uint64_t ReadBe(std::span<const uint8_t> bytes, size_t at, size_t width) {
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) {
    value = (value << 8) | bytes[at + i];
  }
  return value;
}

void WriteBe(std::vector<uint8_t>& out, uint64_t value, size_t width) {
  for (size_t i = width; i > 0; --i) {
    out.push_back(static_cast<uint8_t>(value >> (8 * (i - 1))));
  }
}

bool IsValidType(uint16_t type) {
  return (type & static_cast<uint16_t>(~ExtArea::kCriticalBit)) != 0;
}

std::string Hex16(uint16_t type) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out = "0x";
  for (int shift = 12; shift >= 0; shift -= 4) {
    out.push_back(kDigits[(type >> shift) & 0xf]);
  }
  return out;
}

} // namespace

CodedRoe<ExtArea, ExtArea::Err> ExtArea::FromEntries(std::vector<Entry> entries) {
  std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.type < b.type; });
  size_t total = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    const Entry& entry = entries[i];
    if (!IsValidType(entry.type)) {
      return Failure::Of(Err::InvalidType, "extension type " + Hex16(entry.type) + " is invalid");
    }
    if (i > 0 && entries[i - 1].type == entry.type) {
      return Failure::Of(Err::Duplicate, "extension type " + Hex16(entry.type) + " twice");
    }
    if (entry.value.size() > kMaxEntriesBytes) {
      return Failure::Of(Err::TooLarge, "extension entries over 4 KiB");
    }
    total += kEntryHeaderBytes + entry.value.size();
    if (total > kMaxEntriesBytes) {
      return Failure::Of(Err::TooLarge, "extension entries over 4 KiB");
    }
  }
  return ExtArea(std::move(entries));
}

CodedRoe<ExtArea, ExtArea::Err> ExtArea::DecodeEntries(std::span<const uint8_t> bytes) {
  if (bytes.size() > kMaxEntriesBytes) {
    return Failure::Of(Err::TooLarge, "extension entries are " + std::to_string(bytes.size()) + " bytes, over 4 KiB");
  }
  std::vector<Entry> entries;
  size_t at = 0;
  while (at < bytes.size()) {
    const size_t left = bytes.size() - at;
    if (left < kEntryHeaderBytes) {
      return Failure::Of(Err::BytesLeftOver, std::to_string(left) + " bytes left over after the extension entries");
    }
    const auto type = static_cast<uint16_t>(ReadBe(bytes, at, 2));
    const uint64_t length = ReadBe(bytes, at + 2, 4);
    if (!IsValidType(type)) {
      return Failure::Of(Err::InvalidType, "extension type " + Hex16(type) + " is invalid");
    }
    if (!entries.empty()) {
      const uint16_t before = entries.back().type;
      if (type == before) {
        return Failure::Of(Err::Duplicate, "extension type " + Hex16(type) + " twice");
      }
      if (type < before) {
        return Failure::Of(Err::Unsorted, "extension type " + Hex16(type) + " after " + Hex16(before));
      }
    }
    at += kEntryHeaderBytes;
    if (length > bytes.size() - at) {
      return Failure::Of(Err::LengthPastEnd, "extension " + Hex16(type) + " length " + std::to_string(length) +
                                                 " runs past the end of the area");
    }
    const auto value_begin = bytes.begin() + static_cast<std::ptrdiff_t>(at);
    entries.push_back(
        Entry{type, std::vector<uint8_t>(value_begin, value_begin + static_cast<std::ptrdiff_t>(length))});
    at += static_cast<size_t>(length);
  }
  return ExtArea(std::move(entries));
}

CodedRoe<ExtArea, ExtArea::Err> ExtArea::DecodeAt(std::span<const uint8_t> bytes, size_t* offset) {
  const size_t start = offset == nullptr ? 0 : *offset;
  if (start > bytes.size() || bytes.size() - start < kLengthBytes) {
    return Failure::Of(Err::LengthPastEnd, "the extension area's length runs past the end");
  }
  const uint64_t length = ReadBe(bytes, start, kLengthBytes);
  if (length > kMaxEntriesBytes) {
    return Failure::Of(Err::TooLarge, "extension entries are " + std::to_string(length) + " bytes, over 4 KiB");
  }
  const size_t entries_at = start + kLengthBytes;
  if (length > bytes.size() - entries_at) {
    return Failure::Of(Err::LengthPastEnd,
                       "the extension area's length " + std::to_string(length) + " runs past the end");
  }
  auto area = DecodeEntries(bytes.subspan(entries_at, static_cast<size_t>(length)));
  if (area && offset != nullptr) {
    *offset = entries_at + static_cast<size_t>(length);
  }
  return area;
}

CodedRoe<ExtArea, ExtArea::Err> ExtArea::Decode(std::span<const uint8_t> bytes) {
  size_t offset = 0;
  auto area = DecodeAt(bytes, &offset);
  if (area && offset != bytes.size()) {
    return Failure::Of(Err::BytesLeftOver,
                       std::to_string(bytes.size() - offset) + " bytes left over after the extension area");
  }
  return area;
}

size_t ExtArea::EntriesBytes() const {
  size_t total = 0;
  for (const Entry& entry : entries_) {
    total += kEntryHeaderBytes + entry.value.size();
  }
  return total;
}

std::vector<uint8_t> ExtArea::EncodeEntries() const {
  std::vector<uint8_t> out;
  out.reserve(EntriesBytes());
  for (const Entry& entry : entries_) {
    WriteBe(out, entry.type, 2);
    WriteBe(out, entry.value.size(), 4);
    out.insert(out.end(), entry.value.begin(), entry.value.end());
  }
  return out;
}

void ExtArea::AppendTo(std::vector<uint8_t>& out) const {
  WriteBe(out, EntriesBytes(), kLengthBytes);
  const std::vector<uint8_t> entries = EncodeEntries();
  out.insert(out.end(), entries.begin(), entries.end());
}

std::vector<uint8_t> ExtArea::Encode() const {
  std::vector<uint8_t> out;
  out.reserve(kLengthBytes + EntriesBytes());
  AppendTo(out);
  return out;
}

const ExtArea::Entry* ExtArea::Find(uint16_t type) const {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), type,
                                   [](const Entry& entry, uint16_t wanted) { return entry.type < wanted; });
  return it != entries_.end() && it->type == type ? &*it : nullptr;
}

std::optional<uint16_t> ExtArea::FirstUnknownCritical(std::span<const uint16_t> understood) const {
  for (const Entry& entry : entries_) {
    if (entry.IsCritical() && std::find(understood.begin(), understood.end(), entry.type) == understood.end()) {
      return entry.type;
    }
  }
  return std::nullopt;
}

std::string_view ExtArea::ErrName(Err err) {
  switch (err) {
    case Err::TooLarge:
      return "too_large";
    case Err::LengthPastEnd:
      return "length_past_end";
    case Err::BytesLeftOver:
      return "bytes_left_over";
    case Err::InvalidType:
      return "invalid_type";
    case Err::Unsorted:
      return "unsorted";
    case Err::Duplicate:
      return "duplicate";
  }
  return "unknown";
}

} // namespace pp
