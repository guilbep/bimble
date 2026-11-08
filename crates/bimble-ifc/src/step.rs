//! Minimal ISO 10303-21 (STEP physical file) parser, enough to read IFC.
//!
//! Input is untrusted: nesting depth is bounded, ids are validated, and errors are reported
//! with a line number instead of panicking.

use rayon::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;

/// An upper-case STEP keyword (entity, type or enumeration name), shared between all instances
/// that use it: a 50 MB file has millions of them but only hundreds of distinct ones, and one
/// allocation each made parsing and freeing the file twice as slow.
#[derive(Clone, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Name(Arc<str>);

impl Name {
    pub fn as_str(&self) -> &str {
        &self.0
    }
}
impl std::ops::Deref for Name {
    type Target = str;
    fn deref(&self) -> &str {
        &self.0
    }
}
impl From<&str> for Name {
    fn from(s: &str) -> Self {
        Name(s.into())
    }
}
impl PartialEq<str> for Name {
    fn eq(&self, o: &str) -> bool {
        &*self.0 == o
    }
}
impl PartialEq<&str> for Name {
    fn eq(&self, o: &&str) -> bool {
        &*self.0 == *o
    }
}
impl std::fmt::Display for Name {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}
impl std::fmt::Debug for Name {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        std::fmt::Debug::fmt(&*self.0, f)
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Null,
    Derived,
    Int(i64),
    Real(f64),
    Str(Box<str>),
    Enum(Name),
    Ref(u64),
    /// Exactly sized: a parsed file is millions of short lists (points, loops, faces).
    List(Box<[Value]>),
    /// Typed parameter, e.g. IFCLABEL('x') → ("IFCLABEL", Str("x")). Boxed: it is rare, and
    /// keeps every value at 24 bytes.
    Typed(Box<(Name, Value)>),
}

impl Value {
    /// A typed parameter's type name and value.
    pub fn as_typed(&self) -> Option<(&str, &Value)> {
        if let Value::Typed(t) = self {
            Some((t.0.as_str(), &t.1))
        } else {
            None
        }
    }
}

impl Value {
    pub fn as_ref(&self) -> Option<u64> {
        if let Value::Ref(r) = self {
            Some(*r)
        } else {
            None
        }
    }
    pub fn as_f64(&self) -> Option<f64> {
        match self {
            Value::Real(x) => Some(*x),
            Value::Int(i) => Some(*i as f64),
            Value::Typed(t) => t.1.as_f64(),
            _ => None,
        }
    }
    pub fn as_i64(&self) -> Option<i64> {
        match self {
            Value::Int(i) => Some(*i),
            Value::Typed(t) => t.1.as_i64(),
            _ => None,
        }
    }
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::Str(s) => Some(s),
            Value::Typed(t) => t.1.as_str(),
            _ => None,
        }
    }
    pub fn as_enum(&self) -> Option<&str> {
        if let Value::Enum(s) = self {
            Some(s)
        } else {
            None
        }
    }
    pub fn as_list(&self) -> Option<&[Value]> {
        if let Value::List(l) = self {
            Some(l)
        } else {
            None
        }
    }
}

#[derive(Debug, Clone)]
pub struct Entity {
    /// Upper-case entity name, e.g. "IFCWALL"
    pub name: Name,
    pub args: Box<[Value]>,
}

impl Entity {
    pub fn arg(&self, i: usize) -> &Value {
        self.args.get(i).unwrap_or(&Value::Null)
    }
}

#[derive(Debug, Default)]
pub struct StepFile {
    /// Schema names from FILE_SCHEMA, e.g. ["IFC4"]
    pub schemas: Vec<String>,
    pub entities: Entities,
}

/// Entity instances by id. Ids are usually dense (1, 2, 3…): then a table indexed by id, 32
/// bytes a slot and no hashing; a hash map when they are sparse (a file numbered by tens of
/// thousands, or a hostile id near u64::MAX).
#[derive(Debug)]
pub enum Entities {
    Dense {
        slots: Vec<Option<Entity>>,
        len: usize,
    },
    Sparse(HashMap<u64, Entity>),
}

impl Default for Entities {
    fn default() -> Self {
        Entities::Sparse(HashMap::new())
    }
}

impl Entities {
    /// From instances in file order: a later duplicate id wins, as in a serial parse.
    fn from_ordered(parts: impl IntoIterator<Item = Vec<(u64, Entity)>>, count: usize) -> Self {
        let mut parts: Vec<Vec<(u64, Entity)>> = parts.into_iter().collect();
        let max = parts.iter().flatten().map(|(id, _)| *id).max().unwrap_or(0);
        // Dense when no more than about half the slots would be empty.
        let dense = usize::try_from(max).is_ok_and(|m| m <= count.saturating_mul(2) + 1024);
        if dense {
            let mut slots: Vec<Option<Entity>> = Vec::new();
            slots.resize_with(max as usize + 1, || None);
            let mut len = 0;
            for part in parts.drain(..) {
                for (id, e) in part {
                    let s = &mut slots[id as usize];
                    len += usize::from(s.is_none());
                    *s = Some(e);
                }
            }
            Entities::Dense { slots, len }
        } else {
            let mut m = HashMap::with_capacity(count);
            for part in parts.drain(..) {
                m.extend(part);
            }
            Entities::Sparse(m)
        }
    }

    pub fn get(&self, id: &u64) -> Option<&Entity> {
        match self {
            Entities::Dense { slots, .. } => slots
                .get(usize::try_from(*id).ok()?)
                .and_then(Option::as_ref),
            Entities::Sparse(m) => m.get(id),
        }
    }

    pub fn len(&self) -> usize {
        match self {
            Entities::Dense { len, .. } => *len,
            Entities::Sparse(m) => m.len(),
        }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// (id, entity), in no particular order.
    pub fn iter(&self) -> Box<dyn Iterator<Item = (u64, &Entity)> + Send + '_> {
        match self {
            Entities::Dense { slots, .. } => Box::new(
                slots
                    .iter()
                    .enumerate()
                    .filter_map(|(i, e)| e.as_ref().map(|e| (i as u64, e))),
            ),
            Entities::Sparse(m) => Box::new(m.iter().map(|(k, e)| (*k, e))),
        }
    }

    pub fn keys(&self) -> impl Iterator<Item = u64> + '_ {
        self.iter().map(|(k, _)| k)
    }
}

impl std::ops::Index<&u64> for Entities {
    type Output = Entity;
    fn index(&self, id: &u64) -> &Entity {
        self.get(id).expect("an entity of this file")
    }
}

impl StepFile {
    pub fn get(&self, id: u64) -> Option<&Entity> {
        self.entities.get(&id)
    }
    /// Entity behind a Ref value, if any.
    pub fn deref(&self, v: &Value) -> Option<&Entity> {
        v.as_ref().and_then(|r| self.get(r))
    }
    pub fn by_type<'a>(&'a self, name: &'a str) -> impl Iterator<Item = (u64, &'a Entity)> + 'a {
        self.entities.iter().filter(move |(_, e)| e.name == name)
    }
}

#[derive(Debug, PartialEq)]
pub struct ParseError {
    pub line: usize,
    pub msg: String,
}

impl std::fmt::Display for ParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "line {}: {}", self.line, self.msg)
    }
}

impl std::error::Error for ParseError {}

const MAX_DEPTH: usize = 64;

struct Parser<'a> {
    s: &'a [u8],
    i: usize,
    line: usize,
    /// Keywords seen so far, by their raw bytes.
    names: HashMap<Vec<u8>, Name>,
}

impl<'a> Parser<'a> {
    fn err<T>(&self, msg: impl Into<String>) -> Result<T, ParseError> {
        Err(ParseError {
            line: self.line,
            msg: msg.into(),
        })
    }

    fn peek(&self) -> Option<u8> {
        self.s.get(self.i).copied()
    }

    fn skip_ws(&mut self) {
        while let Some(c) = self.peek() {
            if c == b'\n' {
                self.line += 1;
                self.i += 1;
            } else if c.is_ascii_whitespace() {
                self.i += 1;
            } else if c == b'/' && self.s.get(self.i + 1) == Some(&b'*') {
                self.i += 2;
                while self.i < self.s.len()
                    && !(self.s[self.i] == b'*' && self.s.get(self.i + 1) == Some(&b'/'))
                {
                    if self.s[self.i] == b'\n' {
                        self.line += 1;
                    }
                    self.i += 1;
                }
                self.i = (self.i + 2).min(self.s.len());
            } else {
                break;
            }
        }
    }

    fn expect(&mut self, c: u8) -> Result<(), ParseError> {
        self.skip_ws();
        if self.peek() == Some(c) {
            self.i += 1;
            Ok(())
        } else {
            self.err(format!(
                "expected '{}', found {:?}",
                c as char,
                self.peek().map(|b| b as char)
            ))
        }
    }

    fn keyword(&mut self) -> String {
        self.skip_ws();
        let start = self.i;
        while let Some(c) = self.peek() {
            if c.is_ascii_alphanumeric() || c == b'_' {
                self.i += 1;
            } else {
                break;
            }
        }
        String::from_utf8_lossy(&self.s[start..self.i]).to_ascii_uppercase()
    }

    /// Like `keyword`, interned.
    fn keyword_name(&mut self) -> Name {
        self.skip_ws();
        let start = self.i;
        while let Some(c) = self.peek() {
            if c.is_ascii_alphanumeric() || c == b'_' {
                self.i += 1;
            } else {
                break;
            }
        }
        self.intern(start, self.i)
    }

    fn intern(&mut self, start: usize, end: usize) -> Name {
        let raw = &self.s[start..end];
        if let Some(n) = self.names.get(raw) {
            return n.clone();
        }
        let n = Name::from(String::from_utf8_lossy(raw).to_ascii_uppercase().as_str());
        self.names.insert(raw.to_vec(), n.clone());
        n
    }

    fn number(&mut self) -> Result<Value, ParseError> {
        let start = self.i;
        while let Some(c) = self.peek() {
            if c.is_ascii_digit() || matches!(c, b'+' | b'-' | b'.' | b'E' | b'e') {
                self.i += 1;
            } else {
                break;
            }
        }
        let t = std::str::from_utf8(&self.s[start..self.i]).unwrap_or("");
        if t.contains(['.', 'E', 'e']) {
            match t.parse::<f64>() {
                Ok(x) => Ok(Value::Real(x)),
                Err(_) => self.err(format!("bad real {t:?}")),
            }
        } else {
            match t.parse::<i64>() {
                Ok(x) => Ok(Value::Int(x)),
                Err(_) => self.err(format!("bad integer {t:?}")),
            }
        }
    }

    fn string(&mut self) -> Result<String, ParseError> {
        // At opening quote.
        self.i += 1;
        let mut raw = Vec::new();
        loop {
            match self.peek() {
                None => return self.err("unterminated string"),
                Some(b'\'') if self.s.get(self.i + 1) == Some(&b'\'') => {
                    raw.push(b'\'');
                    self.i += 2;
                }
                Some(b'\'') => {
                    self.i += 1;
                    break;
                }
                Some(c) => {
                    if c == b'\n' {
                        self.line += 1;
                    }
                    raw.push(c);
                    self.i += 1;
                }
            }
        }
        Ok(decode_step_string(&raw))
    }

    fn value(&mut self, depth: usize) -> Result<Value, ParseError> {
        if depth > MAX_DEPTH {
            return self.err("nesting too deep");
        }
        self.skip_ws();
        match self.peek() {
            Some(b'$') => {
                self.i += 1;
                Ok(Value::Null)
            }
            Some(b'*') => {
                self.i += 1;
                Ok(Value::Derived)
            }
            Some(b'#') => {
                self.i += 1;
                let start = self.i;
                while self.peek().is_some_and(|c| c.is_ascii_digit()) {
                    self.i += 1;
                }
                match std::str::from_utf8(&self.s[start..self.i])
                    .ok()
                    .and_then(|t| t.parse().ok())
                {
                    Some(r) => Ok(Value::Ref(r)),
                    None => self.err("bad reference"),
                }
            }
            Some(b'\'') => Ok(Value::Str(self.string()?.into_boxed_str())),
            Some(b'"') => {
                // Binary: keep the hex digits as text.
                self.i += 1;
                let start = self.i;
                while self.peek().is_some_and(|c| c != b'"') {
                    self.i += 1;
                }
                let t = String::from_utf8_lossy(&self.s[start..self.i]).into();
                self.i += 1;
                Ok(Value::Str(t))
            }
            Some(b'.') => {
                self.i += 1;
                let start = self.i;
                while self.peek().is_some_and(|c| c != b'.') {
                    self.i += 1;
                }
                let t = self.intern(start, self.i);
                self.i += 1;
                Ok(Value::Enum(t))
            }
            Some(b'(') => {
                self.i += 1;
                let items = self.items(depth + 1)?;
                Ok(Value::List(items))
            }
            Some(c) if c.is_ascii_digit() || c == b'-' || c == b'+' => self.number(),
            Some(c) if c.is_ascii_alphabetic() => {
                let name = self.keyword_name();
                self.expect(b'(')?;
                let items = self.items(depth + 1)?;
                let inner = if items.len() == 1 {
                    items.into_vec().pop().unwrap_or(Value::Null)
                } else {
                    Value::List(items)
                };
                Ok(Value::Typed(Box::new((name, inner))))
            }
            other => self.err(format!("unexpected {:?}", other.map(|b| b as char))),
        }
    }

    /// Comma-separated values up to and including ')'.
    fn items(&mut self, depth: usize) -> Result<Box<[Value]>, ParseError> {
        let mut out = Vec::new();
        self.skip_ws();
        if self.peek() == Some(b')') {
            self.i += 1;
            return Ok(out.into_boxed_slice());
        }
        loop {
            out.push(self.value(depth)?);
            self.skip_ws();
            match self.peek() {
                Some(b',') => self.i += 1,
                Some(b')') => {
                    self.i += 1;
                    // Exact size: a Vec grown by pushes keeps up to twice what it holds.
                    return Ok(out.into_boxed_slice());
                }
                other => {
                    return self.err(format!(
                        "expected ',' or ')', found {:?}",
                        other.map(|b| b as char)
                    ));
                }
            }
        }
    }

    /// Skip to just after the next ';' outside strings (error recovery).
    fn skip_statement(&mut self) {
        while let Some(c) = self.peek() {
            self.i += 1;
            match c {
                b'\'' => {
                    while let Some(d) = self.peek() {
                        self.i += 1;
                        if d == b'\'' {
                            break;
                        }
                    }
                }
                b'\n' => self.line += 1,
                b';' => return,
                _ => {}
            }
        }
    }
}

/// Decode STEP string escapes: \X2\…\X0\ (UTF-16 hex), \X4\…\X0\ (UTF-32 hex), \X\hh (ISO 8859-1),
/// \S\c (high half of ISO 8859-1), \\ (backslash). Unknown escapes are kept as-is.
pub fn decode_step_string(raw: &[u8]) -> String {
    let mut out = String::new();
    let mut i = 0;
    let hex = |b: &[u8]| {
        std::str::from_utf8(b)
            .ok()
            .and_then(|t| u32::from_str_radix(t, 16).ok())
    };
    while i < raw.len() {
        let rest = &raw[i..];
        if rest.starts_with(b"\\X2\\") || rest.starts_with(b"\\X4\\") {
            let width = if rest[2] == b'2' { 4 } else { 8 };
            let mut j = i + 4;
            let mut units: Vec<u32> = Vec::new();
            while j + width <= raw.len() && !raw[j..].starts_with(b"\\X0\\") {
                match hex(&raw[j..j + width]) {
                    Some(u) => units.push(u),
                    None => break,
                }
                j += width;
            }
            if width == 4 {
                let u16s: Vec<u16> = units.iter().map(|&u| u as u16).collect();
                out.push_str(&String::from_utf16_lossy(&u16s));
            } else {
                out.extend(units.iter().filter_map(|&u| char::from_u32(u)));
            }
            i = if raw[j..].starts_with(b"\\X0\\") {
                j + 4
            } else {
                j
            };
        } else if rest.starts_with(b"\\X\\") && rest.len() >= 5 {
            if let Some(c) = hex(&rest[3..5]).and_then(char::from_u32) {
                out.push(c);
            }
            i += 5;
        } else if rest.starts_with(b"\\S\\") && rest.len() >= 4 {
            if let Some(c) = char::from_u32(rest[3] as u32 + 128) {
                out.push(c);
            }
            i += 4;
        } else if rest.starts_with(b"\\\\") {
            out.push('\\');
            i += 2;
        } else {
            // Raw bytes: accept UTF-8 even though STEP says ASCII (many exporters write UTF-8).
            let end = raw[i..]
                .iter()
                .position(|&b| b == b'\\')
                .map_or(raw.len(), |p| i + p.max(1));
            out.push_str(&String::from_utf8_lossy(&raw[i..end]));
            i = end;
        }
    }
    out
}

/// Parse a STEP file. Malformed entity instances are skipped and counted in `skipped`.
pub fn parse(text: &str) -> Result<(StepFile, usize), ParseError> {
    parse_chunked(text, CHUNK_BYTES, &[])
}

/// Like `parse`, without the instances of the entity types in `skip` (upper case): they are
/// stepped over, never parsed or stored. Property sets can be most of a file.
pub fn parse_skipping(text: &str, skip: &[&str]) -> Result<(StepFile, usize), ParseError> {
    parse_chunked(text, CHUNK_BYTES, skip)
}

fn parse_chunked(
    text: &str,
    chunk_bytes: usize,
    skip: &[&str],
) -> Result<(StepFile, usize), ParseError> {
    let mut p = Parser {
        s: text.as_bytes(),
        i: 0,
        line: 1,
        names: HashMap::new(),
    };
    let mut file = StepFile::default();
    let mut skipped = 0;

    p.skip_ws();
    if p.keyword() != "ISO" {
        return p.err("not a STEP file (missing ISO-10303-21 header)");
    }
    // Header: find FILE_SCHEMA, then DATA.
    let data_pos = match text.find("\nDATA;").or_else(|| text.find("DATA;")) {
        Some(x) => x,
        None => return p.err("no DATA section"),
    };
    let header = &text[..data_pos];
    if let Some(fs) = header.find("FILE_SCHEMA") {
        let mut hp = Parser {
            s: header.as_bytes(),
            i: fs + "FILE_SCHEMA".len(),
            line: 1,
            names: HashMap::new(),
        };
        if hp.expect(b'(').is_ok()
            && let Ok(v) = hp.items(1)
        {
            let mut stack = v.into_vec();
            while let Some(x) = stack.pop() {
                match x {
                    Value::Str(s) => file.schemas.push(s.to_ascii_uppercase()),
                    Value::List(l) => stack.extend(l.into_vec()),
                    _ => {}
                }
            }
        }
    }
    p.line = text[..data_pos].matches('\n').count() + 1;
    p.i = data_pos + text[data_pos..].find("DATA;").unwrap() + 5;

    // Split the DATA section at statement starts, then parse the chunks on all cores and merge
    // them in file order, so later duplicates still win as in a serial parse. The split uses the
    // parser's own skip_ws/skip_statement, i.e. the same statement boundaries as error recovery.
    let (starts, end) = data_chunks(&mut p, chunk_bytes);
    let parts: Vec<(Vec<(u64, Entity)>, usize)> = (0..starts.len())
        .into_par_iter()
        .map(|k| {
            let stop = starts.get(k + 1).map_or(end, |s| s.0);
            parse_chunk(text.as_bytes(), starts[k], stop, skip)
        })
        .collect();
    let count = parts.iter().map(|(v, _)| v.len()).sum();
    skipped += parts.iter().map(|(_, sk)| sk).sum::<usize>();
    file.entities = Entities::from_ordered(parts.into_iter().map(|(v, _)| v), count);
    Ok((file, skipped))
}

/// Target size of a parse chunk. Small enough to balance cores, large enough that per-chunk
/// overhead is negligible.
const CHUNK_BYTES: usize = 1 << 20;

/// Statement starts (byte offset, line) beginning a new chunk, and the end of the data section
/// (ENDSEC or end of text). The parser is left at the end.
fn data_chunks(p: &mut Parser, target: usize) -> (Vec<(usize, usize)>, usize) {
    let mut starts: Vec<(usize, usize)> = Vec::new();
    loop {
        p.skip_ws();
        let here = (p.i, p.line);
        match p.peek() {
            None => break,
            Some(b'#') => {}
            Some(_) => {
                if p.keyword() == "ENDSEC" {
                    p.i = here.0;
                    p.line = here.1;
                    break;
                }
                p.i = here.0;
                p.line = here.1;
            }
        }
        if starts.last().is_none_or(|s| here.0 - s.0 >= target) {
            starts.push(here);
        }
        p.skip_statement();
    }
    (starts, p.i)
}

/// Parse the statements in `s[start.0..stop]`. Malformed instances are skipped and counted.
fn parse_chunk(
    s: &[u8],
    start: (usize, usize),
    stop: usize,
    skip: &[&str],
) -> (Vec<(u64, Entity)>, usize) {
    let mut p = Parser {
        s,
        i: start.0,
        line: start.1,
        names: HashMap::new(),
    };
    let mut out = Vec::new();
    let mut skipped = 0;
    loop {
        p.skip_ws();
        if p.i >= stop {
            break;
        }
        match p.peek() {
            None => break,
            Some(b'#') => {}
            Some(_) => {
                let kw = p.keyword();
                if kw == "ENDSEC" {
                    break;
                }
                p.skip_statement();
                continue;
            }
        }
        let stmt_start = (p.i, p.line);
        let res: Result<Option<(u64, Entity)>, ParseError> = (|| {
            let id = match p.value(0)? {
                Value::Ref(r) => r,
                _ => return p.err("expected entity id"),
            };
            p.expect(b'=')?;
            p.skip_ws();
            let entity = if p.peek() == Some(b'(') {
                // Complex instance: (A(...)B(...)). Keep the first part, which is enough for units.
                p.i += 1;
                let name = p.keyword_name();
                p.expect(b'(')?;
                let args = p.items(1)?;
                p.skip_statement_inner();
                Entity { name, args }
            } else {
                let name = p.keyword_name();
                if name.is_empty() {
                    return p.err("expected entity name");
                }
                if skip.contains(&name.as_str()) {
                    p.skip_statement();
                    return Ok(None);
                }
                p.expect(b'(')?;
                let args = p.items(1)?;
                p.expect(b';')?;
                Entity { name, args }
            };
            Ok(Some((id, entity)))
        })();
        match res {
            Ok(x) => out.extend(x),
            Err(_) => {
                skipped += 1;
                p.i = stmt_start.0;
                p.line = stmt_start.1;
                p.skip_statement();
            }
        }
    }
    (out, skipped)
}

impl Parser<'_> {
    /// For complex instances: skip the remaining parts up to the closing ';'.
    fn skip_statement_inner(&mut self) {
        self.skip_statement();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = "ISO-10303-21;
HEADER;
FILE_DESCRIPTION(('ViewDefinition [x]'),'2;1');
FILE_SCHEMA(('IFC4'));
ENDSEC;
DATA;
#1=IFCCARTESIANPOINT((0.,1.5,-2.E-3));
#2=IFCWALL('2O2Fr$t4X7Zf8NOew3FLOH',$,'Mur l''est \\X2\\00E9\\X0\\',$,$,#3,$,$,.SOLIDWALL.);
/* comment */
#3=IFCPROPERTYSINGLEVALUE('Fire',$,IFCLABEL('EI60'),$);
#4=IFCBROKEN(#1,,);
#5=(IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.)OTHER());
ENDSEC;
END-ISO-10303-21;
";

    #[test]
    fn skipped_types_are_not_stored_and_the_rest_parses_the_same() {
        let (all, s1) = parse(SAMPLE).unwrap();
        let (some, s2) = parse_skipping(SAMPLE, &["IFCPROPERTYSINGLEVALUE"]).unwrap();
        assert_eq!(s1, s2);
        assert!(all.get(3).is_some() && some.get(3).is_none());
        assert_eq!(some.entities.len(), all.entities.len() - 1);
        for (id, e) in some.entities.iter() {
            assert_eq!(e.args, all.entities[&id].args);
        }
    }

    /// Dense ids get a table, sparse ones (or a hostile id near u64::MAX) a hash map; both
    /// answer the same, and a later duplicate id wins.
    #[test]
    fn entities_dense_or_sparse() {
        let file = |ids: &str| {
            format!(
                "ISO-10303-21;\nHEADER;FILE_SCHEMA(('IFC4'));ENDSEC;\nDATA;\n{ids}ENDSEC;\nEND-ISO-10303-21;\n"
            )
        };
        let (d, _) = parse(&file(
            "#1=IFCX(1);\n#2=IFCX(2);\n#2=IFCX(3);\n#4=IFCX(4);\n",
        ))
        .unwrap();
        assert!(matches!(d.entities, Entities::Dense { .. }));
        assert_eq!(d.entities.len(), 3);
        assert_eq!(d.get(2).unwrap().arg(0).as_i64(), Some(3));
        assert!(d.get(3).is_none() && d.get(u64::MAX).is_none());
        let (s, _) = parse(&file("#1=IFCX(1);\n#18446744073709551000=IFCX(2);\n")).unwrap();
        assert!(matches!(s.entities, Entities::Sparse(_)));
        assert_eq!(
            s.get(18446744073709551000).unwrap().arg(0).as_i64(),
            Some(2)
        );
        assert_eq!(s.entities.keys().count(), 2);
    }

    /// A parsed file is millions of these: a value is 24 bytes, an entity 32 (they were 32
    /// and 40, with lists keeping up to twice the room they need).
    #[test]
    fn parsed_values_stay_small() {
        assert_eq!(std::mem::size_of::<Value>(), 24);
        assert_eq!(std::mem::size_of::<Entity>(), 32);
    }

    /// Everything that could make a chunk boundary disagree with the serial parse.
    const TRICKY: &str = "ISO-10303-21;
HEADER;FILE_SCHEMA(('IFC4'));ENDSEC;
DATA;
/* a comment with a quote ' and a ; */
#1=IFCLABEL('semi;colon and #2=IFCX(); inside');
#2=IFCWALL('it''s',$,'l''est;',(#1,#1),.T.);
#3=IFCBROKEN(#1,,);
#5=IFCY(1.);
#6=IFCY(2.);
#6=IFCY(3.);
  /* trailing */ #7=(IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.)OTHER());
#8=IFCZ($);
#4=IFCX('unterminated; swallows the rest;
ENDSEC;
#9=IFCAFTER(1);
END-ISO-10303-21;
";

    fn canonical(f: &StepFile, skipped: usize) -> String {
        let mut v: Vec<_> = f.entities.iter().collect();
        v.sort_by_key(|(k, _)| *k);
        format!("{skipped} {v:?}")
    }

    #[test]
    fn chunked_parse_matches_serial() {
        let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/fixtures");
        let mut texts = vec![SAMPLE.to_owned(), TRICKY.to_owned()];
        for e in std::fs::read_dir(dir).unwrap() {
            let p = e.unwrap().path();
            if p.extension().is_some_and(|x| x == "ifc") {
                texts.push(std::fs::read_to_string(p).unwrap());
            }
        }
        for t in &texts {
            let (serial, s1) = parse_chunked(t, usize::MAX, &[]).unwrap();
            for chunk in [1, 7, 64] {
                let (par, s2) = parse_chunked(t, chunk, &[]).unwrap();
                assert_eq!(canonical(&serial, s1), canonical(&par, s2), "chunk {chunk}");
            }
        }
        // The tricky sample itself, against what a serial reading must give.
        let (f, skipped) = parse_chunked(TRICKY, 1, &[]).unwrap();
        assert_eq!(
            f.get(1).unwrap().arg(0).as_str(),
            Some("semi;colon and #2=IFCX(); inside")
        );
        assert_eq!(f.get(2).unwrap().arg(2).as_str(), Some("l'est;"));
        assert_eq!(
            f.get(6).unwrap().arg(0).as_f64(),
            Some(3.0),
            "later duplicate wins"
        );
        assert_eq!(f.get(7).unwrap().name, "IFCSIUNIT");
        assert!(f.get(8).is_some());
        assert!(
            f.get(9).is_none(),
            "nothing after ENDSEC or the unterminated string"
        );
        assert!(f.get(3).is_none() && f.get(4).is_none());
        assert_eq!(skipped, 2);
    }

    #[test]
    fn parses_sample() {
        let (f, skipped) = parse(SAMPLE).unwrap();
        assert_eq!(f.schemas, vec!["IFC4"]);
        assert_eq!(skipped, 1);
        let p = f.get(1).unwrap();
        assert_eq!(p.name, "IFCCARTESIANPOINT");
        let c: Vec<f64> = p
            .arg(0)
            .as_list()
            .unwrap()
            .iter()
            .map(|v| v.as_f64().unwrap())
            .collect();
        assert_eq!(c, vec![0.0, 1.5, -0.002]);
        let w = f.get(2).unwrap();
        assert_eq!(w.arg(2).as_str(), Some("Mur l'est é"));
        assert_eq!(w.arg(5).as_ref(), Some(3));
        assert_eq!(w.arg(8).as_enum(), Some("SOLIDWALL"));
        assert_eq!(
            f.get(3).unwrap().arg(2),
            &Value::Typed(Box::new(("IFCLABEL".into(), Value::Str("EI60".into()))))
        );
        assert_eq!(f.get(5).unwrap().name, "IFCSIUNIT");
    }

    #[test]
    fn rejects_non_step() {
        assert!(parse("hello").is_err());
        assert!(parse("ISO-10303-21;\nHEADER;ENDSEC;").is_err());
    }

    #[test]
    fn deep_nesting_is_bounded() {
        let deep = format!(
            "ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n#1=X({}{});\nENDSEC;",
            "(".repeat(10_000),
            ")".repeat(10_000)
        );
        let (f, skipped) = parse(&deep).unwrap();
        assert_eq!(skipped, 1);
        assert!(f.entities.is_empty());
    }

    #[test]
    fn decodes_escapes() {
        assert_eq!(decode_step_string(b"a\\X\\E9b"), "aéb");
        assert_eq!(
            decode_step_string(b"\\X2\\00C900E9\\X0\\t\\X2\\00E9\\X0\\"),
            "Éété"
        );
        assert_eq!(decode_step_string(b"c:\\\\x"), "c:\\x");
        assert_eq!(decode_step_string("déjà".as_bytes()), "déjà");
    }
}
