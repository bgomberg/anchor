use heapless::Deque;

use crate::line::{Level, LogLine, format_log_line};

pub struct Record<'a> {
    pub level: Level,
    pub file: &'static str,
    pub line: i32,
    pub timestamp: u64,
    pub msg: &'a [u8],
}

impl Record<'_> {
    pub fn format(&self, out: &mut [u8]) -> usize {
        let log_line = LogLine {
            level: self.level,
            file: self.file.as_bytes(),
            line: self.line,
            module_prefix: None,
            timestamp: Some(self.timestamp),
            msg: self.msg,
        };
        format_log_line(&log_line, out)
    }
}

struct Header {
    level: Level,
    file: &'static str,
    line: i32,
    timestamp: u64,
    msg_len: u16,
}

pub struct RecordRing<const RECORDS: usize, const BYTES: usize> {
    headers: Deque<Header, RECORDS>,
    bytes: Deque<u8, BYTES>,
}

impl<const RECORDS: usize, const BYTES: usize> RecordRing<RECORDS, BYTES> {
    pub const fn new() -> Self {
        Self { headers: Deque::new(), bytes: Deque::new() }
    }

    pub fn len(&self) -> usize {
        self.headers.len()
    }

    pub fn is_empty(&self) -> bool {
        self.headers.is_empty()
    }

    pub fn push(&mut self, record: &Record) -> bool {
        assert!(record.msg.len() <= BYTES && record.msg.len() <= u16::MAX as usize, "log message longer than the ring");
        if self.headers.is_full() || BYTES - self.bytes.len() < record.msg.len() {
            return false;
        }
        for &byte in record.msg {
            let _ = self.bytes.push_back(byte);
        }
        let header = Header {
            level: record.level,
            file: record.file,
            line: record.line,
            timestamp: record.timestamp,
            msg_len: record.msg.len() as u16,
        };
        self.headers.push_back(header).is_ok()
    }

    pub fn pop<'a>(&mut self, msg: &'a mut [u8]) -> Option<Record<'a>> {
        let header = self.headers.pop_front()?;
        let len = header.msg_len as usize;
        assert!(msg.len() >= len, "log message buffer too small");
        for byte in &mut msg[..len] {
            *byte = self.bytes.pop_front()?;
        }
        Some(Record {
            level: header.level,
            file: header.file,
            line: header.line,
            timestamp: header.timestamp,
            msg: &msg[..len],
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn drops_when_full_and_pops_in_order() {
        let mut ring = RecordRing::<2, 16>::new();
        let record = |msg| Record { level: Level::Debug, file: "ring.rs", line: 7, timestamp: 1_000, msg };
        assert!(ring.push(&record(b"first")));
        assert!(!ring.push(&record(b"twelve bytes")));
        assert!(ring.push(&record(b"second")));
        assert!(!ring.push(&record(b"third")));
        let mut msg = [0u8; 32];
        let mut line = [0u8; 64];
        let len = ring.pop(&mut msg).unwrap().format(&mut line);
        assert_eq!(&line[..len], b"1970-01-01 00:00:01.000 DEBUG ring.rs:7: first\n");
        assert_eq!(ring.pop(&mut msg).unwrap().msg, b"second");
        assert!(ring.pop(&mut msg).is_none());
        assert!(ring.push(&record(b"wraps around")));
        assert_eq!(ring.pop(&mut msg).unwrap().msg, b"wraps around");
    }
}
