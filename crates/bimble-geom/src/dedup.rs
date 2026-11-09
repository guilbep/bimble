//! Copies the file didn't mark as copies (ADR 0036 step 3): elements whose meshes are the same
//! up to a linear map and a translation become instances of one shared shape (ADR 0034), like
//! IfcOpenShell's `MeshDedup` (which allows rotations only). Here copies may also be stretched:
//! a pipe of another length is the same mesh stretched along its axis, and most of an MEP
//! model's own meshes are such.
//!
//! Candidates have as many vertices, triangles and edges. A fit from four of their vertices
//! (matched by order: welding numbers them as the faces come, and copies of one family come
//! alike) must then place every vertex within `TOLERANCE_MM`. Their triangles may differ: a
//! polygon's diagonals depend on its orientation (the triangulation projects onto the plane its
//! normal points most along), so a copy takes the shape's triangulation of the same faces. A
//! mirrored copy usually winds its faces the other way, so its vertices come in another order and
//! it keeps its own mesh.
//!
//! Edge normals of a stretched copy are turned by the map, not by its inverse transpose: only
//! their side of the view matters (back edges in the section box), and stretching along an axis
//! keeps them, as for pipes.

use crate::Mesh;
use crate::scene::{Geometry, Placement32, SceneElement};
use std::collections::HashMap;
use std::hash::{Hash, Hasher};
use std::sync::Arc;

/// How far a copy's vertex may be from where the fit puts it (welding quantizes to 0.01 mm).
const TOLERANCE_MM: f64 = 0.1;
/// Distinct shapes tried per bucket of same-looking meshes (bounds the work on large buckets).
const MAX_SHAPES_PER_BUCKET: usize = 16;

type V = [f64; 3];

fn sub(a: V, b: V) -> V {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}
fn dot(a: V, b: V) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}
fn cross(a: V, b: V) -> V {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}
fn unit(a: V) -> Option<V> {
    let l = dot(a, a).sqrt();
    (l > 1e-6).then(|| a.map(|c| c / l))
}
fn p64(p: [f32; 3]) -> V {
    p.map(f64::from)
}

/// Vertices that span the mesh: the first, the farthest from it, the farthest from the line
/// through both, and the farthest from the plane through the three (none for a flat mesh).
fn anchors(m: &Mesh) -> Option<Anchors> {
    let p = |k: usize| p64(m.positions[k]);
    let far = |score: &dyn Fn(V) -> f64| {
        (0..m.positions.len()).max_by(|&a, &b| score(p(a)).total_cmp(&score(p(b))))
    };
    let k1 = far(&|q| dot(sub(q, p(0)), sub(q, p(0))))?;
    let u = sub(p(k1), p(0));
    let size = dot(u, u).sqrt();
    let u = unit(u)?;
    let k2 = far(&|q| {
        let c = cross(u, sub(q, p(0)));
        dot(c, c)
    })?;
    let n = unit(cross(u, sub(p(k2), p(0))))?;
    let k3 = far(&|q| dot(n, sub(q, p(0))).abs())?;
    let solid = dot(n, sub(p(k3), p(0))).abs() > 1e-3 * size;
    Some([0, k1, k2, if solid { k3 } else { usize::MAX }])
}

/// [first, second, third, fourth or `usize::MAX` for a flat mesh].
type Anchors = [usize; 4];

/// Columns of the edges from the first anchor to the others; for a flat mesh, the third is its
/// unit normal (copies of a plate keep their thickness: none).
fn edges_of(m: &Mesh, a: Anchors) -> Option<[V; 3]> {
    let p = |k: usize| p64(m.positions[k]);
    let (e1, e2) = (sub(p(a[1]), p(a[0])), sub(p(a[2]), p(a[0])));
    let e3 = if a[3] == usize::MAX {
        unit(cross(e1, e2))?
    } else {
        sub(p(a[3]), p(a[0]))
    };
    Some([e1, e2, e3])
}

/// A frame (orthonormal columns) a mesh's box is tight in: along its longest feature edge, then
/// the longest edge across it. A box or a pipe lies along it, however it is turned in the scene.
fn own_frame(m: &Mesh) -> [V; 3] {
    let identity = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]];
    let dirs = || m.edges.iter().map(|s| sub(p64(s[1]), p64(s[0])));
    let longest =
        |it: &mut dyn Iterator<Item = V>| it.max_by(|a, b| dot(*a, *a).total_cmp(&dot(*b, *b)));
    let Some(u) = longest(&mut dirs()).and_then(unit) else {
        return identity;
    };
    let across = longest(&mut dirs().filter(|d| unit(*d).is_some_and(|d| dot(d, u).abs() < 0.9)))
        .map(|d| sub(d, u.map(|c| c * dot(d, u))))
        .and_then(unit);
    let v = across.unwrap_or_else(|| {
        // Any direction across u.
        let a = if u[0].abs() < 0.9 {
            [1.0, 0.0, 0.0]
        } else {
            [0.0, 1.0, 0.0]
        };
        unit(cross(u, a)).unwrap_or([0.0, 1.0, 0.0])
    });
    [u, v, cross(u, v)]
}

fn inverse(c: [V; 3]) -> Option<[V; 3]> {
    // Rows of the inverse are the cross products of the columns, over the determinant.
    let det = dot(c[0], cross(c[1], c[2]));
    if det.abs() < 1e-12 {
        return None;
    }
    let r = [cross(c[1], c[2]), cross(c[2], c[0]), cross(c[0], c[1])].map(|v| v.map(|x| x / det));
    // Columns of the inverse from its rows.
    Some([0, 1, 2].map(|j| [r[0][j], r[1][j], r[2][j]]))
}

fn mul(a: [V; 3], b: [V; 3]) -> [V; 3] {
    b.map(|col| [0, 1, 2].map(|i| a[0][i] * col[0] + a[1][i] * col[1] + a[2][i] * col[2]))
}

/// The linear map (columns) and translation taking `from` onto `to`, if every vertex lands
/// within the tolerance: copies may be turned or stretched (pipes of other lengths).
fn fit(from: &Mesh, to: &Mesh, a: Anchors) -> Option<([V; 3], V)> {
    let r = mul(edges_of(to, a)?, inverse(edges_of(from, a)?)?);
    let lin = |v: V| [0, 1, 2].map(|i| r[0][i] * v[0] + r[1][i] * v[1] + r[2][i] * v[2]);
    let t = sub(p64(to.positions[0]), lin(p64(from.positions[0])));
    let ok = from.positions.iter().zip(&to.positions).all(|(x, y)| {
        let d = sub(lin(p64(*x)), sub(p64(*y), t));
        dot(d, d) <= TOLERANCE_MM * TOLERANCE_MM
    });
    ok.then_some((r, t))
}

fn key(m: &Mesh) -> u64 {
    let mut h = std::collections::hash_map::DefaultHasher::new();
    (m.positions.len(), m.indices.len(), m.edges.len()).hash(&mut h);
    h.finish()
}

/// Turn copies among elements with their own mesh into instances of a shared shape. Returns
/// how many elements became copies.
pub fn share_copies(elements: &mut [SceneElement]) -> usize {
    let mut buckets: HashMap<u64, Vec<usize>> = HashMap::new();
    for (i, e) in elements.iter().enumerate() {
        if let Geometry::Own(m) = &e.geometry
            && m.positions.len() >= 3
        {
            buckets.entry(key(m)).or_default().push(i);
        }
    }
    // Per shape: its first element, then (element, rotation, translation) of each copy.
    type Copy = (usize, [V; 3], V);
    let mut groups: Vec<(usize, Anchors, Vec<Copy>)> = Vec::new();
    for members in buckets.into_values().filter(|m| m.len() > 1) {
        let mut shapes: Vec<(usize, Anchors, Vec<Copy>)> = Vec::new();
        for i in members {
            let Geometry::Own(m) = &elements[i].geometry else {
                continue;
            };
            let found = shapes.iter_mut().find_map(|(s, a, copies)| {
                let Geometry::Own(sm) = &elements[*s].geometry else {
                    return None;
                };
                fit(sm, m, *a).map(|(r, t)| copies.push((i, r, t)))
            });
            if found.is_none()
                && shapes.len() < MAX_SHAPES_PER_BUCKET
                && let Some(a) = anchors(m)
            {
                shapes.push((i, a, Vec::new()));
            }
        }
        groups.extend(shapes.into_iter().filter(|(_, _, c)| !c.is_empty()));
    }
    let mut n = 0;
    for (first, _, copies) in groups {
        let Geometry::Own(m) = &elements[first].geometry else {
            continue;
        };
        // The shape in its own frame, around its centre: its box is tight however the
        // element lies, and so are its copies' placed boxes (their bounds and spheres).
        let f = own_frame(m);
        let to_frame = |v: V| [dot(f[0], v), dot(f[1], v), dot(f[2], v)];
        let (lo, hi) = m.positions.iter().map(|p| to_frame(p64(*p))).fold(
            ([f64::INFINITY; 3], [f64::NEG_INFINITY; 3]),
            |(l, h), p| {
                (
                    [0, 1, 2].map(|k| l[k].min(p[k])),
                    [0, 1, 2].map(|k| h[k].max(p[k])),
                )
            },
        );
        let centre: V = [0, 1, 2].map(|k| (lo[k] + hi[k]) / 2.0);
        let half: V = [0, 1, 2].map(|k| (hi[k] - lo[k]) / 2.0);
        let local = |p: [f32; 3]| sub(to_frame(p64(p)), centre).map(|x| x as f32);
        // p = M (F (s + centre)) + t = (M F) s + (M F centre + t)
        let place = |r: [V; 3], t: V| {
            let mf = mul(r, f);
            let lin = |v: V| [0, 1, 2].map(|i| mf[0][i] * v[0] + mf[1][i] * v[1] + mf[2][i] * v[2]);
            let tc = lin(centre);
            // The placed box's longest half-diagonal.
            let reach = [(1.0, 1.0), (1.0, -1.0), (-1.0, 1.0), (-1.0, -1.0)]
                .map(|(sy, sz)| {
                    let d = lin([half[0], sy * half[1], sz * half[2]]);
                    dot(d, d).sqrt()
                })
                .into_iter()
                .fold(0.0, f64::max);
            let pl = Placement32 {
                x: mf[0].map(|x| x as f32),
                y: mf[1].map(|x| x as f32),
                z: mf[2].map(|x| x as f32),
                t: [0, 1, 2].map(|i| (tc[i] + t[i]) as f32),
            };
            (pl, reach)
        };
        // A copy whose placed box is much larger than itself (a sheared fit) keeps its own
        // mesh: loose bounds would frame the model too wide and cull it poorly. Turned copies
        // reach up to √3 of their own box; on the 415 MB model 1 % went beyond 2 (up to 6).
        let tight = |i: usize, reach: f64| {
            let Geometry::Own(c) = &elements[i].geometry else {
                return false;
            };
            let b = crate::bvh::Aabb::of_points(&c.positions);
            let d = sub(p64(b.max), p64(b.min));
            reach <= 2.0 * dot(d, d).sqrt() / 2.0 + 1.0
        };
        let identity = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]];
        let placed: Vec<(usize, Placement32)> = copies
            .into_iter()
            .map(|(i, r, t)| (i, place(r, t)))
            .filter(|(i, (_, reach))| tight(*i, *reach))
            .map(|(i, (pl, _))| (i, pl))
            .collect();
        if placed.is_empty() {
            continue;
        }
        let shape = Arc::new(Mesh {
            positions: m.positions.iter().map(|p| local(*p)).collect(),
            indices: m.indices.clone(),
            edges: m.edges.iter().map(|s| [local(s[0]), local(s[1])]).collect(),
            edge_normals: m
                .edge_normals
                .iter()
                .map(|ns| {
                    ns.map(|n| {
                        if n == [0; 4] {
                            return n;
                        }
                        let v = to_frame([n[0], n[1], n[2]].map(|c| f64::from(c) / 127.0));
                        crate::pack_normal(v.map(|x| x as f32))
                    })
                })
                .collect(),
        });
        let first_at = place(identity, [0.0; 3]).0;
        elements[first].geometry = Geometry::Instance {
            shape: shape.clone(),
            to_scene: first_at,
        };
        for (i, pl) in placed {
            elements[i].geometry = Geometry::Instance {
                shape: shape.clone(),
                to_scene: pl,
            };
            n += 1;
        }
    }
    n
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scene::{Scene, SceneBuilder};

    /// Copies turned, moved or stretched become one shape; one with a corner moved doesn't, nor
    /// (here) a mirrored one, whose vertices come in another order. Each element's triangles
    /// stay where they were.
    #[test]
    fn rotated_copies_share_one_shape() {
        let scene = Scene::from_model(&bimble_core::demo::concrete_frame(
            &bimble_core::demo::FrameSpec::default(),
        ));
        let before: Vec<Mesh> = scene
            .elements
            .iter()
            .map(|e| e.world_mesh().into_owned())
            .collect();
        // Scenes share copies when built: undo it to start from own meshes.
        let mut elements: Vec<SceneElement> = scene
            .elements
            .iter()
            .zip(&before)
            .map(|(e, m)| SceneElement {
                geometry: Geometry::Own(m.clone()),
                ..e.clone()
            })
            .collect();
        let n = share_copies(&mut elements);
        assert!(n > 0, "a generated frame repeats its columns and beams");
        let mut shapes = std::collections::HashSet::new();
        for (e, b) in elements.iter().zip(&before) {
            let a = e.world_mesh();
            assert_eq!(a.indices.len(), b.indices.len());
            for (p, q) in a.positions.iter().zip(&b.positions) {
                let d = sub(p64(*p), p64(*q));
                assert!(dot(d, d).sqrt() < 0.2, "{p:?} vs {q:?}");
            }
            if let Geometry::Instance { shape, .. } = &e.geometry {
                shapes.insert(Arc::as_ptr(shape));
            }
        }
        assert!(shapes.len() < n, "{} shapes for {n} copies", shapes.len());

        // A box turned 90° about z and moved, stretched: one shape. Mirrored, or a corner moved
        // 50 mm: not.
        let boxed = |t: &dyn Fn([f64; 3]) -> [f64; 3], bent: bool| {
            let mut mb = crate::scene::MeshBuilder::default();
            let c = |x: f64, y: f64, z: f64| t([x, y, z]);
            let (a, b, h) = (300.0, 500.0, 3000.0);
            let q = [c(0., 0., 0.), c(a, 0., 0.), c(a, b, 0.), c(0., b, 0.)];
            let r = [c(0., 0., h), c(a, 0., h), c(a, b, h), c(0., b, h)];
            let r = if bent {
                [r[0], r[1], c(a + 50.0, b, h), r[3]]
            } else {
                r
            };
            mb.add_polygon(&[q[0], q[3], q[2], q[1]], &[]);
            mb.add_polygon(&r, &[]);
            for k in 0..4 {
                let j = (k + 1) % 4;
                mb.add_polygon(&[q[k], q[j], r[j], r[k]], &[]);
            }
            mb
        };
        let mut b = SceneBuilder::default();
        let info = scene.elements[0].clone();
        let el = |n: &str| crate::scene::ElementInfo {
            global_id: n.into(),
            class: info.class.clone(),
            name: n.into(),
            storey: info.storey.clone(),
            material: info.material.clone(),
            category: info.category,
        };
        b.push(el("a"), boxed(&|p| p, false));
        b.push(
            el("turned"),
            boxed(&|[x, y, z]| [5000.0 - y, 2000.0 + x, z], false),
        );
        b.push(
            el("mirrored"),
            boxed(&|[x, y, z]| [-x - 9000.0, y, z], false),
        );
        b.push(
            el("longer"),
            boxed(&|[x, y, z]| [x, y, z * 1.5 + 9000.0], false),
        );
        b.push(el("bent"), boxed(&|[x, y, z]| [x + 20000.0, y, z], true));
        let s = b.finish();
        let shared: Vec<bool> = s
            .elements
            .iter()
            .map(|e| matches!(e.geometry, Geometry::Instance { .. }))
            .collect();
        assert_eq!(shared, [true, true, false, true, false]);
    }
}
