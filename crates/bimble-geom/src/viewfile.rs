//! The prepared viewing format, `.bimbleview` (ADR 0040): a `Scene` as bytes, to share a model
//! for others to look at and explore (a browser, a machine with little memory) without parsing
//! the IFC. Read-only: no model behind it, nothing to edit.
//!
//! Layout (little-endian; varint = LEB128, as in [`crate::codec`]; str = varint length + UTF-8):
//!
//! ```text
//! "BIMBLEVW"  u32 version (1)
//! sections, each: [u8; 4] kind, u64 length, payload
//!   HEAD  varint elements, varint shared shapes, varint stored triangles; f64 × 3 origin
//!   META  varint n, str × n: the distinct classes, storeys, materials and families, sorted
//!         storeys: varint n, (str name, f64 elevation, str id) × n
//!         grids: varint n, (str tag, f64 × 4 start and end) × n
//!         densities: varint n, (str material, f64) × n
//!         warnings: varint n, str × n
//!   ELEM  (repeated) varint n, (varint length, codec mesh) × n: the shared shapes first used
//!         here, numbered on from the previous chunks; varint n, elements × n:
//!         str GlobalId, str name, varint class, storey, material, family (indices into META)
//!         u8 0 + varint length + codec mesh (own triangles), or
//!         u8 1 + varint shape + f32 × 12 placement (x, y, z, t columns)
//! ```
//!
//! HEAD comes first and META before any ELEM; a reader skips a section kind it does not know,
//! so later additions (property sets, proxies) need no new version. A chunk closes once it holds
//! [`CHUNK_BYTES`]: what a reader can draw before the rest arrives. The whole stream may be
//! gzipped (the writer does by default); readers recognise gzip by its first two bytes.
//!
//! Canonical: the same scene gives the same bytes (sorted string table, shapes numbered by first
//! use, chunks cut by size, -0.0 written as 0.0, gzip at a fixed level with no time stamp).
//!
//! Untrusted input: lengths are checked against what is there, indices against what was read,
//! floats must be finite, decompressed bytes are bounded, and nothing panics.

use crate::Mesh;
use crate::codec::{self, Reader, put};
use crate::scene::{Geometry, GridAxis, Placement32, Scene, SceneElement, StoreyLevel};
use bimble_core::MaterialCategory;
use std::collections::{BTreeSet, HashMap};
use std::io::Read;
use std::sync::Arc;

/// The file extension, without the dot.
pub const EXTENSION: &str = "bimbleview";
pub const VERSION: u32 = 1;
const MAGIC: &[u8; 8] = b"BIMBLEVW";
/// Elements and shapes go in chunks of about this many bytes (before gzip).
pub const CHUNK_BYTES: usize = 1 << 20;
/// Largest section a reader accepts.
const MAX_SECTION: u64 = 1 << 30;
/// Most a gzipped file may expand (the encoded meshes compress about 2×).
const MAX_EXPANSION: u64 = 64;

#[derive(Debug, PartialEq)]
pub enum Error {
    NotViewFile,
    /// Made by a newer Bimble (or damaged).
    Version(u32),
    Truncated,
    Invalid(String),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::NotViewFile => write!(f, "not a .{EXTENSION} file"),
            Error::Version(v) => write!(
                f,
                "a .{EXTENSION} file of format version {v}; this Bimble reads version {VERSION} \
                 (a newer Bimble made it: update Bimble)"
            ),
            Error::Truncated => write!(f, "the .{EXTENSION} file is cut short"),
            Error::Invalid(why) => write!(f, "damaged .{EXTENSION} file: {why}"),
        }
    }
}

impl std::error::Error for Error {}

impl From<codec::Error> for Error {
    fn from(e: codec::Error) -> Error {
        match e {
            codec::Error::Truncated => Error::Truncated,
            e => Error::Invalid(e.to_string()),
        }
    }
}

/// Whether bytes start like a view file (raw or gzipped; a gzipped one is checked on reading).
pub fn sniff(bytes: &[u8]) -> bool {
    bytes.starts_with(MAGIC) || bytes.starts_with(&[0x1f, 0x8b])
}

/// Whether a file name has the view file's extension.
pub fn is_named(name: &str) -> bool {
    std::path::Path::new(name)
        .extension()
        .is_some_and(|x| x.eq_ignore_ascii_case(EXTENSION))
}

fn f64_bits(out: &mut Vec<u8>, v: f64) {
    out.extend((if v == 0.0 { 0.0 } else { v }).to_le_bytes());
}

fn f32_bits(out: &mut Vec<u8>, v: f32) {
    out.extend((if v == 0.0 { 0.0 } else { v }).to_le_bytes());
}

fn put_str(out: &mut Vec<u8>, s: &str) {
    put(out, s.len() as u64);
    out.extend(s.as_bytes());
}

fn section(out: &mut Vec<u8>, kind: &[u8; 4], payload: &[u8]) {
    out.extend(kind);
    out.extend((payload.len() as u64).to_le_bytes());
    out.extend(payload);
}

/// The scene as a view file; gzipped with `gzip`.
pub fn write(scene: &Scene, gzip: bool) -> Vec<u8> {
    let mut out = Vec::from(*MAGIC);
    out.extend(VERSION.to_le_bytes());

    let mut shapes: HashMap<*const Mesh, u64> = HashMap::new();
    for e in &scene.elements {
        if let Geometry::Instance { shape, .. } = &e.geometry {
            let n = shapes.len() as u64;
            shapes.entry(Arc::as_ptr(shape)).or_insert(n);
        }
    }
    let mut s = Vec::new();
    put(&mut s, scene.elements.len() as u64);
    put(&mut s, shapes.len() as u64);
    put(&mut s, scene.stored_triangles() as u64);
    scene.origin_mm.iter().for_each(|v| f64_bits(&mut s, *v));
    section(&mut out, b"HEAD", &s);

    let table: BTreeSet<&str> = scene
        .elements
        .iter()
        .flat_map(|e| [&*e.class, &*e.storey, &*e.material, e.category.label()])
        .collect();
    let index: HashMap<&str, u64> = table
        .iter()
        .enumerate()
        .map(|(i, s)| (*s, i as u64))
        .collect();
    let mut s = Vec::new();
    put(&mut s, table.len() as u64);
    table.iter().for_each(|t| put_str(&mut s, t));
    put(&mut s, scene.storeys.len() as u64);
    for l in &scene.storeys {
        put_str(&mut s, &l.name);
        f64_bits(&mut s, l.elevation_mm);
        put_str(&mut s, &l.id);
    }
    put(&mut s, scene.grids.len() as u64);
    for g in &scene.grids {
        put_str(&mut s, &g.tag);
        g.start_mm
            .iter()
            .chain(&g.end_mm)
            .for_each(|v| f64_bits(&mut s, *v));
    }
    put(&mut s, scene.densities.len() as u64);
    for (m, d) in &scene.densities {
        put_str(&mut s, m);
        f64_bits(&mut s, *d);
    }
    put(&mut s, scene.warnings.len() as u64);
    scene.warnings.iter().for_each(|w| put_str(&mut s, w));
    section(&mut out, b"META", &s);

    // Chunks: the shapes first used in it, then its elements.
    let (mut new_shapes, mut n_shapes, mut elems, mut n_elems) =
        (Vec::new(), 0u64, Vec::new(), 0u64);
    let mut written = 0u64;
    let flush =
        |out: &mut Vec<u8>, ns: &mut Vec<u8>, n: &mut u64, es: &mut Vec<u8>, m: &mut u64| {
            let mut s = Vec::with_capacity(ns.len() + es.len() + 20);
            put(&mut s, *n);
            s.append(ns);
            put(&mut s, *m);
            s.append(es);
            section(out, b"ELEM", &s);
            (*n, *m) = (0, 0);
        };
    for e in &scene.elements {
        put_str(&mut elems, &e.global_id);
        put_str(&mut elems, &e.name);
        for t in [&*e.class, &*e.storey, &*e.material, e.category.label()] {
            put(&mut elems, index[t]);
        }
        match &e.geometry {
            Geometry::Own(m) => {
                elems.push(0);
                let b = codec::encode(m);
                put(&mut elems, b.len() as u64);
                elems.extend(b);
            }
            Geometry::Instance { shape, to_scene } => {
                let id = shapes[&Arc::as_ptr(shape)];
                if id == written {
                    let b = codec::encode(shape);
                    put(&mut new_shapes, b.len() as u64);
                    new_shapes.extend(b);
                    n_shapes += 1;
                    written += 1;
                }
                elems.push(1);
                put(&mut elems, id);
                let p = to_scene;
                p.x.iter()
                    .chain(&p.y)
                    .chain(&p.z)
                    .chain(&p.t)
                    .for_each(|v| f32_bits(&mut elems, *v));
            }
        }
        n_elems += 1;
        if new_shapes.len() + elems.len() >= CHUNK_BYTES {
            flush(
                &mut out,
                &mut new_shapes,
                &mut n_shapes,
                &mut elems,
                &mut n_elems,
            );
        }
    }
    if n_elems > 0 {
        flush(
            &mut out,
            &mut new_shapes,
            &mut n_shapes,
            &mut elems,
            &mut n_elems,
        );
    }

    if !gzip {
        return out;
    }
    use std::io::Write;
    let mut z = flate2::GzBuilder::new()
        .mtime(0)
        .write(Vec::new(), flate2::Compression::new(6));
    z.write_all(&out).expect("writing to memory");
    z.finish().expect("writing to memory")
}

/// Reads a view file from its bytes (raw or gzipped).
pub fn read_bytes(bytes: &[u8]) -> Result<Scene, Error> {
    if bytes.starts_with(&[0x1f, 0x8b]) {
        let limit = (bytes.len() as u64)
            .saturating_mul(MAX_EXPANSION)
            .max(1 << 26);
        read(flate2::read::GzDecoder::new(bytes).take(limit))
    } else {
        read(bytes)
    }
}

fn str(r: &mut Reader) -> Result<String, Error> {
    let n = r.varint()?;
    let b = r.take(usize::try_from(n).map_err(|_| Error::Truncated)?)?;
    String::from_utf8(b.to_vec()).map_err(|_| Error::Invalid("text not UTF-8".into()))
}

fn f64(r: &mut Reader) -> Result<f64, Error> {
    let b = r.take(8)?;
    let v = f64::from_le_bytes(b.try_into().expect("8 bytes"));
    v.is_finite()
        .then_some(v)
        .ok_or_else(|| Error::Invalid("number not finite".into()))
}

/// A count of items each at least `min` bytes long, checked against what is left.
fn count(r: &mut Reader, min: usize) -> Result<usize, Error> {
    let n = r.varint()?;
    if n.saturating_mul(min as u64) > r.0.len() as u64 {
        return Err(Error::Truncated);
    }
    Ok(n as usize)
}

fn index<'a>(r: &mut Reader, table: &'a [String]) -> Result<&'a String, Error> {
    let i = r.varint()?;
    table
        .get(i as usize)
        .ok_or_else(|| Error::Invalid("string index".into()))
}

/// Reads a view file from a stream of its (raw, not gzipped) bytes, section by section: the
/// stream is never held whole.
pub fn read(mut r: impl Read) -> Result<Scene, Error> {
    let mut head = [0u8; 12];
    read_exact(&mut r, &mut head).map_err(|e| match e {
        Error::Truncated => Error::NotViewFile,
        e => e,
    })?;
    if &head[..8] != MAGIC {
        return Err(Error::NotViewFile);
    }
    let version = u32::from_le_bytes(head[8..].try_into().expect("4 bytes"));
    if version != VERSION {
        return Err(Error::Version(version));
    }
    let mut scene = Scene::default();
    let mut header: Option<(u64, u64)> = None;
    let mut table: Option<Vec<String>> = None;
    let mut shapes: Vec<Arc<Mesh>> = Vec::new();
    while let Some((kind, payload)) = next_section(&mut r)? {
        let mut s = Reader(&payload);
        match &kind {
            b"HEAD" if header.is_none() => {
                let n = s.varint()?;
                let shapes = s.varint()?;
                let _stored_triangles = s.varint()?;
                scene.origin_mm = [f64(&mut s)?, f64(&mut s)?, f64(&mut s)?];
                header = Some((n, shapes));
            }
            _ if header.is_none() => return Err(Error::Invalid("no header first".into())),
            b"META" if table.is_none() => {
                let n = count(&mut s, 1)?;
                table = Some((0..n).map(|_| str(&mut s)).collect::<Result<_, _>>()?);
                let n = count(&mut s, 10)?;
                for _ in 0..n {
                    scene.storeys.push(StoreyLevel {
                        name: str(&mut s)?,
                        elevation_mm: f64(&mut s)?,
                        id: str(&mut s)?,
                    });
                }
                let n = count(&mut s, 33)?;
                for _ in 0..n {
                    scene.grids.push(GridAxis {
                        tag: str(&mut s)?,
                        start_mm: [f64(&mut s)?, f64(&mut s)?],
                        end_mm: [f64(&mut s)?, f64(&mut s)?],
                    });
                }
                let n = count(&mut s, 9)?;
                for _ in 0..n {
                    let m = str(&mut s)?;
                    scene.densities.insert(m, f64(&mut s)?);
                }
                let n = count(&mut s, 1)?;
                for _ in 0..n {
                    scene.warnings.push(str(&mut s)?);
                }
            }
            b"ELEM" => {
                let Some(table) = &table else {
                    return Err(Error::Invalid("elements before the string table".into()));
                };
                let n = count(&mut s, 1)?;
                for _ in 0..n {
                    let len = count(&mut s, 1)?;
                    shapes.push(Arc::new(codec::decode(s.take(len)?)?));
                }
                let n = count(&mut s, 7)?;
                for _ in 0..n {
                    let global_id = str(&mut s)?;
                    let name = str(&mut s)?;
                    let class = index(&mut s, table)?.clone();
                    let storey = index(&mut s, table)?.clone();
                    let material = index(&mut s, table)?.clone();
                    let family = index(&mut s, table)?;
                    let category = MaterialCategory::ALL
                        .into_iter()
                        .find(|c| c.label() == family)
                        .unwrap_or(MaterialCategory::Other);
                    let geometry = match s.byte()? {
                        0 => {
                            let len = count(&mut s, 1)?;
                            Geometry::Own(codec::decode(s.take(len)?)?)
                        }
                        1 => {
                            let shape = shapes
                                .get(s.varint()? as usize)
                                .ok_or_else(|| Error::Invalid("shape index".into()))?
                                .clone();
                            let mut v = [0f32; 12];
                            for x in &mut v {
                                *x = s.f32()?;
                                if !x.is_finite() {
                                    return Err(Error::Invalid("placement not finite".into()));
                                }
                            }
                            let c = |k: usize| [v[k], v[k + 1], v[k + 2]];
                            Geometry::Instance {
                                shape,
                                to_scene: Placement32 {
                                    x: c(0),
                                    y: c(3),
                                    z: c(6),
                                    t: c(9),
                                },
                            }
                        }
                        _ => return Err(Error::Invalid("geometry kind".into())),
                    };
                    scene.elements.push(SceneElement {
                        global_id,
                        class,
                        name,
                        storey,
                        material,
                        category,
                        geometry,
                    });
                }
            }
            b"HEAD" | b"META" => return Err(Error::Invalid("section repeated".into())),
            _ => continue, // a later addition: skipped
        }
        if !s.0.is_empty() {
            return Err(Error::Invalid("bytes left in a section".into()));
        }
    }
    match header {
        Some((n, k)) if n == scene.elements.len() as u64 && k == shapes.len() as u64 => Ok(scene),
        Some(_) => Err(Error::Truncated),
        None => Err(Error::NotViewFile),
    }
}

fn read_exact(r: &mut impl Read, buf: &mut [u8]) -> Result<(), Error> {
    r.read_exact(buf).map_err(|e| match e.kind() {
        std::io::ErrorKind::UnexpectedEof => Error::Truncated,
        _ => Error::Invalid(e.to_string()),
    })
}

/// A section: its kind and payload.
type Section = ([u8; 4], Vec<u8>);

/// The next section, or None at the end of the stream.
fn next_section(r: &mut impl Read) -> Result<Option<Section>, Error> {
    let mut kind = [0u8; 4];
    let mut got = 0;
    while got < 4 {
        match r.read(&mut kind[got..]) {
            Ok(0) if got == 0 => return Ok(None),
            Ok(0) => return Err(Error::Truncated),
            Ok(n) => got += n,
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
            Err(e) => return Err(Error::Invalid(e.to_string())),
        }
    }
    let mut len = [0u8; 8];
    read_exact(r, &mut len)?;
    let len = u64::from_le_bytes(len);
    if len > MAX_SECTION {
        return Err(Error::Invalid("section too large".into()));
    }
    // Grows with what arrives: a length that lies allocates nothing.
    let mut payload = Vec::new();
    r.take(len)
        .read_to_end(&mut payload)
        .map_err(|e| Error::Invalid(e.to_string()))?;
    if payload.len() as u64 != len {
        return Err(Error::Truncated);
    }
    Ok(Some((kind, payload)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::demo::{FrameSpec, concrete_frame};

    /// The demo frame, plus a shape shared by three copies (one mirrored), a storey, a grid.
    fn scene() -> Scene {
        let mut s = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        s.origin_mm = [2_600_000.0, 1_200_000.0, -0.0];
        let shape = Arc::new(s.elements[0].world_mesh().into_owned());
        for (k, mirror) in [(0, false), (1, true), (2, false)] {
            let mut e = s.elements[0].clone();
            e.global_id = format!("copy{k}");
            let x = if mirror { -1.0 } else { 1.0 };
            e.geometry = Geometry::Instance {
                shape: shape.clone(),
                to_scene: Placement32 {
                    x: [x, 0.0, 0.0],
                    y: [0.0, 1.0, 0.0],
                    z: [0.0, 0.0, 1.0],
                    t: [k as f32 * 5000.0, -0.0, 0.0],
                },
            };
            s.elements.push(e);
        }
        s.grids.push(GridAxis {
            tag: "A".into(),
            start_mm: [0.0, 0.0],
            end_mm: [0.0, 12_000.0],
        });
        s.warnings.push("a warning".into());
        s.densities.insert("C30/37".into(), 2500.0);
        s
    }

    fn points(m: &Mesh) -> Vec<[f32; 3]> {
        m.positions
            .iter()
            .chain(m.edges.iter().flatten())
            .copied()
            .collect()
    }

    /// Everything comes back: ids, classes, storeys, grids, shapes shared as they were, every
    /// point within the codec's tolerance; and the bytes are the same each time.
    #[test]
    fn round_trip() {
        let a = scene();
        for gzip in [false, true] {
            let bytes = write(&a, gzip);
            assert_eq!(bytes, write(&a, gzip), "canonical");
            assert!(sniff(&bytes));
            let b = read_bytes(&bytes).unwrap();
            assert_eq!(b.origin_mm, [2_600_000.0, 1_200_000.0, 0.0]);
            assert_eq!(b.origin_mm[2].to_bits(), 0, "no -0.0");
            assert_eq!(b.storeys, a.storeys);
            assert_eq!(b.grids, a.grids);
            assert_eq!(b.warnings, a.warnings);
            assert_eq!(b.densities, a.densities);
            assert_eq!(b.elements.len(), a.elements.len());
            assert_eq!(b.stored_triangles(), a.stored_triangles());
            assert_eq!(b.instance_count(), a.instance_count());
            for (x, y) in a.elements.iter().zip(&b.elements) {
                assert_eq!(
                    (
                        &x.global_id,
                        &x.class,
                        &x.name,
                        &x.storey,
                        &x.material,
                        x.category
                    ),
                    (
                        &y.global_id,
                        &y.class,
                        &y.name,
                        &y.storey,
                        &y.material,
                        y.category
                    )
                );
                if let (
                    Geometry::Instance { to_scene: p, .. },
                    Geometry::Instance { to_scene: q, .. },
                ) = (&x.geometry, &y.geometry)
                {
                    assert_eq!(p, q);
                }
                let (m, n) = (x.world_mesh(), y.world_mesh());
                assert_eq!(m.indices, n.indices);
                assert_eq!(m.edge_normals, n.edge_normals);
                for (p, q) in points(&m).iter().zip(&points(&n)) {
                    for k in 0..3 {
                        assert!((p[k] as f64 - q[k] as f64).abs() <= codec::TOLERANCE_MM);
                    }
                }
            }
        }
    }

    /// Many chunks: a scene larger than a chunk comes back whole, shapes defined where first
    /// used.
    #[test]
    fn chunks() {
        let mut a = scene();
        while write(&a, false).len() < 3 * CHUNK_BYTES {
            let more = a.elements.clone();
            a.elements.extend(more);
        }
        let bytes = write(&a, false);
        let chunks = bytes.windows(4).filter(|w| w == b"ELEM").count();
        assert!(chunks >= 3, "{chunks}");
        let b = read_bytes(&bytes).unwrap();
        assert_eq!(b.elements.len(), a.elements.len());
        assert_eq!(b.stored_triangles(), a.stored_triangles());
    }

    /// Damaged files are errors, never panics: every prefix, flipped bytes, a lying length.
    #[test]
    fn corrupt_files_are_errors_not_panics() {
        let raw = write(&scene(), false);
        for n in (0..300).chain((300..raw.len()).step_by(37)) {
            assert!(read_bytes(&raw[..n]).is_err(), "prefix {n}");
        }
        let z = write(&scene(), true);
        for n in (0..z.len()).step_by(7) {
            assert!(read_bytes(&z[..n]).is_err(), "gzip prefix {n}");
        }
        let mut seed = 0x2545_f491_4f6c_dd1du64;
        for _ in 0..500 {
            let mut b = raw.clone();
            for _ in 0..4 {
                seed ^= seed << 13;
                seed ^= seed >> 7;
                seed ^= seed << 17;
                let i = (seed % b.len() as u64) as usize;
                b[i] ^= (seed >> 32) as u8 | 1;
            }
            let _ = read_bytes(&b);
        }
        // A section that says it is huge, with nothing behind it.
        let mut b = raw[..12].to_vec();
        b.extend(b"HEAD");
        b.extend((MAX_SECTION).to_le_bytes());
        assert_eq!(read_bytes(&b).err(), Some(Error::Truncated));
        assert_eq!(read_bytes(b"ISO-10303-21;").err(), Some(Error::NotViewFile));
    }

    /// A file from a newer Bimble says so.
    #[test]
    fn other_version_is_a_clear_error() {
        let mut b = write(&scene(), false);
        b[8..12].copy_from_slice(&2u32.to_le_bytes());
        let e = read_bytes(&b).unwrap_err();
        assert_eq!(e, Error::Version(2));
        assert!(e.to_string().contains("update Bimble"), "{e}");
    }

    /// A section kind added later is skipped by this reader.
    #[test]
    fn unknown_sections_are_skipped() {
        let mut b = write(&scene(), false);
        section(&mut b, b"PSET", b"later");
        assert_eq!(
            read_bytes(&b).unwrap().elements.len(),
            scene().elements.len()
        );
    }
}
