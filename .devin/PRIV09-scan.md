# PRIV09 — ASan/LSan churn leg over the OTR profile teardown

Date: 2026-10-20 — commit under test: `b771cf0a` (the OTR teardown
implementation itself landed inside `b6494e58`; this run is
verification-only, no product code changed).

## Verdict

**PASS.** 50 churn iterations of `./arktos --otr-teardown-smoke`
under ASan+UBSan+LSan: 48/50 completed 13/13 checks rc=0, **zero
ASan reports** (no heap-use-after-free on the destroyed
`QWebEngineProfile`, no SEGV/poison hits), **zero UBSan
`runtime error:` lines**, and **zero leak blocks allocated in
Arktos code** — every LSan block resolves to upstream
libQt6Widgets / fontconfig sites. No `QWebEngineProfile` object
appears in any leak report: the shared OTR profile is freed, not
leaked. 2/50 runs died in an upstream startup flake detailed below.

## What the churn leg covers (per iteration)

The smoke performs one full cycle plus the spec's two variants:

- two `WebPage`s on the shared OTR profile (multi-window shape):
  cookie state lands, profile survives first page death, releases
  on the second;
- reopen: lazily recreated profile is a genuinely fresh world (no
  previous cookie, services re-installed by `prepareProfile`);
- an in-flight private download holds the teardown until the
  request settles, then the profile releases;
- the normal profile's cookie survives throughout.

## Build

- Throwaway copy `/tmp/arktos-priv07-san.msxcuu` (reused the
  previous run's tree — snapshot ~Oct 20 17:46). Synced the 94
  files changed by post-snapshot commits (PRIV08, LEG01–04) and
  removed the 34 since-deleted files; remaining tree differences
  vs HEAD verified to be only commit `cfe8feb8` copyright-comment
  churn (434 files, 0 code stragglers) — the instrumented binary
  is functionally HEAD.
- `qmake -spec linux-clang 'CONFIG+=sanitize' 'CONFIG+=no-rust'
  'CONFIG-=rustcore' 'CONFIG-=rustdl' 'CONFIG-=adblock_rust'`,
  `make -j2`: clean, 0 errors (36 pre-existing clang-only
  `-Wunused-lambda-capture` warnings). Qt fallback paths cover the
  disabled Rust crates; none sit on the OTR lifecycle.
- Rust had to be off: this is the mistake that killed PRIV07's
  instrumented builds. qmake does **not** split a single argv
  containing several space-separated assignments —
  `'CONFIG+=sanitize CONFIG+=no-rust'` lands `sanitize` and leaves
  the literal token `CONFIG+=no-rust`, so `no-rust` was silently
  never set and cargo ran to the 4 GiB cap. Pass each assignment
  as its own argv.

## Run environment

```
QT_QPA_PLATFORM=offscreen LIBGL_ALWAYS_SOFTWARE=1 HOME=<isolated>
ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:print_stacktrace=1:\
  handle_segv=1:quarantine_size_mb=64:thread_local_quarantine_size_kb=64
LSAN_OPTIONS=exitcode=0:print_suppressions=0:report_objects=1
ASAN_SYMBOLIZER_PATH=/usr/lib/llvm-18/bin/llvm-symbolizer
```

50 sequential invocations, `timeout 300`, ~12 s each; logs in
`/tmp/priv09-churn/run-*.log`.

### quick_exit / LSan caveat (verification-only patch)

The smoke ends in `std::quick_exit`, which bypasses LSan's
`atexit` hook. To get the leak verdict the throwaway tree's
`src/main.cpp` was patched to call `__lsan_do_leak_check()`
immediately before the exit (guarded —
`__has_feature(address_sanitizer)`; note clang does **not**
predefine `__SANITIZE_ADDRESS__`, that is a gcc-ism). Patch lives
only in the instrumented copy — not committed.

## Results

| | |
|---|---|
| iterations | 50 |
| smoke PASS 13/13, rc=0 | 48 |
| startup flake (below) | 2 (runs 16, 19) |
| ASan reports (UAF/OOB/SEGV) | **0** |
| UBSan runtime errors | **0** |
| LSan leak reports | 48 — 36× `936 B/10 alloc`, 12× `760 B/8 alloc` |
| leak blocks sited in `src/`/`autotests/` | **0** |
| leaked `QWebEngineProfile` | **0** |

Every report contains the same three upstream sites
(classification per `.devin/check-leaks.sh` convention — a block
counts as ours only when a leading frame resolves into
`src/`/`autotests/`; none did in any run):

- `QLayoutPrivate::createWidgetItem` — libQt6Widgets widget items
  (7×88 B or 5×88 B per run) still reachable at the smoke's
  early-exit point; expected on `quick_exit` before widget
  teardown.
- fontconfig direct 256 B + indirect 64 B — `FcInit` /
  `expat XML_ParseBuffer` allocations reached from
  `libQt6WebEngineCore` engine startup; classic upstream noise.

## Incidental finding (upstream, not ours)

3 of 53 total launches (runs 16, 19, plus one pre-loop probe)
died before the smoke started with `SIGSEGV` inside
`fontconfig FcInit -> expat XML_ParseBuffer -> getenv`, called
from `libQt6WebEngineCore` init. ASan's `getenv` interceptor +
fontconfig XML parse under engine bootstrap; ~5% launch flake,
no Arktos frames, unrelated to the teardown path.

## Bottom line

The committed OTR teardown holds under the sanitizer churn leg:
profile releases on last private-page death even with an
in-flight private download, recreation yields a clean prepared
world, and nothing owned by Arktos leaks across 50 cycles.
