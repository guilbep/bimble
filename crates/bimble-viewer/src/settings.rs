//! The user's settings, kept between sessions: a JSON file in the platform's settings folder on
//! the desktop, the browser's local storage on the web. Unknown or broken values fall back to
//! the defaults; nothing here can stop Bimble from starting.

/// What the user set.
#[derive(Clone, Debug, PartialEq)]
pub struct Settings {
    /// Share of the machine's memory one model may take (ADR 0033), 0.2–1.0. The browser
    /// ignores it: a tab gets all it can address (4 GB).
    pub memory_share: f64,
}

impl Default for Settings {
    fn default() -> Self {
        Settings { memory_share: 0.8 }
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
        }
    }

    pub fn to_json(&self) -> String {
        serde_json::json!({ "memory_share": self.memory_share }).to_string()
    }

    pub fn load() -> Settings {
        Settings::from_json(&read().unwrap_or_default())
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
        let s = Settings { memory_share: 0.5 };
        assert_eq!(Settings::from_json(&s.to_json()), s);
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
