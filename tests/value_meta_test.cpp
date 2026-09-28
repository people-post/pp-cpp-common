#include "common/FiFoMap.h"
#include "common/Value.h"
#include "common/io/Json.h"
#include "common/Serialize.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using pp::common::Array;
using pp::common::ArrayPtr;
using pp::common::FiFoMap;
using pp::common::Null;
using pp::common::Object;
using pp::common::ObjectPtr;
using pp::common::Value;
using pp::common::ValueWire;
using pp::common::asNonNegInt;
using pp::common::io::objectFromJsonString;
using pp::common::io::objectToJsonString;
using pp::common::io::valueFromJsonString;
using pp::common::io::valueToJsonString;
using pp::InputArchive;
using pp::OutputArchive;

namespace {

template <typename T>
std::string archivePack(const T &value) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & value;
  return oss.str();
}

template <typename T>
bool archiveUnpack(const std::string &data, T &value) {
  std::istringstream iss(data, std::ios::binary);
  InputArchive ar(iss);
  ar & value;
  return !ar.failed();
}

std::string archivePackMetaWire(const std::string &key, uint16_t tag,
                                const std::string &payload,
                                uint64_t entryCount = 1) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & entryCount;
  ar & key;
  ar & tag;
  ar & payload;
  return oss.str();
}

} // namespace

TEST(FiFoMapTest, InsertionOrderAndUpdateKeepsPosition) {
  FiFoMap<std::string, int> m;
  m.set("b", 2);
  m.set("a", 1);
  m.set("c", 3);
  m.set("a", 10);

  ASSERT_EQ(m.size(), 3u);
  auto it = m.begin();
  EXPECT_EQ(it->first, "b");
  EXPECT_EQ(it->second, 2);
  ++it;
  EXPECT_EQ(it->first, "a");
  EXPECT_EQ(it->second, 10);
  ++it;
  EXPECT_EQ(it->first, "c");
  EXPECT_EQ(it->second, 3);
}

TEST(FiFoMapTest, EraseReindexes) {
  FiFoMap<std::string, int> m;
  m.set("a", 1);
  m.set("b", 2);
  m.set("c", 3);
  EXPECT_TRUE(m.erase("b"));
  ASSERT_EQ(m.size(), 2u);
  EXPECT_EQ(m.begin()->first, "a");
  EXPECT_EQ(std::next(m.begin())->first, "c");
  EXPECT_EQ(m.at("c"), 3);
}

TEST(MetaTest, RoundTrip_PrimitivesAndString) {
  Object m;
  m.set("i64", int64_t{-42});
  m.set("u64", uint64_t{42});
  m.set("b", true);
  m.set("d", 3.125);
  m.set("s", std::string("hello"));

  Object out;
  ASSERT_TRUE(archiveUnpack(archivePack(m), out));

  EXPECT_EQ(out.getIf<int64_t>("i64").value(), -42);
  EXPECT_EQ(out.getIf<uint64_t>("u64").value(), 42u);
  EXPECT_EQ(out.getIf<bool>("b").value(), true);
  EXPECT_DOUBLE_EQ(out.getIf<double>("d").value(), 3.125);
  EXPECT_EQ(out.getIf<std::string>("s").value(), "hello");
}

TEST(MetaTest, RoundTrip_NestedMeta) {
  Object inner;
  inner.set("x", uint64_t{7});

  Object m;
  m.set("inner", inner);

  Object out;
  ASSERT_TRUE(archiveUnpack(archivePack(m), out));

  auto nestedOpt = out.getMetaIf("inner");
  ASSERT_TRUE(nestedOpt.has_value());
  EXPECT_EQ(nestedOpt->get().getIf<uint64_t>("x").value(), 7u);
}

TEST(MetaTest, InsertionOrder_PreservedOnWire) {
  Object a;
  a.set("b", uint64_t{2});
  a.set("a", uint64_t{1});

  Object b;
  b.set("a", uint64_t{1});
  b.set("b", uint64_t{2});

  // Human-facing tree: wire follows insertion order (not key-sorted).
  EXPECT_NE(archivePack(a), archivePack(b));

  Object out;
  ASSERT_TRUE(archiveUnpack(archivePack(a), out));
  auto it = out.fields().begin();
  EXPECT_EQ(it->first, "b");
  ++it;
  EXPECT_EQ(it->first, "a");
}

TEST(MetaTest, UnknownTag_IsSkipped) {
  constexpr uint16_t kUnknownWireTag = 999;
  const std::string wire =
      archivePackMetaWire("x", kUnknownWireTag, /*payload*/ "abc");

  Object out;
  ASSERT_TRUE(archiveUnpack(wire, out));
  EXPECT_TRUE(out.empty());
}

TEST(MetaTest, DuplicateKeys_AreRejected) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  uint64_t entryCount = 2;
  ar & entryCount;

  const std::string key = "dup";

  {
    std::ostringstream p(std::ios::binary);
    OutputArchive par(p);
    int64_t v = 1;
    par & v;
    std::string payload = p.str();
    ar & key & ValueWire::TAG_I64 & payload;
  }
  {
    std::ostringstream p(std::ios::binary);
    OutputArchive par(p);
    uint64_t v = 2;
    par & v;
    std::string payload = p.str();
    ar & key & ValueWire::TAG_U64 & payload;
  }

  Object out;
  EXPECT_TRUE(archiveUnpack(oss.str(), out));
  EXPECT_TRUE(out.empty());
}

TEST(MetaTest, Json_RoundTrip_PrimitivesAndNested) {
  Object inner;
  inner.set("x", int64_t{9});
  inner.set("flag", false);

  Object m;
  m.set("i64", int64_t{-1});
  m.set("b", true);
  m.set("d", 2.5);
  m.set("s", std::string("ok"));
  m.set("inner", inner);

  const std::string j = objectToJsonString(m);
  Object parsed;
  ASSERT_TRUE(objectFromJsonString(parsed, j));
  EXPECT_EQ(parsed, m);
}

TEST(MetaTest, Json_NullVsAbsent) {
  Object m;
  m.set("n", Null{});

  const std::string j = objectToJsonString(m);
  Object parsed;
  ASSERT_TRUE(objectFromJsonString(parsed, j));
  EXPECT_TRUE(parsed.isNull("n"));
  EXPECT_FALSE(parsed.contains("missing"));
  EXPECT_FALSE(parsed.isNull("missing"));
}

TEST(MetaTest, Json_RoundTrip_Array) {
  std::vector<Value> items;
  items.push_back(int64_t{-1});
  items.push_back(std::string("x"));
  Object inner;
  inner.set("k", int64_t{3});
  items.push_back(std::make_shared<Object>(inner));

  Object m;
  m.set("arr", Object::array(std::move(items)));

  const std::string j = objectToJsonString(m);
  Object parsed;
  ASSERT_TRUE(objectFromJsonString(parsed, j));
  EXPECT_EQ(parsed, m);
}

TEST(MetaTest, RoundTrip_ArrayWire) {
  std::vector<Value> items;
  items.push_back(uint64_t{9});
  items.push_back(Object::array({std::string("a"), std::string("b")}));

  Object m;
  m.set("nested", Object::array(std::move(items)));

  Object out;
  ASSERT_TRUE(archiveUnpack(archivePack(m), out));
  EXPECT_EQ(out, m);
}

TEST(MetaTest, RoundTrip_NullWire) {
  Object m;
  m.set("n", Null{});
  Object out;
  ASSERT_TRUE(archiveUnpack(archivePack(m), out));
  EXPECT_TRUE(out.isNull("n"));
}

TEST(MetaTest, GetOrDefault_Int64) {
  Object m;
  EXPECT_EQ(m.getOrDefault("a", int64_t{-1}), -1);
  m.set("a", int64_t{42});
  EXPECT_EQ(m.getOrDefault("a", int64_t{0}), 42);
  m.set("b", uint64_t{7});
  EXPECT_EQ(m.getOrDefault("b", int64_t{0}), 0);
  m.set("c", std::string("x"));
  EXPECT_EQ(m.getOrDefault("c", int64_t{99}), 99);
}

TEST(MetaTest, GetOrDefault_Uint64) {
  Object m;
  EXPECT_EQ(m.getOrDefault("a", uint64_t{9}), 9u);
  m.set("a", uint64_t{42});
  EXPECT_EQ(m.getOrDefault("a", uint64_t{0}), 42u);
  m.set("b", int64_t{3});
  EXPECT_EQ(m.getOrDefault("b", uint64_t{0}), 0u);
}

TEST(MetaTest, Json_IntegersAreI64) {
  Object m;
  ASSERT_TRUE(objectFromJsonString(m, R"({"pos":1730000000,"neg":-5})"));
  ASSERT_TRUE(m.getIf<int64_t>("pos").has_value());
  EXPECT_EQ(m.getIf<int64_t>("pos").value(), 1730000000);
  EXPECT_FALSE(m.getIf<uint64_t>("pos").has_value());
  ASSERT_TRUE(m.getIf<int64_t>("neg").has_value());
  EXPECT_EQ(m.getIf<int64_t>("neg").value(), -5);
  EXPECT_EQ(m.getNonNegInt("pos").value_or(0), 1730000000u);
  EXPECT_FALSE(m.getNonNegInt("neg").has_value());
}

TEST(MetaTest, SetUIntForJson_StringFallback) {
  Object m;
  const uint64_t big = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ull;
  m.setUIntForJson("small", 42u);
  m.setUIntForJson("big", big);
  EXPECT_EQ(m.getIf<int64_t>("small").value_or(-1), 42);
  ASSERT_TRUE(m.getIf<std::string>("big").has_value());
  EXPECT_EQ(m.getIf<std::string>("big").value(), std::to_string(big));
  EXPECT_EQ(m.getNonNegInt("big").value_or(0), big);
  const std::string j = objectToJsonString(m);
  EXPECT_NE(j.find("\"big\":\"" + std::to_string(big) + "\""), std::string::npos);
}

TEST(MetaTest, AsNonNegInt_DecimalAndHexStrings) {
  EXPECT_EQ(asNonNegInt(Value(std::string("18446744073709551615"))).value_or(0),
            std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(asNonNegInt(Value(std::string("0x10"))).value_or(0), 16u);
  EXPECT_FALSE(asNonNegInt(Value(std::string("-1"))).has_value());
  EXPECT_FALSE(asNonNegInt(Value(std::string("12x"))).has_value());
}

TEST(MetaTest, Json_IntegerOverflowErrors) {
  auto r = valueFromJsonString("9223372036854775808");
  EXPECT_FALSE(r.isOk());
}

TEST(MetaTest, Json_U64WriteOverflowErrors) {
  Value v = uint64_t{std::numeric_limits<uint64_t>::max()};
  auto r = valueToJsonString(v);
  EXPECT_FALSE(r.isOk());
}

TEST(MetaTest, Json_InsertionOrderPreserved) {
  Object m;
  m.set("z", int64_t{1});
  m.set("a", int64_t{2});
  const std::string j = objectToJsonString(m);
  EXPECT_EQ(j, R"({"z":1,"a":2})");
}

TEST(MetaTest, Json_RootArrayAndUtf8) {
  auto r = valueFromJsonString(R"(["hello", "\u4e16\u754c"])");
  ASSERT_TRUE(r.isOk());
  auto *arr = std::get_if<ArrayPtr>(&r.value());
  ASSERT_TRUE(arr && *arr);
  ASSERT_EQ((*arr)->elements.size(), 2u);
  EXPECT_EQ(std::get<std::string>((*arr)->elements[0]), "hello");
  EXPECT_EQ(std::get<std::string>((*arr)->elements[1]), "世界");
}

TEST(MetaTest, Json_RootNull) {
  auto r = valueFromJsonString("null");
  ASSERT_TRUE(r.isOk());
  EXPECT_TRUE(std::holds_alternative<Null>(r.value()));
}

TEST(MetaTest, Json_DeeplyNestedArrayIsRejectedNotCrashed) {
  // 100k levels would blow the stack pre-fix; must fail cleanly instead.
  std::string deep(100000, '[');
  deep.append(100000, ']');
  auto r = valueFromJsonString(deep);
  EXPECT_FALSE(r.isOk());
}

TEST(MetaTest, Json_DeeplyNestedObjectIsRejectedNotCrashed) {
  std::string deep;
  for (int i = 0; i < 100000; ++i) {
    deep += R"({"a":)";
  }
  deep += "null";
  deep.append(100000, '}');
  auto r = valueFromJsonString(deep);
  EXPECT_FALSE(r.isOk());
}

TEST(MetaTest, Json_ModeratelyNestedArrayStillParses) {
  constexpr int kDepth = 200; // under the 256 cap
  std::string deep(kDepth, '[');
  deep += "1";
  deep.append(kDepth, ']');
  auto r = valueFromJsonString(deep);
  ASSERT_TRUE(r.isOk());
}

TEST(MetaTest, Json_Utf8_RejectsOverlongEncoding) {
  // 2-byte overlong encoding of NUL (0xC0 0x80).
  const std::string bad = std::string("\"") + "\xC0\x80" + "\"";
  EXPECT_FALSE(valueFromJsonString(bad).isOk());
}

TEST(MetaTest, Json_Utf8_RejectsEncodedSurrogate) {
  // 3-byte encoding of U+D800 (surrogate): ED A0 80.
  const std::string bad = std::string("\"") + "\xED\xA0\x80" + "\"";
  EXPECT_FALSE(valueFromJsonString(bad).isOk());
}

TEST(MetaTest, Json_Utf8_RejectsCodePointAboveMax) {
  // 4-byte encoding above U+10FFFF: F4 90 80 80.
  const std::string bad = std::string("\"") + "\xF4\x90\x80\x80" + "\"";
  EXPECT_FALSE(valueFromJsonString(bad).isOk());
}

TEST(MetaTest, Json_Utf8_RejectsTruncatedSequence) {
  const std::string bad = std::string("\"") + "\xE4\xB8" + "\""; // missing 3rd byte
  EXPECT_FALSE(valueFromJsonString(bad).isOk());
}

TEST(MetaTest, Json_Utf8_AcceptsValidMultiByte) {
  const std::string ok = std::string("\"") + "\xE4\xB8\x96\xE7\x95\x8C" + "\""; // "世界"
  auto r = valueFromJsonString(ok);
  ASSERT_TRUE(r.isOk());
  EXPECT_EQ(std::get<std::string>(r.value()), "世界");
}

TEST(MetaTest, Json_WhitespaceRejectsFormFeedAndVerticalTab) {
  // JSON only allows space/\t/\n/\r between tokens; \f and \v are not valid
  // whitespace even though std::isspace accepts them.
  EXPECT_FALSE(valueFromJsonString("\f1").isOk());
  EXPECT_FALSE(valueFromJsonString("\v1").isOk());
  EXPECT_TRUE(valueFromJsonString(" \t\r\n1").isOk());
}

TEST(MetaTest, Json_ErrorMessageIsEscapedInOutput) {
  // The error path used to splice the raw error message (which can embed an
  // object key) into the JSON error object without escaping it.
  Object m;
  m.set("bad\"key", Value(std::numeric_limits<double>::quiet_NaN()));
  const std::string j = objectToJsonString(m);
  auto reparsed = valueFromJsonString(j);
  ASSERT_TRUE(reparsed.isOk()) << "objectToJsonString produced invalid JSON: " << j;
}

TEST(MetaTest, Wire_BoolRejectsNonCanonicalByte) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  uint8_t raw = 0xFF; // neither 0 nor 1
  ar & raw;
  std::istringstream iss(oss.str(), std::ios::binary);
  InputArchive iar(iss);
  bool value = false;
  iar & value;
  EXPECT_TRUE(iar.failed());
}

TEST(MetaTest, Wire_MapRejectsDuplicateKey) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  uint64_t entryCount = 2;
  ar & entryCount;
  std::string key = "dup";
  int64_t v1 = 1;
  int64_t v2 = 2;
  ar & key & v1;
  ar & key & v2;

  std::istringstream iss(oss.str(), std::ios::binary);
  InputArchive iar(iss);
  std::map<std::string, int64_t> m;
  iar & m;
  EXPECT_TRUE(iar.failed());
}

TEST(MetaTest, Wire_VectorWithHugeDeclaredSizeFailsInsteadOfOOM) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  uint64_t hugeSize = std::numeric_limits<uint64_t>::max() / 2;
  ar & hugeSize;
  // No actual element bytes follow.

  std::istringstream iss(oss.str(), std::ios::binary);
  InputArchive iar(iss);
  std::vector<uint8_t> v;
  iar & v;
  EXPECT_TRUE(iar.failed());
}

TEST(MetaTest, Wire_StringWithHugeDeclaredSizeFailsInsteadOfOOM) {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  uint64_t hugeSize = 63ull * 1024 * 1024; // under MAX_STRING_SIZE, but far more
                                           // than the bytes actually present
  ar & hugeSize;
  // Only a few bytes actually follow.
  oss.write("ab", 2);

  std::istringstream iss(oss.str(), std::ios::binary);
  InputArchive iar(iss);
  std::string s;
  iar & s;
  EXPECT_TRUE(iar.failed());
}
