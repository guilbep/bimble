//! Project information (ADR 0030): who and what the project is, entered once and used by every
//! title block. A list of key → value; the keys below are offered in the viewer, in sections,
//! and any other key can be added. A title block's `{key}` placeholder is filled from it.

use std::collections::BTreeMap;

/// Most entries, and their lengths.
pub const MAX_ENTRIES: usize = 200;
pub const MAX_KEY: usize = 64;
pub const MAX_VALUE: usize = 1000;

/// The keys offered, by section: (section, key, label). The key is what a title block's
/// placeholder says (`{client}`, `{project_number}`…).
pub const KNOWN: &[(&str, &str, &str)] = &[
    ("Project", "project_number", "Project number"),
    ("Project", "project_title", "Project name"),
    ("Project", "project_address", "Project address"),
    ("Project", "building", "Building"),
    ("Project", "phase", "Phase"),
    ("Project", "status", "Status"),
    ("Project", "end_date", "End date"),
    ("Client", "client", "Client"),
    ("Client", "client_address", "Address"),
    ("Client", "client_email", "E-mail"),
    ("Client", "client_phone", "Phone"),
    ("Architect", "architect", "Architect"),
    ("Architect", "architect_address", "Address"),
    ("Architect", "architect_email", "E-mail"),
    ("Architect", "architect_phone", "Phone"),
    ("Engineer", "engineer", "Engineering office"),
    ("Engineer", "author", "Author"),
    ("Engineer", "engineer_address", "Address"),
    ("Contractor", "contractor", "Contractor"),
    ("Contractor", "contractor_address", "Address"),
    ("Contractor", "contractor_email", "E-mail"),
    ("Contractor", "contractor_phone", "Phone"),
];

/// Checks project information: keys like placeholders (letters, digits, `_`), bounded sizes.
pub fn validate(p: &BTreeMap<String, String>) -> Result<(), String> {
    if p.len() > MAX_ENTRIES {
        return Err(format!(
            "project information: at most {MAX_ENTRIES} entries"
        ));
    }
    for (k, v) in p {
        let ok_key = !k.is_empty()
            && k.len() <= MAX_KEY
            && k.chars().all(|c| c.is_ascii_alphanumeric() || c == '_');
        if !ok_key {
            return Err(format!(
                "project information: {k:?} is not a key (letters, digits and _, at most {MAX_KEY})"
            ));
        }
        if v.chars().count() > MAX_VALUE {
            return Err(format!(
                "project information {k}: at most {MAX_VALUE} characters"
            ));
        }
    }
    Ok(())
}

/// A key typed by a user, made a key: "Project manager" → "project_manager".
pub fn key_of(label: &str) -> String {
    let mut k = String::new();
    for c in label.trim().chars() {
        if c.is_ascii_alphanumeric() {
            k.push(c.to_ascii_lowercase());
        } else if !k.ends_with('_') && !k.is_empty() {
            k.push('_');
        }
    }
    k.trim_end_matches('_').chars().take(MAX_KEY).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_keys_are_keys_and_once() {
        let mut keys: Vec<&str> = KNOWN.iter().map(|k| k.1).collect();
        let m: BTreeMap<String, String> =
            keys.iter().map(|k| (k.to_string(), "x".into())).collect();
        validate(&m).unwrap();
        keys.sort();
        keys.dedup();
        assert_eq!(keys.len(), KNOWN.len());
    }

    #[test]
    fn bad_keys_are_refused_and_labels_become_keys() {
        let mut m = BTreeMap::new();
        m.insert("a b".to_string(), "x".to_string());
        assert!(validate(&m).is_err());
        assert_eq!(key_of(" Project manager "), "project_manager");
        assert_eq!(key_of("N° SIA-102"), "n_sia_102");
    }
}
