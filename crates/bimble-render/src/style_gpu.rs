//! GPU side of the graphic style: the uniform table the shader reads (see shader.wgsl).
//! The style itself (data, JSON) is `bimble_core::style`.

use bimble_core::MaterialCategory;
use bimble_core::style::{Fill, GraphicStyle, Rgb, plain};

// ---------------------------------------------------------------- GPU table

/// Slots in the shader's style table: one per material family, then selection.
pub(crate) const SLOTS: usize = 16;
pub(crate) const SELECTED_SLOT: u8 = 15;
pub(crate) const REFERENCE_SLOT: u8 = 14;
pub(crate) const ONLY_HERE_SLOT: u8 = 13;
pub(crate) const ONLY_IN_REFERENCE_SLOT: u8 = 12;

pub(crate) fn slot(c: MaterialCategory) -> u8 {
    MaterialCategory::ALL.iter().position(|&x| x == c).unwrap() as u8
}

/// One table entry, laid out as seven vec4 for the shader (see shader.wgsl). Everything that is
/// constant per family is computed here, once, so the fragment shader only evaluates patterns.
#[repr(C)]
#[derive(Clone, Copy, Default, bytemuck::Pod, bytemuck::Zeroable)]
pub(crate) struct GpuFill {
    fill: [f32; 4],
    ink: [f32; 4],
    lines: [[f32; 4]; 2],
    dashes: [[f32; 4]; 2],
    stipple: [f32; 4],
}

/// sRGB-encoded 0..1 to linear, as the shader did per vertex.
fn srgb_to_linear(c: f32) -> f32 {
    if c <= 0.04045 {
        c / 12.92
    } else {
        ((c + 0.055) / 1.055).powf(2.4)
    }
}

fn linear(c: Rgb) -> [f32; 3] {
    c.unit().map(srgb_to_linear)
}

impl GpuFill {
    fn new(f: &Fill, darken: f32) -> GpuFill {
        let c = linear(f.color);
        // Which patterns this family has, as bits for the shader: 1 lines 0, 2 lines 1, 4 stipple.
        let flags =
            f.lines[0].map_or(0, |_| 1) | f.lines[1].map_or(0, |_| 2) | f.stipple.map_or(0, |_| 4);
        // ink.rgb: linear ink colour, ink.a: 1 = use it; otherwise the fill darkened.
        let (ink, mode) = match f.ink {
            Some(i) => (linear(i), 1.0),
            None => (c.map(|x| x * (1.0 - darken)), 0.0),
        };
        let mut g = GpuFill {
            fill: [c[0], c[1], c[2], flags as f32],
            ink: [ink[0], ink[1], ink[2], mode],
            ..Default::default()
        };
        for (k, l) in f.lines.iter().enumerate() {
            let Some(l) = l else { continue };
            // Screen y points down: a line at angle a has the pixel-space normal (sin a, cos a)
            // and runs along (cos a, -sin a).
            let a = l.angle_deg.to_radians();
            let inv = 1.0 / l.spacing_px;
            g.lines[k] = [a.sin() * inv, a.cos() * inv, l.width, 1.0];
            if let Some(d) = l.dash_px {
                g.dashes[k] = [a.cos() / d, -a.sin() / d, l.dash_on, 1.0];
            }
        }
        if let Some(s) = f.stipple {
            g.stipple = [1.0 / s.cell_px, s.density, s.strength, 0.0];
        }
        g
    }
}

pub(crate) fn gpu_table(s: &GraphicStyle) -> [GpuFill; SLOTS] {
    let mut t = [GpuFill::default(); SLOTS];
    for c in MaterialCategory::ALL {
        t[slot(c) as usize] = GpuFill::new(s.families.get(c), s.hatch_darken);
    }
    t[SELECTED_SLOT as usize] = GpuFill::new(&plain(s.selection), s.hatch_darken);
    t[REFERENCE_SLOT as usize] = GpuFill::new(&plain(s.reference), s.hatch_darken);
    t[ONLY_HERE_SLOT as usize] = GpuFill::new(&plain(s.only_here), s.hatch_darken);
    t[ONLY_IN_REFERENCE_SLOT as usize] = GpuFill::new(&plain(s.only_in_reference), s.hatch_darken);
    t
}
