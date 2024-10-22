// We explicitly don't use include guards as this file should not be included recursively.

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _MSC_VER
#define _LOGGING_FORMAT_ATTR
#define _LOGGING_USED_ATTR
#else
#define _LOGGING_FORMAT_ATTR __attribute__((format(printf, 5, 6)))
#define _LOGGING_USED_ATTR __attribute__((used))
#endif

// Internal type used to represent a logger (should not be directly modified)
typedef struct {
    logging_level_t level;
    const char* const module_prefix;
} logging_logger_t;

// Internal implementation macros / functions which are called via the LOG_* macros
#define _LOG_LEVEL_IMPL(LEVEL, ...) if (logging_level_is_active(&_logging_logger, LEVEL)) logging_log_impl(LEVEL, __FILE__, __LINE__, _logging_logger.module_prefix, __VA_ARGS__)
bool logging_level_is_active(logging_logger_t* logger, logging_level_t level);
void logging_log_impl(logging_level_t level, const char* file, int line, const char* module_prefix, const char* fmt, ...) _LOGGING_FORMAT_ATTR;

#ifdef LOGGING_MODULE_NAME
#define _LOGGING_MODULE_PREFIX LOGGING_MODULE_NAME ":"
#undef LOGGING_MODULE_NAME
#else
#define _LOGGING_MODULE_PREFIX 0
#endif

#ifndef LOGGING_FILE_DEFAULT_LEVEL
#define LOGGING_FILE_DEFAULT_LEVEL LOGGING_LEVEL_DEFAULT
#endif

// Per-file context object which we should create
static logging_logger_t _logging_logger _LOGGING_USED_ATTR = {
    .level = LOGGING_FILE_DEFAULT_LEVEL,
    .module_prefix = _LOGGING_MODULE_PREFIX,
};

// Clean up any configuration / internal defines
#undef LOGGING_FILE_DEFAULT_LEVEL
#undef _LOGGING_MODULE_PREFIX
#undef _LOGGING_FORMAT_ATTR
#undef _LOGGING_USED_ATTR

#ifdef __cplusplus
}
#endif
