//! `.bimble` file I/O. POC encoding: canonical JSON with a format header.
//! See docs/format/bimble-format.md.

use crate::Model;
use serde::{Deserialize, Serialize};

pub const FORMAT: &str = "bimble";
pub const FORMAT_VERSION: u32 = 1;
/// Refuse absurdly large files before parsing (untrusted input).
pub const MAX_FILE_BYTES: usize = 512 * 1024 * 1024;

#[derive(Serialize)]
struct Out<'a> {
    format: &'static str,
    version: u32,
    model: &'a Model,
}

#[derive(Deserialize)]
struct In {
    format: String,
    version: u32,
    model: Model,
}

#[derive(Debug, thiserror::Error)]
pub enum FileError {
    #[error("io: {0}")]
    Io(#[from] std::io::Error),
    #[error("parse: {0}")]
    Parse(#[from] serde_json::Error),
    #[error("not a bimble file (format = {0:?})")]
    WrongFormat(String),
    #[error("unsupported bimble format version {0} (this build reads {FORMAT_VERSION})")]
    Version(u32),
    #[error("file too large: {0} bytes")]
    TooLarge(usize),
    #[error("invalid model: {0}")]
    Invalid(String),
}

pub fn to_string(m: &Model) -> String {
    serde_json::to_string_pretty(&Out {
        format: FORMAT,
        version: FORMAT_VERSION,
        model: m,
    })
    .unwrap()
}

pub fn from_str(s: &str) -> Result<Model, FileError> {
    if s.len() > MAX_FILE_BYTES {
        return Err(FileError::TooLarge(s.len()));
    }
    let f: In = serde_json::from_str(s)?;
    if f.format != FORMAT {
        return Err(FileError::WrongFormat(f.format));
    }
    if f.version != FORMAT_VERSION {
        return Err(FileError::Version(f.version));
    }
    f.model.validate().map_err(FileError::Invalid)?;
    Ok(crate::version::canonical(f.model))
}

pub fn save(m: &Model, path: impl AsRef<std::path::Path>) -> Result<(), FileError> {
    write_atomic(path.as_ref(), to_string(m).as_bytes())?;
    Ok(())
}

/// Write to a temp file next to `path`, then rename: an interrupted write never leaves a broken
/// file behind.
pub fn write_atomic(path: &std::path::Path, bytes: &[u8]) -> std::io::Result<()> {
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(".tmp");
    let tmp = std::path::PathBuf::from(tmp);
    std::fs::write(&tmp, bytes)?;
    std::fs::rename(&tmp, path)
}

pub fn load(path: impl AsRef<std::path::Path>) -> Result<Model, FileError> {
    let len = std::fs::metadata(path.as_ref())?.len() as usize;
    if len > MAX_FILE_BYTES {
        return Err(FileError::TooLarge(len));
    }
    from_str(&std::fs::read_to_string(path)?)
}
