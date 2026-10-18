//! WKB01 — EasyList → WebKit/Safari content-blocker JSON converter.
//!
//! Feeds the webkit spine's `WebKitUserContentFilterStore` (WK18): the
//! store wants a compiled Safari-format rule list, so this module walks
//! the crate's own parsed rule model (`adblock::lists::parse_filter`
//! in debug mode, which retains `raw_line`) and drives upstream's
//! `adblock::content_blocking` translators — the same url-filter regex
//! translation, resource-type/load-type mapping and unsupported-option
//! rejection Brave ships for iOS.
//!
//! What upstream's `FilterSet::into_content_blocking` does not hand
//! back is *why* a rule vanished.  Here every rule-shaped input line
//! that produces no output lands in `dropped` with a stable reason
//! code — the drop list is a real output channel, not a log line.
//!
//! Output document (one JSON blob):
//!
//! ```json
//! {
//!   "rules":   [ {"action": {"type": "block"|"block-cookies"|
//!                            "css-display-none"|"ignore-previous-rules",
//!                           "selector": "..."},
//!                 "trigger": {"url-filter": "regex",
//!                             "resource-type": [...],
//!                             "load-type": [...],
//!                             "if-domain"|"unless-domain": [...]}} ],
//!   "dropped": [ {"rule": "original line", "why": "reason-code"} ],
//!   "meta":    {"lines": N, "parsed": N, "converted": N,
//!               "emitted": N, "dropped": N}
//! }
//! ```
//!
//! `rules` is ordered for Safari semantics — `ignore-previous-rules`
//! erases the actions of all *earlier* rules for a load — so every
//! `@@` exception sits after the block/css rules it may cancel, and
//! the first-party-document guard (ABP rules never block top-level
//! documents; Safari cannot scope that, so a trailing
//! `ignore-previous-rules` on first-party document loads approximates
//! it) comes last, exactly as upstream orders them.

use std::collections::HashSet;
use std::convert::TryInto;
use std::os::raw::c_char;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

use adblock::content_blocking::{
    self, CbRule, CbRuleCreationFailure, CbRuleEquivalent, CbType,
};
use adblock::lists::{parse_filter, FilterParseError, ParseOptions, ParsedLine};

use crate::to_c_string;

/// One source rule that produced no content-blocker output, paired
/// with a stable reason code (see drop_reason()).
pub(crate) struct DroppedRule {
    pub rule: String,
    pub why: &'static str,
}

/// The conversion result: ordered Safari rules plus the drop channel.
pub(crate) struct ContentBlockerOutput {
    /// Ready for `WebKitUserContentFilterStore` — serialize the
    /// `rules` field of the JSON document verbatim.
    pub rules: Vec<CbRule>,
    pub dropped: Vec<DroppedRule>,
    /// Rule-shaped input lines (comments/headers/blanks excluded).
    pub lines: usize,
    /// Lines that parsed as ABP rules.
    pub parsed: usize,
    /// Source rules that produced at least one output rule.
    pub converted: usize,
}

impl ContentBlockerOutput {
    /// Serializes to the documented JSON document.
    pub(crate) fn to_json(&self) -> String {
        serde_json::json!({
            "rules": &self.rules,
            "dropped": self
                .dropped
                .iter()
                .map(|d| serde_json::json!({"rule": d.rule, "why": d.why}))
                .collect::<Vec<_>>(),
            "meta": {
                "lines": self.lines,
                "parsed": self.parsed,
                "converted": self.converted,
                "emitted": self.rules.len(),
                "dropped": self.dropped.len(),
            },
        })
        .to_string()
    }
}

/// Mirrors `detect_filter_type`'s NotSupported cases: lines that are
/// not rules at all (blank, one-char junk, `!` comments, `[Adblock`
/// headers, `# ` cosmetic comments) are skipped without counting —
/// they were never convertible, so they are not "dropped".
fn is_non_rule_line(line: &str) -> bool {
    line.is_empty()
        || line.len() == 1
        || line.starts_with('!')
        || line.starts_with("[Adblock")
        || (line.starts_with('#') && line[1..].starts_with(char::is_whitespace))
}

fn drop_reason(err: &CbRuleCreationFailure) -> &'static str {
    use CbRuleCreationFailure as F;
    match err {
        F::NeedsDebugMode => "internal",
        F::UnlessAndIfDomainTogetherUnsupported => "mixed-if-unless-domain",
        F::NoSupportedNetworkOptions(_) => "unsupported-resource-types",
        F::NetworkRedirectUnsupported => "redirect",
        F::NetworkGenerichideUnsupported => "generichide",
        F::NetworkBadFilterUnsupported => "badfilter",
        F::NetworkCspUnsupported => "csp",
        F::NetworkRemoveparamUnsupported => "removeparam",
        F::FullRegexUnsupported => "full-regex",
        F::OptimizedRulesUnsupported => "optimized-rule",
        F::CosmeticEntitiesUnsupported => "entity-domain",
        F::CosmeticActionRulesNotSupported => "cosmetic-action",
        F::ScriptletInjectionsNotSupported => "scriptlet",
        F::RuleContainsNonASCII => "non-ascii",
        F::FromNotSupported => "from-to-option",
        F::ProceduralCosmeticFiltersUnsupported => "procedural-cosmetic",
        // Not marked non_exhaustive upstream — floor keeps a silent
        // variant addition from breaking the build on a dep bump.
        #[allow(unreachable_patterns)]
        _ => "unsupported",
    }
}

fn push_partitioned(rule: CbRule, rules: &mut Vec<CbRule>, exceptions: &mut Vec<CbRule>) {
    if rule.action.typ == CbType::IgnorePreviousRules {
        exceptions.push(rule);
    } else {
        rules.push(rule);
    }
}

/// Converts ABP/uBO filter-list text into the content-blocker output.
pub(crate) fn convert(text: &str) -> ContentBlockerOutput {
    let opts = ParseOptions::default();
    let mut parsed: Vec<ParsedLine<'_>> = Vec::new();
    let mut dropped: Vec<DroppedRule> = Vec::new();
    let mut lines = 0usize;

    // Pass 1 — parse every rule-shaped line through the crate's own
    // filter model.  debug = true retains raw_line, which the upstream
    // translators require (they re-read $domain= spellings from it,
    // and refuse to run without it).
    for raw in text.lines() {
        let line = raw.trim();
        if is_non_rule_line(line) {
            continue;
        }
        lines += 1;
        match parse_filter(line, /* debug = */ true, opts) {
            Ok(p) => parsed.push(p),
            Err(FilterParseError::Empty) => {}
            Err(e) => dropped.push(DroppedRule {
                rule: line.to_string(),
                why: match e {
                    FilterParseError::Network(_) => "network-parse-error",
                    FilterParseError::Cosmetic(_) => "cosmetic-parse-error",
                    _ => "unsupported-syntax",
                },
            }),
        }
    }

    // Pass 2 — $badfilter: upstream resolves it by filter id (the id
    // hash deliberately excludes the BAD_FILTER flag), so a
    // `X$badfilter` line cancels rule `X` wherever it appears.
    let mut bad_ids = HashSet::new();
    for p in &parsed {
        if let ParsedLine::Network(f) = p {
            if f.is_badfilter() {
                bad_ids.insert(f.get_id());
            }
        }
    }

    // Pass 3 — convert.  Network rules first, cosmetics after,
    // exceptions last (then the fp-document guard) — upstream's
    // ordering for ignore-previous-rules semantics.
    let mut rules: Vec<CbRule> = Vec::new();
    let mut exceptions: Vec<CbRule> = Vec::new();
    let mut converted = 0usize;
    let parsed_count = parsed.len();

    let (network, cosmetic): (Vec<ParsedLine<'_>>, Vec<ParsedLine<'_>>) = parsed
        .into_iter()
        .partition(|p| matches!(p, ParsedLine::Network(_)));

    for p in network {
        let ParsedLine::Network(f) = p else {
            continue;
        };
        let line = f.raw_line.as_deref().unwrap_or_default().to_string();
        if f.is_badfilter() {
            dropped.push(DroppedRule {
                rule: line,
                why: "badfilter-directive",
            });
        } else if bad_ids.contains(&f.get_id()) {
            dropped.push(DroppedRule {
                rule: line,
                why: "badfilter-disabled",
            });
        } else {
            match TryInto::<CbRuleEquivalent>::try_into(f) {
                Ok(equivalent) => {
                    converted += 1;
                    for rule in equivalent {
                        push_partitioned(rule, &mut rules, &mut exceptions);
                    }
                }
                Err(e) => dropped.push(DroppedRule {
                    rule: line,
                    why: drop_reason(&e),
                }),
            }
        }
    }

    for p in cosmetic {
        let ParsedLine::Cosmetic(f) = p else {
            continue;
        };
        let line = f.raw_line.as_deref().map_or("", String::as_str).to_string();
        match TryInto::<CbRule>::try_into(f) {
            Ok(rule) => {
                converted += 1;
                push_partitioned(rule, &mut rules, &mut exceptions);
            }
            Err(e) => dropped.push(DroppedRule {
                rule: line,
                why: drop_reason(&e),
            }),
        }
    }

    rules.extend(exceptions);
    if converted > 0 {
        rules.push(content_blocking::ignore_previous_fp_documents());
    }

    ContentBlockerOutput {
        rules,
        dropped,
        lines,
        parsed: parsed_count,
        converted,
    }
}

/// Converts `rules` (UTF-8 ABP/uBO filter-list text, `rules_len`
/// bytes) into the WebKit/Safari content-blocker JSON document.
///
/// Returns a heap-allocated C string the caller must release with
/// arktos_adblock_string_free(), or null on null/empty/non-UTF-8
/// input.  Never panics across the boundary.
///
/// # Safety
/// `rules` must point to `rules_len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn arktos_adblock_contentblocker_convert(
    rules: *const c_char,
    rules_len: usize,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        if rules.is_null() || rules_len == 0 {
            return ptr::null_mut();
        }
        let bytes = unsafe { std::slice::from_raw_parts(rules as *const u8, rules_len) };
        let Ok(text) = std::str::from_utf8(bytes) else {
            return ptr::null_mut();
        };
        to_c_string(convert(text).to_json())
    }))
    .unwrap_or(ptr::null_mut())
}

#[cfg(test)]
mod tests {
    use super::*;
    use adblock::content_blocking::{CbLoadType, CbResourceType};
    use std::ffi::CStr;

    const FIXTURE: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/fixtures/easylist-sample.txt");

    fn fixture() -> ContentBlockerOutput {
        convert(&std::fs::read_to_string(FIXTURE).unwrap())
    }

    fn drop_why<'a>(out: &'a ContentBlockerOutput, rule: &str) -> Option<&'a str> {
        out.dropped
            .iter()
            .find(|d| d.rule == rule)
            .map(|d| d.why)
    }

    fn blocks(out: &ContentBlockerOutput) -> impl Iterator<Item = &CbRule> {
        out.rules
            .iter()
            .filter(|r| r.action.typ == CbType::Block)
    }

    fn exceptions(out: &ContentBlockerOutput) -> impl Iterator<Item = &CbRule> {
        out.rules
            .iter()
            .filter(|r| r.action.typ == CbType::IgnorePreviousRules)
    }

    fn hides(out: &ContentBlockerOutput) -> impl Iterator<Item = &CbRule> {
        out.rules
            .iter()
            .filter(|r| r.action.typ == CbType::CssDisplayNone)
    }

    #[test]
    fn fixture_counts() {
        let out = fixture();
        // 22 rule-shaped lines; one (`#$#` snippet) fails cosmetic
        // parse, one badfilter pair, six option-class drops.
        assert_eq!(out.lines, 22);
        assert_eq!(out.parsed, 21);
        assert_eq!(out.converted, 12);
        // 5 blocks + 5 hides + 2 exceptions + fp-document guard.
        assert_eq!(out.rules.len(), 13);
        assert_eq!(out.dropped.len(), 10);
    }

    #[test]
    fn domain_anchor_and_resource_types() {
        let out = fixture();
        let ads = blocks(&out)
            .find(|r| r.trigger.url_filter.contains("ads\\.example"))
            .expect("||ads.example^ block rule");
        assert!(ads.trigger.url_filter.starts_with('^'));
        assert_eq!(ads.trigger.resource_type, None);

        let img = blocks(&out)
            .find(|r| r.trigger.url_filter.contains("track\\.example"))
            .expect("$image rule");
        assert_eq!(
            img.trigger.resource_type.as_ref().unwrap(),
            &HashSet::from([CbResourceType::Image])
        );

        let script = blocks(&out)
            .find(|r| r.trigger.url_filter.contains("scripts\\.example"))
            .expect("$script,third-party rule");
        assert_eq!(
            script.trigger.resource_type.as_ref().unwrap(),
            &HashSet::from([CbResourceType::Script])
        );
        assert_eq!(script.trigger.load_type, vec![CbLoadType::ThirdParty]);
    }

    #[test]
    fn domain_restrictions() {
        let out = fixture();
        let cdn = blocks(&out)
            .find(|r| r.trigger.url_filter.contains("cdn\\.example"))
            .unwrap();
        assert_eq!(
            cdn.trigger.if_domain.as_ref().unwrap(),
            &vec!["*foo.com".to_string(), "*bar.com".to_string()]
        );

        let neg = blocks(&out)
            .find(|r| r.trigger.url_filter.contains("neg\\.example"))
            .unwrap();
        assert_eq!(
            neg.trigger.unless_domain.as_ref().unwrap(),
            &vec!["*spam.example".to_string()]
        );
    }

    #[test]
    fn cosmetic_rules() {
        let out = fixture();
        let global = hides(&out)
            .find(|r| r.action.selector.as_deref() == Some(".ad-banner"))
            .unwrap();
        assert_eq!(global.trigger.url_filter, ".*");
        assert!(global.trigger.if_domain.is_none());
        assert!(global.trigger.unless_domain.is_none());

        // Cosmetic domains are emitted bare (upstream only adds the
        // `*` subdomain prefix on network $domain= rules).
        let scoped = hides(&out)
            .find(|r| r.action.selector.as_deref() == Some(".sponsored"))
            .unwrap();
        assert_eq!(
            scoped.trigger.if_domain.as_ref().unwrap(),
            &vec!["example.com".to_string()]
        );

        let negated = hides(&out)
            .find(|r| r.action.selector.as_deref() == Some("#top-ad"))
            .unwrap();
        assert_eq!(
            negated.trigger.unless_domain.as_ref().unwrap(),
            &vec!["mobile.example".to_string()]
        );

        // #@# unhide: Safari has no per-selector exception, so
        // upstream maps it to a css-display-none excluding the listed
        // domain — the approximation is pinned, not silently dropped.
        let unhide = hides(&out)
            .find(|r| r.action.selector.as_deref() == Some(".ad-exempt"))
            .unwrap();
        assert_eq!(
            unhide.trigger.unless_domain.as_ref().unwrap(),
            &vec!["example.com".to_string()]
        );

        // Procedural-looking pseudo-classes pass through as plain
        // selector text under this build (css-validation off);
        // WebKit ignores what it cannot apply.
        let procedural = hides(&out)
            .find(|r| r.action.selector.as_deref() == Some(".offer:has-text(buy)"))
            .unwrap();
        assert_eq!(procedural.trigger.url_filter, ".*");
    }

    #[test]
    fn exceptions_after_blocks_and_fp_guard_last() {
        let out = fixture();
        let last_block = out
            .rules
            .iter()
            .rposition(|r| r.action.typ == CbType::Block)
            .unwrap();
        let first_exception = out
            .rules
            .iter()
            .position(|r| r.action.typ == CbType::IgnorePreviousRules)
            .unwrap();
        assert!(first_exception > last_block);

        let exc: Vec<&CbRule> = exceptions(&out).collect();
        assert_eq!(exc.len(), 3); // two @@ rules + fp-document guard
        assert!(exc
            .iter()
            .any(|r| r.trigger.url_filter.contains("ok\\.js")));

        // The trailing guard approximates ABP's "never block the
        // top-level document" — ignore-previous-rules on first-party
        // document loads, after everything else.
        let last = out.rules.last().unwrap();
        assert_eq!(last.action.typ, CbType::IgnorePreviousRules);
        assert_eq!(
            last.trigger.resource_type.as_ref().unwrap(),
            &HashSet::from([CbResourceType::Document])
        );
        assert_eq!(last.trigger.load_type, vec![CbLoadType::FirstParty]);
    }

    #[test]
    fn drop_list_covers_every_unconvertible_rule() {
        let out = fixture();
        assert_eq!(
            drop_why(&out, "||redir.example/vast.xml$redirect=noop-vast"),
            Some("redirect")
        );
        assert_eq!(
            drop_why(&out, "||csp.example^$csp=script-src 'none'"),
            Some("csp")
        );
        assert_eq!(
            drop_why(&out, "||param.example^$removeparam=utm_source"),
            Some("removeparam")
        );
        assert_eq!(drop_why(&out, "/ads[0-9]+\\.js/"), Some("full-regex"));
        assert_eq!(
            drop_why(&out, "example.com##+js(set-constant)"),
            Some("scriptlet")
        );
        assert_eq!(
            drop_why(&out, "example.com##.x:style(color:red)"),
            Some("cosmetic-action")
        );
        assert_eq!(
            drop_why(&out, "example.com#$#body{border:0}"),
            Some("cosmetic-parse-error")
        );
        assert_eq!(
            drop_why(&out, "||mix.example^$domain=foo.com|~bar.example"),
            Some("mixed-if-unless-domain")
        );
    }

    #[test]
    fn badfilter_disables_target_and_is_accounted() {
        let out = fixture();
        assert_eq!(
            drop_why(&out, "||disabled.example^"),
            Some("badfilter-disabled")
        );
        assert_eq!(
            drop_why(&out, "||disabled.example^$badfilter"),
            Some("badfilter-directive")
        );
        assert!(!out
            .rules
            .iter()
            .any(|r| r.trigger.url_filter.contains("disabled")));
    }

    #[test]
    fn empty_and_comment_only_lists_convert_to_nothing() {
        let out = convert("! comment\n[Adblock Plus 2.0]\n# note\n\nx\n");
        assert!(out.rules.is_empty());
        assert!(out.dropped.is_empty());
        assert_eq!(out.converted, 0);
    }

    #[test]
    fn json_document_shape() {
        let out = fixture();
        let v: serde_json::Value = serde_json::from_str(&out.to_json()).unwrap();
        assert!(v["rules"].as_array().unwrap().len() == out.rules.len());
        let dropped = v["dropped"].as_array().unwrap();
        assert!(dropped
            .iter()
            .all(|d| d["rule"].is_string() && d["why"].is_string()));
        assert_eq!(
            v["meta"]["emitted"].as_u64().unwrap() as usize,
            out.rules.len()
        );
        assert_eq!(
            v["meta"]["dropped"].as_u64().unwrap() as usize,
            out.dropped.len()
        );
        assert_eq!(
            v["meta"]["converted"].as_u64().unwrap() as usize,
            out.converted
        );
    }

    #[test]
    fn ffi_surface() {
        let text = std::fs::read_to_string(FIXTURE).unwrap();
        let json = unsafe {
            arktos_adblock_contentblocker_convert(text.as_ptr().cast(), text.len())
        };
        assert!(!json.is_null());
        let v: serde_json::Value = unsafe {
            let s = CStr::from_ptr(json).to_str().unwrap().to_string();
            crate::arktos_adblock_string_free(json);
            serde_json::from_str(&s).unwrap()
        };
        assert!(v["rules"].as_array().unwrap().len() >= 13);
        assert_eq!(v["dropped"].as_array().unwrap().len(), 10);
        assert_eq!(v["meta"]["lines"].as_u64().unwrap(), 22);

        // Bad inputs degrade to null, never to a crash.
        assert!(unsafe { arktos_adblock_contentblocker_convert(ptr::null(), 10) }.is_null());
        assert!(unsafe { arktos_adblock_contentblocker_convert(text.as_ptr().cast(), 0) }
            .is_null());
        let bad_utf8 = [0xffu8, 0xfe, 0xfd];
        assert!(unsafe {
            arktos_adblock_contentblocker_convert(bad_utf8.as_ptr().cast(), bad_utf8.len())
        }
        .is_null());
    }

}
