#include "common/CodedFailure.h"
#include "common/SettledWait.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST(ValueJsonTest, ParsesReadsAndDeepMerges) {
  auto parsed = pp::common::ParseObject(R"({"a":{"x":1,"y":"keep"},"b":true})");
  ASSERT_TRUE(static_cast<bool>(parsed));
  EXPECT_EQ(pp::common::ObjectBool(*parsed, "b"), true);
  EXPECT_FALSE(pp::common::TryParseObject("not json"));
  auto overlay = pp::common::ParseObject(R"({"a":{"x":2},"c":"new"})");
  ASSERT_TRUE(static_cast<bool>(overlay));
  pp::common::DeepMergeObject(*parsed, *overlay);
  const pp::common::Object* a = pp::common::ObjectChild(*parsed, "a");
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(pp::common::ObjectInt64(*a, "x"), 2) << "objects merge";
  EXPECT_EQ(pp::common::ObjectString(*a, "y"), "keep") << "untouched keys stay";
  EXPECT_EQ(pp::common::ObjectString(*parsed, "c"), "new");
  EXPECT_EQ(pp::common::DumpJson(pp::common::Object{}), "{}");
}

enum class TestErr : int32_t { Ok = 0, Refused = 3 };

TEST(CodedFailureTest, CarriesItsCodeAndChainsCauses) {
  const auto failure = pp::CodedFailure<TestErr>::Of(TestErr::Refused, "no");
  EXPECT_EQ(failure.GetCode(), TestErr::Refused);
  pp::CodedRoe<int, TestErr> roe(failure);
  EXPECT_FALSE(static_cast<bool>(roe));
  EXPECT_EQ(pp::detail::AppendFrom("open failed", "link", "timeout"), "open failed [link: timeout]");
  EXPECT_EQ(pp::detail::AppendFrom("open failed", "link", ""), "open failed");
}

TEST(SettledWaitTest, SettlesOnceFromAnyThread) {
  pp::SettledWait<int> wait;
  std::thread worker([wait]() { wait.Finish(7); });
  auto result = wait.Wait(5s, pp::Error("timed out"));
  worker.join();
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(*result, 7);
  EXPECT_FALSE(wait.Finish(8)) << "once";

  pp::SettledWait<int> late;
  auto timed_out = late.Wait(1ms, pp::Error("timed out"));
  EXPECT_FALSE(static_cast<bool>(timed_out));
  EXPECT_FALSE(late.Finish(9)) << "a late callback is dropped";
}
