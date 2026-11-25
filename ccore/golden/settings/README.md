# Settings files (ticket SET1)

The user's `settings.json`, in the form the Rust viewer writes it (`crates/bimble-viewer/src/settings.rs`
`to_json`: `serde_json::json!` of the eleven fields, a sorted map, compact; `text_size` an f32 written as
a JSON f64). Suite `settings` reads each file and writes it back byte for byte.

- `default.json`: `Settings::default().to_json()`.
- `filled.json`: the values of Rust's own test `settings_round_trip_and_bad_values_fall_back`.
- `unknown.json`: a file with members Rust does not know (as a later version could write): the C core
  keeps them as they are (the current Rust app drops them when it saves).

Written by hand in that form: this ticket had no Rust build (DEVIATIONS 23). `ccore_golden` mode
`settings` is to write them from Rust (round-4.md SET1, hooks).
