# RAUDIT01 — rustcore code-quality sweep

Date: 2026-10-11. Scope: `src/rustcore/` (~21.2 kLOC across 27 modules +
the C ABI surface in lib.rs). Method: `cargo build`/`clippy` warnings
review, FFI export ↔ Qt call-site cross-check, targeted reads of the
hot paths and the parser/FFI boundaries, drift diffing of the three
rust-vs-C++ twins named in the task.

## Fixed this run

| Finding | Class | Fix | Commit |
|---|---|---|---|
| `bookmarkhtml::find_closing_tag` — the 256 KiB per-tag window bounded one scan, not the count: N unclosed `<A>` tags cost N×window (~200 MB scanned on the 600 KB hostile fixture; `import_bad_parent_and_hostile_input` took >15 s and was failing its <10 s bound). A 64 MB file of unclosed tags was effectively unbounded CPU. | perf / unbounded work | Document-wide scan budget: 4×input + one window of slack, charged per scanned byte; exhaustion ⇒ empty title + `truncated` in the summary. Hostile test now ~50 ms. | `dba7a6d` |
| `blocklist::contains` vs `tranco::rank_of` — duplicated normalize+suffix-walk (trim/lowercase/dot-strip/`[v6]` unbracket/label-boundary probing) and duplicated `validate_domain` DNS-shape gate. Blocklist additionally heap-allocated a `String` per probe (`to_lowercase`) on the per-request hot path, and an over-253-byte host was probed there but dropped by tranco. | duplication / perf | `util::HostNorm` (stack-buffer normalize + `suffixes()` iterator) + `util::valid_dns_shape`; both probes now zero-alloc and share the gate. Over-length hosts probe their rightmost 253 bytes (every entry is ≤253, so any matchable suffix lives in the tail) — preserves blocklist's old reach, tranco gains it. | `d30db25` |
| 110 clippy warnings on `cargo clippy --all-targets` | lint debt | Zero now: 57 `unsafe extern` fns gained `# Safety` doc sections (the shared FFI contract; the 83 that already had per-fn sections kept theirs), doc-list indentation normalized, ~20 mechanical lints applied (redundant closures, `map_or`→`is_none_or`, `strip_prefix`, `sort_by_key(Reverse)`, `assert!` over `assert_eq!(…, true)`, `contains_key`, elided lifetime). | `a4e89d8` |

## Audit results by category

### (a) Dead code

- `cargo build` reports no `dead_code` warnings.
- FFI exports with no Qt caller (15 of 140): `rc_sitetrust_lookup`,
  `rc_sitetrust_reload`, `rc_lookalike_check` — deliberately ahead of
  the main-lane badge work (TRUST08/09 per the TRUST01 row);
  `rc_cred_get/put/remove/list/change_passphrase` — the RCORE01 part-B
  named-credential ABI surface (SecureStore consumes the custody
  primitives; nothing consumes the map yet); `rc_bm_find/parent/
  set_tags`, `rc_is_sealed`, `rc_ose_url`, `rc_session_encode`,
  `rc_ua_spoof_list` — API-surface completeness/testing seams. Kept:
  removing them is an ABI break on `include/rustcore.h`, not a
  trivially-safe fix. Revisit if still unwired after TRUST08/09.
- No stale modules found; every module maps to a landed task row.

### (b) Duplication + drift

- Host normalization/`validate_domain` duplication — **fixed** (above).
- `RwLock<Option<Arc<T>>>` lazy-list + data-dir-staleness machinery is
  triplicated across blocklist/tranco/urlstrip (~25 lines each). A
  generic shared slot helper is a design-level change — filed as
  RAUDIT02.
- DRIFT — `rustdl::engine::referer_for` vs
  `PrivacyRequestInterceptor::rewrittenReferer`: verdicts agree on the
  full policy matrix except (i) the C++ level-0 early return
  (`rewrittenReferer(0,…)` would hand back the full URL but the
  interceptor never calls it at level 0 — Chromium's strict-origin
  header stands; `referer_for(0,…)` models exactly that effective
  header) and (ii) single-label suffix hosts (`x.uk` vs `uk`): the C++
  `endsWith` fast path calls them same-site, `domain_key` calls them
  cross-site — conservative direction (less referer sent). Both are
  documented modeling choices; no action.
- DRIFT — `bookmarkhtml` vs the C++ `convertHtmlToXbel` fallback: the
  Rust side decodes a curated HTML4 entity subset while the Qt path
  uses `QTextDocumentFragment` (full HTML4 entity set, e.g. `&rArr;`),
  and does not percent-encode hrefs where `encodeUrl()` did. Title/url
  fidelity differences only, documented here.
- `parsers.rs` vs the Qt OpenSearch/XBEL readers: no independent twins
  remain — the Qt readers delegate to `rc_opensearch_parse` /
  `rc_xbel_check` under `ARORA_RUSTCORE`, so the only drift surface is
  the intentional no-rust fallback.

### (c) Performance

- Closing-tag scan quadratic — **fixed** (above).
- `blocklist::contains` per-probe `String` allocation — **fixed**
  (zero-alloc `HostNorm`, matching `rank_of`'s discipline).
- `lookalike::probe` is allocation-free for clean verdicts (verdict
  `String`s only allocate on the LOOKALIKE path); `tranco::rank_of`
  zero-alloc; `sitetrust::lookup` allocates only its JSON result
  (the FFI return); `urlstrip::strip` allocates the result string by
  contract. No unbounded-growth in-memory indexes found —
  `sitedecisions` caps kinds/keys/values/file-size, `bookmarks` caps
  XBEL size+depth, `history` is sqlite-bounded, `tranco`/`blocklist`
  cap entry counts.
- `rustdl::domain_key` allocates a `String` per redirect hop — per
  download-hop, not per-request; left as-is.

### (d) Security / safety

- All 140 `rc_*` entry points run under `catch_unwind`/`status_of` —
  panic-safe by construction. Production-code `unwrap`/`expect`
  inventory: all are invariant-guarded (non-empty `levels` stack,
  store-state `Option`s opened only inside `is_some` gates, fixed
  4-byte `try_into`, vendored-data parse assertions) — none reachable
  on untrusted input.
- `unsafe` outside `#[cfg(test)]`: only `util::{bytes,key32,cstr}` and
  the lib.rs FFI marshaling — all null-checked before deref, all
  justified. `unsafe_member_name` in extverify.rs is a misnamed safe
  fn (name kept — it's descriptive of purpose, not a block).
- Input bounds: every parser entry enforces a size bound
  (bookmarkhtml 64 MB, tranco member 64 MB / lines 512 B, opensearch/
  XBEL bounded, sitedecisions 4 MB file cap); integer arithmetic on
  offsets uses checked/saturating ops (spot-checked extverify CD walk,
  session decode cursor, cred blob reader).
- Mutex-poison discipline is inconsistent: `store::lock()` and the
  `RwLock` list slots recover via `into_inner()`, while the per-module
  `Mutex::lock().unwrap()`s (opensearch, sitedecisions, …) propagate
  poison — contained by `catch_unwind`, so a poison degrades that call
  to an error rather than crashing the boundary. Documented; per-module
  recovery choice left to the owners (report-only).
- `ffi_surface.rs` (COV05 coverage tests) already drives every export's
  happy + bad-argument path — panic-safety at the boundary is tested,
  not assumed.

## Verification

- `cargo test` — 186/186 PASS (baseline before this run: 185/186, the
  timing assertion above was a real defect).
- `cargo clippy --all-targets` — 0 warnings (baseline: 110).
- ABI unchanged: no `include/rustcore.h` or export-signature edits.

## Filed follow-ups

- RAUDIT02 (new task row): deduplicate the `RwLock<Option<Arc<T>>>`
  lazy-list + dir-staleness slot machinery shared by blocklist.rs,
  tranco.rs, urlstrip.rs into a single helper.
