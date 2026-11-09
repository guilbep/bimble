//! Working together through a shared folder (ADR 0038): clone, fetch, push, synchronize, and
//! elements borrowed until their owner synchronizes.
//!
//! A remote is a history folder with no model file next to it (`format`, `refs/heads`,
//! `objects`, as in ADR 0017), plus:
//!
//! ```text
//! lock              held while a ref or a borrow list is written: "<who> <time>"
//! borrowed/<hash>   one person's borrowed elements (file named by the hash of their e-mail)
//! ```
//!
//! The remote is untrusted input: every object copied from it is checked against its hash, refs
//! and borrow lists are bounded, walks are bounded.

use crate::repo::{Commit, Conflict, Hash, Head, Merged, Prefer, Repo, RepoError, Tree};
use crate::{GlobalId, Model};
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::path::{Path, PathBuf};

type Result<T> = std::result::Result<T, RepoError>;

/// Commits copied at most by one fetch or push.
const MAX_COMMITS: usize = 1_000_000;
/// Borrow lists read at most, and their size.
const MAX_BORROWERS: usize = 10_000;
const MAX_BORROW_BYTES: u64 = 16 * 1024 * 1024;
/// Times a synchronization fetches and merges again when someone pushed meanwhile.
const SYNC_ATTEMPTS: usize = 3;

/// The remote's folder, as set by `set_remote` or `clone`.
pub fn remote_of(local: &Repo) -> Result<Option<PathBuf>> {
    let p = local.dir().join("remote");
    if !p.exists() {
        return Ok(None);
    }
    if std::fs::metadata(&p)?.len() > 4096 {
        return Err(RepoError::Bad("the remote setting is too large".into()));
    }
    Ok(Some(PathBuf::from(std::fs::read_to_string(p)?.trim())))
}

/// Stored absolute, so it works from any working directory.
pub fn set_remote(local: &Repo, dir: &Path) -> Result<()> {
    let dir = std::path::absolute(dir)?;
    let text = dir
        .to_str()
        .ok_or_else(|| RepoError::Bad("the remote's path is not valid text".into()))?;
    crate::file::write_atomic(&local.dir().join("remote"), format!("{text}\n").as_bytes())?;
    Ok(())
}

/// Open the remote in `dir`, creating an empty one when the folder has no history yet.
pub fn open_or_init(dir: &Path) -> Result<Repo> {
    if dir.join("format").exists() {
        Repo::open_dir(dir)
    } else {
        Repo::init_dir(dir)
    }
}

/// Every object a tree names.
fn tree_objects(t: &Tree) -> impl Iterator<Item = &Hash> {
    t.storeys
        .values()
        .chain(t.materials.values())
        .chain(t.elements.values())
        .chain(t.views.values())
        .chain(t.sheets.values())
}

/// Copy `tip` and all it needs from `from` to `to`. Returns the number of commits copied.
///
/// The write order is the crash-safety invariant: a tree's objects before the tree, a tree
/// before its commit, parents before children. So a commit present in `to` always has all its
/// ancestors and objects there too, and the walk stops at it; an interrupted copy leaves only
/// unreferenced objects, never a hole the next copy would skip.
fn copy_commits(from: &Repo, to: &Repo, tip: &str) -> Result<usize> {
    // Depth-first, post-order: a commit is written after all its parents.
    let mut stack = vec![(tip.to_owned(), false)];
    let mut seen = HashSet::new();
    let mut copied = 0;
    while let Some((h, parents_done)) = stack.pop() {
        if parents_done {
            let bytes = from.get_bytes(&h)?;
            let c: Commit = serde_json::from_slice(&bytes)
                .map_err(|e| RepoError::Damaged(h.clone(), e.to_string()))?;
            if !to.has(&c.tree) {
                let tree_bytes = from.get_bytes(&c.tree)?;
                let t: Tree = serde_json::from_slice(&tree_bytes)
                    .map_err(|e| RepoError::Damaged(c.tree.clone(), e.to_string()))?;
                for o in tree_objects(&t) {
                    if !to.has(o) {
                        to.put_bytes(&from.get_bytes(o)?)?;
                    }
                }
                to.put_bytes(&tree_bytes)?;
            }
            to.put_bytes(&bytes)?;
            copied += 1;
            continue;
        }
        if to.has(&h) || !seen.insert(h.clone()) {
            continue;
        }
        if seen.len() > MAX_COMMITS {
            return Err(RepoError::Bad("too many commits to copy".into()));
        }
        let c = from.commit_info(&h)?;
        stack.push((h, true));
        stack.extend(c.parents.into_iter().map(|p| (p, false)));
    }
    Ok(copied)
}

/// Copy the remote's `branch` and everything it needs into the local history. Returns the
/// remote's commit (None when the remote has no such branch). Local refs are not changed.
pub fn fetch(local: &Repo, remote: &Repo, branch: &str) -> Result<Option<Hash>> {
    let tip = remote.branch_commit(branch)?;
    if let Some(t) = &tip {
        copy_commits(remote, local, t)?;
    }
    Ok(tip)
}

/// The remote's lock, released when dropped.
struct Lock(PathBuf);

impl Lock {
    fn take(remote: &Repo, who: &str, time: i64) -> Result<Lock> {
        let p = remote.dir().join("lock");
        match std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&p)
        {
            Ok(mut f) => {
                use std::io::Write;
                f.write_all(format!("{who} {time}\n").as_bytes())?;
                Ok(Lock(p))
            }
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {
                let holder = std::fs::read(&p).unwrap_or_default();
                let holder = String::from_utf8_lossy(&holder[..holder.len().min(200)])
                    .trim()
                    .to_owned();
                Err(RepoError::Bad(format!(
                    "the remote is busy ({holder}); try again, or delete {} if no one is \
                     synchronizing",
                    p.display()
                )))
            }
            Err(e) => Err(e.into()),
        }
    }
}

impl Drop for Lock {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

/// Send the local `branch` to the remote. The remote's branch moves only forward: if it has
/// commits the local branch does not contain, nothing moves and `NotFastForward` is returned.
pub fn push(local: &Repo, remote: &Repo, branch: &str, who: &str, time: i64) -> Result<Hash> {
    let tip = local
        .branch_commit(branch)?
        .ok_or_else(|| RepoError::Bad(format!("nothing committed on {branch}")))?;
    // Objects first (harmless if the ref then does not move), then the ref, under the lock.
    copy_commits(local, remote, &tip)?;
    let _lock = Lock::take(remote, who, time)?;
    if let Some(theirs) = remote.branch_commit(branch)?
        && theirs != tip
        && !(local.has(&theirs) && local.merge_base(&tip, &theirs)?.as_deref() == Some(&theirs))
    {
        return Err(RepoError::NotFastForward(branch.to_owned()));
    }
    remote.set_branch(branch, &tip)?;
    Ok(tip)
}

/// A new local copy of the remote's `branch`: the history next to `model`, and the model to
/// write there.
pub fn clone(remote_dir: &Path, model: &Path, branch: &str) -> Result<Model> {
    let remote = Repo::open_dir(remote_dir)?;
    let tip = remote
        .branch_commit(branch)?
        .ok_or_else(|| RepoError::Bad(format!("the remote has no branch {branch}")))?;
    let local = Repo::init(model)?;
    fetch(&local, &remote, branch)?;
    local.set_branch(branch, &tip)?;
    local.set_head(&Head::Branch(branch.to_owned()))?;
    set_remote(&local, remote_dir)?;
    local.model_at(&tip)
}

// ---------- borrowed elements

#[derive(Debug, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Borrows {
    who: String,
    time: i64,
    ids: BTreeSet<GlobalId>,
}

fn borrow_path(remote: &Repo, who: &str) -> PathBuf {
    let name = blake3::hash(who.as_bytes()).to_hex();
    remote.dir().join("borrowed").join(&name[..32])
}

/// Who borrowed which element, on the remote.
pub fn borrowed(remote: &Repo) -> Result<BTreeMap<GlobalId, String>> {
    let dir = remote.dir().join("borrowed");
    let mut out = BTreeMap::new();
    if !dir.exists() {
        return Ok(out);
    }
    for (i, e) in std::fs::read_dir(dir)?.enumerate() {
        if i >= MAX_BORROWERS {
            return Err(RepoError::Bad("too many borrow lists on the remote".into()));
        }
        let p = e?.path();
        if p.to_string_lossy().ends_with(".tmp") || !p.is_file() {
            continue;
        }
        if std::fs::metadata(&p)?.len() > MAX_BORROW_BYTES {
            return Err(RepoError::Bad(format!("{}: too large", p.display())));
        }
        let b: Borrows = serde_json::from_slice(&std::fs::read(&p)?)
            .map_err(|e| RepoError::Bad(format!("{}: {e}", p.display())))?;
        for id in b.ids {
            out.insert(id, b.who.clone());
        }
    }
    Ok(out)
}

/// Borrow elements for `who`. All or nothing: when some belong to someone else, nothing is
/// borrowed and those are returned with their owner ("this element belongs to <name>").
pub fn borrow(
    remote: &Repo,
    who: &str,
    ids: &[GlobalId],
    time: i64,
) -> Result<Vec<(GlobalId, String)>> {
    let _lock = Lock::take(remote, who, time)?;
    let all = borrowed(remote)?;
    let taken: Vec<(GlobalId, String)> = ids
        .iter()
        .filter_map(|id| all.get(id).filter(|w| *w != who).map(|w| (*id, w.clone())))
        .collect();
    if !taken.is_empty() {
        return Ok(taken);
    }
    let mut mine = Borrows {
        who: who.to_owned(),
        time,
        ids: all
            .iter()
            .filter(|(_, w)| *w == who)
            .map(|(id, _)| *id)
            .collect(),
    };
    mine.ids.extend(ids.iter().copied());
    let p = borrow_path(remote, who);
    std::fs::create_dir_all(p.parent().expect("borrowed/"))?;
    crate::file::write_atomic(&p, &serde_json::to_vec(&mine).expect("serializes"))?;
    Ok(Vec::new())
}

/// Give back all of `who`'s borrowed elements.
pub fn release(remote: &Repo, who: &str) -> Result<()> {
    match std::fs::remove_file(borrow_path(remote, who)) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(e.into()),
        _ => Ok(()),
    }
}

/// Elements whose object differs between two commits (`from` None: all of `to`'s).
fn changed_elements(r: &Repo, from: Option<&str>, to: &str) -> Result<BTreeSet<GlobalId>> {
    let after = r.tree(&r.commit_info(to)?.tree)?.elements;
    let before = match from {
        Some(f) => r.tree(&r.commit_info(f)?.tree)?.elements,
        None => BTreeMap::new(),
    };
    Ok(before
        .keys()
        .chain(after.keys())
        .filter(|id| before.get(*id) != after.get(*id))
        .copied()
        .collect())
}

/// Synchronize: fetch the remote's branch, merge it into the local one, push the result, then
/// give back `who`'s borrowed elements. Stops before pushing anything when the merge has
/// conflicts (unless `prefer` picks a side), or when local changes touch elements someone else
/// borrowed (whatever `prefer` says: their owner synchronizes first); both are returned as
/// conflicts. Returns how the local branch moved and the model to write to the working file.
pub fn sync(
    local: &Repo,
    remote: &Repo,
    who: &str,
    time: i64,
    prefer: Prefer,
) -> Result<std::result::Result<(Merged, Model), Vec<Conflict>>> {
    let Head::Branch(branch) = local.head()? else {
        return Err(RepoError::Bad(
            "not on a branch: switch to one to synchronize".into(),
        ));
    };
    let ours = local
        .head_commit()?
        .ok_or_else(|| RepoError::Bad("nothing committed yet".into()))?;
    for _ in 0..SYNC_ATTEMPTS {
        let theirs = fetch(local, remote, &branch)?;
        // My changes since we last agreed must not touch someone else's borrowed elements.
        let base = match &theirs {
            Some(t) => local.merge_base(&ours, t)?,
            None => None,
        };
        let others = borrowed(remote)?;
        let blocked: Vec<GlobalId> = changed_elements(local, base.as_deref(), &ours)?
            .into_iter()
            .filter(|id| others.get(id).is_some_and(|w| w != who))
            .collect();
        if !blocked.is_empty() {
            let m = local.model_at(&ours)?;
            return Ok(Err(blocked
                .into_iter()
                .map(|id| Conflict {
                    kind: "borrowed",
                    id: Some(id),
                    name: format!(
                        "{} belongs to {}",
                        m.elements
                            .get(&id)
                            .map(|e| format!("{} {}", e.class.name(), e.name))
                            .unwrap_or_else(|| "a removed element".into()),
                        others[&id]
                    ),
                })
                .collect()));
        }
        let (how, model) = match &theirs {
            None => (Merged::UpToDate, local.model_at(&ours)?),
            Some(t) => match local.merge_with_message(
                t,
                prefer,
                who,
                time,
                &format!("Synchronize {branch}"),
            )? {
                Ok(r) => r,
                Err(conflicts) => return Ok(Err(conflicts)),
            },
        };
        match push(local, remote, &branch, who, time) {
            Ok(_) => {
                release(remote, who)?;
                return Ok(Ok((how, model)));
            }
            // Someone pushed between our fetch and our push: fetch and merge again.
            Err(RepoError::NotFastForward(_)) => continue,
            Err(e) => return Err(e),
        }
    }
    Err(RepoError::Bad(
        "others kept pushing while synchronizing; try again".into(),
    ))
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

    /// A shared folder and two people's model files.
    struct Office {
        _t: Tmp,
        share: PathBuf,
        alice: PathBuf,
        bob: PathBuf,
    }

    const ALICE: &str = "alice@example.org";
    const BOB: &str = "bob@example.org";

    /// Alice starts the model, pushes it to the share, Bob clones it.
    fn office(name: &str) -> (Office, Model) {
        let d = std::env::temp_dir().join(format!(
            "bimble-remote-{name}-{}-{:?}",
            std::process::id(),
            std::thread::current().id()
        ));
        let _ = std::fs::remove_dir_all(&d);
        for p in ["share", "alice", "bob"] {
            std::fs::create_dir_all(d.join(p)).unwrap();
        }
        let o = Office {
            share: d.join("share/house.bimble-history"),
            alice: d.join("alice/house.bimble"),
            bob: d.join("bob/house.bimble"),
            _t: Tmp(d),
        };
        let m = concrete_frame(&FrameSpec::default());
        let a = Repo::init(&o.alice).unwrap();
        a.commit(&m, "start", ALICE, 1).unwrap();
        let share = open_or_init(&o.share).unwrap();
        push(&a, &share, "main", ALICE, 1).unwrap();
        set_remote(&a, &o.share).unwrap();
        let mb = clone(&o.share, &o.bob, "main").unwrap();
        assert_eq!(mb, m);
        (o, m)
    }

    fn id_of(m: &Model, class: IfcClass) -> GlobalId {
        *m.elements.iter().find(|(_, e)| e.class == class).unwrap().0
    }

    fn rename(m: &Model, id: GlobalId, name: &str) -> Model {
        let mut m = m.clone();
        Command::Rename {
            id,
            name: name.into(),
        }
        .apply(&mut m)
        .unwrap();
        m
    }

    fn share_main(o: &Office) -> Hash {
        Repo::open_dir(&o.share)
            .unwrap()
            .branch_commit("main")
            .unwrap()
            .unwrap()
    }

    #[test]
    fn two_people_synchronize() {
        let (o, m) = office("sync");
        let (a, b) = (Repo::open(&o.alice).unwrap(), Repo::open(&o.bob).unwrap());
        assert_eq!(remote_of(&b).unwrap(), Some(o.share.clone()));
        let share = Repo::open_dir(&o.share).unwrap();
        let (beam, col) = (id_of(&m, IfcClass::IfcBeam), id_of(&m, IfcClass::IfcColumn));
        // A mistyped or unreachable remote is an error, never a new empty one.
        let typo = o.share.with_file_name("hosue.bimble-history");
        assert!(Repo::open_dir(&typo).is_err());
        assert!(!typo.exists());

        // Alice renames a beam and synchronizes: nothing to merge, pushed.
        let ma = rename(&m, beam, "Alice's beam");
        a.commit(&ma, "beam", ALICE, 2).unwrap();
        let (how, _) = sync(&a, &share, ALICE, 2, Prefer::Neither)
            .unwrap()
            .unwrap();
        assert_eq!(how, Merged::UpToDate);
        assert_eq!(share_main(&o), a.head_commit().unwrap().unwrap());

        // Bob renamed a column meanwhile: his sync merges Alice's beam in and pushes the merge.
        let mb = rename(&m, col, "Bob's column");
        b.commit(&mb, "column", BOB, 3).unwrap();
        let (how, merged) = sync(&b, &share, BOB, 3, Prefer::Neither).unwrap().unwrap();
        assert!(matches!(how, Merged::Commit(_)), "{how:?}");
        assert_eq!(merged.elements[&beam].name, "Alice's beam");
        assert_eq!(merged.elements[&col].name, "Bob's column");
        let tip = b.head_commit().unwrap().unwrap();
        assert_eq!(share_main(&o), tip);
        assert_eq!(b.commit_info(&tip).unwrap().message, "Synchronize main");

        // Alice synchronizes: a fast-forward to the same model.
        let (how, back) = sync(&a, &share, ALICE, 4, Prefer::Neither)
            .unwrap()
            .unwrap();
        assert_eq!(how, Merged::FastForward(tip));
        assert_eq!(back, merged);
    }

    #[test]
    fn a_push_that_is_not_a_fast_forward_is_refused() {
        let (o, m) = office("nonff");
        let (a, b) = (Repo::open(&o.alice).unwrap(), Repo::open(&o.bob).unwrap());
        let share = Repo::open_dir(&o.share).unwrap();
        let beam = id_of(&m, IfcClass::IfcBeam);
        a.commit(&rename(&m, beam, "a"), "a", ALICE, 2).unwrap();
        let alices = push(&a, &share, "main", ALICE, 2).unwrap();
        b.commit(&rename(&m, beam, "b"), "b", BOB, 3).unwrap();
        // Refused, even once Bob has fetched Alice's commit, and the remote keeps Alice's work.
        assert!(matches!(
            push(&b, &share, "main", BOB, 3),
            Err(RepoError::NotFastForward(_))
        ));
        fetch(&b, &share, "main").unwrap();
        assert!(matches!(
            push(&b, &share, "main", BOB, 3),
            Err(RepoError::NotFastForward(_))
        ));
        assert_eq!(share_main(&o), alices);

        // A lock left behind (someone synchronizing, or a crash) stops a push and names its holder.
        a.commit(&rename(&m, beam, "a2"), "a2", ALICE, 4).unwrap();
        std::fs::write(share.dir().join("lock"), "carol@example.org 5\n").unwrap();
        let e = push(&a, &share, "main", ALICE, 5).unwrap_err().to_string();
        assert!(e.contains("carol@example.org"), "{e}");
        assert_eq!(share_main(&o), alices);
        std::fs::remove_file(share.dir().join("lock")).unwrap();
        let tip = push(&a, &share, "main", ALICE, 6).unwrap();
        assert_eq!(share_main(&o), tip);
        assert!(!share.dir().join("lock").exists());
    }

    #[test]
    fn a_conflict_stops_the_sync_and_pushes_nothing() {
        let (o, m) = office("conflict");
        let (a, b) = (Repo::open(&o.alice).unwrap(), Repo::open(&o.bob).unwrap());
        let share = Repo::open_dir(&o.share).unwrap();
        let col = id_of(&m, IfcClass::IfcColumn);
        a.commit(&rename(&m, col, "C-alice"), "a", ALICE, 2)
            .unwrap();
        sync(&a, &share, ALICE, 2, Prefer::Neither)
            .unwrap()
            .unwrap();
        let alices = share_main(&o);
        let bobs = b.commit(&rename(&m, col, "C-bob"), "b", BOB, 3).unwrap();
        let conflicts = sync(&b, &share, BOB, 3, Prefer::Neither)
            .unwrap()
            .unwrap_err();
        assert_eq!(conflicts.len(), 1, "{conflicts:?}");
        assert_eq!(conflicts[0].id, Some(col));
        assert_eq!(share_main(&o), alices);
        assert_eq!(b.head_commit().unwrap(), Some(bobs));
        // Keeping theirs resolves it: the merge is pushed with Alice's name on the column.
        let (how, merged) = sync(&b, &share, BOB, 4, Prefer::Theirs).unwrap().unwrap();
        assert!(matches!(how, Merged::Commit(_)), "{how:?}");
        assert_eq!(merged.elements[&col].name, "C-alice");
        assert_eq!(share_main(&o), b.head_commit().unwrap().unwrap());
    }

    #[test]
    fn borrowed_elements_belong_to_their_owner_until_they_synchronize() {
        let (o, m) = office("borrow");
        let (a, b) = (Repo::open(&o.alice).unwrap(), Repo::open(&o.bob).unwrap());
        let share = Repo::open_dir(&o.share).unwrap();
        let (beam, col) = (id_of(&m, IfcClass::IfcBeam), id_of(&m, IfcClass::IfcColumn));

        assert!(borrow(&share, ALICE, &[beam], 2).unwrap().is_empty());
        // Bob selects the beam and the column: the beam is Alice's, so he gets neither.
        assert_eq!(
            borrow(&share, BOB, &[col, beam], 3).unwrap(),
            vec![(beam, ALICE.to_owned())]
        );
        assert!(!borrowed(&share).unwrap().contains_key(&col));
        assert!(borrow(&share, BOB, &[col], 3).unwrap().is_empty());

        // Bob changed the beam offline anyway: his sync stops and pushes nothing.
        b.commit(&rename(&m, beam, "Bob's beam"), "b", BOB, 4)
            .unwrap();
        let before = share_main(&o);
        let c = sync(&b, &share, BOB, 4, Prefer::Neither)
            .unwrap()
            .unwrap_err();
        assert_eq!(c.len(), 1);
        assert_eq!((c[0].kind, c[0].id), ("borrowed", Some(beam)));
        assert!(c[0].name.contains(ALICE), "{}", c[0]);
        assert_eq!(share_main(&o), before);
        // Preferring his side does not override a borrow.
        let c = sync(&b, &share, BOB, 4, Prefer::Ours).unwrap().unwrap_err();
        assert_eq!(c[0].kind, "borrowed");
        assert_eq!(share_main(&o), before);
        // His own borrowed column is not in the way.
        assert_eq!(
            borrowed(&share).unwrap().get(&col).map(|s| s.as_str()),
            Some(BOB)
        );

        // Alice synchronizes (nothing changed): her borrows are given back, Bob's stay.
        sync(&a, &share, ALICE, 5, Prefer::Neither)
            .unwrap()
            .unwrap();
        let left = borrowed(&share).unwrap();
        assert_eq!(
            left.into_iter().collect::<Vec<_>>(),
            vec![(col, BOB.to_owned())]
        );
        // Now Bob's sync goes through, and gives back his column.
        sync(&b, &share, BOB, 6, Prefer::Neither).unwrap().unwrap();
        assert!(borrowed(&share).unwrap().is_empty());
        assert_eq!(
            Repo::open_dir(&o.share)
                .unwrap()
                .model_at("main")
                .unwrap()
                .elements[&beam]
                .name,
            "Bob's beam"
        );
    }

    #[test]
    fn a_damaged_remote_object_is_refused_and_not_copied() {
        let (o, m) = office("damaged");
        let a = Repo::open(&o.alice).unwrap();
        let share = Repo::open_dir(&o.share).unwrap();
        let beam = id_of(&m, IfcClass::IfcBeam);
        let mut ma = m.clone();
        ma = rename(&ma, beam, "changed");
        let c = a.commit(&ma, "x", ALICE, 2).unwrap();
        push(&a, &share, "main", ALICE, 2).unwrap();
        let obj = a.tree(&a.commit_info(&c).unwrap().tree).unwrap().elements[&beam].clone();
        let p = share.dir().join("objects").join(&obj[..2]).join(&obj[2..]);
        let mut bytes = std::fs::read(&p).unwrap();
        bytes[5] ^= 1;
        std::fs::write(&p, bytes).unwrap();

        let b = Repo::open(&o.bob).unwrap();
        let e = fetch(&b, &share, "main").unwrap_err();
        assert!(matches!(e, RepoError::Damaged(..)), "{e}");
        assert!(!b.has(&obj));
        assert!(!b.has(&c), "the commit must not arrive without its objects");
    }
}
