//! Where a model is on Earth, and which way its project north points (ADR 0045).
//!
//! As in Revit: plans and sections are drawn in **project north** (along the building's grid)
//! unless a view asks for true north; coordinates are read in the survey system, **LV95**
//! (CH1903+, EPSG:2056) for Swiss models, or in **WGS84** latitude and longitude.

use serde::{Deserialize, Serialize};

/// A model's place: world (model) coordinates are mm; `east_m`/`north_m`/`height_m` are what
/// the survey system adds to them (0 when the model is drawn in survey coordinates already, as
/// Revit exports usually are).
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Georef {
    /// Project north, degrees counter-clockwise from true (grid) north: the building's turn.
    #[serde(skip_serializing_if = "is_zero")]
    pub rotation_deg: f64,
    /// The survey system, e.g. "EPSG:2056" (LV95); None when unknown.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub crs: Option<String>,
    #[serde(skip_serializing_if = "is_zero")]
    pub east_m: f64,
    #[serde(skip_serializing_if = "is_zero")]
    pub north_m: f64,
    #[serde(skip_serializing_if = "is_zero")]
    pub height_m: f64,
    /// When the model is in its own coordinates (IFC4 IfcMapConversion): the turn of its x axis
    /// in the survey system, degrees counter-clockwise. True north is turned by its opposite.
    #[serde(skip_serializing_if = "is_zero")]
    pub map_rotation_deg: f64,
}

fn is_zero(x: &f64) -> bool {
    *x == 0.0
}

/// LV95, the Swiss survey system (CH1903+).
pub const LV95: &str = "EPSG:2056";

impl Georef {
    pub fn is_default(&self) -> bool {
        *self == Georef::default()
    }

    /// From a turn (the building's x axis, in world XY) and where the building is (world mm):
    /// the rotation, and LV95 when the building lies within Switzerland's LV95 range.
    pub fn from_frame(x_axis: [f64; 2], at_mm: [f64; 3]) -> Georef {
        let mut rotation_deg = x_axis[1].atan2(x_axis[0]).to_degrees();
        // Canonical: whole micro-degrees, no -0.0.
        rotation_deg = (rotation_deg * 1e6).round() / 1e6 + 0.0;
        let (e, n) = (at_mm[0] / 1000.0, at_mm[1] / 1000.0);
        let swiss =
            (2_480_000.0..=2_840_000.0).contains(&e) && (1_070_000.0..=1_300_000.0).contains(&n);
        Georef {
            rotation_deg,
            crs: swiss.then(|| LV95.to_string()),
            ..Default::default()
        }
    }

    /// Survey coordinates (m) of a world point (mm).
    pub fn survey(&self, world_mm: [f64; 3]) -> [f64; 3] {
        let (s, c) = self.map_rotation_deg.to_radians().sin_cos();
        let (x, y) = (world_mm[0] / 1000.0, world_mm[1] / 1000.0);
        [
            c * x - s * y + self.east_m,
            s * x + c * y + self.north_m,
            world_mm[2] / 1000.0 + self.height_m,
        ]
    }

    /// Whether survey coordinates differ from world ones (an IFC4 map conversion).
    pub fn has_map(&self) -> bool {
        self.east_m != 0.0 || self.north_m != 0.0 || self.map_rotation_deg != 0.0
    }

    /// How a plan drawn in survey coordinates (mm, e.g. an LV95 DXF) lies in the world: turn it by
    /// the returned degrees about its origin, then move it by the returned offset (mm), as a
    /// reference plan is placed. The inverse of `survey` in plan.
    pub fn survey_plan_to_world(&self) -> (f64, [f64; 2]) {
        let a = -self.map_rotation_deg.to_radians();
        let (s, c) = a.sin_cos();
        let (e, n) = (self.east_m * 1000.0, self.north_m * 1000.0);
        (
            -self.map_rotation_deg,
            [-(c * e - s * n) + 0.0, -(s * e + c * n) + 0.0],
        )
    }

    /// Latitude and longitude (degrees, WGS84) of a world point, when the survey system is LV95.
    pub fn wgs84(&self, world_mm: [f64; 3]) -> Option<[f64; 2]> {
        (self.crs.as_deref() == Some(LV95)).then(|| {
            let s = self.survey(world_mm);
            lv95_to_wgs84(s[0], s[1])
        })
    }

    /// How much a plan turns (radians, counter-clockwise, from the world's +Y up): by the
    /// building's turn in project north, by the map's opposite in true north.
    pub fn plan_turn(&self, true_north: bool) -> f64 {
        if true_north {
            -self.map_rotation_deg.to_radians()
        } else {
            self.rotation_deg.to_radians()
        }
    }
}

/// swisstopo's approximate formulas (LV95 → WGS84, about 1 m): latitude, longitude in degrees.
pub fn lv95_to_wgs84(east: f64, north: f64) -> [f64; 2] {
    let y = (east - 2_600_000.0) / 1_000_000.0;
    let x = (north - 1_200_000.0) / 1_000_000.0;
    let lon = 2.6779094 + 4.728982 * y + 0.791484 * y * x + 0.1306 * y * x * x - 0.0436 * y.powi(3);
    let lat = 16.9023892 + 3.238272 * x
        - 0.270978 * y * y
        - 0.002528 * x * x
        - 0.0447 * y * y * x
        - 0.0140 * x.powi(3);
    [lat * 100.0 / 36.0, lon * 100.0 / 36.0]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lv95_to_wgs84_as_swisstopo() {
        // The origin (Bern, old observatory) and swisstopo's worked example.
        let [lat, lon] = lv95_to_wgs84(2_600_000.0, 1_200_000.0);
        assert!(
            (lat - 46.951083).abs() < 1e-5 && (lon - 7.438633).abs() < 1e-5,
            "{lat} {lon}"
        );
        let [lat, lon] = lv95_to_wgs84(2_700_000.0, 1_100_000.0);
        let dms = |d: f64, m: f64, s: f64| d + m / 60.0 + s / 3600.0;
        assert!((lat - dms(46.0, 2.0, 38.87)).abs() < 1e-4, "{lat}");
        assert!((lon - dms(8.0, 43.0, 49.79)).abs() < 1e-4, "{lon}");
    }

    /// A model in its own coordinates (IfcMapConversion): a survey point placed by
    /// `survey_plan_to_world` lands on the world point whose survey coordinates it has.
    #[test]
    fn survey_plans_land_where_their_coordinates_say() {
        let g = Georef {
            east_m: 2_600_000.0,
            north_m: 1_200_000.0,
            map_rotation_deg: 12.5,
            crs: Some(LV95.into()),
            ..Default::default()
        };
        let (rot, off) = g.survey_plan_to_world();
        let world = [12_345.0, -6_789.0, 0.0];
        let s = g.survey(world);
        let p = [s[0] * 1000.0, s[1] * 1000.0];
        let (sn, cs) = rot.to_radians().sin_cos();
        let w = [
            cs * p[0] - sn * p[1] + off[0],
            sn * p[0] + cs * p[1] + off[1],
        ];
        assert!(
            (w[0] - world[0]).abs() < 1e-3 && (w[1] - world[1]).abs() < 1e-3,
            "{w:?}"
        );
    }

    #[test]
    fn a_turned_building_in_switzerland() {
        let a = 30f64.to_radians();
        let g = Georef::from_frame([a.cos(), a.sin()], [2_600_000_000.0, 1_200_000_000.0, 0.0]);
        assert_eq!(g.rotation_deg, 30.0);
        assert_eq!(g.crs.as_deref(), Some(LV95));
        let s = g.survey([2_600_123_450.0, 1_200_000_000.0, 432_100.0]);
        assert!((s[0] - 2_600_123.45).abs() < 1e-6 && (s[2] - 432.1).abs() < 1e-9);
        assert!(g.wgs84([2_600_000_000.0, 1_200_000_000.0, 0.0]).is_some());
        // Elsewhere: the turn, no survey system known.
        let g = Georef::from_frame([1.0, 0.0], [0.0; 3]);
        assert!(g.is_default());
    }
}
