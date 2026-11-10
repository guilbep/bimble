//! The user's settings, kept between sessions: a JSON file in the platform's settings folder on
//! the desktop, the browser's local storage on the web. Unknown or broken values fall back to
//! the defaults; nothing here can stop Bimble from starting.

/// What the user set.
#[derive(Clone, Debug, PartialEq)]
pub struct Settings {
    /// Share of the machine's memory one model may take (ADR 0033), 0.2–1.0. The browser
    /// ignores it: a tab gets all it can address (4 GB).
    pub memory_share: f64,
    /// While the view moves, keep at least this frame rate by leaving out what is smallest on
    /// screen, and draw everything again once it stops (0: never leave anything out).
    pub moving_fps: u32,
    /// On a machine that misses that frame rate, draw far parts as simplified stand-ins while
    /// the view moves (ADR 0036 #9).
    pub moving_proxies: bool,
    /// Test each shared shape's copies against the view on the GPU (ADR 0036 #10; where compute
    /// shaders exist). Experimental: no gain measured yet.
    pub gpu_cull: bool,
    /// Interface theme, by id (`ui_theme::THEMES`; ADR 0041).
    pub theme: String,
    /// Interface font, by id (`ui_theme::UiFont`).
    pub ui_font: String,
    /// Body text size of the interface, points.
    pub text_size: f32,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            memory_share: 0.8,
            moving_fps: 30,
            moving_proxies: true,
            gpu_cull: false,
            theme: crate::ui_theme::THEMES[0].id.to_owned(),
            ui_font: crate::ui_theme::UiFont::Roboto.id().to_owned(),
            text_size: crate::ui_theme::TEXT_SIZE,
        }
    }
}

impl Settings {
    pub fn from_json(text: &str) -> Settings {
        let d = Settings::default();
        let v: serde_json::Value = serde_json::from_str(text).unwrap_or_default();
        Settings {
            memory_share: v["memory_share"]
                .as_f64()
                .filter(|x| x.is_finite())
                .map_or(d.memory_share, |x| x.clamp(0.2, 1.0)),
            moving_fps: v["moving_fps"]
                .as_u64()
                .map_or(d.moving_fps, |x| x.min(240) as u32),
            moving_proxies: v["moving_proxies"].as_bool().unwrap_or(d.moving_proxies),
            gpu_cull: v["gpu_cull"].as_bool().unwrap_or(d.gpu_cull),
            theme: v["theme"]
                .as_str()
                .map_or(d.theme, |id| crate::ui_theme::by_id(id).id.to_owned()),
            ui_font: v["ui_font"]
                .as_str()
                .and_then(crate::ui_theme::UiFont::from_id)
                .map_or(d.ui_font, |f| f.id().to_owned()),
            text_size: v["text_size"]
                .as_f64()
                .filter(|x| x.is_finite())
                .map_or(d.text_size, |x| {
                    let r = crate::ui_theme::TEXT_SIZES;
                    (x as f32).clamp(*r.start(), *r.end())
                }),
        }
    }

    pub fn to_json(&self) -> String {
        serde_json::json!({
            "memory_share": self.memory_share,
            "moving_fps": self.moving_fps,
            "moving_proxies": self.moving_proxies,
            "gpu_cull": self.gpu_cull,
            "theme": self.theme,
            "ui_font": self.ui_font,
            "text_size": self.text_size,
        })
        .to_string()
    }

    pub fn load() -> Settings {
        Settings::from_json(&read().unwrap_or_default())
    }

    /// Give the interface this theme, font and text size.
    pub fn apply_look(&self, ctx: &eframe::egui::Context) {
        crate::ui_theme::apply(
            ctx,
            crate::ui_theme::by_id(&self.theme),
            crate::ui_theme::UiFont::from_id(&self.ui_font)
                .unwrap_or(crate::ui_theme::UiFont::Roboto),
            self.text_size,
        );
    }

    pub fn save(&self) {
        write(&self.to_json());
    }
}

#[cfg(not(target_arch = "wasm32"))]
fn path() -> Option<std::path::PathBuf> {
    let env = |k| std::env::var_os(k).map(std::path::PathBuf::from);
    let dir = if cfg!(target_os = "macos") {
        env("HOME")?.join("Library/Application Support/Bimble")
    } else if cfg!(windows) {
        env("APPDATA")?.join("Bimble")
    } else {
        env("XDG_CONFIG_HOME")
            .or_else(|| env("HOME").map(|h| h.join(".config")))?
            .join("bimble")
    };
    Some(dir.join("settings.json"))
}

#[cfg(not(target_arch = "wasm32"))]
fn read() -> Option<String> {
    std::fs::read_to_string(path()?).ok()
}

#[cfg(not(target_arch = "wasm32"))]
fn write(text: &str) {
    let Some(p) = path() else { return };
    if let Some(d) = p.parent() {
        let _ = std::fs::create_dir_all(d);
    }
    if let Err(e) = std::fs::write(&p, text) {
        log::warn!("settings not saved ({}): {e}", p.display());
    }
}

#[cfg(target_arch = "wasm32")]
const KEY: &str = "bimble.settings";

#[cfg(target_arch = "wasm32")]
fn read() -> Option<String> {
    web_sys::window()?
        .local_storage()
        .ok()??
        .get_item(KEY)
        .ok()?
}

#[cfg(target_arch = "wasm32")]
fn write(text: &str) {
    if let Some(s) = web_sys::window().and_then(|w| w.local_storage().ok().flatten()) {
        let _ = s.set_item(KEY, text);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn settings_round_trip_and_bad_values_fall_back() {
        let s = Settings {
            memory_share: 0.5,
            moving_fps: 60,
            moving_proxies: false,
            gpu_cull: true,
            theme: "solarized-light".into(),
            ui_font: "ubuntu".into(),
            text_size: 14.5,
        };
        assert_eq!(Settings::from_json(&s.to_json()), s);
        // An unknown theme or font, a size out of range: the defaults, the nearest size.
        let odd = Settings::from_json(r#"{"theme": "neon", "ui_font": "comic", "text_size": 99}"#);
        assert_eq!(
            (odd.theme.as_str(), odd.ui_font.as_str()),
            ("grey", "roboto")
        );
        assert_eq!(odd.text_size, 16.0);
        assert_eq!(Settings::from_json("not json"), Settings::default());
        assert_eq!(
            Settings::from_json(r#"{"memory_share": 7}"#).memory_share,
            1.0
        );
        assert_eq!(
            Settings::from_json(r#"{"memory_share": "x"}"#),
            Settings::default()
        );
    }
}
