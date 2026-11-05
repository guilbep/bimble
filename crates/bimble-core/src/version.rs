//! Versioning primitives: canonical hashing and element-level semantic diff.
//! Foundation for commits, branches and merges ("GitHub for building models").

use crate::{GlobalId, Model};
use serde::Serialize;
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};

/// Canonical copy of a value: every `-0.0` becomes `0.0`. Both compare equal but serialize
/// differently, so without this two equal models could hash differently.
///
/// Validate first: values must be finite (JSON cannot hold NaN or infinity).
pub fn canonical<T: Serialize + serde::de::DeserializeOwned>(v: T) -> T {
    fn walk(v: &mut Value) {
        match v {
            Value::Number(n) if n.is_f64() && n.as_f64() == Some(0.0) => *v = Value::from(0.0),
            Value::Array(a) => a.iter_mut().for_each(walk),
            Value::Object(o) => o.values_mut().for_each(walk),
            _ => {}
        }
    }
    let mut j = serde_json::to_value(&v).expect("model types always serialize");
    walk(&mut j);
    serde_json::from_value(j).expect("canonical form deserializes to the same type")
}

/// Hash of any serializable object's canonical JSON form.
pub fn content_hash<T: Serialize>(v: &T) -> blake3::Hash {
    blake3::hash(&serde_json::to_vec(v).expect("model types always serialize"))
}

/// Model hash: depends on content only, not on insertion order or machine.
pub fn model_hash(m: &Model) -> blake3::Hash {
    let mut h = blake3::Hasher::new();
    h.update(content_hash(&(&m.project_id, &m.project_name)).as_bytes());
    for (tag, map) in [
        ("storeys", hash_map(&m.storeys)),
        ("materials", hash_map(&m.materials)),
        ("elements", hash_map(&m.elements)),
    ] {
        h.update(tag.as_bytes());
        for (id, eh) in map {
            h.update(id.as_str().as_bytes());
            h.update(eh.as_bytes());
        }
    }
    h.finalize()
}

fn hash_map<T: Serialize>(m: &BTreeMap<GlobalId, T>) -> Vec<(GlobalId, blake3::Hash)> {
    m.iter().map(|(k, v)| (*k, content_hash(v))).collect()
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct FieldChange {
    /// Dotted path, e.g. "generator.height" or "placement.origin.2"
    pub path: String,
    pub before: Value,
    pub after: Value,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(tag = "change", rename_all = "snake_case")]
pub enum Change {
    Added {
        kind: String,
        id: GlobalId,
        name: String,
    },
    Removed {
        kind: String,
        id: GlobalId,
        name: String,
    },
    Modified {
        kind: String,
        id: GlobalId,
        name: String,
        fields: Vec<FieldChange>,
    },
}

/// Element-level diff between two models. Matching is by GlobalId, never by position.
pub fn diff(a: &Model, b: &Model) -> Vec<Change> {
    let mut out = Vec::new();
    if a.project_name != b.project_name {
        out.push(Change::Modified {
            kind: "IfcProject".into(),
            id: b.project_id,
            name: b.project_name.clone(),
            fields: vec![FieldChange {
                path: "project_name".into(),
                before: a.project_name.clone().into(),
                after: b.project_name.clone().into(),
            }],
        });
    }
    diff_map(
        &a.storeys,
        &b.storeys,
        |_| "IfcBuildingStorey".into(),
        |s| s.name.clone(),
        &mut out,
    );
    diff_map(
        &a.materials,
        &b.materials,
        |_| "IfcMaterial".into(),
        |m| m.name.clone(),
        &mut out,
    );
    diff_map(
        &a.elements,
        &b.elements,
        |e| e.class.name().into(),
        |e| e.name.clone(),
        &mut out,
    );
    out
}

fn diff_map<T: Serialize + PartialEq>(
    a: &BTreeMap<GlobalId, T>,
    b: &BTreeMap<GlobalId, T>,
    kind: impl Fn(&T) -> String,
    name: impl Fn(&T) -> String,
    out: &mut Vec<Change>,
) {
    let keys: BTreeSet<_> = a.keys().chain(b.keys()).collect();
    for k in keys {
        match (a.get(k), b.get(k)) {
            (Some(x), None) => out.push(Change::Removed {
                kind: kind(x),
                id: *k,
                name: name(x),
            }),
            (None, Some(y)) => out.push(Change::Added {
                kind: kind(y),
                id: *k,
                name: name(y),
            }),
            (Some(x), Some(y)) if x != y => {
                let mut fields = Vec::new();
                let (vx, vy) = (
                    serde_json::to_value(x).unwrap(),
                    serde_json::to_value(y).unwrap(),
                );
                diff_value("", &vx, &vy, &mut fields);
                out.push(Change::Modified {
                    kind: kind(y),
                    id: *k,
                    name: name(y),
                    fields,
                });
            }
            _ => {}
        }
    }
}

fn diff_value(path: &str, a: &Value, b: &Value, out: &mut Vec<FieldChange>) {
    if a == b {
        return;
    }
    let join = |k: &str| {
        if path.is_empty() {
            k.to_owned()
        } else {
            format!("{path}.{k}")
        }
    };
    match (a, b) {
        (Value::Object(x), Value::Object(y)) if x.get("kind") == y.get("kind") => {
            let keys: BTreeSet<_> = x.keys().chain(y.keys()).collect();
            for k in keys {
                let null = Value::Null;
                diff_value(
                    &join(k),
                    x.get(k).unwrap_or(&null),
                    y.get(k).unwrap_or(&null),
                    out,
                );
            }
        }
        (Value::Array(x), Value::Array(y)) if x.len() == y.len() => {
            for (i, (u, v)) in x.iter().zip(y).enumerate() {
                diff_value(&join(&i.to_string()), u, v, out);
            }
        }
        _ => out.push(FieldChange {
            path: path.to_owned(),
            before: a.clone(),
            after: b.clone(),
        }),
    }
}

impl std::fmt::Display for Change {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Change::Added { kind, id, name } => write!(f, "+ {kind} {name} [{id}]"),
            Change::Removed { kind, id, name } => write!(f, "- {kind} {name} [{id}]"),
            Change::Modified {
                kind,
                id,
                name,
                fields,
            } => {
                write!(f, "~ {kind} {name} [{id}]")?;
                for c in fields {
                    write!(f, "\n    {}: {} → {}", c.path, c.before, c.after)?;
                }
                Ok(())
            }
        }
    }
}
