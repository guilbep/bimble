//! Writes the float and string goldens for ccore (run from this directory):
//! floats.txt: `<hex bits> <serde_json text> <{:?} text>` per value.
//! strings.txt: `<hex of UTF-8 input> <serde_json text>` per string.
//! pretty.txt: serde_json::to_string_pretty of a small nested value.
use std::fmt::Write as _;

fn next_up(x: f64) -> f64 {
    f64::from_bits(if x >= 0.0 { x.to_bits() + 1 } else { x.to_bits() - 1 })
}
fn next_down(x: f64) -> f64 {
    f64::from_bits(if x > 0.0 { x.to_bits() - 1 } else { x.to_bits() + 1 })
}

fn main() {
    let mut v: Vec<f64> = vec![
        0.0, -0.0, 1.0, -1.0, 0.1, 0.2, 0.3, 0.1 + 0.2, 1.0 / 3.0, 2.0 / 3.0, 300.0, 1e15, 1e16,
        1e17, 9007199254740992.0, 9007199254740993.0, 9007199254740994.0, 9007199254740996.0,
        1e-4, 1e-5, 1e-6, 1e-7, 5e-324, 1e-323, 2.2250738585072014e-308,
        2.2250738585072009e-308, f64::MAX, -f64::MAX, f64::MIN_POSITIVE, f64::EPSILON,
        2535844157.0496545, 1e-300, 1e300, 123456789012345680.0, 1.5, 2.5, 100.0, 1000.0,
        12345.0, 3000.0, 4000.5, -250.0, 0.5, 0.25, 0.001, 0.0001234, 1.7976931348623157e308,
        4.9406564584124654e-324, 1e21, 1e22, 1e23, 9.999999999999999e15, 9.999999999999998e15,
        1.2345678901234567e16, 123456.789, 0.000001, 0.00001, 0.0000123, 3.14159, 2.718281828459045,
        std::f64::consts::PI, std::f64::consts::E, 65536.0, 4294967296.0, 18446744073709551616.0,
        -1e-7, -123.456, 1e7, 1e8, 1e9, 1e10, 2e15, 5e16, 7.0e-5, 4.35, 0.07, 1.1, 2.675,
    ];
    for i in -400..=400 {
        v.push(i as f64);
    }
    for k in -20..=22 {
        let p: f64 = format!("1e{k}").parse().unwrap();
        for x in [p, next_up(p), next_down(p), next_up(next_up(p)), 2.0 * p, 5.0 * p, 9.5 * p] {
            v.push(x);
            v.push(-x);
        }
    }
    for e in -1074..=1023 {
        // powers of two, every binade edge
        if e % 3 == 0 {
            let x = 2f64.powi(e);
            if x.is_finite() && x != 0.0 {
                v.push(x);
                v.push(next_down(x));
            }
        }
    }
    let mut s: u64 = 0x9E37_79B9_7F4A_7C15;
    let mut xs = || {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        s
    };
    let mut n = 0;
    while n < 2000 {
        let x = f64::from_bits(xs());
        if x.is_finite() {
            v.push(x);
            n += 1;
        }
    }
    // values a model holds: millimetres with up to 3 decimals
    for _ in 0..500 {
        let r = xs();
        let m = (r % 100_000_000) as f64 / 1000.0;
        v.push(if r >> 63 == 1 { -m } else { m });
    }
    // exact ties between two shortest candidates (Ryu/serde_json: to even, {:?}: up)
    for _ in 0..300 {
        let n = 1_000_000_000_000_000 + xs() % 1_200_000_000_000_000;
        let f = [0.25, 0.75][(xs() % 2) as usize];
        v.push(n as f64 + f);
        let k = (100_000_000_000_000_000 + xs() % 40_000_000_000_000_000) / 16 * 16;
        v.push(k as f64);
        v.push(-(n as f64 + f) / 1024.0);
    }
    let mut out = String::new();
    for x in &v {
        let j = serde_json::to_string(x).unwrap();
        writeln!(out, "{:016x} {} {:?}", x.to_bits(), j, x).unwrap();
    }
    std::fs::write("floats.txt", out).unwrap();

    let strs: Vec<String> = vec![
        String::new(),
        "plain".into(),
        "quote\" backslash\\ slash/".into(),
        "\n\r\t\u{8}\u{c}".into(),
        (0u8..0x20).map(|b| b as char).collect(),
        "\u{7f} del stays".into(),
        "Béton C30/37 – Ø 12 mm ✓ 柱 🧱".into(),
        "IfcColumn 'C1'".into(),
        "<script>&amp;</script>".into(),
        "tab\tin\tmiddle".into(),
    ];
    let mut out = String::new();
    for st in &strs {
        let hex: String = st.bytes().map(|b| format!("{b:02x}")).collect();
        let hex = if hex.is_empty() { "-".to_string() } else { hex };
        writeln!(out, "{} {}", hex, serde_json::to_string(st).unwrap()).unwrap();
    }
    std::fs::write("strings.txt", out).unwrap();

    let val = serde_json::json!({"a": [1, 2, {"b": null}], "c": {}, "d": [],
        "e": {"f": 0.1, "g": true, "h": "x\ny", "i": [-3, 18446744073709551615u64, -9223372036854775808i64]}});
    let mut out = serde_json::to_string_pretty(&val).unwrap();
    out.push('\n');
    out.push_str(&serde_json::to_string(&val).unwrap());
    out.push('\n');
    std::fs::write("pretty.txt", out).unwrap();
}
