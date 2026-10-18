//! QRC01: QR symbol encoding — the matrix behind "Show QR for this
//! page".
//!
//! The encoder is Nayuki's `qrcodegen` crate: the same code lineage
//! (same author, same MIT license) as the vendored C++ qrcodegen.cpp
//! it replaces, so the output is bit-identical — same segment
//! optimizer, same ECL boost, same mask-penalty choice.  What moved
//! here is only the untrusted-text -> module-matrix step (URLs,
//! WIFI: payloads); painting stays Qt-side exactly as before.
//!
//! Wire layout of the returned buffer:
//!   [0..4)  u32le module count per side (21..=177)
//!   [4..)   size*size bytes, row-major, 1 = dark, 0 = light
//!           (no quiet zone — the renderer adds the spec's 4 modules)

use qrcodegen::{QrCode, QrCodeEcc};

use crate::error::{self, Fail, RcResult, RcStatus};

/// Sanity bound far above the QR spec ceiling (~7089 numeric chars,
/// ~2953 UTF-8 bytes at ECC L): the encoder rejects over-capacity
/// input on its own, but a bogus caller should not get to feed the
/// segment optimizer a giant string first.
const MAX_TEXT_LEN: usize = 8192;

/// ECC ordinals — identical numbering to the C++
/// qrcodegen::QrCode::Ecc (LOW=0 .. HIGH=3), so callers pass the enum
/// value straight across the ABI.
fn ecc_of(level: i32) -> Option<QrCodeEcc> {
    match level {
        0 => Some(QrCodeEcc::Low),
        1 => Some(QrCodeEcc::Medium),
        2 => Some(QrCodeEcc::Quartile),
        3 => Some(QrCodeEcc::High),
        _ => None,
    }
}

/// Encode `text` (UTF-8) at `ecc_level`, returning the wire matrix
/// described in the module docs.
pub fn encode(text: &str, ecc_level: i32) -> RcResult<Vec<u8>> {
    let ecl = ecc_of(ecc_level).ok_or_else(|| Fail {
        status: RcStatus::InvalidArgument,
        msg: format!("bad QR ecc level {ecc_level}"),
    })?;
    if text.len() > MAX_TEXT_LEN {
        return error::fail(
            RcStatus::InvalidArgument,
            "text exceeds the QR capacity bound",
        );
    }
    let qr = QrCode::encode_text(text, ecl).map_err(|_| Fail {
        status: RcStatus::InvalidArgument,
        msg: "payload exceeds QR capacity".into(),
    })?;
    let size = qr.size() as usize;
    let mut out = Vec::with_capacity(4 + size * size);
    out.extend_from_slice(&(size as u32).to_le_bytes());
    for y in 0..size {
        for x in 0..size {
            out.push(qr.get_module(x as i32, y as i32) as u8);
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn size_of(buf: &[u8]) -> usize {
        u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]) as usize
    }

    // The 29x29 ground truth the C++ autotest pins for
    // "https://arktos.example/clean?id=42" at ECC M — produced by an
    // independent implementation of the same algorithm.  Matching it
    // proves the crate port is bit-identical to the vendored encoder.
    const REF_TEXT: &str = "https://arktos.example/clean?id=42";
    const REF_MATRIX: [&str; 29] = [
        "11111110010001001111101111111",
        "10000010110000111011101000001",
        "10111010000001101000001011101",
        "10111010001111110011001011101",
        "10111010101000100011001011101",
        "10000010000101000001001000001",
        "11111110101010101010101111111",
        "00000000000111101111100000000",
        "10101010000100111101100010010",
        "10010101010010110000111001001",
        "10100011011001001000110010111",
        "11110100100000000101001000010",
        "10011111011000000100011101011",
        "10101001000111000100101001001",
        "00101011010000101000001101011",
        "10011001000111111111101111010",
        "11011010100110100101101001011",
        "00000000010000100100111001101",
        "10111011000101000010010000011",
        "01000100100010011111011101010",
        "10110010111000010100111110000",
        "00000000101111000001100010111",
        "11111110001010101101101011011",
        "10000010010001111100100011011",
        "10111010110010101110111110001",
        "10111010001000101011000010111",
        "10111010100100000100000111001",
        "10000010011110010110100000010",
        "11111110101111111101111010011",
    ];

    #[test]
    fn reference_matrix_matches() {
        let buf = encode(REF_TEXT, 1).unwrap();
        assert_eq!(size_of(&buf), 29);
        assert_eq!(buf.len(), 4 + 29 * 29);
        for (y, row) in REF_MATRIX.iter().enumerate() {
            for (x, c) in row.chars().enumerate() {
                assert_eq!(
                    buf[4 + y * 29 + x],
                    (c == '1') as u8,
                    "module ({x},{y})"
                );
            }
        }
    }

    #[test]
    fn all_ecc_levels_encode() {
        for level in 0..4 {
            let buf = encode("https://example.org", level).unwrap();
            let size = size_of(&buf);
            assert!((21..=177).contains(&size));
            assert_eq!(buf.len(), 4 + size * size);
            assert!(buf[4..].iter().all(|b| *b <= 1));
        }
    }

    #[test]
    fn empty_and_capacity_edges() {
        // Empty text still yields a valid version-1 symbol.
        assert_eq!(size_of(&encode("", 0).unwrap()), 21);
        // Near-capacity at ECC L succeeds, over-capacity fails with
        // the same verdict the C++ encoder reports as data_too_long.
        let payload = "a".repeat(2953);
        assert!(encode(&payload, 0).is_ok());
        // 6000 bytes of byte-mode payload clears the sanity bound but
        // exceeds the spec ceiling — the encoder's own rejection.
        assert_eq!(
            encode(&"é".repeat(3000), 0).unwrap_err().status,
            RcStatus::InvalidArgument
        );
        // Past the sanity bound: refused before the encoder runs.
        assert_eq!(
            encode(&"a".repeat(9000), 0).unwrap_err().status,
            RcStatus::InvalidArgument
        );
    }

    #[test]
    fn bad_ecc_rejected() {
        assert_eq!(
            encode("x", -1).unwrap_err().status,
            RcStatus::InvalidArgument
        );
        assert_eq!(
            encode("x", 4).unwrap_err().status,
            RcStatus::InvalidArgument
        );
    }

    #[test]
    fn multibyte_text() {
        // UTF-8 payloads (accents, CJK, non-BMP) encode as byte-mode
        // segments — the encoder sees the same UTF-8 bytes the C++
        // side produced from QString::toUtf8.
        let buf = encode("Grüße, 世界, 𝕏 — WIFI:T:WPA;S:Net;P:x;;", 3).unwrap();
        assert!(size_of(&buf) >= 21);
    }
}
