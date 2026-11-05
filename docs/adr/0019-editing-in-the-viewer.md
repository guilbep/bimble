# ADR 0019 — Editing in the viewer: Commands, rebuild, history

- Status: Accepted (6 Nov 2026)
- Decided by: Pierre (I2: edit and version models); implementation by Claude
- Reversal cost: low

## Decision
- Only `.bimble` models are editable (IFC files are views; edits would need writing IFC back).
- Every change made in the viewer is a group of `Command`s applied with `History` (ADR 0005): one undo
  step per action, validated first. Properties are edited in a form copy and applied when the field is
  left (not on every frame of a drag), so a drag is one undo step.
- After a change the scene is rebuilt from the model (`Scene::from_model`, parallel) and re-uploaded;
  the selection follows its GlobalId. Placing works on the level of the chosen storey: columns and
  walls stand on it and are as high as the storey; beams and slabs hang from the level above.
- Save writes the file atomically; History commits save first so the file and the history agree.

## Revisit when
- Large .bimble models: rebuild only the changed elements' meshes (the scene is per element already).
- Snapping placement to axes, columns and other elements; rotating; multi-selection.
