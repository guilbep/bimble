# ADR 0055 — Team members: a list in project information, one of them "my company"

- Status: Proposed
- Decided by: not yet: proposed from the wireframes v01, for Pierre
- Reversal cost: low (keys in the project map)
- Extends: ADR 0030 (project information), ADR 0024 (title blocks)

## Context / Problem

Project information (ADR 0030) offers fixed groups of keys: Client, Architect, Engineer,
Contractor, each with address, e-mail and phone. A real project has more members (geologist,
building physics, surveyor) and some roles twice. Title blocks need the author's own office,
whatever its role. The wireframe A4 shows a "Project team" table and a "Team member" dialog.

## Decision

1. **Data stays in `Model.project`** (the ADR 0030 map). A team member is a set of keys with a
   prefix: `member.<n>.role`, `.company`, `.contact`, `.address`, `.postcode`, `.city`,
   `.phone`, `.email`, with `<n>` a stable number never reused. The map keeps its canonical
   order and the file needs no new field.
2. **Roles** are text, offered from a list (Client, Architect, Structural engineer, Geologist,
   Contractor…). Two members can share a role.
3. **"My company"**: one key `my_company = <n>`. That member fills the drawing's author and the
   office's title block.
4. **Title blocks** keep their fields. `{engineer}`, `{architect}`, `{client}`… read the first
   member with that role. The existing keys (`engineer`, `engineer_email`…) are migrated once to
   members on open, and the old keys stay readable for templates that use them.
5. **UI**: Project → Project information shows the "Project team" table (Role, Company,
   Contact) with "Add a team member". A row opens the "Team member" dialog. Fields apply when
   the user leaves them (ADR 0052). The wireframe's "Cancel" and "Save team member" buttons
   conflict with ADR 0052: this ADR drops them and keeps × and Esc.

## API needed

- Commands: none new. `SetProject { project }` (`command.rs:54`) carries every change.
- Verbs: `member_add {role, company?, contact?, …}` → `{n}`,
  `member_set {n, field, value}`, `member_remove {n}`, `member_mine {n}`. Each verb builds one
  `SetProject`.
- Queries: `project` (exists) gains `members: [{n, role, company, contact, address, postcode,
  city, phone, email, mine}]`.
- IFC: `IfcActor` with `IfcOrganization` and `IfcPostalAddress` / `IfcTelecomAddress`, assigned
  to the project by `IfcRelAssignsToActor` with an `IfcActorRole` (`ARCHITECT`,
  `STRUCTURALENGINEER`, `CLIENT`, `CONTRACTOR`, `USERDEFINED` for a geologist). IFC4 has this
  standard place, so no superset is needed. "My company" is the `OwningUser` organization of
  `IfcOwnerHistory`.

## Alternatives considered

- A new `Model.team` struct list: cleaner types, but a second place beside the project map that
  title blocks must read, and a file format change.
- Keep the fixed groups of ADR 0030: no second engineer, no geologist.
- A shared address book across projects: useful later, out of scope for a model file.

## Evidence

- `docs/design/wireframes-v01/en/TeamMember.html`: table Role / Company / Contact (Client,
  Architect, Structural engineer, Geologist, Contractor), dialog fields, "“My company” fills
  the drawings' author and the office's title block".
- `crates/bimble-core/src/project.rs:15-40`: the fixed keys by group today.
- `crates/bimble-core/src/model.rs:559`: `Model.project` map.
- `crates/bimble-api/src/query.rs:192`: the `project` query.

## Questions for Pierre

- Keep the dialog's Save button (one change for the whole member) or apply per field (ADR 0052)?
- Does "my company" belong to the model or to the user's settings (shared by every project)?

## Revisit when

An office asks for one address book for all its projects.
