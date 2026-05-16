# HANDOFF — duckdb-harbor

> **Purpose:** Current handoff only. Old PR-1 through PR-5-era tasks
> are removed because they are merged. Read this file after `AGENTS.md`
> and `SPEC.md` if resuming work mid-PR.

**Last updated:** 2026-05-16 00:30 MDT
**Last fully merged `main`:** `433692e` — PR-6 follow-up: post-merge review (rounds 19+20) — security + correctness fixes (#14)
**Active branch:** `pr7a-full-ci-matrix` (CI-matrix flip; first of the PR-7-series hardening PRs).
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

## Up next: PR-7 (hardening)

Per `AGENTS.md` Implementation roadmap, the remaining v0.1 work is
hardening:

- ~~Flip `reduced_ci_mode: 'enabled'` off~~ — landing in PR-7a
  (active branch `pr7a-full-ci-matrix`).
- `harbor_query_timeout_s` runtime enforcement (setting is in SPEC,
  but the executor-side interrupt-after-N-seconds wiring is PR-7).
- Default-deny on unknown `Authorization:` schemes (today
  `Authorization: Basic …` falls through to cookie/X-Harbor-Token; should
  explicitly reject anything that isn't `Bearer`).
- Login-page CSP + nonce.
- Full nested-type param parser for `/sql` Mode B wrappers
  (`LIST<...>`, `STRUCT(...)`, etc.).
- More golden tests: per-DuckDB-type `/sql` round-trip in
  `test/types/`; full byte-level Quack/UI fixtures in `test/golden/`.
- Distribution: DuckDB community-extensions repo submission.

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
2. `git status -sb` — should show `main` clean and at `433692e` or later.
3. Branch off `main` for the next PR:
   ```bash
   git switch main
   git pull --ff-only
   git switch -c pr7-hardening
   ```
4. Run a fresh sanity build:
   ```bash
   make release
   make test_release                  # 44/44
   scripts/golden-cookie-auth.sh      # 14/14
   scripts/golden-sql-roundtrip.sh    # 19/19
   scripts/golden-admin-roundtrip.sh  # 31/31
   ```
5. Confirm all green before starting new work.
