# ADR 0061 — Users and rights: a people list in the shared history, three rights, advisory for now

- Status: Proposed
- Decided by: not yet: proposed from the wireframes v01, for Pierre
- Reversal cost: medium (the `people` object becomes part of the remote format)
- Extends: ADR 0038 (rights Q5 and the 10 Nov amendment), ADR 0044 (who you are in Settings until a
  server)

## Context / Problem

ADR 0038 named viewer / modeller / owner, then the amendment of 10 Nov made it three levels: view
only, view and annotate, model and annotate. Each person sets their own right for now. The
`people` object is not built. The wireframe C4 shows a Users panel: a list, an "Add user" button
and three rights View, Edit, Admin. A real identity (log in, proof of who you are) needs a server.
This ADR says what the screen needs first, without a server.

## Decision

- A `people` list lives in the shared history next to the model, one row per person: trigram (the
  key, "PGU"), name, e-mail, role (free text), right, usual sub-projects. Last sync, borrowed count
  and commits are computed from the history, not stored.
- Three rights: **View** opens and looks, cannot synchronize changes. **Edit** edits, borrows and
  synchronizes. **Admin** is Edit, and manages people, rights and sub-projects. This maps the
  amendment's levels, with "view and annotate" left out until annotation is shared. Pierre
  confirms or keeps four levels.
- A new user starts at View. Only an Admin changes someone's right. The first person who shares
  the folder is Admin.
- Who you are stays in Settings → You (ADR 0044). The trigram there picks your row.
- **Advisory on a shared folder.** Bimble follows the rights (refuses to push for View, hides the
  Users editing tools for non-Admins), but the folder cannot enforce them. The panel says so
  in words. A server enforces them later.
- First step, without a server: the list, the three rights, Add user, the computed columns. Log
  in, invitations by e-mail and enforced rights wait for the server remote (ADR 0038 proposal).

## Alternatives considered

- Wait for a server: no people list for months, and borrowing already shows trigrams.
- Each person sets their own right (amendment of 10 Nov): simpler, but anyone becomes Admin. Kept
  as the fallback if an office has no admin.
- The operating system user name as identity: differs per machine, says nothing on a shared folder.

## Evidence

- `docs/design/wireframes-v01/en/Users.html`: rows PGU Admin, AMA Edit, LBO Admin, JFA View (an
  external e-mail), columns Right, Usual sub-projects, Last sync, the rights text, "On a shared
  folder, rights are advisory: Bimble follows them, but the folder can't enforce them. A server
  will." and "A new user starts at View".
- `docs/adr/0038-collaborate-like-git.md:9-13`: the `people` object and its rights are not built.
  three levels replace viewer / modeller / owner.
- `docs/adr/0038-collaborate-like-git.md:51`: the viewer already refuses to push for `view`.

## API needed

- History store (not the model): `people.json` (canonical, sorted by trigram) in the shared folder,
  written with a push. Changes go through history commands, not model Commands.
- Verbs: `user_add` `{"trigram":"AMA","name":"Ana M.","email":"…","role":"…","right":"view"}`,
  `user_set` `{"trigram":"AMA","right":"edit","subprojects":["Concrete"]}`, `user_remove`
  `{"trigram":"JFA"}` (asks once, ADR 0052). Refused unless the caller is Admin.
- Query `users` `{}` → rows with computed `last_sync`, `borrowed`, `commits`. Query `state` gains
  `right`.
- IFC: `IfcPerson` and `IfcPersonAndOrganization` exist (owner history, ADR 0047) but IFC has no
  place for rights. Rights stay in Bimble's superset (ADR 0003). The export writes people only.

## Revisit when

The server remote is built, or an office needs a fourth right ("view and annotate").
