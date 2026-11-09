//! File → Share for viewing: the open model as a prepared viewing file (ADR 0040), for others to
//! open in a browser or on a small machine without parsing the IFC.

use super::*;

impl Viewer {
    /// Save the scene shown as a `.bimbleview` (a download in a browser).
    pub(crate) fn share_for_viewing(&mut self) {
        let Some(l) = &self.loaded else { return };
        let stem = l
            .path
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_else(|| "model".into());
        let ext = bimble_geom::viewfile::EXTENSION;
        let bytes = bimble_geom::viewfile::write(&l.scene, true);
        self.view_message = match files::save(
            &format!("{stem}.{ext}"),
            &[("Bimble view (read-only)", &[ext])],
            &bytes,
        ) {
            Ok(Some(to)) => Some(format!(
                "Saved {to} ({:.1} MB): opens in Bimble, also in a browser, read-only",
                bytes.len() as f64 / 1e6
            )),
            Ok(None) => return,
            Err(e) => Some(e),
        };
    }
}

#[cfg(test)]
mod tests {
    use bimble_geom::{Scene, codec::TOLERANCE_MM};

    fn demo_ifc() -> Vec<u8> {
        let m = bimble_core::demo::concrete_frame(&bimble_core::demo::FrameSpec::default());
        bimble_ifc::export(&m, "t.ifc", "t").into_bytes()
    }

    /// IFC → pack → open gives the IFC's scene: elements, ids, classes, every point within the
    /// codec's tolerance; opened by name like any model, read-only.
    #[test]
    fn packed_ifc_opens_as_the_same_scene() {
        let ifc = bimble_ifc::read::read_bytes(demo_ifc()).unwrap();
        let bytes = bimble_geom::viewfile::write(&ifc, true);
        let (view, model) = crate::load_bytes("frame.bimbleview", bytes).unwrap();
        assert!(model.is_none(), "read-only");
        assert_eq!(view.elements.len(), ifc.elements.len());
        assert_eq!(view.stored_triangles(), ifc.stored_triangles());
        assert_eq!(view.storeys, ifc.storeys);
        for (a, b) in ifc.elements.iter().zip(&view.elements) {
            assert_eq!((&a.global_id, &a.class), (&b.global_id, &b.class));
            let (m, n) = (a.world_mesh(), b.world_mesh());
            assert_eq!(m.indices, n.indices);
            for (p, q) in m.positions.iter().zip(&n.positions) {
                for k in 0..3 {
                    // Placed copies: the shape within tolerance, then the same f32 placement.
                    assert!(
                        (p[k] - q[k]).abs() as f64 <= TOLERANCE_MM * 1.01,
                        "{p:?} {q:?}"
                    );
                }
            }
        }
        assert!(crate::load_bytes("x.bimbleview", b"BIMBLEVW".to_vec()).is_err());
    }

    /// Same picture from the IFC and from its view file, within a few pixels.
    #[test]
    fn packed_ifc_renders_the_same() {
        use bimble_render::{Camera, DrawOptions, Headless, Renderer, Target};
        let Ok(gpu) = Headless::new() else {
            eprintln!("no GPU adapter, skipping");
            return;
        };
        let ifc = bimble_ifc::read::read_bytes(demo_ifc()).unwrap();
        let view =
            bimble_geom::viewfile::read_bytes(&bimble_geom::viewfile::write(&ifc, false)).unwrap();
        let (w, h) = (320, 240);
        let draw = |s: &Scene| {
            let r = Renderer::new(&gpu.device, 1);
            let gs = r.upload(&gpu.device, &gpu.queue, s, |_, _| {
                bimble_render::Style::Normal
            });
            let target = Target::new(&gpu.device, (w, h), 1);
            let mut cam = Camera {
                yaw: 0.6,
                pitch: 0.5,
                ..Default::default()
            };
            cam.fit(&ifc, w as f32 / h as f32);
            r.render(
                &gpu.device,
                &gpu.queue,
                &target,
                &gs,
                &cam,
                &DrawOptions::default(),
            );
            gpu.read_rgba(&target).unwrap()
        };
        let (a, b) = (draw(&ifc), draw(&view));
        let differ = a
            .chunks(4)
            .zip(b.chunks(4))
            .filter(|(p, q)| p.iter().zip(*q).any(|(x, y)| x.abs_diff(*y) > 8))
            .count();
        // A twentieth of a millimetre moves an edge by a pixel at most, here and there.
        assert!(differ * 200 < (w * h) as usize, "{differ} pixels differ");
    }
}
