//! `bimble mcp model.bimble`: an MCP server over stdio (JSON-RPC 2.0, one message per line),
//! so an LLM agent can read and edit a model (ADR 0048). The agent is untrusted input: edits go
//! through `bimble_api::dispatch` (bounded arguments) and `History` (validated, undoable).
//! Nothing is executed; nothing is written until the `save` tool. Logs go to stderr only.

use bimble_api::dispatch;
use bimble_core::{Command, History, Model, file};
use serde_json::{Value, json};
use std::io::{BufRead, Write};
use std::path::Path;

/// Longest line read; a longer one is refused without being parsed.
const MAX_LINE: usize = 1 << 20;

pub fn run(path: &Path) -> Result<(), String> {
    let mut model = file::load(path).map_err(|e| e.to_string())?;
    let mut history = History::default();
    let stdin = std::io::stdin();
    let mut input = stdin.lock();
    let mut out = std::io::stdout().lock();
    log::info!("mcp: serving {}", path.display());
    while let Some(line) = read_line(&mut input).map_err(|e| e.to_string())? {
        let reply = match line {
            Err(n) => Some(error(
                Value::Null,
                -32600,
                &format!("line of {n}+ bytes: at most {MAX_LINE}"),
            )),
            Ok(l) if l.iter().all(u8::is_ascii_whitespace) => None,
            Ok(l) => match serde_json::from_slice::<Value>(&l) {
                Err(e) => Some(error(Value::Null, -32700, &format!("parse error: {e}"))),
                Ok(msg) => handle(&msg, &mut model, &mut history, path),
            },
        };
        if let Some(r) = reply {
            writeln!(out, "{r}")
                .and_then(|_| out.flush())
                .map_err(|e| e.to_string())?;
        }
    }
    Ok(())
}

/// One line without its newline, or `Err(length)` when longer than `MAX_LINE` (the rest of
/// that line is read and dropped, never kept). `None` at the end of input.
fn read_line(r: &mut impl BufRead) -> std::io::Result<Option<Result<Vec<u8>, usize>>> {
    let mut buf = Vec::new();
    let mut len = 0;
    loop {
        let chunk = r.fill_buf()?;
        if chunk.is_empty() {
            return Ok((len > 0).then_some(if len > MAX_LINE { Err(len) } else { Ok(buf) }));
        }
        let (part, done) = match chunk.iter().position(|&b| b == b'\n') {
            Some(i) => (&chunk[..i], Some(i + 1)),
            None => (chunk, None),
        };
        len += part.len();
        if len <= MAX_LINE {
            buf.extend_from_slice(part);
        } else {
            buf = Vec::new();
        }
        let used = done.unwrap_or(chunk.len());
        r.consume(used);
        if done.is_some() {
            return Ok(Some(if len > MAX_LINE { Err(len) } else { Ok(buf) }));
        }
    }
}

fn error(id: Value, code: i64, message: &str) -> Value {
    json!({"jsonrpc": "2.0", "id": id, "error": {"code": code, "message": message}})
}

/// The reply to one message; None for a notification (no id).
fn handle(msg: &Value, model: &mut Model, history: &mut History, path: &Path) -> Option<Value> {
    let id = msg.get("id")?.clone();
    let params = msg.get("params").cloned().unwrap_or(Value::Null);
    let result = match msg.get("method").and_then(Value::as_str) {
        Some("initialize") => json!({
            "protocolVersion": params.get("protocolVersion").and_then(Value::as_str)
                .unwrap_or("2025-06-18"),
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "bimble", "version": env!("CARGO_PKG_VERSION")},
        }),
        Some("ping") => json!({}),
        Some("tools/list") => json!({"tools": tools()}),
        Some("tools/call") => {
            let name = params.get("name").and_then(Value::as_str).unwrap_or("");
            let args = params.get("arguments").cloned().unwrap_or(json!({}));
            let (text, is_error) = match call(name, args, model, history, path) {
                Ok(t) => (t, false),
                Err(t) => (t, true),
            };
            json!({"content": [{"type": "text", "text": text}], "isError": is_error})
        }
        Some(m) => return Some(error(id, -32601, &format!("method not found: {m}"))),
        None => return Some(error(id, -32600, "no method")),
    };
    Some(json!({"jsonrpc": "2.0", "id": id, "result": result}))
}

fn tools() -> Value {
    let none = json!({"type": "object", "properties": {}, "additionalProperties": false});
    let named = |what: &str| {
        json!({
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": what},
                "args": {"type": "object", "description": "The arguments, as `describe_verb` gives for a verb"},
            },
            "required": ["name"],
            "additionalProperties": false,
        })
    };
    json!([
        {"name": "list_verbs", "description": "The verbs that edit the model (place, move, copy, delete), with a summary each.",
         "inputSchema": none},
        {"name": "describe_verb", "description": "JSON schema of a verb's arguments.",
         "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}},
                         "required": ["name"], "additionalProperties": false}},
        {"name": "run_verb", "description": "Run a verb on the model (one undoable step). Lengths in mm, storey-relative. Not saved until `save`.",
         "inputSchema": named("Verb name, from list_verbs")},
        {"name": "query", "description": "Read the model: storeys, materials, elements (filter), element ({\"id\"}).",
         "inputSchema": named("storeys, materials, elements or element")},
        {"name": "undo", "description": "Undo the last step.", "inputSchema": none},
        {"name": "redo", "description": "Redo the last undone step.", "inputSchema": none},
        {"name": "save", "description": "Write the model to its file.", "inputSchema": none},
    ])
}

/// Arguments of a tool: an object with only the `allowed` keys.
fn fields<'a>(
    args: &'a Value,
    allowed: &[&str],
) -> Result<&'a serde_json::Map<String, Value>, String> {
    let o = args.as_object().ok_or("arguments must be an object")?;
    match o.keys().find(|k| !allowed.contains(&k.as_str())) {
        Some(k) => Err(format!("unknown argument {k:?}")),
        None => Ok(o),
    }
}

fn name_arg(o: &serde_json::Map<String, Value>) -> Result<&str, String> {
    o.get("name")
        .and_then(Value::as_str)
        .ok_or_else(|| "name (a string) is required".into())
}

fn call(
    tool: &str,
    args: Value,
    model: &mut Model,
    history: &mut History,
    path: &Path,
) -> Result<String, String> {
    let json = |v: &Value| v.to_string();
    match tool {
        "list_verbs" => {
            fields(&args, &[])?;
            Ok(json(&json!(dispatch::list())))
        }
        "describe_verb" => {
            let name = name_arg(fields(&args, &["name"])?)?;
            dispatch::describe(name)
                .map(|v| json(&v))
                .ok_or_else(|| format!("unknown verb {name:?}"))
        }
        "run_verb" => {
            let o = fields(&args, &["name", "args"])?;
            let name = name_arg(o)?;
            let cmds = dispatch::run(model, name, o.get("args").cloned().unwrap_or(Value::Null))?;
            let n = cmds.len();
            let added: Vec<String> = cmds
                .iter()
                .filter_map(|c| match c {
                    Command::AddElement(e) => Some(e.id.to_string()),
                    _ => None,
                })
                .collect();
            history.apply(model, cmds).map_err(|e| e.to_string())?;
            Ok(json(&json!({"commands": n, "added": added})))
        }
        "query" => {
            let o = fields(&args, &["name", "args"])?;
            let v = dispatch::query(
                model,
                name_arg(o)?,
                o.get("args").cloned().unwrap_or(Value::Null),
            )?;
            Ok(json(&v))
        }
        "undo" | "redo" => {
            fields(&args, &[])?;
            let label = if tool == "undo" {
                history.undo_label()
            } else {
                history.redo_label()
            };
            let Some(label) = label.map(str::to_owned) else {
                return Ok(format!("nothing to {tool}"));
            };
            let done = if tool == "undo" {
                history.undo(model)
            } else {
                history.redo(model)
            };
            Ok(if done {
                format!("{tool}: {label}")
            } else {
                format!("nothing to {tool}")
            })
        }
        "save" => {
            fields(&args, &[])?;
            file::save(model, path).map_err(|e| e.to_string())?;
            let name = path
                .file_name()
                .map(|n| n.to_string_lossy())
                .unwrap_or_default();
            Ok(format!("saved {} elements to {name}", model.elements.len()))
        }
        _ => Err(format!(
            "unknown tool {tool:?}; known: list_verbs, describe_verb, run_verb, query, undo, redo, save"
        )),
    }
}
