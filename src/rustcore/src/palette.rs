//! CPAL01: the command palette's query -> ranked-items decision.
//!
//! The item REGISTRY stays Qt-side (palette rows are QActions,
//! WebViews and bookmark handles that cannot cross the FFI); what
//! moved is the matching and ranking logic — the Qt adapter marshals
//! each row's {match text, id} in and reads ordered indices back.
//!
//! `fuzzy_score` is a faithful port of CommandPalette::fuzzyScore —
//! the subsequence scorer every palette keystroke ran:
//!   * the query and candidate are lowercased (Qt toLower() ≈
//!     Unicode full lowercasing — Rust to_lowercase() is the same
//!     family; exotic expansions can differ by a bonus, never by
//!     a match/no-match verdict),
//!   * a left-to-right subsequence walk scores +10 per matched
//!     character, +15 for a match at index 0, +12 at a word boundary
//!     (previous char not letter-or-number), +8 for a consecutive
//!     run, +25 when the whole query occurs as a substring, and a
//!     (hay_len - needle_len)/10 shortness penalty; an unmatched
//!     subsequence is -1, an empty query scores 0, an empty
//!     candidate -1.
//!   * Matching runs on UTF-16 units — Qt indexes QChar units, so
//!     non-BMP text scores identically too.
//!
//! One documented intentional improvement over the port (the task
//! spec's camel-case bonus): a lower->upper transition in the
//! ORIGINAL candidate ("GitHub" at 'H') also earns the word-boundary
//! +12.  The C++ scorer lowered the candidate before inspecting
//! boundaries and could never see the hump, so queries like "hub"
//! missed the boundary credit.  Rankings against the reference only
//! ever move upward on camel-humped candidates.
//!
//! `rank` adds the MRU recency boost the Qt filter applied —
//! `60 - mruIndex` for the id's first position in the MRU list —
//! then stable-sorts by score descending, reproducing the legacy
//! ordering bit-for-bit (empty query -> MRU items in MRU order,
//! then the untouched item order).  The OMNI01 frecency decay
//! family stays the weight vocabulary for when usage records carry
//! timestamps; the palette's MRU list is positional only, so the
//! positional boost is the faithful recency weight today.

use serde_json::{json, Value};

use crate::error::{self, Fail, RcResult, RcStatus};

/// Item-count bound — palettes hold a few hundred rows; this only
/// exists so a malformed request cannot pin the scoring loop.
const MAX_ITEMS: usize = 65536;

/// One palette row as marshaled by the Qt adapter.
pub struct Item<'a> {
    /// The matched-but-never-shown text (label + path + keywords).
    pub match_text: &'a str,
    /// Stable id — the MRU key ("" is never boosted).
    pub id: &'a str,
}

/// Decode the UTF-16 char starting at `i` (surrogate pairs included);
/// None past the end or on an unpaired half — an unpaired half is
/// what a lone QChar looks like and counts as "not alphanumeric"
/// either way, so the callers' fallback matches Qt's verdict.
fn char_at(units: &[u16], i: usize) -> Option<char> {
    let u = *units.get(i)? as u32;
    if (0xD800..0xDC00).contains(&u) {
        let lo = *units.get(i + 1)? as u32;
        if !(0xDC00..0xE000).contains(&lo) {
            return None;
        }
        char::from_u32(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00))
    } else {
        char::from_u32(u)
    }
}

fn contains(hay: &[u16], needle: &[u16]) -> bool {
    needle.is_empty()
        || hay
            .windows(needle.len())
            .any(|w| w == needle)
}

/// Subsequence score: >= 0 is a match (higher is better), -1 is not.
/// The CommandPalette::fuzzyScore port — see module docs for the
/// scoring table and the camel-hump improvement.
pub fn fuzzy_score(query: &str, candidate: &str) -> i64 {
    let needle: Vec<u16> = query.to_lowercase().encode_utf16().collect();
    let hay: Vec<u16> = candidate.to_lowercase().encode_utf16().collect();
    if needle.is_empty() {
        return 0;
    }
    if hay.is_empty() {
        return -1;
    }
    // The original-case candidate drives the camel-hump check; its
    // unit offsets line up with the lowered haystack whenever
    // lowercasing preserved the unit count (every common case —
    // expansion characters degrade to the ported boundary rule).
    let orig: Vec<u16> = candidate.encode_utf16().collect();
    let aligned = orig.len() == hay.len();

    let mut score: i64 = 0;
    let mut qi = 0usize;
    let mut last_match: i64 = -2;
    for i in 0..hay.len() {
        if qi >= needle.len() {
            break;
        }
        if hay[i] != needle[qi] {
            continue;
        }
        score += 10;
        if i == 0 {
            score += 15;
        } else {
            let prev_alnum = char_at(&hay, i - 1)
                .map(|c| c.is_alphanumeric())
                .unwrap_or(false);
            let camel = aligned
                && char_at(&orig, i)
                    .map(|c| c.is_uppercase())
                    .unwrap_or(false)
                && char_at(&orig, i - 1)
                    .map(|c| c.is_lowercase())
                    .unwrap_or(false);
            if !prev_alnum || camel {
                score += 12;
            }
        }
        if i as i64 == last_match + 1 {
            score += 8;
        }
        last_match = i as i64;
        qi += 1;
    }
    if qi < needle.len() {
        return -1;
    }
    if contains(&hay, &needle) {
        score += 25;
    }
    score - ((hay.len() - needle.len()) / 10) as i64
}

/// Score every row and return the matched `(index, score)` pairs in
/// display order — the whole refilter() ranking in one call.
pub fn rank(query: &str, items: &[Item<'_>], mru: &[String]) -> Vec<(usize, i64)> {
    let mut scored: Vec<(usize, i64)> = Vec::with_capacity(items.len());
    for (i, item) in items.iter().enumerate() {
        let mut score = fuzzy_score(query, item.match_text);
        if score < 0 {
            continue;
        }
        // The legacy boost: first MRU position scores highest, and
        // every ranked item outranks any unranked one.
        if let Some(pos) = mru.iter().position(|m| m == item.id) {
            score += 60 - pos as i64;
        }
        scored.push((i, score));
    }
    // Stable sort, descending — ties keep the item order, matching
    // std::stable_sort in the reference path.
    scored.sort_by(|a, b| b.1.cmp(&a.1));
    scored
}

/// The rc_pal_match body: parse the request JSON, rank, emit the
/// ordered `[{"index","score"}]` array as JSON text.
pub fn match_json(query: &str, request: &str) -> RcResult<String> {
    let v: Value = serde_json::from_str(request).map_err(|_| Fail {
        status: RcStatus::Corrupt,
        msg: "palette match: request is not JSON".into(),
    })?;
    let item_values: &[Value] = v
        .get("items")
        .and_then(|a| a.as_array())
        .map(|a| a.as_slice())
        .unwrap_or(&[]);
    if item_values.len() > MAX_ITEMS {
        return error::fail(RcStatus::InvalidArgument, "palette match: too many items");
    }
    let items: Vec<Item<'_>> = item_values
        .iter()
        .map(|v| Item {
            match_text: v.get("match").and_then(|m| m.as_str()).unwrap_or(""),
            id: v.get("id").and_then(|m| m.as_str()).unwrap_or(""),
        })
        .collect();
    let mru: Vec<String> = v
        .get("mru")
        .and_then(|a| a.as_array())
        .map(|a| {
            a.iter()
                .filter_map(|e| e.as_str().map(String::from))
                .collect()
        })
        .unwrap_or_default();

    let ranked = rank(query, &items, &mru);
    let rows: Vec<Value> = ranked
        .iter()
        .map(|&(index, score)| json!({"index": index, "score": score}))
        .collect();
    Ok(Value::Array(rows).to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn item<'a>(m: &'a str, id: &'a str) -> Item<'a> {
        Item {
            match_text: m,
            id,
        }
    }

    // Hand-computed expectations of the ported scoring table.
    #[test]
    fn ported_scores() {
        // Identical strings: full run + substring bonus.
        assert_eq!(fuzzy_score("new tab", "New Tab"), 170);
        // Boundary-hopped acronym: 'c' +15+10, 'p' and 'd' boundary.
        assert_eq!(fuzzy_score("cpd", "Clear Private Data"), 68);
        // No subsequence -> -1.
        assert_eq!(fuzzy_score("xyz", "New Tab"), -1);
        // Empty query matches everything at 0.
        assert_eq!(fuzzy_score("", "anything"), 0);
        // Empty candidate never matches a non-empty query.
        assert_eq!(fuzzy_score("a", ""), -1);
    }

    #[test]
    fn camel_hump_boundary() {
        // The documented improvement: 'H' after 't' is a boundary in
        // the original case — the ported rule alone scores 71.
        assert_eq!(fuzzy_score("hub", "GitHub"), 83);
    }

    #[test]
    fn rank_orders_and_boosts_mru() {
        let items = [
            item("alpha", "a"),
            item("beta", "b"),
            item("gamma", "g"),
        ];
        // Empty query: MRU order first, then item order.
        let ranked = rank("", &items, &["g".into(), "a".into()]);
        assert_eq!(ranked, vec![(2, 60), (0, 59), (1, 0)]);
        // Real query: score + boost combined, stable order.
        let ranked = rank("a", &items, &["g".into(), "a".into()]);
        assert_eq!(ranked, vec![(0, 109), (2, 95), (1, 35)]);
        // Non-matching rows drop out entirely.
        let ranked = rank("zzz", &items, &[]);
        assert!(ranked.is_empty());
    }

    #[test]
    fn non_ascii() {
        // BMP accents match case-insensitively like Qt does.
        assert!(fuzzy_score("büro", "Büro") > 0);
        assert!(fuzzy_score("BÜRO", "büro") > 0);
        // CJK subsequence.
        assert!(fuzzy_score("日本", "今日の日本語") > 0);
        // Non-BMP characters still subsequence-match.
        assert!(fuzzy_score("𝕏", "a𝕏b") > 0);
    }

    #[test]
    fn match_json_round_trip() {
        let out = match_json(
            "a",
            r#"{"items":[{"match":"alpha","id":"a"},
                         {"match":"beta","id":"b"},
                         {"match":"zzz","id":"z"}],
                "mru":["b"]}"#,
        )
        .unwrap();
        let v: Value = serde_json::from_str(&out).unwrap();
        let rows = v.as_array().unwrap();
        // alpha scores 50, beta 35+60=95, zzz unmatched.
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0]["index"], 1);
        assert_eq!(rows[0]["score"], 95);
        assert_eq!(rows[1]["index"], 0);
        assert_eq!(rows[1]["score"], 50);
    }

    #[test]
    fn match_json_tolerates_shape_gaps() {
        // Missing keys degrade to empty, never to an FFI fault.
        assert_eq!(match_json("x", "{}").unwrap(), "[]");
        assert!(match_json("x", "not json").is_err());
    }
}
