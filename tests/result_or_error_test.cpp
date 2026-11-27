#include "common/ResultOrError.hpp"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

namespace {

struct Point {
  int x = 0;
  int y = 0;
  int Sum() const { return x + y; }
  void Shift() { ++x; }
};

using PointRoe = pp::ResultOrError<Point, pp::RoeErrorBase>;

PointRoe Make() {
  return Point{2, 3};
}

} // namespace

TEST(ResultOrErrorTest, ArrowReachesTheValueOfAConstResult) {
  const PointRoe result = Make();
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(result->x, 2);
  EXPECT_EQ(result->Sum(), 5);
  static_assert(std::is_same_v<decltype(result.operator->()), const Point*>, "a const result gives a const pointer");
}

TEST(ResultOrErrorTest, ArrowOfAMutableResultStillMutates) {
  PointRoe result = Make();
  result->Shift();
  EXPECT_EQ(result->x, 3);
  static_assert(std::is_same_v<decltype(result.operator->()), Point*>);
}

TEST(ResultOrErrorTest, ArrowOnATemporary) {
  EXPECT_EQ(Make()->Sum(), 5);
}
