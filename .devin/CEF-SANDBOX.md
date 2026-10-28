# CEF-SANDBOX — real Chromium sandbox + profile isolation (CS04)

Companion: `CEF-SPIKE.md` (facade/touchpoint map), `CEF-BW-VERDICT.md`
(CS01 extensions), `CEF-INPUT-VERDICT.md` (CS02 embed), `CEF-CAPS.md`
(CS03 shape). CS04 measures two orthogonal questions with running code:

1. Can the prebuilt CEF run with Chromium's **real** sandbox
   (`no_sandbox=0`), and does it survive being nested inside the
   existing `src/sandbox/bwrapgenerator` filesystem wrap?
2. How do `root_cache_path` / `cache_path` / `CefRequestContext` map
   onto the facade-owned `profiles/<id>/` layout — personas, containers,
   and OTR?

Honesty rule applies throughout: a FAIL leg is a measured answer, not a
harness defect.

## Method

- CEF `152.0.15+gb3d72fb` / Chromium `152.0.7977.169` linux64 (Spotify
  prebuilt). Its `Release/chrome-sandbox` helper ships **mode 0755,
  owned by the user** — i.e. not a functional setuid helper.
- Probe: `spikes/cef/qcefprobe` extended — `--sandbox` /
  `CEF_SANDBOX=1` flips `CefSettings.no_sandbox` to 0 (default stays
  off so CS01/CS02 legs are unchanged), `--hold=<ms>` parks the run
  for live `/proc` inspection, `--mode=profiles` +
  `--profiles-root=<dir>` runs the persona battery.
- Runner: `spikes/cef/qcefprobe/run-sandbox.sh` — sandbox legs run
  OSR/offscreen (no display needed); profile legs drive a real HTTP
  origin (`127.0.0.1` fixture) because cookies/localStorage need one.
- Enforcement is measured, not assumed: `rend_evidence()` samples
  **every** live renderer and reports the worst case —
  `Seccomp`/`NSpid` from `/proc/<pid>/status` plus user/PID/mount
  namespace inode diffs vs the browser process. `Seccomp:2` =
  seccomp-BPF filter active; `NSpid` depth >1 = nested PID namespace.
- Host posture (measured at leg start):
  `unprivileged_userns_clone=1`, `max_user_namespaces=55310`,
  `apparmor_restrict_userns=0`, bwrap present. Unprivileged user
  namespaces are fully available on this machine.

## Sandbox leg table (measured 2026-10-25)

| Leg | Verdict | Evidence | Reading |
|---|---|---|---|
| nosandbox-ctl | **PASS** | 2 renderers, `min-seccomp=0`, `max-nspid=1`, ns-diff 0/2 | Control: default probe config leaves renderers unconfined — the baseline the sandbox must beat. |
| userns | **PASS** | 2 renderers, `min-seccomp=2`, `max-nspid=3`, userns-diff 2/2, pidns-diff 2/2 | Real Chromium sandbox engages via **unprivileged user namespaces** — no setuid helper, no root, no flags beyond `no_sandbox=0`. Renderers get seccomp-BPF + nested PID ns + own userns. |
| setuid-userns | **FAIL** | `zygote_host_impl_linux.cc:102] Running as root without --no-sandbox is not supported` (crbug.com/638180) | `unshare -Ur` maps euid→0 and Chromium refuses outright — a userns cannot bootstrap the setuid path. The setuid leg is **untestable without root** (helper would need mode 4755/root), and this prebuilt's helper is 0755 anyway — it would FATAL even if reachable. |
| bwrap-nosb | **PASS** | 2 renderers inside an arktos-style denylist wrap (`--unshare-all --dev-bind / /` + per-path tmpfs/null binds), `min-seccomp=0` | The bwrap filesystem wrap alone does not break CEF: renderers spawn, page loads — but Chromium's own sandbox is off, so only the denylist protects the host. |
| bwrap-sandbox | **FAIL** | `FATAL:sandbox/linux/suid/client/setuid_sandbox_host.cc:166] The SUID sandbox helper binary was found, but is not configured correctly... owned by root and mode 4755` + zygote broken pipe | Inside bwrap's user namespace Chromium **cannot create its own sandbox userns** — it abandons the userns path, falls back to the helper, finds a non-setuid binary, and **hard-aborts at zygote spawn**. |
| bwrap-nohelper | **FAIL** | helper moved aside → `FATAL:zygote_host_impl_linux.cc:129] No usable sandbox!` | With no helper to find, the failure is earlier and blunter — still a hard abort, still no renderers. |
| bwrap-dissuid | **FAIL** | `CEF_SWITCHES=disable-setuid-sandbox` → same `FATAL ... No usable sandbox!` | Skipping the helper lookup changes nothing — no degrade-to-unsandboxed path exists. |

`run-sandbox.sh` totals: sandbox block 3 PASS / 4 FAIL. **Every** bwrap
+ real-sandbox combination ends in a FATAL abort before the first
renderer — Chromium does not degrade, it dies.

### What this means for `src/sandbox/bwrapgenerator`

Arktos's launcher wrap is a filesystem **denylist** (hide $HOME secrets
from every browser-process thread); Chromium's sandbox is
**renderer-confinement** (userns + pidns + seccomp). Different threat
models — but they are not composable as nested sandboxes today:

- The generated wrap `--unshare-all` puts the browser inside one user
  namespace; Chromium's zygote then fails to create a nested one and
  requires the setuid helper — which is either unusable (0755),
  absent, or disabled. All three measured variants FATAL.
- Options a port must choose between (none measured as working
  together):
  a. **CEF outside the wrap** — drop the bwrap layer for the CEF
     process; renderers get Chromium's real sandbox (stronger than a
     denylist), but the browser/GPU processes read all of $HOME — a
     real regression vs the denylist's coverage of the browser proc.
  b. **Wrap + `no_sandbox=1`** — keeps the denylist, loses seccomp/
     pidns on renderers. Strictly weaker than (a)'s renderer story.
  c. **setuid helper inside the wrap** — untestable here (needs root to
     `chown root + chmod 4755`), and likely still broken: bwrap maps a
     single uid, giving the helper no uid range to pivot into.
- Open question for the tree (not measured in this lane): the same
  nested-userns wall presumably already applies to `QtWebEngineProcess`
  under the launcher today — no `QTWEBENGINE_DISABLE_SANDBOX` appears
  anywhere in `src/`. Whether the current engine silently degrades or
  survives another way is worth a dedicated look; CEF's chrome runtime
  measurably does **not** degrade.

## Profile/persona leg table (measured 2026-10-25)

Layout under test: `root_cache_path=<R>/profiles`, contexts for
`alpha`, `beta`, nested `alpha/containers/c1`, and an OTR context —
driven through `--mode=profiles` against a live `127.0.0.1` fixture.

| Leg | Verdict | Evidence | Reading |
|---|---|---|---|
| ctx-persona-fit | **PASS** | `alpha-><R>/profiles/alpha`, `beta-><R>/profiles/beta` (exact `GetCachePath()` round-trip) | Direct-child `cache_path` is honored verbatim. |
| ctx-container-fit | **PASS** | `want=<R>/profiles/alpha/containers/c1 got=` same | The nested path is *accepted* and reports correctly — acceptance is not persistence (see disk-nested). |
| ctx-introspect | **PASS** | `otr.global=0 otr.cache='' a.sharesWith.b=0` | `IsGlobal`/`IsSharingWith`/`GetCachePath` all answer as designed; the empty-path ctx is a real non-global context. |
| persona-write | **PASS** | alpha `cookie=[]→[cs04_alpha=1]`, ls `[]→[alpha]` | Named ctx reads empty, writes, re-reads through a fresh browser. |
| persona-iso | **PASS** | beta `cookie=[]→[cs04_beta=1]` | Beta starts empty *while alpha holds data* — no leakage across ctxs. |
| container-iso | **PASS** | c1 `cookie=[]→[cs04_cont=1]` | A second ctx nested under alpha is still isolated from alpha in-session. |
| otr-session | **PASS** | otr `cookie=[]→[cs04_otr=1]` | OTR ctx works live — session-only. |
| cookie-jar | **PASS** | visitor sees `alpha=[cs04_alpha] beta=[cs04_beta] otr=[cs04_otr]` | Three independent cookie jars via per-ctx `CefCookieManager`. |
| disk-persona | **PASS** | `files alpha=128 beta=128`; `Cookies` sqlite contains `cs04_alpha`/`cs04_beta` respectively; `Local Storage/leveldb` holds the keys | Direct-child ctxs produce full Chromium profile trees that persist across restart. |
| disk-nested | **FAIL** | `files container-c1=0` | **The nested ctx was silently in-memory.** `CreateContext` accepted the path, `GetCachePath` echoed it, session storage worked — and nothing hit disk. Chromium persists a request context only when its `cache_path` is a **direct child** of `root_cache_path`. |
| disk-otr | **PASS** | no `otr` dir in the profile tree | Empty-cache ctx leaves zero disk trace — true OTR behavior. |

Profile block: 10 PASS / 1 FAIL (`disk-nested`).

### The direct-child rule — the architectural finding

- `CefRequestContextSettings` exposes `cache_path` but **no
  `root_cache_path`**: every context shares the process-wide
  `CefSettings.root_cache_path`, and CEF enforces "all cache paths
  share a common root" at init.
- Chromium treats persistent profiles as the *immediate children* of
  the user-data dir. A `cache_path` deeper than one level is accepted
  API-wise and silently runs in-memory — the most dangerous kind of
  failure (everything looks right until restart). This was hit, then
  verified by contrast: direct children persisted (128 files), the
  nested path wrote zero files.
- `CefSettings.cache_path` (the *global* ctx path) follows the same
  rule: set to `<R>/profiles/default` it persisted; any non-child value
  is ignored and data lands in `<R>/Default`.
- `root_cache_path` also carries the process `SingletonSocket` lock —
  one root per CEF browser process, which is why "just give each
  persona its own root" means a separate CEF process per persona.

### Mapping onto the facade layout (RC03/RC09)

Current facade layout: `profiles/<id>/QtWebEngine` sealed storage +
`profiles/<id>/cache/QtWebEngine` disposable cache, containers nested
as `profiles/<pid>/containers/<cid>` under the owning persona. CEF
cannot express that tree in-process:

| Facade concept | CEF mapping | Status |
|---|---|---|
| Persona (persistent) | request ctx at direct child `<root>/<key>` | **fits**, after flattening the id (e.g. `p-alpha`) |
| Container under a persona | **cannot** be `…/<pid>/containers/<cid>` in-process — nested = silently transient | needs flat sibling key (e.g. `c-alpha-c1`) with facade keeping logical ownership, or a separate CEF process+root per persona (singleton-locked, costly, unmeasured) |
| OTR persona | ctx with empty `cache_path` | **fits exactly** — measured zero disk trace |
| Default/global profile | `CefSettings.cache_path` = direct child | **fits** (`<root>/default`) |
| Split sealed-vs-disposable paths | **no equivalent** — `Cache/`, `Code Cache/`, `GPUCache/` live inside the one ctx dir | the `profiles/<id>/cache/` disposable tier collapses into the sealed dir |

So the adapter layer would own a *flat keyspace* under a single
`root_cache_path` (`profiles/cef/<flatkey>`), while the facade keeps
`profiles/<id>` ownership, metadata, and container membership outside
CEF's view. `GetCachePath()` at runtime confirms exactly which dir a
ctx landed on — the verification hook already exists.

## Verdict (feeds CS05)

- **Chromium's real sandbox works** on this host with zero privileges —
  userns path gives renderers seccomp-BPF + nested pidns. That is a
  *stronger* renderer story than the app's current bwrap denylist
  provides.
- **The two sandboxes do not compose**: bwrap-wrap + CEF-sandbox =
  measured FATAL abort in every variant tried (helper unusable, absent,
  or disabled). A CEF port must pick one confinement layer per process
  scope or redesign the wrap (e.g. bind-mount profile dirs into a
  Chromium-userns-compatible jail — unexplored).
- **Persona isolation is real and verified**: independent cookie jars,
  localStorage, on-disk profile trees, and true OTR transience — all
  measured through `CefRequestContext`.
- **The facade's nested container layout does not fit CEF's
  direct-child rule**; containers need flat sibling cache-path keys in
  a shared root (adapter-owned naming) — or per-persona processes,
  which the singleton lock makes expensive.
- Net for CS05: neither finding is a blocker for CEF as an engine, but
  both are real design constraints — sandbox layering is an
  either/or decision, and profile storage needs a flat-key indirection
  between facade ids and CEF dirs.

## Reproduce

```sh
. ./.devin/qt-env.sh
export CEF_ROOT=~/cef-cache/cef_binary_152.0.15+gb3d72fb+chromium-152.0.7977.169_linux64
cd spikes/cef/qcefprobe && ./run-sandbox.sh   # prints the leg table
```

Artifacts stay outside the repo (`/tmp/cs04*`). The script leaves the
tree clean; `chrome-sandbox` is only temporarily renamed mid-leg and
restored.
