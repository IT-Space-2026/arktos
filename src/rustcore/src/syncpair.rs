//! SYNC03 — serverless sync pairing.
//!
//! Two devices meet over a one-time invitation.  The inviter renders
//! a compact payload — QR text for cameras, a grouped base32
//! "pairing code" as the manual fallback — carrying:
//!
//!   * the inviter's sync endpoint (onion address, or a mem/test
//!     endpoint),
//!   * a 256-bit invitation token, and
//!   * an 8-byte hash fragment of the inviter's Ed25519 device key.
//!
//! The token does double duty: it is the Noise prologue (committing
//! both transcripts to the same invitation) and the seed of the
//! one-time restricted-discovery key that lets the joiner reach the
//! inviter's gated onion service for exactly this ceremony.
//!
//! Wire flow (each Noise message rides one transport frame):
//!
//!   joiner -> inviter   msg1  e_i || {"tok":<b64>}   (plaintext)
//!   inviter -> joiner   msg2  e_r || enc(s_r) || enc{hello}
//!   joiner -> inviter   msg3  enc(s_i) || enc{hello}
//!
//! Each `hello` carries the party's Ed25519 device pubkey, a
//! signature binding its Noise static to that identity, its own
//! sync endpoint, and (joiner) the permanent discovery key the
//! inviter should authorize.  Noise_XX authenticates both statics;
//! the Ed25519 signatures authenticate the statics as belonging to
//! the exchanged device identities; the QR's pubkey-hash fragment
//! lets the joiner refuse a wrong invite BEFORE the fingerprint
//! ceremony.
//!
//! Post-handshake the Noise transcript hash `h` becomes the 6-word /
//! 6-emoji fingerprint BOTH users compare out-of-band — the MITM
//! kill: a relayed handshake yields identical fingerprints, a
//! terminated one never does.  Only after both sides confirm does
//! the inviter ship the profile's sync-group secret + its own
//! discovery key inside the encrypted channel; the joiner installs
//! both and the inviter records the peer bound to THAT profile's
//! scope (member_scopes), never another profile's group.
//!
//! Sessions are per-data-root state machines driven by poll():
//! inbound links and frames land as transport events drained per
//! call, so an mw op or a UI timer advances the ceremony — no
//! background thread owns the flow.

use std::collections::{HashMap, VecDeque};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, LazyLock, Mutex};

use ring::signature::{UnparsedPublicKey, ED25519};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use tor_llcrypto::pk::curve25519::{PublicKey, StaticSecret};

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::noisexx::{self, Handshake, NoiseSession};
use crate::sync::{SyncEngine, SYNC_DIR};
use crate::synctransport::{
    self, SyncTransport, TransportEvent, SYNC_ONION_PORT,
};

/// File holding the per-device Noise static secret — the transport
/// subkey the Ed25519 identity signs into the handshake.  32 raw
/// bytes, 0600, same corrupt-means-loud contract as device.key.
const NOISE_KEY_FILE: &str = "noise.key";

/// Signature domain separator for the static-key binding.
const NSIG_DOMAIN: &[u8] = b"arktos-noise-static/v1";

/// The QR payload's text form marker + version.
const QR_PREFIX: &str = "arktos-pair1.";
const PAYLOAD_VERSION: u8 = 1;

/// Invite blob kinds: 1 = raw 35-byte v3 onion, 2 = opaque endpoint
/// string (the mem test backend).
const KIND_ONION: u8 = 1;
const KIND_ENDPOINT: u8 = 2;

const TOKEN_LEN: usize = 32;
const PK_HASH_LEN: usize = 8;

/// Session states, reported verbatim to the UI.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Phase {
    /// Inviter: waiting for a joiner to connect.
    WaitLink,
    /// Joiner: msg1 sent, waiting for msg2.
    WaitMsg2,
    /// Inviter: msg2 sent, waiting for msg3.
    WaitMsg3,
    /// Handshake complete; both users must confirm the fingerprint.
    Fingerprint,
    /// Both sides confirmed; inviter waits for the joiner's done.
    CommitWait,
    /// Joiner: both confirmed; waiting for the commit record.
    AwaitCommit,
    /// Pairing complete — peer is a member of the scope's group.
    Done,
    /// Terminal failure (user reject, bad peer, torn link).
    Failed,
    /// User-cancelled before completion.
    Cancelled,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Side {
    Inviter,
    Joiner,
}

/// What a verified peer told us inside the handshake.
struct PeerInfo {
    device_id: String,
    ed_pub: [u8; 32],
    onion: String,
    /// The peer's permanent discovery pub for OUR service, or "" on
    /// a name-auth backend.
    dkey: String,
}

struct PairSession {
    id: u64,
    root: PathBuf,
    side: Side,
    phase: Phase,
    /// The inviter's profile group this pairing binds to — carried
    /// inviter -> joiner inside the sealed hello.
    scope: String,
    token: [u8; TOKEN_LEN],
    /// Grant nickname for the one-time invite key ("pair-<hex16>").
    grant_nick: String,
    /// Our sync endpoint, told to the peer in the hello.
    our_endpoint: String,
    /// Joiner: the invite's expected inviter-key fragment — checked
    /// against the identity the handshake delivers.
    expected_pk: Option<[u8; PK_HASH_LEN]>,
    link: Option<u64>,
    hs: Option<Handshake>,
    sess: Option<NoiseSession>,
    peer: Option<PeerInfo>,
    local_ok: bool,
    peer_ok: bool,
    error: String,
    /// The QR/code strings the UI rendered — kept for status.
    qr: String,
    code: String,
}

impl PairSession {
    fn status_json(&self) -> Value {
        let state = match self.phase {
            Phase::WaitLink => "waiting",
            Phase::WaitMsg2 | Phase::WaitMsg3 => "handshaking",
            Phase::Fingerprint => "fingerprint",
            Phase::CommitWait | Phase::AwaitCommit => "committing",
            Phase::Done => "done",
            Phase::Failed => "failed",
            Phase::Cancelled => "cancelled",
        };
        let mut doc = json!({
            "pair_id": self.id,
            "side": if self.side == Side::Inviter { "inviter" } else { "joiner" },
            "state": state,
            "scope": self.scope,
            "local_confirmed": self.local_ok,
            "peer_confirmed": self.peer_ok,
        });
        if let Some(sess) = &self.sess {
            doc["fingerprint"] = fingerprint_json(&sess.fingerprint);
        }
        if let Some(peer) = &self.peer {
            doc["peer_device_id"] = json!(peer.device_id);
        }
        if !self.error.is_empty() {
            doc["error"] = json!(self.error);
        }
        if self.side == Side::Inviter && !self.qr.is_empty() {
            doc["qr"] = json!(self.qr);
            doc["code"] = json!(self.code);
        }
        doc
    }
}

static SESSIONS: LazyLock<Mutex<HashMap<u64, PairSession>>> =
    LazyLock::new(|| Mutex::new(HashMap::new()));
/// Inbound links not yet claimed by a msg1 token match.
static PENDING_LINKS: LazyLock<Mutex<VecDeque<(PathBuf, u64)>>> =
    LazyLock::new(|| Mutex::new(VecDeque::new()));
static NEXT_ID: AtomicU64 = AtomicU64::new(1);

fn sessions() -> std::sync::MutexGuard<'static, HashMap<u64, PairSession>> {
    SESSIONS.lock().unwrap_or_else(|e| e.into_inner())
}

fn hex(data: &[u8]) -> String {
    let mut s = String::with_capacity(data.len() * 2);
    for b in data {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

fn hex_decode(s: &str) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 {
        return None;
    }
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(s.get(i..i + 2)?, 16).ok())
        .collect()
}

// ---- small codecs -------------------------------------------------------------
//
// The invite blob crosses three renderings with no standard crate
// already in the tree for two of them (base64url, base32) — both are
// small table codecs kept local so payload parsing never depends on
// serde JSON either (the QR must stay compact).

const B64U: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

fn b64url_encode(data: &[u8]) -> String {
    let mut out = String::with_capacity(data.len().div_ceil(3) * 4);
    for c in data.chunks(3) {
        let n = ((c[0] as u32) << 16) | ((*c.get(1).unwrap_or(&0) as u32) << 8)
            | *c.get(2).unwrap_or(&0) as u32;
        out.push(B64U[(n >> 18) as usize & 63] as char);
        out.push(B64U[(n >> 12) as usize & 63] as char);
        if c.len() > 1 {
            out.push(B64U[(n >> 6) as usize & 63] as char);
        }
        if c.len() > 2 {
            out.push(B64U[n as usize & 63] as char);
        }
    }
    out
}

fn b64url_decode(s: &str) -> Option<Vec<u8>> {
    let val = |c: u8| -> Option<u32> {
        Some(match c {
            b'A'..=b'Z' => (c - b'A') as u32,
            b'a'..=b'z' => (c - b'a' + 26) as u32,
            b'0'..=b'9' => (c - b'0' + 52) as u32,
            b'-' => 62,
            b'_' => 63,
            _ => return None,
        })
    };
    let mut bits = 0u32;
    let mut nbits = 0u32;
    let mut out = Vec::with_capacity(s.len() * 3 / 4);
    for &c in s.as_bytes() {
        bits = (bits << 6) | val(c)?;
        nbits += 6;
        if nbits >= 8 {
            nbits -= 8;
            out.push((bits >> nbits) as u8);
        }
    }
    Some(out)
}

const B32: &[u8; 32] = b"abcdefghijklmnopqrstuvwxyz234567";

fn b32_encode(data: &[u8]) -> String {
    let mut out = String::with_capacity(data.len().div_ceil(5) * 8);
    let mut bits = 0u32;
    let mut nbits = 0u32;
    for &b in data {
        bits = (bits << 8) | b as u32;
        nbits += 8;
        while nbits >= 5 {
            nbits -= 5;
            out.push(B32[(bits >> nbits) as usize & 31] as char);
        }
    }
    if nbits > 0 {
        out.push(B32[(bits << (5 - nbits)) as usize & 31] as char);
    }
    out
}

fn b32_decode(s: &str) -> Option<Vec<u8>> {
    let mut bits = 0u32;
    let mut nbits = 0u32;
    let mut out = Vec::with_capacity(s.len() * 5 / 8);
    for c in s.bytes() {
        let v = match c {
            b'a'..=b'z' => (c - b'a') as u32,
            b'A'..=b'Z' => (c - b'A') as u32,
            b'2'..=b'7' => (c - b'2' + 26) as u32,
            _ => return None,
        };
        bits = (bits << 5) | v;
        nbits += 5;
        if nbits >= 8 {
            nbits -= 8;
            out.push((bits >> nbits) as u8);
        }
    }
    Some(out)
}

// ---- invite payload --------------------------------------------------------------

/// The one-time invitation: endpoint + token + a short fragment of
/// the inviter's device pubkey hash — exactly what the QR carries.
#[derive(Clone, Debug, PartialEq)]
pub struct Invite {
    /// Connect endpoint — "<onion>" or "mem:<name>" verbatim.
    pub endpoint: String,
    pub token: [u8; TOKEN_LEN],
    pub pk_hash: [u8; PK_HASH_LEN],
}

impl Invite {
    /// Compact wire blob: `[v][kind][field][token][pk]` — the bytes
    /// both renderings (QR base64url, manual base32) carry.
    fn blob(&self) -> RcResult<Vec<u8>> {
        let mut v = vec![PAYLOAD_VERSION];
        let onion = self
            .endpoint
            .strip_suffix(&format!(":{SYNC_ONION_PORT}"))
            .unwrap_or(&self.endpoint);
        if let Some(raw) = decode_onion(onion) {
            v.push(KIND_ONION);
            v.extend_from_slice(&raw);
        } else {
            let s = self.endpoint.as_bytes();
            if s.len() > 200 {
                return fail(RcStatus::InvalidArgument, "endpoint too long for invite");
            }
            v.push(KIND_ENDPOINT);
            v.push(s.len() as u8);
            v.extend_from_slice(s);
        }
        v.extend_from_slice(&self.token);
        v.extend_from_slice(&self.pk_hash);
        Ok(v)
    }

    fn from_blob(b: &[u8]) -> RcResult<Invite> {
        let want = 2 + TOKEN_LEN + PK_HASH_LEN;
        if b.len() < want || b[0] != PAYLOAD_VERSION {
            return fail(RcStatus::InvalidArgument, "not an arktos invite payload");
        }
        let (endpoint, used) = match b[1] {
            KIND_ONION => {
                if b.len() < 2 + 35 + TOKEN_LEN + PK_HASH_LEN {
                    return fail(RcStatus::InvalidArgument, "truncated onion invite");
                }
                (encode_onion(&b[2..2 + 35])?, 2 + 35)
            }
            KIND_ENDPOINT => {
                let len = *b.get(2).ok_or_else(|| Fail {
                    status: RcStatus::InvalidArgument,
                    msg: "truncated endpoint invite".into(),
                })? as usize;
                let s = b
                    .get(3..3 + len)
                    .ok_or_else(|| Fail {
                        status: RcStatus::InvalidArgument,
                        msg: "truncated endpoint invite".into(),
                    })?;
                (
                    String::from_utf8(s.to_vec()).map_err(|_| Fail {
                        status: RcStatus::InvalidArgument,
                        msg: "invite endpoint is not UTF-8".into(),
                    })?,
                    3 + len,
                )
            }
            _ => return fail(RcStatus::InvalidArgument, "unknown invite kind"),
        };
        if b.len() != used + TOKEN_LEN + PK_HASH_LEN {
            return fail(RcStatus::InvalidArgument, "invite payload length mismatch");
        }
        let mut token = [0u8; TOKEN_LEN];
        token.copy_from_slice(&b[used..used + TOKEN_LEN]);
        let mut pk_hash = [0u8; PK_HASH_LEN];
        pk_hash.copy_from_slice(&b[used + TOKEN_LEN..]);
        Ok(Invite {
            endpoint,
            token,
            pk_hash,
        })
    }

    /// The QR text — one line, base64url after the marker.
    pub fn qr_text(&self) -> RcResult<String> {
        Ok(format!("{}{}", QR_PREFIX, b64url_encode(&self.blob()?)))
    }

    /// The manual fallback — the same blob in lowercase base32,
    /// grouped six chars per cluster so a person can read it off
    /// one screen and type it on the other.
    pub fn manual_code(&self) -> RcResult<String> {
        let enc = b32_encode(&self.blob()?);
        let mut out = String::with_capacity(enc.len() + enc.len() / 6);
        for (i, c) in enc.chars().enumerate() {
            if i > 0 && i % 6 == 0 {
                out.push('-');
            }
            out.push(c);
        }
        Ok(out)
    }

    /// Accepts the QR text OR the grouped manual code — the UI's
    /// paste field and camera feed funnel through this one parse.
    pub fn parse(text: &str) -> RcResult<Invite> {
        let t = text.trim();
        if let Some(rest) = t.strip_prefix(QR_PREFIX) {
            let rest: String = rest
                .chars()
                .filter(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_')
                .collect();
            let blob = b64url_decode(&rest).ok_or_else(|| Fail {
                status: RcStatus::InvalidArgument,
                msg: "QR payload is not valid base64url".into(),
            })?;
            return Invite::from_blob(&blob);
        }
        // Manual path: strip everything that isn't base32.
        let filtered: String = t
            .chars()
            .filter(|c| c.is_ascii_alphanumeric())
            .collect::<String>()
            .to_lowercase();
        let blob = b32_decode(&filtered).ok_or_else(|| Fail {
            status: RcStatus::InvalidArgument,
            msg: "pairing code is not valid base32".into(),
        })?;
        Invite::from_blob(&blob)
    }

    /// The full connect endpoint (port appended for onions).
    fn connect_endpoint(&self) -> String {
        if self.endpoint.starts_with("mem:") {
            self.endpoint.clone()
        } else {
            format!("{}:{SYNC_ONION_PORT}", self.endpoint)
        }
    }
}

/// v3 onion (56-char base32, no ".onion") -> the raw 35 bytes
/// (32 pubkey + 2 checksum + 1 version); None when the string isn't
/// a well-formed onion.
fn decode_onion(addr: &str) -> Option<Vec<u8>> {
    let body = addr.strip_suffix(".onion").unwrap_or(addr);
    if body.len() != 56 {
        return None;
    }
    let raw = b32_decode(body)?;
    if raw.len() != 35 || raw[34] != 3 {
        return None;
    }
    Some(raw)
}

fn encode_onion(raw: &[u8]) -> RcResult<String> {
    if raw.len() != 35 || raw[34] != 3 {
        return fail(RcStatus::InvalidArgument, "bad onion field in invite");
    }
    Ok(format!("{}.onion", b32_encode(raw)))
}

/// The one-time discovery keypair derived from the invite token —
/// the joiner installs the secret, the inviter authorized the
/// public half when it minted the invite.
fn invite_dkey(token: &[u8; TOKEN_LEN]) -> StaticSecret {
    let seed = Sha256::digest(&[b"arktos-pair-dkey/v1" as &[u8], token].concat());
    StaticSecret::from(<[u8; 32]>::from(seed))
}

/// `descriptor:x25519:<base32>` — the restricted-discovery grant
/// spelling tor's static_keys takes.
fn dkey_public_str(secret: &StaticSecret) -> String {
    let pub_bytes = PublicKey::from(secret).to_bytes();
    format!("descriptor:x25519:{}", b32_encode(&pub_bytes).to_uppercase())
}

fn invite_nick(token: &[u8; TOKEN_LEN]) -> String {
    format!("pair-{:.16}", hex(token))
}

// ---- noise static identity ------------------------------------------------------

/// The device Noise static — a signed transport subkey, NOT the
/// Ed25519 identity itself (signing keys can't DH).  Generated once,
/// persisted raw at sync/noise.key 0600, corrupt = loud failure.
fn noise_keypair(dir: &Path) -> RcResult<(StaticSecret, PublicKey)> {
    let path = dir.join(NOISE_KEY_FILE);
    if path.exists() {
        let bytes = std::fs::read(&path)?;
        if bytes.len() == 32 {
            let mut raw = [0u8; 32];
            raw.copy_from_slice(&bytes);
            let s = StaticSecret::from(raw);
            return Ok((s.clone(), PublicKey::from(&s)));
        }
        return fail(
            RcStatus::Corrupt,
            "sync/noise.key is not a 32-byte x25519 secret",
        );
    }
    let raw = noisexx::rand_bytes::<32>()?;
    crate::store::atomic_write(&path, &raw)?;
    crate::store::tighten_perms(&path);
    let s = StaticSecret::from(raw);
    Ok((s.clone(), PublicKey::from(&s)))
}

/// The signed-hello doc each side sends inside its encrypted
/// handshake payload — Ed25519 binds the Noise static to the device
/// identity the payload claims.
fn hello_doc(
    engine: &SyncEngine,
    noise_pub: &PublicKey,
    endpoint: &str,
    scope: &str,
    dkey: &str,
) -> RcResult<Vec<u8>> {
    let ed = engine.public_key();
    let mut sig_msg = NSIG_DOMAIN.to_vec();
    sig_msg.extend_from_slice(noise_pub.as_bytes());
    let sig = engine.sign(&sig_msg);
    let doc = json!({
        "v": 1,
        "id": engine.device_id(),
        "ed": crate::mw::b64_encode(&ed),
        "ns": crate::mw::b64_encode(&sig),
        "on": endpoint,
        "sc": scope,
        "dk": dkey,
    });
    Ok(serde_json::to_vec(&doc).unwrap_or_default())
}

/// Verify a peer hello: signature under the claimed Ed pub over the
/// Noise static that actually arrived, device id = hex(ed).  Errors
/// carry the reason for the failure state.
fn verify_hello(doc: &[u8], remote_static: &PublicKey) -> RcResult<PeerInfo> {
    let v: Value = serde_json::from_slice(doc).map_err(|e| Fail {
        status: RcStatus::Crypto,
        msg: format!("peer hello is not JSON: {e}"),
    })?;
    let ed_b64 = v.get("ed").and_then(Value::as_str).unwrap_or("");
    let ed_vec = crate::mw::b64_decode(ed_b64).ok_or_else(|| Fail {
        status: RcStatus::Crypto,
        msg: "peer hello has no Ed25519 key".into(),
    })?;
    if ed_vec.len() != 32 {
        return fail(RcStatus::Crypto, "peer Ed25519 key is not 32 bytes");
    }
    let mut ed = [0u8; 32];
    ed.copy_from_slice(&ed_vec);
    let ns_b64 = v.get("ns").and_then(Value::as_str).unwrap_or("");
    let sig = crate::mw::b64_decode(ns_b64).unwrap_or_default();
    let mut sig_msg = NSIG_DOMAIN.to_vec();
    sig_msg.extend_from_slice(remote_static.as_bytes());
    UnparsedPublicKey::new(&ED25519, &ed)
        .verify(&sig_msg, &sig)
        .map_err(|_| Fail {
            status: RcStatus::Crypto,
            msg: "peer's Noise static is not signed by its claimed identity".into(),
        })?;
    let device_id = hex(&ed);
    if v.get("id").and_then(Value::as_str) != Some(device_id.as_str()) {
        return fail(RcStatus::Crypto, "peer id does not match its key");
    }
    let onion = v.get("on").and_then(Value::as_str).unwrap_or("").to_string();
    if onion.len() > 300 {
        return fail(RcStatus::Crypto, "peer endpoint too long");
    }
    Ok(PeerInfo {
        device_id,
        ed_pub: ed,
        onion,
        dkey: v.get("dk").and_then(Value::as_str).unwrap_or("").to_string(),
    })
}

// ---- fingerprint ----------------------------------------------------------------
//
// The transcript hash rendered for human comparison: 6 words from a
// 256-entry list (48 bits) + 6 emoji from a 64-entry list (36 bits)
// — ~84 bits of the SHA-256 handshake transcript.  Both sides show
// the same render only when the transcripts are byte-identical.

const FP_WORDS: [&str; 256] = [
    "amber", "anchor", "apple", "apron", "arch", "arrow", "aspen", "atlas",
    "bacon", "badge", "baker", "bamboo", "barn", "basil", "beach", "beacon",
    "berry", "birch", "bison", "blanket", "blaze", "blossom", "bolt", "bonnet",
    "boulder", "bracket", "bridge", "bronze", "brook", "brush", "bucket", "button",
    "cabin", "cactus", "candle", "canvas", "castle", "cedar", "chalk", "charm",
    "cherry", "cider", "cinder", "circle", "clover", "cobalt", "cocoa", "comet",
    "copper", "coral", "cotton", "cradle", "crane", "creek", "cricket", "crystal",
    "dagger", "daisy", "dancer", "dawn", "delta", "denim", "desert", "diamond",
    "dolphin", "domino", "donkey", "dragon", "drizzle", "dune", "dusty", "dynamo",
    "eagle", "ember", "engine", "estate", "fable", "falcon", "feather", "fennel",
    "ferry", "fiddle", "fig", "finch", "flame", "flint", "forest", "fossil",
    "fox", "frost", "galleon", "garden", "garnet", "gecko", "ginger", "glacier",
    "goblin", "golden", "goose", "granite", "grape", "gravel", "griffin", "grove",
    "hammer", "harbor", "hazel", "hedge", "heron", "hollow", "honey", "hornet",
    "hunter", "icicle", "indigo", "ingot", "island", "ivory", "jacket", "jaguar",
    "jasper", "jewel", "jigsaw", "jungle", "juniper", "kayak", "kelp", "kestrel",
    "kettle", "kingdom", "kitten", "kiwi", "ladder", "lagoon", "lantern", "laurel",
    "lemon", "lilac", "lily", "linden", "lizard", "lobster", "locket", "lumber",
    "magnet", "mango", "mantle", "maple", "marble", "meadow", "mercury", "meteor",
    "midnight", "miller", "minnow", "mirror", "moccasin", "monarch", "moss", "mulberry",
    "napkin", "nectar", "needle", "nickel", "noble", "north", "novel", "nutmeg",
    "oasis", "ocean", "olive", "onyx", "opal", "orange", "orchid", "otter",
    "paddle", "palace", "panther", "parrot", "pastel", "pebble", "pelican", "pepper",
    "pewter", "phoenix", "pickle", "pigeon", "pine", "plaid", "planet", "plover",
    "pocket", "poppy", "prairie", "quartz", "quill", "rabbit", "raccoon", "raptor",
    "raven", "ribbon", "ripple", "robin", "rocket", "rubble", "russet", "saddle",
    "saffron", "sailor", "salmon", "sandal", "satin", "scarlet", "shadow", "shamrock",
    "shelter", "sierra", "silver", "slate", "sparrow", "spindle", "spruce", "stable",
    "stone", "summit", "sunset", "talon", "tango", "temple", "thicket", "timber",
    "topaz", "torrent", "trout", "tulip", "tundra", "tunnel", "turtle", "twilight",
    "umber", "valley", "velvet", "violet", "viper", "vista", "walnut", "wander",
    "wasabi", "willow", "winter", "wonder", "yellow", "zephyr", "zigzag", "zodiac",
];

const FP_EMOJI: [&str; 64] = [
    "🍎", "🍐", "🍊", "🍋", "🍉", "🍇", "🍓", "🍑",
    "🥝", "🍍", "🥥", "🥑", "🌽", "🥕", "🍄", "🌰",
    "🌵", "🌲", "🌴", "🌊", "⭐", "🌙", "☀️", "⛅",
    "🌈", "❄️", "🔥", "⚡", "🌍", "🏔️", "🏝️", "🚀",
    "🛶", "⛵", "🚲", "🏰", "🗼", "🎡", "🎯", "🎲",
    "🎸", "🎺", "🔔", "📚", "✏️", "🔒", "🔑", "🧭",
    "⏰", "🔭", "🧲", "⚗️", "🧸", "🐝", "🦋", "🐢",
    "🦉", "🦊", "🐳", "🦩", "🐿️", "🦔", "🐧", "🦉‍⬛",
];

/// Render the transcript hash for side-by-side comparison.
pub fn fingerprint_json(h: &[u8; 32]) -> Value {
    let words: Vec<&str> = h[0..6].iter().map(|b| FP_WORDS[*b as usize]).collect();
    let emoji: Vec<&str> = h[6..12]
        .iter()
        .map(|b| FP_EMOJI[(*b as usize) & 63])
        .collect();
    json!({ "words": words, "emoji": emoji })
}

// ---- session machinery ----------------------------------------------------------

fn send(t: &Arc<dyn SyncTransport>, link: u64, payload: &[u8]) -> RcResult<()> {
    t.send(link, payload)
}

fn transport_for(root: &Path) -> RcResult<Arc<dyn SyncTransport>> {
    synctransport::running(root).ok_or_else(|| Fail {
        status: RcStatus::NotEnabled,
        msg: "sync transport is not started".into(),
    })
}

/// Our connect endpoint for the payload: the running transport's,
/// else (tor builds) the keystore-derived onion so invites work
/// before the transport has ever started.
fn our_endpoint(root: &Path) -> RcResult<String> {
    if let Some(t) = synctransport::running(root) {
        return t.endpoint();
    }
    #[cfg(feature = "synctor")]
    {
        return synctransport::tor::endpoint_for(&root.join(SYNC_DIR));
    }
    #[cfg(not(feature = "synctor"))]
    {
        fail(
            RcStatus::NotEnabled,
            "sync transport is not started and no tor backend exists",
        )
    }
}

// ---- public API -----------------------------------------------------------------

/// Inviter side: mint a one-time invite bound to `scope`'s sync
/// group, authorize its derived discovery key, open the session.
/// Returns the session id plus the QR text and manual code.
pub fn invite_begin(data_root: &Path, scope: &str) -> RcResult<Value> {
    if scope.len() > 128 {
        return fail(RcStatus::InvalidArgument, "scope too long");
    }
    let engine = SyncEngine::open(&data_root.to_string_lossy())?;
    let token = noisexx::rand_bytes::<TOKEN_LEN>()?;
    let pk = engine.public_key();
    let pk_hash: [u8; PK_HASH_LEN] = Sha256::digest(pk)[..PK_HASH_LEN]
        .try_into()
        .unwrap();
    let endpoint = our_endpoint(data_root)?;
    // The connect spelling the joiner dials stays in the payload;
    // the bare onion is what lands in the blob.
    let bare = endpoint
        .strip_suffix(&format!(":{SYNC_ONION_PORT}"))
        .unwrap_or(&endpoint)
        .to_string();
    let invite = Invite {
        endpoint: bare.clone(),
        token,
        pk_hash,
    };
    let grant = invite_nick(&token);
    // The one-time discovery key derived from the token gets the
    // pair-* grant — revoked as soon as the member grant lands.
    let invite_secret = invite_dkey(&token);
    synctransport::authorize_grant(data_root, &grant, &dkey_public_str(&invite_secret))?;
    let id = NEXT_ID.fetch_add(1, Ordering::SeqCst);
    let qr = invite.qr_text()?;
    let code = invite.manual_code()?;
    let s = PairSession {
        id,
        root: data_root.to_path_buf(),
        side: Side::Inviter,
        phase: Phase::WaitLink,
        scope: scope.to_string(),
        token,
        grant_nick: grant,
        our_endpoint: endpoint.clone(),
        expected_pk: None,
        link: None,
        hs: None,
        sess: None,
        peer: None,
        local_ok: false,
        peer_ok: false,
        error: String::new(),
        qr: qr.clone(),
        code: code.clone(),
    };
    sessions().insert(id, s);
    Ok(json!({
        "pair_id": id,
        "qr": qr,
        "code": code,
        "endpoint": endpoint,
        "scope": scope,
    }))
}

/// Joiner side: parse the scanned/typed payload, install the
/// invite-derived discovery key, connect, and send message 1.
/// Returns the session id — progress rides poll().
pub fn join_begin(data_root: &Path, payload_text: &str) -> RcResult<u64> {
    let invite = Invite::parse(payload_text)?;
    let engine = SyncEngine::open(&data_root.to_string_lossy())?;
    let own_pk = engine.public_key();
    if Sha256::digest(own_pk)[..PK_HASH_LEN] == invite.pk_hash {
        return fail(RcStatus::InvalidArgument, "cannot pair with this device itself");
    }
    let t = transport_for(data_root)?;
    // The invite-derived secret is our one-time client-auth key for
    // the inviter's service — installed before dialling.
    let secret = invite_dkey(&invite.token);
    let secret_bytes = secret.to_bytes();
    t.import_discovery_key(&invite.endpoint, &secret_bytes)
        .or_else(|e| {
            // Mem backends answer Unavailable — name auth needs nothing.
            if e.status == RcStatus::Unavailable {
                Ok(())
            } else {
                Err(e)
            }
        })?;
    let nick = invite_nick(&invite.token);
    let link = t.connect(&invite.connect_endpoint(), &nick)?;
    let (noise_s, _noise_p) = noise_keypair(&data_root.join(SYNC_DIR))?;
    let mut hs = Handshake::initiator(&invite.token, noise_s);
    // msg1's plaintext payload carries the token so the inviter can
    // select the matching prologue before hashing anything.
    let p1 = serde_json::to_vec(&json!({
        "v": 1, "tok": crate::mw::b64_encode(&invite.token),
    }))
    .unwrap_or_default();
    let m1 = hs.write_msg1(&p1)?;
    t.send(link, &m1)?;
    let id = NEXT_ID.fetch_add(1, Ordering::SeqCst);
    let s = PairSession {
        id,
        root: data_root.to_path_buf(),
        side: Side::Joiner,
        phase: Phase::WaitMsg2,
        scope: String::new(), // learned inside the sealed hello
        token: invite.token,
        grant_nick: nick,
        our_endpoint: t.endpoint().unwrap_or_default(),
        expected_pk: Some(invite.pk_hash),
        link: Some(link),
        hs: Some(hs),
        sess: None,
        peer: None,
        local_ok: false,
        peer_ok: false,
        error: String::new(),
        qr: String::new(),
        code: String::new(),
    };
    sessions().insert(id, s);
    Ok(id)
}

/// The user's fingerprint decision.  Both sides confirm before the
/// group secret moves; either side rejecting fails the ceremony.
pub fn confirm(data_root: &Path, pair_id: u64, ok: bool) -> RcResult<()> {
    let mut map = sessions();
    let s = map
        .get_mut(&pair_id)
        .filter(|s| s.root == data_root)
        .ok_or_else(|| Fail {
            status: RcStatus::NotFound,
            msg: format!("no pairing session {pair_id}"),
        })?;
    if !matches!(
        s.phase,
        Phase::Fingerprint | Phase::AwaitCommit | Phase::CommitWait
    ) {
        return fail(
            RcStatus::InvalidArgument,
            "pairing is not awaiting a fingerprint decision",
        );
    }
    let t = transport_for(data_root)?;
    let link = s.link.ok_or_else(|| Fail {
        status: RcStatus::Crypto,
        msg: "pairing link is gone".into(),
    })?;
    if !ok {
        let rec = s
            .sess
            .as_mut()
            .and_then(|n| n.seal(br#"{"t":"reject"}"#).ok());
        if let Some(r) = rec {
            let _ = send(&t, link, &r);
        }
        s.phase = Phase::Failed;
        s.error = "rejected by user".into();
        return Ok(());
    }
    s.local_ok = true;
    let rec = s
        .sess
        .as_mut()
        .and_then(|n| n.seal(br#"{"t":"confirm"}"#).ok())
        .ok_or_else(|| Fail {
            status: RcStatus::Crypto,
            msg: "no Noise session".into(),
        })?;
    send(&t, link, &rec)?;
    finalize_if_ready(s, &t)
}

/// The mutual-confirm branch: both flags up means the fingerprint
/// matched on BOTH ends — the inviter now ships the group secret
/// and its discovery key inside the channel.
fn finalize_if_ready(s: &mut PairSession, t: &Arc<dyn SyncTransport>) -> RcResult<()> {
    if !(s.local_ok && s.peer_ok) {
        return Ok(());
    }
    match s.side {
        Side::Inviter => {
            let root = s.root.clone();
            let engine = SyncEngine::open(&root.to_string_lossy())?;
            let peer = s.peer.take().ok_or_else(|| Fail {
                status: RcStatus::Crypto,
                msg: "pairing peer info missing".into(),
            })?;
            // The peer joins THIS profile's group only.
            let member_id = engine.member_add(&peer.ed_pub, &peer.onion, &s.scope)?;
            // Permanent grant replaces the one-time invite grant:
            // pair-<token> out, member-<device> in — that is the
            // profile's onion client-auth the peer now holds.
            let member_nick = format!("member-{}", &member_id[..16.min(member_id.len())]);
            let _ = synctransport::deauthorize_grant(&root, &s.grant_nick);
            synctransport::authorize_grant(
                &root,
                &member_nick,
                if peer.dkey.is_empty() {
                    &s.grant_nick // mem ignores the key but keeps parity shape
                } else {
                    &peer.dkey
                },
            )?;
            let gkey = engine.group_secret_get_or_create(&s.scope)?;
            // Our discovery key for the PEER's onion lets us reach
            // their service when roles reverse.
            let our_dkey = if peer.onion.is_empty() {
                String::new()
            } else {
                synctransport::discovery_key(&root, &peer.onion).unwrap_or_default()
            };
            let rec = serde_json::to_vec(&json!({
                "t": "commit",
                "gkey": hex(&gkey),
                "dk": our_dkey,
                "sc": s.scope,
            }))
            .unwrap_or_default();
            let sealed = s
                .sess
                .as_mut()
                .ok_or_else(|| Fail {
                    status: RcStatus::Crypto,
                    msg: "no Noise session".into(),
                })?
                .seal(&rec)?;
            send(t, s.link.unwrap_or(0), &sealed)?;
            s.peer = Some(peer);
            s.phase = Phase::CommitWait;
        }
        Side::Joiner => {
            // Both confirmed — now waiting on the commit record.
            s.phase = Phase::AwaitCommit;
        }
    }
    Ok(())
}

/// Inviter-side msg2 construction once msg1's token matched this
/// session.
fn inviter_on_msg1(s: &mut PairSession, t: &Arc<dyn SyncTransport>, msg: &[u8]) -> RcResult<()> {
    let pt = Handshake::peek_msg1_payload(msg)?.to_vec();
    let v: Value = serde_json::from_slice(&pt).unwrap_or(Value::Null);
    let tok = v
        .get("tok")
        .and_then(Value::as_str)
        .and_then(crate::mw::b64_decode)
        .unwrap_or_default();
    if tok.as_slice() != s.token.as_slice() {
        return fail(RcStatus::Crypto, "invitation token mismatch");
    }
    let (noise_s, _p) = noise_keypair(&s.root.join(SYNC_DIR))?;
    let mut hs = Handshake::responder(&s.token, noise_s);
    hs.read_msg1(msg)?;
    let engine = SyncEngine::open(&s.root.to_string_lossy())?;
    let hello = hello_doc(
        &engine,
        &hs.static_public(),
        &s.our_endpoint,
        &s.scope,
        "",
    )?;
    let m2 = hs.write_msg2(&hello)?;
    let link = s.link.unwrap_or(0);
    send(t, link, &m2)?;
    s.hs = Some(hs);
    s.phase = Phase::WaitMsg3;
    Ok(())
}

/// Joiner: msg2 arrives -> verify the static's signature + the QR's
/// pubkey-hash fragment, then answer msg3.
fn joiner_on_msg2(s: &mut PairSession, t: &Arc<dyn SyncTransport>, msg: &[u8]) -> RcResult<()> {
    let expected = s.expected_pk.unwrap_or([0u8; PK_HASH_LEN]);
    let mut hs = s.hs.take().ok_or_else(|| Fail {
        status: RcStatus::Crypto,
        msg: "joiner handshake missing".into(),
    })?;
    let pt = hs.read_msg2(msg)?;
    let rs = hs.remote_static().ok_or_else(|| Fail {
        status: RcStatus::Crypto,
        msg: "no remote static".into(),
    })?;
    let peer = verify_hello(&pt, &rs)?;
    // The QR's short pubkey hash is the pre-auth tripwire: a wrong
    // invite (photocopied and re-pointed) dies here, before the
    // fingerprint ceremony.
    if Sha256::digest(peer.ed_pub)[..PK_HASH_LEN] != expected {
        return fail(
            RcStatus::Crypto,
            "inviter identity does not match the invite payload",
        );
    }
    s.scope = {
        let v: Value = serde_json::from_slice(&pt).unwrap_or(Value::Null);
        v.get("sc").and_then(Value::as_str).unwrap_or("").to_string()
    };
    // Rotate off the invite-derived key NOW — the permanent key's
    // public half rides the hello so the inviter can grant it.
    let perm_dkey = synctransport::rotate_discovery_key(&s.root, &peer.onion)
        .unwrap_or_default();
    let engine = SyncEngine::open(&s.root.to_string_lossy())?;
    let hello = hello_doc(
        &engine,
        &hs.static_public(),
        &s.our_endpoint,
        "",
        &perm_dkey,
    )?;
    let m3 = hs.write_msg3(&hello)?;
    let link = s.link.unwrap_or(0);
    send(t, link, &m3)?;
    s.sess = Some(hs.into_transport());
    s.peer = Some(peer);
    s.phase = Phase::Fingerprint;
    Ok(())
}

/// Inviter: msg3 arrives -> verify, learn the member record.
fn inviter_on_msg3(s: &mut PairSession, _t: &Arc<dyn SyncTransport>, msg: &[u8]) -> RcResult<()> {
    let mut hs = s.hs.take().ok_or_else(|| Fail {
        status: RcStatus::Crypto,
        msg: "inviter handshake missing".into(),
    })?;
    let pt = hs.read_msg3(msg)?;
    let rs = hs.remote_static().ok_or_else(|| Fail {
        status: RcStatus::Crypto,
        msg: "no remote static".into(),
    })?;
    let peer = verify_hello(&pt, &rs)?;
    s.sess = Some(hs.into_transport());
    s.peer = Some(peer);
    s.phase = Phase::Fingerprint;
    Ok(())
}

/// A sealed pairing record arrived — confirm/reject/commit/done.
fn on_record(s: &mut PairSession, t: &Arc<dyn SyncTransport>, frame: &[u8]) -> RcResult<()> {
    let doc = {
        let sess = s.sess.as_mut().ok_or_else(|| Fail {
            status: RcStatus::Crypto,
            msg: "record before handshake".into(),
        })?;
        sess.open(frame)?
    };
    let v: Value = serde_json::from_slice(&doc).map_err(|e| Fail {
        status: RcStatus::Crypto,
        msg: format!("pairing record is not JSON: {e}"),
    })?;
    match v.get("t").and_then(Value::as_str).unwrap_or("") {
        "confirm" => {
            s.peer_ok = true;
            finalize_if_ready(s, t)
        }
        "reject" => {
            s.phase = Phase::Failed;
            s.error = "rejected by peer".into();
            Ok(())
        }
        "commit" => joiner_on_commit(s, t, &v),
        "done" => {
            s.phase = Phase::Done;
            Ok(())
        }
        other => fail(
            RcStatus::Crypto,
            format!("unknown pairing record \"{other}\""),
        ),
    }
}

/// Joiner: the commit record carries the profile's group secret and
/// the inviter's permanent discovery key for our service — install
/// both, member the inviter, authorize it, and close the loop.
fn joiner_on_commit(s: &mut PairSession, t: &Arc<dyn SyncTransport>, v: &Value) -> RcResult<()> {
    if s.phase != Phase::AwaitCommit {
        return fail(RcStatus::Crypto, "commit record out of order");
    }
    let gkey_hex = v.get("gkey").and_then(Value::as_str).unwrap_or("");
    let gkey_raw = hex_decode(gkey_hex).unwrap_or_default();
    if gkey_raw.len() != 32 {
        return fail(RcStatus::Crypto, "bad group secret in commit record");
    }
    let mut gkey = [0u8; 32];
    gkey.copy_from_slice(&gkey_raw);
    let scope = v
        .get("sc")
        .and_then(Value::as_str)
        .unwrap_or(&s.scope)
        .to_string();
    let engine = SyncEngine::open(&s.root.to_string_lossy())?;
    // Conflicting group secrets are a loud failure, not a swap.
    engine.group_secret_install(&scope, &gkey)?;
    let peer = s.peer.take().ok_or_else(|| Fail {
        status: RcStatus::Crypto,
        msg: "pairing peer info missing".into(),
    })?;
    // The peer joins OUR copy of that profile's group — and gets
    // our onion client-auth for the same scope's peers.
    let member_id = engine.member_add(&peer.ed_pub, &peer.onion, &scope)?;
    let member_nick = format!("member-{}", &member_id[..16.min(member_id.len())]);
    let dk = v.get("dk").and_then(Value::as_str).unwrap_or("");
    if !dk.is_empty() || s.our_endpoint.starts_with("mem:") {
        synctransport::authorize_grant(&s.root, &member_nick, dk)?;
    }
    let sealed = s
        .sess
        .as_mut()
        .ok_or_else(|| Fail {
            status: RcStatus::Crypto,
            msg: "no Noise session".into(),
        })?
        .seal(br#"{"t":"done"}"#)?;
    send(t, s.link.unwrap_or(0), &sealed)?;
    s.peer = Some(peer);
    s.scope = scope;
    s.phase = Phase::Done;
    Ok(())
}

/// Drain the transport and advance every session rooted at
/// `data_root`; returns the status doc for `pair_id` (or the list
/// when 0).  Pass-through events on links no session owns ride the
/// response so transport_poll callers lose nothing.
pub fn poll(data_root: &Path, pair_id: u64) -> RcResult<Value> {
    let Some(t) = synctransport::running(data_root) else {
        // No transport: sessions still report their state (an
        // inviter session is legitimately waiting for a start).
        return Ok(match pair_id {
            0 => json!({ "sessions": sessions_json(data_root) }),
            id => sessions()
                .get(&id)
                .filter(|s| s.root == data_root)
                .map(|s| s.status_json())
                .unwrap_or_else(|| json!({ "pair_id": id, "state": "gone" })),
        });
    };
    let events = t.poll();
    let mut passthrough: Vec<Value> = Vec::new();
    for ev in events {
        match ev {
            TransportEvent::Incoming { link, .. } => {
                PENDING_LINKS
                    .lock()
                    .unwrap_or_else(|e| e.into_inner())
                    .push_back((data_root.to_path_buf(), link));
            }
            TransportEvent::Frame { link, payload } => {
                on_frame(data_root, &t, link, &payload, &mut passthrough)?;
            }
            TransportEvent::LinkClosed { link, reason } => {
                let mut map = sessions();
                for s in map.values_mut() {
                    if s.root == data_root
                        && s.link == Some(link)
                        && !matches!(
                            s.phase,
                            Phase::Done | Phase::Failed | Phase::Cancelled
                        )
                    {
                        s.phase = Phase::Failed;
                        s.error = format!("pairing link closed: {reason}");
                    }
                }
            }
            other => passthrough.push(json!({"event": format!("{other:?}")})),
        }
    }
    let out = match pair_id {
        0 => json!({ "sessions": sessions_json(data_root) }),
        id => sessions()
            .get(&id)
            .filter(|s| s.root == data_root)
            .map(|s| s.status_json())
            .unwrap_or_else(|| json!({ "pair_id": id, "state": "gone" })),
    };
    let mut out = out;
    if !passthrough.is_empty() {
        out["events"] = json!(passthrough);
    }
    Ok(out)
}

/// Route one inbound frame: bound session first, else a pending
/// inbound link whose msg1 token claims an inviter session.
fn on_frame(
    root: &Path,
    t: &Arc<dyn SyncTransport>,
    link: u64,
    payload: &[u8],
    passthrough: &mut Vec<Value>,
) -> RcResult<()> {
    // A bound session consumes it.
    {
        let mut map = sessions();
        if let Some(s) = map
            .values_mut()
            .find(|s| s.root == root && s.link == Some(link))
        {
            return drive(s, t, payload);
        }
    }
    // A pending inbound link: msg1's token picks its inviter.
    let pending = PENDING_LINKS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .contains(&(root.to_path_buf(), link));
    if !pending {
        passthrough.push(json!({"event": "frame", "link_id": link}));
        return Ok(());
    }
    let tok = Handshake::peek_msg1_payload(payload)
        .ok()
        .and_then(|pt| serde_json::from_slice::<Value>(pt).ok())
        .and_then(|v| {
            v.get("tok")
                .and_then(Value::as_str)
                .and_then(crate::mw::b64_decode)
        })
        .unwrap_or_default();
    let mut map = sessions();
    let hit = map
        .values_mut()
        .find(|s| {
            s.root == root
                && s.side == Side::Inviter
                && s.phase == Phase::WaitLink
                && tok.as_slice() == s.token.as_slice()
        });
    let Some(s) = hit else {
        // Wrong invite for this device — drop the link, the
        // pending entry, and any chance of probing further.
        let _ = t.close_link(link);
        PENDING_LINKS
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .retain(|(r, l)| !(r == root && *l == link));
        return Ok(());
    };
    PENDING_LINKS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .retain(|(r, l)| !(r == root && *l == link));
    s.link = Some(link);
    drive(s, t, payload)
}

/// Advance one bound session on a fresh frame.
fn drive(s: &mut PairSession, t: &Arc<dyn SyncTransport>, payload: &[u8]) -> RcResult<()> {
    let r = match s.phase {
        Phase::WaitLink => inviter_on_msg1(s, t, payload),
        Phase::WaitMsg2 => joiner_on_msg2(s, t, payload),
        Phase::WaitMsg3 => inviter_on_msg3(s, t, payload),
        Phase::Fingerprint | Phase::AwaitCommit | Phase::CommitWait => {
            on_record(s, t, payload)
        }
        _ => Ok(()),
    };
    if let Err(e) = &r {
        if !matches!(s.phase, Phase::Done | Phase::Cancelled) {
            s.phase = Phase::Failed;
            s.error = e.msg.clone();
        }
    }
    r
}

/// User cancels an in-flight ceremony — best-effort reject to the
/// peer so its UI doesn't hang.
pub fn cancel(data_root: &Path, pair_id: u64) -> RcResult<()> {
    let mut map = sessions();
    let s = map
        .get_mut(&pair_id)
        .filter(|s| s.root == data_root)
        .ok_or_else(|| Fail {
            status: RcStatus::NotFound,
            msg: format!("no pairing session {pair_id}"),
        })?;
    if s.side == Side::Inviter {
        let _ = synctransport::deauthorize_grant(&s.root, &s.grant_nick);
    }
    if let Ok(t) = transport_for(data_root) {
        if let (Some(sess), Some(link)) = (s.sess.as_mut(), s.link) {
            if let Ok(rec) = sess.seal(br#"{"t":"reject"}"#) {
                let _ = send(&t, link, &rec);
            }
        }
        if let Some(link) = s.link {
            let _ = t.close_link(link);
        }
    }
    s.phase = Phase::Cancelled;
    Ok(())
}

fn sessions_json(root: &Path) -> Vec<Value> {
    sessions()
        .values()
        .filter(|s| s.root == root)
        .map(|s| s.status_json())
        .collect()
}

/// Status without driving — for UIs that poll pair_list.
pub fn list(data_root: &Path) -> Vec<Value> {
    sessions_json(data_root)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The blob round-trips through BOTH renderings — QR base64url
    /// and grouped base32 — and garbage never parses.
    #[test]
    fn invite_payload_round_trip() {
        let inv = Invite {
            endpoint: "abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrstuvwx.onion"
                .to_string(),
            token: [7u8; 32],
            pk_hash: [9u8; 8],
        };
        let qr = inv.qr_text().unwrap();
        assert!(qr.starts_with(QR_PREFIX));
        let code = inv.manual_code().unwrap();
        assert!(code.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-'));
        assert_eq!(Invite::parse(&qr).unwrap(), inv);
        assert_eq!(Invite::parse(&code).unwrap(), inv);
        // Grouped-with-spaces or pasted-uppercase also parses.
        assert_eq!(Invite::parse(&code.to_uppercase().replace('-', " ")).unwrap(), inv);
        assert!(Invite::parse("arktos-pair1.!!!").is_err());
        assert!(Invite::parse("hello world").is_err());
    }

    /// A mem endpoint rides the length-prefixed kind — the e2e path.
    #[test]
    fn invite_payload_mem_endpoint() {
        let inv = Invite {
            endpoint: "mem:device-a".to_string(),
            token: [1u8; 32],
            pk_hash: [2u8; 8],
        };
        assert_eq!(Invite::parse(&inv.qr_text().unwrap()).unwrap(), inv);
        assert_eq!(Invite::parse(&inv.manual_code().unwrap()).unwrap(), inv);
    }

    /// The invite-derived key is deterministic and domain-separated:
    /// same token -> same keypair, different token -> different.
    #[test]
    fn invite_key_is_token_deterministic() {
        let a1 = invite_dkey(&[1u8; 32]);
        let a2 = invite_dkey(&[1u8; 32]);
        let b = invite_dkey(&[2u8; 32]);
        assert_eq!(PublicKey::from(&a1), PublicKey::from(&a2));
        assert_ne!(PublicKey::from(&a1), PublicKey::from(&b));
    }

    // ---- e2e over the mem loopback -----------------------------------------

    fn tmpdir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-syncpair-{}-{}-{}",
            std::process::id(),
            tag,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    /// Serialize against every test that moves shared transport
    /// state — the LOOPS table and SESSIONS map are process-global.
    fn guard() -> std::sync::MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    /// Pump both roots until both sessions reach `want` (or a bound
    /// of polls expires) — returns the two status docs.
    fn pump_until(
        a: &Path,
        b: &Path,
        id_a: u64,
        id_b: u64,
        want: &str,
    ) -> (Value, Value) {
        let mut sa = Value::Null;
        let mut sb = Value::Null;
        for _ in 0..600 {
            sa = poll(a, id_a).unwrap();
            sb = poll(b, id_b).unwrap();
            if sa["state"] == want && sb["state"] == want {
                return (sa, sb);
            }
            // A "failed" target IS the expected end state — only a
            // non-failed target should bail early on a failure.
            if want != "failed" && (sa["state"] == "failed" || sb["state"] == "failed") {
                panic!("pairing failed early: {sa} / {sb}");
            }
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
        panic!("never reached {want}: {sa} / {sb}");
    }

    /// The whole ceremony over the loopback transport: invite ->
    /// Noise_XX -> equal fingerprints -> mutual confirm -> member
    /// rows bound to the SAME profile scope on both devices, the
    /// group secret transferred, and the one-time invite grant
    /// swapped for the member grant.
    #[test]
    fn pairing_end_to_end_mem() {
        let _g = guard();
        let a = tmpdir("pa");
        let b = tmpdir("pb");
        let _ta = synctransport::start(&a, "mem", "pdevA").unwrap();
        let _tb = synctransport::start(&b, "mem", "pdevB").unwrap();

        let inv = invite_begin(&a, "work").unwrap();
        let id_a = inv["pair_id"].as_u64().unwrap();
        assert_eq!(inv["scope"], "work");
        // Both renderings exist; the joiner uses the manual code.
        let code = inv["code"].as_str().unwrap().to_string();
        let id_b = join_begin(&b, &code).unwrap();

        // Handshake completes -> both sides show a fingerprint and
        // it is byte-identical (the MITM check).
        let (sa, sb) = pump_until(&a, &b, id_a, id_b, "fingerprint");
        assert_eq!(sa["fingerprint"], sb["fingerprint"]);
        assert_eq!(sa["fingerprint"]["words"].as_array().unwrap().len(), 6);
        assert_eq!(sa["fingerprint"]["emoji"].as_array().unwrap().len(), 6);
        assert!(sa["scope"] == "work");
        // The joiner learned the inviter's scope inside the sealed
        // hello — not from any out-of-band field.
        assert_eq!(sb["scope"], "work");

        // Both confirm -> commit -> done on both ends.
        confirm(&a, id_a, true).unwrap();
        confirm(&b, id_b, true).unwrap();
        let (sa, sb) = pump_until(&a, &b, id_a, id_b, "done");
        assert_eq!(sa["state"], "done");
        assert_eq!(sb["state"], "done");
        // Each side's status names the OTHER device.
        let joiner_dev = sa["peer_device_id"].as_str().unwrap().to_string();
        let inviter_dev = sb["peer_device_id"].as_str().unwrap().to_string();
        assert_ne!(joiner_dev, inviter_dev);

        // Membership persisted into the profile's sync store on
        // BOTH devices, bound to the "work" scope only.
        let ea = SyncEngine::open(&a.to_string_lossy()).unwrap();
        let eb = SyncEngine::open(&b.to_string_lossy()).unwrap();
        let ma = ea.members_list().unwrap();
        let mb = eb.members_list().unwrap();
        assert_eq!(ma.len(), 1);
        assert_eq!(mb.len(), 1);
        // Inviter's member row is the joiner; joiner's is the inviter.
        assert_eq!(ma[0].device_id, joiner_dev);
        assert_eq!(mb[0].device_id, inviter_dev);
        assert_eq!(ma[0].scopes, vec!["work".to_string()]);
        assert_eq!(mb[0].scopes, vec!["work".to_string()]);

        // The group secret crossed: same bytes, both stores.
        assert_eq!(
            ea.group_secret("work").unwrap(),
            eb.group_secret("work").unwrap()
        );
        assert!(ea.group_secret("work").unwrap().is_some());

        // The one-time invite grant is gone; the member grant
        // replaced it (a fresh pair-* connect would be refused).
        let authed_a = synctransport::authorized_set(&a.join(SYNC_DIR));
        assert!(authed_a.keys().all(|k| k.starts_with("member-")));
        let t = synctransport::running(&b).unwrap();
        assert!(t.connect("mem:pdevA", "pair-anything").is_err());

        let _ = synctransport::stop(&a);
        let _ = synctransport::stop(&b);
    }

    /// A reject on EITHER side fails the ceremony on both — no
    /// group secret crosses on a mismatch.
    #[test]
    fn pairing_reject_aborts() {
        let _g = guard();
        let a = tmpdir("ra");
        let b = tmpdir("rb");
        let _ta = synctransport::start(&a, "mem", "rA").unwrap();
        let _tb = synctransport::start(&b, "mem", "rB").unwrap();
        let inv = invite_begin(&a, "personal").unwrap();
        let id_a = inv["pair_id"].as_u64().unwrap();
        let id_b = join_begin(&b, inv["qr"].as_str().unwrap()).unwrap();
        let (_, _) = pump_until(&a, &b, id_a, id_b, "fingerprint");
        confirm(&a, id_a, true).unwrap();
        confirm(&b, id_b, false).unwrap();
        let (sa, sb) = pump_until(&a, &b, id_a, id_b, "failed");
        assert_eq!(sa["state"], "failed");
        assert_eq!(sb["state"], "failed");
        let ea = SyncEngine::open(&a.to_string_lossy()).unwrap();
        assert!(ea.group_secret("personal").unwrap().is_none()
            || ea.members_list().unwrap().is_empty());
        let _ = synctransport::stop(&a);
        let _ = synctransport::stop(&b);
    }

    /// A forged payload (right shape, wrong token) never reaches a
    /// pending invite — the joiner is refused at the link, not at
    /// the app layer.
    #[test]
    fn pairing_wrong_token_refused() {
        let _g = guard();
        let a = tmpdir("wa");
        let b = tmpdir("wb");
        let _ta = synctransport::start(&a, "mem", "wA").unwrap();
        let _tb = synctransport::start(&b, "mem", "wB").unwrap();
        let _inv = invite_begin(&a, "work").unwrap();
        // A different token means a different derived grant — the
        // mem gate refuses the nickname outright.
        let mut evil = Invite {
            endpoint: "mem:wA".into(),
            token: [0xEE; 32],
            pk_hash: [0; 8],
        };
        // Grant the forged nickname too — even with Tor-layer entry
        // the msg1 token check drops the link.
        let t = synctransport::running(&a).unwrap();
        t.authorize(&invite_nick(&evil.token), "").unwrap();
        evil.pk_hash = [0xEE; 8];
        let _ = join_begin(&b, &evil.qr_text().unwrap()).unwrap();
        // The inviter never binds the link to a session — it is
        // closed on the token mismatch; the invite session stays
        // open for the REAL joiner.
        let sa = poll(&a, 0).unwrap();
        let sessions = sa["sessions"].as_array().unwrap();
        assert_eq!(sessions.len(), 1);
        assert_eq!(sessions[0]["state"], "waiting");
        let _ = synctransport::stop(&a);
        let _ = synctransport::stop(&b);
    }

    /// The word/emoji tables are well-formed: 256 unique words, 64
    /// unique emoji, every index reachable.
    #[test]
    fn fingerprint_tables_wellformed() {
        let unique: std::collections::BTreeSet<_> = FP_WORDS.iter().collect();
        assert_eq!(unique.len(), 256);
        let unique_e: std::collections::BTreeSet<_> = FP_EMOJI.iter().collect();
        assert_eq!(unique_e.len(), 64);
        let doc = fingerprint_json(&[0xAB; 32]);
        assert_eq!(doc["words"].as_array().unwrap().len(), 6);
        assert_eq!(doc["emoji"].as_array().unwrap().len(), 6);
        // Deterministic: same transcript, same render.
        assert_eq!(fingerprint_json(&[0xAB; 32]), doc);
    }
}
