#include "gtest/gtest.h"

extern "C" {

#include "anchor/console/console.h"

};

#include <cstring>
#include <string>

#define UP_ARROW_SEQUENCE   "\x1b[A"
#define DOWN_ARROW_SEQUENCE "\x1b[B"

extern std::vector<char> g_console_write_buffer;

#define EXPECT_WRITE_BUFFER(str) do { \
    g_console_write_buffer.push_back('\0'); \
    const char* write_buffer_str = g_console_write_buffer.data(); \
    EXPECT_STREQ(write_buffer_str, str); \
    g_console_write_buffer.clear(); \
  } while (0)

static void process_line(const char* str) {
  console_process((const uint8_t*)str, (uint32_t)strlen(str));
}

static std::string recalled(const char* entry, uint32_t erased_chars = 0) {
  return std::string(erased_chars, '\b') + std::string(erased_chars, ' ') + "\r> " + entry;
}

TEST(ConsoleHistoryTest, TestUpArrowRecallsPreviousLine) {
  process_line("say_hi\n");
  EXPECT_WRITE_BUFFER("say_hi\nhi\n> ");

  process_line(UP_ARROW_SEQUENCE);
  EXPECT_WRITE_BUFFER(recalled("say_hi").c_str());

  process_line("\n");
  EXPECT_WRITE_BUFFER("\nhi\n> ");
}

TEST(ConsoleHistoryTest, TestUpArrowWalksBackwards) {
  process_line("say_hi\n");
  g_console_write_buffer.clear();
  process_line("say_bye\n");
  g_console_write_buffer.clear();

  process_line(UP_ARROW_SEQUENCE);
  EXPECT_WRITE_BUFFER(recalled("say_bye").c_str());

  process_line(UP_ARROW_SEQUENCE);
  EXPECT_WRITE_BUFFER(recalled("say_hi", strlen("say_bye") - strlen("say_hi")).c_str());

  // already at the oldest entry
  process_line(UP_ARROW_SEQUENCE);
  EXPECT_WRITE_BUFFER("");

  process_line("\n");
  EXPECT_WRITE_BUFFER("\nhi\n> ");
}

TEST(ConsoleHistoryTest, TestDownArrowReturnsToAnEmptyLine) {
  process_line("say_hi\n");
  g_console_write_buffer.clear();

  process_line(UP_ARROW_SEQUENCE);
  EXPECT_WRITE_BUFFER(recalled("say_hi").c_str());

  process_line(DOWN_ARROW_SEQUENCE);
  EXPECT_WRITE_BUFFER(recalled("", strlen("say_hi")).c_str());

  // and there is nothing below the empty line
  process_line(DOWN_ARROW_SEQUENCE);
  EXPECT_WRITE_BUFFER("");

  process_line("\n");
  EXPECT_WRITE_BUFFER("\n> ");
}

TEST(ConsoleHistoryTest, TestConsecutiveDuplicatesAreNotStoredTwice) {
  // the console is a singleton shared by every test in this file
  process_line("say_bye\n");
  process_line("minimal\n");
  process_line("minimal\n");
  g_console_write_buffer.clear();

  process_line(UP_ARROW_SEQUENCE);
  EXPECT_WRITE_BUFFER(recalled("minimal").c_str());

  process_line(UP_ARROW_SEQUENCE);
  EXPECT_WRITE_BUFFER(recalled("say_bye").c_str());

  process_line("\n");
  EXPECT_WRITE_BUFFER("\nhi\n> ");
}

TEST(ConsoleHistoryTest, TestOnlyTheFirstTokenBoundaryAddsToHistory) {
  process_line("add 1 2\n");
  EXPECT_WRITE_BUFFER("add 1 2\n1 + 2 = 3\n> ");

  process_line(UP_ARROW_SEQUENCE);
  EXPECT_WRITE_BUFFER(recalled("add 1 2").c_str());

  process_line("\n");
  EXPECT_WRITE_BUFFER("\n1 + 2 = 3\n> ");
}

TEST(ConsoleHistoryTest, TestUnknownCommandIsStillAddedToHistory) {
  process_line("invalid\n");
  EXPECT_WRITE_BUFFER("invalid\nERROR: Command not found (invalid)\n> ");

  process_line(UP_ARROW_SEQUENCE);
  EXPECT_WRITE_BUFFER(recalled("invalid").c_str());

  process_line("\n");
  EXPECT_WRITE_BUFFER("\nERROR: Command not found (invalid)\n> ");
}

TEST(ConsoleHistoryTest, TestRingBufferDropsTheOldestEntry) {
  process_line("say_hi\n");
  process_line("say_bye\n");
  process_line("minimal\n");
  process_line("add 1 2\n");
  process_line("add 3 4\n");
  g_console_write_buffer.clear();

  const char* expected[] = {"add 3 4", "add 1 2", "minimal", "say_bye"};
  uint32_t previous_len = 0;
  for (const char* entry : expected) {
    const uint32_t erased = previous_len > strlen(entry) ? previous_len - (uint32_t)strlen(entry) : 0;
    process_line(UP_ARROW_SEQUENCE);
    EXPECT_WRITE_BUFFER(recalled(entry, erased).c_str());
    previous_len = (uint32_t)strlen(entry);
  }

  // "say_hi" was evicted
  process_line(UP_ARROW_SEQUENCE);
  EXPECT_WRITE_BUFFER("");

  process_line("\n");
  EXPECT_WRITE_BUFFER("\nhi\n> ");
}

TEST(ConsoleHistoryTest, TestMaxLengthLineRoundTripsThroughHistory) {
  const std::string longest_line(CONSOLE_MAX_LINE_LENGTH - 1, 'x');
  const std::string response = "ERROR: Command not found (" + longest_line + ")\n> ";

  process_line(longest_line.c_str());
  g_console_write_buffer.clear();
  process_line("\n");
  EXPECT_WRITE_BUFFER(("\n" + response).c_str());

  process_line(UP_ARROW_SEQUENCE);
  EXPECT_WRITE_BUFFER(recalled(longest_line.c_str()).c_str());

  process_line("\n");
  EXPECT_WRITE_BUFFER(("\n" + response).c_str());
}
