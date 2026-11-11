//! Bimble core: IFC-typed building model, commands with undo, canonical hashing and diff.

mod command;
pub mod date;
pub mod demo;
pub mod file;
pub mod georef;
mod guid;
pub mod hatches;
pub mod library;
mod model;
pub mod pens;
pub mod project;
pub mod remote;
pub mod repo;
pub mod schedule;
pub mod sheet;
pub mod style;
pub mod version;
pub mod view;

pub use command::{Command, CommandError, History};
pub use guid::GlobalId;
pub use model::*;

#[cfg(test)]
mod tests {
    use super::*;
    use demo::{FrameSpec, concrete_frame};

    fn first(m: &Model, class: IfcClass) -> GlobalId {
        *m.elements.iter().find(|(_, e)| e.class == class).unwrap().0
    }

    #[test]
    fn demo_frame_counts_and_validates() {
        let spec = FrameSpec::default();
        let m = concrete_frame(&spec);
        m.validate().unwrap();
        assert_eq!(m.elements.len(), spec.element_count());
        let n = |c| m.elements.values().filter(|e| e.class == c).count();
        assert_eq!(n(IfcClass::IfcFooting), 12);
        assert_eq!(n(IfcClass::IfcColumn), 24);
        assert_eq!(n(IfcClass::IfcBeam), 34);
        assert_eq!(n(IfcClass::IfcSlab), 2);
    }

    #[test]
    fn file_roundtrip_is_lossless_and_hash_stable() {
        let m = concrete_frame(&FrameSpec::default());
        let s = file::to_string(&m);
        let back = file::from_str(&s).unwrap();
        assert_eq!(back, m);
        assert_eq!(version::model_hash(&back), version::model_hash(&m));
        assert_eq!(file::to_string(&back), s, "serialization must be canonical");
    }

    #[test]
    fn binary_file_roundtrip_is_exact_and_canonical() {
        let mut m = concrete_frame(&FrameSpec::default());
        // A proxy with its own class and a mesh of awkward floats, exact after the trip.
        let mut e = m.elements.values().next().unwrap().clone();
        e.id = crate::GlobalId::derived(b"element/proxy");
        e.class = IfcClass::IfcBuildingElementProxy;
        e.ifc_entity = Some("IfcPipeSegment".into());
        e.generator = crate::Generator::Mesh {
            positions: vec![
                [0.1, 1e-300, 2535844157.0496545],
                [1.5, 0.0, 1.0],
                [0.0, 1.0, 0.0],
            ]
            .into(),
            indices: vec![0, 1, 2].into(),
        };
        // A copy of the same shape, placed elsewhere: shares the mesh, written once.
        let mut copy = e.clone();
        copy.id = crate::GlobalId::derived(b"element/proxy-copy");
        copy.placement.origin[0] += 5000.0;
        m.elements.insert(e.id, e);
        m.elements.insert(copy.id, copy);
        let b = file::to_bytes(&m);
        assert!(b.starts_with(file::MAGIC));
        let back = file::from_bytes(&b).unwrap();
        assert_eq!(back, m);
        let meshes: Vec<_> = back
            .elements
            .values()
            .filter_map(|e| match &e.generator {
                crate::Generator::Mesh { positions, .. } => Some(positions.clone()),
                _ => None,
            })
            .collect();
        assert_eq!(meshes.len(), 2);
        assert!(
            std::sync::Arc::ptr_eq(&meshes[0], &meshes[1]),
            "copies share again"
        );
        assert_eq!(version::model_hash(&back), version::model_hash(&m));
        assert_eq!(
            file::to_bytes(&back),
            b,
            "binary serialization must be canonical"
        );
        // The JSON text still opens, and a cut-off binary file is an error, not a panic.
        assert_eq!(file::from_bytes(file::to_string(&m).as_bytes()).unwrap(), m);
        assert!(file::from_bytes(&b[..b.len() / 2]).is_err());
    }

    #[test]
    fn undo_redo_restores_exact_state() {
        let mut m = concrete_frame(&FrameSpec::default());
        let h0 = version::model_hash(&m);
        let beam = first(&m, IfcClass::IfcBeam);
        let col = first(&m, IfcClass::IfcColumn);
        let mut hist = History::default();
        hist.apply(
            &mut m,
            vec![
                Command::SetGenerator {
                    id: beam,
                    generator: Generator::RectBeam {
                        width: 250.0,
                        height: 600.0,
                        length: 6000.0,
                    },
                },
                Command::RemoveElement { id: col },
            ],
        )
        .unwrap();
        let h1 = version::model_hash(&m);
        assert_ne!(h0, h1);
        assert!(hist.undo(&mut m));
        assert_eq!(version::model_hash(&m), h0);
        assert!(hist.redo(&mut m));
        assert_eq!(version::model_hash(&m), h1);
    }

    #[test]
    fn failed_transaction_rolls_back() {
        let mut m = concrete_frame(&FrameSpec::default());
        let h0 = version::model_hash(&m);
        let col = first(&m, IfcClass::IfcColumn);
        let mut hist = History::default();
        let err = hist.apply(
            &mut m,
            vec![
                Command::Rename {
                    id: col,
                    name: "renamed".into(),
                },
                Command::SetGenerator {
                    id: col,
                    generator: Generator::RectColumn {
                        width: -1.0,
                        depth: 300.0,
                        height: 3000.0,
                    },
                },
            ],
        );
        assert!(matches!(err, Err(CommandError::Invalid(_))));
        assert_eq!(version::model_hash(&m), h0);
        assert!(!hist.undo(&mut m));
    }

    #[test]
    fn storey_in_use_cannot_be_removed() {
        let mut m = concrete_frame(&FrameSpec::default());
        let st = *m.storeys.keys().next().unwrap();
        assert_eq!(
            Command::RemoveStorey { id: st }.apply(&mut m),
            Err(CommandError::InUse(st))
        );
    }

    #[test]
    fn semantic_diff_reports_field_changes() {
        let a = concrete_frame(&FrameSpec::default());
        let mut b = a.clone();
        let beam = first(&b, IfcClass::IfcBeam);
        let col = first(&b, IfcClass::IfcColumn);
        let Generator::RectBeam { width, length, .. } = b.elements[&beam].generator else {
            unreachable!()
        };
        Command::SetGenerator {
            id: beam,
            generator: Generator::RectBeam {
                width,
                height: 600.0,
                length,
            },
        }
        .apply(&mut b)
        .unwrap();
        Command::RemoveElement { id: col }.apply(&mut b).unwrap();

        let d = version::diff(&a, &b);
        assert_eq!(d.len(), 2, "{d:#?}");
        assert!(
            d.iter()
                .any(|c| matches!(c, version::Change::Removed { id, .. } if *id == col))
        );
        let mods: Vec<_> = d
            .iter()
            .filter_map(|c| match c {
                version::Change::Modified { id, fields, .. } if *id == beam => Some(fields),
                _ => None,
            })
            .collect();
        assert_eq!(mods.len(), 1);
        assert_eq!(mods[0].len(), 1);
        assert_eq!(mods[0][0].path, "generator.height");
        assert!(version::diff(&a, &a).is_empty());
    }

    #[test]
    fn rejects_bad_files() {
        assert!(file::from_str(r#"{"format":"x","version":1,"model":{}}"#).is_err());
        let m = concrete_frame(&FrameSpec::default());
        let bumped = file::to_string(&m).replacen("\"version\": 1", "\"version\": 99", 1);
        assert!(matches!(
            file::from_str(&bumped),
            Err(file::FileError::Version(99))
        ));
        // Dangling storey reference
        let mut bad = m.clone();
        let e = bad.elements.values_mut().next().unwrap();
        e.storey = GlobalId::new();
        assert!(matches!(
            file::from_str(&file::to_string(&bad)),
            Err(file::FileError::Invalid(_))
        ));
    }

    /// Review 2026-11-05: serde_json without `float_roundtrip` read some values back off by one
    /// ulp, which changed the model hash and produced false diffs.
    #[test]
    fn non_round_floats_roundtrip_exactly() {
        let mut m = concrete_frame(&FrameSpec::default());
        let id = first(&m, IfcClass::IfcColumn);
        let mut p = m.elements[&id].placement.clone();
        p.origin = [57414.518664216484, -1234.5678901234567, 0.1 + 0.2];
        Command::SetPlacement { id, placement: p }
            .apply(&mut m)
            .unwrap();
        // A spread of awkward values, not just the one found in review.
        let mut x = 0.123456789012345_f64;
        for (i, e) in m.elements.values_mut().enumerate() {
            x = (x * 7919.0 + i as f64 * 0.3141592653589793).rem_euclid(100_000.0);
            e.placement.origin[0] = x;
        }
        let back = file::from_str(&file::to_string(&m)).unwrap();
        assert_eq!(back, m);
        assert_eq!(version::model_hash(&back), version::model_hash(&m));
        assert!(version::diff(&m, &back).is_empty());
    }

    /// Review 2026-11-05: -0.0 == 0.0 but serialized differently, so equal models hashed apart.
    #[test]
    fn negative_zero_does_not_change_the_hash() {
        let a = concrete_frame(&FrameSpec::default());
        let mut b = a.clone();
        let id = first(&b, IfcClass::IfcColumn);
        let mut p = b.elements[&id].placement.clone();
        p.origin[2] = -0.0;
        Command::SetPlacement { id, placement: p }
            .apply(&mut b)
            .unwrap();
        assert_eq!(a, b);
        assert_eq!(version::model_hash(&a), version::model_hash(&b));
    }

    /// Review 2026-11-05: skewed, scaled or NaN axes were accepted and drew wrongly.
    #[test]
    fn placement_axes_are_validated() {
        let mut m = concrete_frame(&FrameSpec::default());
        let id = first(&m, IfcClass::IfcColumn);
        let base = m.elements[&id].placement.clone();
        let bad = [
            Placement {
                z_axis: [0.0, 0.0, 2.0],
                ..base.clone()
            },
            Placement {
                x_axis: [0.0, 0.0, 1.0],
                ..base.clone()
            },
            Placement {
                x_axis: [f64::NAN, 0.0, 0.0],
                ..base.clone()
            },
            Placement {
                z_axis: [0.0, 0.0, 0.0],
                ..base.clone()
            },
        ];
        for p in bad {
            let r = Command::SetPlacement {
                id,
                placement: p.clone(),
            }
            .apply(&mut m);
            assert!(matches!(r, Err(CommandError::Invalid(_))), "accepted {p:?}");
            let mut file_model = m.clone();
            file_model.elements.get_mut(&id).unwrap().placement = p;
            assert!(file::from_str(&file::to_string(&file_model)).is_err());
        }
        // A rotated but valid frame is fine.
        let s = std::f64::consts::FRAC_1_SQRT_2;
        let ok = Placement {
            x_axis: [s, s, 0.0],
            ..base
        };
        Command::SetPlacement { id, placement: ok }
            .apply(&mut m)
            .unwrap();
    }
}
