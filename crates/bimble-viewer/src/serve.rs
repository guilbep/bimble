//! Dev-only "native twin" transport: a tiny std-only HTTP/1.1 server on 127.0.0.1.
//!
//! The lab (`web-ui/src/bimble.ts`, `nativeClient`) sends `POST /<op>/<name>?t=<token>` and
//! long-polls `GET /change?since=<n>&t=<token>`. This module knows nothing about the viewer:
//! it calls a [`Handler`]. Input is untrusted: bounded reads, no panics, token, Origin and
//! Host checks before anything else.

use std::io::{self, BufRead, BufReader, Read, Write};
use std::net::{Ipv4Addr, SocketAddr, TcpListener, TcpStream};
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Duration;

const MAX_BODY: usize = 1 << 20;
const MAX_HEADER: usize = 16 << 10;
const MAX_CONNECTIONS: usize = 32;
const IDLE_TIMEOUT: Duration = Duration::from_secs(30);
const POLL_TIMEOUT: Duration = Duration::from_secs(25);

pub(crate) struct Request {
    pub op: String,
    pub name: String,
    pub body: String,
}

pub(crate) trait Handler: Send + Sync + 'static {
    /// One call; runs on a server thread. Ok = 200 with the text; Err = 400 with the text.
    fn call(&self, req: Request) -> Result<String, String>;
    /// Long-poll: block until seq > since or timeout; return (seq, change JSON or "null").
    fn wait_change(&self, since: u64, timeout: Duration) -> (u64, String);
    /// A per-process id sent with each change: a client that sees it change (the server
    /// restarted, seq began again) resets `since` to 0. None: the field is left out.
    fn epoch(&self) -> Option<&str> {
        None
    }
}

pub(crate) struct Config {
    pub port: u16,
    pub token: String,
    pub allowed_origins: Vec<String>,
}

/// Binds 127.0.0.1:port (IPv4), spawns an accept thread, a thread per connection; returns the bound port.
pub(crate) fn start(cfg: Config, h: Arc<dyn Handler>) -> io::Result<u16> {
    let listener = TcpListener::bind(SocketAddr::from((Ipv4Addr::LOCALHOST, cfg.port)))?;
    let port = listener.local_addr()?.port();
    let cfg = Arc::new(cfg);
    let live = Arc::new(AtomicUsize::new(0));
    std::thread::Builder::new()
        .name("bimble-serve".into())
        .spawn(move || {
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { continue };
                if live.fetch_add(1, Ordering::SeqCst) >= MAX_CONNECTIONS {
                    live.fetch_sub(1, Ordering::SeqCst);
                    let _ = respond(
                        &mut stream,
                        503,
                        "",
                        "text/plain; charset=utf-8",
                        "busy",
                        false,
                    );
                    continue;
                }
                let (cfg, h, done) = (cfg.clone(), h.clone(), live.clone());
                let spawned = std::thread::Builder::new()
                    .name("bimble-serve-conn".into())
                    .spawn(move || {
                        let _ = serve_connection(stream, port, &cfg, h.as_ref());
                        done.fetch_sub(1, Ordering::SeqCst);
                    });
                if spawned.is_err() {
                    live.fetch_sub(1, Ordering::SeqCst);
                }
            }
        })?;
    Ok(port)
}

struct Head {
    method: String,
    target: String,
    origin: Option<String>,
    host: Option<String>,
    content_length: Option<usize>,
    close: bool,
}

enum ReadErr {
    Eof,
    Io,
    TooLarge,
    Malformed,
}

/// Reads one header block (request line + headers), bounded by `MAX_HEADER`.
fn read_head(r: &mut BufReader<TcpStream>) -> Result<Head, ReadErr> {
    let mut total = 0usize;
    let mut lines: Vec<String> = Vec::new();
    loop {
        let mut buf = Vec::new();
        let limit = (MAX_HEADER + 1 - total.min(MAX_HEADER)) as u64;
        let n = r
            .by_ref()
            .take(limit)
            .read_until(b'\n', &mut buf)
            .map_err(|_| ReadErr::Io)?;
        if n == 0 {
            return Err(if lines.is_empty() && total == 0 {
                ReadErr::Eof
            } else {
                ReadErr::Malformed
            });
        }
        total += n;
        if total > MAX_HEADER {
            return Err(ReadErr::TooLarge);
        }
        if buf.last() != Some(&b'\n') {
            return Err(ReadErr::Malformed);
        }
        let line = String::from_utf8(buf).map_err(|_| ReadErr::Malformed)?;
        let line = line.trim_end_matches(['\r', '\n']);
        if line.is_empty() {
            if lines.is_empty() {
                continue; // tolerate a stray CRLF between requests
            }
            break;
        }
        lines.push(line.to_string());
    }
    let mut parts = lines[0].split(' ');
    let (Some(method), Some(target), Some(version), None) =
        (parts.next(), parts.next(), parts.next(), parts.next())
    else {
        return Err(ReadErr::Malformed);
    };
    if !version.starts_with("HTTP/1.") || method.is_empty() || !target.starts_with('/') {
        return Err(ReadErr::Malformed);
    }
    let mut head = Head {
        method: method.to_string(),
        target: target.to_string(),
        origin: None,
        host: None,
        content_length: None,
        close: version == "HTTP/1.0",
    };
    for line in &lines[1..] {
        let Some((k, v)) = line.split_once(':') else {
            return Err(ReadErr::Malformed);
        };
        let v = v.trim();
        match k.trim().to_ascii_lowercase().as_str() {
            "origin" => head.origin = Some(v.to_string()),
            "host" => head.host = Some(v.to_string()),
            "content-length" => {
                let n = v.parse::<usize>().map_err(|_| ReadErr::Malformed)?;
                if head.content_length.is_some_and(|m| m != n) {
                    return Err(ReadErr::Malformed);
                }
                head.content_length = Some(n);
            }
            "transfer-encoding" => return Err(ReadErr::Malformed),
            "connection" => {
                let v = v.to_ascii_lowercase();
                if v.contains("close") {
                    head.close = true;
                } else if v.contains("keep-alive") {
                    head.close = false;
                }
            }
            _ => {}
        }
    }
    Ok(head)
}

fn serve_connection(stream: TcpStream, port: u16, cfg: &Config, h: &dyn Handler) -> io::Result<()> {
    stream.set_nodelay(true)?;
    stream.set_read_timeout(Some(IDLE_TIMEOUT))?;
    stream.set_write_timeout(Some(IDLE_TIMEOUT))?;
    let mut out = stream.try_clone()?;
    let mut reader = BufReader::new(stream);
    const TEXT: &str = "text/plain; charset=utf-8";
    loop {
        let head = match read_head(&mut reader) {
            Ok(head) => head,
            Err(ReadErr::Eof | ReadErr::Io) => return Ok(()),
            Err(ReadErr::TooLarge) => {
                return respond(&mut out, 431, "", TEXT, "header block too large", false);
            }
            Err(ReadErr::Malformed) => {
                return respond(&mut out, 400, "", TEXT, "malformed request", false);
            }
        };
        let origin_ok = head
            .origin
            .as_ref()
            .is_none_or(|o| cfg.allowed_origins.iter().any(|a| a == o));
        let acao = if origin_ok {
            head.origin.clone().unwrap_or_default()
        } else {
            String::new()
        };
        let (path, query) = head.target.split_once('?').unwrap_or((&head.target, ""));
        let token_ok = query_param(query, "t").is_some_and(|t| t == cfg.token);
        let host_ok = head.host.as_deref().is_some_and(|host| {
            host == format!("127.0.0.1:{port}") || host == format!("localhost:{port}")
        });
        let is_options = head.method == "OPTIONS";
        if !origin_ok || !host_ok || (!token_ok && !is_options) {
            return respond(&mut out, 403, &acao, TEXT, "forbidden", false);
        }
        let len = head.content_length.unwrap_or(0);
        if len > MAX_BODY {
            return respond(&mut out, 413, &acao, TEXT, "body too large", false);
        }
        let mut body = vec![0u8; len];
        if reader.read_exact(&mut body).is_err() {
            return Ok(());
        }
        let keep = !head.close;
        if is_options {
            respond(&mut out, 204, &acao, TEXT, "", keep)?;
        } else if head.method == "GET" && path == "/change" {
            let Some(since) = query_param(query, "since").and_then(|s| s.parse::<u64>().ok())
            else {
                return respond(&mut out, 400, &acao, TEXT, "bad since", false);
            };
            let (seq, change) = h.wait_change(since, POLL_TIMEOUT);
            let json = match h.epoch() {
                Some(e) => format!("{{\"epoch\":\"{e}\",\"seq\":{seq},\"change\":{change}}}"),
                None => format!("{{\"seq\":{seq},\"change\":{change}}}"),
            };
            respond(&mut out, 200, &acao, "application/json", &json, keep)?;
        } else if head.method == "POST" {
            let mut segs = path.trim_start_matches('/').split('/');
            let (Some(op), Some(name), None) = (segs.next(), segs.next(), segs.next()) else {
                respond(&mut out, 404, &acao, TEXT, "not found", keep)?;
                continue;
            };
            let name = if name == "-" {
                Some(String::new())
            } else {
                percent_decode(name)
            };
            let (Some(op), Some(name), Ok(body)) =
                (percent_decode(op), name, String::from_utf8(body))
            else {
                return respond(&mut out, 400, &acao, TEXT, "malformed request", false);
            };
            match h.call(Request { op, name, body }) {
                Ok(text) => respond(&mut out, 200, &acao, TEXT, &text, keep)?,
                Err(text) => respond(&mut out, 400, &acao, TEXT, &text, keep)?,
            }
        } else {
            respond(&mut out, 404, &acao, TEXT, "not found", keep)?;
        }
        if !keep {
            return Ok(());
        }
    }
}

fn respond(
    out: &mut TcpStream,
    status: u16,
    acao: &str,
    content_type: &str,
    body: &str,
    keep: bool,
) -> io::Result<()> {
    let reason = match status {
        200 => "OK",
        204 => "No Content",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        413 => "Payload Too Large",
        431 => "Request Header Fields Too Large",
        _ => "Service Unavailable",
    };
    let mut head = format!("HTTP/1.1 {status} {reason}\r\n");
    if !acao.is_empty() {
        head.push_str(&format!(
            "Access-Control-Allow-Origin: {acao}\r\nVary: Origin\r\n"
        ));
    }
    if status == 204 {
        head.push_str(
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n\
             Access-Control-Allow-Headers: Content-Type\r\n\
             Access-Control-Max-Age: 600\r\n",
        );
    }
    let conn = if keep { "keep-alive" } else { "close" };
    head.push_str(&format!(
        "Content-Type: {content_type}\r\nContent-Length: {}\r\nConnection: {conn}\r\n\r\n",
        body.len()
    ));
    let mut buf = head.into_bytes();
    buf.extend_from_slice(body.as_bytes());
    out.write_all(&buf)?;
    out.flush()
}

fn query_param<'a>(query: &'a str, key: &str) -> Option<&'a str> {
    query
        .split('&')
        .filter_map(|kv| kv.split_once('='))
        .find(|(k, _)| *k == key)
        .map(|(_, v)| v)
}

/// Percent-decodes a path segment; `None` on a bad escape or invalid UTF-8.
fn percent_decode(s: &str) -> Option<String> {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' {
            let hex = s.get(i + 1..i + 3)?;
            out.push(u8::from_str_radix(hex, 16).ok()?);
            i += 3;
        } else {
            out.push(b[i]);
            i += 1;
        }
    }
    String::from_utf8(out).ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    struct TestHandler;

    impl Handler for TestHandler {
        fn call(&self, req: Request) -> Result<String, String> {
            if req.op == "fail" {
                return Err(format!("failed: {}", req.name));
            }
            Ok(format!("{}|{}|{}", req.op, req.name, req.body))
        }
        fn wait_change(&self, since: u64, _timeout: Duration) -> (u64, String) {
            (since + 1, "{\"x\":1}".to_string())
        }
    }

    const ORIGIN: &str = "http://localhost:5173";

    fn server() -> u16 {
        let cfg = Config {
            port: 0,
            token: "T".into(),
            allowed_origins: vec![ORIGIN.into()],
        };
        start(cfg, Arc::new(TestHandler)).expect("start")
    }

    fn connect(port: u16) -> BufReader<TcpStream> {
        let s = TcpStream::connect(("127.0.0.1", port)).expect("connect");
        s.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
        BufReader::new(s)
    }

    fn send(r: &mut BufReader<TcpStream>, raw: &str) {
        r.get_mut().write_all(raw.as_bytes()).expect("write");
    }

    /// Reads one response; returns (status, body).
    fn read_response(r: &mut BufReader<TcpStream>) -> (u16, String) {
        let mut line = String::new();
        r.read_line(&mut line).expect("status line");
        let status: u16 = line.split(' ').nth(1).expect("status").parse().unwrap();
        let mut len = 0usize;
        loop {
            let mut h = String::new();
            r.read_line(&mut h).expect("header");
            let h = h.trim_end();
            if h.is_empty() {
                break;
            }
            if let Some((k, v)) = h.split_once(':')
                && k.eq_ignore_ascii_case("content-length")
            {
                len = v.trim().parse().unwrap();
            }
        }
        let mut body = vec![0u8; len];
        r.read_exact(&mut body).expect("body");
        (status, String::from_utf8(body).unwrap())
    }

    fn post(port: u16, path: &str, body: &str, extra: &str) -> String {
        format!(
            "POST {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n{extra}Content-Type: text/plain\r\nContent-Length: {}\r\n\r\n{body}",
            body.len()
        )
    }

    fn one(port: u16, raw: &str) -> (u16, String) {
        let mut c = connect(port);
        send(&mut c, raw);
        read_response(&mut c)
    }

    #[test]
    fn no_token_is_403() {
        let port = server();
        assert_eq!(one(port, &post(port, "/verb/place", "{}", "")).0, 403);
    }

    #[test]
    fn foreign_origin_is_403() {
        let port = server();
        let raw = post(
            port,
            "/verb/place?t=T",
            "{}",
            "Origin: http://evil.example\r\n",
        );
        assert_eq!(one(port, &raw).0, 403);
    }

    #[test]
    fn wrong_host_is_403() {
        let port = server();
        let raw = format!(
            "POST /verb/place?t=T HTTP/1.1\r\nHost: evil.example:{port}\r\nContent-Length: 2\r\n\r\n{{}}"
        );
        assert_eq!(one(port, &raw).0, 403);
    }

    #[test]
    fn good_call_is_200_with_body() {
        let port = server();
        let raw = post(
            port,
            "/verb/place?t=T",
            "{}",
            &format!("Origin: {ORIGIN}\r\n"),
        );
        assert_eq!(one(port, &raw), (200, "verb|place|{}".to_string()));
    }

    #[test]
    fn handler_err_is_400() {
        let port = server();
        let raw = post(port, "/fail/a%20b?t=T", "", "");
        assert_eq!(one(port, &raw), (400, "failed: a b".to_string()));
    }

    #[test]
    fn two_requests_one_connection() {
        let port = server();
        let mut c = connect(port);
        send(&mut c, &post(port, "/verb/place?t=T", "{}", ""));
        assert_eq!(read_response(&mut c).0, 200);
        send(&mut c, &post(port, "/query/-?t=T", "x", ""));
        assert_eq!(read_response(&mut c), (200, "query||x".to_string()));
    }

    #[test]
    fn body_over_1mib_is_413() {
        let port = server();
        let raw = format!(
            "POST /verb/place?t=T HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nContent-Length: {}\r\n\r\n",
            MAX_BODY + 1
        );
        assert_eq!(one(port, &raw).0, 413);
    }

    #[test]
    fn change_long_poll() {
        let port = server();
        let raw = format!("GET /change?since=4&t=T HTTP/1.1\r\nHost: localhost:{port}\r\n\r\n");
        assert_eq!(
            one(port, &raw),
            (200, "{\"seq\":5,\"change\":{\"x\":1}}".to_string())
        );
    }
}
