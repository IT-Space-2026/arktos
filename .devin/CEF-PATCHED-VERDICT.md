# CEF-PATCHED-VERDICT — what does a patched CEF actually cost/buy? (CS10)

Date: 2026-10-28 · Lane: `cef-spike` · Pin throughout:
CEF `152.0.15+gb3d72fb` / Chromium `152.0.7977.169` (prebuilt Spotify
binary *and* the CS07 source build — version-identical, so every leg
below was measurable on both).

Inputs — all measured this lane:

| Doc | Task | What it proved |
|---|---|---|
| `CEF-ALLOY-EXT.md` | CS06 | CEF#3859 maintainer claim verified **and exceeded**: alloy-style runs RequestContext-level extension functionality *and* renders `chrome-extension://` pages inside our own OSR view (BW popup pixel witness). Real gaps reduced to: tab-model enumeration, Chrome-UI surfaces (by design), then-believed nativeMessaging |
| `CEF-SOURCEBUILD.md` | CS07/CS11 | source build at pin real+runnable: libcef.so 2.8G + cefsimple + chrome_sandbox, ~30h wall under a 4G memcg, 30G disk; dcheck/PA-malloc config needs runtime flags (env artifact, not graft) |
| `CEF-GRAFT-ALLOY.md` + `spikes/cef/patches/` | CS08 | G1 alloy tab-model graft: 2 patches, all probe legs PASS on alloy-windowed **and** alloy-OSR incl. `tabs.query`/`windows.*`/`tabs.onUpdated`; BW popup rendered in patched alloy-OSR (pixel witness) |
| `CEF-GRAFT-NATIVEMSG.md` | CS12 | G2 = **zero-line graft**: nativeMessaging works unpatched on prebuilt+source × chrome+alloy, 11/11 legs incl. exact upstream error strings; prior "absent" was ungranted-permission API hiding |
| `CEF-GRAFT-AUTH.md` | CS09 | G3 = **zero-line graft**: `webRequest.onAuthRequired` answers unpatched on all 3 runtimes (blocking/async/cancel/wrong-cred re-challenge, server-verified); instrumented trace proves `cefCreateLoginDelegate→FwdToExt` nav path + proxied-URLLoader subresource path |
| `CEF-VERDICT.md` | CS05 | the baseline this doc updates: contract-fit GO, REJECT on value (~20–34wk adapter port, parity-not-differentiator, Wayland+nativeMessaging blockers) |

## 1. Per-graft result

| Graft | Feared scope | Actual | Patch lines | Result |
|---|---|---|---|---|
| G1 — extension tab model on Alloy (CS08) | "extension-page host", possibly deep delegate wiring | 2 patches: `0001` cef-side 587 ln (+507/−0, 5 files, ~480 ln = new `AlloyExtensionWindowController`/`AlloyBaseWindow` pair + registration + host wiring); `0002` chromium-side 532 ln (+345/−13, 7 files, `ENABLE_CEF` hunks in `tabs_api`, `tabs/windows_event_router`, `chrome_extension_function_details`, `tabs_windows_api`) | **1,119 patch-file lines; ~852 net new + 13 modified** | PASS on all legs, both alloy shapes — `tabs.query`, `windows.getAll/getCurrent/getLastFocused`, `tabs.onUpdated`, real `windowId`/`sender.tab`; baseline dead → fixed |
| G2 — nativeMessaging (CS12) | graft `extensions::NativeMessagingHost` + manifest discovery + stdio JSON host lifecycle | **0 lines.** Whole stack linked into libcef and wired on both runtimes incl. the shipped prebuilt | **0** | 11/11 legs PASS ×3 runtimes: echo, multi-msg, one-shot, host-exit `"Native host has exited."`, disconnect reaping (pid dead + clean EOF), fresh spawn per connect, absent host + `allowed_origins` mismatch exact upstream strings, user-level `NativeMessagingHosts` dir discovery |
| G3 — `webRequest.onAuthRequired` (CS09) | chrome/browser auth UI stack, possibly ungraftable | **0 lines.** `CreateLoginDelegate→ForwardToExtension→MaybeProxyAuthRequest→WebRequestEventRouter` chain intact; subresource path = proxied URLLoader | **0** | All iso legs PASS ×3 runtimes (fresh-profile real 401s, server-log-verified creds/cancels, lazy-SW dispatch); earlier combined-run "FAILs" = upstream preemptive auth-cache mask, identical on stock Chrome |

**Total carried patch surface for a fully-working MV3-on-all-shapes
CEF: one graft, ~852 net lines across 12 files (2 of them wholly
ours).** Both feared "deep" grafts evaporated — the runtime was more
complete than the docs/issues implied; the only real gap upstream left
was the alloy tab-model enumeration.

## 2. The deciding number: patch maintenance cost per Chromium bump

Diffstat (measured on the patch files, not estimated):

| Patch | Files | Patch-file lines | Net +/− | Where the risk lives |
|---|---|---|---|---|
| `0001` (cef repo) | 5 | 587 | +507 / −0 | ~480 of it is `alloy_window_controller.{h,cc}` — **our file in our directory**; rebase exposure is only the `BUILD.gn` line + 3 small `AlloyBrowserHostImpl` hunks |
| `0002` (chromium/src) | 7 | 532 | +345 / −13 | append-style `#if ENABLE_CEF` hunks; hot spots are `tabs_api.cc::BuildTabList` (upstream still churning) and the `extensions::WindowController`/`ui::BaseWindow` interfaces the new file implements |
| **total** | 12 | **1,119** | **+852 / −13** | |

Per-bump cost estimate:

- **Typical milestone bump** (CEF ships a new branch every ~4–8 wk
  Chromium cadence; an embedder rebases only per adopted pin, not per
  milestone): context-drift conflicts in 1–3 of the 7 chromium-side
  files; the hunks are guard-flagged and mechanically resolvable —
  **hours, not days**.
- **Worst case**: upstream refactors `WindowController`/`BaseWindow`
  or reworks `BuildTabList` → the ~480-line controller file needs
  matching rework — **bounded by a ~480-line rewrite, days at most**.
- **Per-pin rebuild cost dominates the patch cost**: our ~30h wall at
  −j1..5/4G memcg on this box is the real recurring bill (a real
  build host does it in a few hours; it's machine time either way,
  plus 30G disk per pin). The CS07 runtime-flag caveats
  (`UseBoringSSLForRandBytes`, stack-guard) are dcheck/non-official
  build artifacts, not patch artifacts — a production patched build
  would run `is_official_build`-style args and need neither.

Against the two comparison bills:

- **vs the 20–34wk adapter port** (CEF-VERDICT §3): the graft is
  noise. Consumer un-coupling (74 files/1176 refs) alone dwarfs it by
  two orders of magnitude; patch maintenance is a rounding error on
  the port estimate, not a new cost category.
- **vs the GECKO lane's ~119-patch/ESR bill** (its measured embedder
  patchset carried per ESR bump): patched-CEF is **~119 patches → 1
  patch, ~850 net lines, 2 hot files** for the same
  "make the engine a real extension host" goal — because Chromium
  ships the extension machinery in-tree and merely leaves it unwired
  on alloy, whereas Gecko makes the embedder carry everything. This is
  the single starkest asymmetry the lanes measured.

## 3. Did the two headline blockers actually die?

### nativeMessaging — **dead, completely, with zero patches**

CS12's correction: it was never absent — Chromium hides
permission-gated APIs until granted, and every earlier measurement ran
Bitwarden (which holds `nativeMessaging` in `optional_permissions`).
Verified unpatched on the *shipped prebuilt* and the source build, on
chrome **and** alloy runtimes, browser-process-side so OSR/headless
get it free. The residual is cosmetic: the optional-permission grant
arc reaches `ExtensionInstallPrompt` (modal pends under bare Xvfb) —
a Chrome-UI surface the embedder replaces, verified post-grant via
seeded prefs. **CS05's "CEF's extension story is not even strictly
superior" clause is now wrong in CEF's favor**: `connectNative`
works where QtWebEngine has no equivalent at all.

### Wayland — **mostly dead, conditional on carrying G1**

- Pre-graft: OSR/alloy had SW+storage+cs↔sw+extension-page rendering
  (CS06 — the popup-in-OSR fear was already unfounded) but no tab
  model → half an extension host.
- Post-G1 on alloy-OSR: `tabs.*`/`windows.*`/events PASS, `sender.tab`
  real, `scripting.executeScript(tabId)` reaches the tab — **every
  measured MV3 surface now works on the Wayland shape**.
- Still dead on Wayland (and everywhere on CEF, by CEF#3859 design):
  `chrome://extensions`, action-toolbar strip, `sidePanel` inset —
  all *browser-chrome* surfaces Arktos implements embedder-side
  anyway; the content half (popup page rendering) is verified.
- The honest qualifier: Wayland extension parity exists only on
  **patched** CEF — stock prebuilt keeps the empty-tab-model gap.
  Windowed reparent also remains X11-only; on Wayland the embed shape
  is OSR (which is exactly where the graft was verified).
- Unmeasured residue unchanged from CS05: real IBus/fcitx IME inside
  the embed, `identity`/`privacy` APIs (undeclared-permission absent),
  unanswered non-extension auth → views login prompt (embedder hook
  `CefRequestHandler::GetAuthCredentials` exists; no graft needed).

### Corrected #3859 understanding

The maintainer's claim ("RequestContext-level works; Chrome-UI dies
by design") was **conservative**: CS06 proved the
`chrome-extension://` page surface itself renders on alloy-OSR (a page
surface, not chrome-UI), and CS08 closed the one real functional gap
the claim didn't mention — tab-model enumeration — with a ~850-line
patch in upstream's own graft style. Post-G1, "what's dead on alloy"
is precisely and only the Chrome-UI list; everything else measured is
live. The residual `ExtensionInstallPrompt`/login-prompt modals are
the same category: dialogs the embedder owns.

## 4. Updated go/no-go vs CEF-VERDICT's reject

What this arc changed:

1. The extension-fidelity column went from "verified on chrome-style
   only, Wayland a partial-at-best unknown" to **verified full MV3 on
   every shape** given one ~850-line patch — and two of three feared
   grafts cost literally nothing.
2. `nativeMessaging` flipped from a listed absence to a verified
   capability QtWebEngine lacks — CEF's extension story *is* strictly
   superior now in a named dimension (native hosts, `webRequestAuthProvider`
   answering, full `tabs.*`/`windows.*` vs Qt's curated subset).
3. The fork-maintenance fear ("carrying patches = the Gecko bill") is
   measured false: 1 patch / ~850 net lines / 2 hot files vs ~119
   patches/ESR.

What it did **not** change:

- The ~20–34wk adapter port is untouched — it was never about grafts;
  74 files / 1176 must-port refs plus packaging legs still dominate.
- Extensions remain *parity-plus-a-bit*, not a must-have
  differentiator: backend-0 already ships a curated MV3 story;
  `connectNative` superiority matters only to extensions that use it
  (Bitwarden biometric/desktop unlock is the real example measured).
- Shipping weight (~150–300MB), the bwrap×sandbox fork, codec parity,
  and the IME unknown all stand from CS05.

**Explicit answer — three options:**

- **Stay on hosted runtime — chosen.** Today's decision is unchanged:
  do not adopt CEF at all. CS05's REJECT stands because the blocker was
  always value (port cost vs parity), never capability — and this arc
  only strengthened the capability side. The hosted prebuilt is
  additionally proven sufficient for everything except the alloy tab
  model.
- **Adopt patched CEF as third engine — conditionally endorsed, not
  triggered.** If a re-entry driver lands (CS05 §5 criteria — now with
  nativeMessaging as a named candidate requirement), the measured shape
  is: hosted pin + G1 only. The graft bill (~850 net lines, hours-per-bump
  typical, days worst-case) is affordable and small enough that "patched"
  should be the default assumption rather than a fork to fear — it is
  what buys Wayland extension parity. Third-engine remains the only
  defensible adoption shape (CEF-CAPS.md design holds).
- **Adopt as QtWebEngine replacement — rejected, stronger than
  before.** Same Chromium, minus Qt's integration, still negative delta
  on day one; the graft arc widened the capability gap in CEF's favor
  but did nothing to the ~20–34wk + feature-regression ledger that made
  replacement wrong.

Net: the lane's question "can CEF host real MV3 extensions embedded in
Qt" is answered **yes, everywhere, cheaply** — and the answer to
"should Arktos switch" is still **no**, for the same reason as before:
the price was never in the patches.
