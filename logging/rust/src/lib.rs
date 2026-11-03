#![cfg_attr(not(test), no_std)]

mod line;
mod ring;

pub use line::{Level, LineWriter, LogLine, format_log_line, push_timestamp};
pub use ring::{Record, RecordRing};
