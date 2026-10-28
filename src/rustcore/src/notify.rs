//! Change-notification callback registry — the Rust half of the
//! callback -> Qt-signal bridge.  One process-wide callback (a fn
//! pointer + opaque userdata) is enough for Arktos: the Qt side
//! (RustCoreBridge) re-emits each topic as a queued Qt signal, so a
//! Rust worker thread never touches the GUI thread directly.
//!
//! RC07 adds the facade lane: each AcCore registers its own
//! (module, profile_id) callback — the profile the mutating call was
//! bound to, stamped by the ac_* wrappers through a thread-local
//! hint.  A mutation that arrives over the legacy rc_* surface has
//! no bound profile and reports NULL.  Both lanes fire for every
//! emit: the legacy callback keeps its topic-string contract
//! unchanged until RC08 retires it.
//!
//! Emitters MUST call emit() after releasing the store lock — the
//! callback re-enters this library on the Qt side.

use std::cell::RefCell;
use std::collections::BTreeSet;
use std::ffi::CString;
use std::os::raw::{c_char, c_void};
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

use serde_json::{json, Value};

pub type RcChangeCallback = unsafe extern "C" fn(*const c_char, *mut c_void);

/// The facade callback: (module topic, bound profile id or NULL,
/// userdata) — the "dispatched per module+profile" contract
/// arktos_core.h documents.
pub type AcChangeCallback =
    unsafe extern "C" fn(*const c_char, *const c_char, *mut c_void);

static CALLBACK: Mutex<Option<(RcChangeCallback, usize)>> = Mutex::new(None);

/// Facade subscribers keyed by their AcCore token (the handle
/// pointer as usize — unique while the core lives).  Copied out
/// before dispatch so a callback that re-registers or shuts its
/// core down mid-emit cannot deadlock.
static CORE_CALLBACKS: Mutex<Vec<(usize, AcChangeCallback, usize)>> =
    Mutex::new(Vec::new());

pub fn set_callback(cb: Option<RcChangeCallback>, userdata: *mut c_void) {
    let mut slot = CALLBACK.lock().unwrap_or_else(|e| e.into_inner());
    *slot = cb.map(|f| (f, userdata as usize));
}

/// Registers (cb = Some) or clears (cb = None) one core's callback.
/// ac_shutdown clears with the same token.
pub fn set_core_callback(
    token: usize,
    cb: Option<AcChangeCallback>,
    userdata: *mut c_void,
) {
    let mut subs = CORE_CALLBACKS.lock().unwrap_or_else(|e| e.into_inner());
    subs.retain(|(t, _, _)| *t != token);
    if let Some(f) = cb {
        subs.push((token, f, userdata as usize));
    }
}

thread_local! {
    static PROFILE_HINT: RefCell<Option<CString>> =
        const { RefCell::new(None) };
}

/// Stamps the profile the current thread's forwarded call is bound
/// to so emit() routes it to the right scope.  RAII: restores the
/// previous hint on drop, so nested or alternating binds compose.
/// Held for the duration of one forwarded rc_* call — emit() runs
/// synchronously inside it on the same thread.
pub struct ProfileHint(Option<CString>);

pub fn profile_hint(profile_id: &str) -> ProfileHint {
    let prev = PROFILE_HINT.with(|slot| {
        slot.borrow_mut()
            .replace(CString::new(profile_id).unwrap_or_default())
    });
    ProfileHint(prev)
}

impl Drop for ProfileHint {
    fn drop(&mut self) {
        PROFILE_HINT.with(|slot| *slot.borrow_mut() = self.0.take());
    }
}

/// Topics so far: "credentials", "bookmarks", "history", "autofill",
/// "settings", "profiles", "sitedecisions", "searchengines",
/// "cookies", "icons", "containers", "notes", "torctl" (RC14 — a
/// pull-drain wake-up, no store mutated), "synctransport" (SYNC02 —
/// the same pull-drain wake-up shape for the sync transport event
/// queues) — the string is the
/// contract; later components add their own.
pub fn emit(topic: &str) {
    let cb = *CALLBACK.lock().unwrap_or_else(|e| e.into_inner());
    let topic_c = CString::new(topic).unwrap_or_default();
    if let Some((f, userdata)) = cb {
        unsafe { f(topic_c.as_ptr(), userdata as *mut c_void) }
    }
    let subs: Vec<(usize, AcChangeCallback, usize)> = CORE_CALLBACKS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .clone();
    if subs.is_empty() {
        return;
    }
    // Clone the hint before dispatch: a callback that re-enters the
    // library could move the TLS slot under a borrowed pointer.
    let hint = PROFILE_HINT.with(|slot| slot.borrow().clone());
    let hint_ptr = hint.as_ref().map_or(ptr::null(), |c| c.as_ptr());
    for (_, f, userdata) in subs {
        unsafe { f(topic_c.as_ptr(), hint_ptr, userdata as *mut c_void) }
    }
}

// ---- namespaced event lane (MB03, MW-ARCH §5) ---------------------
//
// The opt-in half of the bridge: producers call emit_event() with a
// "domain/verb" topic and the payload is a JSON document — v1 schema
// {"v":1,"topic":<t>,"profile":<bound profile or null>, ...domain
// fields}.  Subscribers register per topic filter through
// ac_events_subscribe; nothing on this lane disturbs the legacy
// topics above — a mutation that wants both audiences calls emit()
// AND emit_event() (the history sites do).
//
// Same thread contract as emit(): synchronous on the mutating
// thread, post-lock only; the consumer hops threads itself.

/// The payload schema version — the "v" field of every doc
/// emit_event() hands subscribers.
pub const EVENT_SCHEMA_VERSION: u32 = 1;

/// The subscriber callback: (namespaced topic, UTF-8 JSON payload,
/// userdata).  Both strings are NUL-terminated and valid only for
/// the call's duration.
pub type AcEventCallback =
    unsafe extern "C" fn(*const c_char, *const c_char, *mut c_void);

/// One registry entry — the declared contract for a namespaced
/// topic.  `fields` names the domain-specific payload keys beyond
/// the envelope's "v"/"topic"/"profile"; `produced` is the honesty
/// bit: a declared topic with no in-tree emit site is contract
/// subscribers may rely on but cannot observe yet.
pub struct EventTopic {
    pub topic: &'static str,
    pub summary: &'static str,
    pub fields: &'static [&'static str],
    pub produced: bool,
}

/// The topic registry — append-only, the string is the contract.
/// Session ops deliberately still emit nothing on ANY lane
/// (MW-ARCH §5): "session/saved" is reserved for a future save
/// lifecycle producer, not per-op store noise.
pub static EVENT_TOPICS: &[EventTopic] = &[
    EventTopic {
        topic: "history/changed",
        summary: "history store mutated; op names the mutation: \
                  visit|title|remove|clear|clear_since|expire",
        fields: &["op"],
        produced: true,
    },
    EventTopic {
        topic: "policy/changed",
        summary: "a sitedecisions/policy record changed",
        fields: &["kind", "key", "op"],
        produced: false,
    },
    EventTopic {
        topic: "profile/changed",
        summary: "persona registry row added/renamed/removed or the \
                  active persona switched",
        fields: &["op", "id"],
        produced: false,
    },
    EventTopic {
        topic: "session/saved",
        summary: "a session file was persisted — the save lifecycle, \
                  not the emit-silent per-op store lane",
        fields: &["path"],
        produced: false,
    },
];

struct EventSub {
    id: u64,
    /// The owning AcCore's token (handle pointer as usize — the
    /// CORE_CALLBACKS convention); ac_shutdown clears by it.
    token: usize,
    filter: String,
    cb: AcEventCallback,
    userdata: usize,
}

static EVENT_SUBS: Mutex<Vec<EventSub>> = Mutex::new(Vec::new());
static NEXT_SUB_ID: AtomicU64 = AtomicU64::new(1);

/// Topics an emit_event() actually fired this process — the
/// runtime-truth half of the registry's honesty story.
static SEEN_TOPICS: Mutex<BTreeSet<String>> = Mutex::new(BTreeSet::new());

/// Filter grammar: "" or "*" subscribes to every event, a trailing
/// '/' ("history/") takes the whole namespace, anything else is an
/// exact topic match.
fn filter_matches(filter: &str, topic: &str) -> bool {
    match filter {
        "" | "*" => true,
        f if f.ends_with('/') => topic.starts_with(f),
        f => f == topic,
    }
}

pub fn subscribe_event(
    token: usize,
    filter: &str,
    cb: AcEventCallback,
    userdata: *mut c_void,
) -> u64 {
    let id = NEXT_SUB_ID.fetch_add(1, Ordering::Relaxed);
    EVENT_SUBS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .push(EventSub {
            id,
            token,
            filter: filter.to_string(),
            cb,
            userdata: userdata as usize,
        });
    id
}

/// Drops the (token, id) pair — the token check keeps one core from
/// unsubscribing another's registration.
pub fn unsubscribe_event(token: usize, id: u64) -> bool {
    let mut subs = EVENT_SUBS.lock().unwrap_or_else(|e| e.into_inner());
    let before = subs.len();
    subs.retain(|s| !(s.token == token && s.id == id));
    subs.len() != before
}

/// ac_shutdown's half — drops every subscription the core owned.
pub fn clear_event_subs(token: usize) {
    EVENT_SUBS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .retain(|s| s.token != token);
}

/// The registry as JSON: [{topic,summary,fields,produced,seen}].
/// `produced` is the declared contract (an emit site exists in
/// tree); `seen` is runtime truth — a producer actually fired the
/// topic this process.
pub fn topics_json() -> String {
    let seen = SEEN_TOPICS.lock().unwrap_or_else(|e| e.into_inner());
    let doc: Vec<Value> = EVENT_TOPICS
        .iter()
        .map(|t| {
            json!({
                "topic": t.topic,
                "summary": t.summary,
                "fields": t.fields,
                "produced": t.produced,
                "seen": seen.contains(t.topic),
            })
        })
        .collect();
    serde_json::to_string(&doc).unwrap_or_else(|_| "[]".into())
}

/// Fires one namespaced event at the subscriber lane.  The payload
/// is the v1 envelope {"v","topic","profile",...extra}: the profile
/// is stamped from the calling thread's bound-profile hint (NULL
/// when the mutation arrived over the legacy rc_* lane).  The
/// envelope keys are reserved — `extra` entries under them are
/// dropped.
///
/// Emits unconditionally mark the topic seen — `seen` records what
/// producers fired, not what anyone heard.
pub fn emit_event(topic: &str, extra: Value) {
    SEEN_TOPICS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .insert(topic.to_string());
    let subs: Vec<(AcEventCallback, usize)> = EVENT_SUBS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .iter()
        .filter(|s| filter_matches(&s.filter, topic))
        .map(|s| (s.cb, s.userdata))
        .collect();
    if subs.is_empty() {
        return;
    }
    let mut doc = json!({
        "v": EVENT_SCHEMA_VERSION,
        "topic": topic,
        "profile": Value::Null,
    });
    // Clone before use: a callback re-entering the library could
    // move the TLS slot under a borrowed pointer.
    let hint = PROFILE_HINT.with(|slot| slot.borrow().clone());
    if let Some(h) = &hint {
        doc["profile"] = json!(h.to_string_lossy());
    }
    if let Value::Object(m) = extra {
        for (k, v) in m {
            if !matches!(k.as_str(), "v" | "topic" | "profile") {
                doc[k] = v;
            }
        }
    }
    let payload = CString::new(doc.to_string()).unwrap_or_default();
    let topic_c = CString::new(topic).unwrap_or_default();
    for (f, userdata) in subs {
        unsafe { f(topic_c.as_ptr(), payload.as_ptr(), userdata as *mut c_void) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;
    use std::sync::MutexGuard;

    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn last_payload() -> String {
        LAST.with(|l| l.borrow().clone())
    }

    thread_local! {
        static LAST: RefCell<String> = const { RefCell::new(String::new()) };
        static LAST_TOPIC: RefCell<String> =
            const { RefCell::new(String::new()) };
    }

    unsafe extern "C" fn record(
        topic: *const c_char,
        payload: *const c_char,
        _ud: *mut c_void,
    ) {
        LAST_TOPIC.with(|l| {
            *l.borrow_mut() = unsafe { CStr::from_ptr(topic) }
                .to_string_lossy()
                .into_owned()
        });
        LAST.with(|l| {
            *l.borrow_mut() = unsafe { CStr::from_ptr(payload) }
                .to_string_lossy()
                .into_owned()
        });
    }

    unsafe extern "C" fn noop(_t: *const c_char, _p: *const c_char, _u: *mut c_void) {}

    #[test]
    fn filter_grammar() {
        assert!(filter_matches("", "history/changed"));
        assert!(filter_matches("*", "history/changed"));
        assert!(filter_matches("history/", "history/changed"));
        assert!(filter_matches("history/changed", "history/changed"));
        assert!(!filter_matches("history/", "settings/changed"));
        assert!(!filter_matches("history/changed", "history/cleared"));
        // A bare prefix without the slash is an exact match, not a
        // wildcard — "history" does not swallow "history/changed".
        assert!(!filter_matches("history", "history/changed"));
    }

    #[test]
    fn envelope_shape_and_profile_stamp() {
        let _g = guard();
        let token = 0xE001usize;
        let id = subscribe_event(token, "history/", record, ptr::null_mut());
        assert_ne!(id, 0);

        emit_event("history/changed", json!({"op": "visit"}));
        let doc: Value =
            serde_json::from_str(&last_payload()).expect("payload is JSON");
        assert_eq!(doc["v"], 1);
        assert_eq!(doc["topic"], "history/changed");
        assert_eq!(doc["profile"], Value::Null);
        assert_eq!(doc["op"], "visit");
        LAST_TOPIC.with(|t| assert_eq!(*t.borrow(), "history/changed"));

        // The bound-profile hint stamps the envelope.
        {
            let _h = profile_hint("work");
            emit_event("history/changed", json!({"op": "clear"}));
        }
        let doc: Value = serde_json::from_str(&last_payload()).unwrap();
        assert_eq!(doc["profile"], "work");

        // Envelope keys are reserved — extras cannot overwrite them.
        emit_event(
            "history/changed",
            json!({"op": "remove", "v": 99, "topic": "x", "profile": "spoof"}),
        );
        let doc: Value = serde_json::from_str(&last_payload()).unwrap();
        assert_eq!(doc["v"], 1);
        assert_eq!(doc["topic"], "history/changed");
        assert_eq!(doc["profile"], Value::Null);
        assert_eq!(doc["op"], "remove");

        assert!(unsubscribe_event(token, id));
        clear_event_subs(token);
    }

    #[test]
    fn filtering_unsubscribe_and_core_scoping() {
        let _g = guard();
        static HITS: AtomicU64 = AtomicU64::new(0);
        unsafe extern "C" fn count(
            _t: *const c_char,
            _p: *const c_char,
            _u: *mut c_void,
        ) {
            HITS.fetch_add(1, Ordering::SeqCst);
        }

        let a = 0xE010usize;
        let b = 0xE011usize;
        let sa = subscribe_event(a, "settings/", count, ptr::null_mut());
        let _sb = subscribe_event(b, "*", count, ptr::null_mut());
        let base = HITS.load(Ordering::SeqCst);

        // Non-matching filters never fire.
        emit_event("history/changed", json!({}));
        assert_eq!(HITS.load(Ordering::SeqCst) - base, 1); // only b
        emit_event("settings/changed", json!({}));
        assert_eq!(HITS.load(Ordering::SeqCst) - base, 3); // a + b

        // Wrong token cannot drop another core's sub.
        assert!(!unsubscribe_event(b, sa));
        // Right token can; id 0 never existed.
        assert!(unsubscribe_event(a, sa));
        assert!(!unsubscribe_event(a, 0));
        emit_event("settings/changed", json!({}));
        assert_eq!(HITS.load(Ordering::SeqCst) - base, 4); // only b

        // Shutdown clears the core's subs wholesale.
        clear_event_subs(b);
        emit_event("history/changed", json!({}));
        assert_eq!(HITS.load(Ordering::SeqCst) - base, 4);
    }

    #[test]
    fn topics_registry_json() {
        let _g = guard();
        let doc: Value = serde_json::from_str(&topics_json()).unwrap();
        let rows = doc.as_array().unwrap();
        assert_eq!(rows.len(), EVENT_TOPICS.len());
        let hist = rows
            .iter()
            .find(|r| r["topic"] == "history/changed")
            .expect("history/changed declared");
        assert_eq!(hist["produced"], true);
        assert!(hist["fields"].as_array().unwrap().contains(&json!("op")));
        // A declared-but-unproduced topic is honest contract.
        let sess = rows
            .iter()
            .find(|r| r["topic"] == "session/saved")
            .unwrap();
        assert_eq!(sess["produced"], false);

        // seen flips only when an emit site actually fired.
        let token = 0xE020usize;
        subscribe_event(token, "", noop, ptr::null_mut());
        emit_event("policy/changed", json!({}));
        let doc: Value = serde_json::from_str(&topics_json()).unwrap();
        let pol = doc
            .as_array()
            .unwrap()
            .iter()
            .find(|r| r["topic"] == "policy/changed")
            .unwrap();
        assert_eq!(pol["seen"], true);
        assert_eq!(pol["produced"], false); // contract still honest
        clear_event_subs(token);
    }
}
