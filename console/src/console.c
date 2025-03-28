#include "anchor/console/console.h"

#include <limits.h>
#include <string.h>
#include <stdlib.h>

#define CHAR_CTRL_C     0x03

#define FOREACH_COMMAND(VAR) \
    for (uint32_t __i = 0; __i < m_num_commands; __i++) \
        for (const console_command_def_t* VAR = &m_commands[__i]; VAR; VAR = NULL)

typedef union {
    intptr_t value_int;
    const char* value_str;
    void* ptr;
} parsed_arg_t;

typedef enum {
    PROCESS_ERROR_SUCCESS = 0,
    PROCESS_ERROR_LEADING_WHITESPACE,
    PROCESS_ERROR_EXTRA_WHITESPACE,
    PROCESS_ERROR_EMPTY_LINE,
} process_error_t;

#if CONSOLE_HELP_COMMAND
#if CONSOLE_TAB_COMPLETE
CONSOLE_COMMAND_DEF_WITH_TAB_COMPLETION(help, "List all commands, or give details about a specific command",
    CONSOLE_OPTIONAL_STR_ARG_DEF(command, "The name of the command to give details about")
);
#else
CONSOLE_COMMAND_DEF(help, "List all commands, or give details about a specific command",
    CONSOLE_OPTIONAL_STR_ARG_DEF(command, "The name of the command to give details about")
);
#endif
#endif

static console_init_t m_init;
static console_command_def_t m_commands[CONSOLE_MAX_COMMANDS] CONSOLE_BUFFER_ATTRIBUTES;
static uint32_t m_num_commands;
static char m_line_buffer[CONSOLE_MAX_LINE_LENGTH] CONSOLE_BUFFER_ATTRIBUTES;
static uint32_t m_line_len;
static uint32_t m_cursor_pos;
static bool m_line_invalid;
static bool m_is_active;
static uint32_t m_escape_sequence_index = 0;
#if CONSOLE_HISTORY
static char m_history_buffer[CONSOLE_HISTORY][CONSOLE_MAX_LINE_LENGTH] CONSOLE_BUFFER_ATTRIBUTES;
static uint32_t m_history_start_index = 0;
static uint32_t m_history_len = 0;
static int32_t m_history_index = -1;
#endif

static bool validate_arg_def(const console_arg_def_t* arg, bool is_last) {
    switch (arg->type) {
        case CONSOLE_ARG_TYPE_INT:
        case CONSOLE_ARG_TYPE_STR:
            return arg->name && (!arg->is_optional || is_last);
        default:
            return false;
    }
}

static const console_command_def_t* get_command(const char* name) {
    FOREACH_COMMAND(cmd_def) {
        if (!strcmp(cmd_def->name, name)) {
            return cmd_def;
        }
    }
    return NULL;
}

static void write_str(const char* str) {
    if (!m_init.write_function) {
        return;
    }
    m_init.write_function(str);
}

static bool parse_arg(const char* arg_str, console_arg_type_t type, parsed_arg_t* parsed_arg) {
    switch (type) {
        case CONSOLE_ARG_TYPE_INT: {
            char* end_ptr = NULL;
            long int result = strtol(arg_str, &end_ptr, 0);
            if (result == LONG_MAX || result == LONG_MIN || end_ptr == NULL || *end_ptr != '\0') {
                return false;
            }
            parsed_arg->value_int = (intptr_t)result;
            return true;
        }
        case CONSOLE_ARG_TYPE_STR:
            parsed_arg->value_str = arg_str;
            return true;
        default:
            // should never get here
            return false;
    }
}

static uint32_t get_num_required_args(const console_command_def_t* cmd) {
    if (cmd->num_args == 0) {
        return 0;
    }
    const console_arg_def_t* last_arg = &cmd->args[cmd->num_args-1];
    switch (last_arg->type) {
        case CONSOLE_ARG_TYPE_INT:
        case CONSOLE_ARG_TYPE_STR:
            if (last_arg->is_optional) {
                return cmd->num_args - 1;
            } else {
                return cmd->num_args;
            }
        default:
            return 0;
    }
}
#if CONSOLE_HISTORY
static void history_add_line(void) {
    if (!strcmp(m_history_buffer[(m_history_start_index + m_history_len) % CONSOLE_HISTORY], m_line_buffer)) {
        // same as the previous history line, so don't bother re-adding it
        return;
    }
    const uint32_t history_write_index = (m_history_start_index + m_history_len + 1) % CONSOLE_HISTORY;
    strcpy(m_history_buffer[history_write_index], m_line_buffer);
    if (m_history_len == CONSOLE_HISTORY) {
        // history buffer is full, so drop the first element to clear room for the new one
        m_history_start_index = (m_history_start_index + 1) % CONSOLE_HISTORY;
    } else {
        m_history_len++;
    }
}
#endif

static const console_command_def_t* parse_line(uint32_t* num_args) {
    if (m_line_invalid) {
        return NULL;
    }

    // parse and validate the line
    const console_command_def_t* cmd = NULL;
    uint32_t arg_index = 0;
    const char* current_token = NULL;
    for (uint32_t i = 0; i <= m_line_len; i++) {
        const char c = m_line_buffer[i];
        if (c == ' ' || c == '\0') {
            // end of a token
            if (!current_token) {
                if (cmd) {
                    // too much whitespace
                    write_str("ERROR: Extra whitespace between arguments"CONSOLE_NEWLINE);
                    return NULL;
                } else {
                    bool is_empty = true;
                    for (uint32_t j = 0; j < m_line_len; j++) {
                        if (m_line_buffer[j] != ' ') {
                            is_empty = false;
                            break;
                        }
                    }
                    if (!is_empty) {
                        write_str("ERROR: Whitespace before command"CONSOLE_NEWLINE);
                    }
                    return NULL;
                }
            }

#if CONSOLE_HISTORY
            if (!cmd) {
                history_add_line();
            }
#endif

            // process this token
            m_line_buffer[i] = '\0';
            if (!cmd) {
                // find the command
                cmd = get_command(current_token);
                if (!cmd) {
                    write_str("ERROR: Command not found (");
                    write_str(current_token);
                    write_str(")"CONSOLE_NEWLINE);
                    return NULL;
                }
            } else {
                // this is an argument
                if (arg_index == cmd->num_args) {
                    write_str("ERROR: Too many arguments"CONSOLE_NEWLINE);
                    return NULL;
                }
                // validate the argument
                const console_arg_def_t* arg = &cmd->args[arg_index];
                parsed_arg_t parsed_arg;
                if (!parse_arg(current_token, arg->type, &parsed_arg)) {
                    write_str("ERROR: Invalid value for '");
                    write_str(arg->name);
                    write_str("' (");
                    write_str(current_token);
                    write_str(")"CONSOLE_NEWLINE);
                    return NULL;
                }
                cmd->args_ptr[arg_index] = parsed_arg.ptr;
                arg_index++;
            }
            current_token = NULL;
        } else if (!current_token) {
            current_token = &m_line_buffer[i];
        }
    }

    *num_args = arg_index;
    return cmd;
}

static void process_line(void) {
    uint32_t num_args = 0;
    const console_command_def_t* cmd = parse_line(&num_args);
    if (!cmd) {
        return;
    } else if (num_args < get_num_required_args(cmd)) {
        write_str("ERROR: Too few arguments"CONSOLE_NEWLINE);
        return;
    }

    if (num_args != cmd->num_args) {
        // set the optional argument to its default value
        switch (cmd->args[num_args].type) {
            case CONSOLE_ARG_TYPE_INT:
                cmd->args_ptr[num_args] = (void*)CONSOLE_INT_ARG_DEFAULT;
                break;
            case CONSOLE_ARG_TYPE_STR:
                cmd->args_ptr[num_args] = (void*)CONSOLE_STR_ARG_DEFAULT;
                break;
        }
    }

    // run the handler
    m_is_active = true;
    if (cmd->num_args) {
        cmd->handler(cmd->args_ptr);
    } else {
        cmd->handler_no_args();
    }
    m_is_active = false;
}

static void reset_line_and_print_prompt(void) {
#if CONSOLE_HISTORY
    m_history_index = -1;
#endif
    m_escape_sequence_index = 0;
    m_line_len = 0;
    m_cursor_pos = 0;
    m_line_invalid = false;
    m_line_buffer[0] = '\0';
    write_str(CONSOLE_PROMPT);
}

static void push_char(char c) {
    if (m_cursor_pos != m_line_len) {
        // shift the existing data to the right to make space
        memmove(&m_line_buffer[m_cursor_pos + 1], &m_line_buffer[m_cursor_pos], m_line_len - m_cursor_pos);
    }
    m_line_buffer[m_cursor_pos] = c;
    m_cursor_pos++;
    m_line_len++;
    m_line_buffer[m_line_len] = '\0';
    if (m_line_len == CONSOLE_MAX_LINE_LENGTH) {
        // filled up the line buffer, so mark the line invalid
        m_line_invalid = true;
    }
}

#if CONSOLE_FULL_CONTROL
static void move_cursor_to_end(void) {
    if (m_cursor_pos == m_line_len) {
        return;
    }
    write_str(&m_line_buffer[m_cursor_pos]);
    m_cursor_pos = m_line_len;
}

static void erase_current_line(uint32_t new_line_line) {
    if (new_line_line < m_line_len) {
        // TODO: could optimize this to not have to write out characters we're just going to erase
        move_cursor_to_end();
        // erase the characters which the new line won't overwrite
        const uint32_t char_to_erase = m_line_len - new_line_line;
        for (uint32_t i = 0; i < char_to_erase; i++) {
            write_str("\b");
        }
        for (uint32_t i = 0; i < char_to_erase; i++) {
            write_str(" ");
        }
    }
    write_str("\r");
}
#endif

#if CONSOLE_TAB_COMPLETE
static const char* command_tab_complete_iterator(bool start) {
    static uint32_t iter_index = 0;
    if (start) {
        iter_index = 0;
    }
    if (iter_index >= m_num_commands) {
        return NULL;
    }
    return m_commands[iter_index++].name;
}

static void do_tab_complete(void) {
    m_line_buffer[m_line_len] = '\0';
    const char* prefix = m_line_buffer;
    uint32_t prefix_length = m_line_len;
    uint32_t offset = 0;
    console_tab_complete_iterator_t iter = command_tab_complete_iterator;
    FOREACH_COMMAND(cmd_def) {
        const uint32_t cmd_name_len = strlen(cmd_def->name);
        if (m_line_len < cmd_name_len + 1 || strncmp(cmd_def->name, m_line_buffer, cmd_name_len) || m_line_buffer[cmd_name_len] != ' ') {
            // current buffer doesn't start with this command
            continue;
        }
        if (cmd_def->tab_complete_iter) {
            iter = cmd_def->tab_complete_iter;
            offset = cmd_name_len + 1;
            prefix += offset;
            prefix_length -= offset;
        }
        break;
    }

    uint32_t num_matches = 0;
    uint32_t longest_common_prefix = 0;
    const char* first_tab_complete = NULL;
    for (const char* tab_complete = iter(true); tab_complete; tab_complete = iter(false)) {
        if (strncmp(tab_complete, prefix, prefix_length)) {
            continue;
        }
        if (num_matches > 0) {
            for (uint32_t j = 0; j < longest_common_prefix; j++) {
                if (tab_complete[j] != first_tab_complete[j]) {
                    longest_common_prefix = j;
                    break;
                }
            }
        } else {
            first_tab_complete = tab_complete;
            longest_common_prefix = strlen(tab_complete);
        }
        num_matches++;
    }
    const uint32_t completion_length = longest_common_prefix - (m_line_len - offset);
    if (num_matches == 0 || (num_matches == 1 && completion_length == 0)) {
        // nothing to auto-complete
        return;
    }

    move_cursor_to_end();
    if (completion_length) {
        // auto complete the remaining common prefix
        memcpy(&m_line_buffer[m_line_len], &first_tab_complete[m_line_len - offset], completion_length);
        m_line_buffer[m_line_len + completion_length] = '\0';
        write_str(&m_line_buffer[m_line_len]);
        m_line_len += completion_length;
        m_cursor_pos = m_line_len;
    } else {
        // nothing left to auto complete so print all the potential matches in a new line
        write_str(CONSOLE_NEWLINE);
        for (const char* tab_complete = iter(true); tab_complete; tab_complete = iter(false)) {
            if (strncmp(tab_complete, prefix, prefix_length)) {
                continue;
            }
            if (tab_complete != first_tab_complete) {
                write_str(" ");
            }
            write_str(tab_complete);
        }
        write_str(CONSOLE_NEWLINE);
        // re-print the prompt and any valid, pending command
        write_str(CONSOLE_PROMPT);
        if (!m_line_invalid) {
            write_str(m_line_buffer);
        }
    }
}
#endif

#if CONSOLE_HELP_COMMAND
#if CONSOLE_TAB_COMPLETE
static const char* help_tab_complete_iterator(bool start) {
    return command_tab_complete_iterator(start);
}
#endif
static void help_command_handler(const help_args_t* args) {
    if (args->command != CONSOLE_STR_ARG_DEFAULT) {
        const console_command_def_t* cmd_def = get_command(args->command);
        if (!cmd_def) {
            write_str("ERROR: Unknown command (");
            write_str(args->command);
            write_str(")"CONSOLE_NEWLINE);
            return;
        }
        if (cmd_def->desc) {
            write_str(cmd_def->desc);
            write_str(CONSOLE_NEWLINE);
        }
        write_str("Usage: ");
        write_str(args->command);
        uint32_t max_name_len = 0;
        for (uint32_t i = 0; i < cmd_def->num_args; i++) {
            const console_arg_def_t* arg_def = &cmd_def->args[i];
            const uint32_t name_len = strlen(arg_def->name);
            if (name_len > max_name_len) {
                max_name_len = name_len;
            }
            if (arg_def->is_optional) {
                write_str(" [");
            } else {
                write_str(" ");
            }
            write_str(cmd_def->args[i].name);
            if (arg_def->is_optional) {
                write_str("]");
            }
        }
        write_str(CONSOLE_NEWLINE);
        for (uint32_t i = 0; i < cmd_def->num_args; i++) {
            const console_arg_def_t* arg_def = &cmd_def->args[i];
            write_str("  ");
            write_str(arg_def->name);
            if (arg_def->desc) {
                // pad the description so they all line up
                const uint32_t name_len = strlen(arg_def->name);
                for (uint32_t j = 0; j < max_name_len - name_len; j++) {
                    write_str(" ");
                }
                write_str(" - ");
                write_str(arg_def->desc);
            }
            write_str(CONSOLE_NEWLINE);
        }
    } else {
        write_str("Available commands:"CONSOLE_NEWLINE);
        // get the max name length for padding
        uint32_t max_name_len = 0;
        FOREACH_COMMAND(cmd_def) {
            const uint32_t name_len = strlen(cmd_def->name);
            if (name_len > max_name_len) {
                max_name_len = name_len;
            }
        }
        FOREACH_COMMAND(cmd_def) {
            write_str("  ");
            write_str(cmd_def->name);
            if (cmd_def->desc) {
                // pad the description so they all line up
                const uint32_t name_len = strlen(cmd_def->name);
                for (uint32_t j = 0; j < max_name_len - name_len; j++) {
                    write_str(" ");
                }
                write_str(" - ");
                write_str(cmd_def->desc);
            }
            write_str(CONSOLE_NEWLINE);
        }
    }
}
#endif

void console_init(const console_init_t* init) {
    m_init = *init;

    // make sure to deregister previous commands
    m_num_commands = 0;
    memset(m_commands, 0, sizeof(m_commands) / sizeof(m_commands[0]));

#if CONSOLE_HELP_COMMAND
    console_command_register(help);
#endif
    write_str(CONSOLE_NEWLINE CONSOLE_PROMPT);
}

bool console_command_register(const console_command_def_t* cmd) {
    if (m_num_commands == CONSOLE_MAX_COMMANDS) {
        return false;
    }
    // validate the command
    if (!cmd->name || !cmd->handler || strlen(cmd->name) >= CONSOLE_MAX_LINE_LENGTH) {
        return false;
    }
    // validate the arguments
    for (uint32_t i = 0; i < cmd->num_args; i++) {
        if (!validate_arg_def(cmd->args, i + 1 == cmd->num_args)) {
            return false;
        }
    }
    // make sure it's not already registered
    if (get_command(cmd->name)) {
        return false;
    }
    // add the command
    m_commands[m_num_commands++] = *cmd;
    return true;
}

void console_process(const uint8_t* data, uint32_t length) {
#if CONSOLE_FULL_CONTROL
    const char* echo_str = NULL;
    for (uint32_t i = 0; i < length; i++) {
        const char c = data[i];
        if (m_escape_sequence_index == 0 && c == '\x1b') {
            // start of an escape sequence
            m_escape_sequence_index++;
            continue;
        } else if (m_escape_sequence_index == 1) {
            if (c == '[') {
                m_escape_sequence_index++;
            } else {
                // invalid escape sequence
                m_escape_sequence_index = 0;
            }
            continue;
        } else if (m_escape_sequence_index == 2) {
            // process the command
            m_escape_sequence_index = 0;
            if (c == 'C') {
                // right arrow
                if (m_cursor_pos < m_line_len) {
                    const char str[2] = {m_line_buffer[m_cursor_pos], '\0'};
                    write_str(str);
                    m_cursor_pos++;
                }
            } else if (c == 'D') {
                // left arrow
                if (m_cursor_pos) {
                    write_str("\b");
                    m_cursor_pos--;
                }
            }
#if CONSOLE_HISTORY
            bool update_line_from_history = false;
            if (c == 'A') {
                // up arrow
                if (m_history_index + 1 < m_history_len) {
                    m_history_index++;
                    update_line_from_history = true;
                }
            } else if (c == 'B') {
                // down arrow
                if (m_history_index > -1) {
                    m_history_index--;
                    update_line_from_history = true;
                }
            }
            if (update_line_from_history) {
                const char* history_line = (m_history_index == -1) ? "" : m_history_buffer[(m_history_start_index + m_history_len - m_history_index) % CONSOLE_HISTORY];
                erase_current_line(strlen(history_line));
                strcpy(m_line_buffer, history_line);
                m_line_len = strlen(m_line_buffer);
                m_cursor_pos = m_line_len;
                write_str(CONSOLE_PROMPT);
                write_str(m_line_buffer);
            }
#endif
            continue;
        }
        if (c == CONSOLE_RETURN_KEY) {
            if (echo_str) {
                write_str(echo_str);
                echo_str = NULL;
            }
            write_str(CONSOLE_NEWLINE);
            process_line();
            reset_line_and_print_prompt();
        } else if (c == CHAR_CTRL_C) {
            if (echo_str) {
                write_str(echo_str);
                echo_str = NULL;
            }
            write_str(CONSOLE_NEWLINE);
            reset_line_and_print_prompt();
            echo_str = NULL;
        } else if (!m_line_invalid && c == '\b') {
            if (echo_str) {
                write_str(echo_str);
                echo_str = NULL;
            }
            if (m_cursor_pos) {
                write_str("\b \b");
                if (m_cursor_pos != m_line_len) {
                    // shift all the characters in the line down
                    memmove(&m_line_buffer[m_cursor_pos-1], &m_line_buffer[m_cursor_pos], m_line_len - m_cursor_pos);
                }
                m_cursor_pos--;
                m_line_len--;
                m_line_buffer[m_line_len] = '\0';
                if (m_cursor_pos != m_line_len) {
                    write_str(&m_line_buffer[m_cursor_pos]);
                    write_str(" ");
                    for (uint32_t i = 0; i < m_line_len - m_cursor_pos + 1; i++) {
                        write_str("\b");
                    }
                }
            }
#if CONSOLE_TAB_COMPLETE
        } else if (!m_line_invalid && c == '\t') {
            if (echo_str) {
                write_str(echo_str);
                echo_str = NULL;
            }
            do_tab_complete();
#endif
        } else if (!m_line_invalid && c >= ' ' && c <= '~') {
            // valid character
            if (m_cursor_pos != m_line_len) {
                if (echo_str) {
                    write_str(echo_str);
                    echo_str = NULL;
                }
                const uint32_t prev_cursor_pos = m_cursor_pos;
                push_char(c);
                write_str(&m_line_buffer[prev_cursor_pos]);
                for (uint32_t i = 0; i < m_line_len - m_cursor_pos; i++) {
                    write_str("\b");
                }
            } else {
                if (!echo_str) {
                    // FIXME for m_cursor_pos != m_line_len
                    echo_str = &m_line_buffer[m_line_len];
                }
                push_char(c);
            }
        }
    }
    if (echo_str) {
        write_str(echo_str);
    }
#else
    for (uint32_t i = 0; i < length; i++) {
        const char c = data[i];
        if (c == CONSOLE_RETURN_KEY) {
            process_line();
            reset_line_and_print_prompt();
        } else if (!m_line_invalid && c >= ' ' && c <= '~') {
            // valid character
            push_char(c);
        } else {
            m_line_invalid = true;
        }
    }
#endif
}

#if CONSOLE_FULL_CONTROL
void console_print_line(const char* str) {
    // erase any characters which would end up still showing after the end line
    erase_current_line(strlen(str));
    // print the line
    write_str(str);
    if (!m_is_active) {
        // re-print the prompt and any valid, pending command
        write_str(CONSOLE_PROMPT);
        if (!m_line_invalid) {
            write_str(m_line_buffer);
            // fix the cursor position if needed
            for (uint32_t i = 0; i < m_line_len - m_cursor_pos; i++) {
                write_str("\b");
            }
        }
    }
}
#endif
