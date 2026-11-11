//! Model history: commits, branches, log and diff, stored next to the model file.
//!
//! `house.bimble` keeps its history in `house.bimble-history/` (see ADR 0017):
//!
//! ```text
//! format                 "bimble-history 1"
//! HEAD                   "ref: main" or a commit hash (detached)
//! refs/heads/<branch>    a commit hash
//! objects/ab/cdef…       content-addressed objects, named by the blake3 hash of their bytes
//! ```
//!
//! Objects are canonical JSON: one per storey, material and element (the bytes `content_hash`
//! hashes, so an element's object name is its content hash), one tree per model state (ids →
//! object hashes, plus the project fields) and one per commit (tree, parents, author, time,
//! message). An element unchanged between commits is stored once. Every object read is checked
//! against its name, so a damaged history is reported, never silently used.

use crate::version::{Change, content_hash, diff};
use crate::{Element, GlobalId, Material, Model, Storey};
use serde::de::DeserializeOwned;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

const FORMAT: &str = "bimble-history 1";
/// Untrusted input: refuse objects larger than this.
const MAX_OBJECT_BYTES: u64 = 256 * 1024 * 1024;
/// Commits walked at most by `log`, `resolve` (HEAD~n) and ancestry searches.
const MAX_WALK: usize = 1_000_000;

#[derive(Debug, thiserror::Error)]
pub enum RepoError {
    #[error("io: {0}")]
    Io(#[from] std::io::Error),
    #[error("{0}")]
    Bad(String),
    #[error("no history next to {0} (run `bimble history init`)")]
    NoRepo(PathBuf),
    #[error("nothing to commit: the model is the same as {0}")]
    NothingToCommit(String),
    #[error("unknown revision {0:?}")]
    UnknownRev(String),
    #[error("damaged history: object {0} {1}")]
    Damaged(String, String),
    #[error("the remote's {0} has changes you don't have yet: synchronize again")]
    NotFastForward(String),
}

type Result<T> = std::result::Result<T, RepoError>;

/// A commit hash, 64 lowercase hex digits.
pub type Hash = String;

/// One model state: the project fields and, for each object, the hash of its content.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Tree {
    pub project_id: GlobalId,
    pub project_name: String,
    pub storeys: BTreeMap<GlobalId, Hash>,
    pub materials: BTreeMap<GlobalId, Hash>,
    pub elements: BTreeMap<GlobalId, Hash>,
    /// Absent in trees of models without views (older histories read unchanged).
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub views: BTreeMap<GlobalId, Hash>,
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub sheets: BTreeMap<GlobalId, Hash>,
    /// Project information, inline (it is small).
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub project: BTreeMap<String, String>,
    /// Schedules, inline (small).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub schedules: Vec<crate::schedule::ScheduleDef>,
    /// Where the model is and its project north (ADR 0045), inline.
    #[serde(default, skip_serializing_if = "crate::georef::Georef::is_default")]
    pub georef: crate::georef::Georef,
    /// The mesh arrays the elements refer to by hash (ADR 0042), so copying a tree copies them.
    #[serde(default, skip_serializing_if = "std::collections::BTreeSet::is_empty")]
    pub meshes: std::collections::BTreeSet<Hash>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Commit {
    pub tree: Hash,
    /// First parent: the branch's previous commit; a second one for a merge.
    pub parents: Vec<Hash>,
    pub author: String,
    /// Seconds since 1970-01-01 UTC.
    pub time: i64,
    pub message: String,
}

/// Where HEAD points.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Head {
    Branch(String),
    Detached(Hash),
}

pub struct Repo {
    dir: PathBuf,
}

/// The history folder of a model file: `house.bimble` → `house.bimble-history`.
pub fn history_dir(model: &Path) -> PathBuf {
    let mut d = model.as_os_str().to_owned();
    d.push("-history");
    PathBuf::from(d)
}

/// Branch names: letters, digits, `-`, `_`, `.` and `/` between parts; no `..`, no leading `.`
/// or `-`, at most 100 characters (they are file names under refs/heads).
pub fn check_branch_name(name: &str) -> Result<()> {
    let ok = !name.is_empty()
        && name.len() <= 100
        && name.split('/').all(|part| {
            !part.is_empty()
                && !part.starts_with('.')
                && !part.starts_with('-')
                && !part.contains("..")
                && part
                    .chars()
                    .all(|c| c.is_ascii_alphanumeric() || matches!(c, '-' | '_' | '.'))
        })
        && name != "HEAD";
    if ok {
        Ok(())
    } else {
        Err(RepoError::Bad(format!(
            "invalid branch name {name:?}: use letters, digits, '-', '_', '.' and '/'"
        )))
    }
}

fn is_hash(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

fn read_small(path: &Path) -> Result<String> {
    let len = std::fs::metadata(path)?.len();
    if len > 4096 {
        return Err(RepoError::Bad(format!("{}: too large", path.display())));
    }
    Ok(std::fs::read_to_string(path)?.trim().to_owned())
}

impl Repo {
    /// Create the history of `model` (an empty one, HEAD on branch `main`).
    pub fn init(model: &Path) -> Result<Repo> {
        Self::init_dir(&history_dir(model))
    }

    /// Create a history in `dir` itself (a remote has no model file next to it).
    pub fn init_dir(dir: &Path) -> Result<Repo> {
        let dir = dir.to_owned();
        if dir.join("format").exists() {
            return Err(RepoError::Bad(format!(
                "{} already has a history",
                dir.display()
            )));
        }
        std::fs::create_dir_all(dir.join("objects"))?;
        std::fs::create_dir_all(dir.join("refs/heads"))?;
        crate::file::write_atomic(&dir.join("HEAD"), b"ref: main\n")?;
        crate::file::write_atomic(&dir.join("format"), format!("{FORMAT}\n").as_bytes())?;
        Ok(Repo { dir })
    }

    pub fn open(model: &Path) -> Result<Repo> {
        if !history_dir(model).join("format").exists() {
            return Err(RepoError::NoRepo(model.to_owned()));
        }
        Self::open_dir(&history_dir(model))
    }

    /// Open the history in `dir` itself.
    pub fn open_dir(dir: &Path) -> Result<Repo> {
        let dir = dir.to_owned();
        let f = dir.join("format");
        if !f.exists() {
            return Err(RepoError::NoRepo(dir));
        }
        let format = read_small(&f)?;
        if format != FORMAT {
            return Err(RepoError::Bad(format!(
                "{}: unsupported history format {format:?} (this build reads {FORMAT:?})",
                dir.display()
            )));
        }
        Ok(Repo { dir })
    }

    pub fn dir(&self) -> &Path {
        &self.dir
    }

    // ---------- objects

    fn object_path(&self, hash: &str) -> PathBuf {
        self.dir.join("objects").join(&hash[..2]).join(&hash[2..])
    }

    /// Whether the object is stored here (false for anything that is not a hash).
    pub(crate) fn has(&self, hash: &str) -> bool {
        is_hash(hash) && self.object_path(hash).exists()
    }

    /// Store bytes under their hash; returns the hash. Existing objects are not rewritten.
    /// Stored gzipped (like git's objects), named by the hash of the bytes themselves.
    pub(crate) fn put_bytes(&self, bytes: &[u8]) -> Result<Hash> {
        use std::io::Write;
        let hash = blake3::hash(bytes).to_hex().to_string();
        let path = self.object_path(&hash);
        if !path.exists() {
            std::fs::create_dir_all(path.parent().expect("objects/xx"))?;
            let mut gz = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::fast());
            gz.write_all(bytes)?;
            crate::file::write_atomic(&path, &gz.finish()?)?;
        }
        Ok(hash)
    }

    /// Store an object; the mesh arrays it holds are stored as their own objects first and
    /// referred to by hash (ADR 0042), so copies of a shape store it once.
    fn put<T: Serialize>(&self, v: &T) -> Result<Hash> {
        let refs = crate::file::shared::Refs::start();
        let bytes = serde_json::to_vec(v).expect("history types always serialize");
        for (_, mesh) in refs.take_fresh() {
            self.put_bytes(&mesh)?;
        }
        self.put_bytes(&bytes)
    }

    fn get<T: DeserializeOwned>(&self, hash: &str) -> Result<T> {
        let bytes = self.get_bytes(hash)?;
        let _resolve = self.resolving();
        serde_json::from_slice(&bytes).map_err(|e| RepoError::Damaged(hash.into(), e.to_string()))
    }

    /// Mesh references read from this store while alive, each array once.
    fn resolving(&self) -> crate::file::shared::Resolve {
        let repo = Repo {
            dir: self.dir.clone(),
        };
        crate::file::shared::Resolve::start(move |h| repo.get_bytes(h).ok())
    }

    /// An object's bytes, checked against its name and size.
    pub(crate) fn get_bytes(&self, hash: &str) -> Result<Vec<u8>> {
        if !is_hash(hash) {
            return Err(RepoError::Damaged(hash.into(), "is not a hash".into()));
        }
        let path = self.object_path(hash);
        let len = std::fs::metadata(&path)
            .map_err(|_| RepoError::Damaged(hash.into(), "is missing".into()))?
            .len();
        if len > MAX_OBJECT_BYTES {
            return Err(RepoError::Damaged(hash.into(), "is too large".into()));
        }
        let mut bytes = std::fs::read(&path)?;
        // Gzipped (objects written since ADR 0042), or plain JSON (older ones). Untrusted: what
        // unpacks is capped too.
        if bytes.starts_with(&[0x1f, 0x8b]) {
            use std::io::Read;
            let mut plain = Vec::new();
            flate2::read::GzDecoder::new(&bytes[..])
                .take(MAX_OBJECT_BYTES + 1)
                .read_to_end(&mut plain)
                .map_err(|e| RepoError::Damaged(hash.into(), e.to_string()))?;
            if plain.len() as u64 > MAX_OBJECT_BYTES {
                return Err(RepoError::Damaged(hash.into(), "is too large".into()));
            }
            bytes = plain;
        }
        if blake3::hash(&bytes).to_hex().as_str() != hash {
            return Err(RepoError::Damaged(
                hash.into(),
                "does not match its hash".into(),
            ));
        }
        Ok(bytes)
    }

    pub fn commit_info(&self, hash: &str) -> Result<Commit> {
        self.get(hash)
    }

    pub fn tree(&self, hash: &str) -> Result<Tree> {
        self.get(hash)
    }

    /// Store a model's objects and its tree; returns the tree hash.
    pub fn write_tree(&self, m: &Model) -> Result<Hash> {
        fn map<T: Serialize>(
            r: &Repo,
            m: &BTreeMap<GlobalId, T>,
        ) -> Result<BTreeMap<GlobalId, Hash>> {
            m.iter()
                .map(|(id, v)| {
                    let h = r.put(v)?;
                    debug_assert_eq!(h, content_hash(v).to_hex().to_string());
                    Ok((*id, h))
                })
                .collect()
        }
        // One scope: each mesh hashed and stored once, and the tree lists them all.
        let refs = crate::file::shared::Refs::start();
        let mut t = Tree {
            project_id: m.project_id,
            project_name: m.project_name.clone(),
            storeys: map(self, &m.storeys)?,
            materials: map(self, &m.materials)?,
            elements: map(self, &m.elements)?,
            views: map(self, &m.views)?,
            sheets: map(self, &m.sheets)?,
            project: m.project.clone(),
            schedules: m.schedules.clone(),
            georef: m.georef.clone(),
            meshes: Default::default(),
        };
        t.meshes = refs.referenced();
        self.put(&t)
    }

    /// The model a tree describes, validated like a model file.
    pub fn read_tree(&self, hash: &str) -> Result<Model> {
        let t = self.tree(hash)?;
        // One scope: copies sharing a mesh share it again.
        let _resolve = self.resolving();
        fn map<T: DeserializeOwned>(
            r: &Repo,
            m: &BTreeMap<GlobalId, Hash>,
        ) -> Result<BTreeMap<GlobalId, T>> {
            m.iter().map(|(id, h)| Ok((*id, r.get(h)?))).collect()
        }
        let m = Model {
            project_id: t.project_id,
            project_name: t.project_name,
            storeys: map::<Storey>(self, &t.storeys)?,
            materials: map::<Material>(self, &t.materials)?,
            elements: map::<Element>(self, &t.elements)?,
            views: map::<crate::view::View>(self, &t.views)?,
            sheets: map::<crate::sheet::Sheet>(self, &t.sheets)?,
            project: t.project,
            schedules: t.schedules,
            georef: t.georef,
        };
        m.validate()
            .map_err(|e| RepoError::Damaged(hash.into(), format!("invalid model: {e}")))?;
        Ok(crate::version::canonical_model(m))
    }

    // ---------- refs

    pub fn head(&self) -> Result<Head> {
        let h = read_small(&self.dir.join("HEAD"))?;
        if let Some(b) = h.strip_prefix("ref: ") {
            check_branch_name(b)?;
            Ok(Head::Branch(b.to_owned()))
        } else if is_hash(&h) {
            Ok(Head::Detached(h))
        } else {
            Err(RepoError::Bad("HEAD is damaged".into()))
        }
    }

    pub(crate) fn set_head(&self, h: &Head) -> Result<()> {
        let text = match h {
            Head::Branch(b) => format!("ref: {b}\n"),
            Head::Detached(c) => format!("{c}\n"),
        };
        crate::file::write_atomic(&self.dir.join("HEAD"), text.as_bytes())?;
        Ok(())
    }

    /// A branch's commit; None for a branch without commits (a new history).
    pub fn branch_commit(&self, name: &str) -> Result<Option<Hash>> {
        check_branch_name(name)?;
        let p = self.dir.join("refs/heads").join(name);
        if !p.exists() {
            return Ok(None);
        }
        let h = read_small(&p)?;
        if !is_hash(&h) {
            return Err(RepoError::Bad(format!("branch {name} is damaged")));
        }
        Ok(Some(h))
    }

    pub(crate) fn set_branch(&self, name: &str, hash: &str) -> Result<()> {
        check_branch_name(name)?;
        let p = self.dir.join("refs/heads").join(name);
        if let Some(d) = p.parent() {
            std::fs::create_dir_all(d)?;
        }
        crate::file::write_atomic(&p, format!("{hash}\n").as_bytes())?;
        Ok(())
    }

    /// HEAD's commit; None before the first commit.
    pub fn head_commit(&self) -> Result<Option<Hash>> {
        match self.head()? {
            Head::Branch(b) => self.branch_commit(&b),
            Head::Detached(h) => Ok(Some(h)),
        }
    }

    /// Branches and their commits, sorted by name.
    pub fn branches(&self) -> Result<Vec<(String, Hash)>> {
        let root = self.dir.join("refs/heads");
        let mut out = Vec::new();
        let mut stack = vec![(root.clone(), 0)];
        while let Some((d, depth)) = stack.pop() {
            if depth > 16 {
                continue;
            }
            for e in std::fs::read_dir(&d)? {
                let e = e?;
                let p = e.path();
                if e.file_type()?.is_dir() {
                    stack.push((p, depth + 1));
                } else if let Ok(rel) = p.strip_prefix(&root) {
                    let name = rel.to_string_lossy().replace('\\', "/");
                    if name.ends_with(".tmp") || check_branch_name(&name).is_err() {
                        continue;
                    }
                    if let Some(h) = self.branch_commit(&name)? {
                        out.push((name, h));
                    }
                }
            }
        }
        out.sort();
        Ok(out)
    }

    // ---------- operations

    /// Record `m` as a new commit on HEAD. Refused when nothing changed since HEAD's commit.
    pub fn commit(&self, m: &Model, message: &str, author: &str, time: i64) -> Result<Hash> {
        if message.trim().is_empty() {
            return Err(RepoError::Bad("a commit needs a message".into()));
        }
        let tree = self.write_tree(m)?;
        let parent = self.head_commit()?;
        if let Some(p) = &parent
            && self.commit_info(p)?.tree == tree
        {
            return Err(RepoError::NothingToCommit(p[..12].to_owned()));
        }
        let c = Commit {
            tree,
            parents: parent.into_iter().collect(),
            author: author.to_owned(),
            time,
            message: message.trim().to_owned(),
        };
        let hash = self.put(&c)?;
        match self.head()? {
            Head::Branch(b) => self.set_branch(&b, &hash)?,
            Head::Detached(_) => self.set_head(&Head::Detached(hash.clone()))?,
        }
        Ok(hash)
    }

    /// A commit from a revision: `HEAD`, a branch name, a full hash or a unique prefix of at
    /// least 4 hex digits, each optionally followed by `~n` (n first parents back).
    pub fn resolve(&self, rev: &str) -> Result<Hash> {
        let unknown = || RepoError::UnknownRev(rev.to_owned());
        let (base, back) = match rev.split_once('~') {
            Some((b, n)) => (
                b,
                if n.is_empty() {
                    1
                } else {
                    n.parse::<usize>().map_err(|_| unknown())?
                },
            ),
            None => (rev, 0),
        };
        let mut h = if base == "HEAD" || base.is_empty() {
            self.head_commit()?.ok_or_else(unknown)?
        } else if check_branch_name(base).is_ok()
            && let Some(h) = self.branch_commit(base)?
        {
            h
        } else if base.len() >= 4 && base.bytes().all(|b| b.is_ascii_hexdigit()) {
            self.find_prefix(&base.to_ascii_lowercase())?
                .ok_or_else(unknown)?
        } else {
            return Err(unknown());
        };
        for _ in 0..back.min(MAX_WALK) {
            h = self
                .commit_info(&h)?
                .parents
                .first()
                .cloned()
                .ok_or_else(unknown)?;
        }
        Ok(h)
    }

    /// The commit whose hash starts with `prefix` (only commits are considered).
    fn find_prefix(&self, prefix: &str) -> Result<Option<Hash>> {
        let dir = self.dir.join("objects").join(&prefix[..2]);
        if !dir.exists() {
            return Ok(None);
        }
        let mut found = None;
        for e in std::fs::read_dir(dir)? {
            let name = format!("{}{}", &prefix[..2], e?.file_name().to_string_lossy());
            if name.starts_with(prefix) && is_hash(&name) && self.get::<Commit>(&name).is_ok() {
                if found.is_some() {
                    return Err(RepoError::Bad(format!("{prefix} is ambiguous")));
                }
                found = Some(name);
            }
        }
        Ok(found)
    }

    /// The model at a revision.
    pub fn model_at(&self, rev: &str) -> Result<Model> {
        let c = self.commit_info(&self.resolve(rev)?)?;
        self.read_tree(&c.tree)
    }

    /// Commits from `rev` back along first parents, newest first, at most `limit`.
    pub fn log(&self, rev: &str, limit: usize) -> Result<Vec<(Hash, Commit)>> {
        let mut out = Vec::new();
        let mut next = Some(self.resolve(rev)?);
        while let Some(h) = next {
            if out.len() >= limit.min(MAX_WALK) {
                break;
            }
            let c = self.commit_info(&h)?;
            next = c.parents.first().cloned();
            out.push((h, c));
        }
        Ok(out)
    }

    /// What changed in the working model since HEAD's commit (everything, before the first).
    pub fn status(&self, working: &Model) -> Result<Vec<Change>> {
        let base = match self.head_commit()? {
            Some(h) => self.read_tree(&self.commit_info(&h)?.tree)?,
            None => Model {
                project_id: working.project_id,
                project_name: working.project_name.clone(),
                ..Default::default()
            },
        };
        Ok(diff(&base, working))
    }

    /// Create a branch at a revision (HEAD by default).
    pub fn create_branch(&self, name: &str, at: Option<&str>) -> Result<Hash> {
        check_branch_name(name)?;
        if self.branch_commit(name)?.is_some() {
            return Err(RepoError::Bad(format!("branch {name} already exists")));
        }
        let h = self.resolve(at.unwrap_or("HEAD"))?;
        self.set_branch(name, &h)?;
        Ok(h)
    }

    /// Point HEAD at a branch (or, for any other revision, at that commit, detached) and return
    /// the model to write to the working file.
    pub fn switch(&self, rev: &str) -> Result<Model> {
        if check_branch_name(rev).is_ok()
            && let Some(h) = self.branch_commit(rev)?
        {
            let m = self.read_tree(&self.commit_info(&h)?.tree)?;
            self.set_head(&Head::Branch(rev.to_owned()))?;
            return Ok(m);
        }
        let h = self.resolve(rev)?;
        let m = self.read_tree(&self.commit_info(&h)?.tree)?;
        self.set_head(&Head::Detached(h))?;
        Ok(m)
    }
}

/// Which side wins when both changed the same object differently.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Prefer {
    /// Report conflicts and merge nothing.
    Neither,
    Ours,
    Theirs,
}

/// An object both sides changed differently since their common base.
#[derive(Clone, Debug, PartialEq)]
pub struct Conflict {
    pub kind: &'static str,
    pub id: Option<GlobalId>,
    pub name: String,
}

impl std::fmt::Display for Conflict {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self.id {
            Some(id) => write!(f, "{} {} [{id}]", self.kind, self.name),
            None => write!(f, "{} {}", self.kind, self.name),
        }
    }
}

/// What a merge did.
#[derive(Clone, Debug, PartialEq)]
pub enum Merged {
    /// HEAD already contains the other side.
    UpToDate,
    /// HEAD was behind: it now points at the other commit.
    FastForward(Hash),
    /// A merge commit with two parents.
    Commit(Hash),
}

/// Three-way merge, object by object (matched by GlobalId): a side that left an object as it
/// was in `base` takes the other side's version (changed, added or removed). When both changed
/// the same object, its fields are merged the same way (one side's height, the other's width);
/// only a field both changed differently is a conflict.
pub fn merge_models(
    base: &Model,
    ours: &Model,
    theirs: &Model,
    prefer: Prefer,
) -> (Model, Vec<Conflict>) {
    fn pick<T: Clone + PartialEq>(
        b: Option<&T>,
        o: Option<&T>,
        t: Option<&T>,
        prefer: Prefer,
    ) -> (Option<T>, bool) {
        if o == t || t == b {
            (o.cloned(), false)
        } else if o == b {
            (t.cloned(), false)
        } else {
            let take = if prefer == Prefer::Theirs { t } else { o };
            (take.cloned(), true)
        }
    }
    /// Tagged enums (generators' "kind", view kinds' "type") are one variant on every side.
    fn same_variant(
        a: &serde_json::Map<String, Value>,
        b: &serde_json::Map<String, Value>,
    ) -> bool {
        ["kind", "type"].iter().all(|k| {
            let tag =
                |m: &serde_json::Map<String, Value>| m.get(*k).filter(|v| v.is_string()).cloned();
            tag(a) == tag(b)
        })
    }
    /// Field-level three-way merge of JSON values: objects (of the same `kind`, for generators)
    /// merge key by key; anything else is taken whole. Returns the paths in conflict.
    fn merge_value(
        b: Option<&Value>,
        o: Option<&Value>,
        t: Option<&Value>,
        path: &str,
        prefer: Prefer,
        conflicts: &mut Vec<String>,
    ) -> Option<Value> {
        if let (Some(Value::Object(bo)), Some(Value::Object(oo)), Some(Value::Object(to))) =
            (b, o, t)
            && same_variant(bo, oo)
            && same_variant(oo, to)
            && o != t
        {
            let keys: std::collections::BTreeSet<&String> =
                bo.keys().chain(oo.keys()).chain(to.keys()).collect();
            let mut out = serde_json::Map::new();
            for k in keys {
                let p = if path.is_empty() {
                    k.clone()
                } else {
                    format!("{path}.{k}")
                };
                if let Some(v) = merge_value(bo.get(k), oo.get(k), to.get(k), &p, prefer, conflicts)
                {
                    out.insert(k.clone(), v);
                }
            }
            return Some(Value::Object(out));
        }
        let (v, conflict) = pick(b, o, t, prefer);
        if conflict {
            conflicts.push(path.to_owned());
        }
        v
    }
    fn map<T: Clone + PartialEq + Serialize + DeserializeOwned>(
        kind: &'static str,
        b: &BTreeMap<GlobalId, T>,
        o: &BTreeMap<GlobalId, T>,
        t: &BTreeMap<GlobalId, T>,
        name: impl Fn(&T) -> String,
        prefer: Prefer,
        conflicts: &mut Vec<Conflict>,
    ) -> BTreeMap<GlobalId, T> {
        let ids: std::collections::BTreeSet<&GlobalId> =
            b.keys().chain(o.keys()).chain(t.keys()).collect();
        let mut out = BTreeMap::new();
        let json = |v: Option<&T>| v.map(|v| serde_json::to_value(v).expect("serializes"));
        for id in ids {
            let (bv, ov, tv) = (b.get(id), o.get(id), t.get(id));
            let (v, conflict) = pick(bv, ov, tv, prefer);
            if !conflict {
                if let Some(v) = v {
                    out.insert(*id, v);
                }
                continue;
            }
            // Both changed it: merge field by field.
            let mut paths = Vec::new();
            let merged = merge_value(
                json(bv).as_ref(),
                json(ov).as_ref(),
                json(tv).as_ref(),
                "",
                prefer,
                &mut paths,
            );
            let n = ov.or(tv).or(bv).map(&name).unwrap_or_default();
            // A field merge that doesn't make a valid object again counts as one conflict.
            let merged: Option<Option<T>> = match merged {
                None => Some(None),
                Some(v) => serde_json::from_value(v).ok().map(Some),
            };
            let merged = match merged {
                Some(m) => m,
                None => {
                    paths = vec![String::new()];
                    v
                }
            };
            for p in paths {
                conflicts.push(Conflict {
                    kind,
                    id: Some(*id),
                    name: if p.is_empty() {
                        n.clone()
                    } else {
                        format!("{n}: {p}")
                    },
                });
            }
            if let Some(m) = merged {
                out.insert(*id, m);
            }
        }
        out
    }
    let mut conflicts = Vec::new();
    let (project_name, c) = pick(
        Some(&base.project_name),
        Some(&ours.project_name),
        Some(&theirs.project_name),
        prefer,
    );
    if c {
        conflicts.push(Conflict {
            kind: "IfcProject",
            id: None,
            name: "project name".into(),
        });
    }
    // Project information, key by key.
    let mut project = BTreeMap::new();
    let keys: std::collections::BTreeSet<&String> = base
        .project
        .keys()
        .chain(ours.project.keys())
        .chain(theirs.project.keys())
        .collect();
    for k in keys {
        let (v, c) = pick(
            base.project.get(k),
            ours.project.get(k),
            theirs.project.get(k),
            prefer,
        );
        if c {
            conflicts.push(Conflict {
                kind: "IfcProject",
                id: None,
                name: format!("project information {k}"),
            });
        }
        if let Some(v) = v {
            project.insert(k.clone(), v);
        }
    }
    // Schedules, by name: each one picked like an element.
    let mut schedules = Vec::new();
    let names: Vec<String> = {
        let mut n: Vec<String> = Vec::new();
        for s in base
            .schedules
            .iter()
            .chain(&ours.schedules)
            .chain(&theirs.schedules)
        {
            if !n.contains(&s.name) {
                n.push(s.name.clone());
            }
        }
        n
    };
    for name in names {
        let get = |v: &[crate::schedule::ScheduleDef]| v.iter().find(|s| s.name == name).cloned();
        let (b, o, t) = (
            get(&base.schedules),
            get(&ours.schedules),
            get(&theirs.schedules),
        );
        let (s, c) = pick(b.as_ref(), o.as_ref(), t.as_ref(), prefer);
        if c {
            conflicts.push(Conflict {
                kind: "schedule",
                id: None,
                name,
            });
        }
        if let Some(s) = s {
            schedules.push(s);
        }
    }
    let m = Model {
        project_id: ours.project_id,
        project,
        schedules,
        // Whichever side changed it (ours when both did): it is set on import, rarely after.
        georef: if ours.georef != base.georef {
            ours.georef.clone()
        } else {
            theirs.georef.clone()
        },
        project_name: project_name.unwrap_or_default(),
        storeys: map(
            "IfcBuildingStorey",
            &base.storeys,
            &ours.storeys,
            &theirs.storeys,
            |s| s.name.clone(),
            prefer,
            &mut conflicts,
        ),
        materials: map(
            "IfcMaterial",
            &base.materials,
            &ours.materials,
            &theirs.materials,
            |s| s.name.clone(),
            prefer,
            &mut conflicts,
        ),
        elements: map(
            "element",
            &base.elements,
            &ours.elements,
            &theirs.elements,
            |e| format!("{} {}", e.class_name(), e.name),
            prefer,
            &mut conflicts,
        ),
        views: map(
            "view",
            &base.views,
            &ours.views,
            &theirs.views,
            |v| v.name.clone(),
            prefer,
            &mut conflicts,
        ),
        sheets: map(
            "sheet",
            &base.sheets,
            &ours.sheets,
            &theirs.sheets,
            |s| format!("{} {}", s.number, s.name),
            prefer,
            &mut conflicts,
        ),
    };
    (m, conflicts)
}

impl Repo {
    /// All ancestors of a commit (itself included), breadth first, bounded.
    fn ancestors(&self, h: &str) -> Result<Vec<Hash>> {
        let mut seen = std::collections::HashSet::new();
        let mut order = Vec::new();
        let mut queue = std::collections::VecDeque::from([h.to_owned()]);
        while let Some(c) = queue.pop_front() {
            if order.len() >= MAX_WALK {
                break;
            }
            if seen.insert(c.clone()) {
                queue.extend(self.commit_info(&c)?.parents);
                order.push(c);
            }
        }
        Ok(order)
    }

    /// Nearest common ancestor of two commits.
    pub fn merge_base(&self, a: &str, b: &str) -> Result<Option<Hash>> {
        let of_a: std::collections::HashSet<Hash> = self.ancestors(a)?.into_iter().collect();
        Ok(self.ancestors(b)?.into_iter().find(|c| of_a.contains(c)))
    }

    /// Merge a revision into HEAD. With conflicts and `Prefer::Neither` nothing changes and the
    /// conflicts are returned as the error's list. Returns what happened and the model to write
    /// to the working file.
    pub fn merge(
        &self,
        rev: &str,
        prefer: Prefer,
        author: &str,
        time: i64,
    ) -> Result<std::result::Result<(Merged, Model), Vec<Conflict>>> {
        self.merge_with_message(rev, prefer, author, time, &format!("Merge {rev}"))
    }

    /// `merge`, with the message of the merge commit.
    pub(crate) fn merge_with_message(
        &self,
        rev: &str,
        prefer: Prefer,
        author: &str,
        time: i64,
        message: &str,
    ) -> Result<std::result::Result<(Merged, Model), Vec<Conflict>>> {
        let theirs = self.resolve(rev)?;
        let Some(ours) = self.head_commit()? else {
            return Err(RepoError::Bad("nothing committed yet".into()));
        };
        let base = self.merge_base(&ours, &theirs)?;
        if base.as_deref() == Some(theirs.as_str()) {
            return Ok(Ok((Merged::UpToDate, self.model_at(&ours)?)));
        }
        if base.as_deref() == Some(ours.as_str()) {
            match self.head()? {
                Head::Branch(b) => self.set_branch(&b, &theirs)?,
                Head::Detached(_) => self.set_head(&Head::Detached(theirs.clone()))?,
            }
            return Ok(Ok((
                Merged::FastForward(theirs.clone()),
                self.model_at(&theirs)?,
            )));
        }
        let m_ours = self.model_at(&ours)?;
        let m_theirs = self.model_at(&theirs)?;
        let m_base = match &base {
            Some(b) => self.model_at(b)?,
            None => Model {
                project_id: m_ours.project_id,
                ..Default::default()
            },
        };
        let (merged, conflicts) = merge_models(&m_base, &m_ours, &m_theirs, prefer);
        if !conflicts.is_empty() && prefer == Prefer::Neither {
            return Ok(Err(conflicts));
        }
        // Each side valid does not make the union valid (an element on a storey the other side
        // removed).
        merged
            .validate()
            .map_err(|e| RepoError::Bad(format!("the merged model is invalid: {e}")))?;
        let merged = crate::version::canonical_model(merged);
        let tree = self.write_tree(&merged)?;
        let c = Commit {
            tree,
            parents: vec![ours, theirs],
            author: author.to_owned(),
            time,
            message: message.to_owned(),
        };
        let hash = self.put(&c)?;
        match self.head()? {
            Head::Branch(b) => self.set_branch(&b, &hash)?,
            Head::Detached(_) => self.set_head(&Head::Detached(hash.clone()))?,
        }
        Ok(Ok((Merged::Commit(hash), merged)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::demo::{FrameSpec, concrete_frame};
    use crate::{Command, IfcClass};

    struct Tmp(PathBuf);
    impl Drop for Tmp {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    fn tmp(name: &str) -> (Tmp, PathBuf) {
        let d = std::env::temp_dir().join(format!(
            "bimble-repo-{name}-{}-{:?}",
            std::process::id(),
            std::thread::current().id()
        ));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        let model = d.join("m.bimble");
        (Tmp(d), model)
    }

    fn rename_first_beam(m: &mut Model, name: &str) {
        let id = *m
            .elements
            .iter()
            .find(|(_, e)| e.class == IfcClass::IfcBeam)
            .unwrap()
            .0;
        Command::Rename {
            id,
            name: name.into(),
        }
        .apply(m)
        .unwrap();
    }

    #[test]
    fn commit_log_diff_and_model_at_round_trip() {
        let (_t, path) = tmp("basic");
        let r = Repo::init(&path).unwrap();
        assert!(Repo::init(&path).is_err(), "init twice");
        let mut m = concrete_frame(&FrameSpec::default());
        // Before the first commit everything is new.
        assert_eq!(
            r.status(&m).unwrap().len(),
            m.storeys.len() + m.materials.len() + m.elements.len()
        );
        let c1 = r.commit(&m, "first", "pierre", 1_000).unwrap();
        assert!(matches!(
            r.commit(&m, "again", "pierre", 1_001),
            Err(RepoError::NothingToCommit(_))
        ));
        assert!(r.status(&m).unwrap().is_empty());
        rename_first_beam(&mut m, "B-renamed");
        let st = r.status(&m).unwrap();
        assert_eq!(st.len(), 1, "{st:?}");
        let c2 = r.commit(&m, "rename a beam", "pierre", 2_000).unwrap();
        assert_eq!(r.resolve("HEAD").unwrap(), c2);
        assert_eq!(r.resolve("main~1").unwrap(), c1);
        assert_eq!(r.resolve(&c1[..8]).unwrap(), c1);
        assert!(r.resolve("main~5").is_err());
        let log = r.log("HEAD", 10).unwrap();
        assert_eq!(
            log.iter()
                .map(|(_, c)| c.message.as_str())
                .collect::<Vec<_>>(),
            ["rename a beam", "first"]
        );
        assert_eq!(log[0].1.parents, vec![c1.clone()]);
        // The model comes back exactly, hash included.
        let back = r.model_at("HEAD").unwrap();
        assert_eq!(back, m);
        assert_eq!(
            crate::version::model_hash(&back),
            crate::version::model_hash(&m)
        );
        assert_eq!(diff(&r.model_at(&c1).unwrap(), &back).len(), 1);
    }

    /// Where the model is and its project north (ADR 0045) go through the history, and count
    /// in the model's hash.
    #[test]
    fn the_georeference_is_kept_in_the_history() {
        let (_t, path) = tmp("georef");
        let r = Repo::init(&path).unwrap();
        let plain = concrete_frame(&FrameSpec::default());
        let mut m = plain.clone();
        m.georef = crate::georef::Georef::from_frame(
            [0.866025403784, 0.5],
            [2_600_000_000.0, 1_200_000_000.0, 0.0],
        );
        assert_ne!(
            crate::version::model_hash(&m),
            crate::version::model_hash(&plain)
        );
        let c = r.commit(&m, "placed", "pierre", 1_000).unwrap();
        assert_eq!(r.model_at(&c).unwrap().georef, m.georef);
    }

    #[test]
    fn unchanged_elements_are_stored_once() {
        let (_t, path) = tmp("dedup");
        let r = Repo::init(&path).unwrap();
        let mut m = concrete_frame(&FrameSpec::default());
        let count = |r: &Repo| walk(&r.dir().join("objects"));
        fn walk(d: &Path) -> usize {
            std::fs::read_dir(d)
                .unwrap()
                .map(|e| {
                    let p = e.unwrap().path();
                    if p.is_dir() { walk(&p) } else { 1 }
                })
                .sum()
        }
        r.commit(&m, "first", "a", 1).unwrap();
        let n1 = count(&r);
        rename_first_beam(&mut m, "x");
        r.commit(&m, "second", "a", 2).unwrap();
        // One new element, one new tree, one new commit.
        assert_eq!(count(&r), n1 + 3);
    }

    #[test]
    fn branches_and_switch() {
        let (_t, path) = tmp("branch");
        let r = Repo::init(&path).unwrap();
        let mut m = concrete_frame(&FrameSpec::default());
        let c1 = r.commit(&m, "first", "a", 1).unwrap();
        r.create_branch("variant/b", None).unwrap();
        assert!(r.create_branch("variant/b", None).is_err());
        for bad in ["", "../x", "a/../b", "-x", "a b", "HEAD", ".hidden", "a//b"] {
            assert!(r.create_branch(bad, None).is_err(), "{bad:?}");
        }
        let back = r.switch("variant/b").unwrap();
        assert_eq!(back, m);
        assert_eq!(r.head().unwrap(), Head::Branch("variant/b".into()));
        rename_first_beam(&mut m, "on b");
        let c2 = r.commit(&m, "b work", "a", 2).unwrap();
        assert_eq!(r.branch_commit("main").unwrap(), Some(c1.clone()));
        assert_eq!(
            r.branches().unwrap(),
            vec![("main".into(), c1.clone()), ("variant/b".into(), c2)]
        );
        // Back to main: the model without the rename. A hash detaches HEAD.
        let main = r.switch("main").unwrap();
        assert_ne!(main, m);
        r.switch(&c1[..10]).unwrap();
        assert_eq!(r.head().unwrap(), Head::Detached(c1));
    }

    fn first_column(m: &Model) -> GlobalId {
        *m.elements
            .iter()
            .find(|(_, e)| e.class == IfcClass::IfcColumn)
            .unwrap()
            .0
    }

    #[test]
    fn merges_take_both_sides_and_report_conflicts() {
        let (_t, path) = tmp("merge");
        let r = Repo::init(&path).unwrap();
        let base = concrete_frame(&FrameSpec::default());
        r.commit(&base, "base", "a", 1).unwrap();
        r.create_branch("b", None).unwrap();
        // main renames a beam.
        let mut ours = base.clone();
        rename_first_beam(&mut ours, "main beam");
        r.commit(&ours, "main", "a", 2).unwrap();
        // b renames a column and removes nothing else.
        r.switch("b").unwrap();
        let mut theirs = base.clone();
        let col = first_column(&theirs);
        Command::Rename {
            id: col,
            name: "b column".into(),
        }
        .apply(&mut theirs)
        .unwrap();
        let cb = r.commit(&theirs, "b", "a", 3).unwrap();
        r.switch("main").unwrap();
        let (how, merged) = r.merge("b", Prefer::Neither, "a", 4).unwrap().unwrap();
        assert!(matches!(how, Merged::Commit(_)));
        assert_eq!(merged.elements[&col].name, "b column");
        assert!(merged.elements.values().any(|e| e.name == "main beam"));
        assert_eq!(r.model_at("HEAD").unwrap(), merged);
        assert_eq!(
            r.commit_info(&r.resolve("HEAD").unwrap())
                .unwrap()
                .parents
                .len(),
            2
        );
        // Merging again: nothing to do.
        let (how, _) = r.merge("b", Prefer::Neither, "a", 5).unwrap().unwrap();
        assert_eq!(how, Merged::UpToDate);
        // b is behind main now: merging main into b fast-forwards.
        r.switch("b").unwrap();
        let (how, m) = r.merge("main", Prefer::Neither, "a", 6).unwrap().unwrap();
        assert!(matches!(how, Merged::FastForward(_)));
        assert_eq!(m, merged);
        assert_ne!(r.resolve("b").unwrap(), cb);

        // Both rename the same column: a conflict, unless a side is preferred.
        let mut x = merged.clone();
        Command::Rename {
            id: col,
            name: "x".into(),
        }
        .apply(&mut x)
        .unwrap();
        r.commit(&x, "x", "a", 7).unwrap();
        r.switch("main").unwrap();
        let mut y = merged.clone();
        Command::Rename {
            id: col,
            name: "y".into(),
        }
        .apply(&mut y)
        .unwrap();
        r.commit(&y, "y", "a", 8).unwrap();
        let conflicts = r.merge("b", Prefer::Neither, "a", 9).unwrap().unwrap_err();
        assert_eq!(conflicts.len(), 1);
        assert_eq!(conflicts[0].id, Some(col));
        let (_, m) = r.merge("b", Prefer::Theirs, "a", 9).unwrap().unwrap();
        assert_eq!(m.elements[&col].name, "x");
    }

    /// Both sides change different fields of one element: both changes are kept.
    #[test]
    fn field_level_merge_keeps_both_changes() {
        use crate::Generator;
        let base = concrete_frame(&FrameSpec::default());
        let id = *base
            .elements
            .iter()
            .find(|(_, e)| matches!(e.generator, Generator::RectBeam { .. }))
            .unwrap()
            .0;
        let set = |m: &Model, f: &dyn Fn(&mut f64, &mut f64)| {
            let mut m = m.clone();
            let mut g = m.elements[&id].generator.clone();
            if let Generator::RectBeam { width, height, .. } = &mut g {
                f(width, height);
            }
            Command::SetGenerator { id, generator: g }
                .apply(&mut m)
                .unwrap();
            m
        };
        let ours = set(&base, &|w, _| *w = 300.0);
        let theirs = set(&base, &|_, h| *h = 650.0);
        let (m, c) = merge_models(&base, &ours, &theirs, Prefer::Neither);
        assert!(c.is_empty(), "{c:?}");
        let Generator::RectBeam { width, height, .. } = m.elements[&id].generator else {
            panic!()
        };
        assert_eq!((width, height), (300.0, 650.0));
        // The same field changed differently: one conflict, naming the field.
        let theirs = set(&base, &|w, _| *w = 350.0);
        let (_, c) = merge_models(&base, &ours, &theirs, Prefer::Neither);
        assert_eq!(c.len(), 1);
        assert!(c[0].name.ends_with("generator.width"), "{}", c[0]);
    }

    /// Views are versioned like elements: committed, read back, merged field by field.
    #[test]
    fn sheets_are_committed_and_merged() {
        let (_t, path) = tmp("sheets");
        let r = Repo::init(&path).unwrap();
        let mut m = concrete_frame(&FrameSpec::default());
        let v = crate::view::default_views(&m.levels())
            .into_iter()
            .find(|v| v.name == "Plan Niveau 1")
            .unwrap();
        let mut s = crate::sheet::Sheet::new("S-101", "Plans");
        s.placed.push(crate::sheet::Placed {
            id: GlobalId::new(),
            view: v.id,
            scale: 100,
            at_mm: [400.0, 300.0],
        });
        Command::AddSheet(s.clone()).apply(&mut m).unwrap();
        r.commit(&m, "a sheet", "a", 1).unwrap();
        assert_eq!(r.model_at("HEAD").unwrap(), m);
        // A new name on one branch, a new paper on the other: both kept.
        r.create_branch("b", None).unwrap();
        let mut ours = m.clone();
        let mut renamed = s.clone();
        renamed.name = "Formwork".into();
        Command::SetSheet(renamed).apply(&mut ours).unwrap();
        r.commit(&ours, "rename", "a", 2).unwrap();
        r.switch("b").unwrap();
        let mut theirs = m.clone();
        let mut a0 = s.clone();
        a0.paper = "A0".into();
        Command::SetSheet(a0).apply(&mut theirs).unwrap();
        r.commit(&theirs, "paper", "a", 3).unwrap();
        r.switch("main").unwrap();
        let (_, merged) = r.merge("b", Prefer::Neither, "a", 4).unwrap().unwrap();
        let ms = &merged.sheets[&s.id];
        assert_eq!((ms.name.as_str(), ms.paper.as_str()), ("Formwork", "A0"));
    }

    /// Project information is committed, read back, and merged key by key (ADR 0030).
    #[test]
    fn project_information_is_committed_and_merged() {
        let (_t, path) = tmp("project");
        let r = Repo::init(&path).unwrap();
        let mut m = concrete_frame(&FrameSpec::default());
        let info = |pairs: &[(&str, &str)]| -> BTreeMap<String, String> {
            pairs
                .iter()
                .map(|(k, v)| (k.to_string(), v.to_string()))
                .collect()
        };
        Command::SetProject {
            project: info(&[("client", "Synthetic client"), ("phase", "Tender")]),
        }
        .apply(&mut m)
        .unwrap();
        r.commit(&m, "project", "a", 1).unwrap();
        assert_eq!(r.model_at("HEAD").unwrap(), m);
        r.create_branch("b", None).unwrap();
        let mut ours = m.clone();
        Command::SetProject {
            project: info(&[("client", "Synthetic client"), ("phase", "Execution")]),
        }
        .apply(&mut ours)
        .unwrap();
        r.commit(&ours, "phase", "a", 2).unwrap();
        r.switch("b").unwrap();
        let mut theirs = m.clone();
        Command::SetProject {
            project: info(&[
                ("client", "Synthetic client"),
                ("phase", "Tender"),
                ("project_number", "P-001"),
            ]),
        }
        .apply(&mut theirs)
        .unwrap();
        r.commit(&theirs, "number", "a", 3).unwrap();
        r.switch("main").unwrap();
        let (_, merged) = r.merge("b", Prefer::Neither, "a", 4).unwrap().unwrap();
        assert_eq!(merged.project["phase"], "Execution");
        assert_eq!(merged.project["project_number"], "P-001");
    }

    #[test]
    fn views_are_committed_and_merged() {
        let (_t, path) = tmp("views");
        let r = Repo::init(&path).unwrap();
        let mut m = concrete_frame(&FrameSpec::default());
        let v = crate::view::default_views(&m.levels())
            .into_iter()
            .find(|v| v.name == "Plan Niveau 1")
            .unwrap();
        Command::AddView(v.clone()).apply(&mut m).unwrap();
        r.commit(&m, "a plan view", "a", 1).unwrap();
        assert_eq!(r.model_at("HEAD").unwrap(), m);
        // Rename on one branch, change the cut on the other: both kept.
        r.create_branch("b", None).unwrap();
        let mut ours = m.clone();
        let mut renamed = v.clone();
        renamed.name = "Formwork N1".into();
        Command::SetView(renamed).apply(&mut ours).unwrap();
        r.commit(&ours, "rename", "a", 2).unwrap();
        r.switch("b").unwrap();
        let mut theirs = m.clone();
        let mut cut = v.clone();
        if let crate::view::ViewKind::Plan { range, .. } = &mut cut.kind {
            range.cut.offset_mm = 1000.0;
        }
        Command::SetView(cut).apply(&mut theirs).unwrap();
        r.commit(&theirs, "cut", "a", 3).unwrap();
        r.switch("main").unwrap();
        let (_, merged) = r.merge("b", Prefer::Neither, "a", 4).unwrap().unwrap();
        let mv = &merged.views[&v.id];
        assert_eq!(mv.name, "Formwork N1");
        assert!(
            matches!(&mv.kind, crate::view::ViewKind::Plan { range, .. } if range.cut.offset_mm == 1000.0)
        );
    }

    #[test]
    fn damaged_objects_are_reported() {
        let (_t, path) = tmp("damaged");
        let r = Repo::init(&path).unwrap();
        let m = concrete_frame(&FrameSpec::default());
        let c = r.commit(&m, "first", "a", 1).unwrap();
        let tree = r.commit_info(&c).unwrap().tree;
        let p = r.object_path(&tree);
        let mut bytes = std::fs::read(&p).unwrap();
        bytes[10] ^= 1;
        std::fs::write(&p, bytes).unwrap();
        let e = r.model_at("HEAD").unwrap_err();
        assert!(matches!(e, RepoError::Damaged(..)), "{e}");
        std::fs::write(r.dir().join("HEAD"), "ref: ../../etc\n").unwrap();
        assert!(r.head().is_err());
    }
}
