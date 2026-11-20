//! `bimble mcp`: the binary spoken to over stdio, as an MCP client would.
//! Fixture: the demo frame (synthetic), saved to a temporary file.

use bimble_core::{demo, file};
use serde_json::{Value, json};
use std::io::Write;
use std::path::Path;
use std::process::{Command, Stdio};

/// Write the lines to `bimble mcp`, close stdin, return the replies by id (null id: "null").
fn session(model: &Path, lines: &[String]) -> std::collections::BTreeMap<String, Value> {
    let mut child = Command::new(env!("CARGO_BIN_EXE_bimble"))
        .arg("mcp")
        .arg(model)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()
        .unwrap();
    let mut stdin = child.stdin.take().unwrap();
    for l in lines {
        writeln!(stdin, "{l}").unwrap();
    }
    drop(stdin);
    let out = child.wait_with_output().unwrap();
    assert!(out.status.success());
    String::from_utf8(out.stdout)
        .unwrap()
        .lines()
        .map(|l| {
            let v: Value = serde_json::from_str(l).unwrap();
            assert_eq!(v["jsonrpc"], "2.0");
            (v["id"].to_string(), v)
        })
        .collect()
}

fn call(id: u64, name: &str, arguments: Value) -> String {
    json!({"jsonrpc": "2.0", "id": id, "method": "tools/call",
           "params": {"name": name, "arguments": arguments}})
    .to_string()
}

fn text(r: &Value, is_error: bool) -> &str {
    assert_eq!(r["result"]["isError"], is_error, "{r}");
    r["result"]["content"][0]["text"].as_str().unwrap()
}

#[test]
fn mcp_reads_edits_and_saves_a_model() {
    let dir = std::env::temp_dir().join(format!("bimble-mcp-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("frame.bimble");
    file::save(&demo::concrete_frame(&Default::default()), &path).unwrap();
    let before = file::load(&path).unwrap();
    let n = before.elements.len();

    // 1. Handshake, tools, a query, protocol errors.
    let r = session(
        &path,
        &[
            json!({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                   "params": {"protocolVersion": "2025-03-26"}})
            .to_string(),
            json!({"jsonrpc": "2.0", "method": "notifications/initialized"}).to_string(),
            json!({"jsonrpc": "2.0", "id": 2, "method": "tools/list"}).to_string(),
            call(3, "query", json!({"name": "storeys"})),
            json!({"jsonrpc": "2.0", "id": 4, "method": "no/such"}).to_string(),
            "{not json".into(),
            "x".repeat(2 << 20),
        ],
    );
    // 1, 2, 3, 4 and two errors with a null id (the second replaces the first in the map).
    assert_eq!(r["1"]["result"]["protocolVersion"], "2025-03-26");
    assert_eq!(r["1"]["result"]["serverInfo"]["name"], "bimble");
    let names: Vec<_> = r["2"]["result"]["tools"]
        .as_array()
        .unwrap()
        .iter()
        .map(|t| t["name"].as_str().unwrap())
        .collect();
    assert_eq!(
        names,
        [
            "list_verbs",
            "describe_verb",
            "run_verb",
            "query",
            "undo",
            "redo",
            "save"
        ]
    );
    let storeys: Value = serde_json::from_str(text(&r["3"], false)).unwrap();
    let storeys = storeys.as_array().unwrap();
    assert_eq!(storeys.len(), before.storeys.len());
    assert_eq!(r["4"]["error"]["code"], -32601);
    assert_eq!(r["null"]["error"]["code"], -32600); // the long line, last

    // 2. Edits: a column, a refused one (out of bounds), save.
    let storey = storeys[1]["id"].clone();
    let place = |id, a: [f64; 2]| {
        call(
            id,
            "run_verb",
            json!({"name": "place", "args": {"kind": "column", "storey": storey, "a": a}}),
        )
    };
    let r = session(
        &path,
        &[
            place(5, [1000.0, 1000.0]),
            place(6, [1.0e9, 0.0]),
            call(7, "save", json!({})),
            "{not json".into(),
        ],
    );
    let done: Value = serde_json::from_str(text(&r["5"], false)).unwrap();
    assert_eq!(done["added"].as_array().unwrap().len(), 1);
    assert!(text(&r["6"], true).contains("finite value"));
    assert_eq!(
        text(&r["7"], false),
        format!("saved {} elements to frame.bimble", n + 1)
    );
    assert_eq!(r["null"]["error"]["code"], -32700);
    assert_eq!(file::load(&path).unwrap().elements.len(), n + 1);

    // 3. Undo then save: back to the original count; a second undo has nothing to undo.
    let r = session(
        &path,
        &[
            call(8, "undo", json!({})),
            place(9, [2000.0, 2000.0]),
            call(10, "undo", json!({})),
            call(11, "undo", json!({})),
            call(12, "redo", json!({})),
            call(13, "save", json!({})),
        ],
    );
    assert_eq!(text(&r["8"], false), "nothing to undo");
    assert!(text(&r["10"], false).starts_with("undo: "));
    assert_eq!(text(&r["11"], false), "nothing to undo");
    assert!(text(&r["12"], false).starts_with("redo: "));
    assert_eq!(file::load(&path).unwrap().elements.len(), n + 2);

    std::fs::remove_dir_all(&dir).unwrap();
}
