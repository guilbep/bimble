//! Minimal ISO 10303-21 (STEP physical file) parser, enough to read IFC.
//!
//! Input is untrusted: nesting depth is bounded, ids are validated, and errors are reported
//! with a line number instead of panicking.

use std::collections::HashMap;

#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Null,
    Derived,
    Int(i64),
    Real(f64),
    Str(String),
    Enum(String),
    Ref(u64),
    List(Vec<Value>),
    /// Typed parameter, e.g. IFCLABEL('x') → ("IFCLABEL", Str("x"))
    Typed(String, Box<Value>),
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
            Value::Typed(_, v) => v.as_f64(),
            _ => None,
        }
    }
    pub fn as_i64(&self) -> Option<i64> {
        match self {
            Value::Int(i) => Some(*i),
            Value::Typed(_, v) => v.as_i64(),
            _ => None,
        }
    }
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::Str(s) => Some(s),
            Value::Typed(_, v) => v.as_str(),
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
    pub name: String,
    pub args: Vec<Value>,
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
    pub entities: HashMap<u64, Entity>,
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
        self.entities
            .iter()
            .filter(move |(_, e)| e.name == name)
            .map(|(k, e)| (*k, e))
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
            Some(b'\'') => Ok(Value::Str(self.string()?)),
            Some(b'"') => {
                // Binary: keep the hex digits as text.
                self.i += 1;
                let start = self.i;
                while self.peek().is_some_and(|c| c != b'"') {
                    self.i += 1;
                }
                let t = String::from_utf8_lossy(&self.s[start..self.i]).into_owned();
                self.i += 1;
                Ok(Value::Str(t))
            }
            Some(b'.') => {
                self.i += 1;
                let start = self.i;
                while self.peek().is_some_and(|c| c != b'.') {
                    self.i += 1;
                }
                let t = String::from_utf8_lossy(&self.s[start..self.i]).to_ascii_uppercase();
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
                let name = self.keyword();
                self.expect(b'(')?;
                let mut items = self.items(depth + 1)?;
                let inner = if items.len() == 1 {
                    items.pop().unwrap()
                } else {
                    Value::List(items)
                };
                Ok(Value::Typed(name, Box::new(inner)))
            }
            other => self.err(format!("unexpected {:?}", other.map(|b| b as char))),
        }
    }

    /// Comma-separated values up to and including ')'.
    fn items(&mut self, depth: usize) -> Result<Vec<Value>, ParseError> {
        let mut out = Vec::new();
        self.skip_ws();
        if self.peek() == Some(b')') {
            self.i += 1;
            return Ok(out);
        }
        loop {
            out.push(self.value(depth)?);
            self.skip_ws();
            match self.peek() {
                Some(b',') => self.i += 1,
                Some(b')') => {
                    self.i += 1;
                    return Ok(out);
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
    let mut p = Parser {
        s: text.as_bytes(),
        i: 0,
        line: 1,
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
        };
        if hp.expect(b'(').is_ok()
            && let Ok(v) = hp.items(1)
        {
            let mut stack = v;
            while let Some(x) = stack.pop() {
                match x {
                    Value::Str(s) => file.schemas.push(s.to_ascii_uppercase()),
                    Value::List(l) => stack.extend(l),
                    _ => {}
                }
            }
        }
    }
    p.line = text[..data_pos].matches('\n').count() + 1;
    p.i = data_pos + text[data_pos..].find("DATA;").unwrap() + 5;

    loop {
        p.skip_ws();
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
        let res: Result<(u64, Entity), ParseError> = (|| {
            let id = match p.value(0)? {
                Value::Ref(r) => r,
                _ => return p.err("expected entity id"),
            };
            p.expect(b'=')?;
            p.skip_ws();
            let entity = if p.peek() == Some(b'(') {
                // Complex instance: (A(...)B(...)). Keep the first part, which is enough for units.
                p.i += 1;
                let name = p.keyword();
                p.expect(b'(')?;
                let args = p.items(1)?;
                p.skip_statement_inner();
                Entity { name, args }
            } else {
                let name = p.keyword();
                if name.is_empty() {
                    return p.err("expected entity name");
                }
                p.expect(b'(')?;
                let args = p.items(1)?;
                p.expect(b';')?;
                Entity { name, args }
            };
            Ok((id, entity))
        })();
        match res {
            Ok((id, e)) => {
                file.entities.insert(id, e);
            }
            Err(_) => {
                skipped += 1;
                p.i = stmt_start.0;
                p.line = stmt_start.1;
                p.skip_statement();
            }
        }
    }
    Ok((file, skipped))
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
            &Value::Typed("IFCLABEL".into(), Box::new(Value::Str("EI60".into())))
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
