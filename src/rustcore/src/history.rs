//! RCORE02b: the canonical history store — SQLite via rusqlite
//! (bundled amalgamation, so the staticlib stays self-contained).
//!
//! HistoryManager keeps its QList<HistoryEntry> as the hot mirror
//! the existing models iterate; this store is what persists.  Every
//! mutation writes through immediately, so the AutoSaver window
//! disappears — the file is always current.
//!
//! Schema (<data dir>/history.db, WAL):
//!   visits(seq PK, url, title, visited_at ms-epoch)
//!     — append-only visit log; duplicates allowed, exactly like the
//!     legacy QDataStream file.  The deduped listing replicates the
//!     old parser's consecutive-identical collapse.
//!   icons(host PK, png blob, updated_at)
//!     — per-host favicons, the HIST01 icon persistence: keyed by
//!     host (favicons are per-origin in practice), stored as PNG.
//!
//! The legacy "history" file is a READ-ONLY import source: the Qt
//! side parses it once (bounded HistoryParser) when the .db does not
//! exist yet and replays the rows through rc_hist_add.  Old entries
//! never flow back — new writes land in SQLite only.

use std::path::PathBuf;
use std::sync::Mutex;

use rusqlite::{params, Connection};

use crate::error::{Fail, RcResult, RcStatus};
use crate::omnibox;
use crate::store;

const DB_FILE: &str = "history.db";

fn sql_fail(ctx: &str, e: rusqlite::Error) -> Fail {
    Fail {
        status: RcStatus::Io,
        msg: format!("{ctx}: {e}"),
    }
}

#[derive(Clone)]
pub struct Entry {
    pub url: String,
    pub title: String,
    /// Milliseconds since the Unix epoch (QDateTime::toMSecsSinceEpoch).
    pub visited_at: i64,
}

pub struct HistoryStore {
    conn: Option<Connection>,
    path: Option<PathBuf>,
    /// Deduped newest-first listing cache — rebuilt lazily and dropped
    /// on every mutation so the Qt mirror's load is O(rows) once.
    entries: Option<Vec<Entry>>,
}

static HIST: Mutex<HistoryStore> = Mutex::new(HistoryStore {
    conn: None,
    path: None,
    entries: None,
});

/// Runs `f` against the shared history store, surviving a poisoned
/// mutex.
pub fn with<R>(f: impl FnOnce(&mut HistoryStore) -> R) -> R {
    let mut guard = HIST.lock().unwrap_or_else(|e| e.into_inner());
    f(&mut guard)
}

impl HistoryStore {
    /// Opens (or reuses) the database at `path`.  `None` resolves to
    /// <data dir>/history.db.  Reopening the same path is a no-op.
    pub fn open(&mut self, path: Option<&str>) -> RcResult<()> {
        let path = match path {
            Some(p) => PathBuf::from(p),
            None => store::lock().dir()?.join(DB_FILE),
        };
        if self.conn.is_some() && self.path.as_deref() == Some(path.as_path()) {
            return Ok(());
        }
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir).map_err(|e| Fail {
                status: RcStatus::Io,
                msg: format!("cannot create {}: {e}", dir.display()),
            })?;
        }
        let conn = Connection::open(&path).map_err(|e| Fail {
            status: RcStatus::Io,
            msg: format!("cannot open {}: {e}", path.display()),
        })?;
        conn.execute_batch(
            "PRAGMA journal_mode = WAL;
             PRAGMA synchronous = NORMAL;
             CREATE TABLE IF NOT EXISTS visits(
                 seq INTEGER PRIMARY KEY AUTOINCREMENT,
                 url TEXT NOT NULL,
                 title TEXT NOT NULL DEFAULT '',
                 visited_at INTEGER NOT NULL);
             CREATE INDEX IF NOT EXISTS visits_url ON visits(url);
             CREATE TABLE IF NOT EXISTS icons(
                 host TEXT PRIMARY KEY,
                 png BLOB NOT NULL,
                 updated_at INTEGER NOT NULL);",
        )
        .map_err(|e| Fail {
            status: RcStatus::Io,
            msg: format!("cannot initialize {}: {e}", path.display()),
        })?;
        self.conn = Some(conn);
        self.path = Some(path);
        self.entries = None;
        Ok(())
    }

    fn conn(&self) -> RcResult<&Connection> {
        self.conn.as_ref().ok_or_else(|| Fail {
            status: RcStatus::NotInitialized,
            msg: "rc_hist_open() has not been called".into(),
        })
    }

    /// Existence check without opening — used to decide whether the
    /// legacy QDataStream file still needs importing.
    pub fn exists_on_disk(path: Option<&str>) -> bool {
        let path = match path {
            Some(p) => PathBuf::from(p),
            None => match store::lock().dir() {
                Ok(dir) => dir.join(DB_FILE),
                Err(_) => return false,
            },
        };
        path.exists()
    }

    // ---- visits ------------------------------------------------------

    pub fn add(&mut self, url: &str, title: &str, visited_at: i64) -> RcResult<()> {
        self.conn()?
            .execute(
                "INSERT INTO visits(url, title, visited_at) VALUES(?1, ?2, ?3)",
                params![url, title, visited_at],
            )
            .map_err(|e| sql_fail("history insert", e))?;
        self.entries = None;
        Ok(())
    }

    /// Title-update parity with the old in-memory loop: only the
    /// newest matching row changes.
    pub fn update_title(&mut self, url: &str, title: &str) -> RcResult<()> {
        let changed = self.conn()?
            .execute(
                "UPDATE visits SET title = ?2 WHERE seq = (
                     SELECT seq FROM visits WHERE url = ?1
                     ORDER BY seq DESC LIMIT 1)",
                params![url, title],
            )
            .map_err(|e| sql_fail("history update", e))?;
        if changed > 0 {
            self.entries = None;
        }
        Ok(())
    }

    /// Removes the newest row exactly matching (url, title, ts) —
    /// removeOne semantics from the QList mirror.
    pub fn remove(&mut self, url: &str, title: &str, visited_at: i64) -> RcResult<()> {
        let changed = self.conn()?
            .execute(
                "DELETE FROM visits WHERE seq = (
                     SELECT seq FROM visits WHERE url = ?1 AND title = ?2
                     AND visited_at = ?3 ORDER BY seq DESC LIMIT 1)",
                params![url, title, visited_at],
            )
            .map_err(|e| sql_fail("history remove", e))?;
        if changed > 0 {
            self.entries = None;
        }
        Ok(())
    }

    pub fn clear(&mut self) -> RcResult<()> {
        self.conn()?
            .execute("DELETE FROM visits", [])
            .map_err(|e| sql_fail("history clear", e))?;
        self.entries = None;
        Ok(())
    }

    /// The deduped, newest-first listing: consecutive rows identical
    /// in (url, title, ts) collapse into one, keeping the newest
    /// non-empty title in the run — the old HistoryParser rule, so a
    /// save/load round trip dedupes exactly like the legacy file.
    fn entries(&mut self) -> RcResult<&Vec<Entry>> {
        if self.entries.is_none() {
            let out = {
                let conn = self.conn()?;
                let mut stmt = conn
                    .prepare(
                        "SELECT url, title, visited_at FROM visits
                         ORDER BY visited_at DESC, seq DESC",
                    )
                    .map_err(|e| sql_fail("history list", e))?;
                let rows = stmt
                    .query_map([], |r| {
                        Ok(Entry {
                            url: r.get(0)?,
                            title: r.get(1)?,
                            visited_at: r.get(2)?,
                        })
                    })
                    .map_err(|e| sql_fail("history list", e))?;
                let mut out: Vec<Entry> = Vec::new();
                for row in rows {
                    let e = row.map_err(|e| sql_fail("history list", e))?;
                    if let Some(last) = out.last_mut() {
                        if last.url == e.url && last.visited_at == e.visited_at {
                            // Consecutive duplicate: collapse, preferring
                            // a non-empty title (the parser's merge).
                            if last.title.is_empty() {
                                last.title = e.title;
                            }
                            continue;
                        }
                    }
                    out.push(e);
                }
                out
            };
            self.entries = Some(out);
        }
        Ok(self.entries.as_ref().unwrap())
    }

    pub fn count(&mut self) -> RcResult<i64> {
        Ok(self.entries()?.len() as i64)
    }

    /// Entry `row` of the deduped listing as a JSON object.
    pub fn entry_at(&mut self, row: i64) -> RcResult<Option<String>> {
        let entries = self.entries()?;
        if row < 0 || row as usize >= entries.len() {
            return Ok(None);
        }
        let e = &entries[row as usize];
        Ok(Some(
            serde_json::json!({"url": e.url, "title": e.title, "ts": e.visited_at})
                .to_string(),
        ))
    }

    // ---- icons ---------------------------------------------------------

    pub fn icon_set(&mut self, host: &str, png: &[u8], updated_at: i64) -> RcResult<()> {
        self.conn()?
            .execute(
                "INSERT INTO icons(host, png, updated_at) VALUES(?1, ?2, ?3)
                 ON CONFLICT(host) DO UPDATE SET png = ?2, updated_at = ?3",
                params![host, png, updated_at],
            )
            .map_err(|e| sql_fail("icon insert", e))?;
        Ok(())
    }

    pub fn icon_get(&self, host: &str) -> RcResult<Vec<u8>> {
        self.conn()?
            .query_row("SELECT png FROM icons WHERE host = ?1", params![host], |r| {
                r.get(0)
            })
            .map_err(|e| match e {
                rusqlite::Error::QueryReturnedNoRows => Fail {
                    status: RcStatus::NotFound,
                    msg: "no icon for host".into(),
                },
                e => Fail {
                    status: RcStatus::Io,
                    msg: format!("icon lookup: {e}"),
                },
            })
    }

    pub fn icon_clear(&mut self) -> RcResult<()> {
        self.conn()?
            .execute("DELETE FROM icons", [])
            .map_err(|e| sql_fail("icon clear", e))?;
        Ok(())
    }

    // ---- omnibox suggestions (OMNI01) ---------------------------------
    //
    // Ranked completion rows for a term — the Rust twin of the
    // HistoryFilterModel + HistoryCompletionModel pipeline: visits are
    // grouped per-url (newest visit supplies the row's title/ts),
    // frecency is the summed per-visit decay, a word-boundary hit of the
    // term on the url's host or the title doubles the score, and the
    // result is ordered score-descending.  `limit` caps the marshal —
    // the completer only ever displays a slice.
    pub fn suggest(&mut self, term: &str, limit: i64, now_ms: i64) -> RcResult<String> {
        let limit = if limit <= 0 {
            100usize
        } else {
            (limit as usize).min(1000)
        };
        let now_day = omnibox::local_day_number(now_ms);
        let term_l = term.to_lowercase();

        struct Agg {
            title: String,
            ts: i64,
            frecency: i64,
        }
        // Newest-first group order, mirroring the legacy hash's
        // first-seen-in-descending-order representative row.
        let mut order: Vec<String> = Vec::new();
        let mut agg: std::collections::HashMap<String, Agg> =
            std::collections::HashMap::new();
        {
            let conn = self.conn()?;
            let mut stmt = conn
                .prepare(
                    "SELECT url, title, visited_at FROM visits
                     ORDER BY visited_at DESC, seq DESC",
                )
                .map_err(|e| sql_fail("history suggest", e))?;
            let rows = stmt
                .query_map([], |r| {
                    Ok((
                        r.get::<_, String>(0)?,
                        r.get::<_, String>(1)?,
                        r.get::<_, i64>(2)?,
                    ))
                })
                .map_err(|e| sql_fail("history suggest", e))?;
            for row in rows {
                let (url, title, ts) =
                    row.map_err(|e| sql_fail("history suggest", e))?;
                let score = omnibox::decay(now_day - omnibox::local_day_number(ts));
                match agg.entry(url) {
                    std::collections::hash_map::Entry::Occupied(mut e) => {
                        e.get_mut().frecency += score;
                    }
                    std::collections::hash_map::Entry::Vacant(e) => {
                        order.push(e.key().clone());
                        e.insert(Agg {
                            title,
                            ts,
                            frecency: score,
                        });
                    }
                }
            }
        }

        let mut scored: Vec<serde_json::Value> = Vec::new();
        for url in order {
            let a = agg.remove(&url).unwrap();
            if !term_l.is_empty()
                && !url.to_lowercase().contains(&term_l)
                && !a.title.to_lowercase().contains(&term_l)
            {
                continue;
            }
            // HistoryCompletionModel::lessThan's word-boundary bonus.
            let boundary = omnibox::word_boundary_match(
                omnibox::url_host(&url),
                term,
            ) || omnibox::word_boundary_match(&a.title, term);
            let score = if boundary { a.frecency * 2 } else { a.frecency };
            scored.push(serde_json::json!({
                "url": url,
                "title": a.title,
                "ts": a.ts,
                "frecency": a.frecency,
                "score": score,
            }));
        }
        scored.sort_by(|a, b| {
            b["score"]
                .as_i64()
                .cmp(&a["score"].as_i64())
                // HistoryFilterModel appends first-seen rows while
                // iterating newest-first, so score ties resolve
                // newest-representative-first.
                .then(b["ts"].as_i64().cmp(&a["ts"].as_i64()))
                .then(a["url"].as_str().cmp(&b["url"].as_str()))
        });
        scored.truncate(limit);
        Ok(serde_json::to_string(&scored).unwrap_or_else(|_| "[]".into()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    fn temp_db(name: &str) -> String {
        let dir = std::env::temp_dir().join(format!(
            "arora-hist-{}-{}-{}",
            name,
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        dir.join("history.db").to_string_lossy().into_owned()
    }

    fn store_at(path: &str) -> HistoryStore {
        let mut s = HistoryStore {
            conn: None,
            path: None,
            entries: None,
        };
        s.open(Some(path)).unwrap();
        s
    }

    #[test]
    fn visits_round_trip_and_dedup() {
        let p = temp_db("dedup");
        let mut s = store_at(&p);
        // Oldest first, like the legacy file replay.
        s.add("http://a/", "", 100).unwrap();
        s.add("http://b/", "B", 200).unwrap();
        // Consecutive identical row collapses in the listing.
        s.add("http://b/", "B", 200).unwrap();
        // Same url at a different time stays.
        s.add("http://b/", "B", 300).unwrap();
        assert_eq!(s.count().unwrap(), 3);
        let mut e = |i: i64| serde_json::from_str::<serde_json::Value>(
            &s.entry_at(i).unwrap().unwrap(),
        )
        .unwrap();
        assert_eq!(e(0)["ts"], 300);
        assert_eq!(e(1)["ts"], 200);
        assert_eq!(e(2)["url"], "http://a/");
        assert!(s.entry_at(3).unwrap().is_none());
        std::fs::remove_dir_all(Path::new(&p).parent().unwrap()).ok();
    }

    #[test]
    fn update_title_and_remove() {
        let p = temp_db("upd");
        let mut s = store_at(&p);
        s.add("http://a/", "", 100).unwrap();
        s.add("http://a/", "", 200).unwrap();
        s.update_title("http://a/", "new").unwrap();
        // Only the newest row changes.
        assert_eq!(
            s.entry_at(0).unwrap().unwrap().contains("\"new\""),
            true
        );
        assert_eq!(
            s.entry_at(1).unwrap().unwrap().contains("\"\""),
            true
        );
        s.remove("http://a/", "new", 200).unwrap();
        assert_eq!(s.count().unwrap(), 1);
        s.clear().unwrap();
        assert_eq!(s.count().unwrap(), 0);
        std::fs::remove_dir_all(Path::new(&p).parent().unwrap()).ok();
    }

    #[test]
    fn icons_persist_across_reopen() {
        let p = temp_db("icons");
        {
            let mut s = store_at(&p);
            s.icon_set("example.com", &[1, 2, 3], 1).unwrap();
            s.icon_set("other.org", &[4, 5], 2).unwrap();
        }
        {
            let mut s = store_at(&p);
            assert_eq!(s.icon_get("example.com").unwrap(), vec![1, 2, 3]);
            assert!(matches!(
                s.icon_get("missing.invalid"),
                Err(Fail { status: RcStatus::NotFound, .. })
            ));
            s.icon_clear().unwrap();
            assert!(s.icon_get("other.org").is_err());
        }
        std::fs::remove_dir_all(Path::new(&p).parent().unwrap()).ok();
    }

    #[test]
    fn suggest_ranks_filters_and_aggregates() {
        let p = temp_db("suggest");
        let mut s = store_at(&p);
        // Fixed "now" — mid-bucket offsets keep local-day diffs inside
        // their decay buckets regardless of the host timezone.
        let now = 1_800_000_000_000i64;
        let day = 86_400_000i64;
        // B: three visits ~3 days ago -> 3x90 = 270 beats A's single
        // fresh visit (100): the multi-visit sum is the ordering key.
        s.add("http://b.example/old", "B", now - 3 * day).unwrap();
        s.add("http://b.example/old", "B", now - 3 * day - 1000).unwrap();
        s.add("http://b.example/old", "B", now - 3 * day - 2000).unwrap();
        s.add("http://a.example/fresh", "A", now).unwrap();
        // C only matches its title, not the url.
        s.add("http://c.example/", "needle page", now).unwrap();
        let rows = |json: &str| -> Vec<serde_json::Value> {
            serde_json::from_str(json).unwrap()
        };
        let r = rows(&s.suggest("", 500, now).unwrap());
        assert_eq!(r[0]["url"], "http://b.example/old");
        assert_eq!(r[0]["frecency"], 270);
        // A and C tie at 100 frecency — the word-boundary bonus
        // decides: "" matches everywhere, so both get doubled; the
        // tie-break is newest ts then url.
        let r = rows(&s.suggest("example", 500, now).unwrap());
        assert_eq!(r.len(), 3);
        // term filter: only the title-matching row survives.
        let r = rows(&s.suggest("needle", 500, now).unwrap());
        assert_eq!(r.len(), 1);
        assert_eq!(r[0]["url"], "http://c.example/");
        // word-boundary doubling: "b" starts b.example's host at a
        // boundary, "fresh" does not appear at one in a.example.
        let r = rows(&s.suggest("b", 500, now).unwrap());
        assert_eq!(r[0]["url"], "http://b.example/old");
        assert_eq!(r[0]["score"], 540);
        // limit truncates the marshal.
        assert_eq!(rows(&s.suggest("", 1, now).unwrap()).len(), 1);
        std::fs::remove_dir_all(Path::new(&p).parent().unwrap()).ok();
    }

    #[test]
    fn reopen_preserves_visits() {
        let p = temp_db("reopen");
        {
            let mut s = store_at(&p);
            s.add("http://persist/", "P", 42).unwrap();
        }
        {
            let mut s = store_at(&p);
            assert_eq!(s.count().unwrap(), 1);
            assert!(s.entry_at(0).unwrap().unwrap().contains("http://persist/"));
        }
        std::fs::remove_dir_all(Path::new(&p).parent().unwrap()).ok();
    }
}
