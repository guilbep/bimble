//! `.bimble` file I/O. Written binary (version 2, ADR 0042): a magic line, then gzip of the
//! model as CBOR. Read: that, or the canonical JSON text of version 1, kept for tests, diffs by
//! eye and older files. See docs/format/bimble-format.md.

use crate::{Generator, Model};
use serde::{Deserialize, Serialize};
use std::sync::Arc;

pub const FORMAT: &str = "bimble";
/// The JSON text version (`to_string`), still read.
pub const FORMAT_VERSION: u32 = 1;
/// The binary version (`to_bytes`), what `save` writes.
pub const BINARY_VERSION: u32 = 2;
/// First bytes of a binary `.bimble`; a JSON one starts with `{`.
pub const MAGIC: &[u8] = b"bimble\0";
/// Refuse absurdly large files before parsing (untrusted input). A model imported from a large
/// IFC of faceted geometry can pass 2 GB of text (a 709 MB IFC gave 2.4 GB): 8 GiB on 64-bit
/// machines; 1 GiB where memory addresses are 32-bit (the browser).
#[cfg(target_pointer_width = "64")]
pub const MAX_FILE_BYTES: usize = 8 << 30;
#[cfg(not(target_pointer_width = "64"))]
pub const MAX_FILE_BYTES: usize = 1 << 30;

#[derive(Serialize)]
struct Out<'a> {
    format: &'static str,
    version: u32,
    /// Binary only: each mesh array once, ahead of the model that refers to them by index.
    #[serde(skip_serializing_if = "Option::is_none")]
    positions: Option<Vec<Arc<Vec<[f64; 3]>>>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    indices: Option<Vec<Arc<Vec<u32>>>>,
    model: &'a Model,
}

#[derive(Deserialize)]
struct In {
    format: String,
    // Read before `model` (written first), and kept where the model's references find them.
    #[serde(default, deserialize_with = "shared::read_table::<Vec<[f64; 3]>, _>")]
    #[allow(dead_code)]
    positions: (),
    #[serde(default, deserialize_with = "shared::read_table::<Vec<u32>, _>")]
    #[allow(dead_code)]
    indices: (),
    version: u32,
    model: Model,
}

#[derive(Debug, thiserror::Error)]
pub enum FileError {
    #[error("io: {0}")]
    Io(#[from] std::io::Error),
    #[error("parse: {0}")]
    Parse(#[from] serde_json::Error),
    #[error("parse: {0}")]
    Binary(String),
    #[error("not a bimble file (format = {0:?})")]
    WrongFormat(String),
    #[error("unsupported bimble format version {0} (this build reads 1 and 2)")]
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
        positions: None,
        indices: None,
        model: m,
    })
    .unwrap()
}

pub fn from_str(s: &str) -> Result<Model, FileError> {
    if s.len() > MAX_FILE_BYTES {
        return Err(FileError::TooLarge(s.len()));
    }
    checked(serde_json::from_str(s)?, FORMAT_VERSION)
}

fn checked(f: In, version: u32) -> Result<Model, FileError> {
    if f.format != FORMAT {
        return Err(FileError::WrongFormat(f.format));
    }
    if f.version != version {
        return Err(FileError::Version(f.version));
    }
    f.model.validate().map_err(FileError::Invalid)?;
    Ok(crate::version::canonical_model(f.model))
}

/// The binary form `save` writes: `MAGIC`, then gzip of CBOR. Canonical like the JSON (same
/// serde order, sorted maps, floats kept exact), so the same model gives the same bytes.
pub fn to_bytes(m: &Model) -> Vec<u8> {
    let mut gz = flate2::write::GzEncoder::new(MAGIC.to_vec(), flate2::Compression::fast());
    // Every mesh array once, in model order (so the bytes are canonical); then the model, its
    // meshes written as indices into these tables.
    let (mut positions, mut indices) = (shared::Pool::default(), shared::Pool::default());
    for e in m.elements.values() {
        let gens = std::iter::once(&e.generator).chain(e.openings.iter().map(|o| &o.generator));
        for g in gens {
            if let Generator::Mesh {
                positions: p,
                indices: i,
            } = g
            {
                positions.add(p);
                indices.add(i);
            }
        }
    }
    let _writing = shared::Writing::start(positions.index(), indices.index());
    let out = Out {
        format: FORMAT,
        version: BINARY_VERSION,
        positions: Some(positions.items),
        indices: Some(indices.items),
        model: m,
    };
    ciborium::into_writer(&out, &mut gz).expect("a model encodes into memory");
    gz.finish().expect("gzip into memory")
}

fn from_binary(bytes: &[u8]) -> Result<Model, FileError> {
    use std::io::Read;
    // Untrusted: cap what decompresses too, not only the file (a small file can unpack huge).
    let body = flate2::read::GzDecoder::new(&bytes[MAGIC.len()..]).take(MAX_FILE_BYTES as u64);
    let f: In = {
        let _reading = shared::Reading::start();
        ciborium::de::from_reader_with_recursion_limit(std::io::BufReader::new(body), 64)
            .map_err(|e| FileError::Binary(e.to_string()))?
    };
    checked(f, BINARY_VERSION)
}

/// Binary (version 2); a path ending in `.json` gets the text form (version 1), to read, diff
/// or check with other tools.
pub fn save(m: &Model, path: impl AsRef<std::path::Path>) -> Result<(), FileError> {
    let path = path.as_ref();
    if path.extension().is_some_and(|x| x == "json") {
        write_atomic(path, to_string(m).as_bytes())?;
    } else {
        write_atomic(path, &to_bytes(m))?;
    }
    Ok(())
}

/// Write to a temp file next to `path`, then rename: an interrupted write never leaves a broken
/// file behind. The temp name is unique, so two people writing the same file on a shared folder
/// never rename each other's half-written file into place.
pub fn write_atomic(path: &std::path::Path, bytes: &[u8]) -> std::io::Result<()> {
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(format!(".{}.tmp", uuid::Uuid::new_v4().simple()));
    let tmp = std::path::PathBuf::from(tmp);
    std::fs::write(&tmp, bytes)?;
    std::fs::rename(&tmp, path)
}

/// A model from a file's content (e.g. picked in a browser). Same size cap as `load`.
pub fn from_bytes(bytes: &[u8]) -> Result<Model, FileError> {
    if bytes.len() > MAX_FILE_BYTES {
        return Err(FileError::TooLarge(bytes.len()));
    }
    if bytes.starts_with(MAGIC) {
        return from_binary(bytes);
    }
    let text = std::str::from_utf8(bytes)
        .map_err(|e| FileError::Io(std::io::Error::new(std::io::ErrorKind::InvalidData, e)))?;
    from_str(text)
}

pub fn load(path: impl AsRef<std::path::Path>) -> Result<Model, FileError> {
    let len = std::fs::metadata(path.as_ref())?.len() as usize;
    if len > MAX_FILE_BYTES {
        return Err(FileError::TooLarge(len));
    }
    from_bytes(&std::fs::read(path)?)
}

/// Mesh arrays shared between elements (copies of one shape, ADR 0042). Outside a binary
/// read or write (JSON, hashes) each is written in full. While `to_bytes` writes, an array is
/// written as its index in the table ahead of the model; while `from_binary` reads, that table
/// (read first) gives the arrays back, shared again.
pub(crate) mod shared {
    use serde::de::{DeserializeOwned, Error};
    use serde::{Deserialize, Deserializer, Serialize, Serializer};
    use std::any::Any;
    use std::cell::RefCell;
    use std::collections::HashMap;
    use std::sync::Arc;

    /// Distinct arrays, by address, in the order first met.
    pub(super) struct Pool<T> {
        pub(super) items: Vec<Arc<T>>,
        seen: HashMap<usize, u64>,
    }

    impl<T> Default for Pool<T> {
        fn default() -> Self {
            Pool {
                items: Vec::new(),
                seen: HashMap::new(),
            }
        }
    }

    impl<T> Pool<T> {
        pub(super) fn add(&mut self, a: &Arc<T>) {
            let n = self.items.len() as u64;
            self.seen.entry(Arc::as_ptr(a) as usize).or_insert_with(|| {
                self.items.push(a.clone());
                n
            });
        }
        pub(super) fn index(&self) -> HashMap<usize, u64> {
            self.seen.clone()
        }
    }

    /// One table read from a file: each entry an `Arc` of positions or of indices.
    type Table = Vec<Box<dyn Any>>;

    thread_local! {
        /// Writing: address → index, positions and indices.
        static WRITE: RefCell<Option<[HashMap<usize, u64>; 2]>> = const { RefCell::new(None) };
        /// Reading: the tables, once read (positions, indices).
        static READ: RefCell<Option<[Table; 2]>> = const { RefCell::new(None) };
    }

    fn slot<T: 'static>() -> usize {
        usize::from(std::any::TypeId::of::<T>() == std::any::TypeId::of::<Vec<u32>>())
    }

    /// Indices are written while alive; dropped (even by a panic), arrays are written in full.
    pub(super) struct Writing;
    impl Writing {
        pub(super) fn start(p: HashMap<usize, u64>, i: HashMap<usize, u64>) -> Writing {
            WRITE.with(|w| *w.borrow_mut() = Some([p, i]));
            Writing
        }
    }
    impl Drop for Writing {
        fn drop(&mut self) {
            WRITE.with(|w| *w.borrow_mut() = None);
        }
    }

    pub(super) struct Reading;
    impl Reading {
        pub(super) fn start() -> Reading {
            READ.with(|r| *r.borrow_mut() = Some([Vec::new(), Vec::new()]));
            Reading
        }
    }
    impl Drop for Reading {
        fn drop(&mut self) {
            READ.with(|r| *r.borrow_mut() = None);
        }
    }

    pub(crate) fn serialize<T: Serialize + 'static, S: Serializer>(
        v: &Arc<T>,
        s: S,
    ) -> Result<S::Ok, S::Error> {
        let at = WRITE.with(|w| {
            w.borrow()
                .as_ref()
                .and_then(|t| t[slot::<T>()].get(&(Arc::as_ptr(v) as usize)).copied())
        });
        match at {
            Some(i) => s.serialize_u64(i),
            None => v.serialize(s),
        }
    }

    pub(crate) fn deserialize<'de, T: DeserializeOwned + 'static, D: Deserializer<'de>>(
        d: D,
    ) -> Result<Arc<T>, D::Error> {
        if READ.with(|r| r.borrow().is_none()) {
            return Ok(Arc::new(T::deserialize(d)?));
        }
        let i = u64::deserialize(d)? as usize;
        READ.with(|r| {
            r.borrow().as_ref().and_then(|t| {
                t[slot::<T>()]
                    .get(i)
                    .and_then(|a| a.downcast_ref::<Arc<T>>())
                    .cloned()
            })
        })
        .ok_or_else(|| D::Error::custom(format!("mesh {i} not in the file's table")))
    }

    /// A table field of the binary file: kept for the model's references, read after it.
    pub(super) fn read_table<'de, T: DeserializeOwned + 'static, D: Deserializer<'de>>(
        d: D,
    ) -> Result<(), D::Error> {
        let items: Vec<T> = Vec::deserialize(d)?;
        READ.with(|r| {
            if let Some(t) = r.borrow_mut().as_mut() {
                t[slot::<T>()] = items
                    .into_iter()
                    .map(|x| Box::new(Arc::new(x)) as Box<dyn Any>)
                    .collect();
            }
        });
        Ok(())
    }
}
