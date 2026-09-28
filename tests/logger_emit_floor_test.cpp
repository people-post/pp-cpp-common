#include "common/Logger.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

namespace {

using pp::logging::Handler;
using pp::logging::Level;
using pp::logging::Logger;

struct CaptureHandler : Handler {
  void emit(Level level, const std::string& /*loggerName*/,
            const std::string& message) override {
    last_level = level;
    last_message = message;
    ++count;
  }

  Level last_level = Level::DEBUG;
  std::string last_message;
  int count = 0;
};

class EmitFloorGuard {
public:
  EmitFloorGuard() : previous_(pp::logging::getEmitFloor()) {}
  ~EmitFloorGuard() { pp::logging::setEmitFloor(previous_); }

  EmitFloorGuard(const EmitFloorGuard&) = delete;
  EmitFloorGuard& operator=(const EmitFloorGuard&) = delete;

private:
  Level previous_;
};

} // namespace

TEST(LoggerEmitFloorTest, PromotesAfterFilterKeepsOriginalThreshold) {
  EmitFloorGuard restore;
  pp::logging::setEmitFloor(Level::WARNING);

  auto handler = std::make_shared<CaptureHandler>();
  Logger log = pp::logging::getLogger("test.emit_floor.promote");
  log.setLevel(Level::INFO);
  log.setPropagate(false);
  log.addHandler(handler);

  log.info << "visible";
  ASSERT_EQ(handler->count, 1);
  EXPECT_EQ(handler->last_level, Level::WARNING);
  EXPECT_NE(handler->last_message.find("[WARNING]"), std::string::npos);
  EXPECT_NE(handler->last_message.find("visible"), std::string::npos);

  // DEBUG still filtered by logger level (INFO); floor must not bypass that.
  log.debug << "hidden";
  EXPECT_EQ(handler->count, 1);
}

namespace {

// Returns a Logger built from a copy of a local Logger whose original goes out
// of scope before this function returns.
Logger MakeLoggerViaCopy(std::shared_ptr<CaptureHandler> handler) {
  Logger original = pp::logging::getLogger("test.logger_copy.independent");
  original.setPropagate(false);
  original.addHandler(std::move(handler));
  Logger copy = original; // exercises Logger's copy constructor
  return copy;
} // `original` is destroyed here.

} // namespace

TEST(LoggerTest, CopyProxiesStayValidAfterSourceDestroyed) {
  auto handler = std::make_shared<CaptureHandler>();
  Logger copy = MakeLoggerViaCopy(handler);

  // Before the fix, LogProxy members were copied verbatim and kept pointing
  // at the (now-destroyed) `original` Logger inside MakeLoggerViaCopy, so
  // this would log through a dangling pointer instead of through `copy`.
  copy.setLevel(Level::INFO);
  copy.info << "via-copy";

  ASSERT_EQ(handler->count, 1);
  EXPECT_NE(handler->last_message.find("via-copy"), std::string::npos);
}

TEST(LoggerTest, MoveProxiesStayValidAfterSourceDestroyed) {
  auto handler = std::make_shared<CaptureHandler>();
  Logger original = pp::logging::getLogger("test.logger_move.independent");
  original.setPropagate(false);
  original.setLevel(Level::INFO);
  original.addHandler(handler);

  Logger moved = std::move(original);
  moved.info << "via-move";

  ASSERT_EQ(handler->count, 1);
  EXPECT_NE(handler->last_message.find("via-move"), std::string::npos);
}

TEST(LoggerEmitFloorTest, DefaultFloorIsNoOp) {
  EmitFloorGuard restore;
  pp::logging::setEmitFloor(pp::logging::kLevelDebug);

  auto handler = std::make_shared<CaptureHandler>();
  Logger log = pp::logging::getLogger("test.emit_floor.noop");
  log.setLevel(Level::INFO);
  log.setPropagate(false);
  log.addHandler(handler);

  log.info << "plain";
  ASSERT_EQ(handler->count, 1);
  EXPECT_EQ(handler->last_level, Level::INFO);
  EXPECT_NE(handler->last_message.find("[INFO]"), std::string::npos);
}
