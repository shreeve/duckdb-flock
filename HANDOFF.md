# HANDOFF — duckdb-harbor

> **Purpose:** Current handoff only. Old PR-1 through PR-5-era tasks
> are removed because they are merged. Read this file after `AGENTS.md`
> and `SPEC.md` if resuming work mid-PR.

**Last updated:** 2026-05-15 17:30 MDT
**Last fully merged `main`:** `4203d73` — PR-5: add JSON `/sql` endpoint with NDJSON streaming (#11)
**Active branch:** `pr12-rename-harbor`
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

All merged PRs were green on the 7-target CI matrix at merge time.

## Active work: PR-12 (project rename)

### Scope

Pre-v0.1 project rename: `duckdb-flock` → `duckdb-harbor`. Mechanical
rename across build identifiers, source tree, SQL surface, HTTP
cookie/headers, env vars, scripts, and docs. **No runtime behavior
change beyond the intentional public rename.**

Quack compatibility intentionally preserved:

- `/quack` route, wire format, and `X-Quack-Protocol-Version` header.
- `quack:` URI scheme accepted alongside the new canonical `harbor:`.
- `quack_*` SQL functions and settings retained as compatibility
  aliases; `harbor_*` is now the primary surface.
- `src/quack/` source tree, `QuackMessage`, `QuackHandlers`,
  `QuackLogType` all unchanged.
- Storage extension registered under both `"harbor"` and `"quack"`
  type keys; canonical listener identity stays `quack:host:port` so
  start/stop calls match across schemes.

Auth resolution order: Harbor settings → Quack settings (compat) →
built-in default.

### Local verification

```bash
make release            # artifact: build/release/extension/harbor/harbor.duckdb_extension
make test_release       # 43/43 assertions
scripts/golden-cookie-auth.sh    # 14/14, including PR-8 credential strip on X-Harbor-* / harbor_session
scripts/golden-sql-roundtrip.sh  # all assertions
```

Smoke tests confirm:

- `LOAD harbor; SELECT harbor_version();` works.
- `harbor_check_token`, `harbor_nop_authorization`, `harbor_uri_parser`
  registered.
- `harbor_serve('harbor:127.0.0.1:N')` followed by
  `harbor_stop('quack:127.0.0.1:N')` works (canonical-id matching
  across schemes).
- `quack_serve('quack:...')`, `quack_query(...)`, `quack_stop('harbor:...')`
  all work (full Quack-side compat).
- `ATTACH 'harbor:127.0.0.1:N' AS h (TYPE harbor, TOKEN '...');
   SELECT * FROM h.query('SELECT 99 AS x')` works.

Repo-wide grep on tracked files (excluding `duckdb/`,
`extension-ci-tools/`, `misc/`, `build/`) for `flock|Flock|FLOCK|
flock_session|X-Flock|__FLOCK|flock:|duckdb-flock`: zero hits.

### Commit / PR workflow

Single squash-friendly commit, branch off `main`, full 7-target CI
matrix runs against the rename before merge.

```bash
git switch -c pr12-rename-harbor
git add -A
git commit -m "PR-12: rename duckdb-flock to duckdb-harbor"
git push -u origin pr12-rename-harbor
gh pr create ...
```

Wait for all 7 CI checks + `architecture-guard` to go green, then
squash-merge.

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
2. `git status -sb`
3. Confirm branch — `pr12-rename-harbor` if rename PR is still in flight,
   `main` after merge.
4. If rename PR is merged and you are starting PR-6, branch off latest
   `main`:
   ```bash
   git switch main
   git pull --ff-only
   git switch -c pr6-admin-handlers
   ```
5. Run:
   ```bash
   make release
   make test_release
   scripts/golden-cookie-auth.sh
   scripts/golden-sql-roundtrip.sh
   ```
6. Confirm all green before starting new work.
