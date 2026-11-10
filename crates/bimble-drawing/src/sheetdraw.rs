//! Sheets on paper (#33): the frame and the template's title block once, each placed view drawn
//! at its own scale where it is placed, the sheet's own texts, and one legend for all its views.

use crate::plan::{PlanOptions, Swatch, draw_legend, frame_and_title_block, plan};
use crate::sheet::{Anchor, Item, Paper, Sheet};
use crate::template::{PlanTemplate, fill};
use bimble_core::sheet::Sheet as SheetDef;
use bimble_core::style::GraphicStyle;
use bimble_core::view::View;
use bimble_geom::Scene;

/// Sheet margin around the frame, mm (as on plans).
const MARGIN: f64 = 10.0;

#[derive(Clone, Debug, Default)]
pub struct SheetOptions {
    pub style: GraphicStyle,
    pub template: PlanTemplate,
    pub project: String,
    pub date: String,
}

/// What `draw` did, for reporting.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SheetInfo {
    pub views_drawn: usize,
    /// Views that could not be drawn, with why (they show as a note on the sheet).
    pub problems: Vec<String>,
    /// Where each placed view was drawn (for picking, moving and dimensioning on screen).
    pub placed: Vec<PlacedInfo>,
}

/// A placed view on paper.
#[derive(Clone, Debug, PartialEq)]
pub struct PlacedInfo {
    /// The `Placed` entry and its view.
    pub id: bimble_core::GlobalId,
    pub view: bimble_core::GlobalId,
    /// Its drawing's box on the paper, mm: x0, y0, x1, y1.
    pub rect: [f64; 4],
    pub frame: crate::viewdraw::ViewFrame,
    /// Its lines on paper, for snapping.
    pub segments: Vec<[[f64; 2]; 2]>,
}

/// Move every point of an item by `d` (paper mm).
fn shift(item: &mut Item, d: [f64; 2]) {
    let mv = |p: &mut [f64; 2]| {
        p[0] += d[0];
        p[1] += d[1];
    };
    match item {
        Item::Area { loops, .. } => loops.iter_mut().flatten().for_each(mv),
        Item::Lines { lines, .. } => lines.iter_mut().flatten().for_each(mv),
        Item::Text { pos, .. } => mv(pos),
    }
}

/// The sheet `def` on paper; `views` are the model's views (stored and default).
pub fn draw(
    scene: &Scene,
    def: &SheetDef,
    views: &[View],
    o: &SheetOptions,
) -> Result<(Sheet, SheetInfo), String> {
    def.validate(|id| views.iter().any(|v| v.id == id))?;
    let (w, h) = def.size_mm().ok_or("unknown paper")?;
    let paper = Paper::parse(&def.paper).ok_or("unknown paper")?;
    let ink = o.style.edges;
    let mut items = Vec::new();
    let mut info = SheetInfo::default();
    let mut legend: Vec<(String, Swatch)> = Vec::new();
    for p in &def.placed {
        let Some(v) = views.iter().find(|v| v.id == p.view) else {
            continue;
        };
        let opts = PlanOptions {
            scale: p.scale,
            paper: Some(paper),
            style: o.style.clone(),
            project: o.project.clone(),
            date: o.date.clone(),
            template: o.template.clone(),
            view: Some(v.clone()),
            bare: true,
            section_marks: crate::plan::section_marks(views),
            ..Default::default()
        };
        match plan(scene, &opts) {
            Ok((drawn, pi)) => {
                let d = [
                    p.at_mm[0] - drawn.width_mm / 2.0,
                    p.at_mm[1] - drawn.height_mm / 2.0,
                ];
                let mut segments = Vec::new();
                for mut it in drawn.items {
                    shift(&mut it, d);
                    match &it {
                        Item::Lines { lines, .. } => {
                            for l in lines {
                                segments.extend(l.windows(2).map(|w| [w[0], w[1]]));
                            }
                        }
                        Item::Area {
                            loops,
                            outline: Some(_),
                            ..
                        } => {
                            for l in loops {
                                segments.extend(l.windows(2).map(|w| [w[0], w[1]]));
                                if let (Some(a), Some(b)) = (l.last(), l.first()) {
                                    segments.push([*a, *b]);
                                }
                            }
                        }
                        _ => {}
                    }
                    items.push(it);
                }
                info.placed.push(PlacedInfo {
                    id: p.id,
                    view: p.view,
                    rect: [d[0], d[1], d[0] + drawn.width_mm, d[1] + drawn.height_mm],
                    frame: pi.frame.shifted(d),
                    segments,
                });
                for e in pi.legend {
                    if !legend.contains(&e) {
                        legend.push(e);
                    }
                }
                info.views_drawn += 1;
            }
            Err(e) => {
                let msg = format!("{}: {e}", v.name);
                items.push(Item::Text {
                    pos: p.at_mm,
                    size_mm: 3.0,
                    text: msg.clone(),
                    color: ink,
                    anchor: Anchor::Middle,
                    angle_deg: 0.0,
                    bold: false,
                });
                info.problems.push(msg);
            }
        }
    }
    for t in &def.texts {
        let tt = t.kind.as_ref().and_then(|k| o.style.paper.text_type(k));
        let size = tt.map_or(t.size_mm, |x| x.size_mm as f64);
        let color = tt.map_or(ink, |x| x.color);
        for (i, line) in t.text.split('\n').enumerate() {
            items.push(Item::Text {
                pos: [t.at_mm[0], t.at_mm[1] + i as f64 * size * 1.25],
                size_mm: size,
                text: line.to_string(),
                color,
                anchor: Anchor::Start,
                angle_deg: t.angle_deg,
                bold: false,
            });
        }
    }
    // Cut swatches first, then seen, then above, as on a plan.
    let rank = |s: &Swatch| match s {
        Swatch::Cut(_) => 0,
        Swatch::Seen(_) => 1,
        Swatch::Above => 2,
    };
    legend.sort_by_key(|(_, s)| rank(s));
    let tb_top = h - MARGIN - o.template.title_block.height_mm;
    draw_legend(&mut items, MARGIN, tb_top, &legend, &o.template, &o.style);

    let mut fields = o.template.fields.clone();
    fields.extend(def.fields.clone());
    let values = [
        ("project", o.project.clone()),
        ("number", def.number.clone()),
        ("sheet", def.name.clone()),
        ("drawing", def.name.clone()),
        ("drawing_no", def.number.clone()),
        (
            "scale",
            scales(def)
                .iter()
                .map(|s| format!("1:{s}"))
                .collect::<Vec<_>>()
                .join(", "),
        ),
        (
            "paper",
            format!(
                "{}{}",
                paper.name(),
                if def.portrait { " portrait" } else { "" }
            ),
        ),
        ("date", o.date.clone()),
        ("bimble", env!("CARGO_PKG_VERSION").to_string()),
        // Plan-only values: empty on a sheet of several views.
        ("storey", String::new()),
        ("level", String::new()),
        ("view", String::new()),
        ("n", String::new()),
    ];
    let filled = |s: &str| fill(s, &values, &fields);
    frame_and_title_block(&mut items, w, h, &o.template, &filled, ink, &o.style);
    let title = format!("{} {}", def.number, def.name);
    Ok((
        Sheet {
            width_mm: w,
            height_mm: h,
            title: if o.project.is_empty() {
                title
            } else {
                format!("{} – {title}", o.project)
            },
            items,
            font: o.style.paper.font,
        },
        info,
    ))
}

/// The scales of the views on a sheet, each once, largest drawing first.
fn scales(def: &SheetDef) -> Vec<u32> {
    let mut s: Vec<u32> = def.placed.iter().map(|p| p.scale).collect();
    s.sort_unstable();
    s.dedup();
    s
}

#[cfg(test)]
mod tests {
    use super::*;
    use bimble_core::GlobalId;
    use bimble_core::demo::{FrameSpec, concrete_frame};
    use bimble_core::sheet::{Placed, SheetText};
    use bimble_core::view::{Anchor as VAnchor, Annotation, Facing, ViewKind, default_views};

    fn texts(s: &Sheet) -> Vec<String> {
        s.items
            .iter()
            .filter_map(|i| match i {
                Item::Text { text, .. } => Some(text.clone()),
                _ => None,
            })
            .collect()
    }

    /// A sheet with a plan at 1:100 and a section at 1:50: both drawn where they are placed,
    /// one title block filled from the sheet, the sheet's text, one legend.
    #[test]
    fn a_sheet_places_views_at_their_scales() {
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let mut views = default_views(&scene.levels());
        let section = View {
            id: GlobalId::new(),
            name: "Section A".into(),
            kind: ViewKind::Section {
                origin: [9000.0, 5000.0],
                facing: Facing::North,
                depth_mm: 0.0,
            },
            visibility: Default::default(),
            references: Vec::new(),
            annotations: vec![Annotation::Text {
                id: GlobalId::new(),
                text: "Axis 2".into(),
                at: [0.0, 5000.0, 3500.0],
                size_mm: 3.5,
                angle_deg: 0.0,
                kind: None,
            }],
        };
        views.push(section.clone());
        let plan_view = views
            .iter()
            .find(|v| v.name.starts_with("Plan Niveau 1"))
            .unwrap();
        let mut def = SheetDef::new("S-101", "Plans and section");
        def.paper = "A1".into();
        def.fields.insert("phase".into(), "Tender".into());
        def.placed.push(Placed {
            id: GlobalId::new(),
            view: plan_view.id,
            scale: 100,
            at_mm: [250.0, 220.0],
        });
        def.placed.push(Placed {
            id: GlobalId::new(),
            view: section.id,
            scale: 50,
            at_mm: [620.0, 220.0],
        });
        def.texts.push(SheetText {
            id: GlobalId::new(),
            text: "Concrete C30/37".into(),
            at_mm: [40.0, 40.0],
            size_mm: 5.0,
            angle_deg: 0.0,
            kind: None,
        });
        let o = SheetOptions {
            project: "Demo".into(),
            date: "2026-11-06".into(),
            ..Default::default()
        };
        let (sheet, info) = draw(&scene, &def, &views, &o).unwrap();
        assert_eq!((sheet.width_mm, sheet.height_mm), (841.0, 594.0));
        assert_eq!(info.views_drawn, 2, "{info:?}");
        let t = texts(&sheet);
        for want in [
            "Plan Niveau 1, 1:100",
            "Section A, 1:50",
            "Axis 2",
            "Concrete C30/37",
            "Plans and section",
            "1:50, 1:100",
        ] {
            assert!(t.iter().any(|x| x == want), "{want}: {t:?}");
        }
        // One legend title, one title block.
        assert_eq!(t.iter().filter(|x| *x == "Legend").count(), 1);
        assert_eq!(t.iter().filter(|x| *x == "PROJECT").count(), 1);
        // The plan is around its place: its title is left of 250 + half its width.
        let plan_title = sheet.items.iter().find_map(|i| match i {
            Item::Text { text, pos, .. } if text == "Plan Niveau 1, 1:100" => Some(*pos),
            _ => None,
        });
        let p = plan_title.unwrap();
        assert!(
            p[0] > 100.0 && p[0] < 250.0 && p[1] > 220.0 && p[1] < 350.0,
            "{p:?}"
        );
        // Every item is on the paper.
        for i in &sheet.items {
            let pts: Vec<[f64; 2]> = match i {
                Item::Area { loops, .. } => loops.iter().flatten().copied().collect(),
                Item::Lines { lines, .. } => lines.iter().flatten().copied().collect(),
                Item::Text { pos, .. } => vec![*pos],
            };
            for q in pts {
                assert!(
                    (0.0..=841.0).contains(&q[0]) && (0.0..=594.0).contains(&q[1]),
                    "{q:?} off the sheet"
                );
            }
        }
        sheet.to_pdf();
    }

    /// A view's dimension is drawn with its value in the template's unit (cm by default).
    #[test]
    fn view_dimensions_are_drawn_in_the_template_unit() {
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let mut views = default_views(&scene.levels());
        let v = views
            .iter_mut()
            .find(|v| v.name == "Plan Niveau 1")
            .unwrap();
        v.annotations.push(Annotation::Dimension {
            id: GlobalId::new(),
            a: VAnchor {
                at: [0.0, 0.0, 0.0],
                element: None,
            },
            b: VAnchor {
                at: [6000.0, 0.0, 0.0],
                element: None,
            },
            offset_mm: -1500.0,
            axis: Default::default(),
            kind: None,
        });
        let id = v.id;
        let mut def = SheetDef::new("S-102", "Plan");
        def.placed.push(Placed {
            id: GlobalId::new(),
            view: id,
            scale: 100,
            at_mm: [400.0, 250.0],
        });
        let (sheet, _) = draw(&scene, &def, &views, &SheetOptions::default()).unwrap();
        assert!(
            texts(&sheet).iter().any(|t| t == "600"),
            "{:?}",
            texts(&sheet)
        );
    }

    /// A dimension of a type is written in that type's unit and colour, and a sheet text of a
    /// type takes that type's size and colour (#43).
    #[test]
    fn annotation_types_set_unit_size_and_colour() {
        use bimble_core::sheet::SheetText;
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let mut views = default_views(&scene.levels());
        let v = views
            .iter_mut()
            .find(|v| v.name == "Plan Niveau 1")
            .unwrap();
        v.annotations.push(Annotation::Dimension {
            id: GlobalId::new(),
            a: VAnchor {
                at: [0.0, 0.0, 0.0],
                element: None,
            },
            b: VAnchor {
                at: [6000.0, 0.0, 0.0],
                element: None,
            },
            offset_mm: -1500.0,
            axis: Default::default(),
            kind: Some("red m".into()),
        });
        let id = v.id;
        let mut def = SheetDef::new("S-104", "Typed");
        def.placed.push(Placed {
            id: GlobalId::new(),
            view: id,
            scale: 100,
            at_mm: [400.0, 250.0],
        });
        def.texts.push(SheetText {
            id: GlobalId::new(),
            text: "Note".into(),
            at_mm: [100.0, 100.0],
            size_mm: 9.0,
            angle_deg: 0.0,
            kind: Some("Arial 4 mm blue".into()),
        });
        let o = SheetOptions::default();
        let red = o.style.paper.dim_type("red m").unwrap().color;
        let blue = o.style.paper.text_type("Arial 4 mm blue").unwrap().color;
        let (sheet, _) = draw(&scene, &def, &views, &o).unwrap();
        let found = |want: &str| {
            sheet.items.iter().find_map(|i| match i {
                Item::Text {
                    text,
                    color,
                    size_mm,
                    ..
                } if text == want => Some((*color, *size_mm)),
                _ => None,
            })
        };
        let (c, size) = found("6.00 m").unwrap_or_else(|| panic!("{:?}", texts(&sheet)));
        assert_eq!(c, red);
        assert!((size - 2.5).abs() < 1e-6);
        let (c, size) = found("Note").unwrap();
        assert_eq!(c, blue);
        assert!((size - 4.0).abs() < 1e-6);
        // Its lines are red too.
        assert!(sheet.items.iter().any(|i| matches!(i,
            Item::Lines { pen, .. } if pen.color == red)));
    }

    /// A placed view that can't be drawn leaves a note, not a failed sheet.
    #[test]
    fn a_view_that_cannot_be_drawn_leaves_a_note() {
        let scene = Scene::from_model(&concrete_frame(&FrameSpec::default()));
        let views = default_views(&scene.levels());
        let three_d = views.iter().find(|v| v.name == "3D").unwrap();
        let mut def = SheetDef::new("S-103", "Odd");
        def.placed.push(Placed {
            id: GlobalId::new(),
            view: three_d.id,
            scale: 100,
            at_mm: [400.0, 250.0],
        });
        let (sheet, info) = draw(&scene, &def, &views, &SheetOptions::default()).unwrap();
        assert_eq!(info.views_drawn, 0);
        assert_eq!(info.problems.len(), 1);
        assert!(
            texts(&sheet)
                .iter()
                .any(|t| t.contains("3D view has no paper drawing"))
        );
    }
}
