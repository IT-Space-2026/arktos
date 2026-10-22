//! RC20: the `updates` module — the update-check's policy + parse
//! half.  UpdateChecker keeps the network fetch Qt-side (UPD01: a
//! manual Help > Check for Updates against the GitHub releases API);
//! everything the reply's bytes steer — manifest parse, the
//! trusted-download-URL gate, version compare, platform asset pick —
//! plus the persisted check state live here, engine-agnostic.
//!
//! State decision (the spec's "decide and record why"): the state is
//! three keys + a channel name, so it lives as TYPED HELPERS over the
//! RC02 settings store — not a new file format.  The scope is the
//! GLOBAL settings (settings.json / the global ini in the no-rust
//! lane), never a persona's: the check is application-level (one
//! shared binary, one release feed) and "updates" is deliberately
//! absent from MPROF04's profile-group table, so a ProfileSettings()
//! write lands global in BOTH lanes.  The keys:
//!
//!   updates/lastCheckMs    int — when a check last fired
//!   updates/snoozeUntilMs  int — notify suppression deadline
//!                          (0/absent = not snoozed)
//!   updates/skippedVersion str — a tag the user dismissed;
//!                          should_notify filters it
//!   updates/channel        str — "stable" (default) | "beta"
//!
//! Channel rules: the app ships the stable lane — /releases/latest
//! only ever serves the newest non-draft non-prerelease manifest,
//! so gating on the parsed draft/prerelease flags is a no-op on the
//! real endpoint and a hardening on any future /releases listing or
//! self-hosted feed.  A "beta" channel accepts prereleases; drafts
//! are never an offer on either channel.
//!
//! Nothing here binds a profile, so — like torctl (RC14) — this is a
//! rc_* module only, not an AcUpdates facade handle.  Facade callers
//! reach the state through ac_settings(core, NULL) + the rc_updates_*
//! state ops, which take the same RcSettings handle type.

use crate::error::{fail, RcResult, RcStatus};
use crate::settings::{RcSettings, Value};

const K_LAST_CHECK: &str = "updates/lastCheckMs";
const K_SNOOZE_UNTIL: &str = "updates/snoozeUntilMs";
const K_SKIPPED: &str = "updates/skippedVersion";
const K_CHANNEL: &str = "updates/channel";

/// The release channel the app offers updates on.  Only "stable" is
/// wired today (the api url is hardwired to /releases/latest); "beta"
/// exists so a manifest's prerelease flag has a spelled-out rule
/// rather than an accidental answer.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Channel {
    #[default]
    Stable,
    Beta,
}

impl Channel {
    pub fn as_str(&self) -> &'static str {
        match self {
            Channel::Stable => "stable",
            Channel::Beta => "beta",
        }
    }
}

/// "" / "stable" -> Stable (the absent-key default), "beta" -> Beta;
/// anything else is an error — an unrecognized channel must never
/// silently widen what gets offered.
pub fn channel_from_str(s: &str) -> RcResult<Channel> {
    match s.trim().to_lowercase().as_str() {
        "" | "stable" => Ok(Channel::Stable),
        "beta" => Ok(Channel::Beta),
        _ => fail(RcStatus::InvalidArgument, "unknown update channel"),
    }
}

/// Whether a manifest with these flags counts as an offer on the
/// channel.  Stable rejects drafts AND prereleases; beta rejects
/// drafts only.
pub fn channel_accepts(channel: Channel, draft: bool, prerelease: bool) -> bool {
    match channel {
        Channel::Stable => !draft && !prerelease,
        Channel::Beta => !draft,
    }
}

/// UpdateChecker::normalizedVersion, byte-for-byte: trim, strip a
/// leading v/V, cut at the first whitespace-or-'(' (the app's own
/// "0.12 (Git: …)" decoration), then cut at the first '-' or '+'
/// (prerelease/build suffixes).  A cut at index 0 is NOT taken —
/// "(1.2" and "-1.2" pass through, like the C++ `indexOf > 0` guard.
pub fn normalize_version(version: &str) -> String {
    let mut normalized = version.trim();
    if normalized.starts_with('v') || normalized.starts_with('V') {
        normalized = &normalized[1..];
    }
    if let Some(i) = normalized.find(|c: char| c.is_whitespace() || c == '(') {
        if i > 0 {
            normalized = &normalized[..i];
        }
    }
    if let Some(i) = normalized.find(|c| c == '-' || c == '+') {
        if i > 0 {
            normalized = &normalized[..i];
        }
    }
    normalized.to_string()
}

/// One semver part per Qt's toLongLong(base-10, full-parse): plain
/// ASCII digits with an optional leading sign.  Empty parts are
/// skipped by the caller, matching the C++ `part.isEmpty() continue`.
fn parse_part(part: &str) -> Option<i64> {
    if part.is_empty() {
        return None;
    }
    part.parse::<i64>().ok()
}

/// UpdateChecker::compareVersions — -1/0/+1.  Both sides normalize
/// first; when every dotted part parses numerically the compare runs
/// elementwise with missing parts counting as 0 ("1.0" == "1.0.0").
/// A non-numeric side falls back to a string compare of the
/// normalized forms — it still answers "same release" honestly.
pub fn compare_versions(a: &str, b: &str) -> i32 {
    let na = normalize_version(a);
    let nb = normalize_version(b);
    let parts_a: Vec<&str> = na.split('.').filter(|p| !p.is_empty()).collect();
    let parts_b: Vec<&str> = nb.split('.').filter(|p| !p.is_empty()).collect();
    let nums_a: Option<Vec<i64>> = parts_a.iter().map(|p| parse_part(p)).collect();
    let nums_b: Option<Vec<i64>> = parts_b.iter().map(|p| parse_part(p)).collect();
    if let (Some(x), Some(y)) = (nums_a, nums_b) {
        for i in 0..x.len().max(y.len()) {
            let xv = x.get(i).copied().unwrap_or(0);
            let yv = y.get(i).copied().unwrap_or(0);
            if xv != yv {
                return if xv < yv { -1 } else { 1 };
            }
        }
        return 0;
    }
    na.cmp(&nb) as i32
}

/// A release asset that survived the trust gate.
#[derive(Clone, Debug, PartialEq)]
pub struct ReleaseAsset {
    pub name: String,
    /// The URL verbatim as the manifest carried it — already gated.
    pub url: String,
    pub size: i64,
}

/// A parsed release manifest — UpdateChecker::Result's payload half,
/// plus the draft/prerelease flags the channel gate needs (the C++
/// parser ignores them today; the FFI carries them so the gate can
/// run where the parse happened).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Release {
    /// tag_name verbatim ("" when absent/non-string).
    pub version: String,
    /// 'name' freeform title ("" when absent/non-string).
    pub title: String,
    /// html_url — stored only when http/https, else "".
    pub page_url: String,
    pub draft: bool,
    pub prerelease: bool,
    pub assets: Vec<ReleaseAsset>,
}

/// The URL decomposition isTrustedDownloadUrl needs: (scheme,
/// host, has_userinfo) all lowercase — or None when the input is
/// relative/scheme-less (a manifest field is never a browser
/// fromUserInput guess, so anything without a real scheme is out).
/// QUrl is lenient — this split is deliberately shallow: it does not
/// validate the path/query at all, mirroring Qt's tolerance while
/// deciding only what the gate consults.
fn split_url(s: &str) -> Option<(String, String, bool)> {
    let colon = s.find(':')?;
    let scheme = &s[..colon];
    if scheme.is_empty()
        || !scheme.bytes().next().unwrap().is_ascii_alphabetic()
        || !scheme
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'+' || b == b'-' || b == b'.')
    {
        return None;
    }
    let rest = &s[colon + 1..];
    if !rest.starts_with("//") {
        // Opaque URI (javascript:/data:/mailto:) — a scheme but no
        // authority: host is empty, which fails every host rule.
        return Some((scheme.to_lowercase(), String::new(), false));
    }
    let authority = &rest[2..];
    let end = authority
        .find(|c| c == '/' || c == '?' || c == '#')
        .unwrap_or(authority.len());
    let authority = &authority[..end];
    let has_userinfo = authority.contains('@');
    let hostport = match authority.rfind('@') {
        Some(i) => &authority[i + 1..],
        None => authority,
    };
    let host = if hostport.starts_with('[') {
        match hostport.find(']') {
            Some(i) => &hostport[1..i],
            None => hostport,
        }
    } else {
        match hostport.find(':') {
            Some(i) => &hostport[..i],
            None => hostport,
        }
    };
    Some((scheme.to_lowercase(), host.to_lowercase(), has_userinfo))
}

/// UpdateChecker::isTrustedDownloadUrl — the release payload is
/// server input, so only URLs on the GitHub release surface
/// (github.com / *.github.com / *.githubusercontent.com over https)
/// or on the api url's own host (loopback tests, self-hosted GHE —
/// never a scheme downgrade below the api's) are download targets.
/// Relative, credential-bearing and off-host URLs refuse.
pub fn is_trusted_download_url(url: &str, api_url: &str) -> bool {
    let (scheme, host, userinfo) = match split_url(url) {
        Some(u) => u,
        None => return false,
    };
    if host.is_empty() || userinfo {
        return false;
    }
    let github_host = host == "github.com"
        || host.ends_with(".github.com")
        || host.ends_with(".githubusercontent.com");
    if github_host {
        return scheme == "https";
    }
    let (api_scheme, api_host, _) = match split_url(api_url) {
        Some(u) => u,
        None => return false,
    };
    !api_host.is_empty()
        && host == api_host
        && (scheme == "https" || (scheme == "http" && api_scheme == "http"))
}

/// UpdateChecker::parseReleaseJson — the manifest must be a JSON
/// object; tag_name/name/html_url are taken only when strings
/// (QJsonValue::toString on non-strings yields "", which this
/// reproduces), html_url only when http/https, and every asset's
/// browser_download_url must pass the trust gate or the row drops.
/// Assets with an empty name drop like the C++.
pub fn parse_release(payload: &[u8], api_url: &str) -> RcResult<Release> {
    let v: serde_json::Value =
        serde_json::from_slice(payload).map_err(|e| crate::error::Fail {
            status: RcStatus::InvalidArgument,
            msg: format!("release manifest: {e}"),
        })?;
    if !v.is_object() {
        return fail(RcStatus::InvalidArgument, "release manifest: not an object");
    }
    let mut release = Release {
        version: v.get("tag_name").and_then(|x| x.as_str()).unwrap_or("").into(),
        title: v.get("name").and_then(|x| x.as_str()).unwrap_or("").into(),
        draft: v.get("draft").and_then(|x| x.as_bool()).unwrap_or(false),
        prerelease: v
            .get("prerelease")
            .and_then(|x| x.as_bool())
            .unwrap_or(false),
        ..Default::default()
    };
    let page = v.get("html_url").and_then(|x| x.as_str()).unwrap_or("");
    if let Some((scheme, ..)) = split_url(page) {
        if scheme == "http" || scheme == "https" {
            release.page_url = page.to_string();
        }
    }
    if let Some(assets) = v.get("assets").and_then(|x| x.as_array()) {
        for asset in assets {
            let name = asset
                .get("name")
                .and_then(|x| x.as_str())
                .unwrap_or("")
                .to_string();
            if name.is_empty() {
                continue;
            }
            let url = asset
                .get("browser_download_url")
                .and_then(|x| x.as_str())
                .unwrap_or("")
                .to_string();
            if !is_trusted_download_url(&url, api_url) {
                continue;
            }
            // QVariant(double).toLongLong truncates toward zero;
            // non-numbers read as 0, like the C++.
            let size = asset
                .get("size")
                .and_then(|x| x.as_f64())
                .map(|f| f as i64)
                .unwrap_or(0);
            release.assets.push(ReleaseAsset { name, url, size });
        }
    }
    Ok(release)
}

/// The platform pick — UpdateChecker::PlatformPick.  `candidates` is
/// indices into the release's asset list, preference-ordered.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PlatformPick {
    pub candidates: Vec<usize>,
    pub ambiguous: bool,
}

fn matching(
    assets: &[ReleaseAsset],
    suffix: &str,
    prefer_installer: bool,
) -> Vec<usize> {
    let mut installer_named = Vec::new();
    let mut plain = Vec::new();
    for (i, a) in assets.iter().enumerate() {
        let lower = a.name.to_lowercase();
        if !lower.ends_with(suffix) {
            continue;
        }
        if prefer_installer && lower.contains("installer") {
            installer_named.push(i);
        } else {
            plain.push(i);
        }
    }
    installer_named.extend(plain);
    installer_named
}

/// UpdateChecker::platformAssetsFor — windows prefers
/// *installer*.exe then any *.exe (never a zip), macOS *.dmg, and
/// everything else goes through the /etc/os-release family table:
/// deb on the Debian ids, rpm on the rpm ids, flatpak as the generic
/// fallback, ambiguous when the distro is unrecognized so the
/// dialog offers the candidates explicitly.
pub fn platform_pick(
    os: &str,
    os_release_id_like: &str,
    assets: &[ReleaseAsset],
) -> PlatformPick {
    let mut pick = PlatformPick::default();
    if os == "windows" {
        pick.candidates = matching(assets, ".exe", true);
        return pick;
    }
    if os == "macos" {
        pick.candidates = matching(assets, ".dmg", false);
        return pick;
    }
    let id_like = os_release_id_like.to_lowercase();
    let debian_family = ["debian", "ubuntu", "mint", "raspbian"]
        .iter()
        .any(|s| id_like.contains(s));
    let rpm_family = ["fedora", "rhel", "centos", "opensuse", "suse", "mageia"]
        .iter()
        .any(|s| id_like.contains(s));
    let debs = matching(assets, ".deb", false);
    let rpms = matching(assets, ".rpm", false);
    let flatpaks = matching(assets, ".flatpak", false);
    if debian_family && !rpm_family {
        pick.candidates = debs;
        pick.candidates.extend(flatpaks);
    } else if rpm_family && !debian_family {
        pick.candidates = rpms;
        pick.candidates.extend(flatpaks);
    } else {
        pick.candidates = flatpaks;
        pick.candidates.extend(debs);
        pick.candidates.extend(rpms);
        pick.ambiguous = pick.candidates.len() > 1;
    }
    pick
}

/// The persisted update-check state — all fields default to "never
/// checked, nothing snoozed/skipped, stable channel".
#[derive(Clone, Debug, Default, PartialEq)]
pub struct UpdateState {
    pub last_check_ms: i64,
    pub snooze_until_ms: i64,
    pub skipped_version: String,
    pub channel: Channel,
}

impl UpdateState {
    pub fn to_json(&self) -> serde_json::Value {
        serde_json::json!({
            "last_check_ms": self.last_check_ms,
            "snooze_until_ms": self.snooze_until_ms,
            "skipped_version": self.skipped_version,
            "channel": self.channel.as_str(),
        })
    }
}

/// Reads the state off a settings handle — intended for the GLOBAL
/// scope (a profile-bound handle would silently scope the keys to
/// the persona, which nothing writes; the FFI doc comments say so).
/// Tolerant reads: a wrong-typed or corrupt key degrades to its
/// default, never an error — a stale value must not wedge the
/// checker.
pub fn read_state(s: &RcSettings) -> RcResult<UpdateState> {
    let mut state = UpdateState {
        last_check_ms: s.get_int(K_LAST_CHECK).unwrap_or(0),
        snooze_until_ms: s.get_int(K_SNOOZE_UNTIL).unwrap_or(0),
        skipped_version: s.get_string(K_SKIPPED).unwrap_or_default(),
        channel: Channel::Stable,
        ..Default::default()
    };
    if let Ok(name) = s.get_string(K_CHANNEL) {
        state.channel = channel_from_str(&name).unwrap_or(Channel::Stable);
    }
    Ok(state)
}

/// Stamps the last-check time (ms since epoch).
pub fn record_check(s: &RcSettings, now_ms: i64) -> RcResult<()> {
    s.set(K_LAST_CHECK, Value::Int(now_ms))
}

/// Snooze notifications until `until_ms`; <= 0 clears the snooze.
pub fn snooze(s: &RcSettings, until_ms: i64) -> RcResult<()> {
    if until_ms <= 0 {
        s.remove(K_SNOOZE_UNTIL)
    } else {
        s.set(K_SNOOZE_UNTIL, Value::Int(until_ms))
    }
}

/// The tag the user dismissed ("" or None clears).
pub fn set_skipped(s: &RcSettings, version: Option<&str>) -> RcResult<()> {
    match version {
        Some(v) if !v.is_empty() => s.set(K_SKIPPED, Value::Str(v.to_string())),
        _ => s.remove(K_SKIPPED),
    }
}

/// Persists the channel name — validated, so a typo can't widen the
/// offer set.
pub fn set_channel(s: &RcSettings, name: &str) -> RcResult<()> {
    let channel = channel_from_str(name)?;
    if channel == Channel::Stable {
        // The default needs no key — keep the file sparse.
        s.remove(K_CHANNEL)
    } else {
        s.set(K_CHANNEL, Value::Str(channel.as_str().to_string()))
    }
}

/// The notify verdict: a release on this profile's channel that is
/// newer than the local version, not the tag the user skipped, and
/// past the snooze deadline.  `remote` is the manifest's tag_name
/// verbatim; an unparseable/empty remote never notifies.
pub fn should_notify(
    state: &UpdateState,
    remote: &str,
    local: &str,
    now_ms: i64,
) -> bool {
    if normalize_version(remote).is_empty() {
        return false;
    }
    if compare_versions(remote, local) <= 0 {
        return false;
    }
    if !state.skipped_version.is_empty() && state.skipped_version == remote {
        return false;
    }
    state.snooze_until_ms <= now_ms
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;

    fn tmpdir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-updates-{}-{}-{}",
            std::process::id(),
            tag,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn normalize_version_matches_cpp_rules() {
        // The tst_updatechecker matrix verbatim.
        assert_eq!(normalize_version("v1.2.3"), "1.2.3");
        assert_eq!(normalize_version("1.2.3-rc1+build5"), "1.2.3");
        assert_eq!(normalize_version("0.12 (Git: abc123 4)"), "0.12");
        assert_eq!(normalize_version("  0.9  "), "0.9");
        // Edge forms the C++ `indexOf > 0` guard passes through.
        assert_eq!(normalize_version("V2.0"), "2.0");
        assert_eq!(normalize_version("(1.2"), "(1.2");
        assert_eq!(normalize_version("-1.2"), "-1.2");
        assert_eq!(normalize_version(""), "");
        assert_eq!(normalize_version("v"), "");
        // Suffix cut before decoration cut?  The C++ cuts whitespace
        // first — "1.2 -rc" -> "1.2" at the space, never reaching '-'.
        assert_eq!(normalize_version("1.2 -rc"), "1.2");
    }

    #[test]
    fn compare_versions_matrix() {
        // The C++ test legs plus the boundary forms around them.
        assert_eq!(compare_versions("1.2.3", "1.2.3"), 0);
        assert_eq!(compare_versions("v0.13", "0.12"), 1);
        assert_eq!(compare_versions("0.2", "0.12"), -1);
        assert_eq!(compare_versions("1.0", "1.0.0"), 0);
        assert_eq!(compare_versions("2.0-rc1", "1.9"), 1);
        assert_eq!(compare_versions("nightly", "nightly"), 0);
        assert!(compare_versions("beta", "alpha") > 0);
        assert!(compare_versions("alpha", "beta") < 0);
        // Missing parts count as 0 either direction; empty parts skip.
        assert_eq!(compare_versions("1.2", "1.2.0.0"), 0);
        assert_eq!(compare_versions("1.2.1", "1.2"), 1);
        assert_eq!(compare_versions("1..2", "1.2"), 0);
        // Numeric beats lexicographic ("9" < "10" numerically).
        assert_eq!(compare_versions("9.0", "10.0"), -1);
        // A non-numeric side drops BOTH sides to string compare.
        assert_ne!(compare_versions("1.2.x", "1.2.3"), 0);
        assert_eq!(compare_versions("", ""), 0);
    }

    #[test]
    fn channel_rules() {
        assert_eq!(channel_from_str("").unwrap(), Channel::Stable);
        assert_eq!(channel_from_str("stable").unwrap(), Channel::Stable);
        assert_eq!(channel_from_str(" STABLE ").unwrap(), Channel::Stable);
        assert_eq!(channel_from_str("beta").unwrap(), Channel::Beta);
        assert!(channel_from_str("nightly").is_err());
        assert!(channel_accepts(Channel::Stable, false, false));
        assert!(!channel_accepts(Channel::Stable, false, true));
        assert!(!channel_accepts(Channel::Stable, true, false));
        assert!(channel_accepts(Channel::Beta, false, true));
        assert!(!channel_accepts(Channel::Beta, true, false));
    }

    fn release_json(tag: &str, assets: &str) -> String {
        format!(
            "{{\"tag_name\":\"{tag}\",\"name\":\"Arktos {tag}\",\"html_url\":\"https://github.com/o/r/releases/x\",\"assets\":[{assets}]}}"
        )
    }

    fn asset_json(name: &str, url: &str) -> String {
        format!(
            "{{\"name\":\"{name}\",\"browser_download_url\":\"{url}\",\"size\":100}}"
        )
    }

    #[test]
    fn manifest_parse_fixtures() {
        let api = "https://api.github.com/repos/o/r/releases/latest";
        // Malformed payloads and non-objects refuse like the C++.
        assert!(parse_release(b"this is not json {", api).is_err());
        assert!(parse_release(b"[1,2]", api).is_err());
        assert!(parse_release(b"\"x\"", api).is_err());

        let payload = release_json(
            "v9.9",
            &[
                asset_json("a.deb", "https://github.com/o/r/releases/download/v9.9/a.deb"),
                asset_json("b.dmg", "https://objects.githubusercontent.com/x/b.dmg"),
                asset_json("evil.deb", "javascript:alert(1)"),
                asset_json("off.deb", "https://evil.example/e.deb"),
                asset_json("http.deb", "http://github.com/e.deb"),
                asset_json("cred.deb", "https://u:p@github.com/e.deb"),
                asset_json("", "https://github.com/e.deb"),
            ]
            .join(","),
        );
        let r = parse_release(payload.as_bytes(), api).unwrap();
        assert_eq!(r.version, "v9.9");
        assert_eq!(r.title, "Arktos v9.9");
        assert_eq!(r.page_url, "https://github.com/o/r/releases/x");
        assert!(!r.draft && !r.prerelease);
        // Only the two trusted assets survive, manifest order kept.
        assert_eq!(r.assets.len(), 2);
        assert_eq!(r.assets[0].name, "a.deb");
        assert_eq!(r.assets[1].name, "b.dmg");
        assert_eq!(r.assets[0].size, 100);

        // html_url gates on scheme — a javascript: page is dropped.
        let bad = payload.replace(
            "https://github.com/o/r/releases/x",
            "javascript:alert(1)",
        );
        let r2 = parse_release(bad.as_bytes(), api).unwrap();
        assert_eq!(r2.page_url, "");

        // Non-string fields read as "" like QJsonValue::toString.
        let nonstr = "{\"tag_name\":5,\"name\":null,\"assets\":\"x\"}";
        let r3 = parse_release(nonstr.as_bytes(), api).unwrap();
        assert_eq!(r3.version, "");
        assert_eq!(r3.title, "");
        assert!(r3.assets.is_empty());

        // Draft/prerelease flags ride out for the channel gate.
        let pre = "{\"tag_name\":\"v1.0\",\"draft\":true,\"prerelease\":true}";
        let r4 = parse_release(pre.as_bytes(), api).unwrap();
        assert!(r4.draft && r4.prerelease);

        // Loopback api: same-host http assets are trusted (tests,
        // self-hosted GHE); other hosts and downgrades are not.
        let loop_api = "http://127.0.0.1:9/r";
        assert!(is_trusted_download_url("http://127.0.0.1/a.deb", loop_api));
        assert!(!is_trusted_download_url("http://127.0.0.2/a.deb", loop_api));
        let ghe = "https://ghe.corp.example/api";
        assert!(!is_trusted_download_url("http://ghe.corp.example/a.deb", ghe));
        assert!(is_trusted_download_url("https://ghe.corp.example/a.deb", ghe));
        // Relative and garbage inputs refuse.
        assert!(!is_trusted_download_url("a.deb", api));
        assert!(!is_trusted_download_url("/x/a.deb", api));
        assert!(!is_trusted_download_url("", api));
        assert!(!is_trusted_download_url("file:///etc/passwd", api));
        assert!(!is_trusted_download_url("data:x;base64,AA==", api));
    }

    #[test]
    fn platform_pick_matrix() {
        let names = [
            ("Arktos 9.9 Installer.exe", "https://github.com/x/e.exe"),
            ("arktos-9.9-win.zip", "https://github.com/x/w.zip"),
            ("Arktos-9.9.dmg", "https://github.com/x/m.dmg"),
            ("arktos_9.9_amd64.deb", "https://github.com/x/l.deb"),
            ("arktos-9.9.x86_64.rpm", "https://github.com/x/l.rpm"),
            ("arktos-9.9.flatpak", "https://github.com/x/l.flatpak"),
        ];
        let assets: Vec<ReleaseAsset> = names
            .iter()
            .map(|(n, u)| ReleaseAsset {
                name: n.to_string(),
                url: u.to_string(),
                size: 1,
            })
            .collect();
        // Windows: installer first, zip never a candidate.
        let win = platform_pick("windows", "", &assets);
        assert_eq!(win.candidates, vec![0]);
        assert!(!win.ambiguous);
        let mac = platform_pick("macos", "", &assets);
        assert_eq!(mac.candidates, vec![2]);
        let deb = platform_pick("linux", "debian", &assets);
        assert_eq!(deb.candidates, vec![3, 5]);
        assert!(!deb.ambiguous);
        let ub = platform_pick("linux", "ubuntu debian", &assets);
        assert_eq!(ub.candidates, vec![3, 5]);
        let rpm = platform_pick("linux", "fedora", &assets);
        assert_eq!(rpm.candidates, vec![4, 5]);
        // Unknown distro: everything plausible, flatpak leading,
        // flagged ambiguous.
        let unk = platform_pick("linux", "plan9", &assets);
        assert!(unk.ambiguous);
        assert_eq!(unk.candidates, vec![5, 3, 4]);
        // A second installer-unnamed exe sorts after the installer.
        let mut two = assets.clone();
        two.push(ReleaseAsset {
            name: "arktos-plain.exe".into(),
            url: "https://github.com/x/p.exe".into(),
            size: 1,
        });
        let win2 = platform_pick("windows", "", &two);
        assert_eq!(win2.candidates, vec![0, 6]);
        // No host package → empty candidates (dialog falls back to
        // the release page).
        assert!(platform_pick("linux", "debian", &[]).candidates.is_empty());
    }

    #[test]
    fn state_roundtrip_over_global_settings() {
        let root = tmpdir("state");
        let s = RcSettings::open(root.to_str().unwrap(), None).unwrap();
        let st = read_state(&s).unwrap();
        assert_eq!(st, UpdateState::default());

        record_check(&s, 1_700_000_000_000).unwrap();
        snooze(&s, 1_800_000_000_000).unwrap();
        set_skipped(&s, Some("v9.9")).unwrap();
        set_channel(&s, "beta").unwrap();
        let st = read_state(&s).unwrap();
        assert_eq!(st.last_check_ms, 1_700_000_000_000);
        assert_eq!(st.snooze_until_ms, 1_800_000_000_000);
        assert_eq!(st.skipped_version, "v9.9");
        assert_eq!(st.channel, Channel::Beta);

        // The keys are exactly where the C++ mirror writes them.
        assert_eq!(s.get_int(K_LAST_CHECK).unwrap(), 1_700_000_000_000);
        assert_eq!(s.get_string(K_CHANNEL).unwrap(), "beta");

        // Clears: snooze <= 0 removes, empty skipped removes, stable
        // removes (default needs no key).
        snooze(&s, 0).unwrap();
        set_skipped(&s, Some("")).unwrap();
        set_channel(&s, "stable").unwrap();
        let st = read_state(&s).unwrap();
        assert_eq!(st.snooze_until_ms, 0);
        assert_eq!(st.skipped_version, "");
        assert_eq!(st.channel, Channel::Stable);
        assert!(!s.contains(K_SNOOZE_UNTIL).unwrap());
        assert!(!s.contains(K_CHANNEL).unwrap());
        assert!(set_channel(&s, "nightly").is_err());
    }

    #[test]
    fn notify_verdict() {
        let mut st = UpdateState::default();
        assert!(should_notify(&st, "v2.0", "1.0", 100));
        assert!(!should_notify(&st, "v1.0", "1.0", 100));
        assert!(!should_notify(&st, "v0.9", "1.0", 100));
        assert!(!should_notify(&st, "", "1.0", 100));
        // A non-semver tag is not refused — it falls to the string
        // compare like the C++ ("garbage" > "1.0"), so the verdict
        // stays parity-honest rather than stricter than the checker.
        assert!(should_notify(&st, "garbage(", "1.0", 100));
        assert!(!should_notify(&st, "0.alpha", "1.0", 100));
        st.skipped_version = "v2.0".into();
        assert!(!should_notify(&st, "v2.0", "1.0", 100));
        assert!(should_notify(&st, "v2.1", "1.0", 100));
        st.skipped_version.clear();
        st.snooze_until_ms = 200;
        assert!(!should_notify(&st, "v2.0", "1.0", 100));
        assert!(should_notify(&st, "v2.0", "1.0", 200));
    }
}
