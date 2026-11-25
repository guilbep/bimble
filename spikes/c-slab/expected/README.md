Case: one IfcSlab, outline (0,0) (6000,0) (6000,4000) (0,4000) CCW, thickness 220 mm, placement at (1000, 2000, 3000) with default axes, storey elevation 0.
Format: `origin` = 3 f64 as 16-hex-digit `to_bits` (scene only); `v` = 3 f32 as 8-hex-digit `to_bits`; `i` = 3 u32 per triangle; `e` = 6 f32 hex per edge; `n` = 8 i8 edge normals (scene only); all in mesh order.
`local.txt` is `bimble_geom::element_mesh`; `scene.txt` is the element's `local_mesh()` after `Scene::from_model` (relative to the scene origin).
Regenerate: `cargo run -q -p bimble-geom --example slab_dump -- local > spikes/c-slab/expected/local.txt` and the same with `scene` > `scene.txt`.
Counts: local 8 v, 12 i, 12 e; scene 1 origin (4000, 4000, 3110), 8 v, 12 i, 12 e, 12 n.
