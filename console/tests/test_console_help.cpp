#include "gtest/gtest.h"

extern "C" {

#include "anchor/console/console.h"

};

#include <cstring>

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

#if CONSOLE_HELP_COMMAND
TEST(ConsoleTest, TestHelpMultiOptionalArgs) {
  process_line("help multi_add\n");
  EXPECT_WRITE_BUFFER(
    "help multi_add\n"
    "Add numbers with multiple optional args\n"
    "Usage: multi_add num1 [num2] [num3]\n"
    "  num1 - First number\n"
    "  num2 - Second (optional) number\n"
    "  num3 - Third (optional) number\n"
    "> ");

  process_line("help greet\n");
  EXPECT_WRITE_BUFFER(
    "help greet\n"
    "Greet with optional name and title\n"
    "Usage: greet greeting [name] [title]\n"
    "  greeting - The greeting word\n"
    "  name     - Name to greet\n"
    "  title    - Title prefix\n"
    "> ");
}

TEST(ConsoleTest, TestHelpCommand) {
  process_line("help\n");
  EXPECT_WRITE_BUFFER("help\n"
    "Available commands:\n"
    "  help      - List all commands, or give details about a specific command\n"
    "  say_hi    - Says hi\n"
    "  say_bye   - Says bye\n"
    "  minimal\n"
    "  add       - Add two numbers\n"
    "  stroff    - Prints a string starting from an offset\n"
    "  multi_add - Add numbers with multiple optional args\n"
    "  greet     - Greet with optional name and title\n"
    "> ");

  process_line("help add\n");
  EXPECT_WRITE_BUFFER(
    "help add\n"
    "Add two numbers\n"
    "Usage: add num1 num2 [num3]\n"
    "  num1 - First number\n"
    "  num2 - Second number\n"
    "  num3 - Third (optional) number\n"
    "> ");

  process_line("help minimal\n");
  EXPECT_WRITE_BUFFER(
    "help minimal\n"
    "Usage: minimal\n"
    "> ");
}

TEST(ConsoleTest, TestHelpTabCompletion) {
  process_line("help sa\t");
  EXPECT_WRITE_BUFFER("help say_");

  process_line("\t");
  EXPECT_WRITE_BUFFER("\nsay_hi say_bye\n> help say_");

  process_line("h\t");
  EXPECT_WRITE_BUFFER("hi");
}
#endif
