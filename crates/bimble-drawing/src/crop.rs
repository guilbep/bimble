//! A view's crop region on paper (ADR 0046): the drawing's geometry clipped to a rectangle
//! before it is laid out, so the page, axes and chains follow the crop. Loops are clipped ring by
//! ring (Sutherland–Hodgman): the rectangle is convex, so their even-odd fill clips right.

type P2 = [f64; 2];

/// An axis-aligned rectangle, scene mm in the turned scene.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Rect {
    pub lo: P2,
    pub hi: P2,
}

impl Rect {
    pub fn contains(&self, p: P2) -> bool {
        (0..2).all(|k| p[k] >= self.lo[k] && p[k] <= self.hi[k])
    }

    /// A closed loop clipped to the rectangle; None when nothing is left.
    pub fn polygon(&self, poly: &[P2]) -> Option<Vec<P2>> {
        let mut out: Vec<P2> = poly.to_vec();
        // Each side: (axis, bound, keep below the bound?).
        for (k, v, below) in [
            (0, self.lo[0], false),
            (0, self.hi[0], true),
            (1, self.lo[1], false),
            (1, self.hi[1], true),
        ] {
            if out.is_empty() {
                break;
            }
            let inside = |p: &P2| if below { p[k] <= v } else { p[k] >= v };
            let cross = |a: &P2, b: &P2| {
                let t = (v - a[k]) / (b[k] - a[k]);
                let mut q = [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t];
                q[k] = v;
                q
            };
            let input = std::mem::take(&mut out);
            for (i, b) in input.iter().enumerate() {
                let a = &input[(i + input.len() - 1) % input.len()];
                match (inside(a), inside(b)) {
                    (true, true) => out.push(*b),
                    (true, false) => out.push(cross(a, b)),
                    (false, true) => {
                        out.push(cross(a, b));
                        out.push(*b);
                    }
                    (false, false) => {}
                }
            }
        }
        (out.len() >= 3).then_some(out)
    }

    /// A segment clipped to the rectangle (Liang–Barsky); None when outside.
    pub fn segment(&self, s: [P2; 2]) -> Option<[P2; 2]> {
        let d = [s[1][0] - s[0][0], s[1][1] - s[0][1]];
        let (mut t0, mut t1) = (0.0f64, 1.0f64);
        for k in 0..2 {
            for (p, q) in [(-d[k], s[0][k] - self.lo[k]), (d[k], self.hi[k] - s[0][k])] {
                if p == 0.0 {
                    if q < 0.0 {
                        return None;
                    }
                } else {
                    let r = q / p;
                    if p < 0.0 {
                        t0 = t0.max(r);
                    } else {
                        t1 = t1.min(r);
                    }
                }
            }
        }
        (t0 <= t1).then(|| {
            let at = |t: f64| [s[0][0] + d[0] * t, s[0][1] + d[1] * t];
            [at(t0), at(t1)]
        })
    }

    /// An open polyline clipped: the pieces inside.
    pub fn polyline(&self, line: &[P2]) -> Vec<Vec<P2>> {
        let mut pieces: Vec<Vec<P2>> = Vec::new();
        for w in line.windows(2) {
            let Some([a, b]) = self.segment([w[0], w[1]]) else {
                continue;
            };
            match pieces.last_mut() {
                Some(last) if last.last() == Some(&a) => last.push(b),
                _ => pieces.push(vec![a, b]),
            }
        }
        pieces
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn area(p: &[P2]) -> f64 {
        p.iter()
            .zip(p.iter().cycle().skip(1))
            .map(|(a, b)| a[0] * b[1] - b[0] * a[1])
            .sum::<f64>()
            / 2.0
    }

    #[test]
    fn loops_lines_and_polylines_are_clipped() {
        let r = Rect {
            lo: [0.0, 0.0],
            hi: [10.0, 10.0],
        };
        // A square half outside: half its area kept.
        let sq = [[5.0, 2.0], [15.0, 2.0], [15.0, 8.0], [5.0, 8.0]];
        assert!((area(&r.polygon(&sq).unwrap()) - 30.0).abs() < 1e-9);
        // Wholly outside: nothing.
        assert!(
            r.polygon(&[[20.0, 0.0], [30.0, 0.0], [30.0, 5.0]])
                .is_none()
        );
        // A segment across, one outside, one inside.
        assert_eq!(
            r.segment([[-5.0, 5.0], [15.0, 5.0]]),
            Some([[0.0, 5.0], [10.0, 5.0]])
        );
        assert_eq!(r.segment([[-5.0, -5.0], [-1.0, 20.0]]), None);
        assert_eq!(
            r.segment([[1.0, 1.0], [2.0, 2.0]]),
            Some([[1.0, 1.0], [2.0, 2.0]])
        );
        // A polyline leaving and coming back: two pieces.
        let pl = r.polyline(&[[2.0, 5.0], [12.0, 5.0], [12.0, 7.0], [2.0, 7.0]]);
        assert_eq!(pl.len(), 2, "{pl:?}");
    }
}
