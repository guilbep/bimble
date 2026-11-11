# ADR 0038 — Collaborate like git: a shared history, synchronize = pull, merge, push

- Status: Accepted (9 Nov 2026) as amended by the office's answers, with conflicts (Q4) and rights (Q5)
  below. Built so far: shared-folder remote, clone, fetch, push (fast-forward only, locked),
  synchronize (with `--prefer ours|theirs` for the whole sync), element borrowing (explicit,
  `bimble history borrow`). In the viewer (10 Nov, #55): Collaborate → Shared folder… and
  Synchronize (saves and commits first), borrowing on an element's first change (a new element
  too), "<element> belongs to <who>" with Synchronize or Cancel, a chip after 4 h without
  synchronizing, who you are in Settings → You. Not built yet: conflict resolution per element,
  the `people` object and its rights, layers. Still proposals: the HTTP server remote, a chat.
- Amended (10 Nov, Cécile on #51): the rights are three levels, view only / view and annotate /
  model and annotate, each person setting their own for now; later maybe one admin per project
  who sets the others' (new people at the lowest). This replaces viewer / modeller / owner below
  when the `people` object is built.
- Decided by: the office (#51 answers); Pierre ("let's be inspired by git"; Q4 and Q5: "I trust
  you on that"); Q4, Q5 and the format by Claude
- Reversal cost: high once offices share models through it (the remote format becomes a contract)

## Context / Problem
Several people of one office work on one model (#51 item 5). They want:
- a list of who works on it (e-mail, trigram from the part before the "@"), their role and what they
  may do ("view and select only", "modify");
- layers, like Revit's worksets (sub-projects);
- a "Synchronize" button that makes each person's changes visible to the others.

Bimble already keeps a git-like history next to each model (ADR 0017). It is content-addressed
(blake3), with one object per element, and has commits, branches and a merge. The merge works by
GlobalId, then field by field: a conflict only arises when both sides changed the same field
differently. What is missing is a shared place, people and rights.

## Proposal (to decide)
1. **A remote, as in git.** A model's history gets remotes. A first remote kind is a shared folder:
   the office's file server, a synced drive, or a USB key. A second kind is a small HTTP server; it
   is the only kind that can enforce rights (point 4). Objects are immutable and named by their
   hash, so copying them is safe and idempotent, and a remote only has to store files.
2. **Synchronize = fetch, merge, push.** This is the button.
   - Fetch the remote's branch, merge it into the local work (ADR 0017's field-level merge), then
     push.
   - On a conflict: two people changed the same field of the same element differently. The button
     stops and lists the conflicts, each with "keep mine" and "keep theirs". Nothing is pushed until
     they are resolved.
   - A push that is not a fast-forward is refused, and the button fetches again. This is git's
     rule, so no one's work is overwritten.
3. **People.** `people` is an object in the history:
   - each person has an e-mail, a trigram (default: the part before "@", upper-cased, editable when
     two collide), a role (free text: architect, engineer, drafter…) and a right (`view` or
     `modify`);
   - commits name their author by e-mail;
   - the Collaborate tab shows the list.
4. **Rights are enforced where they can be.**
   - On a shared folder, rights are advisory: the viewer refuses to push for a `view` person, but
     anyone with write access to the folder can bypass that, and the UI must say so.
   - Real enforcement needs the server: it checks the pusher's identity and right before accepting a
     ref update.
   - Rights per layer come later.
5. **Layers (worksets).**
   - Each element belongs to one layer. A layer is a named group, stored in the model and versioned
     like the rest.
   - Layers can be shown or hidden.
   - A layer can be *borrowed* by one person: a lock object on the remote, pushed like a ref. Others
     can still view it, but their edits to it are refused at sync.
   - This is what Revit's worksets do. It turns frequent conflicts into "ask the owner".
6. **Offline first.** Everyone works on their own copy and history. Synchronize is explicit, like
   Revit's "Synchronize with Central" and like `git pull && git push`. There is no live
   co-editing.

## Alternatives considered
- **Git itself on the .bimble file:** line-based merges don't understand elements (ADR 0017). Git
  could still carry Bimble's objects, which is an option for the remote layer.
- **A central database (BIMserver-like):** live, but needs a server from day one and doesn't work
  offline.
- **Real-time co-editing (CRDT, Figma-like):** smoothest to use, but the most work. It is also
  unclear for model validation: a merged model must still be valid.
- **Speckle-style streams of objects:** close in spirit (content-addressed objects, commits, a
  server). We could use it as a remote kind instead of our own server.

## The office's answers (#51, 9 Nov) and what they change

1. **Where:** the central model sits on the office server, like Revit's central file; everyone
   with access to the server may open it. A cloud service maybe later. So the first remote kind
   is the shared folder (point 1), and that is enough.
2. **Rights are a safety net, not a lock:** each person sets their own right, "view" or
   "model". The engineer who knows the tool less opens in "view" so as not to break anything.
   Point 4 changes: no enforcement is needed; the right is a per-person mode that stops
   accidental edits.
3. **Layers belong to no one:** they filter the model (other than by material or class), and
   their list is editable. Starting list: links; levels and grid lines; topography; excavations;
   masonry and concrete structure; timber structure; timber assembly; steel structure; steel
   assembly; MEP. Point 5 changes: no borrowing of layers.
4. **Elements are borrowed, not layers:** an element someone changed or created is theirs until
   they synchronize. Selecting it to change it shows "this element belongs to <name>", with
   "synchronize" or "cancel". Ownership is written with the push and released by the next sync.
   A reminder to synchronize twice a day (as Revit does). A chat inside Bimble was asked about:
   open.
   How we read "written with the push and released by the next sync": a colleague must see "this
   element belongs to <name>" *before* its owner synchronizes, so a borrow is written to the
   remote when the element is first changed (if the server is reachable; offline borrows go up at
   the next sync), and all of a person's borrows are released when their sync succeeds. This is
   what Revit does. The office should confirm this reading.
5. Still open: conflicts (keep mine / keep theirs, or both side by side in 3D first?), and
   whether more rights than "view" and "model" are needed. Both decided below.

## Conflicts (Q4): decided
Short answer to the office: keep mine / keep theirs, per element, from a list. Each row selects and
frames its element in 3D. Showing both side by side in 3D comes later, if the list proves not to be
enough.

Borrowing prevents most conflicts. Some remain: one person changed an element offline, or before
the other's borrow reached the server. Ten ways to handle them. Scores run from 1 to 5, higher is
better. "Effort" is 5 for the least work. "Safety" means no silent loss.

| # | Proposition | Borrowing fit | Effort | Safety | Offline | Revit users | Total |
|---|---|---|---|---|---|---|---|
| 1 | List of conflicting *fields*, keep mine / keep theirs per field | 4 | 4 | 5 | 5 | 3 | 21 |
| 2 | **List grouped by *element*, keep mine / keep theirs per element (fields and both values shown); "all mine" / "all theirs" shortcuts; a row frames its element in 3D** | 5 | 5 | 5 | 5 | 4 | **24** |
| 3 | One choice for the whole sync: all mine or all theirs (today's `--prefer`) | 3 | 5 | 2 | 5 | 2 | 17 |
| 4 | Side-by-side 3D: theirs as a ghost over mine, pick in the view | 4 | 1 | 5 | 5 | 3 | 18 |
| 5 | Last writer wins, overwritten values listed in a log | 2 | 5 | 2 | 5 | 2 | 16 |
| 6 | Central wins: my conflicting change is set aside on a branch `conflicts/<me>` and reported | 4 | 4 | 4 | 5 | 4 | 21 |
| 7 | Ask the other person: the conflict waits until they decide (message or chat) | 3 | 2 | 5 | 2 | 3 | 15 |
| 8 | Keep both: my version becomes a copy with a new GlobalId | 1 | 4 | 3 | 5 | 1 | 14 |
| 9 | Borrow before any edit, online only: no conflicts can arise | 5 | 3 | 5 | 1 | 4 | 18 |
| 10 | Replay my Commands on top of theirs (a rebase); a Command that no longer validates is a conflict | 3 | 2 | 4 | 5 | 2 | 16 |

**Choice: 2.**
- Synchronize stops on a conflict and pushes nothing. Fields that only one side changed still
  merge by themselves (ADR 0017).
- The list has one row per element: its class and name, the person on the other side, and each
  conflicting field with both values. The choice is "keep mine" or "keep theirs" for the whole
  element, which is how Revit users think. Borrowing is per element too.
- Elements borrowed by someone else stop the sync the same way. Nothing of mine is pushed onto
  someone else's borrowed element. "Keep theirs" drops my change; otherwise I wait for their sync.
- Nothing is lost silently: the merge commit keeps both sides as parents, so the discarded value
  stays in the history and can be brought back.
- Proposition 4 (3D ghost) is the upgrade if the list is not enough. Proposition 6 is the fallback
  if people find stopping a sync too heavy.

## Rights (Q5): decided
The office asked for a self-chosen "view" or "model" right as a safety net. That stays, as a
**personal mode**. It is a local setting, not stored in the model. It stops my own accidental edits,
and anyone may switch it at any time (a viewer can only stay in view mode). Rights are separate:
they say what a person may do to the shared model.

- **Three roles**, enough for 5–10 people:

  | Role | View, select, measure, print, fetch | Edit, borrow, push | Manage people, roles, layers list |
  |---|---|---|---|
  | `viewer` | yes | no | no |
  | `modeller` | yes | yes, optionally only on some layers | no |
  | `owner` | yes | yes | yes |

- **Where they are stored.** A `people` object in the history, versioned like the rest (a tree
  field next to the project information). It maps each e-mail to a trigram, a job (free text:
  architect, engineer, drafter), a role, and an optional `layers` list for a modeller (absent =
  all layers). The rules:
  - there is always at least one owner;
  - the first person to push to an empty remote becomes its owner;
  - the people object merges key by key, like project information.
- **What is checked.** At push, against the remote's people object:
  - the pusher must be a modeller or an owner;
  - a change to the people object (or to the layers list) must come from an owner;
  - a modeller restricted to some layers may only change elements on those layers.

  A refused push is a stop with a message, like a conflict.
- **Where it is enforced.**
  - On a shared folder, the check is advisory. Bimble does it, but identity is the self-declared
    e-mail (`BIMBLE_AUTHOR`), and anyone with write access to the folder can bypass it. The
    Collaborate tab says so.
  - Real enforcement needs the HTTP server remote: it authenticates the pusher, then runs the same
    check before it accepts a ref update. Same rules, one more place to run them.
- **Not done:** per-element rights, groups, and rights that expire. Add them when an office asks.

## Revisit when
- The office confirms (or corrects) how borrowing reads, and the first two-person test on their
  server.
- A list of conflicts proves too abstract (then build the 3D ghost), or a sync stopped by a
  conflict is too heavy (then "central wins, mine set aside").
- An office needs real enforcement of rights: build the server remote.
