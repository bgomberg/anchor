# Logging

Simple logging library with support for multiple levels, timestamps, file name,
and line numbers.

## Usage

The logging library is initialized by calling `logging_init()`.

To generate logs, simply include `logging.h` from your source file and use
one of the `LOG_[DEBUG|INFO|WARN|ERROR]` macros as you would use `printf` to
write a log line at a given level. Each file is treated as its own module, and
the level of each module can be changed using the `LOG_SET_LEVEL(...)` macro.

### Shorter File Names

The logging library will use the `__FILE__` define provided by gcc to get the
name of the file. To reduce code size, and make the logs easier to read, it's
recommended to use the `-fmacro-prefix-map={DIRECTORY}=` GCC option.
An example Make rule which does this is shown below:
```
$(BUILD_DIR)/%.o: %.c
	@$(CC) -c -fmacro-prefix-map=$(dir $<)/= $(CFLAGS) $< -o $@
```

### Module Prefix

In order to prefix the file name with a module name, define
`LOGGING_MODULE_NAME` before including `logging.h` in your source file.

### Per-File Default Level

In order to set a default logging level at compile time, define
`LOGGING_FILE_DEFAULT_LEVEL` before including `logging.h`.

### Compile Options

Parameters and additional features of the logging library can be configured
with the following defines:
* `LOGGING_CUSTOM_HANDLER` - Allows for specifying a custom log handler, which
bypasses any formatting done by logging library.
* `LOGGING_MAX_MSG_LENGTH` - The maximum length (128 by default) of a log
message not including the extra prefixes added by the logging library. This
setting is not used if LOGGING_CUSTOM_HANDLER is set.
* `LOGGING_USE_DATETIME` - Prefixes log lines with a full datetime string
(YYYY-MM-DD HH:MM:SS.SSS) instead of a system uptime (HHH:MM:SS.SSS) and
changes `time_ms_function` to return a `uint64_t` which should be the number of
milliseconds since epoch. This defaults to 0.

## Example Output
```
  0:00:00.626 WARN  system.c:199: Last reset due to software reset
  0:00:00.632 INFO  system.c:243: Serial number is 003b001b524b501020353548
  0:00:00.639 INFO  system.c:257: Detected DVT hardware
  0:00:00.648 INFO  piezo.c:45: is_present=1
  0:00:16.732 ERROR SONAR:link_layer.c:161: Invalid packet: Response sequence number does not match request
```
