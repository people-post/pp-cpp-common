// The signed extension area (common/ExtArea.h; pp-browser SIGNED_FORMATS.md § 1.2, § 6 "Extension area").
//
// Frozen vectors: tests/data/ext_area_vectors.json, copied byte for byte by pp-browser, pp-cpp-mesh and the relay
// (packages/shared/src/sign-payload/ext-area.ts). Each case's bytes are written here by hand, not by the encoder, so
// the file checks the codec rather than echoing it. The committed file is checked against what this test builds;
// PP_WRITE_EXT_AREA_VECTORS=<path> writes a fresh one. The file's shape:
//
//   format        "pp-cpp-common ext area vectors v1"
//   layout        the byte layout and the cap, in words and numbers (critical_bit, max_entries_bytes)
//   refusals      every refusal name a case can expect
//   cases[]       entry-level cases; each holds the same entries three ways, and every form gives the same result:
//                   entries_hex   the entries alone (what ext_b64 holds)
//                   ext_b64       standard base64 of entries_hex ("" for no entries; JSON writers omit it then)
//                   area_hex      u64 BE length of the entries, then entries_hex (the form in signed bytes)
//                   result        "ok" or a refusal name
//                   entries[]     when ok: {type (the whole u16, as a number), critical, value_hex}
//   area_cases[]  area-form only (the u64 length and what follows it): area_hex, result
//   unknown_critical[]  the query: an ok case's name, the types a reader understands, and the first critical type
//                   it does not (null when none)

#include "common/ExtArea.h"

#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace {

using pp::ExtArea;
using pp::common::Object;
using pp::common::Value;
using Bytes = std::vector<uint8_t>;

constexpr char kFormat[] = "pp-cpp-common ext area vectors v1";

std::string Hex(const Bytes& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const uint8_t b : bytes) {
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 0xf]);
  }
  return out;
}

Bytes FromHex(const std::string& hex) {
  Bytes out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

std::string Base64(const Bytes& bytes) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const uint32_t n = (uint32_t{bytes[i]} << 16) | (i + 1 < bytes.size() ? uint32_t{bytes[i + 1]} << 8 : 0) |
                       (i + 2 < bytes.size() ? uint32_t{bytes[i + 2]} : 0);
    out.push_back(kAlphabet[(n >> 18) & 0x3f]);
    out.push_back(kAlphabet[(n >> 12) & 0x3f]);
    out.push_back(i + 1 < bytes.size() ? kAlphabet[(n >> 6) & 0x3f] : '=');
    out.push_back(i + 2 < bytes.size() ? kAlphabet[n & 0x3f] : '=');
  }
  return out;
}

void PutBe(Bytes& out, uint64_t value, size_t width) {
  for (size_t i = width; i > 0; --i) {
    out.push_back(static_cast<uint8_t>(value >> (8 * (i - 1))));
  }
}

/** An entry's bytes as written by hand: `length` is the field as written, which a refusal may make wrong. */
Bytes Raw(uint16_t type, uint32_t length, const Bytes& value) {
  Bytes out;
  PutBe(out, type, 2);
  PutBe(out, length, 4);
  out.insert(out.end(), value.begin(), value.end());
  return out;
}

Bytes Raw(uint16_t type, const Bytes& value) {
  return Raw(type, static_cast<uint32_t>(value.size()), value);
}

Bytes Cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes& part : parts) {
    out.insert(out.end(), part.begin(), part.end());
  }
  return out;
}

Bytes AreaOf(const Bytes& entries) {
  Bytes out;
  PutBe(out, entries.size(), 8);
  out.insert(out.end(), entries.begin(), entries.end());
  return out;
}

struct Case {
  std::string name;
  std::string about;
  Bytes entries;
  std::string result; // "ok" or a refusal name
  std::vector<ExtArea::Entry> expected;
};

struct AreaCase {
  std::string name;
  std::string about;
  Bytes area;
  std::string result;
};

struct QueryCase {
  std::string area; // a Case's name
  std::vector<uint16_t> understood;
  std::optional<uint16_t> first_unknown_critical;
};

std::vector<Case> Cases() {
  const Bytes at_cap_value(ExtArea::kMaxEntriesBytes - ExtArea::kEntryHeaderBytes, 0xab);
  const Bytes over_cap_value(ExtArea::kMaxEntriesBytes - ExtArea::kEntryHeaderBytes + 1, 0xab);
  return {
      {"empty", "no entries: every area at release 1", {}, "ok", {}},
      {"one_non_critical",
       "type 0x0001, 3 bytes",
       Raw(0x0001, {0x01, 0x02, 0x03}),
       "ok",
       {{0x0001, {0x01, 0x02, 0x03}}}},
      {"one_critical", "type 0x8001 (critical), 2 bytes", Raw(0x8001, {0xaa, 0xbb}), "ok", {{0x8001, {0xaa, 0xbb}}}},
      {"two_sorted",
       "0x0002 then 0x8001: the order is by the whole u16, so a critical type sorts after every non-critical one",
       Cat({Raw(0x0002, {0x01}), Raw(0x8001, {0x02, 0x03})}),
       "ok",
       {{0x0002, {0x01}}, {0x8001, {0x02, 0x03}}}},
      {"empty_value", "type 0x0005 with a zero-length value", Raw(0x0005, {}), "ok", {{0x0005, {}}}},
      {"at_cap",
       "one entry of exactly 4096 bytes (6 header + 4090 value)",
       Raw(0x0001, at_cap_value),
       "ok",
       {{0x0001, at_cap_value}}},
      {"unsorted", "0x0002 then 0x0001", Cat({Raw(0x0002, {0x01}), Raw(0x0001, {0x02})}), "unsorted", {}},
      {"unsorted_critical_first",
       "0x8001 then 0x0002 (the whole u16 is the order)",
       Cat({Raw(0x8001, {0x01}), Raw(0x0002, {0x02})}),
       "unsorted",
       {}},
      {"duplicate", "0x0001 twice", Cat({Raw(0x0001, {0x01}), Raw(0x0001, {0x02})}), "duplicate", {}},
      {"type_zero", "type 0x0000", Raw(0x0000, {0x01}), "invalid_type", {}},
      {"type_critical_zero", "type 0x8000 (critical bit, id 0)", Raw(0x8000, {0x01}), "invalid_type", {}},
      {"length_past_end", "type 0x0001 says 5 bytes, 2 follow", Raw(0x0001, 5, {0x01, 0x02}), "length_past_end", {}},
      {"length_past_end_huge",
       "type 0x0001 says 0xffffffff bytes",
       Raw(0x0001, 0xffffffffu, {0x01}),
       "length_past_end",
       {}},
      {"bytes_left_over",
       "a whole entry, then 3 bytes: too few for an entry's 6-byte header",
       Cat({Raw(0x0001, {0x01}), Bytes{0x00, 0x02, 0x00}}),
       "bytes_left_over",
       {}},
      {"over_cap",
       "one entry of 4097 bytes (6 header + 4091 value), otherwise well formed",
       Raw(0x0001, over_cap_value),
       "too_large",
       {}},
  };
}

std::vector<AreaCase> AreaCases() {
  Bytes short_length = {0x00, 0x00, 0x00};
  Bytes length_past_end;
  PutBe(length_past_end, 10, 8);
  const Bytes one = Raw(0x0001, {0x01, 0x02, 0x03});
  length_past_end.insert(length_past_end.end(), one.begin(), one.end());
  Bytes huge_length;
  PutBe(huge_length, 0xffffffffffffffffull, 8);
  Bytes over_cap_length;
  PutBe(over_cap_length, ExtArea::kMaxEntriesBytes + 1, 8);
  return {
      {"area_length_cut_short", "3 bytes: too few for the u64 length", short_length, "length_past_end"},
      {"area_length_past_end", "the u64 length says 10, a 9-byte entry follows", length_past_end, "length_past_end"},
      {"area_length_over_cap", "the u64 length says 4097: refused before the input is read", over_cap_length,
       "too_large"},
      {"area_length_huge", "the u64 length says 2^64 - 1", huge_length, "too_large"},
      {"area_bytes_after", "the empty area, then one byte", Bytes{0, 0, 0, 0, 0, 0, 0, 0, 0x00}, "bytes_left_over"},
  };
}

std::vector<QueryCase> QueryCases() {
  return {
      {"empty", {}, std::nullopt},
      {"one_non_critical", {}, std::nullopt},
      {"one_critical", {}, 0x8001},
      {"one_critical", {0x0001}, 0x8001},
      {"one_critical", {0x8001}, std::nullopt},
      {"two_sorted", {}, 0x8001},
      {"two_sorted", {0x8001}, std::nullopt},
  };
}

Value EntriesJson(const std::vector<ExtArea::Entry>& entries) {
  std::vector<Value> out;
  for (const ExtArea::Entry& entry : entries) {
    Object row;
    row.set("type", int64_t{entry.type});
    row.set("critical", entry.IsCritical());
    row.set("value_hex", Hex(entry.value));
    out.push_back(pp::common::ObjectValue(std::move(row)));
  }
  return pp::common::ArrayValue(std::move(out));
}

Object BuildVectors() {
  Object layout;
  layout.set("area", std::string("u64 BE byte length of the entries, then the entries"));
  layout.set("entry", std::string("u16 BE type, u32 BE length, then length bytes of value"));
  layout.set("type", std::string("bit 15 (0x8000) critical, bits 0-14 the id; 0x0000 and 0x8000 invalid"));
  layout.set("order", std::string("strictly ascending by the whole u16 type"));
  layout.set("critical_bit", int64_t{ExtArea::kCriticalBit});
  layout.set("max_entries_bytes", int64_t{ExtArea::kMaxEntriesBytes});

  std::vector<Value> refusals;
  for (const auto err : {ExtArea::Err::TooLarge, ExtArea::Err::LengthPastEnd, ExtArea::Err::BytesLeftOver,
                         ExtArea::Err::InvalidType, ExtArea::Err::Unsorted, ExtArea::Err::Duplicate}) {
    refusals.emplace_back(std::string(ExtArea::ErrName(err)));
  }

  std::vector<Value> cases;
  for (const Case& c : Cases()) {
    Object row;
    row.set("name", c.name);
    row.set("about", c.about);
    row.set("entries_hex", Hex(c.entries));
    row.set("ext_b64", Base64(c.entries));
    row.set("area_hex", Hex(AreaOf(c.entries)));
    row.set("result", c.result);
    if (c.result == "ok") {
      row.set("entries", EntriesJson(c.expected));
    }
    cases.push_back(pp::common::ObjectValue(std::move(row)));
  }

  std::vector<Value> area_cases;
  for (const AreaCase& c : AreaCases()) {
    Object row;
    row.set("name", c.name);
    row.set("about", c.about);
    row.set("area_hex", Hex(c.area));
    row.set("result", c.result);
    area_cases.push_back(pp::common::ObjectValue(std::move(row)));
  }

  std::vector<Value> queries;
  for (const QueryCase& q : QueryCases()) {
    Object row;
    row.set("case", q.area);
    std::vector<Value> understood;
    for (const uint16_t type : q.understood) {
      understood.emplace_back(int64_t{type});
    }
    row.set("understood", pp::common::ArrayValue(std::move(understood)));
    if (q.first_unknown_critical) {
      row.set("first_unknown_critical", int64_t{*q.first_unknown_critical});
    } else {
      row.set("first_unknown_critical", pp::common::Null{});
    }
    queries.push_back(pp::common::ObjectValue(std::move(row)));
  }

  Object out;
  out.set("format", std::string(kFormat));
  out.set("layout", layout);
  out.set("refusals", pp::common::ArrayValue(std::move(refusals)));
  out.set("cases", pp::common::ArrayValue(std::move(cases)));
  out.set("area_cases", pp::common::ArrayValue(std::move(area_cases)));
  out.set("unknown_critical", pp::common::ArrayValue(std::move(queries)));
  return out;
}

std::string ReadFile(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

Object CommittedVectors() {
  const std::string text = ReadFile(PP_EXT_AREA_VECTORS_PATH);
  EXPECT_FALSE(text.empty()) << PP_EXT_AREA_VECTORS_PATH;
  return pp::common::TryParseObject(text).value_or(Object{});
}

std::string Str(const Object& row, const char* key) {
  return row.getString(key).value_or("");
}

const pp::common::Array& Rows(const Object& vectors, const char* key) {
  static const pp::common::Array kNone;
  const pp::common::Array* rows = vectors.getArray(key);
  EXPECT_NE(rows, nullptr) << key;
  return rows != nullptr ? *rows : kNone;
}

/** "ok" or the refusal's name. */
std::string ResultOf(const pp::CodedRoe<ExtArea, ExtArea::Err>& decoded) {
  return decoded ? "ok" : std::string(ExtArea::ErrName(decoded.error().GetCode()));
}

ExtArea Area(std::initializer_list<ExtArea::Entry> entries) {
  return ExtArea::FromEntries(entries).value();
}

// ---- The frozen vectors ----

TEST(ExtAreaVectorsTest, WriteAFreshFileWhenAsked) {
  const char* out = std::getenv("PP_WRITE_EXT_AREA_VECTORS");
  if (out == nullptr || *out == '\0') {
    GTEST_SKIP() << "set PP_WRITE_EXT_AREA_VECTORS=<path> to write the vectors";
  }
  std::ofstream file(out, std::ios::binary);
  file << pp::common::DumpJson(BuildVectors(), 2) << "\n";
  ASSERT_TRUE(file.good()) << out;
}

TEST(ExtAreaVectorsTest, TheCommittedFileIsWhatThisTestBuilds) {
  EXPECT_EQ(ReadFile(PP_EXT_AREA_VECTORS_PATH), pp::common::DumpJson(BuildVectors(), 2) + "\n")
      << "regenerate with PP_WRITE_EXT_AREA_VECTORS=" << PP_EXT_AREA_VECTORS_PATH;
}

TEST(ExtAreaVectorsTest, EveryCaseDecodesAsTheFileSaysInBothForms) {
  const Object vectors = CommittedVectors();
  EXPECT_EQ(Str(vectors, "format"), kFormat);
  const auto& cases = Rows(vectors, "cases");
  EXPECT_EQ(cases.elements.size(), Cases().size());
  for (const Value& value : cases.elements) {
    const Object* row = pp::common::asObject(value);
    ASSERT_NE(row, nullptr);
    const std::string name = Str(*row, "name");
    const Bytes entries = FromHex(Str(*row, "entries_hex"));
    const Bytes area = FromHex(Str(*row, "area_hex"));
    EXPECT_EQ(Base64(entries), Str(*row, "ext_b64")) << name;
    EXPECT_EQ(area, AreaOf(entries)) << name;

    auto from_entries = ExtArea::DecodeEntries(entries);
    auto from_area = ExtArea::Decode(area);
    EXPECT_EQ(ResultOf(from_entries), Str(*row, "result")) << name;
    EXPECT_EQ(ResultOf(from_area), Str(*row, "result")) << name;
    if (!from_entries || !from_area) {
      continue;
    }
    EXPECT_EQ(*from_entries, *from_area) << name;
    EXPECT_EQ(from_entries->EncodeEntries(), entries) << name << ": one set of entries has one encoding";
    EXPECT_EQ(from_area->Encode(), area) << name;

    const pp::common::Array* expected = row->getArray("entries");
    ASSERT_NE(expected, nullptr) << name;
    ASSERT_EQ(expected->elements.size(), from_entries->entries().size()) << name;
    for (size_t i = 0; i < expected->elements.size(); ++i) {
      const Object* want = pp::common::asObject(expected->elements[i]);
      ASSERT_NE(want, nullptr);
      const ExtArea::Entry& got = from_entries->entries()[i];
      EXPECT_EQ(want->getNonNegInt("type"), got.type) << name;
      EXPECT_EQ(want->getIf<bool>("critical"), got.IsCritical()) << name;
      EXPECT_EQ(Str(*want, "value_hex"), Hex(got.value)) << name;
    }
  }
}

TEST(ExtAreaVectorsTest, EveryAreaCaseDecodesAsTheFileSays) {
  const Object vectors = CommittedVectors();
  const auto& cases = Rows(vectors, "area_cases");
  EXPECT_EQ(cases.elements.size(), AreaCases().size());
  for (const Value& value : cases.elements) {
    const Object* row = pp::common::asObject(value);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(ResultOf(ExtArea::Decode(FromHex(Str(*row, "area_hex")))), Str(*row, "result")) << Str(*row, "name");
  }
}

TEST(ExtAreaVectorsTest, TheUnknownCriticalQueryAnswersAsTheFileSays) {
  const Object vectors = CommittedVectors();
  std::vector<std::pair<std::string, Bytes>> named;
  for (const Value& value : Rows(vectors, "cases").elements) {
    const Object* row = pp::common::asObject(value);
    named.emplace_back(Str(*row, "name"), FromHex(Str(*row, "entries_hex")));
  }
  const auto& queries = Rows(vectors, "unknown_critical");
  EXPECT_EQ(queries.elements.size(), QueryCases().size());
  for (const Value& value : queries.elements) {
    const Object* row = pp::common::asObject(value);
    ASSERT_NE(row, nullptr);
    const std::string name = Str(*row, "case");
    const auto found = std::find_if(named.begin(), named.end(), [&](const auto& n) { return n.first == name; });
    ASSERT_NE(found, named.end()) << name;
    auto area = ExtArea::DecodeEntries(found->second);
    ASSERT_TRUE(static_cast<bool>(area)) << name;
    std::vector<uint16_t> understood;
    for (const Value& type : row->getArray("understood")->elements) {
      ASSERT_TRUE(std::holds_alternative<int64_t>(type)) << name;
      understood.push_back(static_cast<uint16_t>(std::get<int64_t>(type)));
    }
    const std::optional<uint64_t> want = row->getNonNegInt("first_unknown_critical");
    const std::optional<uint16_t> got = area->FirstUnknownCritical(understood);
    EXPECT_EQ(got.has_value(), want.has_value()) << name;
    if (got && want) {
      EXPECT_EQ(*got, *want) << name;
    }
    EXPECT_EQ(area->HasUnknownCritical(understood), want.has_value()) << name;
  }
}

// ---- Beyond the vectors ----

TEST(ExtAreaTest, TheEmptyAreaIsEightZeroBytesAndNoEntries) {
  const ExtArea empty;
  EXPECT_TRUE(empty.empty());
  EXPECT_EQ(empty.EntriesBytes(), 0u);
  EXPECT_EQ(empty.Encode(), Bytes(8, 0x00));
  EXPECT_TRUE(empty.EncodeEntries().empty());
  EXPECT_EQ(ExtArea::DecodeEntries({}).value(), empty);
}

TEST(ExtAreaTest, FromEntriesSortsAndRoundTrips) {
  auto area = ExtArea::FromEntries({{0x8003, {0x09}}, {0x0001, {}}, {0x0002, {0x01, 0x02}}});
  ASSERT_TRUE(static_cast<bool>(area));
  ASSERT_EQ(area->entries().size(), 3u);
  EXPECT_EQ(area->entries()[0].type, 0x0001);
  EXPECT_EQ(area->entries()[1].type, 0x0002);
  EXPECT_EQ(area->entries()[2].type, 0x8003);
  EXPECT_EQ(area->entries()[2].Id(), 0x0003);

  const Bytes encoded = area->Encode();
  EXPECT_EQ(Hex(encoded),
            "0000000000000015"
            "000100000000"
            "0002000000020102"
            "80030000000109");
  EXPECT_EQ(ExtArea::Decode(encoded).value(), *area);
  EXPECT_EQ(ExtArea::DecodeEntries(area->EncodeEntries()).value(), *area);
  EXPECT_EQ(area->EntriesBytes(), area->EncodeEntries().size());
}

TEST(ExtAreaTest, FromEntriesRefusesWhatADecoderWouldRefuse) {
  EXPECT_EQ(ExtArea::FromEntries({{0x0000, {}}}).error().GetCode(), ExtArea::Err::InvalidType);
  EXPECT_EQ(ExtArea::FromEntries({{0x8000, {}}}).error().GetCode(), ExtArea::Err::InvalidType);
  EXPECT_EQ(ExtArea::FromEntries({{0x0007, {0x01}}, {0x0007, {0x02}}}).error().GetCode(), ExtArea::Err::Duplicate);
  EXPECT_TRUE(static_cast<bool>(ExtArea::FromEntries({{0x0007, {}}, {0x8007, {}}})))
      << "0x0007 and 0x8007 are two types";
}

TEST(ExtAreaTest, TheCapIsFourKibOfEntriesHeadersIncluded) {
  const size_t header = ExtArea::kEntryHeaderBytes;
  auto at_cap = ExtArea::FromEntries({{0x0001, Bytes(2000, 0x01)}, {0x0002, Bytes(4096 - 2000 - 2 * header, 0)}});
  ASSERT_TRUE(static_cast<bool>(at_cap));
  EXPECT_EQ(at_cap->EntriesBytes(), 4096u);
  EXPECT_EQ(at_cap->Encode().size(), 8u + 4096u) << "the u64 length is not counted";
  EXPECT_TRUE(static_cast<bool>(ExtArea::Decode(at_cap->Encode())));

  const auto over =
      ExtArea::FromEntries({{0x0001, Bytes(2000, 0x01)}, {0x0002, Bytes(4096 - 2000 - 2 * header + 1, 0)}});
  EXPECT_EQ(over.error().GetCode(), ExtArea::Err::TooLarge);
  EXPECT_EQ(ExtArea::FromEntries({{0x0001, Bytes(100000, 0)}}).error().GetCode(), ExtArea::Err::TooLarge);

  // Many empty entries add up: 683 headers = 4098 bytes.
  std::vector<ExtArea::Entry> many;
  for (uint16_t type = 1; type <= 683; ++type) {
    many.push_back({type, {}});
  }
  EXPECT_EQ(ExtArea::FromEntries(many).error().GetCode(), ExtArea::Err::TooLarge);
  many.pop_back();
  EXPECT_TRUE(static_cast<bool>(ExtArea::FromEntries(many))) << "682 headers = 4092 bytes";
}

TEST(ExtAreaTest, TheUnknownCriticalQueryLooksOnlyAtCriticalTypes) {
  const ExtArea area = Area({{0x0001, {0x01}}, {0x0009, {}}, {0x8002, {0x02}}, {0x8004, {}}});
  const std::array<uint16_t, 0> none{};
  EXPECT_EQ(area.FirstUnknownCritical(none), 0x8002) << "the first, in type order";
  const std::array<uint16_t, 1> knows_8002 = {0x8002};
  EXPECT_EQ(area.FirstUnknownCritical(knows_8002), 0x8004);
  const std::array<uint16_t, 2> knows_both = {0x8004, 0x8002};
  EXPECT_FALSE(area.HasUnknownCritical(knows_both)) << "unknown non-critical 0x0001 and 0x0009 are ignored";
  const std::array<uint16_t, 2> ids_only = {0x0002, 0x0004};
  EXPECT_TRUE(area.HasUnknownCritical(ids_only)) << "types are whole u16s: 0x0002 is not 0x8002";
  EXPECT_FALSE(Area({{0x0001, {}}}).HasUnknownCritical(none));
}

TEST(ExtAreaTest, UnknownEntriesAreKeptAndFound) {
  const ExtArea area = Area({{0x0001, {0x01}}, {0x7fff, {0xff}}, {0xffff, {0xee}}});
  ASSERT_NE(area.Find(0x7fff), nullptr);
  EXPECT_EQ(area.Find(0x7fff)->value, Bytes{0xff});
  ASSERT_NE(area.Find(0xffff), nullptr) << "the highest type is valid";
  EXPECT_EQ(area.Find(0x8001), nullptr);
  EXPECT_EQ(area.Find(0x0002), nullptr);
  EXPECT_EQ(ExtArea::Decode(area.Encode()).value().Encode(), area.Encode()) << "a stored object keeps its bytes";
}

TEST(ExtAreaTest, DecodeAtReadsAnAreaInsideLargerBytesAndMovesTheOffset) {
  Bytes signed_bytes = {'d', 'o', 'm', 0x00, 0x05};
  const ExtArea area = Area({{0x0003, {0x0a, 0x0b}}});
  area.AppendTo(signed_bytes);
  signed_bytes.push_back(0x99); // a field after the area (a blob's next field)
  size_t offset = 5;
  auto decoded = ExtArea::DecodeAt(signed_bytes, &offset);
  ASSERT_TRUE(static_cast<bool>(decoded));
  EXPECT_EQ(*decoded, area);
  EXPECT_EQ(offset, signed_bytes.size() - 1);

  size_t past = signed_bytes.size();
  EXPECT_EQ(ExtArea::DecodeAt(signed_bytes, &past).error().GetCode(), ExtArea::Err::LengthPastEnd);
  EXPECT_EQ(past, signed_bytes.size()) << "unchanged on a refusal";
  size_t bad = 6; // mid-length
  EXPECT_FALSE(static_cast<bool>(ExtArea::DecodeAt(signed_bytes, &bad)));
  EXPECT_EQ(bad, 6u);
}

TEST(ExtAreaTest, EveryTruncationOfAValidAreaIsRefused) {
  const Bytes full = Area({{0x0001, {0x01, 0x02}}, {0x8002, {0x03}}}).Encode();
  for (size_t n = 0; n < full.size(); ++n) {
    EXPECT_FALSE(static_cast<bool>(ExtArea::Decode(Bytes(full.begin(), full.begin() + n)))) << n;
  }
  const Bytes entries(full.begin() + 8, full.end());
  for (size_t n = 1; n < entries.size(); ++n) {
    auto decoded = ExtArea::DecodeEntries(Bytes(entries.begin(), entries.begin() + n));
    if (n == 8) {
      // Exactly the first entry: a valid area of one entry.
      EXPECT_TRUE(static_cast<bool>(decoded));
      continue;
    }
    EXPECT_FALSE(static_cast<bool>(decoded)) << n;
  }
}

} // namespace
