#!/usr/bin/env python3
"""The IFC corpus run (ticket IFC1): every IFC file of the public corpora through the C display
reader (ifc_dump read), Rust's read_str (ccore_golden --dump-read), the C reader under ASan+UBSan
and, on a sample, IfcOpenShell. Called by bench/corpus.sh; run with `python3 -I`.

    corpus.py run   <corpus-dir> <results.jsonl> --c BIN [--rust BIN] [--asan BIN] [--only SUBSTR]
    corpus.py ios   <corpus-dir> <results.jsonl> <ios.jsonl> --python VENV_PYTHON [--n 40]
    corpus.py report <results.jsonl> [<ios.jsonl>]       markdown tables on stdout

Files are untrusted data: nothing in them is executed, each run has a timeout (120 s) and a memory
cap (systemd-run --user --scope MemoryMax, else ulimit -v), one file at a time. A run directory
(`<corpus-dir>/_runs/`) is kept only when C and Rust differ. Paths in the output are relative to
the corpus directory."""
import argparse, collections, hashlib, json, os, re, shutil, subprocess, sys, time

TIMEOUT = 120
MEM = "6G"
IFC_EXT = (".ifc", ".ifc_", ".ifczip", ".ifcxml", ".ifcjson", ".anyextension", ".zip", ".ifc.txt")
SKIP_DIRS = {".git", "venv", "_runs"}


def has_scope():
    try:
        return subprocess.run(["systemd-run", "--user", "--scope", "-q", "true"],
                              capture_output=True, timeout=30).returncode == 0
    except Exception:
        return False


SCOPE = None


def capped(cmd):
    """The command under the timeout and the memory cap."""
    global SCOPE
    if SCOPE is None:
        SCOPE = has_scope()
    pre = ["timeout", "-k", "5", str(TIMEOUT)]
    if SCOPE:
        return pre + ["systemd-run", "--user", "--scope", "-q", "-p", f"MemoryMax={MEM}",
                      "-p", "MemorySwapMax=0"] + cmd
    return ["bash", "-c", 'ulimit -v 6291456; exec "$@"', "capped"] + pre + cmd


def files_of(root):
    out = []
    for d, dirs, names in os.walk(root):
        dirs[:] = sorted(x for x in dirs if x not in SKIP_DIRS)
        for n in sorted(names):
            p = os.path.join(d, n)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            low = n.lower()
            with open(p, "rb") as f:
                head = f.read(256)
            h = head.lstrip(b"\xef\xbb\xbf \t\r\n")
            step = h.startswith(b"ISO-10303-21")
            if not step and not low.endswith(IFC_EXT):
                continue
            if h.startswith(b"version https://git-lfs"):
                fmt = "lfs-pointer"
            elif step:
                fmt = "step"
            elif h.startswith(b"PK"):
                fmt = "zip"
            elif h.startswith(b"<"):
                fmt = "xml"
            elif h.startswith(b"{") or h.startswith(b"["):
                fmt = "json"
            else:
                fmt = "other"
            if fmt in ("zip", "other") and "ifc" not in low and not low.endswith(IFC_EXT[:6]):
                continue  # a zip or text that is not named as IFC
            out.append((os.path.relpath(p, root), fmt))
    return out


SCHEMA_RE = re.compile(rb"FILE_SCHEMA\s*\(\s*\(\s*'([^']*)'", re.I)
ENT_RE = re.compile(rb"(?:^|;)\s*#\d+\s*=", re.M)


def schema_of(path):
    with open(path, "rb") as f:
        head = f.read(1 << 16)
    m = SCHEMA_RE.search(head)
    return m.group(1).decode("latin-1").strip() if m else ""


def schema_class(s):
    u = s.upper()
    if u.startswith("IFC2X3"):
        return "IFC2X3"
    if u.startswith("IFC4X3"):
        return "IFC4X3"
    if u in ("IFC4",) or re.fullmatch(r"IFC4_?(ADD\d|TC\d|_)*.*", u) and not u.startswith("IFC4X"):
        return "IFC4"
    return "other"


def count_entities(path):
    n = 0
    with open(path, "rb") as f:
        tail = b""
        while True:
            b = f.read(1 << 24)
            if not b:
                break
            chunk = tail + b
            cut = chunk.rfind(b";")
            if cut < 0:
                tail = chunk
                continue
            n += len(ENT_RE.findall(chunk[: cut + 1]))
            tail = chunk[cut + 1 :]
    return n + len(ENT_RE.findall(tail))


def run(cmd, env=None):
    t = time.monotonic()
    p = subprocess.run(capped(cmd), capture_output=True, env=env)
    return p.returncode, time.monotonic() - t, p.stderr.decode("utf-8", "replace")


def read_text(p):
    try:
        with open(p, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        return None


def outcome(rc, d, stderr):
    """(class, detail). Classes: ok, refused, crash, timeout, memory."""
    scene = read_text(os.path.join(d, "scene.txt"))
    if rc == 124:
        return "timeout", ""
    if rc in (137, -9):
        return "memory", "killed (cgroup MemoryMax)"
    if rc == 101:
        m = re.search(r"panicked at ([^\n]*)\n([^\n]*)", stderr)
        return "crash", "panic " + (m.group(1) + ": " + m.group(2) if m else stderr[-200:])
    if rc < 0 or rc > 128:
        sig = -rc if rc < 0 else rc - 128
        return "crash", f"signal {sig}"
    if rc != 0 or scene is None:
        return "crash", f"exit {rc}: {stderr.strip()[-200:]}"
    if scene.startswith("error "):
        msg = scene[6:].strip()
        return ("memory" if "out of memory" in msg else "refused"), msg[:200]
    return "ok", ""


def scene_stats(d):
    s = read_text(os.path.join(d, "scene.txt")) or ""
    r = {"elements": 0, "triangles": 0, "warnings": []}
    m = re.search(r"^elements (\d+) triangles (\d+)", s, re.M)
    if m:
        r["elements"], r["triangles"] = int(m.group(1)), int(m.group(2))
    r["warnings"] = [l[8:] for l in s.splitlines() if l.startswith("warning ")]
    t = read_text(os.path.join(d, "time.txt")) or ""
    m = re.search(r"read_ms ([\d.]+)", t)
    r["read_ms"] = float(m.group(1)) if m else None
    m = re.search(r"peak_rss_kb (-?\d+)", t)
    r["rss_kb"] = int(m.group(1)) if m else None
    c = read_text(os.path.join(d, "counts.txt")) or ""
    for k in ("products", "entities"):
        m = re.search(rf"^{k} (\d+)", c, re.M)
        if m:
            r[k] = int(m.group(1))
    r["empty"] = {m.group(1): int(m.group(2)) for m in re.finditer(r"^empty (\w+) (\d+)$", c, re.M)}
    return r


def same(a, b):
    out = []
    for n in ("scene.txt", "meshes.txt"):
        x, y = read_text(os.path.join(a, n)), read_text(os.path.join(b, n))
        if x != y:
            out.append(n)
    return out


SAN_RE = re.compile(r"(ERROR: AddressSanitizer[^\n]*|ERROR: LeakSanitizer[^\n]*|[^\n]*runtime error:[^\n]*)")


def cmd_run(a):
    root = os.path.abspath(a.corpus)
    files = files_of(root)
    if a.only:
        files = [f for f in files if a.only in f[0]]
    done = set()
    if os.path.exists(a.results) and not a.fresh:
        for l in open(a.results):
            done.add(json.loads(l)["path"])
    runs = os.path.join(root, "_runs")
    os.makedirs(runs, exist_ok=True)
    out = open(a.results, "w" if a.fresh else "a")
    for i, (rel, fmt) in enumerate(files):
        if rel in done:
            continue
        path = os.path.join(root, rel)
        rec = {"path": rel, "corpus": rel.split(os.sep)[0], "format": fmt, "bytes": os.path.getsize(path)}
        if fmt == "lfs-pointer":
            rec["c"] = {"outcome": "skipped", "detail": "git-lfs pointer (git-lfs not installed)"}
            out.write(json.dumps(rec) + "\n"); out.flush()
            continue
        if fmt == "step":
            rec["schema"] = schema_of(path)
            rec["entities"] = count_entities(path)
        rec["class"] = schema_class(rec.get("schema", "")) if fmt == "step" else fmt
        key = hashlib.sha1(rel.encode()).hexdigest()[:16]
        base = os.path.join(runs, key)
        cdir, rdir = base + "/c", base + "/rust"
        shutil.rmtree(base, ignore_errors=True)
        os.makedirs(cdir); os.makedirs(rdir)
        rc, wall, err = run([a.c, "read", path, cdir])
        o, det = outcome(rc, cdir, err)
        rec["c"] = {"outcome": o, "detail": det, "wall_s": round(wall, 3), **(scene_stats(cdir) if o == "ok" else {})}
        if a.rust:
            rc, wall, err = run([a.rust, "--dump-read", path, rdir])
            o2, det2 = outcome(rc, rdir, err)
            rec["rust"] = {"outcome": o2, "detail": det2, "wall_s": round(wall, 3), **(scene_stats(rdir) if o2 == "ok" else {})}
            if o == "ok" and o2 == "ok":
                d = same(cdir, rdir)
                rec["same"] = not d
                rec["diff"] = d
            elif o == "refused" and o2 == "refused":
                rec["same"] = read_text(cdir + "/scene.txt") == read_text(rdir + "/scene.txt")
        if a.asan:
            adir = base + "/asan"
            os.makedirs(adir)
            env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:detect_leaks=1:abort_on_error=0",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
            rc, wall, err = run([a.asan, "read", path, adir], env)
            o3, det3 = outcome(rc, adir, err)
            m = SAN_RE.search(err)
            rec["asan"] = {"outcome": o3, "wall_s": round(wall, 3), "report": m.group(1)[:300] if m else "",
                           "detail": det3}
            if m:
                with open(os.path.join(runs, key + ".asan.txt"), "w") as f:
                    f.write(err[-20000:])
        if not rec.get("diff"):
            shutil.rmtree(base, ignore_errors=True)
        else:  # keep the two scene texts only
            for d in (cdir, rdir):
                for n in os.listdir(d):
                    if n not in ("scene.txt", "meshes.txt", "time.txt", "counts.txt"):
                        os.remove(os.path.join(d, n))
        out.write(json.dumps(rec) + "\n"); out.flush()
        print(f"[{i + 1}/{len(files)}] {rec['c']['outcome']:8} "
              f"{rec.get('rust', {}).get('outcome', '-'):8} {'=' if rec.get('same') else '≠' if 'same' in rec else ' '} "
              f"{rec.get('asan', {}).get('report', '')[:60]} {rel}", flush=True)


def cmd_ios(a):
    """IfcOpenShell on a sample: at most n ok STEP files, spread over schema classes and sizes."""
    recs = [json.loads(l) for l in open(a.results)]
    # Files with at least one product with a body, in a schema IfcOpenShell 0.8/0.9 opens (not the
    # IFC4X3 release candidates, IFC4X1, IFC4X2), under 60 MB.
    ios_ok = re.compile(r"IFC2X3|IFC4|IFC4X3(_ADD\d|_TC\d)?$", re.I)
    recs = [r for r in recs if r["format"] == "step" and r["c"]["outcome"] == "ok" and r["bytes"] < 60e6
            and r["c"].get("products", 0) > 0 and ios_ok.fullmatch(r.get("schema", ""))]
    by = collections.defaultdict(list)
    for r in recs:
        by[r["class"]].append(r)
    pick = []
    for cls in sorted(by):
        rs = sorted(by[cls], key=lambda r: (r["bytes"], r["path"]))
        k = max(1, round(a.n * len(rs) / len(recs)))
        k = min(k, len(rs))
        pick += [rs[int(j * (len(rs) - 1) / max(1, k - 1))] for j in range(k)] if k > 1 else [rs[len(rs) // 2]]
    seen, sample = set(), []
    for r in pick:
        if r["path"] not in seen:
            seen.add(r["path"]); sample.append(r)
    helper = os.path.join(os.path.dirname(os.path.abspath(__file__)), "corpus_ios.py")
    out = open(a.ios, "w")
    for i, r in enumerate(sample[: a.n]):
        path = os.path.join(os.path.abspath(a.corpus), r["path"])
        t = time.monotonic()
        p = subprocess.run(capped([a.python, "-I", helper, path]), capture_output=True)
        res = {"path": r["path"], "class": r["class"], "wall_s": round(time.monotonic() - t, 2), "rc": p.returncode}
        try:
            res.update(json.loads(p.stdout.decode().strip().splitlines()[-1]))
        except Exception:
            res["error"] = p.stderr.decode("utf-8", "replace")[-300:]
        res["c_elements"] = r["c"].get("elements")
        res["c_products"] = r["c"].get("products")
        out.write(json.dumps(res) + "\n"); out.flush()
        print(f"[{i + 1}] ios={res.get('with_geometry')} c={res['c_elements']} {r['path']}", flush=True)


WARN_RE = re.compile(r"^(.*?)(?: \(×(\d+)\))?$")


def norm_warning(w):
    """(kind, count): the warning without its repeat count; counts are occurrences."""
    m = WARN_RE.match(w)
    return m.group(1), int(m.group(2) or 1)


def cmd_report(a):
    recs = [json.loads(l) for l in open(a.results)]
    P = print
    classes = ["IFC2X3", "IFC4", "IFC4X3", "other"]
    # files by corpus and format
    P("### Files by corpus\n")
    P("| corpus | STEP files | IFC2X3 | IFC4 | IFC4X3 | other schema | zip | xml | json | other | LFS pointer (not fetched) | MB |")
    P("|---|---|---|---|---|---|---|---|---|---|---|---|")
    by = collections.defaultdict(list)
    for r in recs:
        by[r["corpus"]].append(r)
    for c in sorted(by, key=str.lower):
        rs = by[c]
        cnt = collections.Counter(r["class"] if r["format"] == "step" else "fmt:" + r["format"] for r in rs)
        P(f"| {c} | {sum(1 for r in rs if r['format'] == 'step')} | " + " | ".join(str(cnt.get(k, 0)) for k in classes)
          + " | " + " | ".join(str(cnt.get("fmt:" + k, 0)) for k in ("zip", "xml", "json", "other", "lfs-pointer"))
          + f" | {sum(r['bytes'] for r in rs) / 1e6:.0f} |")
    steps = [r for r in recs if r["format"] == "step"]
    tot = collections.Counter(r["class"] if r["format"] == "step" else "fmt:" + r["format"] for r in recs)
    P(f"| all | {len(steps)} | " + " | ".join(str(tot.get(k, 0)) for k in classes) + " | "
      + " | ".join(str(tot.get("fmt:" + k, 0)) for k in ("zip", "xml", "json", "other", "lfs-pointer"))
      + f" | {sum(r['bytes'] for r in recs) / 1e6:.0f} |")
    P("\nSchemas (FILE_SCHEMA): " + ", ".join(f"{k} {v}" for k, v in sorted(collections.Counter(r.get("schema") for r in steps).items())))
    P("\n### Outcome by schema (STEP files)\n")
    for who in ("c", "rust", "asan"):
        if not any(who in r for r in steps):
            continue
        P(f"\n{ {'c': 'C reader (clang -O2)', 'rust': 'Rust read_str', 'asan': 'C reader, ASan+UBSan'}[who] }:\n")
        P("| schema | files | ok | refused | crash | timeout | over memory |")
        P("|---|---|---|---|---|---|---|")
        tot = collections.Counter()
        for k in classes + ["all"]:
            rs = [r for r in steps if who in r and (k == "all" or r["class"] == k)]
            cnt = collections.Counter(r[who]["outcome"] for r in rs)
            if k == "all":
                cnt = tot
            else:
                tot.update(cnt)
            n = sum(cnt.values())
            P(f"| {k} | {n} | " + " | ".join(str(cnt.get(o, 0)) for o in ("ok", "refused", "crash", "timeout", "memory")) + " |")
    nonstep = [r for r in recs if r["format"] not in ("step", "lfs-pointer")]
    if nonstep:
        P("\nNon-STEP files (ifczip, ifcxml, zip, ...), C reader: "
          + ", ".join(f"{k} {v}" for k, v in sorted(collections.Counter(r["c"]["outcome"] for r in nonstep).items()))
          + "; Rust: " + ", ".join(f"{k} {v}" for k, v in sorted(collections.Counter(r.get("rust", {}).get("outcome", "-") for r in nonstep).items())))
    # C vs Rust
    both = [r for r in steps if r["c"]["outcome"] == "ok" and r.get("rust", {}).get("outcome") == "ok"]
    P(f"\n### C vs Rust\n\nBoth ok: {len(both)}; identical (scene.txt and every mesh hash): "
      f"{sum(1 for r in both if r.get('same'))}; different: {sum(1 for r in both if not r.get('same'))}.")
    disagree = [r for r in steps if r["c"]["outcome"] != r.get("rust", {}).get("outcome", r["c"]["outcome"])]
    P(f"Outcome classes differ on {len(disagree)} file(s).\n")
    rows = [r for r in both if not r.get("same")] + disagree
    if rows:
        P("| file | C | Rust | differs in | C elements / tris | Rust elements / tris |")
        P("|---|---|---|---|---|---|")
        for r in rows:
            c, u = r["c"], r.get("rust", {})
            P(f"| `{r['path']}` | {c['outcome']} {c.get('detail', '')[:60]} | {u.get('outcome')} {u.get('detail', '')[:60]} | "
              f"{', '.join(r.get('diff', [])) or '-'} | {c.get('elements', '-')} / {c.get('triangles', '-')} | "
              f"{u.get('elements', '-')} / {u.get('triangles', '-')} |")
    # products without geometry
    ok = [r for r in steps if r["c"]["outcome"] == "ok"]
    prod = sum(r["c"].get("products", 0) for r in ok)
    el = sum(r["c"].get("elements", 0) for r in ok)
    tri = sum(r["c"].get("triangles", 0) for r in ok)
    P(f"\n### Geometry (C, files read ok: {len(ok)})\n")
    P(f"Products with a body (IfcProductDefinitionShape and a placement, not a skipped class): {prod}; "
      f"drawn (non-empty mesh): {el}; drawn empty (no geometry produced): {prod - el} "
      f"({100 * (prod - el) / max(1, prod):.1f} %). Triangles: {tri}.\n")
    P("| schema | files ok | products with a body | drawn | empty | empty % |")
    P("|---|---|---|---|---|---|")
    for k in classes:
        rs = [r for r in ok if r["class"] == k]
        p = sum(r["c"].get("products", 0) for r in rs); e = sum(r["c"].get("elements", 0) for r in rs)
        P(f"| {k} | {len(rs)} | {p} | {e} | {p - e} | {100 * (p - e) / max(1, p):.1f} |")
    emp = collections.defaultdict(lambda: [0, 0, None])
    for r in ok:
        for k, n in r["c"].get("empty", {}).items():
            emp[k][0] += 1; emp[k][1] += n; emp[k][2] = emp[k][2] or r["path"]
    P("\nProducts with a body drawn empty, by IFC type (ranked by products):\n")
    P("| IFC type | files | products | example file |")
    P("|---|---|---|---|")
    for k, (f, n, ex) in sorted(emp.items(), key=lambda kv: (-kv[1][1], kv[0])):
        P(f"| {k} | {f} | {n} | `{ex}` |")
    # warnings
    kinds = collections.defaultdict(lambda: [0, 0, []])
    for r in ok:
        for w in r["c"].get("warnings", []):
            k, n = norm_warning(w)
            kinds[k][0] += 1; kinds[k][1] += n; kinds[k][2].append(r["path"])
    P("\n### Reader warnings, ranked by files (C; occurrences are the reader's counts)\n")
    P("| warning | files | occurrences | example file |")
    P("|---|---|---|---|")
    for k, (f, n, ps) in sorted(kinds.items(), key=lambda kv: (-kv[1][0], -kv[1][1], kv[0])):
        P(f"| {k.replace('|', '/')} | {f} | {n} | `{ps[0]}` |")
    # unsupported entity types: aggregate the warning's entity name
    ent = collections.defaultdict(lambda: [0, 0, set()])
    for r in ok:
        seen = set()
        for w in r["c"].get("warnings", []):
            k, n = norm_warning(w)
            m = re.match(r"unsupported (geometry|curve|profile|placement|face loop) (\w+)", k) or \
                re.match(r"(\w+): unsupported (base surface)", k)
            if not m:
                continue
            name = m.group(2) if m.re.pattern.startswith("unsupported") else "base surface of " + m.group(1)
            key = (m.group(1) if m.re.pattern.startswith("unsupported") else "boolean", name)
            ent[key][1] += n
            ent[key][2].add(r["corpus"])
            if key not in seen:
                ent[key][0] += 1; seen.add(key)
    P("\n### Unsupported entity types (the to-do list), ranked by files\n")
    P("| kind | IFC type | files | occurrences | corpora |")
    P("|---|---|---|---|---|")
    for (kind, name), (f, n, cs) in sorted(ent.items(), key=lambda kv: (-kv[1][0], -kv[1][1])):
        P(f"| {kind} | {name} | {f} | {n} | {len(cs)} |")
    # crashes & sanitizer
    P("\n### Crashes, timeouts, memory, sanitizer reports\n")
    bad = [(who, r) for r in recs for who in ("c", "rust") if r.get(who, {}).get("outcome") in ("crash", "timeout", "memory")]
    bad += [("asan", r) for r in recs if r.get("asan", {}).get("report") or r.get("asan", {}).get("outcome") in ("crash", "timeout", "memory")]
    if not bad:
        P("None.")
    for who, r in bad:
        x = r[who]
        P(f"- {who}: {x['outcome']} `{r['path']}` {x.get('report') or x.get('detail', '')}")
    # refusals
    P("\n### Refusals (C), by message\n")
    ref = collections.defaultdict(list)
    for r in steps:
        if r["c"]["outcome"] == "refused":
            ref[re.sub(r"line \d+", "line N", r["c"]["detail"])[:120]].append(r["path"])
    P("| message | files | example |")
    P("|---|---|---|")
    for m, ps in sorted(ref.items(), key=lambda kv: -len(kv[1])):
        P(f"| {m.replace('|', '/')} | {len(ps)} | `{ps[0]}` |")
    # timings
    P("\n### Timings (read call, ms; wall in s includes process start and the dump)\n")
    for who in ("c", "rust"):
        rs = [r for r in ok if r.get(who, {}).get("outcome") == "ok"]
        P(f"- {who}: {len(rs)} files, read total {sum(r[who]['read_ms'] or 0 for r in rs) / 1e3:.1f} s, "
          f"wall total {sum(r[who]['wall_s'] for r in rs):.1f} s")
    P("\nSlowest 10 (C read call):\n")
    P("| file | MB | schema | C read ms | Rust read ms | C peak RSS MB | elements | triangles |")
    P("|---|---|---|---|---|---|---|---|")
    for r in sorted(ok, key=lambda r: -(r["c"].get("read_ms") or 0))[:10]:
        c, u = r["c"], r.get("rust", {})
        P(f"| `{r['path']}` | {r['bytes'] / 1e6:.1f} | {r.get('schema')} | {c.get('read_ms')} | {u.get('read_ms', '-')} | "
          f"{(c.get('rss_kb') or 0) / 1024:.0f} | {c.get('elements')} | {c.get('triangles')} |")
    if a.ios and os.path.exists(a.ios):
        ios = [json.loads(l) for l in open(a.ios)]
        P("\n### IfcOpenShell vs C: products with geometry (sample)\n")
        P("| file | schema | IfcOpenShell products with geometry | C drawn | C products with a body | IfcOpenShell s |")
        P("|---|---|---|---|---|---|")
        for x in ios:
            P(f"| `{x['path']}` | {x['class']} | {x.get('with_geometry', 'error ' + str(x.get('rc')))} | {x['c_elements']} | "
              f"{x['c_products']} | {x['wall_s']} |")
        g = [x for x in ios if "with_geometry" in x]
        P(f"\nSum over {len(g)} files: IfcOpenShell {sum(x['with_geometry'] for x in g)}, C {sum(x['c_elements'] for x in g)}; "
          f"files where C draws fewer: {sum(1 for x in g if x['c_elements'] < x['with_geometry'])}, more: "
          f"{sum(1 for x in g if x['c_elements'] > x['with_geometry'])}, equal: {sum(1 for x in g if x['c_elements'] == x['with_geometry'])}.")


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    r = sp.add_parser("run"); r.add_argument("corpus"); r.add_argument("results")
    r.add_argument("--c", required=True); r.add_argument("--rust"); r.add_argument("--asan")
    r.add_argument("--only"); r.add_argument("--fresh", action="store_true")
    i = sp.add_parser("ios"); i.add_argument("corpus"); i.add_argument("results"); i.add_argument("ios")
    i.add_argument("--python", required=True); i.add_argument("--n", type=int, default=40)
    p = sp.add_parser("report"); p.add_argument("results"); p.add_argument("ios", nargs="?")
    a = ap.parse_args()
    {"run": cmd_run, "ios": cmd_ios, "report": cmd_report}[a.cmd](a)


if __name__ == "__main__":
    main()
