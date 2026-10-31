#include "common/Logger.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

using pp::logging::Handler;
using pp::logging::Level;
using pp::logging::Logger;

struct CaptureHandler : Handler {
  void emit(Level /*level*/, const std::string& /*loggerName*/, const std::string& message) override {
    last_message = message;
    ++count;
  }
  std::string last_message;
  int count = 0;
};

} // namespace

TEST(LoggerLineBreaksTest, ABodyWithLineBreaksStaysOneRecord) {
  auto handler = std::make_shared<CaptureHandler>();
  Logger log = pp::logging::getLogger("test.line_breaks");
  log.setLevel(Level::DEBUG);
  log.setPropagate(false);
  log.addHandler(handler);

  log.info << "first\n[2026-10-28 09:00:00.000] [INFO] [Net] forged\r\nlast";
  ASSERT_EQ(handler->count, 1);
  EXPECT_EQ(handler->last_message.find('\n'), std::string::npos);
  EXPECT_EQ(handler->last_message.find('\r'), std::string::npos);
  EXPECT_NE(handler->last_message.find("first\\n[2026-10-28 09:00:00.000] [INFO] [Net] forged\\r\\nlast"),
            std::string::npos);
}

TEST(LoggerLineBreaksTest, PlainBodiesAreUnchanged) {
  auto handler = std::make_shared<CaptureHandler>();
  Logger log = pp::logging::getLogger("test.line_breaks.plain");
  log.setLevel(Level::DEBUG);
  log.setPropagate(false);
  log.addHandler(handler);

  log.info << "plain body with [brackets] and \\ backslash";
  ASSERT_EQ(handler->count, 1);
  EXPECT_NE(handler->last_message.find("plain body with [brackets] and \\ backslash"), std::string::npos);
}
