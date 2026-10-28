# Arktos Sync — design document

Serverless, device-to-device sync. There is **no server**: every
device keeps a signed op-log; peers exchange ops over authenticated
links and converge locally. Everything below describes
`src/rustcore/src/sync*.rs` (engine + adapters + transports) and mw
domain 19 (`src/rustcore/src/mw/mod.rs`) as implemented, including
the honest limitations at the end.

## Identity, membership, op-log

- Each device has an Ed25519 identity in `<data root>/sync/device.key`
  (0600, generated once, **never synced**). `device_id` = hex(pubkey).
- `sync.db` (SQLite) holds the member table, per-scope group secrets,
  and the op-log. Members are bound to **scopes** — a scope is a
  profile id (`"default"`, `profiles/<id>`); a scoped member signs
  only that profile's ops.
- Wire op (JSON `v:1`):
  `{op_id, store, scope, device_id, lamport, wall_ms, body, sig_b64}`.
  `op_id` = hex-SHA-256 over the signed content frame, so dedup is
  content-addressed and transport-agnostic. The Ed25519 signature is
  verified against the signer's member pubkey on import.
- Apply order is **canonical, not arrival order**: every accepted
  batch is sorted by `(lamport, device_id, op_id)` before appliers
  run. Import is per-op: a bad op is rejected with a reason and never
  aborts the batch. Ops for stores with no registered applier stay
  durable in the log flagged `applied=0` and retry via `drain`.
- Synced stores: `bookmarks`, `history`, `tabs`, `settings`,
  `passwords`, `syncmeta` (group-management ops — key rotation and
  revocation notices; engine state, never user data).

## Exchange model

There is deliberately **no background scheduler** in the engine. The
exchange is the domain-19 ops themselves:

```
sync.export {store?, since_seq}        -> [WireOp docs]
transport_send {link_id, payload_b64}  -> one frame
peer: transport_poll                   -> Frame events
peer: sync.import {ops:[...]}          -> verify/dedup/apply
peer: sync.drain {store}               -> retry deferred ops
```

The Qt shell and the integration tests ship `{"ops":[...]}` docs as
opaque frames. `since_seq` is the per-log cursor; a stateless client
can always export `0` — dedup makes resends free.

## Transport tiers

All transports implement one `SyncTransport` seam
(`synctransport.rs`): `endpoint / authorize / deauthorize / connect /
send / close_link / poll`. Inbound activity lands as pull-queue
events (`Ready, Bootstrap, Incoming, Frame, LinkClosed, Failed`),
announced via `notify::emit("synctransport")`. Max frame 16 MiB.

| backend | endpoint | admission gate |
|---------|----------|----------------|
| `tor` (feature `synctor`, default) | `<56ch>.onion:14430` | v3 onion service per device, hsid in the Arti keystore (stable across restarts). Restricted discovery: the descriptor is x25519-encrypted; unpaired peers cannot decrypt it and are refused **at the Tor layer, before one app byte**. Outbound connects retry/backoff until a deadline; auth-refusal is terminal. |
| `lan` (feature `synclan`, default) | `lan://<device-id>` / `lan://host:port` | mDNS (`_arktos-sync._tcp.local`, hand-rolled codec) discovers co-located paired devices; TXT `dev=`/`on=` advertise id + onion. QUIC (quinn) carries TLS 1.3 with a self-signed Ed25519 device cert bound to the sync key; the handshake gates on the `member-<id16>` grant set **plus** a full-key pin against `sync.db` members — stricter than tor's grant-only check, refusal is pre-app-byte. |
| `auto` (needs both) | composite | Onion endpoints resolve through LAN discovery first, Tor fallback when not co-located; per-leg degradation; grants fan out to both legs. |
| `mem` | `mem:<name>` | In-process loopback — the hermetic parity contract the real backends are tested against (same authorize/connect/frame/close semantics, same pre-app-byte refusals). |

## Pairing flow (SYNC03)

1. `pair_begin {profile?}` mints a one-time invite: endpoint + 256-bit
   token + 8-byte hash of the inviter's device key, rendered as QR
   text or a grouped-base32 manual code.
2. `pair_join {payload}` parses either rendering, derives a one-time
   restricted-discovery key from the token, and connects — the
   `pair-<tok16>` grant works exactly once.
3. Noise_XX with `token` as prologue; both statics are bound to the
   Ed25519 device identities by signature. The transcript hash becomes
   the **6-word / 6-emoji fingerprint** both users compare
   out-of-band — a relayed MITM yields identical fingerprints, a
   terminated one never does.
4. Only after **both** sides `pair_confirm ok:true` does the
   profile's group secret + permanent discovery keys cross the sealed
   channel. Each side records the peer in `member_scopes` bound to
   that profile, swaps the `pair-*` grant for `member-<id16>`.
5. `pair_poll` drives the state machine — transport events are
   drained per call, no background thread owns the ceremony.

## CRDT semantics per store

- **bookmarks** (`syncbookmarks.rs`): OR-set of node guids (XBEL `id`
  attr; random at create, deterministic content fingerprint for
  foreign-loaded nodes so pre-sync trees dedup). `link` ops add+move
  by (lamport, device) LWW with fractional-rank `pos` sibling order;
  `set` ops are per-field LWW; deletes are tombstones recording the
  remover's observed place stamps — un-observed placements survive.
  Causal parking holds ops for not-yet-known parents; move cycles
  resolve deterministically. The op-log is the adapter's only durable
  state — tombstones/stamps rebuild by replay.
- **history** (`synchistory.rs`): append-only union set, one element
  per visit keyed by a deterministic visit-id over (url, visited_at)
  — identical pre-sync visits on two devices name the same element
  and merge instead of doubling. Title LWW by (lamport, device);
  empty remote titles never erase. **Deletes and retention are
  device-local by contract** — remove/clear/expire never emit.
  `expire_older_than` records the widest cutoff locally and the
  applier re-prunes after every merge, so a peer's longer history
  cannot resurrect expired rows.
- **tabs** (`synctabs.rs`): *ephemeral* per-device presence doc —
  whole-doc LWW by (lamport, device); republish replaces outright, so
  closes propagate by absence; empty = wipe. Applied to an in-memory
  view only: peer tabs are a read-only view, **never** history rows.
- **settings** (`syncsettings.rs`): per-key LWW map (`set`/`del`
  ops carrying typed-value docs). Exclusion prefixes gate emit *and*
  apply — built-ins are raw `starts_with` prefixes: `sync`,
  `sessions`, `BrowserMainWindow`, `proxy`, `downloadmanager/*`
  (so `sync*` keys are device-local; the `sync/localExclude`
  user list itself sits under `sync` and can never sync into
  changing itself).
- **passwords** (`syncpasswords.rs`): strictly opt-in, E2E
  encrypted. `passwords_enable {passphrase}` derives the vault key
  as Argon2id(passphrase, salt bound to the group secret) — the
  group secret comes from pairing, so keys never ride ops. Wire ops
  carry only `eid` + sealed blobs (`pwv` verifier, `set`/`del`).
  `eid` = hex-SHA-256(url) — deterministic, so identical pre-sync
  logins merge. Field-level LWW on the op's (lamport, device) stamp;
  `del` tombstones the entry. A locked vault defers apply — the op
  stays durable and drains on the next enable.
- **syncmeta** (`syncmeta.rs`): group-management ops — `{t:"rekey",
  gen, revoked:[…], wraps:[…]}`. Wraps are ephemeral-x25519→device-key
  seals of the rotated group secret, one per surviving member.

## Revocation (SYNC09)

`device_revoke {profile, device_id}` on one call: bumps the group
secret generation, emits a signed `rekey` op (revoked list + wraps
for survivors), removes the member (or just that scope's binding for
multi-scope members), deauthorizes the `member-<id16>` grant, drops
the peer's presence doc, and re-wraps the vault.

On apply the revoked list lands **unconditionally** (idempotent —
concurrent revocations by different devices all must land). A device
finding *itself* on the list tears the pair down symmetrically:
drops the signer's member row and grant. The secret installs only
past the stored `RotationMark` (gen, lamport, device, op) — the total
order concurrent rotations resolve by.

Post-revoke: fresh dials are refused at the transport gate, and any
op signed by the removed device is rejected at import ("unknown
device — not a sync member") — by signature, never silently dropped.

## Verification map

- Per-adapter envelope legs (mw tests): `sync_bookmarks|history|
  settings|passwords|tabs_presence_adapter_end_to_end`,
  `sync_device_management_end_to_end`.
- Transport parity legs: `sync_transport_mem_end_to_end`,
  `sync_transport_lan_end_to_end`, `tools/synctor-e2e.sh` (live Tor:
  endpoint stability, authorized ping, unauthorized refusal,
  restart stability).
- SYNC11 full-stack legs: `sync_full_stack_over_mem_circuits`
  (real pairing + all five stores riding transport frames both
  directions + wire-ciphertext scan), `sync_conflicts_converge_over_
  mem` (offline edits on both sides converging to the computed
  (lamport, device) winner; identical-visit dedup),
  `sync_revocation_cuts_peer_over_mem` (mid-session cut,
  symmetric teardown, gate refusal, signature rejection),
  `sync_ops_over_lan_circuit` (real mDNS + QUIC member-cert link
  carrying real ops; honest skip without a multicast iface),
  `sync_full_stack_over_tor_live` (`#[ignore]` — the same path over
  real Tor; run explicitly with `-- --ignored`).

## Recorded limitations (honest)

- **Two passphrases in one group fork the vault**: a device that
  enables with a different passphrase holds ops it cannot read —
  divergence by design (E2E means we cannot force uniformity).
- **`eid` is brute-forceable**: hex-SHA-256(url) means a group peer
  can dictionary-probe which sites have saved logins; history
  visit-ids are likewise correlatable for known (url, ts).
- **Re-pair after revoke hits `group_secret_install` conflicts** —
  the loud path, not silent acceptance.
- **Group-secret rotation re-bases the vault salt**: the re-wrap
  must re-seal password fields under the re-derived key; only
  own-authored fields are re-emitted (re-emitting peer fields would
  manufacture LWW recency).
- **Revoking a live link does not kill it**: the grant is gone so
  *new* links are refused, but the open link keeps carrying frames
  until closed — the op signature gate is what actually rejects the
  ex-member's ops (same caveat restricted discovery documents).
- **History deletes are device-local**: a peer's `clear history`
  never touches this device's copy — intentional, but surprising to
  users expecting remote wipe.
- **Presence is memory-only**: a restarted device shows no peer tabs
  until the peers republish.
- **LAN pin caveat**: LAN admission needs the member row's full key,
  so `pair-*` nicks are not honored — pairing always rides tor.
- **No clock discipline**: `lamport` is the ordering truth;
  `wall_ms` is informational — a wrong-device clock does not poison
  merge order but skews "last synced" displays.
- **No automatic scheduler**: the engine exposes export/import and
  the transport exposes links; *when* to sync is the caller's policy
  (Qt shell drives rounds on change/interval today).
