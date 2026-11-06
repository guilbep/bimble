# ADR 0017 — History store: content-addressed objects next to the model

- Status: Accepted (6 Nov 2026); builds the store announced in ADR 0004
- Decided by: Pierre ("own format, I trust you on your decisions"); format and implementation by Claude
- Reversal cost: medium (stored histories would need a converter; the model format is unchanged)

## Problem
ADR 0004 gave stable ids, canonical JSON, element hashes and a semantic diff. Engineers need what Git
gives code: record versions with a message, go back, try a variant on a branch, bring variants together,
and see what changed — at the level of building elements, not lines of text.

## Decision
- **One history per model file**, in a folder next to it: `house.bimble` → `house.bimble-history/`
  (`format`, `HEAD`, `refs/heads/<branch>`, `objects/ab/cdef…`). Copying the two together copies the
  project with its history. No server, no Git needed.
- **Objects are canonical JSON named by their BLAKE3 hash**: one per storey, material and element
  (exactly the bytes `content_hash` hashes, so an element's object name is its content hash), one *tree*
  per model state (GlobalId → object hash, plus the project fields) and one *commit* (tree, parents,
  author, time, message). An element unchanged between commits is stored once: a commit that renames one
  beam adds three small files (element, tree, commit).
- **Reads verify**: every object is re-hashed on read and its size bounded; a mismatch is reported as a
  damaged history, never used. Branch names are checked (no `..`, no leading `.`/`-`, letters, digits,
  `-_./` only) because they are file names. Walks (log, `HEAD~n`, merge base) are bounded.
- **Merge by GlobalId, then field by field.** An object one side left as it was in the common base takes
  the other side's version (changed, added, removed). When both sides changed the same object, its JSON
  fields merge the same way (one side's height, the other's width); only a field both changed differently
  is a conflict. Conflicts stop the merge and are listed (`IfcBeam B1: generator.width`), unless the user
  says `--prefer ours|theirs`. The merged model must validate (an element on a storey the other side
  removed fails the merge). Fast-forward when HEAD is behind.
- **Interfaces**: `bimble history init|commit|status|log|diff|branch|switch|merge` and the viewer's
  History window (start, commit, branches, switch, merge, log). `switch` refuses to drop changes not
  committed unless `--force`.

## Alternatives considered
- **Git on the .bimble file**: line merges don't understand elements; whole-file blobs per version waste
  space on large models; and engineers would need Git. Git can still carry the folder.
- **One file holding all versions** (SQLite or an append-only log): harder to inspect and to sync with
  ordinary file tools; partial writes are harder to make safe than write-then-rename of small files.
- **Element-level conflicts only** (first version): two people changing different properties of one beam
  conflicted for nothing, and `--prefer` then silently dropped one change. Field-level merge fixed it
  (found by trying it on the demo: height on one branch, width on the other).

## Evidence
- Tests: commit/log/diff/model-at round trip with identical model hash; one renamed beam adds exactly 3
  objects; branches, switch, detached HEAD; a flipped byte in a tree is reported; bad branch names
  refused; merges: both sides kept, up to date, fast-forward, a real conflict, field-level merge.
- Demo frame (72 elements), 3 commits and a merge: 664 KB history folder.

## Revisit when
- Models of 50 000+ elements: one file per element costs inodes and time on some file systems; pack
  files (many objects per file, with an index) would be the next step.
- Several people at once: a shared remote (push/pull of objects and refs) and a review UI.
- Properties/psets arrive in the model: they merge field by field with no change here.
