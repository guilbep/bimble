# ADR 0004 — Versioning: stable ids, canonical form, element hashes, semantic diff

- Status: Accepted (5 Nov 2026); the store is built (ADR 0017, 6 Nov 2026)
- Decided by: Pierre ("we need GitHub-like features but for architects, so we should anticipate that");
  the mechanism below was proposed and implemented by Claude
- Reversal cost: medium (hash format changes invalidate stored hashes)

## Decision
1. **Stable identity.** Every object has an IFC GlobalId (22 characters, IFC base64). It never changes on
   edit. The encoding is checked against `ifcopenshell.guid.compress`.
2. **Canonical serialization.** Maps are `BTreeMap`s keyed by GlobalId; struct fields have a fixed order;
   floats are written in shortest round-trip form and read back exactly (`serde_json` `float_roundtrip`);
   `-0.0` is canonicalised to `0.0`.
3. **Hashes.** `content_hash` = BLAKE3 of an object's canonical JSON. `model_hash` = hash over sorted
   `(GlobalId, element hash)` pairs per collection. Same model → same hash on any machine.
4. **Semantic diff.** Elements are matched by GlobalId, never by position. Changes are reported field by field
   (`IfcBeam B1.y10: generator.height 500 → 600`), also as JSON for tools and agents.
5. **Next:** a content-addressed object store (one object per element), commits (parent, author, message,
   model hash), branches, merges with element-level conflicts, a review UI.

## Why not plain Git on files
Line-based merges don't understand building elements: two people moving the same beam, or one deleting a
column another resized, are element conflicts, not text conflicts. Git can still carry the object store.

## Evidence and lessons
- Review (5 Nov) found two ways the "same model → same hash" promise broke: ~9 % of random coordinates came
  back one ulp off after save/load (default serde_json parsing), and `-0.0` vs `0.0`. Both fixed, both
  with regression tests. **Any new float field or serializer must keep these tests passing.**

## Revisit when
- Building the object store: decide chunking (one object per element vs per storey) and whether the
  hash should cover derived data (it should not).
