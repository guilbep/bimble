# ADR 0038 — Collaborate like git: a shared history, synchronize = pull, merge, push

- Status: Proposed (9 Nov 2026) — the office answered (#51); conflicts and further rights open
- Decided by: — (asked by the office in #51 item 5; Pierre: "let's be inspired by git")
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
5. Still open: conflicts (keep mine / keep theirs, or both side by side in 3D first?), and
   whether more rights than "view" and "model" are needed.

## Revisit when
The office answers the questions; the first two-person test on a shared folder.
