//! IFC GlobalId: a 128-bit UUID compressed to 22 characters (IFC base64 alphabet).

use serde::{Deserialize, Serialize};
use std::fmt;

const ALPHABET: &[u8; 64] = b"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$";

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(try_from = "String", into = "String")]
pub struct GlobalId([u8; 22]);

impl GlobalId {
    /// A fresh random GlobalId.
    pub fn new() -> Self {
        Self::from_u128(uuid::Uuid::new_v4().as_u128())
    }

    /// Deterministic GlobalId derived from arbitrary bytes (used for IFC relationship entities).
    pub fn derived(seed: &[u8]) -> Self {
        let h = blake3::hash(seed);
        let mut b = [0u8; 16];
        b.copy_from_slice(&h.as_bytes()[..16]);
        Self::from_u128(u128::from_be_bytes(b))
    }

    pub fn from_u128(n: u128) -> Self {
        let mut out = [0u8; 22];
        out[0] = ALPHABET[((n >> 126) & 0x3) as usize];
        for (i, c) in out.iter_mut().enumerate().skip(1) {
            let shift = 126 - 6 * i as u32;
            *c = ALPHABET[((n >> shift) & 0x3f) as usize];
        }
        GlobalId(out)
    }

    pub fn to_u128(&self) -> u128 {
        let idx = |c: u8| ALPHABET.iter().position(|&a| a == c).unwrap() as u128;
        let mut n = idx(self.0[0]);
        for &c in &self.0[1..] {
            n = (n << 6) | idx(c);
        }
        n
    }

    pub fn as_str(&self) -> &str {
        // Only ASCII from ALPHABET is ever stored.
        std::str::from_utf8(&self.0).unwrap()
    }
}

impl Default for GlobalId {
    fn default() -> Self {
        Self::new()
    }
}

impl TryFrom<String> for GlobalId {
    type Error = String;
    fn try_from(s: String) -> Result<Self, String> {
        s.parse()
    }
}

impl std::str::FromStr for GlobalId {
    type Err = String;
    fn from_str(s: &str) -> Result<Self, String> {
        let b = s.as_bytes();
        if b.len() != 22 || !b.iter().all(|c| ALPHABET.contains(c)) || !b"0123".contains(&b[0]) {
            return Err(format!("invalid IFC GlobalId: {s:?}"));
        }
        let mut out = [0u8; 22];
        out.copy_from_slice(b);
        Ok(GlobalId(out))
    }
}

impl From<GlobalId> for String {
    fn from(g: GlobalId) -> String {
        g.as_str().to_owned()
    }
}

impl fmt::Display for GlobalId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

impl fmt::Debug for GlobalId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "GlobalId({})", self.as_str())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        for _ in 0..1000 {
            let g = GlobalId::new();
            let n = g.to_u128();
            assert_eq!(GlobalId::from_u128(n), g);
            assert_eq!(g.as_str().parse::<GlobalId>().unwrap(), g);
        }
    }

    #[test]
    fn known_value() {
        // Reference: ifcopenshell.guid.compress("00000000000000000000000000000000") == "0000000000000000000000"
        assert_eq!(GlobalId::from_u128(0).as_str(), "0000000000000000000000");
        // ifcopenshell.guid.compress("ffffffffffffffffffffffffffffffff") == "3$$$$$$$$$$$$$$$$$$$$$"
        assert_eq!(
            GlobalId::from_u128(u128::MAX).as_str(),
            "3$$$$$$$$$$$$$$$$$$$$$"
        );
        // ifcopenshell.guid.compress("0123456789abcdef0fedcba987654321") == "018qLdYQlDxm$jowc7PKCX"
        assert_eq!(
            GlobalId::from_u128(0x0123456789abcdef0fedcba987654321).as_str(),
            "018qLdYQlDxm$jowc7PKCX"
        );
    }

    #[test]
    fn rejects_garbage() {
        assert!("short".parse::<GlobalId>().is_err());
        assert!("4$$$$$$$$$$$$$$$$$$$$$".parse::<GlobalId>().is_err());
    }
}
