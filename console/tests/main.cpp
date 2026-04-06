#include "gtest/gtest.h"

extern "C" {

#include "anchor/console/console.h"

};

#include <stdio.h>

#define CONSOLE_COMMAND_GET_INT_ARG(NAME) ({ \
    int _arg; \
    const bool _res = console_command_get_int_arg(NAME, &_arg); \
    ASSERT_TRUE(_res); \
    _arg; \
  })

#define CONSOLE_COMMAND_GET_STR_ARG(NAME) ({ \
    const char* _arg; \
    const bool _res = console_command_get_str_arg(NAME, &_arg); \
    ASSERT_TRUE(_res); \
    _arg; \
  })

#define CONSOLE_COMMAND_GET_OPTIONAL_INT_ARG(NAME, DEFAULT_VALUE) ({ \
    int _arg; \
    const bool _res = console_command_get_int_arg(NAME, &_arg); \
    _res ? _arg : DEFAULT_VALUE; \
  })

#if CONSOLE_HELP_COMMAND
CONSOLE_COMMAND_DEF(say_hi, "Says hi");
CONSOLE_COMMAND_DEF(say_bye, "Says bye");
CONSOLE_COMMAND_DEF(minimal, NULL);
CONSOLE_COMMAND_DEF(add, "Add two numbers",
  CONSOLE_INT_ARG_DEF(num1, "First number"),
  CONSOLE_INT_ARG_DEF(num2, "Second number"),
  CONSOLE_OPTIONAL_INT_ARG_DEF(num3, "Third (optional) number")
);
CONSOLE_COMMAND_DEF(stroff, "Prints a string starting from an offset",
  CONSOLE_STR_ARG_DEF(str, "The string"),
  CONSOLE_INT_ARG_DEF(offset, "Offset into the string")
);
CONSOLE_COMMAND_DEF(multi_add, "Add numbers with multiple optional args",
  CONSOLE_INT_ARG_DEF(num1, "First number"),
  CONSOLE_OPTIONAL_INT_ARG_DEF(num2, "Second (optional) number"),
  CONSOLE_OPTIONAL_INT_ARG_DEF(num3, "Third (optional) number")
);
CONSOLE_COMMAND_DEF(greet, "Greet with optional name and title",
  CONSOLE_STR_ARG_DEF(greeting, "The greeting word"),
  CONSOLE_OPTIONAL_STR_ARG_DEF(name, "Name to greet"),
  CONSOLE_OPTIONAL_STR_ARG_DEF(title, "Title prefix")
);
#else
CONSOLE_COMMAND_DEF(say_hi);
CONSOLE_COMMAND_DEF(say_bye);
CONSOLE_COMMAND_DEF(minimal);
CONSOLE_COMMAND_DEF(add,
  CONSOLE_INT_ARG_DEF(num1),
  CONSOLE_INT_ARG_DEF(num2),
  CONSOLE_OPTIONAL_INT_ARG_DEF(num3)
);
CONSOLE_COMMAND_DEF(stroff,
  CONSOLE_STR_ARG_DEF(str),
  CONSOLE_INT_ARG_DEF(offset)
);
CONSOLE_COMMAND_DEF(multi_add,
  CONSOLE_INT_ARG_DEF(num1),
  CONSOLE_OPTIONAL_INT_ARG_DEF(num2),
  CONSOLE_OPTIONAL_INT_ARG_DEF(num3)
);
CONSOLE_COMMAND_DEF(greet,
  CONSOLE_STR_ARG_DEF(greeting),
  CONSOLE_OPTIONAL_STR_ARG_DEF(name),
  CONSOLE_OPTIONAL_STR_ARG_DEF(title)
);
#endif

std::vector<char> g_console_write_buffer;

static void write_function(const char* str) {
  char c;
  while ((c = *str++) != '\0') {
    g_console_write_buffer.push_back(c);
  }
}

static void say_hi_command_handler(void) {
  write_function("hi\n");
}

static void say_bye_command_handler(void) {
  write_function("hi\n");
}

static void minimal_command_handler(void) {
  write_function("Hello world!\n");
}

static void add_command_handler(const add_args_t* args) {
  char buffer[100];
  if (args->num3 != CONSOLE_INT_ARG_DEFAULT) {
    snprintf(buffer, sizeof(buffer), "%ld + %ld + %ld = %ld\n", args->num1, args->num2, args->num3, args->num1 + args->num2 + args->num3);
  } else {
    snprintf(buffer, sizeof(buffer), "%ld + %ld = %ld\n", args->num1, args->num2, args->num1 + args->num2);
  }
  write_function(buffer);
}

static void stroff_command_handler(const stroff_args_t* args) {
  char buffer[100];
  snprintf(buffer, sizeof(buffer), "%s\n", args->str + args->offset);
  write_function(buffer);
}

static void multi_add_command_handler(const multi_add_args_t* args) {
  char buffer[100];
  intptr_t result = args->num1;
  if (args->num2 != CONSOLE_INT_ARG_DEFAULT && args->num3 != CONSOLE_INT_ARG_DEFAULT) {
    snprintf(buffer, sizeof(buffer), "%ld + %ld + %ld = %ld\n", args->num1, args->num2, args->num3, args->num1 + args->num2 + args->num3);
  } else if (args->num2 != CONSOLE_INT_ARG_DEFAULT) {
    result += args->num2;
    snprintf(buffer, sizeof(buffer), "%ld + %ld = %ld\n", args->num1, args->num2, result);
  } else {
    snprintf(buffer, sizeof(buffer), "%ld\n", result);
  }
  write_function(buffer);
}

static void greet_command_handler(const greet_args_t* args) {
  char buffer[200];
  if (args->name != CONSOLE_STR_ARG_DEFAULT && args->title != CONSOLE_STR_ARG_DEFAULT) {
    snprintf(buffer, sizeof(buffer), "%s %s %s\n", args->greeting, args->title, args->name);
  } else if (args->name != CONSOLE_STR_ARG_DEFAULT) {
    snprintf(buffer, sizeof(buffer), "%s %s\n", args->greeting, args->name);
  } else {
    snprintf(buffer, sizeof(buffer), "%s\n", args->greeting);
  }
  write_function(buffer);
}

int main(int argc, char **argv) {
  const console_init_t init_console = {
    .write_function = write_function,
  };
  console_init(&init_console);
  g_console_write_buffer.clear();
  console_command_register(say_hi);
  console_command_register(say_bye);
  console_command_register(minimal);
  console_command_register(add);
  console_command_register(stroff);
  console_command_register(multi_add);
  console_command_register(greet);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
