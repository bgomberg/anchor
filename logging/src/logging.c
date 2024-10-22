#include "anchor/logging/logging.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LEVEL_PREFIX_LENGTH     6
#if LOGGING_USE_DATETIME
#define TIME_LENGTH             24
#else
#define TIME_LENGTH             14
#endif
#define FILE_NAME_LENGTH        32
#define FULL_LOG_MAX_LENGTH     (LOGGING_MAX_MSG_LENGTH + LEVEL_PREFIX_LENGTH + TIME_LENGTH + FILE_NAME_LENGTH + 1)

#define BUFFER_PRINTF(BUFFER, SIZE, FMT, ...) ({ \
        const uint32_t _len = strlen(BUFFER); \
        snprintf(&BUFFER[_len], (SIZE)-_len, FMT, __VA_ARGS__); \
    })

#define BUFFER_VA_PRINTF(BUFFER, SIZE, FMT, ARGS) ({ \
        const uint32_t _len = strlen(BUFFER); \
        vsnprintf(&BUFFER[_len], (SIZE)-_len, FMT, ARGS); \
    })

#define BUFFER_WRITE(BUFFER, SIZE, STR) BUFFER_PRINTF(BUFFER, SIZE, "%s", STR)

static const char* const LEVEL_PREFIX[] = {
    [LOGGING_LEVEL_DEFAULT] =   "????? ", // should never be printed
    [LOGGING_LEVEL_DEBUG] =     "DEBUG ",
    [LOGGING_LEVEL_INFO] =      "INFO  ",
    [LOGGING_LEVEL_WARN] =      "WARN  ",
    [LOGGING_LEVEL_ERROR] =     "ERROR ",
};
#if LOGGING_USE_DATETIME
static const uint8_t DAYS_PER_MONTH[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
#endif

static logging_init_t m_init;

#if LOGGING_USE_DATETIME
static bool is_leap_year(uint16_t year) {
    // A year is a leap year if it's evenly divisible by 4 unless it's evenly divisible by 100 but not 400.
    return ((year & 3) == 0 && (year % 100)) || (year % 400) == 0;
}
#endif

static void get_timestamp_components(logging_timestamp_t timestamp, logging_timestamp_components_t* components) {
#if LOGGING_USE_DATETIME
    logging_timestamp_t seconds_timestamp = timestamp % 86400000;
#else
    logging_timestamp_t seconds_timestamp = timestamp;
#endif
    components->ms = seconds_timestamp % 1000;
    seconds_timestamp /= 1000;
    components->second = seconds_timestamp % 60;
    seconds_timestamp /= 60;
    components->minute = seconds_timestamp % 60;
    seconds_timestamp /= 60;
    components->hour = seconds_timestamp;

#if LOGGING_USE_DATETIME
    // Calculate days since epoch (add 1 for the current day)
    uint32_t days = timestamp / 86400000 + 1;

    // Calculate the year
    uint16_t days_in_year = 365;
    components->year = 1970;
    while (days > days_in_year) {
        days -= days_in_year;
        components->year++;
        if (is_leap_year(components->year)) {
            // this is a leap year
            days_in_year = 366;
        } else {
            days_in_year = 365;
        }
    }

    // Calculate the month and day
    components->month = 0;
    for (uint8_t month = 1; month <= 12; month++) {
        const uint8_t days_in_month = (month == 2 && is_leap_year(components->year)) ? 29 : DAYS_PER_MONTH[month-1];
        if (days <= days_in_month) {
            components->month = month;
            break;
        } else {
            days -= days_in_month;
        }
    }
    components->day = days;
#endif
}

#if !LOGGING_CUSTOM_HANDLER
static void handle_log_line(const logging_line_t* log_line) {
    if (m_init.lock_function) {
        m_init.lock_function(true);
    }
    static char buffer[FULL_LOG_MAX_LENGTH];
    logging_format_line(log_line, buffer, sizeof(buffer));
    m_init.write_function(buffer);
    if (m_init.lock_function) {
        m_init.lock_function(false);
    }
}
#endif

static void log_line_helper(logging_level_t level, const char* file, int line, const char* module_prefix, const char* fmt, va_list args) {
    logging_line_t log_line = {
        .level = level,
        .file = file,
        .line = line,
        .module_prefix = module_prefix,
        .timestamp = m_init.time_ms_function ? m_init.time_ms_function() : 0,
        .fmt = fmt,
        .args = args,
    };
    get_timestamp_components(log_line.timestamp, &log_line.timestamp_components);

#if LOGGING_CUSTOM_HANDLER
    m_init.handler(&log_line);
#else
    handle_log_line(&log_line);
#endif
}

bool logging_init(const logging_init_t* init) {
    if (init->default_level == LOGGING_LEVEL_DEFAULT) {
        return false;
    }
#if LOGGING_CUSTOM_HANDLER
    if (!init->handler) {
        return false;
    }
#else
    if (!init->write_function) {
        return false;
    }
#endif
    m_init = *init;
    return true;
}

void logging_log_line(logging_level_t level, const char* file, int line, const char* module_prefix, const char* fmt, va_list args) {
    if (level < m_init.default_level) {
        return;
    }
    log_line_helper(level, file, line, module_prefix, fmt, args);
}

bool logging_level_is_active(logging_logger_t* logger, logging_level_t level) {
    const logging_level_t min_level = logger->level == LOGGING_LEVEL_DEFAULT ? m_init.default_level : logger->level;
    return level >= min_level;
}

void logging_log_impl(logging_level_t level, const char* file, int line, const char* module_prefix, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_line_helper(level, file, line, module_prefix, fmt, args);
    va_end(args);
}

void logging_format_line(const logging_line_t* log_line, char* buffer, uint32_t size) {
    buffer[0] = '\0';

    // Add the timestamp
    if (m_init.time_ms_function || log_line->timestamp) {
        logging_timestamp_components_t timestamp_components;
        get_timestamp_components(log_line->timestamp, &timestamp_components);
        BUFFER_PRINTF(buffer, size, LOGGING_TIMESTAMP_FMT_STR " ", LOGGING_TIMESTAMP_FMT_ARGS(timestamp_components));
    }

    // Add the level
    BUFFER_WRITE(buffer, size, LEVEL_PREFIX[log_line->level]);

    // Add the module (if set)
    if (log_line->module_prefix) {
        BUFFER_WRITE(buffer, size, log_line->module_prefix);
    }

    // Add the file name
    BUFFER_WRITE(buffer, size, log_line->file);

    // Add the line number
    BUFFER_PRINTF(buffer, size, ":%d: ", log_line->line);

    // Add the formatted log message
    BUFFER_VA_PRINTF(buffer, size, log_line->fmt, log_line->args);

    // Always add a newline, even if it means truncating the message
    if (strlen(buffer) == size - 1) {
        // Need to make space for the newline
        buffer[size-2] = '\0';
    }
    BUFFER_WRITE(buffer, size, "\n");
}
