# HANDOFF — duckdb-harbor

> **Purpose:** Current handoff. Read this file after `AGENTS.md`
> and `SPEC.md` if resuming work mid-PR.

**Last updated:** 2026-05-16 18:00 MDT
**Last fully merged `main`:** PR-7e (#19) — per-DuckDB-type /sql encoding round-trip + TIMESTAMP_S/MS encoder fix
**Active branch:** `pr7f-v01-readiness` — final v0.1 docs polish; community-extensions submission is an external PR in `duckdb/community-extensions`.
**Project repo:** `/Users/shreeve/Data/Code/duckdb-harbor` · GitHub `shreeve/duckdb-harbor`
**GPT-5.5 conversation id:** `duckdb-flock-spec` (kept from before the rename — references the project as Harbor going forward)

## v0.1 status: IMPLEMENTATION COMPLETE

All v0.1 work is merged on `main`. Final test totals on the latest `main`:

| Suite | Assertions |
|---|---|
| `test/sql/harbor.test` (sqllogictest) | 45 |
| `scripts/golden-cookie-auth.sh` | 18 |
| `scripts/golden-sql-roundtrip.sh` | 32 |
| `scripts/golden-admin-roundtrip.sh` | 31 |
| `scripts/golden-query-timeout.sh` | 10 |
| `scripts/golden-sql-types.sh` | 62 |
| **Total** | **198** |

CI matrix: 9 build targets per push (linux_amd64, linux_arm64,
osx_amd64, osx_arm64, windows_amd64, windows_amd64_mingw, wasm_mvp,
wasm_eh, wasm_threads) + matrix-generation step + architecture-guard.

## TL;DR

harbor is a DuckDB extension that turns one DuckDB process into a
multi-protocol HTTP service on one port:

- Quack RPC (`POST /quack`) — wire-compat with stock Quack clients.
- DuckDB UI (`/ddb/*`, `/localEvents`, `/localToken`, `GET /.*`) —
  cookie-gated, credential-strip enforced.
- JSON SQL (`POST /sql`) — NDJSON streaming, principal-owned sessions,
  full nested-type Mode B param parser, query timeout enforcement.
- Admin endpoints (`/ready`, `/whoami`, `/tables`, `/schema/:db/:t`,
  `/checkpoint`, `/sessions`, `/interrupt`, `/sql/cancel`) — centralized
  `__HARBOR_ADMIN__:` default-deny + `harbor_allow_admin_without_authz`
  operator opt-in.

Architecture is `httplib + OpenSSL`. The PR-10b migration to
mbedTLS/HTTPUtil/plain-httplib was evaluated and declined for v0.1; do
not restart it unless the trigger conditions in `AGENTS.md` are met.

## Merged state on `main`

| PR | Status | Notes |
|---|---|---|
| PR-1 | merged | Vendored `duckdb-quack` as `src/quack/`; extension build name `harbor`. |
| PR-1.5 | merged | `/quack` runtime roundtrip in `test/sql/harbor.test`; current regression baseline. |
| PR-2 | merged | Shared `HarborHttpServer`, `SessionManager`, `AuthManager`, `harbor_serve`/`stop`/`wait`, `/health`, `/info`. |
| PR-3 | merged | Vendored `duckdb-ui`; `UiHandlers`; `duckdb_httplib_openssl::` namespace; UI proxy mode. |
| PR-4 | merged | `harbor_crypto`, cookie auth, `/auth/login`, `/auth/logout`, cookie-gated UI and `/ddb/*`, CORS allow-list. |
| PR-5 | merged | JSON `/sql` endpoint, NDJSON streaming, principal-owned sessions, golden tests. |
| PR-6 / PR-6.1 | merged | Admin handlers + centralized `__HARBOR_ADMIN__:` default-deny + post-merge security follow-up (`IsBuiltinNopAuthz`, RNG TOCTOU fix, `/ready` info-leak fix, tighter Content-Type validation). |
| PR-7 / PR-8 | merged | OpenSSL CSPRNG comment cleanup; UI proxy credential-strip. |
| PR-7a (#15) | merged | Flipped `reduced_ci_mode: 'enabled'` → `'disabled'` — every PR runs the full 9-target matrix before merging. |
| PR-7b (#16) | merged | `harbor_query_timeout_s` runtime enforcement. Sweeper thread (250ms tick, generation-versioned race-fix), per-request `QueryTimeoutWatchdog` for ephemeral connections, `InterruptCause` enum. Wired into 5 call sites. |
| PR-7c (#17) | merged | Auth scheme tightening + login-page CSP+nonce. New `UNSUPPORTED_AUTH_SCHEME` errorCode for non-Bearer `Authorization` headers. Login page now serves with `Content-Security-Policy: default-src 'none'; script-src 'nonce-<csprng>'; …` (16 random bytes, standard base64 per round-23). `RandomBytes` failure returns 500 (no fallback to CSP-less page). |
| PR-7d (#18) | merged | Full nested-type Mode B param parser for `/sql`: `LIST<T>`, `ARRAY<T,N>`, `MAP<K,V>`, `STRUCT(name1 type1, ...)`. STRUCT decoding hardened: case-insensitive lookup, duplicate-key rejection, missing-field-as-NULL, extra-key rejection. MAP value shape locked to array-of-pairs. Whitespace-tolerant type strings. Recursion depth cap 32; type string length cap 4 KiB. |
| PR-7e (#19) | merged | Per-DuckDB-type `/sql` encoding round-trip — new `golden-sql-types.sh` (62 assertions) covering all scalar families, integer boundaries, DOUBLE non-finite, DECIMAL, VARCHAR (Unicode + emoji + escapes), date/time families, INTERVAL, BLOB, BIT, JSON, LIST/ARRAY/STRUCT/MAP, ENUM (UDT). Found and fixed a latent encoder bug for `TIMESTAMP_S` and `TIMESTAMP_MS` (unit-confusion in the formatter). |
| PR-7f (#?) | active | Final v0.1 docs polish + community-extensions submission readiness. Out-of-repo: open a PR in `duckdb/community-extensions` once this lands. |

All merged PRs were green on every CI check at merge time.

## Out of scope (post-v0.1 roadmap)

- `test/golden/quack/` byte-level fixtures (deferred from PR-7e per
  R-27 — heavy lift requiring stock-Quack tooling; runtime wire compat
  already covered by `test/sql/harbor.test`).
- `test/golden/ui/` byte-level fixtures (same rationale).
- Spatial GEOMETRY type encoding (requires spatial extension dep).
- Bundled UI assets mode (UI assets vendored into the extension binary
  so harbor works offline; `proxy` is the v0.1 mode).
- mbedTLS / HTTPUtil migration (PR-10b — declined for v0.1; trigger
  conditions in AGENTS.md).
- Operator-controlled cookie signing key via `HARBOR_COOKIE_SIGNING_KEY`
  env var (v0.2).
- /sql `errorCode` translation for `UNSUPPORTED_AUTH_SCHEME` (PR-7c
  noted /sql still surfaces generic `UNAUTHORIZED`).
- Error envelope normalization between `/auth/*` (`{"error":"<code>"}`)
  and `/sql` / admin (`{"ok":false,"errorCode":"<code>"}`).

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
- `SessionManager`'s `StartTimeoutSweeper()` is invoked from
  `HarborHttpServer::RegisterBuiltinHandlers()`; the sweeper thread
  is joined in `~SessionManager`.
- `harbor_query_timeout_s` enforcement uses generation counters
  (`HarborSession::query_generation` + `timed_out_generation`) so a
  late sweeper-tick can't interrupt the next-generation query.
- TIMESTAMP_S / TIMESTAMP_MS rendering: must convert via
  `Timestamp::FromEpochSeconds` / `FromEpochMs` before calling
  `Timestamp::ToString` (the latter assumes microseconds).

## If resuming after a reconnect

1. `cd /Users/shreeve/Data/Code/duckdb-harbor`
2. `git status -sb` — should show `main` clean and at PR-7e or later.
3. Run a fresh sanity build:
   ```bash
   make release
   make test_release                  # 45/45
   scripts/golden-cookie-auth.sh      # 18/18
   scripts/golden-sql-roundtrip.sh    # 32/32
   scripts/golden-admin-roundtrip.sh  # 31/31
   scripts/golden-query-timeout.sh    # 10/10
   scripts/golden-sql-types.sh        # 62/62
   ```
4. Confirm 198 assertions across 6 suites all green before any new work.
