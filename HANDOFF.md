# HANDOFF — duckdb-harbor

> **Purpose:** Current handoff only. Old PR-1 through PR-5-era tasks
> are removed because they are merged. Read this file after `AGENTS.md`
> and `SPEC.md` if resuming work mid-PR.

**Last updated:** 2026-05-16 13:30 MDT
**Last fully merged `main`:** `9a29edf` — PR-7c: auth scheme tightening + login-page CSP+nonce (#17)
**Active branch:** none yet — PR-7d (full nested-type Mode B param parser for `/sql`) is the next planned PR; branch `pr7d-nested-mode-b` will be created off `main` when work begins.
**Project repo:** `/Users/shreeve/Data/Code/duckdb-harbor` · GitHub `shreeve/duckdb-harbor`
**GPT-5.5 conversation id:** `duckdb-flock-spec` (kept from before the rename — references the project as Harbor going forward)

## TL;DR

harbor is a DuckDB extension that turns one DuckDB process into a
multi-protocol HTTP service on one port:

- Quack RPC (`POST /quack`) — merged and roundtrip-tested.
- DuckDB UI (`/ddb/*`, `/localEvents`, `/localToken`, `GET /.*`) —
  merged, cookie-gated, credential-strip tested.
- JSON SQL (`POST /sql`) — merged with NDJSON streaming, principal-owned
  sessions, golden-tested.
- Admin endpoints (`/ready`, `/whoami`, `/tables`, `/schema/:db/:t`,
  `/checkpoint`, `/sessions`, `/interrupt`, `/sql/cancel`) — merged
  with centralized `__HARBOR_ADMIN__:` default-deny and
  `harbor_allow_admin_without_authz` operator opt-in.

Architecture is `httplib + OpenSSL` for v0.1. The PR-10b migration to
mbedTLS/HTTPUtil/plain-httplib was evaluated and declined; do not
restart it unless the trigger conditions in `AGENTS.md` are met.

## Merged state on `main`

Latest merged chain:

| PR | Status | Notes |
|---|---|---|
| PR-1 | merged | Vendored `duckdb-quack` as `src/quack/`; extension build name `harbor`. |
| PR-1.5 | merged | `/quack` runtime roundtrip in `test/sql/harbor.test`; current regression baseline. |
| PR-2 | merged | Shared `HarborHttpServer`, `SessionManager`, `AuthManager`, `harbor_serve`/`stop`/`wait`, `/health`, `/info`. |
| PR-3 | merged | Vendored `duckdb-ui`; `UiHandlers`; `duckdb_httplib_openssl::` namespace; UI proxy mode. |
| PR-4 | merged | `harbor_crypto`, cookie auth, `/auth/login`, `/auth/logout`, cookie-gated UI and `/ddb/*`, CORS allow-list. |
| PR-7 | merged | Corrected misleading CSPRNG comments (`GetEncryptionUtil` is OpenSSL-via-httpfs). |
| PR-8 | merged | Security fix: UI proxy strips `Cookie`, `Authorization`, `X-Harbor-*`, etc. before forwarding upstream. |
| PR-10a | merged | Docs: v0.1 UI assets = proxy/disabled; bundled deferred to v0.2. |
| PR-11 | merged | Docs: cancelled PR-10b migration; strengthened why `curl` is required. |
| PR-5 | merged | JSON `/sql` endpoint, NDJSON streaming, principal-owned sessions, golden tests. |
| PR-12 | merged | Pre-v0.1 project rename: `duckdb-flock` → `duckdb-harbor`. Build identity, source tree, SQL surface, HTTP cookie/headers, env vars, scripts, and docs all moved to `harbor`. Quack wire compat preserved. |
| PR-6 | merged | Admin handlers (`/ready`, `/whoami`, `/tables`, `/schema/:db/:t`, `/checkpoint`, `/sessions`, `/interrupt`, `/sql/cancel`). Centralized `__HARBOR_ADMIN__:` default-deny in `AuthManager::RunAuthorization` (detected by setting presence — robust against aliased fn names) with `harbor_allow_admin_without_authz` operator opt-in. `HarborSession` instrumented (`created_at`/`last_query`/`query_in_flight`); `SessionManager::Snapshot()` + `InterruptSession()`. CSRF + `Content-Type: application/json` + body-limit on every mutating admin POST. `/schema` uses `duckdb_columns()` with bound parameters — path identifiers never SQL-interpolated. New `golden-admin-roundtrip.sh` (26 assertions across three lifecycles: default-deny, admin-bypass, custom authz fn). |
| PR-6.1 (#14) | merged | Post-merge security + correctness follow-up surfaced by GPT-5.5 round 19 and signed off in round 20. Fixed: (a) **default-deny fail-open** when operator explicitly set `harbor_authorization_function` / `quack_authorization_function` to a built-in nop name (security); the `IsBuiltinNopAuthz` normalizer now lower-cases, strips whitespace, and strips a leading schema-qualifier prefix before comparison. (b) **RNG TOCTOU** in `SessionManager::GenerateSessionId` (correctness; lock now held across init + `GenerateRandomData`). (c) **`/ready` info leak** — bare `{"ok":false}` 503 with no DuckDB error detail. (d) Tighter `Content-Type: application/json` parser (rejects `application/jsonjunk`; only `;` is the standard MIME parameter separator). (e) `/checkpoint` body validation now fires on chunked transfer encoding too. Golden coverage extended (26 → 31 assertions) with regression guards for explicit-nop, mixed-case-nop, schema-qualified-nop, and the tighter Content-Type. |
| PR-7a (#15) | merged | Flipped `reduced_ci_mode: 'enabled'` → `'disabled'` so every PR runs against the full upstream non-opt-in matrix BEFORE merging. Matrix went from 5 to 9 build targets per push: Linux (amd64 + arm64), MacOS (amd64 + arm64), Windows (amd64 + amd64_mingw), Wasm (mvp + eh + threads). All four new platforms (`linux_arm64`, `osx_amd64`, `wasm_eh`, `wasm_threads`) passed on first try with zero source changes. First of the PR-7-series focused hardening PRs (split from the original "PR-7 omnibus" so each concern is reviewable independently). |
| PR-7c (#17) | merged | Auth scheme tightening + login-page CSP+nonce. New `UNSUPPORTED_AUTH_SCHEME` errorCode for non-Bearer `Authorization` headers (clearer than the previous `MISSING_CREDENTIAL` for misconfigured reverse proxies that strip/inject `Basic`); applied in both `AuthManager::AuthenticateRequest` and `AuthHandlers::HandleLogin` (both reject without falling through to ambient state per the round-11 invariant). Login page now serves with `Content-Security-Policy: default-src 'none'; script-src 'nonce-<csprng>'; style-src 'unsafe-inline'; …` and a per-request `<script nonce="...">` attribute (16 random bytes, standard base64 per round-23). `RandomBytes` failure returns 500 (no fallback to CSP-less page). golden-cookie-auth.sh: 14 → 18 assertions. |
| PR-7b (#16) | merged | `harbor_query_timeout_s` runtime enforcement. Sweeper thread (250ms tick, generation-versioned race-fix) interrupts overdue SessionManager-tracked queries; per-request RAII `QueryTimeoutWatchdog` (clean cv-driven join, never detached) handles ephemeral `/sql` + transient admin/UI connections. `InterruptCause` enum classifies TIMEOUT/USER_CANCEL/DISCONNECT so the catch path emits HTTP 504 + `errorCode: "QUERY_TIMEOUT"` (pre-response) or mid-stream `{"type":"error","code":"QUERY_TIMEOUT"\|"QUERY_CANCELLED"}` (NDJSON streaming, status frozen per SPEC §5.2). Wired in 5 call sites: SqlHandlers (sessionful + ephemeral + streaming guard transfer), QuackHandlers PREPARE/APPEND/FETCH, AdminHandlers /tables/schema/checkpoint, UiHandlers /ddb/run. R-21 design review caught generation counter race-fix; R-22 post-impl caught streaming-USER_CANCEL fake-natural-end bug; PR-7a's full 9-platform CI caught a Windows MSVC most-vexing-parse before merge. New `golden-query-timeout.sh` (10 assertions across 3 server lifecycles incl. generation-race regression guard). |

All merged PRs were green on every CI check at merge time. With
PR-7a's flip of `reduced_ci_mode: 'enabled'` → `'disabled'`, the
matrix now runs nine build targets per push:

- Linux: `linux_amd64`, `linux_arm64`
- MacOS: `osx_amd64`, `osx_arm64`
- Windows: `windows_amd64`, `windows_amd64_mingw`
- Wasm: `wasm_mvp`, `wasm_eh`, `wasm_threads`

Plus the matrix-generation step plus the `architecture-guard`
(single `duckdb_httplib::Server` owner) check — eleven total CI
rows per push. The `*_musl` and `windows_arm64` targets remain
opt-in only.

## Up next: PR-7-series (hardening, split into focused PRs)

The original "PR-7 omnibus" was split into focused PRs so each concern
is reviewable independently. PR-7a (CI matrix flip) is merged; the
remaining items in dependency order:

- ~~PR-7a: flip `reduced_ci_mode: 'enabled'` off~~ — **merged** as `96192e0`.
- ~~PR-7b: `harbor_query_timeout_s` runtime enforcement~~ — **merged** as `3195c8e`.
- ~~PR-7c: auth scheme tightening + login-page CSP+nonce~~ — **merged** as `9a29edf`.
- **PR-7d**: Full nested-type Mode B param parser for `/sql`
  (`LIST<...>`, `STRUCT(...)`, `MAP<...>`).
- **PR-7e**: More golden tests — per-DuckDB-type `/sql` round-trip
  in `test/types/`; byte-level Quack/UI fixtures in `test/golden/`.
- **PR-7f**: Community-extensions repo submission (last; depends on
  everything else green).

Each gets its own GPT-5.5 design review (R-N pre-coding) → impl →
post-impl review (R-N+1) → sign-off (R-N+2) — matching the pattern
that worked for PR-5 and PR-6.

## Design decisions / caveats to preserve

- Keep `httplib + OpenSSL` architecture. Do not restart PR-10b.
- Keep `vcpkg.json` as `["openssl", "curl"]`; both are required by
  httpfs sibling build and runtime.
- Do not touch `src/quack/quack_message.{cpp,hpp}`.
- Do not edit `misc/`.
- Do not change `/quack` wire format. `test/sql/harbor.test` roundtrip
  is the regression baseline.
- For streaming `/sql`, always buffer an entire NDJSON line or chunk
  before `sink.write()`.
- Hold the session mutex for the full streaming lifetime.
- Keep `ActiveRequestGuard` alive for the full streaming provider
  lifetime, not just the route lambda.

## If resuming after a reconnect

1. `cd /Users/shreeve/Data/Code/duckdb-harbor`
2. `git status -sb` — should show `main` clean and at `9a29edf` or later.
3. Branch off `main` for the next PR:
   ```bash
   git switch main
   git pull --ff-only
   git switch -c pr7d-nested-mode-b
   ```
4. Run a fresh sanity build:
   ```bash
   make release
   make test_release                  # 45/45
   scripts/golden-cookie-auth.sh      # 18/18
   scripts/golden-sql-roundtrip.sh    # 19/19
   scripts/golden-admin-roundtrip.sh  # 31/31
   scripts/golden-query-timeout.sh    # 10/10
   ```
5. Confirm all green before starting new work.
