//! Generated test models. The default is the prototype case from the Feuille de route:
//! a concrete frame, 2 storeys, 3 × 2 bays, with columns, beams, slabs and pad footings.

use crate::*;

#[derive(Clone, Debug)]
pub struct FrameSpec {
    pub bays_x: usize,
    pub bays_y: usize,
    pub storeys: usize,
    pub span_x: f64,
    pub span_y: f64,
    pub storey_height: f64,
}

impl Default for FrameSpec {
    fn default() -> Self {
        FrameSpec {
            bays_x: 3,
            bays_y: 2,
            storeys: 2,
            span_x: 6000.0,
            span_y: 5000.0,
            storey_height: 3000.0,
        }
    }
}

impl FrameSpec {
    /// Number of elements `concrete_frame` will create.
    pub fn element_count(&self) -> usize {
        let cols = (self.bays_x + 1) * (self.bays_y + 1);
        let beams = self.bays_x * (self.bays_y + 1) + self.bays_y * (self.bays_x + 1);
        cols + self.storeys * (cols + beams + 1)
    }
}

pub fn concrete_frame(spec: &FrameSpec) -> Model {
    let mut m = Model::new(format!(
        "Concrete frame {}x{}x{}",
        spec.bays_x, spec.bays_y, spec.storeys
    ));
    let mut cmds = Vec::new();
    let concrete = Material {
        id: GlobalId::new(),
        name: "C30/37".into(),
        category: MaterialCategory::Concrete,
        // Its density, modulus and strength from the library (ADR 0031).
        props: crate::library::lookup("C30/37")
            .map(|m| m.props)
            .unwrap_or_default(),
    };
    let mat = concrete.id;
    cmds.push(Command::AddMaterial(concrete));

    let (col, beam_w, beam_h, slab_t) = (300.0, 250.0, 500.0, 220.0);
    let (lx, ly) = (
        spec.bays_x as f64 * spec.span_x,
        spec.bays_y as f64 * spec.span_y,
    );
    let grid = |i: usize, j: usize| (i as f64 * spec.span_x, j as f64 * spec.span_y);
    let el = |class, name: String, storey, placement, generator| Element {
        id: GlobalId::new(),
        class,
        name,
        storey,
        material: mat,
        placement,
        generator,
        openings: Vec::new(),
        properties: Default::default(),
    };
    // no openings or properties in the demo frame

    let mut storey_ids = Vec::new();
    for s in 0..=spec.storeys {
        let st = Storey {
            id: GlobalId::new(),
            name: if s == 0 {
                "Fondations".into()
            } else {
                format!("Niveau {s}")
            },
            elevation: if s == 0 {
                -800.0
            } else {
                (s - 1) as f64 * spec.storey_height
            },
        };
        storey_ids.push(st.id);
        cmds.push(Command::AddStorey(st));
    }

    // Pad footings: top at level 0 (storey elevation -800, height 800).
    for i in 0..=spec.bays_x {
        for j in 0..=spec.bays_y {
            let (x, y) = grid(i, j);
            cmds.push(Command::AddElement(el(
                IfcClass::IfcFooting,
                format!("S{i}{j}"),
                storey_ids[0],
                Placement::at(x, y, 0.0),
                Generator::PadFooting {
                    width: 1200.0,
                    depth: 1200.0,
                    height: 800.0,
                },
            )));
        }
    }

    for (s, &st) in storey_ids.iter().enumerate().skip(1) {
        let h = spec.storey_height;
        for i in 0..=spec.bays_x {
            for j in 0..=spec.bays_y {
                let (x, y) = grid(i, j);
                cmds.push(Command::AddElement(el(
                    IfcClass::IfcColumn,
                    format!("P{s}.{i}{j}"),
                    st,
                    Placement::at(x, y, 0.0),
                    Generator::RectColumn {
                        width: col,
                        depth: col,
                        height: h - slab_t,
                    },
                )));
            }
        }
        // Beams: soffit under the slab. Placement: local Z along the beam axis, profile centred.
        let z = h - slab_t - beam_h / 2.0;
        let beam = |name: String, x0: f64, y0: f64, dir: [f64; 3], len: f64| {
            el(
                IfcClass::IfcBeam,
                name,
                st,
                Placement {
                    origin: [x0, y0, z],
                    z_axis: dir,
                    x_axis: [-dir[1], dir[0], 0.0],
                },
                Generator::RectBeam {
                    width: beam_w,
                    height: beam_h,
                    length: len,
                },
            )
        };
        for j in 0..=spec.bays_y {
            for i in 0..spec.bays_x {
                let (x, y) = grid(i, j);
                cmds.push(Command::AddElement(beam(
                    format!("B{s}.x{i}{j}"),
                    x,
                    y,
                    [1.0, 0.0, 0.0],
                    spec.span_x,
                )));
            }
        }
        for i in 0..=spec.bays_x {
            for j in 0..spec.bays_y {
                let (x, y) = grid(i, j);
                cmds.push(Command::AddElement(beam(
                    format!("B{s}.y{i}{j}"),
                    x,
                    y,
                    [0.0, 1.0, 0.0],
                    spec.span_y,
                )));
            }
        }
        let d = col / 2.0;
        cmds.push(Command::AddElement(el(
            IfcClass::IfcSlab,
            format!("D{s}"),
            st,
            Placement::at(0.0, 0.0, h - slab_t),
            Generator::Slab {
                outline: vec![[-d, -d], [lx + d, -d], [lx + d, ly + d], [-d, ly + d]],
                thickness: slab_t,
            },
        )));
    }

    let mut hist = History::default();
    hist.apply(&mut m, cmds).expect("demo frame is valid");
    m
}
