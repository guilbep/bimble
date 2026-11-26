// QTY1: one number formatter for the page (an audit found a wall length shown as "3 101.6 124 838 541 646": the
// thousands regex ran over the decimals too). No import: node runs it as it is (ccore/test/lab_quantity_lists.mjs).
//
// groupDigits: the integer part in groups of three with a space, the decimals as they are (an editable field
//   keeps its exact value: 3101.6124838541646 -> "3 101.6124838541646"), minus as U+2212 (the reference's).
// formatMm: bimble_api::measure::format_mm, ported in ccore/session/measure.c cc_format_mm: from 100 mm a whole
//   number ("3 102 mm"), below one decimal ("12.3 mm"); groups with U+2009 as Rust; "NaN mm".

/** `n` with its integer part grouped by three (a space), its decimals untouched, minus as U+2212. */
export function groupDigits(n: number, sep = " "): string {
  if (!Number.isFinite(n)) return String(n);
  const s = String(n);
  const neg = s.startsWith("-");
  const body = neg ? s.slice(1) : s;
  if (/e/i.test(body)) return (neg ? "−" : "") + body; // 1e+21 and 1e-7: as they are
  const dot = body.indexOf(".");
  const int = dot < 0 ? body : body.slice(0, dot);
  const frac = dot < 0 ? "" : body.slice(dot);
  return (neg ? "−" : "") + int.replace(/\B(?=(\d{3})+(?!\d))/g, sep) + frac;
}

/** Rust's format_mm: "3 102 mm" (U+2009 groups) from 100 mm, "12.3 mm" below, "NaN mm". */
export function formatMm(mm: number): string {
  if (Number.isNaN(mm)) return "NaN mm";
  const a = Math.abs(mm);
  const t = a >= 100 ? String(Math.sign(mm) * Math.floor(a + 0.5)) : mm.toFixed(1);
  const neg = t.startsWith("-");
  const [int = "", frac] = (neg ? t.slice(1) : t).split(".");
  return `${neg ? "-" : ""}${int.replace(/\B(?=(\d{3})+(?!\d))/g, " ")}${frac !== undefined ? `.${frac}` : ""} mm`;
}
