# WEBKIT-PACKAGING — how the WPE .so set ships inside deb/rpm/flatpak (WKB05)

Date: 2026-10-18 · Status: strategy + tooling, nothing applied to live
packaging · Feeds the webkit spine (`.devin/Arora-Task-WebKit.md`) — the
rows that need this are WK06 (build gate), WK19 (surfaces) and any
future "ship the webkit backend" packaging row. Companion docs:
`.devin/WEBKIT-STUDY.md` (API mapping), `WEBKIT-TOR.md`,
`WEBKIT-EXTENSIONS.md`, `WEBKIT-DEVTOOLS.md`.

All measurements below come from `.devin/wpe-deps.sh` run against the
live `~/wpe` prefix WK00 acquired — Debian **sid** `libwpewebkit-2.0-1`
**2.54.1-1** plus its bundled dep set and a sid `libc6` 2.43 (host:
Ubuntu 24.04-class glibc **2.39**). The numbers are real, not estimated.

## The payload — what "ship WPE" actually means

`wpe-deps.sh scan ~/wpe` seeds the transitive `DT_NEEDED` closure from
`libWPEWebKit-2.0.so.1` + the four WPE helper binaries
(`WPEWebProcess`, `WPENetworkProcess`, `WPEGPUProcess`,
`libWPEInjectedBundle.so` — upstream dev tools MiniBrowser/WPEWebDriver
are excluded: they are not part of the embed payload, and MiniBrowser
alone drags in the legacy `libWPEBackend-fdo-1.0.so.1`).

Result on the sid 2.54.1 set:

| class | count | size | examples |
|-------|-------|------|----------|
| WPE stack itself | 7 | ~127M | libWPEWebKit (124M), libwpe-1.0, 4 helpers |
| version-skewed deps (soname ≠ noble) | ~30 | ~85M | icu .78 (noble: 74), libxml2 .16 (noble: abi12/.so.2), libjpeg .62 (noble: .8), libjxl .0.11, libabsl* .20260526, libwebp/mux/demux, libxml2, libavif16, gstreamer **1.28** (noble: 1.24), flite voices (~29M — hard NEEDED, speech synthesis), AV1 stack aom/dav1d/gav1/rav1e/SvtAv1Enc/yuv (~20M), libenchant/hunspell/aspell |
| bundled glibc | 2 | 3.1M | libc.so.6, libm.so.6 2.43 (+ ld.so, gconv — see below) |
| system-resolved on noble | 46 sonames | — | glib/gio stack, atk, wayland, xkbcommon, freetype/fontconfig/harfbuzz/png, gbm/drm/EGL/GL, seccomp, soup-3.0(!), sqlite3, xslt, lcms2, gcrypt, systemd, udev, input, xcb/X11, z, brotli, orc, tasn1, hyphen, gudev, asound |
| MISSING | 0 | — | closure is fully satisfied by prefix+host |

Total NEEDED-closure payload: **72 files ≈ 223M uncompressed**.

### What the NEEDED walk does NOT see

- **GStreamer plugins** — dlopened from `*/gstreamer-1.0/`. The sid
  prefix carries only `libgstcoreelements/coretracers` (512K): Debian's
  package expects *distro* `gstreamer1.0-plugins-{base,good}` + `libav`
  as dependencies. **ABI trap:** plugin builds must match the gst core's
  feature version — our bundled core is **1.28**, noble's plugin dirs
  are **1.24**, so the host's plugins get rejected by the registry. Any
  bundling plan must ship the matching 1.28 plugin set (or accept
  media-degraded operation — WPE still renders, no `<video>`/WebRTC).
- **enchant providers** (`enchant-2/enchant_{hunspell,hspell,aspell}.so`)
  — dlopened for spellcheck; ship iff spellcheck stays enabled.
- **`usr/share/wpe-webkit-2.0/inspector.gresource`** — non-ELF resource
  required for the WebKitWebInspector surface (WKB04's devtools path).
- **gconv modules** under the bundled glibc — iconv resolution breaks
  without them (present in `glibc/usr/lib/.../gconv`).
- **Sandbox helpers**: WPE's process sandbox execs **bwrap** +
  **xdg-dbus-proxy**. Both exist on this host; the package must depend
  on them or WPE falls back to unsandboxed helpers (WebKit tolerates
  `WEBKIT_FORCE_SANDBOX=0`-style degradation; under flatpak WebKit
  detects `/.flatpak-info` and uses flatpak's own sandboxing instead).

### The glibc floor — the finding that shapes everything

`wpe-deps.sh floor ~/wpe`:

```
payload max undefined refs: GLIBC_2.43 GLIBCXX_3.4.30 CXXABI_1.3.9
host glibc: 2.39
```

`libWPEWebKit-2.0.so.1` itself references `GLIBC_2.42`/`2.43` symbols.
A shim cannot satisfy *version* refs — only a real newer libc.so.6 can —
so the whole process (arktos included, if it NEEDED-links the lib) must
start under a ≥2.43 glibc. WK00 proved both mechanisms on this host:

- **patched PT_INTERP** — the three `WPE*Process` helpers were
  `patchelf --set-interpreter <glibc>/ld-linux` + rpath'd; they run
  standalone. For OUR binaries the same applies, but PT_INTERP is an
  *absolute* path: a packaged build must patch to the installed
  `/usr/lib/arktos/...` path at stage time (or link with
  `-Wl,--dynamic-linker=` when `CONFIG+=webkit`).
- **launcher-execs-ld.so** — `exec <glibc>/ld.so <binary>` puts the
  whole process under the bundled glibc with *no binary modification*.
  This is the cleaner packaging route: `/usr/bin/arktos` is already a
  shell-script launcher (pkg-deb.sh stage), so it becomes a one-line
  change — see staged diff below. glibc is forward-compatible: bundled
  Qt 6.12 + all system deps run fine under 2.43.

## The tool — `.devin/wpe-deps.sh`

```
wpe-deps.sh scan TARGET    TARGET = WPE prefix dir | dir of .debs | .deb files
wpe-deps.sh debs TARGET    scan + per-source-deb SHIP/DROP verdicts
wpe-deps.sh floor TARGET   GLIBC_/GLIBCXX_/CXXABI_ floor report only
wpe-deps.sh selftest       gcc-fixture assertions (no WPE needed)
```

`scan` walks DT_NEEDED transitively (readelf — no ldd, nothing
executes), resolves each soname prefix-first then through the host's
ldconfig cache (arch-filtered — a 32-bit shadow can't win), and prints:
the payload file list + sizes; per-soname verdict `bundle`/`dep`/
`MISSING` with host package via `dpkg -S`; dlopen-only payload
candidates; PT_INTERP of every prefix executable (in-prefix vs host vs
ABSENT); version floors; and a curated soname × target-distro matrix
for the headline sonames. `debs` additionally reports which source
.debs contribute ≥1 closure file (SHIP) vs headers/dev-tools (DROP) —
that is the minimal acquisition set when refreshing the bundle.

## Per-format analysis

### (a) deb — Depends vs bundled private libs

**Depends-on-distro is a dead end on our actual target.** Ubuntu
24.04 (noble) packages *no* WPE at all — `apt-cache policy
libwpewebkit-2.0-1 libwpe-1.0-1` returns nothing; even libwpe is
absent. Debian side: `libwpewebkit-2.0-1` exists only in **sid**
(2.54.x) — trixie's 2.48 predates WPEPlatform entirely. So a
`Depends: libwpewebkit-2.0-1 (>= 2.54)` deb is installable nowhere our
users are. The SONAME risk is also real even where it exists: sid is a
moving floor — icu78→79, absl rebuilds — and each transition would
break our deb until refreshed. We already bundle Qt for exactly this
reason; there is no consistency argument for treating WPE differently.

**Bundled private libs — the working model.** Stage the closure under
`/usr/lib/arktos/wpe/` mirroring the existing `/usr/lib/arktos` private
Qt layout:

```
/usr/lib/arktos/wpe/lib/           the 72-file closure (.so set)
/usr/lib/arktos/wpe/lib/wpe-webkit-2.0/{WPEWebProcess,WPENetworkProcess,
                                       WPEGPUProcess,injected-bundle/}
/usr/lib/arktos/wpe/glibc/         sid libc/libm/ld.so + gconv
/usr/lib/arktos/wpe/share/wpe-webkit-2.0/inspector.gresource
/usr/lib/arktos/wpe/lib/gstreamer-1.0/   matching-1.28 plugin set
/usr/lib/arktos/wpe/lib/enchant-2/       providers (spellcheck opt-in)
```

Mechanics at stage time (all proven by WK00's layout):
- `patchelf --set-interpreter /usr/lib/arktos/wpe/glibc/.../ld-linux
  --set-rpath /usr/lib/arktos/wpe/glibc/...:/usr/lib/arktos/wpe/lib`
  on the three WPE*Process helpers (interp must be the *installed*
  absolute path — ~/wpe paths don't survive packaging).
- arktos itself: **preferred** = no binary change at all; the
  `/usr/bin/arktos` launcher gains a conditional
  `exec $ARKTOS_HOME/wpe/glibc/.../ld.so $ARKTOS_HOME/bin/arktos "$@"`
  (only when the wpe payload is staged — the launcher already wraps
  LD_LIBRARY_PATH). Fallback = patchelf the arktos interp identically.
- Depends additions: `bubblewrap` (WPE process sandbox) — cheap, distro
  ships it everywhere; xdg-dbus-proxy likewise.
- `dpkg-shlibdeps` must NOT see the wpe libdir in its `-l` scan (or it
  emits Depends on SONAMEs only we provide — same policy as the Qt
  payload today).
- Cost: ~+90–110M compressed deb on top of the current payload
  (223M uncompressed .so set; xz on ELF data lands ~40–45%). That's a
  real size jump — acceptable for an optional engine payload, and a
  `Recommends:`-style split deb (`arktos-engine-webkit`) is available
  as a later refinement if size matters.

### (b) rpm — the one place distro-packaged WPE exists (sort of)

The spec row said "Fedora-side where WPE IS packaged" — that turned
out stale on inspection:

- **Fedora proper does NOT ship wpewebkit** — the official package was
  orphaned in 2020 (F32 era); the only current builds are the
  `philn/wpewebkit` COPR (Igalia-maintained, credible but a COPR —
  repodata coverage per Fedora release is spotty: F44's engine RPM was
  built against F42 only). Depending on a COPR in our spec is not
  shippable.
- **openSUSE Tumbleweed/Leap DO package it** — Factory `wpewebkit`
  (currently 2.52.x; Leap 16 has 2.46.6) with split packages:
  `libWPEWebKit-2_0-1`, `wpewebkit-devel`,
  `wpewebkit-2_0-injected-bundles`, `wpewebkit-minibrowser`.
  Our spec is already openSUSE-flavored, so `Requires:
  libWPEWebKit-2_0-1 >= 2.52` is *technically* viable on TW — BUT our
  floor is 2.54 (WPEPlatform stable; 2.52 lacks the API level the spine
  targets), and TW's 2.52.4 doesn't satisfy `>= 2.54` today.

**Verdict: same bundled payload as the deb.** Uniform artifacts, exact
version pinning, zero dependence on distro cadence — and
`AutoReqProv: no` is already set precisely for bundled SONAMEs. The
`Requires: libWPEWebKit-2_0-1` route is documented for a future
Tumbleweed-only variant once TW ships ≥2.54: it would mean an
unbundled build on TW (system WPE → no glibc games, no 200M payload),
which is genuinely attractive — worth revisiting when TW catches up.
Requires additions for the bundled path: `bubblewrap` only (all 46
system sonames already map onto packages the spec or base system
provides — regenerate via `pkg-rpm.sh requires` once the payload is
staged so the drift check keeps them honest).

### (c) flatpak — the glibc floor bites here

Measured on the installed `org.kde.Platform//6.11` (= freedesktop
25.08 base): **glibc 2.42** — one short of the sid set's 2.43 floor, so
the extracted Debian binaries *do not run inside our current
manifest's runtime*. Options:

1. **Bundled module, built from source (recommended).** Add a
   `wpewebkit` module compiling the pinned 2.54.x release tarball
   inside org.kde.Sdk. The runtime already carries nearly everything:
   measured present — libsoup-3.0, gstreamer-1.0 (+226 plugins in the
   image), libwebp7, libseccomp2, libepoxy, harfbuzz, lcms2, hyphen,
   avif16, jxl-0.11, icu-77, xml2-16, jpeg62, atk-bridge, gbm, gcrypt,
   sqlite3, xslt, png, tasn1. Missing and needed only as *small*
   modules: **libwpe** (tiny), plus optional feature libs (enchant/
   flite can be configured off: `-DENABLE_SPEECH_SYNTHESIS=OFF`
   `-DUSE_LIBENCHANT=OFF` — decide per capability honesty in WK19).
   Build-time deps (unifdef, gperf, ruby, cmake/ninja) live in the SDK.
   Cost: a multi-hour WebKit build in flatpak-builder — fine for the
   dev loop (state-dir caching), painful for CI unless cached. Honest
   upside: no glibc games, no foreign libc inside the sandbox, plugins
   match the runtime's gstreamer.
2. **Stage the sid binaries + private glibc inside /app** — viable
   stopgap, same trick as the deb (`/app/lib/arktos/wpe/glibc/ld.so`
   interp on helpers, launcher-execs-ld.so for arktos), but foreign
   glibc inside a flatpak is fragile (NSS/gconv/module-path corner
   cases) and doubles up against the runtime's own libc — documented,
   not recommended unless option 1's build time is unacceptable.
3. **Runtime extension** — there is no upstream
   `org.freedesktop.*.Extension.wpewebkit`; we'd be publishing our own
   extension just for ourselves. Only pays off when several apps share
   it; adds a second release artifact and flathub review surface.
   Not recommended now — revisit if WPE extensions emerge upstream.
4. **Honest fallback** — engine caps already handle "backend
   unavailable": ship the flatpak without the webkit engine until the
   module exists (the spine's compile-but-inert pattern means the
   binary builds regardless). This is the zero-cost default.

Also note under flatpak: WPE detects `/.flatpak-info` and does not
spawn its own bubblewrap sandbox — flatpak's sandboxing covers it, so
no bwrap dependency inside the manifest.

## Recommended strategy

| format | strategy | why |
|--------|----------|-----|
| deb | **bundle** under /usr/lib/arktos/wpe + bundled glibc + launcher-execs-ld.so; Depends += bubblewrap | nothing to Depend on anywhere we ship; mirrors the private-Qt policy; version-pinned |
| rpm | **bundle**, identical payload (pkg-rpm stage already delegates to pkg-deb stage) + Requires: bubblewrap | Fedora has no WPE; TW's 2.52 < our 2.54 floor; uniform artifact wins. Revisit the `libWPEWebKit-2_0-1` Requires path when TW ships ≥2.54 |
| flatpak | **in-manifest source-build module** (fallback: ship without webkit engine until it lands) | runtime glibc 2.42 < 2.43 kills the sid binaries; runtime already carries ~90% of the dep surface |

Cross-cutting: whichever engine linkage WK06 chooses, prefer the
launcher-execs-ld.so deployment over interp-patching arktos — the
packaged launcher script is already ours, and unmodified binaries are
easier to verify (`pkg-deb.sh verify`'s ldd legs stay meaningful).

## Staged changes (NOT applied — land on the webkit spine)

1. **`BuildProcess/fetch-wpe.sh`** — formalize WK00's manual steps:
   download the sid .deb set (list driven by `wpe-deps.sh debs`
   SHIP verdicts), `dpkg -x` into a target prefix, patchelf the
   helpers, write `ACQUIRED.md`. Sketched interface:
   `ARKTOS_WPE_PREFIX=<dir> ./BuildProcess/fetch-wpe.sh [--glibc]` —
   mirrors `fetch-tor.sh`'s shape.
2. **`pkg-deb.sh stage`** — after the tor leg:
   ```
   if [ -d "$ARKTOS_WPE_PREFIX" ]; then
       install -d "$D/$LIBDIR/wpe"
       # closure list from: ./.devin/wpe-deps.sh scan "$ARKTOS_WPE_PREFIX"
       cp -a <payload files> "$D/$LIBDIR/wpe/"
       patchelf --set-interpreter /usr/lib/arktos/wpe/glibc/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 \
                --set-rpath /usr/lib/arktos/wpe/glibc/usr/lib/x86_64-linux-gnu:/usr/lib/arktos/wpe/lib \
                "$D/$LIBDIR"/wpe/lib/wpe-webkit-2.0/WPE*Process
   fi
   ```
   and the `/usr/bin/arktos` launcher gains:
   ```
   if [ -x "$ARKTOS_HOME/wpe/glibc/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" ]; then
       exec "$ARKTOS_HOME/wpe/glibc/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" \
            "$ARKTOS_HOME/bin/arktos" "$@"
   fi
   ```
   plus `Depends: bubblewrap` in debian/control and the wpe libdir kept
   OUT of the dpkg-shlibdeps `-l` list.
3. **`rpm/arktos.spec`** — same stage (free via delegation), add
   `Requires: bubblewrap`, and the `wpe-deps.sh scan` output becomes the
   input to the Requires-drift check (system sonames join the explicit
   Requires block exactly like today's Qt payload).
4. **`com.arktos_browser.Arktos.yaml`** — add a `wpewebkit` module
   before the arktos module (staged sketch):
   ```yaml
   - name: wpewebkit
     buildsystem: cmake-ninja
     config-opts:
       - -DPORT=WPE
       - -DENABLE_WPE_PLATFORM=ON
       - -DENABLE_SPEECH_SYNTHESIS=OFF   # flite not in runtime
       - -DUSE_LIBENCHANT=OFF            # decide with WK19 caps
       - -DCMAKE_BUILD_TYPE=Release
     sources:
       - type: archive
         url: https://wpewebkit.org/releases/wpewebkit-2.54.1.tar.xz
         sha256: <pin — fetch + record when staging for real>
   - name: libwpe          # if the resolved wpewebkit still wants it
     buildsystem: cmake-ninja
     sources: [ wpe-1.16.x tarball, sha256-pinned ]
   ```
   Module order: libwpe (if needed) → wpewebkit → arktos. `qmake
   src.pro` gets `CONFIG+=webkit` gated the same way the servo stanza
   is (WK06 owns that bit).
5. **verify legs** — `pkg-deb.sh verify` / `pkg-flatpak.sh verify` add
   a `--engine-list` or WK07-equivalent probe asserting the webkit
   backend reports available from the installed layout.

## Verification record (this task)

- `./.devin/wpe-deps.sh selftest` — PASS: fixture prefix (two compiled
  .so's + a PIE + a dlopen-only plugin) scans correctly; fixture .deb
  judged SHIP.
- `./.devin/wpe-deps.sh scan ~/wpe` — ran clean end-to-end: 72 payload
  files / 46 system sonames / 0 missing; floors GLIBC_2.43 /
  GLIBCXX_3.4.30; PT_INTERP of all three WPE*Process confirmed
  in-prefix; MiniBrowser's libWPEBackend-fdo drag identified and
  excluded.
- Runtime probes: `apt-cache` — noble has no WPE packages; flatpak
  `org.kde.Platform//6.11` inspected live (glibc 2.42; dep presence
  table above is measured, not assumed).

## Open items the spine inherits

- **GStreamer plugin set** for the bundled path: pick the 1.28 plugin
  debs (plugins-base/good/bad-free/libav) by feature need — WK19's
  media/WebRTC capability decision drives the list. The version-match
  trap (bundled core vs system plugins) is real in both directions.
- **flite (~29M) is a hard NEEDED** of the sid build — can't be
  dropped from the binary set; only a from-source build
  (`-DENABLE_SPEECH_SYNTHESIS=OFF`) sheds it.
- **Security cadence**: bundled = we own WPE CVE refreshes. Same
  posture as bundled QtWebEngine today; the fetch script should pin
  deb versions + record sha256 like fetch-tor.sh does.
- **WPE sandbox vs our containers**: WPE wants bwrap on the host for
  its process sandbox — confirm our Tor/profile isolation doesn't
  conflict (WK19 surface).
