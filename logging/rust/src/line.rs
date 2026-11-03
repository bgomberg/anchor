const MS_PER_DAY: u64 = 86_400_000;
const EPOCH_YEAR: u16 = 1970;
const DAYS_PER_MONTH: [u8; 12] = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum Level {
    Default = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
}

impl Level {
    pub fn from_raw(value: u8) -> Self {
        match value {
            1 => Self::Debug,
            2 => Self::Info,
            3 => Self::Warn,
            4 => Self::Error,
            _ => Self::Default,
        }
    }

    const fn prefix(self) -> &'static [u8] {
        match self {
            Self::Default => b"????? ",
            Self::Debug => b"DEBUG ",
            Self::Info => b"INFO  ",
            Self::Warn => b"WARN  ",
            Self::Error => b"ERROR ",
        }
    }
}

pub struct LogLine<'a> {
    pub level: Level,
    pub file: &'a [u8],
    pub line: i32,
    pub module_prefix: Option<&'a [u8]>,
    pub timestamp: Option<u64>,
    pub msg: &'a [u8],
}

pub struct LineWriter<'a> {
    buffer: &'a mut [u8],
    len: usize,
}

impl<'a> LineWriter<'a> {
    pub fn new(buffer: &'a mut [u8]) -> Self {
        if let Some(first) = buffer.first_mut() {
            *first = 0;
        }
        Self { buffer, len: 0 }
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn is_full(&self) -> bool {
        self.len + 1 >= self.buffer.len()
    }

    pub fn push(&mut self, data: &[u8]) {
        if self.buffer.is_empty() {
            return;
        }
        let available = self.buffer.len() - 1 - self.len;
        let count = data.len().min(available);
        self.buffer[self.len..self.len + count].copy_from_slice(&data[..count]);
        self.len += count;
        self.buffer[self.len] = 0;
    }

    pub fn push_unsigned(&mut self, value: u64, min_width: usize) {
        let mut digits = [0u8; 20];
        let mut count = 0;
        let mut remaining = value;
        while remaining > u32::MAX as u64 {
            digits[digits.len() - 1 - count] = b'0' + (remaining % 10) as u8;
            count += 1;
            remaining /= 10;
        }
        let mut remaining = remaining as u32;
        loop {
            digits[digits.len() - 1 - count] = b'0' + (remaining % 10) as u8;
            count += 1;
            remaining /= 10;
            if remaining == 0 {
                break;
            }
        }
        for _ in count..min_width {
            self.push(b"0");
        }
        self.push(&digits[digits.len() - count..]);
    }

    pub fn push_signed(&mut self, value: i64) {
        if value < 0 {
            self.push(b"-");
        }
        self.push_unsigned(value.unsigned_abs(), 0);
    }

    fn truncate_last(&mut self) {
        if self.len == 0 {
            return;
        }
        self.len -= 1;
        self.buffer[self.len] = 0;
    }
}

struct TimestampComponents {
    year: u16,
    month: u8,
    day: u8,
    hour: u32,
    minute: u32,
    second: u32,
    ms: u32,
}

fn is_leap_year(year: u16) -> bool {
    (year.is_multiple_of(4) && !year.is_multiple_of(100)) || year.is_multiple_of(400)
}

fn timestamp_components(timestamp: u64) -> TimestampComponents {
    let days_since_epoch = timestamp / MS_PER_DAY;
    let mut seconds_timestamp = (timestamp - days_since_epoch * MS_PER_DAY) as u32;
    let ms = seconds_timestamp % 1000;
    seconds_timestamp /= 1000;
    let second = seconds_timestamp % 60;
    seconds_timestamp /= 60;
    let minute = seconds_timestamp % 60;
    seconds_timestamp /= 60;
    let hour = seconds_timestamp;

    let mut days = (days_since_epoch + 1) as u32;
    let mut days_in_year = 365;
    let mut year = EPOCH_YEAR;
    while days > days_in_year {
        days -= days_in_year;
        year += 1;
        days_in_year = if is_leap_year(year) { 366 } else { 365 };
    }

    let mut month = 0;
    for (index, &days_in_month) in DAYS_PER_MONTH.iter().enumerate() {
        let days_in_month = if index == 1 && is_leap_year(year) { 29 } else { days_in_month } as u32;
        if days <= days_in_month {
            month = index as u8 + 1;
            break;
        }
        days -= days_in_month;
    }

    TimestampComponents {
        year,
        month,
        day: days as u8,
        hour,
        minute,
        second,
        ms,
    }
}

pub fn push_timestamp(writer: &mut LineWriter, timestamp: u64) {
    let components = timestamp_components(timestamp);
    writer.push_unsigned(components.year as u64, 4);
    writer.push(b"-");
    writer.push_unsigned(components.month as u64, 2);
    writer.push(b"-");
    writer.push_unsigned(components.day as u64, 2);
    writer.push(b" ");
    writer.push_unsigned(components.hour as u64, 2);
    writer.push(b":");
    writer.push_unsigned(components.minute as u64, 2);
    writer.push(b":");
    writer.push_unsigned(components.second as u64, 2);
    writer.push(b".");
    writer.push_unsigned(components.ms as u64, 3);
    writer.push(b" ");
}

pub fn format_log_line(line: &LogLine, out: &mut [u8]) -> usize {
    let mut writer = LineWriter::new(out);
    if let Some(timestamp) = line.timestamp {
        push_timestamp(&mut writer, timestamp);
    }
    writer.push(line.level.prefix());
    if let Some(module_prefix) = line.module_prefix {
        writer.push(module_prefix);
    }
    writer.push(line.file);
    writer.push(b":");
    writer.push_signed(line.line as i64);
    writer.push(b": ");
    writer.push(line.msg);
    if writer.is_full() {
        writer.truncate_last();
    }
    writer.push(b"\n");
    writer.len()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn formats_like_anchor() {
        let mut out = [0u8; 64];
        let line = LogLine {
            level: Level::Info,
            file: b"app",
            line: 65,
            module_prefix: None,
            timestamp: Some(1_709_164_800_321),
            msg: b"Booted",
        };
        let len = format_log_line(&line, &mut out);
        assert_eq!(&out[..len], b"2024-02-29 00:00:00.321 INFO  app:65: Booted\n");
        let len = format_log_line(&line, &mut out[..40]);
        assert_eq!(&out[..len], b"2024-02-29 00:00:00.321 INFO  app:65: \n");
    }
}
