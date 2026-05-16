# HANDOFF — duckdb-harbor

> **Purpose:** Current handoff only. Old PR-1 through PR-5-era tasks
> are removed because they are merged. Read this file after `AGENTS.md`
> and `SPEC.md` if resuming work mid-PR.

**Last updated:** 2026-05-15 21:55 MDT
**Last fully merged `main`:** `a03fbc9` — PR-12: rename duckdb-flock to duckdb-harbor (#12)
**Active branch:** none yet — PR-6 (admin handlers) is the next planned PR; branch `pr6-admin-handlers` will be created off `main` when work begins.
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

## Up next: PR-6 (admin handlers)

Per `AGENTS.md` Implementation roadmap, the next functional PR after
the rename is PR-6 — admin handlers per SPEC §4:

- `GET /whoami`
- `GET /tables`
- `GET /schema/:db/:table`
- `POST /checkpoint`
- `GET /sessions`
- `POST /sessions/:sid/interrupt`
- `POST /sql/cancel` (deferred from PR-5; needs admin authz)

All routed through `harbor_authorization_function` with synthetic
`__HARBOR_ADMIN__:<resource>:<action>` query strings (default-deny when
no custom authz function is configured, unless
`harbor_allow_admin_without_authz=true`).

Path parameters MUST be identifier-escaped via
`KeywordHelper::WriteQuoted(name, '"')` — never string-interpolated
into SQL or into the `__HARBOR_ADMIN__:` policy string.

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
2. `git status -sb` — should show `main` clean and at `a03fbc9` or later.
3. Branch off `main` for the next PR:
   ```bash
   git switch main
   git pull --ff-only
   git switch -c pr6-admin-handlers
   ```
4. Run a fresh sanity build:
   ```bash
   make release
   make test_release
   scripts/golden-cookie-auth.sh
   scripts/golden-sql-roundtrip.sh
   ```
5. Confirm all green before starting new work.
