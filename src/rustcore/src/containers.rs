//! RC19: the container registry + the storage-namespace map.
//!
//! Audit call (per the RC19 spec): ContainerManager persists the
//! registry inside profile settings — the "containers" QSettings
//! group — so this module is a TYPED SPECIALIZATION over RC02's
//! store, not a new file format.  The keys live in the profile
//! scope's settings.json (mirrored to settings.ini by the C++
//! ProfileSettings shim, which keeps the no-rust lane on identical
//! bytes):
//!
//!   containers/order        string list — ids in creation order
//!   containers/<id>/name    user-visible name
//!   containers/<id>/color   QColor name() text ("#rrggbb")
//!   containers/<id>/icon    optional icon tag — the RC19 registry
//!                           shape's forward field; today's C++
//!                           Container carries id/name/color only
//!                           and ignores the key.
//!
//! Semantics mirror ContainerManager::loadRegistry exactly: "order"
//! is the authoritative id list — an order entry whose group has no
//! keys is skipped (left in the list, like the C++), a group that
//! never made the order list is stale and pruned on read (the C++
//! registry-hygiene pass).  The default container — the unlabeled
//! context — is the empty id and never a registry entry.
//!
//! The namespace map is the contract engines partition storage by
//! (and RC15's cookie database partitions rows on): given the app
//! data root, a persona slug and a container id it derives the same
//! paths ContainerManager computes — persistent storage root, the
//! VSEC02 cache tree, the Qt-keyed residue dir and the WebEngine
//! storage name — plus the cookie-partition key the ac_cookies_*
//! lane takes.  Site-assignment rules ("always open in <container>")
//! are NOT here: they already live in sitedecisions.json's
//! "container" kind (SITED01), keyed by normalized host.

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::profiles;
use crate::settings::{RcSettings, Value};
use std::path::PathBuf;

const ORDER_KEY: &str = "containers/order";
const GROUP_PREFIX: &str = "containers/";

const MAX_ID_LEN: usize = 64;
const MAX_FIELD_LEN: usize = 1024;
const MAX_CONTAINERS: usize = 512;

/// One registry row — the C++ ContainerManager::Container plus the
/// optional icon the RC19 spec lists ("" when unset; the C++ reader
/// ignores the key).
#[derive(Clone, Debug, PartialEq)]
pub struct Container {
    pub id: String,
    pub name: String,
    pub color: String,
    pub icon: String,
}

/// Partial edit for `update` — absent fields keep their stored
/// value; `icon` is a tri-state: None leaves it, Some("") clears it.
#[derive(Default)]
pub struct Patch<'a> {
    pub name: Option<&'a str>,
    pub color: Option<&'a str>,
    pub icon: Option<&'a str>,
}

/// The storage namespace one container id implies — every path an
/// engine (or the cleanup lanes) derives for it.  Mirrors
/// ContainerManager::storagePath / cachePathFor / defaultDataPathFor
/// / storageNameFor byte-for-byte.
#[derive(Clone, Debug, PartialEq)]
pub struct Namespace {
    /// "arktos-container-<profile>-<id>" — the QWebEngineProfile
    /// storage name; the "arktos-container-" prefix is what keeps
    /// persona-name parses (BrowserProfile::profileIdFor) from
    /// mistaking a container for a persona.
    pub storage_name: String,
    /// <root>/profiles/<profile>/containers/<id> — the profile's
    /// persistentStoragePath.
    pub storage_path: String,
    /// <root>/profiles/<profile>/cache/containers/<id> — the VSEC02
    /// cache root (sealed with the owning persona's cache tree).
    pub cache_path: String,
    /// <root>/QtWebEngine/<storage_name> — the residue dir Qt keys
    /// off the storage name (dictionaries/download state) even when
    /// the real paths are overridden; kept for wipe bookkeeping.
    pub default_data_path: String,
    /// The ac_cookies_* partition key — the container id itself
    /// ("" names the default context's partition; this namespace is
    /// always a registered id so it is never empty here).
    pub cookie_partition: String,
}

/// Valid registry-id shape: ascii alphanumeric, '-' and '_',
/// 1..=64 bytes — today's ids are QUuid hex-with-dashes; the shape
/// admits future slugs while refusing anything that cannot be a
/// QSettings group / directory name (no '/', no '.', no controls —
/// the id lands verbatim in storage paths).
pub fn is_container_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= MAX_ID_LEN
        && id.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
}

fn check_id(id: &str) -> RcResult<()> {
    if is_container_id(id) {
        Ok(())
    } else {
        fail(RcStatus::InvalidArgument, "invalid container id")
    }
}

fn check_field(s: &str) -> RcResult<()> {
    if s.len() > MAX_FIELD_LEN {
        fail(RcStatus::InvalidArgument, "container field too large")
    } else if s.bytes().any(|b| b < 0x20 && b != b'\t') {
        fail(RcStatus::InvalidArgument, "container field has controls")
    } else {
        Ok(())
    }
}

fn key(id: &str, field: &str) -> String {
    format!("{GROUP_PREFIX}{id}/{field}")
}

/// The ids under containers/ that have at least one stored key —
/// the childGroups() equivalent.  "order" itself is a key, never a
/// group.
fn groups(s: &RcSettings) -> RcResult<Vec<String>> {
    let mut out: Vec<String> = s
        .list(Some("containers"))?
        .iter()
        .filter_map(|k| {
            let rest = k.strip_prefix(GROUP_PREFIX)?;
            let seg = rest.split('/').next().unwrap_or("");
            if seg.is_empty() || seg == "order" {
                None
            } else {
                Some(seg.to_string())
            }
        })
        .collect();
    out.sort();
    out.dedup();
    Ok(out)
}

fn read_row(s: &RcSettings, id: &str) -> Container {
    Container {
        id: id.to_string(),
        name: s.get_string(&key(id, "name")).unwrap_or_default(),
        color: s.get_string(&key(id, "color")).unwrap_or_default(),
        icon: s.get_string(&key(id, "icon")).unwrap_or_default(),
    }
}

/// The registry in creation order — ContainerManager::containers()
/// semantics: order-listed ids with a live group, plus the
/// stale-group pruning loadRegistry performs on every load.
pub fn list(s: &RcSettings) -> RcResult<Vec<Container>> {
    let order = s.get_list(ORDER_KEY).unwrap_or_default();
    let live = groups(s)?;
    // Registry hygiene: a group that never made the order list is
    // stale (interrupted write) — drop its keys, mirroring the C++
    // settings.remove(group) sweep.
    for g in &live {
        if !order.iter().any(|id| id == g) {
            let prefix = format!("{GROUP_PREFIX}{g}");
            for k in s.list(Some(&prefix))? {
                s.remove(&k)?;
            }
        }
    }
    let mut seen = std::collections::HashSet::new();
    let mut out = Vec::new();
    for id in &order {
        if id.is_empty() || !seen.insert(id.clone()) || !live.contains(id) {
            continue;
        }
        out.push(read_row(s, id));
    }
    Ok(out)
}

/// One row by id — RC_NOT_FOUND when unregistered (an id in the
/// order list but key-less reads as unregistered, same as the C++).
pub fn get(s: &RcSettings, id: &str) -> RcResult<Container> {
    check_id(id)?;
    if !is_registered(s, id)? {
        return fail(RcStatus::NotFound, "no such container");
    }
    Ok(read_row(s, id))
}

/// isContainerId() — order-listed AND group-present.
pub fn is_registered(s: &RcSettings, id: &str) -> RcResult<bool> {
    if !is_container_id(id) {
        return Ok(false);
    }
    Ok(list(s)?.iter().any(|c| c.id == id))
}

/// Mint a container — the QUuid::createUuid().toString(WithoutBraces)
/// id shape (v4 uuid, hex with dashes) appended to "order" with its
/// name/color(/icon) group.  `name` must be non-blank — the only
/// C++ producer path enforces the same.
pub fn create(
    s: &RcSettings,
    name: &str,
    color: &str,
    icon: Option<&str>,
) -> RcResult<Container> {
    let name = name.trim();
    if name.is_empty() {
        return fail(RcStatus::InvalidArgument, "container name is empty");
    }
    check_field(name)?;
    check_field(color)?;
    if let Some(i) = icon {
        check_field(i)?;
    }
    if list(s)?.len() >= MAX_CONTAINERS {
        return fail(RcStatus::InvalidArgument, "container registry full");
    }
    let id = new_id()?;
    s.set(&key(&id, "name"), Value::Str(name.to_string()))?;
    s.set(&key(&id, "color"), Value::Str(color.to_string()))?;
    if let Some(i) = icon {
        if !i.is_empty() {
            s.set(&key(&id, "icon"), Value::Str(i.to_string()))?;
        }
    }
    let mut order = s.get_list(ORDER_KEY).unwrap_or_default();
    order.push(id.clone());
    s.set(ORDER_KEY, Value::List(order))?;
    Ok(Container {
        id,
        name: name.to_string(),
        color: color.to_string(),
        icon: icon.unwrap_or("").to_string(),
    })
}

/// renameContainer/setContainerColor (and the icon slot) — patch
/// semantics: every Some field is written.  RC_NOT_FOUND on an
/// unregistered id, like the C++ setters returning false.
pub fn update(
    s: &RcSettings,
    id: &str,
    patch: &Patch,
) -> RcResult<Container> {
    check_id(id)?;
    let current = get(s, id)?;
    let name = patch.name.unwrap_or(current.name.as_str());
    let color = patch.color.unwrap_or(current.color.as_str());
    check_field(name)?;
    check_field(color)?;
    if let Some(i) = patch.icon {
        check_field(i)?;
    }
    if let Some(n) = patch.name {
        s.set(&key(id, "name"), Value::Str(n.to_string()))?;
    }
    if let Some(c) = patch.color {
        s.set(&key(id, "color"), Value::Str(c.to_string()))?;
    }
    if let Some(i) = patch.icon {
        if i.is_empty() {
            s.remove(&key(id, "icon"))?;
        } else {
            s.set(&key(id, "icon"), Value::Str(i.to_string()))?;
        }
    }
    Ok(read_row(s, id))
}

/// removeRegistry() — the store half of deleteContainer: the
/// <id>/ group is dropped and the id leaves "order".  False on an
/// unregistered id (the C++ guard); the on-disk storage trees are
/// NOT this layer's job (deleteContainer removes them from the
/// namespace paths namespace() derives).
pub fn remove(s: &RcSettings, id: &str) -> RcResult<bool> {
    check_id(id)?;
    if !is_registered(s, id)? {
        return Ok(false);
    }
    let prefix = format!("{GROUP_PREFIX}{id}");
    for k in s.list(Some(&prefix))? {
        s.remove(&k)?;
    }
    let mut order = s.get_list(ORDER_KEY).unwrap_or_default();
    order.retain(|o| o != id);
    s.set(ORDER_KEY, Value::List(order))?;
    Ok(true)
}

/// The WebEngine storage name — "arktos-container-<profile>-<id>".
pub fn storage_name(profile_id: &str, container_id: &str) -> RcResult<String> {
    if !profiles::is_profile_id(profile_id) {
        return fail(RcStatus::InvalidArgument, "invalid profile id");
    }
    check_id(container_id)?;
    Ok(format!("arktos-container-{profile_id}-{container_id}"))
}

/// The full namespace for one registered container id.  `data_root`
/// is the application data root (BrowserPaths::dataRoot), NOT the
/// profile dir.  The default ("") container has no namespace of its
/// own — its storage is the persona profile — so it is refused like
/// any non-registry id.
pub fn namespace(
    data_root: &str,
    profile_id: &str,
    container_id: &str,
) -> RcResult<Namespace> {
    if data_root.is_empty() {
        return fail(RcStatus::InvalidArgument, "empty data root");
    }
    let name = storage_name(profile_id, container_id)?;
    let profile_dir = PathBuf::from(data_root)
        .join("profiles")
        .join(profile_id);
    Ok(Namespace {
        storage_path: profile_dir
            .join("containers")
            .join(container_id)
            .to_string_lossy()
            .into_owned(),
        cache_path: profile_dir
            .join("cache")
            .join("containers")
            .join(container_id)
            .to_string_lossy()
            .into_owned(),
        default_data_path: PathBuf::from(data_root)
            .join("QtWebEngine")
            .join(&name)
            .to_string_lossy()
            .into_owned(),
        storage_name: name,
        cookie_partition: container_id.to_string(),
    })
}

/// The registry's JSON row shape — id/name/color/icon.
pub fn to_json(c: &Container) -> serde_json::Value {
    serde_json::json!({
        "id": c.id,
        "name": c.name,
        "color": c.color,
        "icon": c.icon,
    })
}

/// The namespace's JSON shape — the ProfileOptions/storage map an
/// engine backend consumes.
pub fn namespace_json(n: &Namespace) -> serde_json::Value {
    serde_json::json!({
        "storage_name": n.storage_name,
        "storage_path": n.storage_path,
        "cache_path": n.cache_path,
        "default_data_path": n.default_data_path,
        "cookie_partition": n.cookie_partition,
    })
}

/// A v4 uuid in QUuid::toString(WithoutBraces) form —
/// 8-4-4-4-12 lowercase hex.
fn new_id() -> RcResult<String> {
    let mut b = [0u8; 16];
    getrandom::getrandom(&mut b).map_err(|e| Fail {
        status: RcStatus::Unavailable,
        msg: format!("getrandom: {e}"),
    })?;
    b[6] = (b[6] & 0x0f) | 0x40; // version 4
    b[8] = (b[8] & 0x3f) | 0x80; // variant 10
    let hex: String = b.iter().map(|v| format!("{v:02x}")).collect();
    Ok(format!(
        "{}-{}-{}-{}-{}",
        &hex[0..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..32]
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;

    fn tmpdir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-containers-{}-{}-{}",
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

    fn store(root: &PathBuf, pid: &str) -> RcSettings {
        RcSettings::open(root.to_str().unwrap(), Some(pid)).unwrap()
    }

    #[test]
    fn registry_crud_roundtrip() {
        let root = tmpdir("crud");
        let s = store(&root, "work");
        assert!(list(&s).unwrap().is_empty());

        let a = create(&s, " Work ", "#ff8800", None).unwrap();
        let b = create(&s, "Banking", "#3366ff", Some("bank")).unwrap();
        assert!(is_container_id(&a.id));
        assert_eq!(a.id.len(), 36);
        assert_eq!(a.name, "Work"); // trimmed like the C++ dialog

        let rows = list(&s).unwrap();
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0].id, a.id); // creation order preserved
        assert_eq!(rows[1].id, b.id);
        assert_eq!(rows[1].icon, "bank");
        assert!(is_registered(&s, &a.id).unwrap());
        assert!(!is_registered(&s, "deadbeef").unwrap());

        // get + update: rename, recolor, set-then-clear icon.
        assert_eq!(get(&s, &b.id).unwrap().color, "#3366ff");
        let patched = update(
            &s,
            &b.id,
            &Patch {
                name: Some("Vault"),
                icon: Some(""),
                ..Default::default()
            },
        )
        .unwrap();
        assert_eq!(patched.name, "Vault");
        assert_eq!(patched.color, "#3366ff");
        assert_eq!(patched.icon, "");
        assert!(get(&s, &b.id).unwrap().icon.is_empty());
        assert!(update(&s, "ghost", &Patch::default()).is_err());

        // remove: the group dies and "order" shrinks.
        assert!(remove(&s, &a.id).unwrap());
        assert!(!remove(&s, &a.id).unwrap());
        assert_eq!(list(&s).unwrap().len(), 1);
        assert!(get(&s, &a.id).is_err());

        // Everything round-trips through disk on a fresh handle.
        drop(s);
        let s2 = store(&root, "work");
        let rows = list(&s2).unwrap();
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].name, "Vault");

        // The blank-name guard and the default-container refusal.
        assert!(create(&s2, "   ", "#fff", None).is_err());
        assert!(remove(&s2, "").is_err());
        assert!(get(&s2, "").is_err());
    }

    #[test]
    fn legacy_cpp_layout_reads_and_prunes() {
        let root = tmpdir("legacy");
        let s = store(&root, "default");
        // The exact keys ContainerManager::loadRegistry binds.
        s.set(ORDER_KEY, Value::List(vec!["cid1".into(), "ghost".into()]))
            .unwrap();
        s.set("containers/cid1/name", Value::Str("One".into()))
            .unwrap();
        s.set("containers/cid1/color", Value::Str("#123456".into()))
            .unwrap();
        // "ghost" sits in order but has no group — skipped, and the
        // order list itself is left alone (the C++ never rewrites it
        // on load).
        // A stale group never listed in order is pruned on read.
        s.set("containers/stale/name", Value::Str("Gone".into()))
            .unwrap();

        let rows = list(&s).unwrap();
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].id, "cid1");
        assert_eq!(rows[0].name, "One");
        assert_eq!(rows[0].color, "#123456");
        assert_eq!(rows[0].icon, "");
        // The stale group's keys are gone; the ghost order entry and
        // the live row's keys remain.
        assert!(!s.contains("containers/stale/name").unwrap());
        assert_eq!(
            s.get_list(ORDER_KEY).unwrap(),
            vec!["cid1".to_string(), "ghost".to_string()]
        );
        assert!(s.contains("containers/cid1/name").unwrap());
    }

    #[test]
    fn namespace_matches_containermanager_paths() {
        let root = tmpdir("ns");
        let ns = namespace(root.to_str().unwrap(), "work", "abc-123").unwrap();
        let dir = root.to_str().unwrap();
        assert_eq!(ns.storage_name, "arktos-container-work-abc-123");
        assert_eq!(
            ns.storage_path,
            format!("{dir}/profiles/work/containers/abc-123")
        );
        assert_eq!(
            ns.cache_path,
            format!("{dir}/profiles/work/cache/containers/abc-123")
        );
        assert_eq!(
            ns.default_data_path,
            format!("{dir}/QtWebEngine/arktos-container-work-abc-123")
        );
        assert_eq!(ns.cookie_partition, "abc-123");

        // Refusals: the default container has no namespace, non-slug
        // ids never reach a path.
        assert!(namespace(dir, "work", "").is_err());
        assert!(namespace(dir, "../escape", "abc").is_err());
        assert!(namespace(dir, "work", "../escape").is_err());
        assert!(namespace(dir, "work", "has space").is_err());
        assert!(namespace("", "work", "abc").is_err());
        assert_eq!(
            storage_name("persona_2", "dead-beef").unwrap(),
            "arktos-container-persona_2-dead-beef"
        );
    }

    #[test]
    fn json_shapes() {
        let c = Container {
            id: "i1".into(),
            name: "N".into(),
            color: "#000".into(),
            icon: "".into(),
        };
        let j = to_json(&c);
        assert_eq!(j["id"], "i1");
        assert_eq!(j["icon"], "");
        let n = namespace("/root", "default", "i1").unwrap();
        let nj = namespace_json(&n);
        assert_eq!(nj["storage_name"], "arktos-container-default-i1");
        assert_eq!(nj["cookie_partition"], "i1");
    }
}
