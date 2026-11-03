#![cfg_attr(not(test), no_std)]

use core::ffi::CStr;

pub use anchor_console_macros::console_command;
pub use engine::{ActiveLine, Console, LineSnapshot, Output};

mod engine;

pub const MAX_ARGS: usize = 8;

pub trait Write {
    fn write(&mut self, data: &[u8]);

    fn command_started(&mut self, _line: &LineSnapshot<'_>) {}

    fn command_finished(&mut self) {}
}

impl<F: FnMut(&[u8])> Write for F {
    fn write(&mut self, data: &[u8]) {
        self(data)
    }
}

#[derive(Clone, Copy)]
pub enum ArgKind {
    Int,
    Str,
    Choice(&'static [&'static str]),
}

#[derive(Clone, Copy)]
pub struct ArgSpec {
    pub name: &'static str,
    pub desc: &'static str,
    pub kind: ArgKind,
    pub optional: bool,
}

impl ArgSpec {
    pub const fn int(name: &'static str, desc: &'static str) -> Self {
        Self { name, desc, kind: ArgKind::Int, optional: false }
    }

    pub const fn str(name: &'static str, desc: &'static str) -> Self {
        Self { name, desc, kind: ArgKind::Str, optional: false }
    }

    pub const fn choice(name: &'static str, desc: &'static str, values: &'static [&'static str]) -> Self {
        Self { name, desc, kind: ArgKind::Choice(values), optional: false }
    }

    pub const fn optional(self) -> Self {
        Self { optional: true, ..self }
    }
}

pub struct Command {
    pub name: &'static str,
    pub desc: &'static str,
    pub args: &'static [ArgSpec],
    pub completions: &'static [&'static str],
    pub handler: fn(&Args<'_>, &mut Output<'_>),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RegisterError {
    Full,
    InvalidName,
    InvalidArgs,
    Duplicate,
}

#[derive(Clone, Copy)]
enum Value<'a> {
    Missing,
    Int(isize),
    Str(&'a CStr),
    Choice(usize),
}

/// Typed wrapper for accessing parsed command arguments.
pub struct Args<'a> {
    values: [Value<'a>; MAX_ARGS],
}

impl<'a> Args<'a> {
    pub fn int(&self, index: usize) -> isize {
        self.optional_int(index).expect("missing int argument")
    }

    pub fn str(&self, index: usize) -> &'a str {
        self.optional_str(index).expect("missing str argument")
    }

    pub fn choice(&self, index: usize) -> usize {
        match self.values[index] {
            Value::Choice(value) => value,
            _ => panic!("missing choice argument"),
        }
    }

    pub fn optional_int(&self, index: usize) -> Option<isize> {
        match self.values[index] {
            Value::Int(value) => Some(value),
            _ => None,
        }
    }

    pub fn c_str(&self, index: usize) -> &'a CStr {
        self.optional_c_str(index).expect("missing str argument")
    }

    pub fn optional_str(&self, index: usize) -> Option<&'a str> {
        self.optional_c_str(index).and_then(|value| value.to_str().ok())
    }

    pub fn optional_c_str(&self, index: usize) -> Option<&'a CStr> {
        match self.values[index] {
            Value::Str(value) => Some(value),
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use std::vec::Vec;

    use super::*;

    const ARGS: &[ArgSpec] = &[ArgSpec::int("value", "The value"), ArgSpec::str("name", "Optional name").optional()];

    fn foo(_args: &Args<'_>, out: &mut Output<'_>) {
        out.print_line(b"foo handler log\n");
    }

    static FOO: Command = Command { name: "foo", desc: "Foo the bar", args: &[], completions: &[], handler: foo };
    static BAD: Command = Command {
        name: "bad",
        desc: "",
        args: &[ArgSpec::str("a", "").optional(), ArgSpec::int("b", "")],
        completions: &[],
        handler: foo,
    };
    static SET: Command = Command { name: "set", desc: "Sets", args: ARGS, completions: &[], handler: foo };

    #[test]
    fn matches_anchor_c_session() {
        let mut console = Console::<128, 4, 3>::new();
        let mut output = Vec::new();
        let mut out = |data: &[u8]| output.extend_from_slice(data);
        console.start(&mut out);
        assert_eq!(console.register(&FOO), Ok(()));
        assert_eq!(console.register(&FOO), Err(RegisterError::Duplicate));
        assert_eq!(console.register(&BAD), Err(RegisterError::InvalidArgs));
        assert_eq!(console.register(&SET), Ok(()));
        assert_eq!(console.register(&BAD), Err(RegisterError::Full));
        console.process(b"fo\t\x08o\n\x1b[A\x03", &mut out);
        assert_eq!(output.as_slice(), b"\n> foo\x08 \x08o\n\rfoo handler log\n> \r> foo\n> ");
    }
}
