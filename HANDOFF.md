# HANDOFF — duckdb-harbor

> **Purpose:** Current handoff only. Old PR-1 through PR-5-era tasks
> are removed because they are merged. Read this file after `AGENTS.md`
> and `SPEC.md` if resuming work mid-PR.

**Last updated:** 2026-05-15 22:30 MDT
**Last fully merged `main`:** `a03fbc9` — PR-12: rename duckdb-flock to duckdb-harbor (#12)
**Active branch:** `pr6-admin-handlers` (about to open as PR)
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
- **Admin endpoints (PR-6, in flight as `pr6-admin-handlers`)** — `/ready`,
  `/whoami`, `/tables`, `/schema/:db/:t`, `/checkpoint`, `/sessions`,
  `/interrupt`, `/sql/cancel`. `__HARBOR_ADMIN__:resource:action` authz
  with centralized default-deny and `harbor_allow_admin_without_authz`
  operator opt-in. 26-assertion HTTP golden test.

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

All merged PRs were green on every CI check at merge time. The current
CI matrix runs five build targets (Linux `linux_amd64`, MacOS
`osx_arm64`, Windows `windows_amd64`, Windows `windows_amd64_mingw`,
DuckDB-Wasm `wasm_mvp`) plus a matrix-generation step plus the
`architecture-guard` (single `duckdb_httplib::Server` owner) check
— seven total checks per push. `osx_amd64` and `linux_arm64` are
intentionally excluded by `reduced_ci_mode: enabled`; PR-7+ revisits
the full matrix.

## Active work: PR-6 (admin handlers)

Branch `pr6-admin-handlers`. Implementation, instrumentation, and tests
are all complete locally; ready to open the PR.

Routes added (registered before UiHandlers' `GET /.*` catch-all):

- `GET /ready` (public probe; runs `SELECT 1`)
- `GET /whoami` (authz `__HARBOR_ADMIN__:server:whoami`)
- `GET /tables` (authz `__HARBOR_ADMIN__:catalog:list_tables`)
- `GET /schema/:db/:table` (authz `__HARBOR_ADMIN__:catalog:describe_table`)
- `POST /checkpoint` (authz `__HARBOR_ADMIN__:checkpoint:create`)
- `GET /sessions` (authz `__HARBOR_ADMIN__:sessions:list`)
- `POST /interrupt` (authz `__HARBOR_ADMIN__:sessions:interrupt`)
- `POST /sql/cancel` (authz `__HARBOR_ADMIN__:sessions:cancel`; lives in `SqlHandlers`)
- OPTIONS preflight on each new mutating POST

Load-bearing invariants:

- `AuthManager::RunAuthorization` enforces centralized default-deny on
  any `__HARBOR_ADMIN__:` string when no custom authz fn is configured,
  unless `harbor_allow_admin_without_authz=true`. Detection by setting
  presence (not fn-name string compare) so aliased/qualified names
  cannot bypass.
- `/schema/:db/:table` uses `duckdb_columns()` with bound `Value`
  parameters — path components NEVER SQL-interpolated and NEVER in
  the `__HARBOR_ADMIN__:` authz string.
- Every mutating admin POST requires `Content-Type: application/json`,
  bounds the body at `harbor_max_request_body_bytes`, and (when
  cookie-authenticated) requires an `Origin` or `Referer` in
  `harbor_cors_origins`.
- `HarborSession` now carries `created_at`, `last_query`, and atomic
  `query_in_flight`. `SessionManager::Snapshot()` uses correct lock
  ordering (map → copy `shared_ptr`s → release → per-session brief).
- `SessionManager::InterruptSession()` calls `Connection::Interrupt()`
  without taking the per-session mutex (Interrupt is concurrency-safe;
  taking the mutex would deadlock against the in-flight Execute).
- Loud startup `WARN` log when `harbor_allow_admin_without_authz=true`
  is in effect with no custom authz fn.

Local verification:

```bash
make release
make test_release                         # 44/44 (was 43; +1 PR-6 setting check)
scripts/golden-cookie-auth.sh             # 14/14
scripts/golden-sql-roundtrip.sh           # 19/19 (after the PR-6 default-deny opt-in)
scripts/golden-admin-roundtrip.sh         # 26/26 (NEW — full admin HTTP coverage)
```

## Up next after PR-6: PR-7 (hardening)

Per `AGENTS.md` Implementation roadmap, after PR-6 merges the
remaining v0.1 work is hardening:

- Flip `reduced_ci_mode: 'enabled'` off; add `osx_amd64` + `linux_arm64`
  to the matrix (currently 5 platforms; target is the full 7).
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
2. `git status -sb` — if PR-6 is still in flight, should show
   `pr6-admin-handlers` (12+ modified files); if merged, `main` clean
   at the PR-6 merge commit.
3. If PR-6 merged and you are starting PR-7, branch off latest `main`:
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
   scripts/golden-admin-roundtrip.sh  # 26/26
   ```
5. Confirm all green before starting new work.
