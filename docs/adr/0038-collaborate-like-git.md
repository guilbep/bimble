# ADR 0038 — Collaborate like git: a shared history, synchronize = pull, merge, push

- Status: Proposed (9 Nov 2026) — to decide with Pierre and the office
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

## Questions for the office
1. Is a shared folder enough to start, with rights advisory only, or is a server needed from day one?
2. Layers: do the office's Revit worksets map one-to-one (structure, façade, interior…)?
3. Should a borrowed layer be strict (others' edits refused), or a warning only?

## Revisit when
The office answers the questions; the first two-person test on a shared folder.
