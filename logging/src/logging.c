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

#define GET_IMPL(LOGGER) ((logger_impl_t*)LOGGER->_private)

typedef struct {
    logging_level_t level;
} logger_impl_t;
_Static_assert(sizeof(logger_impl_t) == sizeof(((logging_logger_t*)0)->_private), "Invalid context size");

#if LOGGING_USE_DATETIME
typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint16_t ms;
} datetime_t;
#endif

static const char* LEVEL_PREFIX[] = {
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
static char m_write_buffer[FULL_LOG_MAX_LENGTH];

#if LOGGING_USE_DATETIME
static bool is_leap_year(uint16_t year) {
    // A year is a leap year if it's evenly divisible by 4 unless it's evenly divisible by 100 but not 400.
    return ((year & 3) == 0 && (year % 100)) || (year % 400) == 0;
}

static void timestamp_to_datetime(uint64_t ms, datetime_t* datetime) {
    uint32_t seconds = ms / 1000;

    // Calculate days since epoch (add 1 for the current day)
    uint32_t days = seconds / 86400 + 1;
    seconds = seconds % 86400;

    // Calculate the hour, minute, seconds of the datetime
    datetime->hour = (uint8_t)(seconds / 3600);
    seconds = seconds % 3600;
    datetime->minute = (uint8_t)(seconds / 60);
    datetime->second = (uint8_t)(seconds % 60);
    datetime->ms = ms % 1000;

    // Calculate the year
    uint16_t days_in_year = 365;
    datetime->year = 1970;
    while (days > days_in_year) {
        days -= days_in_year;
        datetime->year++;
        if (is_leap_year(datetime->year)) {
            // this is a leap year
            days_in_year = 366;
        } else {
            days_in_year = 365;
        }
    }

    // Calculate the month and day
    datetime->month = 0;
    for (uint8_t month = 1; month <= 12; month++) {
        const uint8_t days_in_month = (month == 2 && is_leap_year(datetime->year)) ? 29 : DAYS_PER_MONTH[month-1];
        if (days <= days_in_month) {
            datetime->month = month;
            break;
        } else {
            days -= days_in_month;
        }
    }
    datetime->day = days;
}
#endif

bool logging_init(const logging_init_t* init) {
    if ((!init->write_function && !init->raw_write_function) || init->default_level == LOGGING_LEVEL_DEFAULT) {
        return false;
    }
    m_init = *init;
    return true;
}

void logging_log_impl(logging_logger_t* logger, logging_level_t level, const char* file, int line, const char* fmt, ...) {
    logger_impl_t* impl = GET_IMPL(logger);
    const logging_level_t min_level = impl->level == LOGGING_LEVEL_DEFAULT ? m_init.default_level : impl->level;
    if (level < min_level) {
        return;
    }

    if (m_init.lock_function) {
        m_init.lock_function(true);
    }

    // time
    if (m_init.time_ms_function) {
#if LOGGING_USE_DATETIME
        uint64_t current_time = m_init.time_ms_function();
        datetime_t datetime;
        timestamp_to_datetime(current_time, &datetime);
        snprintf(m_write_buffer, FULL_LOG_MAX_LENGTH, "%04u-%02u-%02u %02u:%02u:%02u.%03u ", datetime.year, datetime.month,
            datetime.day, datetime.hour, datetime.minute, datetime.second, datetime.ms);
#else
        uint32_t current_time = m_init.time_ms_function();
        const uint16_t ms = current_time % 1000;
        current_time /= 1000;
        const uint16_t sec = current_time % 60;
        current_time /= 60;
        const uint16_t min = current_time % 60;
        current_time /= 60;
        const uint16_t hours = current_time;
        snprintf(m_write_buffer, FULL_LOG_MAX_LENGTH, "%3u:%02u:%02u.%03u ", hours, min, sec, ms);
#endif
    }

    // level
    snprintf(&m_write_buffer[strlen(m_write_buffer)], FULL_LOG_MAX_LENGTH-strlen(m_write_buffer), "%s", LEVEL_PREFIX[level]);

    // module (if set)
    if (logger->module_prefix) {
        snprintf(&m_write_buffer[strlen(m_write_buffer)], FULL_LOG_MAX_LENGTH-strlen(m_write_buffer), "%s", logger->module_prefix);
    }

    // file name
    snprintf(&m_write_buffer[strlen(m_write_buffer)], FULL_LOG_MAX_LENGTH-strlen(m_write_buffer), "%s", file);

    // line number
    snprintf(&m_write_buffer[strlen(m_write_buffer)], FULL_LOG_MAX_LENGTH-strlen(m_write_buffer), ":%d: ", line);

    // log message
    va_list args;
    va_start(args, fmt);
    vsnprintf(&m_write_buffer[strlen(m_write_buffer)], FULL_LOG_MAX_LENGTH-strlen(m_write_buffer), fmt, args);
    va_end(args);

    // newline
    snprintf(&m_write_buffer[strlen(m_write_buffer)], FULL_LOG_MAX_LENGTH-strlen(m_write_buffer), "\n");

    if (m_init.write_function) {
        m_init.write_function(m_write_buffer);
    }
    if (m_init.raw_write_function) {
        m_init.raw_write_function(level, logger->module_prefix, m_write_buffer);
    }

    if (m_init.lock_function) {
        m_init.lock_function(false);
    }
}

void logging_set_level_impl(logging_logger_t* logger, logging_level_t level) {
    GET_IMPL(logger)->level = level;
}
