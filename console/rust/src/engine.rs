use core::ffi::CStr;
use core::iter;
use core::ptr;
use core::str;
use core::sync::atomic::{AtomicBool, AtomicU8, AtomicUsize, Ordering};

use heapless::Vec;

use crate::{ArgKind, ArgSpec, Args, Command, MAX_ARGS, RegisterError, Value, Write};

const PROMPT: &[u8] = b"> ";
const NEWLINE: &[u8] = b"\n";
const RETURN_KEY: u8 = b'\n';
const CHAR_CTRL_C: u8 = 0x03;
const CHAR_BACKSPACE: u8 = 0x08;
const CHAR_TAB: u8 = b'\t';
const CHAR_ESCAPE: u8 = 0x1b;

static HELP: Command = Command {
    name: "help",
    desc: "List all commands, or give details about a specific command",
    args: &[ArgSpec::str("command", "The name of the command to give details about").optional()],
    completions: &[],
    handler: |_, _| {},
};

#[derive(Clone, Copy)]
enum Completer {
    Commands,
    List(&'static [&'static str]),
}

fn completer_for(command: &'static Command) -> Option<Completer> {
    if ptr::eq(command, &HELP) {
        Some(Completer::Commands)
    } else {
        (!command.completions.is_empty()).then_some(Completer::List(command.completions))
    }
}

struct Editor {
    line_len: usize,
    cursor_pos: usize,
    line_invalid: bool,
    escape_sequence_index: u8,
    history_offset: usize,
}

pub struct Output<'a> {
    line: &'a [u8],
    editor: &'a mut Editor,
    out: &'a mut dyn Write,
}

impl Output<'_> {
    pub fn write(&mut self, data: &[u8]) {
        self.out.write(data);
    }

    pub fn print_line(&mut self, text: &[u8]) {
        erase_current_line(self.line, self.editor, text.len(), self.out);
        self.out.write(text);
    }
}

pub struct LineSnapshot<'a> {
    pub line_len: usize,
    pub cursor_pos: usize,
    pub tail: &'a [u8],
}

pub struct ActiveLine<const LINE: usize> {
    active: AtomicBool,
    line_len: AtomicUsize,
    cursor_pos: AtomicUsize,
    tail_len: AtomicUsize,
    tail: [AtomicU8; LINE],
}

impl<const LINE: usize> ActiveLine<LINE> {
    pub const fn new() -> Self {
        Self {
            active: AtomicBool::new(false),
            line_len: AtomicUsize::new(0),
            cursor_pos: AtomicUsize::new(0),
            tail_len: AtomicUsize::new(0),
            tail: [const { AtomicU8::new(0) }; LINE],
        }
    }

    pub fn is_active(&self) -> bool {
        self.active.load(Ordering::Acquire)
    }

    pub fn release(&self) {
        self.active.store(false, Ordering::Release);
    }

    pub fn capture(&self, snapshot: &LineSnapshot<'_>) {
        self.line_len.store(snapshot.line_len, Ordering::Relaxed);
        self.cursor_pos.store(snapshot.cursor_pos, Ordering::Relaxed);
        self.tail_len.store(snapshot.tail.len(), Ordering::Relaxed);
        for (slot, &byte) in self.tail.iter().zip(snapshot.tail) {
            slot.store(byte, Ordering::Relaxed);
        }
        self.active.store(true, Ordering::Release);
    }

    pub fn print_line(&self, text: &[u8], out: &mut impl Write) {
        let line_len = self.line_len.load(Ordering::Relaxed);
        if text.len() < line_len {
            if self.cursor_pos.swap(line_len, Ordering::Relaxed) != line_len {
                for slot in &self.tail[..self.tail_len.load(Ordering::Relaxed)] {
                    out.write(&[slot.load(Ordering::Relaxed)]);
                }
            }
            write_repeat(out, b"\x08", line_len - text.len());
            write_repeat(out, b" ", line_len - text.len());
        }
        out.write(b"\r");
        out.write(text);
    }
}

fn terminated(buffer: &[u8]) -> &[u8] {
    let len = buffer.iter().position(|&b| b == 0).unwrap_or(buffer.len());
    &buffer[..len]
}

fn write_repeat(out: &mut (impl Write + ?Sized), data: &[u8], count: usize) {
    for _ in 0..count {
        out.write(data);
    }
}

fn move_cursor_to_end(line: &[u8], editor: &mut Editor, out: &mut dyn Write) {
    if editor.cursor_pos == editor.line_len {
        return;
    }
    out.write(terminated(&line[editor.cursor_pos..]));
    editor.cursor_pos = editor.line_len;
}

fn erase_current_line(line: &[u8], editor: &mut Editor, new_line_len: usize, out: &mut dyn Write) {
    let line_len = editor.line_len;
    if new_line_len < line_len {
        move_cursor_to_end(line, editor, out);
        let chars_to_erase = line_len - new_line_len;
        write_repeat(out, b"\x08", chars_to_erase);
        write_repeat(out, b" ", chars_to_erase);
    }
    out.write(b"\r");
}

fn parse_int(token: &[u8]) -> Option<isize> {
    let mut index = 0;
    let negative = match token.first() {
        Some(b'-') => {
            index += 1;
            true
        }
        Some(b'+') => {
            index += 1;
            false
        }
        _ => false,
    };
    let mut base: u32 = 10;
    if token.len() > index + 1 && token[index] == b'0' && (token[index + 1] | 0x20) == b'x' {
        if token.get(index + 2).is_some_and(|c| c.is_ascii_hexdigit()) {
            base = 16;
            index += 2;
        }
    } else if token.get(index) == Some(&b'0') {
        base = 8;
    }
    let digits_start = index;
    let mut value: i64 = 0;
    let mut overflow = false;
    while let Some(digit) = token.get(index).and_then(|c| (*c as char).to_digit(base)) {
        value = value * base as i64 + digit as i64;
        if value > i32::MAX as i64 + 1 {
            overflow = true;
            value = i32::MAX as i64 + 1;
        }
        index += 1;
    }
    if index == digits_start || index != token.len() {
        return None;
    }
    let value = if negative { -value } else { value };
    if overflow || value >= i32::MAX as i64 || value <= i32::MIN as i64 {
        return None;
    }
    Some(value as isize)
}

#[derive(Clone, Copy)]
enum Slot {
    Missing,
    Int(isize),
    Str(usize, usize),
    Choice(usize),
}

fn parse_value(kind: ArgKind, token: &[u8], start: usize) -> Option<Slot> {
    match kind {
        ArgKind::Int => parse_int(token).map(Slot::Int),
        ArgKind::Str => str::from_utf8(token).is_ok().then_some(Slot::Str(start, start + token.len())),
        ArgKind::Choice(values) => values.iter().position(|value| value.as_bytes() == token).map(Slot::Choice),
    }
}

fn write_padded_desc(out: &mut dyn Write, name: &[u8], max_name_len: usize, desc: &[u8]) {
    write_repeat(out, b" ", max_name_len - name.len());
    out.write(b" - ");
    out.write(desc);
}

pub struct Console<const LINE: usize, const HISTORY: usize, const COMMANDS: usize> {
    line: [u8; LINE],
    editor: Editor,
    history: [[u8; LINE]; HISTORY],
    history_start: usize,
    history_len: usize,
    commands: Vec<&'static Command, COMMANDS>,
}

impl<const LINE: usize, const HISTORY: usize, const COMMANDS: usize> Console<LINE, HISTORY, COMMANDS> {
    pub const fn new() -> Self {
        const { assert!(LINE > 1 && HISTORY > 0 && COMMANDS > 0) };
        Self {
            line: [0; LINE],
            editor: Editor { line_len: 0, cursor_pos: 0, line_invalid: false, escape_sequence_index: 0, history_offset: 0 },
            history: [[0; LINE]; HISTORY],
            history_start: 0,
            history_len: 0,
            commands: Vec::new(),
        }
    }

    pub fn start(&mut self, out: &mut impl Write) {
        out.write(b"\n> ");
    }

    pub fn register(&mut self, command: &'static Command) -> Result<(), RegisterError> {
        if self.commands.len() + 1 >= COMMANDS {
            return Err(RegisterError::Full);
        }
        if command.name.len() >= LINE {
            return Err(RegisterError::InvalidName);
        }
        if command.args.len() > MAX_ARGS {
            return Err(RegisterError::InvalidArgs);
        }
        let mut has_optional = false;
        for arg in command.args {
            if arg.optional {
                has_optional = true;
            } else if has_optional {
                return Err(RegisterError::InvalidArgs);
            }
        }
        if self.find(command.name.as_bytes()).is_some() {
            return Err(RegisterError::Duplicate);
        }
        self.commands.push(command).map_err(|_| RegisterError::Full)
    }

    fn find(&self, name: &[u8]) -> Option<&'static Command> {
        entries(&self.commands).find(|command| command.name.as_bytes() == name)
    }

    pub fn process(&mut self, data: &[u8], out: &mut impl Write) {
        self.process_dyn(data, out);
    }

    pub fn print_line(&mut self, text: &[u8], out: &mut impl Write) {
        erase_current_line(&self.line, &mut self.editor, text.len(), out);
        out.write(text);
        out.write(PROMPT);
        if !self.editor.line_invalid {
            out.write(terminated(&self.line));
            write_repeat(out, b"\x08", self.editor.line_len - self.editor.cursor_pos);
        }
    }

    fn process_dyn(&mut self, data: &[u8], out: &mut dyn Write) {
        let mut echo: Option<usize> = None;
        for &c in data {
            let escape_sequence_index = self.editor.escape_sequence_index;
            if escape_sequence_index == 0 && c == CHAR_ESCAPE {
                self.editor.escape_sequence_index += 1;
                continue;
            } else if escape_sequence_index == 1 {
                self.editor.escape_sequence_index = if c == b'[' { 2 } else { 0 };
                continue;
            } else if escape_sequence_index == 2 {
                self.editor.escape_sequence_index = 0;
                self.handle_escape_sequence(c, out);
                continue;
            }
            let line_invalid = self.editor.line_invalid;
            if c == RETURN_KEY {
                self.flush_echo(&mut echo, out);
                out.write(NEWLINE);
                self.process_line(out);
                self.reset_line_and_print_prompt(out);
            } else if c == CHAR_CTRL_C {
                self.flush_echo(&mut echo, out);
                out.write(NEWLINE);
                self.reset_line_and_print_prompt(out);
            } else if !line_invalid && c == CHAR_BACKSPACE {
                self.flush_echo(&mut echo, out);
                self.handle_backspace(out);
            } else if !line_invalid && c == CHAR_TAB {
                self.flush_echo(&mut echo, out);
                self.do_tab_complete(out);
            } else if !line_invalid && (b' '..=b'~').contains(&c) {
                let (cursor_pos, line_len) = (self.editor.cursor_pos, self.editor.line_len);
                if cursor_pos != line_len {
                    self.flush_echo(&mut echo, out);
                    self.push_char(c);
                    out.write(terminated(&self.line[cursor_pos..]));
                    write_repeat(out, b"\x08", self.editor.line_len - self.editor.cursor_pos);
                } else {
                    if echo.is_none() {
                        echo = Some(line_len);
                    }
                    self.push_char(c);
                }
            }
        }
        self.flush_echo(&mut echo, out);
    }

    fn flush_echo(&self, echo: &mut Option<usize>, out: &mut dyn Write) {
        if let Some(start) = echo.take() {
            out.write(terminated(&self.line[start..]));
        }
    }

    fn reset_line_and_print_prompt(&mut self, out: &mut dyn Write) {
        self.editor.history_offset = 0;
        self.editor.escape_sequence_index = 0;
        self.editor.line_len = 0;
        self.editor.cursor_pos = 0;
        self.editor.line_invalid = false;
        self.line[0] = 0;
        out.write(PROMPT);
    }

    fn push_char(&mut self, c: u8) {
        let editor = &mut self.editor;
        if editor.line_len + 1 >= LINE {
            editor.line_invalid = true;
            return;
        }
        if editor.cursor_pos != editor.line_len {
            self.line.copy_within(editor.cursor_pos..editor.line_len, editor.cursor_pos + 1);
        }
        self.line[editor.cursor_pos] = c;
        editor.cursor_pos += 1;
        editor.line_len += 1;
        self.line[editor.line_len] = 0;
    }

    fn handle_backspace(&mut self, out: &mut dyn Write) {
        let (cursor_pos, line_len) = (self.editor.cursor_pos, self.editor.line_len);
        if cursor_pos == 0 {
            return;
        }
        out.write(b"\x08 \x08");
        if cursor_pos != line_len {
            self.line.copy_within(cursor_pos..line_len, cursor_pos - 1);
        }
        let editor = &mut self.editor;
        editor.cursor_pos -= 1;
        editor.line_len -= 1;
        self.line[editor.line_len] = 0;
        if editor.cursor_pos != editor.line_len {
            out.write(terminated(&self.line[editor.cursor_pos..]));
            out.write(b" ");
            write_repeat(out, b"\x08", editor.line_len - editor.cursor_pos + 1);
        }
    }

    fn handle_escape_sequence(&mut self, c: u8, out: &mut dyn Write) {
        let (cursor_pos, line_len) = (self.editor.cursor_pos, self.editor.line_len);
        match c {
            b'C' if cursor_pos < line_len => {
                out.write(&self.line[cursor_pos..cursor_pos + 1]);
                self.editor.cursor_pos += 1;
            }
            b'D' if cursor_pos != 0 => {
                out.write(b"\x08");
                self.editor.cursor_pos -= 1;
            }
            _ => {}
        }
        let offset = self.editor.history_offset;
        let new_offset = match c {
            b'A' if offset < self.history_len => offset + 1,
            b'B' if offset > 0 => offset - 1,
            _ => return,
        };
        self.editor.history_offset = new_offset;
        let index = (self.history_start + self.history_len + 1 - new_offset) % HISTORY;
        let entry = if new_offset == 0 { &[][..] } else { terminated(&self.history[index]) };
        let history_line_len = entry.len();
        erase_current_line(&self.line, &mut self.editor, history_line_len, out);
        self.line[..history_line_len].copy_from_slice(entry);
        self.line[history_line_len] = 0;
        self.editor.line_len = history_line_len;
        self.editor.cursor_pos = history_line_len;
        out.write(PROMPT);
        out.write(terminated(&self.line));
    }

    fn history_add_line(&mut self) {
        let line_len = self.editor.line_len;
        let last = (self.history_start + self.history_len) % HISTORY;
        if terminated(&self.history[last]) == terminated(&self.line) {
            return;
        }
        let write_index = (self.history_start + self.history_len + 1) % HISTORY;
        self.history[write_index][..line_len + 1].copy_from_slice(&self.line[..line_len + 1]);
        if self.history_len == HISTORY {
            self.history_start = (self.history_start + 1) % HISTORY;
        } else {
            self.history_len += 1;
        }
    }

    fn parse_line(&mut self, out: &mut dyn Write) -> Option<(&'static Command, [Slot; MAX_ARGS], usize)> {
        if self.editor.line_invalid {
            return None;
        }
        let line_len = self.editor.line_len;
        let mut command: Option<&'static Command> = None;
        let mut slots = [Slot::Missing; MAX_ARGS];
        let mut arg_index = 0;
        let mut token_start: Option<usize> = None;
        for i in 0..=line_len {
            let c = self.line[i];
            if c != b' ' && c != 0 {
                if token_start.is_none() {
                    token_start = Some(i);
                }
                continue;
            }
            let Some(start) = token_start.take() else {
                if command.is_some() {
                    out.write(b"ERROR: Extra whitespace between arguments\n");
                } else if self.line[..line_len].iter().any(|&b| b != b' ') {
                    out.write(b"ERROR: Whitespace before command\n");
                }
                return None;
            };
            if command.is_none() {
                self.history_add_line();
            }
            self.line[i] = 0;
            let token = &self.line[start..i];
            let Some(found) = command else {
                let Some(found) = self.find(token) else {
                    out.write(b"ERROR: Command not found (");
                    out.write(token);
                    out.write(b")\n");
                    return None;
                };
                command = Some(found);
                continue;
            };
            if arg_index == found.args.len() {
                out.write(b"ERROR: Too many arguments\n");
                return None;
            }
            let arg = &found.args[arg_index];
            let Some(slot) = parse_value(arg.kind, token, start) else {
                out.write(b"ERROR: Invalid value for '");
                out.write(arg.name.as_bytes());
                out.write(b"' (");
                out.write(token);
                out.write(b")\n");
                return None;
            };
            slots[arg_index] = slot;
            arg_index += 1;
        }
        command.map(|command| (command, slots, arg_index))
    }

    fn process_line(&mut self, out: &mut dyn Write) {
        let Some((command, slots, num_args)) = self.parse_line(out) else {
            return;
        };
        if num_args < command.args.iter().filter(|arg| !arg.optional).count() {
            out.write(b"ERROR: Too few arguments\n");
            return;
        }
        let line = &self.line;
        let mut args = Args { values: [Value::Missing; MAX_ARGS] };
        for (value, slot) in args.values.iter_mut().zip(slots) {
            *value = match slot {
                Slot::Missing => Value::Missing,
                Slot::Int(int) => Value::Int(int),
                Slot::Str(start, end) => CStr::from_bytes_with_nul(&line[start..=end]).map_or(Value::Missing, Value::Str),
                Slot::Choice(index) => Value::Choice(index),
            };
        }
        out.command_started(&LineSnapshot {
            line_len: self.editor.line_len,
            cursor_pos: self.editor.cursor_pos,
            tail: terminated(&line[self.editor.cursor_pos..]),
        });
        let mut output = Output { line, editor: &mut self.editor, out };
        if ptr::eq(command, &HELP) {
            help(&self.commands, &args, output.out);
        } else {
            (command.handler)(&args, &mut output);
        }
        out.command_finished();
    }

    fn for_each_completion(&self, completer: Completer, mut f: impl FnMut(&'static [u8])) {
        match completer {
            Completer::Commands => entries(&self.commands).for_each(|command| f(command.name.as_bytes())),
            Completer::List(values) => values.iter().for_each(|value| f(value.as_bytes())),
        }
    }

    fn do_tab_complete(&mut self, out: &mut dyn Write) {
        let line_len = self.editor.line_len;
        self.line[line_len] = 0;
        let mut offset = 0;
        let mut completer = Completer::Commands;
        for command in entries(&self.commands) {
            let name = command.name.as_bytes();
            let line = &self.line;
            if line_len < name.len() + 1 || &line[..name.len()] != name || line[name.len()] != b' ' {
                continue;
            }
            if let Some(command_completer) = completer_for(command) {
                completer = command_completer;
                offset = name.len() + 1;
            }
            break;
        }

        let mut num_matches = 0;
        let mut longest_common_prefix = 0;
        let mut first: &'static [u8] = b"";
        {
            let prefix = &self.line[offset..line_len];
            self.for_each_completion(completer, |candidate| {
                if !candidate.starts_with(prefix) {
                    return;
                }
                if num_matches > 0 {
                    let candidate_with_nul = candidate.iter().chain(iter::repeat(&0));
                    if let Some(j) = first[..longest_common_prefix].iter().zip(candidate_with_nul).position(|(a, b)| a != b) {
                        longest_common_prefix = j;
                    }
                } else {
                    first = candidate;
                    longest_common_prefix = candidate.len();
                }
                num_matches += 1;
            });
        }
        let mut completion_length = longest_common_prefix.wrapping_sub(line_len - offset);
        let max_completion_length = LINE - 1 - line_len;
        if completion_length > max_completion_length {
            completion_length = max_completion_length;
        }
        if num_matches == 0 || (num_matches == 1 && completion_length == 0) {
            return;
        }

        move_cursor_to_end(&self.line, &mut self.editor, out);
        if completion_length != 0 {
            let src_start = line_len - offset;
            self.line[line_len..line_len + completion_length].copy_from_slice(&first[src_start..src_start + completion_length]);
            self.line[line_len + completion_length] = 0;
            out.write(&self.line[line_len..line_len + completion_length]);
            self.editor.line_len += completion_length;
            self.editor.cursor_pos = self.editor.line_len;
        } else {
            out.write(NEWLINE);
            let prefix = &self.line[offset..line_len];
            self.for_each_completion(completer, |candidate| {
                if !candidate.starts_with(prefix) {
                    return;
                }
                if !(candidate.as_ptr() == first.as_ptr() && candidate.len() == first.len()) {
                    out.write(b" ");
                }
                out.write(candidate);
            });
            out.write(NEWLINE);
            out.write(PROMPT);
            if !self.editor.line_invalid {
                out.write(terminated(&self.line));
            }
        }
    }
}

fn entries<'a, const COMMANDS: usize>(commands: &'a Vec<&'static Command, COMMANDS>) -> impl Iterator<Item = &'static Command> + 'a {
    iter::once(&HELP).chain(commands.iter().copied())
}

fn help<const COMMANDS: usize>(commands: &Vec<&'static Command, COMMANDS>, args: &Args<'_>, out: &mut dyn Write) {
    let Some(name) = args.optional_str(0) else {
        out.write(b"Available commands:\n");
        let max_name_len = entries(commands).map(|command| command.name.len()).max().unwrap_or(0);
        for command in entries(commands) {
            out.write(b"  ");
            out.write(command.name.as_bytes());
            write_padded_desc(out, command.name.as_bytes(), max_name_len, command.desc.as_bytes());
            out.write(NEWLINE);
        }
        return;
    };
    let name = name.as_bytes();
    let Some(command) = entries(commands).find(|command| command.name.as_bytes() == name) else {
        out.write(b"ERROR: Unknown command (");
        out.write(name);
        out.write(b")\n");
        return;
    };
    out.write(command.desc.as_bytes());
    out.write(NEWLINE);
    out.write(b"Usage: ");
    out.write(name);
    let mut max_name_len = 0;
    for arg in command.args {
        max_name_len = max_name_len.max(arg.name.len());
        out.write(if arg.optional { b" [" } else { b" " });
        out.write(arg.name.as_bytes());
        if arg.optional {
            out.write(b"]");
        }
    }
    out.write(NEWLINE);
    for arg in command.args {
        out.write(b"  ");
        out.write(arg.name.as_bytes());
        write_padded_desc(out, arg.name.as_bytes(), max_name_len, arg.desc.as_bytes());
        out.write(NEWLINE);
    }
}
