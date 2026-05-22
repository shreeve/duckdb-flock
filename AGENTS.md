# AI Agent Guide for duckdb-harbor

**Purpose:** This document orients AI assistants (and human contributors)
to the `harbor` DuckDB extension codebase. The authoritative design
document is [`SPEC.md`](./SPEC.md); the user-facing introduction is
[`README.md`](./README.md). This file is the contributor's map.

## What is harbor

harbor is a **single DuckDB extension** that runs **one HTTP server on
one port** and serves three protocols concurrently against one shared
in-process DuckDB instance:

| Protocol | Endpoints | Forked from |
|---|---|---|
| **Quack RPC** | `POST /quack` | `duckdb/duckdb-quack` v1.5-variegata (vendored in `src/quack/`) |
| **JSON SQL** | `POST /sql` | new code; closest reference is `rip-lang/packages/db/db.rip` |
| **DuckDB UI** | `POST /ddb/*`, `GET /info`, `GET /localEvents`, `GET /localToken`, `GET /.*` | `duckdb/duckdb-ui` (vendored in `src/ui/`) |

Plus auth routes (`/auth/login`, `/auth/logout`) and convenience routes
(`/health`, `/ready`, `/tables`, `/schema/:db/:t`, `/whoami`,
`/checkpoint`, `/sessions`, `/interrupt`).

For the public-facing "why does this exist when those three already
exist" framing — useful when explaining harbor to evaluators or
grounding yourself before reading SPEC.md — see
the "Why harbor?" section of [`README.md`](./README.md).

## Implementation roadmap

harbor is being implemented in staged PRs. **Do not jump ahead** unless
the PR explicitly says so. The current state of `src/` does not yet
match the target architecture described in SPEC.md §2 — it gets there
incrementally.

| PR | Scope | Explicitly excluded |
|----|---|---|
| **PR-1** | Vendor `duckdb-quack` source verbatim into `src/quack/`; rename build identifiers (`EXT_NAME`, `TARGET_NAME`, extension class) so the loadable extension is `harbor.duckdb_extension`. Inherit upstream's `vcpkg.json` (openssl + curl). Keep upstream's `quack_serve`/`quack_stop`/`quack_check_token`/all settings intact. Add `harbor_version()` scalar. `/quack` works against stock quack clients on day one. | Renaming SQL functions/settings to `harbor_*`. `harbor_serve`/`stop`/`wait` (semantics differ from `quack_serve`). Touching wire format. UI, `/sql`, admin. Architectural refactor. |
| **PR-1.5** | Enable `LOAD_TESTS` so `test/sql/harbor.test` actually runs in CI. Extend the smoke test with a `/quack` runtime roundtrip block (con1 hosts via `quack_serve`, con2 attaches via `quack_query`, transactions/secrets/auth-failure all exercised, clean `quack_stop`). Document the five surgical edits to vendored `quack_extension.cpp` in [`docs/upstream-quack-patches.md`](./docs/upstream-quack-patches.md). Add the roundtrip-passes-after-refactor item to the PR-2 acceptance checklist below. | Anything that changes wire format or production behavior. PR-1.5 is purely test/doc infrastructure that PR-2's refactor can be measured against. |
| **PR-2** | Architectural refactor: extract `httplib::Server` from `QuackServer` into a new `HarborHttpServer`; rename `QuackServer` → `QuackHandlers`; extract `SessionManager` + `AuthManager` as standalone subsystems. Add `/health` and `/info` routes registered against the shared server. Introduce `harbor_serve` / `harbor_stop` / `harbor_wait` with SPEC §9 semantics; keep `quack_*` as functional aliases. Quack roundtrip test passes throughout. CI grep guard active. | UI, `/sql`, admin handlers. |
| **PR-3** | Port `duckdb-ui` source as `UiHandlers` registered against the shared server. Migrated `HarborHttpServer` from `duckdb_httplib::Server` (plain) to `duckdb_httplib_openssl::Server` (one namespace throughout — UI handlers can register on the shared server cleanly). UI assets via `proxy` mode (forward `GET /.*` to `ui.duckdb.org` over OpenSSL-backed cpp-httplib HTTPS client — `bundled` mode is post-v0.1, see SPEC §14). Auth: upstream UI's same-Origin check (allowed Origins = loopback variants + bind host) — harbor cookie auth deferred to PR-4. AdminHandlers `/info` extended with `X-DuckDB-UI-Extension-Version`. `HarborHttpServer::ShutdownHandlers()` added so long-running threads (UiHandlers' Watcher, EventDispatcher's SSE consumers) get released BEFORE the active-request drain. SSE handler captures `shared_ptr<ActiveRequestGuard>` in the chunked content provider closure. CI grep guard updated to match both namespaces. | `/sql`, admin. `bundled` UI assets mode. Cookie auth + harbor_crypto + login wrapper (PR-4). |
| **PR-4** | `src/harbor_crypto.{cpp,hpp}` wraps OpenSSL `libcrypto` for SHA-256 (`principal_id = hex(sha256(token))` per SPEC §6), HMAC-SHA256 (`harbor_session` cookie signing per SPEC §7), CSPRNG (`RAND_bytes` for ephemeral signing key + 16-byte cookie nonce), and base64url. New `AuthHandlers` registers `POST /auth/login` (synthetic sid `__HARBOR_AUTH__:login`), `POST /auth/logout`, `OPTIONS /auth/*`, `OPTIONS /quack`. AuthManager extended with `AuthenticateRequest()` returning `{ok, principal_id, source, error_code}` (precedence: `Authorization: Bearer` → `X-Harbor-Token` → `Cookie` — explicit-bad bearer never falls back to cookie). UiHandlers' Origin-set check supplemented with cookie-aware auth on `/ddb/run`, `/ddb/tokenize`, `/ddb/interrupt`, AND `/localEvents`. The UI catch-all (`GET /.*`) is now cookie-gated: no valid cookie → minimal HTML login page; valid cookie → proxy through to `ui.duckdb.org` (Option B per round-11 review — single code path owns "serve UI asset"). UI connection pool keyed on `(principal_id, X-DuckDB-UI-Connection-Name)` so user-controlled connection names cannot collide across principals (round-11 blocker fix). Local-dev bypass uses fixed principal `__local_dev__` so the principal-scoped invariant holds even with `harbor_local_dev_mode=true`. `harbor_cors_origins` allow-list replaces the wildcard `Access-Control-Allow-Origin: *` on `/info`; `OPTIONS` preflight wired for `/quack` and `/auth/*`; `harbor_serve` refuses to start if `harbor_cors_origins='*'`. **Cookie signing key is ephemeral random per process** (NOT a SQL setting — closes the SQL-readable-secret leak; see SPEC §7 + §15 question 2). Settings registered: `harbor_auth_cookie_ttl_s`, `harbor_cors_origins`, `harbor_local_dev_mode`. | `/sql` (PR-5). Admin handlers (PR-6). `bundled` UI assets mode. `(principal, ui_connection_name) → db_session_id` map in SessionManager (PR-5 once /sql lands and SessionManager genuinely needs principal scope; PR-4 only scopes UiHandlers' connection-name pool). `?destroy_sessions=true` on `/auth/logout` (logged-but-ignored in PR-4). `harbor_cookie_signing_key` SQL setting (deferred to v0.2 as `HARBOR_COOKIE_SIGNING_KEY` env var). |
| **PR-5** | `/sql` endpoint with `SqlHandlers` per SPEC §5.2–5.4. NDJSON streaming. Param decoding + type-encoding round trip. Principal-owned SQL sessions, `/sql/sessions/new`, `DELETE /sql/sessions/<id>`, `/auth/logout?destroy_sessions=true`, and `OPTIONS /sql` CORS preflight. | Admin handlers. `/sql/cancel`. Query timeout enforcement. UiHandlers migration to SessionManager. |
| **PR-6** | Admin handlers (`/ready`, `/whoami`, `/tables`, `/schema/:db/:t`, `/checkpoint`, `/sessions`, `/interrupt`) per SPEC §4. `__HARBOR_ADMIN__:resource:action` authz integration with centralized default-deny in `AuthManager::RunAuthorization` and `harbor_allow_admin_without_authz` operator opt-in. `POST /sql/cancel` shipped here too (admin authz on `:sessions:cancel`). HarborSession instrumented (`created_at`, `last_query`, `query_in_flight`); `SessionManager::Snapshot()` + `InterruptSession()`. CSRF + `Content-Type: application/json` + body-limit on every mutating admin POST. `/schema` uses `duckdb_columns()` with bound parameters — path identifiers never SQL-interpolated. | UiHandlers `last_query` instrumentation (v0.2 follow-up alongside UI-pool-into-SessionManager). `/schema/:db/:schema/:table` non-default schemas. CHECKPOINT auto-escalation to FORCE CHECKPOINT. `harbor_query_timeout_s` runtime enforcement (PR-7 hardening). |
| **PR-7a** (merged) | Flipped `reduced_ci_mode: 'enabled'` → `'disabled'`. CI matrix runs 9 build targets per push: `linux_amd64`, `linux_arm64`, `osx_amd64`, `osx_arm64`, `windows_amd64`, `windows_amd64_mingw`, `wasm_mvp`, `wasm_eh`, `wasm_threads` (plus matrix-generation + architecture-guard = 11 CI rows). | (none) |
| **PR-7b** (merged) | `harbor_query_timeout_s` runtime enforcement. SessionManager sweeper thread (250ms tick, generation-versioned race-fix) + per-request `QueryTimeoutWatchdog` for ephemeral connections. `InterruptCause` enum (TIMEOUT/USER_CANCEL/DISCONNECT) → HTTP 504 + `QUERY_TIMEOUT` errorCode pre-response or mid-stream NDJSON. Wired in 5 call sites. | (none) |
| **PR-7c** (merged) | Auth scheme tightening + login-page CSP+nonce. New `UNSUPPORTED_AUTH_SCHEME` errorCode for non-Bearer `Authorization`. Login page `Content-Security-Policy: default-src 'none'; script-src 'nonce-<csprng>'; …`, per-request 16-byte standard-base64 nonce. `RandomBytes` failure returns 500. | (none) |
| **PR-7d** (merged) | Full nested-type Mode B param parser for `/sql`: `LIST<T>`, `ARRAY<T,N>`, `MAP<K,V>`, `STRUCT(name1 type1, ...)`. STRUCT decoding hardened (case-insensitive lookup, duplicate-key rejection, missing-field NULL, extra-key rejection). MAP shape: array-of-pairs. Whitespace-tolerant. Recursion cap 32; type string cap 4 KiB. | UNION (clean BAD_REQUEST for v0.1); DuckDB shorthand `T[]` / `INTEGER[3]`; quoted struct field names. |
| **PR-7e** (merged) | Per-DuckDB-type `/sql` encoding round-trip — new `golden-sql-types.sh` (62 assertions). Latent encoder bug fix for `TIMESTAMP_S`/`TIMESTAMP_MS` (unit confusion). | `test/golden/quack/` + `test/golden/ui/` byte-level fixtures (post-v0.1 — runtime compat already covered by `test/sql/harbor.test`); spatial GEOMETRY. |
| **PR-7f** (active) | v0.1 docs polish + community-extensions submission readiness. Actual submission opens in `duckdb/community-extensions`. | (none) |
| **Post-v0.1+** | Bundled UI assets mode; `HARBOR_COOKIE_SIGNING_KEY` env var; `/sql` errorCode translation for `UNSUPPORTED_AUTH_SCHEME`; error envelope normalization; mbedTLS/HTTPUtil migration only on PR-10b trigger. | |
| ~~**PR-10b**~~ | **EVALUATED AND DECLINED** for v0.1 — see "PR-10b: declined" below. The OpenSSL/cpp-httplib architectural cleanup was originally planned post-PR-7. After the round-13/round-14 architectural review reduced its scope (vcpkg deps stay; the only concrete win is dropping harbor's *direct* libssl/libcrypto link), the cost-benefit no longer justified the migration risk. May be revisited under specific trigger conditions; see the dedicated section below. | n/a |

When changing code, **keep docs and tests consistent with the current
implementation status**. Don't promise anything in a release that PR-N
hasn't actually delivered.

### Architecture as of PR-2

The PR-2 refactor dissolved upstream quack's `QuackServer` /
`HttpQuackServer` hierarchy and brought harbor's target architecture
online. The current state of `src/`:

- `src/include/harbor_http_server.hpp` + `src/harbor_http_server.cpp`
  own the only `duckdb_httplib::Server` in the process. Enforced by
  the `Single duckdb_httplib::Server owner` check in
  `.github/workflows/architecture-guard.yml`.
- `src/include/harbor_session.hpp` + `src/harbor_session.cpp` hold the
  per-process session pool (`SessionManager` + `HarborSession`),
  shared by `QuackHandlers` (today) and future UI/SQL handlers.
- `src/include/harbor_auth.hpp` + `src/harbor_auth.cpp` hold the auth
  callback resolution (`AuthManager`). Cookie/principal model
  arrives in PR-3.
- `src/quack/quack_server.{cpp,hpp}` is now a stateless `QuackHandlers`
  class that registers `/quack` and `OPTIONS /quack` routes against
  the shared server. It borrows references to SessionManager + AuthManager
  via its ctor.
- `harbor_serve` / `harbor_stop` / `harbor_wait` are the SPEC §9
  lifecycle SQL functions. `quack_serve` / `quack_stop` are kept as
  thin shims that delegate to the same `HarborServerState::Global()`,
  so stock-quack tooling continues to work.
- `/health` and `/info` are served by `AdminHandlers`. The full admin
  surface (`/whoami`, `/tables`, `/checkpoint`, etc.) lands in PR-6.

Wire format on `/quack` is byte-identical to upstream Quack — the
`/quack` roundtrip block in `test/sql/harbor.test` (introduced PR-1.5)
verifies this on every CI run.

### PR-2 acceptance checklist (closed — kept here as architectural reference)

PR-2 was the architectural refactor and the highest-risk PR in the
sequence. All acceptance criteria were satisfied at merge time and
remain load-bearing invariants going forward:

- [x] Exactly one `duckdb_httplib::Server` instance exists in the
      process. It is owned by `HarborHttpServer`. `QuackHandlers` no
      longer owns or constructs an httplib server.
- [x] No `QuackServer::listen` / `QuackServer::run` / equivalent.
      Listening lifecycle lives entirely in `HarborHttpServer`
      (the entire `QuackServer` base class and `HttpQuackServer`
      derived class were deleted; `src/quack/quack_http_server.cpp`
      is gone from the tree).
- [x] `/health`, `/info`, and `/quack` are served by the same
      `HarborHttpServer` instance on the same listening socket.
- [x] The PR-1.5 `/quack` runtime roundtrip in `test/sql/harbor.test`
      still passes unchanged (auth happy + failure paths,
      multi-statement transactions, secret-based auth, large-result
      FETCH chunking, idempotent `quack_stop`, post-stop IO error).
      This proves wire format and lifecycle survived the refactor.
- [x] CI grep guard active in `.github/workflows/architecture-guard.yml`:
      `grep -REn '(make_uniq|unique_ptr|shared_ptr)<\s*duckdb_httplib::Server\s*>' src`
      must produce no matches outside `src/{include/,}harbor_http_server.*`
      (catches future regressions where another file accidentally
      reintroduces server ownership).
- [x] `SessionManager` and `AuthManager` are standalone classes
      constructed by `HarborHttpServer` and passed by reference to
      `QuackHandlers`. No global session state lives inside
      `QuackHandlers`.
- [x] `harbor_serve`, `harbor_stop`, `harbor_wait` exist with SPEC §9
      semantics (single-server-per-process; generation-counter `Wait()`
      for restart races). `quack_serve`, `quack_stop` (and the rest of
      the `quack_*` functions/settings) remain as functional aliases
      delegating to the same `HarborServerState::Global()`.
- [x] AGENTS.md "Implementation roadmap" updated to reflect PR-2 done
      and PR-3 next.

Two follow-up TODOs were captured in code as comments at PR-2 merge:

1. **Listener thread exception observability**
   (`src/harbor_http_server.cpp::ListenThreadMain`) — listen exceptions
   are silently swallowed. PR-3+ should route through the `Harbor` log
   type with the exception string.
2. **CORS wildcard on `/quack`** (`src/quack/quack_server.cpp::Register`)
   — `Access-Control-Allow-Origin: *` is fine for `/quack`-only today
   (no cookies flow through `/quack`); PR-3 must replace with the
   configured `harbor_cors_origins` allow-list when cookie auth arrives.
   **Resolved in PR-4** — `OPTIONS /quack` is owned by `AuthHandlers`
   with the allow-list, and `POST /quack` echoes the matching origin
   only when the request `Origin` is in `harbor_cors_origins`.

### PR-4 acceptance closure (closed)

PR-4 added cookie auth to the UI surface, the `harbor_crypto` libcrypto
wrapper, and the `harbor_cors_origins` allow-list. Round-11 GPT-5.5
review surfaced two security improvements over the SPEC's earlier
draft, both of which were folded in before code landed (see SPEC §7
+ §15 question 2 for the rationale). All acceptance criteria green at
merge:

- [x] `src/harbor_crypto.{cpp,hpp}` implements SHA-256, HMAC-SHA256,
      RAND_bytes, base64url, and constant-time-equal as a thin layer
      over OpenSSL `libcrypto` (already linked via PR-3).
- [x] `harbor_session` cookie format is
      `v1.<b64url(principal)>.<b64url(expires_unix)>.<b64url(nonce16)>.<b64url(hmac32)>`.
      HMAC is over the exact ASCII bytes of `v1.<seg1>.<seg2>.<seg3>`
      so verification recomputes over the on-the-wire prefix. Constant-
      time MAC compare via `CRYPTO_memcmp`.
- [x] Cookie signing key is **ephemeral random per process** (32 bytes
      from `RAND_bytes`, lazy-init under mutex on first use). NO
      `harbor_cookie_signing_key` SQL setting in v0.1 (security review:
      exposing the HMAC secret to authorized SQL would let any SQL
      caller mint cookies). v0.2 reintroduces operator control via
      the `HARBOR_COOKIE_SIGNING_KEY` environment variable.
- [x] `AuthManager::AuthenticateRequest` parses request credentials in
      precedence order Bearer → X-Harbor-Token → Cookie. Bad bearer
      NEVER falls back to cookie (round-11 review: explicit creds
      should not be masked by ambient browser state).
- [x] `AuthHandlers` owns `POST /auth/login` (synthetic sid
      `__HARBOR_AUTH__:login`), `POST /auth/logout`, `OPTIONS /auth/*`,
      and `OPTIONS /quack`. Login accepts JSON body, Bearer header,
      or X-Harbor-Token. Sets `Set-Cookie harbor_session=v1...; HttpOnly;
      SameSite=Strict; Path=/; Max-Age=<ttl>` plus `Secure` when
      `X-Forwarded-Proto: https`. Logout always returns 200 (never
      reveals whether the caller had a valid cookie).
- [x] UI catch-all `GET /.*` is cookie-gated (Option B per round-11
      review — single code path owns "serve UI asset", with cookie
      check inline). No cookie + `GET /` → minimal harbor login page
      (~70 lines inline HTML/CSS/JS, no external deps). No cookie +
      any other `GET` → 401. Valid cookie or `harbor_local_dev_mode` →
      proxy through to `ui.duckdb.org`.
- [x] `/ddb/run`, `/ddb/tokenize`, `/ddb/interrupt`, **and**
      `/localEvents` (round-11 catch — closes the unauthenticated SSE
      catalog-change observation channel) gated on Origin (CSRF) AND
      auth (cookie/bearer/local-dev). SPEC §7 line 861 invariant
      "Browser-origin requests do NOT bypass auth" enforced.
- [x] **UI connection pool keyed on `principal_id || \\0 || X-DuckDB-UI-Connection-Name`**
      (round-11 blocker fix). `X-DuckDB-UI-Connection-Name` is
      user-controlled; raw keying would let principal A guess/share
      principal B's connection. Composite keying isolates per-principal
      pools without changing `UIStorageExtensionInfo`'s API.
- [x] Local-dev bypass uses fixed principal
      `sha256("__HARBOR_LOCAL_DEV__")` so the principal-scoped invariant
      holds even with `harbor_local_dev_mode=true`.
- [x] `harbor_cors_origins` allow-list replaces wildcard CORS on `/info`
      and `/quack`. Each entry must be a well-formed
      `scheme://host[:port]` (no path/query/fragment/trailing slash).
      `harbor_serve` **refuses to start** if the setting is `'*'` or
      contains a malformed entry — the SQL `CALL harbor_serve(...)`
      throws and the server never binds.
- [x] OPTIONS preflight on `/quack`, `/auth/login`, `/auth/logout`
      emits `Access-Control-Allow-Origin: <exact-match>` only when
      the request Origin is in the allow-list. Bare 204 with no CORS
      headers when not (browser blocks). Allowed headers per SPEC §7.
- [x] Three new settings registered in `quack_extension.cpp::LoadInternal`:
      `harbor_auth_cookie_ttl_s` UBIGINT default 43200,
      `harbor_cors_origins` VARCHAR default `''`,
      `harbor_local_dev_mode` BOOLEAN default `false`.
- [x] `test/sql/harbor.test` extended (22 → 40 assertions) with
      PR-4-specific coverage: setting registration, defaults, refuse-
      to-start scenarios for `harbor_cors_origins='*'`, malformed
      origin (path), missing scheme, and non-http(s) scheme.
- [x] HTTP-level cookie roundtrip tested by
      `scripts/golden-cookie-auth.sh` (10 assertions covering /info
      no-Origin / allowed-Origin / disallowed-Origin, /auth/login
      valid/invalid/Bearer, /auth/logout cookie-clear, GET / login
      page, GET /random 401, /quack still served).
- [x] PR-1.5 `/quack` runtime roundtrip in `test/sql/harbor.test`
      passes byte-for-byte after the AuthHandlers wrapping (40
      assertions total, all green; quack wire compat preserved).
- [x] All 7 CI checks green.

Two follow-up TODOs captured in code at PR-4 merge:

1. **Default-deny for unknown Authentication scheme.** AuthManager's
   try_explicit_token requires `Authorization: Bearer ` prefix; an
   `Authorization: Basic ...` header is treated as missing-credential
   and falls through to cookie/X-Harbor-Token. Acceptable for v0.1 (we
   only document Bearer) but should explicitly reject other schemes
   in PR-5+ to avoid accidental "Basic" passthrough on misconfigured
   reverse proxies.
2. **Login page CSP + nonce.** The inline `<script>` runs without a
   `Content-Security-Policy` header, which means a future XSS in any
   future harbor-served page could inject script. The login page itself
   has no XSS surface (no untrusted strings interpolated), but PR-5+
   should add CSP `default-src 'self'; script-src 'nonce-<random>';`
   when the SQL endpoint adds error pages with potentially-tainted
   strings.

### PR-5 acceptance closure (closed)

PR-5 added the JSON `/sql` endpoint per SPEC §5.2-5.4. Design was
reviewed with GPT-5.5 round 15 before coding; the load-bearing catches
were: stream by DataChunk, buffer-before-write, emit mid-stream errors
immediately in the catch, include `/sql` CORS preflight, and do not
defer session ownership / authz / `409 SESSION_BUSY` semantics.

- [x] New `SqlHandlers` registers `POST /sql`,
      `POST /sql/sessions/new`, and `DELETE /sql/sessions/<id>` against
      the shared `HarborHttpServer`.
- [x] `POST /sql` accepts `{"sql","params","sessionId"}` JSON body,
      rejects missing `sql`, multi-statement SQL, and client-supplied
      `__HARBOR_ADMIN__:` strings with `BAD_REQUEST`.
- [x] Auth path reuses `AuthManager::AuthenticateRequest` (Bearer →
      X-Harbor-Token → Cookie). Authz path invokes
      `harbor_authorization_function` on every `/sql` request.
- [x] `/sql` response modes:
      default `application/x-ndjson` row mode,
      `application/x-ndjson; shape=chunk` chunk mode, and one-shot
      `application/json` mode.
- [x] NDJSON shape matches SPEC §5.2: `schema` line, `row`/`chunk`
      lines, and `end` line; mid-stream failures emit a final
      `{type:"error",code,message}` line while HTTP status remains 200.
- [x] Buffer-before-write invariant: each NDJSON provider call builds
      a full line/chunk string before `sink.write()`, so an exception
      cannot leave a partial JSON object on the wire.
- [x] `SqlJsonWriter` handles JSON string escaping (quotes, backslashes,
      control chars, UTF-8 validation/replacement) and locale-neutral
      numeric formatting.
- [x] `SqlParamDecoder` supports Mode A implicit params and Mode B
      typed wrappers for core scalar types (`{"type":"DECIMAL(18,4)",
      "value":"123.4567"}`), including typed NULL.
- [x] `SqlChunkEncoder` emits schema metadata + lossless row encodings
      for representative core types tested in golden coverage:
      smart BIGINT/UBIGINT/HUGEINT/UHUGEINT number-vs-string encoding,
      DECIMAL-as-string, INTERVAL object, BLOB base64, JSON text string,
      plus the scalar families.
- [x] Principal-owned SQL sessions: `HarborSession.owner_principal_id`,
      `CreateOwnedSession`, `LookupOwnedSession`, `DestroyOwnedSession`,
      and `DestroyAllOwnedBy`. Wrong-principal / unknown session ids
      collapse to `404 SESSION_NOT_FOUND` (anti-enumeration per SPEC §6).
- [x] `POST /sql/sessions/new` creates an explicit session for
      transaction state. `DELETE /sql/sessions/<id>` destroys owned
      sessions. `BEGIN` in an ephemeral request is rejected.
- [x] `/auth/logout?destroy_sessions=true` now destroys SQL sessions
      owned by the authenticated principal (legacy Quack sessions with
      empty owner are not touched).
- [x] `AuthHandlers` registers `OPTIONS /sql` CORS preflight with the
      same exact-origin allow-list behavior as `/quack` and `/auth/*`.
- [x] Settings registered in `quack_extension.cpp::LoadInternal`:
      `harbor_max_sessions` (1024), `harbor_max_response_rows` (0),
      `harbor_max_request_body_bytes` (268435456).
- [x] `test/sql/harbor.test` extended (40 → 43 assertions) with the
      PR-5 settings/defaults.
- [x] HTTP-level `/sql` golden test added:
      `scripts/golden-sql-roundtrip.sh`. It covers CORS preflight,
      row-mode NDJSON, chunk-mode NDJSON, one-shot JSON, validation
      errors, auth failures, cookie auth, params, representative type
      encodings, explicit SQL sessions, session delete, and
      logout-destroy-sessions.
- [x] Regression coverage remains green locally:
      `make release`, `make test_release` (43/43),
      `scripts/golden-cookie-auth.sh` (14/14),
      `scripts/golden-sql-roundtrip.sh` (all assertions).

PR-5 deliberate deferrals:

1. **`POST /sql/cancel`** — shipped with PR-6/admin because it needs
   admin authz (`__HARBOR_ADMIN__:sessions:cancel`) and shared interrupt
   management.
2. **Query-timeout enforcement (`harbor_query_timeout_s`)** — PR-7
   hardening. Setting remains in SPEC; runtime interrupt-after-N-seconds
   is not in PR-5.
3. **UiHandlers migration to SessionManager** — PR-4 already
   principal-scoped the UI pool using `UIStorageExtensionInfo`. PR-5
   does not churn working UI code; shared UI/SQL session state can be
   revisited after v0.1 if there is a real use case.
4. **Full nested-type param parser** — Mode B wrapper parsing supports
   core scalar type strings in PR-5. Nested explicit wrapper type
   parsing (`LIST<...>`, `STRUCT(...)`, etc.) is PR-7 hardening.

### PR-6 acceptance closure (closed)

PR-6 added the admin handler surface per SPEC §4 + §7. Design was
reviewed with GPT-5.5 round 18 before coding; the load-bearing catches
were: track "custom authz configured?" by setting presence (not
fn-name string compare), pivot `/schema/:db/:table` to `duckdb_columns()`
with bound parameters (vs. composing identifiers into a SQL string),
require CSRF + Content-Type + body-limit on every mutating POST, and
fix the lock ordering for `SessionManager::Snapshot()` (map-lock →
copy `shared_ptr`s → release → per-session brief lock).

- [x] `AuthManager::RunAuthorization` enforces a centralized
      default-deny on `__HARBOR_ADMIN__:` synthetic strings whenever
      no custom hook is configured (both `harbor_authorization_function`
      and `quack_authorization_function` empty), unless the new
      `harbor_allow_admin_without_authz=true` operator opt-in is set.
      Detection is by setting presence — not by string-comparing the
      resolved fn name to the literal `"harbor_nop_authorization"` —
      so schema-qualified, cased, or aliased names cannot silently
      bypass the rule.
- [x] `harbor_allow_admin_without_authz` BOOLEAN default `false`,
      registered alongside the other PR-4/PR-5 settings.
- [x] Loud startup `WARN` log via the existing Harbor/Quack log type
      whenever `harbor_allow_admin_without_authz=true` is in effect
      with no custom authz function — per SPEC §7 line 845
      ("Logged loudly at server start").
- [x] `HarborSession` instrumented with `created_at` (steady_clock,
      set in ctor), `last_query` (string, mutated under per-session
      `lock`), and `query_in_flight` (`std::atomic<bool>`). Wired in
      `SqlHandlers::HandleSql` (around `Execute()` + the streaming
      provider lifetime) and `QuackHandlers` PREPARE / FETCH / APPEND
      cases. UiHandlers' `/ddb/run` is intentionally NOT instrumented
      — its connections live in the separate `UIStorageExtensionInfo`
      pool and don't appear in `SessionManager::active`; the
      UI-pool-into-SessionManager migration is a v0.2 follow-up.
- [x] `SessionManager::Snapshot(idx_t last_query_max_chars)` returns
      a `vector<SessionSnapshot>` for `/sessions` JSON output. Lock
      ordering: take map mutex, copy `shared_ptr<HarborSession>`'s,
      release map mutex, then briefly take each session's `lock` to
      copy `last_query`. `query_in_flight` is read via std::atomic
      without taking the per-session lock. `SessionSnapshot.last_query`
      is truncated to `last_query_max_chars` (200 in /sessions); the
      `last_query_truncated` boolean indicates whether truncation
      happened.
- [x] `SessionManager::InterruptSession(sid)` looks up under map
      lock, copies the `shared_ptr` so the session stays alive for
      the call, releases map lock, then invokes `Connection::Interrupt()`
      WITHOUT taking the per-session mutex (Interrupt is concurrency-
      safe by design — it just sets the executor's interrupt flag).
- [x] `AdminHandlers` ctor extended to take `(server, auth, sessions, db)`
      and registers all 8 admin routes per SPEC §4: `/health` (PR-2),
      `/info` (PR-2), `/ready` (new), `/whoami`, `/tables`,
      `/schema/:db/:table`, `/checkpoint`, `/sessions`, `/interrupt`.
- [x] `/schema/:db/:table` uses `duckdb_columns()` with bound `Value`
      parameters for `database_name` and `table_name` — path
      parameters are NEVER string-interpolated into SQL or into the
      `__HARBOR_ADMIN__:` policy decision input (per SPEC §7). Default
      schema is `main`; a future `/schema/:db/:schema/:table` can
      cover non-default schemas. Missing-table returns 404
      `NOT_FOUND`; 200 returns the columns array with name, type,
      nullable, and default (NULL when absent).
- [x] `/checkpoint` runs plain `CHECKPOINT;` and reports `409 CONFLICT`
      cleanly when DuckDB returns "Cannot CHECKPOINT: there are other
      write transactions active". Operators who want forcing semantics
      issue `FORCE CHECKPOINT` via `/sql` from a privileged session;
      auto-escalating in the handler would risk indefinite blocks.
      Response shape: `{ok:true, checkpointed_at:<iso UTC>,
      wal_state_available:false}` — explicit about the v0.1 limitation
      that v1.5.2 doesn't expose stable WAL-size accounting from a
      Connection.
- [x] `/sessions` returns `{ok:true, sessions:[{session_id, principal,
      age_s, in_flight, last_query, last_query_truncated}]}` from
      `SessionManager::Snapshot(200)`.
- [x] `/interrupt` and `/sql/cancel` (the latter lives in `SqlHandlers`)
      both require `Content-Type: application/json`, cap the body at
      `harbor_max_request_body_bytes` (413 on overflow), parse a
      lightweight `{"sessionId":"…"}` envelope, and call
      `SessionManager::InterruptSession()`. Distinct authz strings
      (`__HARBOR_ADMIN__:sessions:interrupt` vs.
      `__HARBOR_ADMIN__:sessions:cancel`) so a custom authz macro can
      grant cancel without granting full interrupt.
- [x] Cookie-authenticated mutating POSTs (`/checkpoint`, `/interrupt`,
      `/sql/cancel`) require an `Origin` (or, falling back, `Referer`)
      in `harbor_cors_origins` — same CSRF gate that PR-5 applied to
      `/sql`. Bearer / `X-Harbor-Token` callers bypass the gate (they
      are not browser-ambient).
- [x] OPTIONS preflight registered in `AuthHandlers` for `/checkpoint`,
      `/interrupt`, `/sql/cancel` alongside the existing `/sql`,
      `/sql/sessions/*`, `/auth/*`, `/quack` preflights.
- [x] PR-5's `__HARBOR_ADMIN__:sessions:create` /
      `__HARBOR_ADMIN__:sessions:delete` paths now genuinely default-deny
      (PR-5 had registered the strings but the rule wasn't enforced
      yet). The `/sql` golden harness opts into the bypass with one
      new `SET GLOBAL harbor_allow_admin_without_authz=true` line so
      it can keep exercising session create + transaction state without
      writing a custom authz fn.
- [x] `test/sql/harbor.test` extended (43 → 44 assertions) with
      `harbor_allow_admin_without_authz` setting registration +
      default value.
- [x] HTTP-level admin coverage added: `scripts/golden-admin-roundtrip.sh`
      (26 assertions across three server lifecycles — default-deny mode,
      admin-bypass mode, and a custom authz-fn mode that grants
      per-resource:action so the SPEC §7 grammar is provably decidable).
      Covers: /health/info/ready public probes, default-deny matrix
      across 7 admin routes, /whoami identity JSON, /tables shape +
      after-CREATE-TABLE reflection, /schema 404 on missing,
      /schema identifier safety against quoted-name attacks,
      /schema 200 on existing (columns array + database/schema fields),
      /checkpoint 200-or-409 contract (never 5xx), /interrupt 415/400/404/200,
      /interrupt body-limit 413, /sessions empty + post-create snapshot
      with all instrumentation fields, /sql/cancel 200 happy path,
      cookie-auth Origin gating (no/allowed/disallowed → 403/200/403),
      OPTIONS preflight for /interrupt, /sql/cancel, /checkpoint,
      and per-resource granular gating with a custom authz fn.
- [x] All four test suites green:
      `make test_release` (44/44),
      `scripts/golden-cookie-auth.sh` (14/14),
      `scripts/golden-sql-roundtrip.sh` (19/19),
      `scripts/golden-admin-roundtrip.sh` (26/26).

PR-6 deliberate deferrals:

1. **Query-timeout enforcement (`harbor_query_timeout_s`)** — PR-7
   hardening. Setting remains in SPEC; runtime interrupt-after-N-seconds
   is not in PR-6.
2. **UiHandlers `last_query`/`query_in_flight` instrumentation** —
   v0.2 follow-up alongside the UI-pool-into-`SessionManager` merge.
   `/sessions` legitimately reflects `/quack` and `/sql` sessions only
   today; admins observing UI activity should use upstream DuckDB
   instrumentation (`pragma_database_size`, `duckdb_logs_parsed`).
3. **`/schema/:db/:schema/:table`** — non-default schema support.
   v0.1 only describes `<db>.main.<table>`; the route table will get
   a 3-segment variant in PR-7+.
4. **CHECKPOINT auto-escalation to FORCE CHECKPOINT** — declined for
   v0.1. Auto-escalating risks indefinite blocks; the 409 CONFLICT
   contract gives operators an explicit retry/escalate decision point.
5. **Default-deny on `__HARBOR_ADMIN__:server:whoami` carve-out** —
   declined per round-18 review. SPEC literal applies uniformly across
   all `__HARBOR_ADMIN__:*` strings; operators flip
   `harbor_allow_admin_without_authz=true` for unrestricted dev access
   or write a custom authz fn.

### PR-8 acceptance closure (closed)

PR-8 was a security regression fix on the PR-4 cookie auth, surfaced
by GPT-5.5 round-13 architectural review. The pre-PR-4 `HandleProxyGet`
forwarded the entire browser `Cookie` header to `ui.duckdb.org` so
MotherDuck's domain cookies could pass through. PR-4 introduced our
own `harbor_session=v1.<principal_hex>...` cookie under harbor's origin;
the browser sends it on every request to harbor, including
`/assets/*`, and the old passthrough forwarded it to `ui.duckdb.org`
— leaking harbor auth material to a third-party origin.

- [x] `HandleProxyGet` rewritten with strict allow-list: forwards
      only `Accept`, `Accept-Encoding`, `Accept-Language`,
      `If-None-Match`, `If-Modified-Since`, `Range`. Never
      `Cookie`, `Authorization`, `X-Harbor-Token`, `X-Harbor-Session-Id`,
      `Origin`, or `Sec-*` headers.
- [x] `scripts/golden-cookie-auth.sh` extended (11 → 14 assertions).
      New fixture: tiny inline Python listener acting as a fake
      `ui.duckdb.org` that captures whatever harbor proxies upstream.
      Three scenarios cover all three auth credential types
      (Cookie, Bearer, X-Harbor-Token); each asserts the credential
      itself does NOT leak upstream while allow-listed asset-fetch
      headers DO pass through.
- [x] SPEC §8 rewritten with the credential-strip allow-list as a
      named invariant, not just an implementation detail.
- [x] Codified for future change: any future rewrite of
      `HandleProxyGet` MUST preserve the same allow-list invariant.
      Both `src/ui/ui_handlers.cpp` and SPEC §8 carry the load-bearing
      comment.

### PR-9: CANCELLED — `curl` is required (analytical + empirical)

A planned PR-9 to drop `"curl"` from `vcpkg.json` was cancelled. The
"appears unused" framing in the round-13 external handoff was a
narrow claim about harbor's direct source code (no
`#include <curl/...>` in `src/`) — true, but consistent with "harbor
uses libcurl via `HTTPUtil` at runtime". Two independent reasons
keep `curl` in `vcpkg.json`:

**Analytical (the one I should have caught before attempting PR-9):**
harbor auto-loads `httpfs` at runtime in `src/quack/quack_client.cpp`
(`ExtensionHelper::AutoLoadExtension(db, "httpfs")` at line 127) and
then uses `HTTPUtil::Get(db)` at line 34 for the outbound HTTPS path
of `ATTACH 'quack:host'`. `HTTPUtil` is DuckDB's libcurl-backed HTTP
client (provided by httpfs). So harbor USES libcurl at runtime,
transitively through httpfs. The build pipeline that bundles httpfs
must therefore provide libcurl, which is what `vcpkg.json`'s `curl`
entry does.

**Empirical (mechanical confirmation):** `make release` immediately
breaks if `curl` is dropped:

```text
No rule to make target `vcpkg_installed/arm64-osx/lib/libcurl.a',
  needed by `extension/httpfs/httpfs.duckdb_extension'
```

`extension_config.cmake:16-19` builds httpfs as a sibling extension
so harbor's tests (specifically the PR-1.5 `/quack` runtime roundtrip
in `test/sql/harbor.test`, which exercises auto-loading httpfs) work
end-to-end in CI. httpfs's own source needs `libcurl` for HTTP and
`libssl`/`libcrypto` for HTTPS / S3 signing. So `vcpkg.json`'s
`["openssl", "curl"]` list is **not dead weight** — both are required
by the bundled httpfs build, AND we use both at runtime.

### PR-10b: declined — keep httplib + OpenSSL

Originally planned post-PR-7: rewrite `HandleProxyGet` to
`HTTPUtil`/libcurl, rewrite `harbor_crypto.cpp` to wrap
`MbedTlsWrapper::ComputeSha256Hash` + `Hmac256`, migrate cpp-httplib
namespace `duckdb_httplib_openssl::` → plain `duckdb_httplib::`,
drop harbor's direct `find_package(OpenSSL)` + `target_link_libraries`,
add a bounded asset cache + `harbor_crypto_selftest()` smoke test
function.

After the round-14 GPT-5.5 review reduced scope (vcpkg deps stay
regardless; httpfs is a mandatory runtime dep through which OpenSSL
arrives anyway), this was **evaluated and declined for v0.1**.

Cost/benefit at the actual scope:

| Save | Concrete impact |
|---|---|
| Binary size | ~200 KB – 1 MB per platform binary; 0.6%–3% of the current 34 MB. One-time per user download. |
| Direct OpenSSL link from harbor | harbor_extension stops linking libssl/libcrypto; operationally the same — OpenSSL still in-process via httpfs. |
| One cpp-httplib namespace | Cleaner; marginal. |
| `harbor_crypto.cpp` shorter | ~10 LOC mbedTLS wrappers vs ~50 LOC OpenSSL ceremony. But the existing OpenSSL version IS written, working, and tested. |

| Cost | Concrete impact |
|---|---|
| ~500 LOC migration | Touches crypto + proxy + server namespace + CMake. |
| **mbedTLS ABI risk** | `MbedTlsWrapper` symbols are NOT `DUCKDB_API`-annotated. Per round-14, may not resolve from a loadable extension on some platform. If it fails: vendor `mbedtls/library/sha256.c` etc. into our build — more files to maintain across upstream rebases forever. |
| **HandleProxyGet rewrite risk** | Per round-13: HTTPUtil "may buffer full responses, may normalize/throw on non-2xx including 304, may auto-decompress / follow redirects / alter headers, may not expose arbitrary response headers cleanly." Could regress the PR-3 UI golden roundtrip. |
| **Re-implementing PR-8** | The credential-strip allow-list lives inside cpp-httplib `Headers` today. HTTPUtil has a different headers API; the invariant has to be re-asserted in the new shape, with new tests. |
| Diverges from upstream `duckdb-ui` | Upstream chose `duckdb_httplib_openssl::`. Migrating away makes future rebases harder. |
| Opportunity cost | ~1 day of work that PR-5 (`/sql`) could be using. |

**Decision:** the win is small enough to be aesthetic; the migration
risk + opportunity cost (PR-5 needs the same attention) outweighs
it. Operationally, the user-visible behavior is identical before and
after PR-10b — vcpkg deps stay, runtime OpenSSL dep stays via httpfs,
binary is functionally equivalent.

The current architecture (cpp-httplib + OpenSSL on the link line +
OpenSSL libcrypto in `harbor_crypto.cpp` + httpfs at runtime for
ATTACH-outbound and CSPRNG) is the v0.1 architecture. CI is green on
all 7 platforms; PR-8's credential-strip invariant is enforced by
`scripts/golden-cookie-auth.sh`.

**Trigger conditions that WOULD justify revisiting PR-10b later:**

- Downstream forces a crypto migration (CVE in libcrypto, distribution
  pressure, certificate-authority change that breaks our pinned
  OpenSSL build, etc.).
- Upstream `duckdb-ui` itself migrates away from
  `duckdb_httplib_openssl::` (we follow to keep rebases easy).
- A real binary-size constraint emerges (CDN size limits, embedded
  use case, mobile distribution).
- A real "want to drop httpfs entirely" use case emerges (then
  everything in this conversation gets revisited from scratch).

Until any of those happens: the architecture as it stands is fine.
Don't migrate just for cleanliness.

## Critical rules — read first

- **`SPEC.md` is the source of truth for design decisions.** When in
  doubt about behavior, read SPEC.md before guessing. If SPEC.md is
  silent on something material, raise it as an open question — don't
  invent semantics ad hoc.
- **Never edit `src/quack/quack_message.{cpp,hpp}`.** This is the wire
  format. Changes desync from upstream Quack clients. Coordinate with
  upstream `duckdb-quack` instead.
- **Never edit `misc/`.** Read-only reference clones of upstream repos.
  See "Reading upstream" below.
- **Never break Quack wire compatibility.** A vanilla DuckDB client with
  the upstream `quack` extension installed must `ATTACH 'quack:host'`
  successfully against a harbor server. The roundtrip block in
  `test/sql/harbor.test` is the v0.1 regression baseline (byte-level
  fixtures in `test/golden/quack/` are post-v0.1 backlog). Anytime
  you touch vendored quack code (per
  [`docs/upstream-quack-patches.md`](./docs/upstream-quack-patches.md)):
  rerun the roundtrip block locally and in CI.
- **Never break UI wire compatibility.** The official DuckDB UI must
  work against `GET /` and `POST /ddb/*` byte-for-byte against the
  pinned `duckdb-ui` commit. Cookie auth + UI-proxy credential-strip
  invariants are covered by `scripts/golden-cookie-auth.sh`
  (byte-level UI fixtures in `test/golden/ui/` are post-v0.1 backlog).
- **Route order matters.** cpp-httplib resolves routes in registration
  order. The `GET /.*` catch-all (UI assets) **must** be registered
  last. Specific routes (`/info`, `/health`, `/sql`, `/quack`, `/ddb/*`,
  admin) **must** be registered before it.
- **Run tests before every commit.** `make test_release` plus the
  six `scripts/golden-*.sh` suites must all pass (198 assertions
  across 6 suites — see "When you finish a substantial change" below).

## Compilation pipeline

```text
src/*.cpp ──► CMake ──► build/release/extension/harbor/harbor.duckdb_extension
                            │
                            ├── statically links DuckDB                (via duckdb/ submodule)
                            ├── statically links cpp-httplib           (vendored from duckdb-quack)
                            ├── statically links OpenSSL libssl/libcrypto
                            └── runtime: forwards UI catch-all to ui.duckdb.org
                                                                       (proxy mode in v0.1; bundled mode is post-v0.1)
```

The extension is a single `.duckdb_extension` file. Distribution is via
DuckDB's community extension repo (planned) and GitHub Releases (today).

## Repository layout

The top-level layout follows the standard DuckDB community-extension
template — don't restructure it, the `extension-ci-tools` Makefiles
expect every path where it is.

Two upstream-source directories sit side-by-side at the top, doing
different jobs (this trips up first-time readers):

- **`duckdb/`** — git submodule pinned to v1.5.3. **Build-required:**
  `CMakeLists.txt` references `duckdb/third_party/httplib` and
  `duckdb/extension/autocomplete/include`. The build literally won't
  work without it. Never edit.
- **`misc/duckdb-quack/`, `misc/duckdb-ui/`** — untracked, gitignored
  local clones for human-reference reading and rebase diff'ing. Not
  part of the build. Never edit. See "Reading upstream" below.

Top-level files + tracked dirs:

| Path | Role | Edit? |
|---|---|---|
| `SPEC.md` | Authoritative design | Carefully — major changes need consensus |
| `README.md` | User-facing introduction | Yes |
| `AGENTS.md` | This file | Yes |
| `LICENSE` | MIT, dual copyright (DuckDB Foundation + harbor authors) | Never silently |
| `CMakeLists.txt`, `Makefile`, `vcpkg.json`, `extension_config.cmake`, `.editorconfig` | Build configuration | Carefully |
| `duckdb/` | Submodule, pinned at v1.5.3 (build-required) | Never |
| `extension-ci-tools/` | Submodule (provides Makefiles + Actions matrix) | Never |
| `docs/upstream-quack-patches.md`, `docs/upstream-ui-patches.md` | Tracks every edit harbor made to vendored upstream code so future rebases are diffable | Yes (append when rebasing) |

Source tree (`src/` is flat — handlers live at the top of `src/`,
not in per-subsystem subdirs; `quack/` and `ui/` subdirs hold the
larger vendored chunks):

| Path | Role | Edit? |
|---|---|---|
| `src/harbor_extension.cpp` | Entry point: registers settings, scalar functions, lifecycle table macros | Yes |
| `src/harbor_lifecycle.{cpp,hpp}` | `harbor_serve` / `harbor_stop` / `harbor_wait` semantics | Yes |
| `src/harbor_http_server.{cpp,hpp}` | `HarborHttpServer` — owns the only `httplib::Server` in-process | Yes |
| `src/harbor_session.{cpp,hpp}` | `SessionManager` + sweeper thread for query timeouts | Yes |
| `src/harbor_auth.{cpp,hpp}` | `AuthManager` — token + hook resolution, HMAC cookie verify, CORS allow-list | Yes |
| `src/harbor_crypto.{cpp,hpp}` | OpenSSL libcrypto wrapper: SHA-256, HMAC-SHA256, RAND_bytes, base64url | Yes |
| `src/harbor_query_timeout.{cpp,hpp}` | `QueryExecutionGuard` + `QueryTimeoutWatchdog` (PR-7b) | Yes |
| `src/auth_handlers.{cpp,hpp}` | `/auth/login`, `/auth/logout`, OPTIONS preflights | Yes |
| `src/admin_handlers.{cpp,hpp}` | `/ready`, `/whoami`, `/info`, `/tables`, `/schema/:db/:t`, `/checkpoint`, `/sessions`, `/interrupt` | Yes |
| `src/sql_handlers.{cpp,hpp}` | `POST /sql`, `/sql/sessions/{new,delete}`, `/sql/cancel` | Yes |
| `src/sql_chunk_encoder.{cpp,hpp}` | `DataChunk` → NDJSON per SPEC §5.4 | Yes |
| `src/sql_json_writer.{cpp,hpp}` | JSON-string escaping + locale-neutral number formatting | Yes |
| `src/sql_param_decoder.{cpp,hpp}` | JSON params → DuckDB Value (Mode A + nested Mode B per PR-7d) | Yes |
| `src/quack/quack_extension.cpp` | DuckDB extension entry for vendored Quack code | Yes |
| `src/quack/quack_server.{cpp,hpp}` | `QuackHandlers` — `/quack` route registration + dispatch | Yes |
| `src/quack/quack_message.{cpp,hpp}` | Wire format — match upstream | **Never** unless tracking upstream change |
| `src/quack/quack_scan.cpp`, `src/quack/quack_storage.cpp`, `src/quack/storage/*` | Client side (`ATTACH 'quack:host'`), mostly unchanged from upstream Quack | Carefully |
| `src/ui/ui_extension.cpp` | DuckDB extension entry for vendored UI code | Yes |
| `src/ui/ui_handlers.{cpp,hpp}` | `/ddb/*`, `/info`, `/localEvents`, `/localToken`, GET `/.*` proxy + login page (PR-7c CSP+nonce) | Yes |
| `src/ui/event_dispatcher.cpp`, `src/ui/watcher.cpp`, `src/ui/state.cpp`, `src/ui/settings.cpp`, `src/ui/utils/*` | Vendored UI subsystems | Carefully |

Tests + scripts:

| Path | Role | Edit? |
|---|---|---|
| `test/sql/harbor.test` | sqllogictest (loaded via `LOAD_TESTS` in `extension_config.cmake`) — extension load smoke + `/quack` runtime roundtrip + setting-defaults | Yes |
| `scripts/golden-cookie-auth.sh` | HTTP roundtrip: `/auth/login`, `/auth/logout`, cookie + CORS, login-page CSP+nonce, UI-proxy credential-strip | Yes |
| `scripts/golden-sql-roundtrip.sh` | HTTP roundtrip: `/sql` row/chunk NDJSON + one-shot JSON, sessions, params (incl. nested Mode B) | Yes |
| `scripts/golden-admin-roundtrip.sh` | HTTP roundtrip: admin endpoints + default-deny matrix | Yes |
| `scripts/golden-query-timeout.sh` | HTTP roundtrip: `harbor_query_timeout_s` enforcement (sweeper + watchdog) | Yes |
| `scripts/golden-sql-types.sh` | HTTP roundtrip: per-DuckDB-type `/sql` encoding (62 types incl. ENUM) | Yes |

> **Post-v0.1 test backlog:** byte-level Quack/UI fixtures in
> `test/golden/quack/` + `test/golden/ui/`, and a per-type encoder
> unit test in `test/types/`. Runtime wire compat is currently covered
> end-to-end by `test/sql/harbor.test`'s `/quack` block + the
> `scripts/golden-*.sh` suite.

Untracked (gitignored):

| Path | Role |
|---|---|
| `build/` | CMake out-of-tree build (`make release` / `make debug` populate this) |
| `.cache/` | clangd / IDE indexer cache |
| `misc/` | Local clones of upstream `duckdb-quack` and `duckdb-ui` for reference (see "Reading upstream") |
| `duckdb_unittest_tempdir/` | Created on demand by DuckDB's unittest binary — auto-recreated, safe to delete |

## Reading upstream

`misc/` holds untracked clones of the upstream projects harbor derives
from. They are not part of the build. Use them as reference when:

- Tracking what upstream changed since our last rebase
- Diagnosing whether a wire-format mismatch is a bug in harbor or a real
  upstream change
- Understanding why an upstream design decision was made

```bash
# refresh upstream references
( cd misc/duckdb-quack && git fetch && git checkout v1.5-variegata && git pull )
( cd misc/duckdb-ui    && git fetch && git checkout main             && git pull )
```

If upstream introduces a change we want to absorb, copy the relevant
files into `src/quack/` or `src/ui/`, preserve their copyright headers,
add ours, run the golden tests, fix what breaks.

## Component model

```text
HarborHttpServer (owns httplib::Server, listener thread, all subsystems)
│
├── SessionManager           ─┐
├── AuthManager              ─┤  shared by every handler subsystem
├── InterruptManager         ─┤  (passed by reference, never copied)
├── EventDispatcher (SSE)    ─┘
│
├── QuackHandlers      .Register(httplib::Server&)  → /quack, OPTIONS /quack
├── SqlHandlers        .Register(httplib::Server&)  → /sql, /sql/sessions, /sql/cancel
├── UiHandlers         .Register(httplib::Server&)  → /ddb/*, /info, /localEvents, /localToken
├── AdminHandlers      .Register(httplib::Server&)  → /health, /ready, /whoami, /tables, /schema, /checkpoint, /sessions, /interrupt
└── UiAssets          .Register(httplib::Server&)  → GET /.*    (MUST BE LAST — catch-all)
```

The handler subsystems are independent. None depends on another. They
share only `SessionManager`, `AuthManager`, `InterruptManager`, and the
`shared_ptr<DatabaseInstance>`.

This is a **deliberate departure from upstream**:

- Upstream **`duckdb-quack`** has its `QuackServer` directly own the
  `httplib::Server`. We refactor: `QuackServer` becomes
  `QuackHandlers`, drops its server, registers routes against the
  shared one.
- Upstream **`duckdb-ui`** has its `ui::HttpServer` directly own a
  separate `httplib::Server`. We refactor identically.

Why: we need both protocols on one port, and inheritance-based
ownership ("Quack owns the server, UI plugs in") would couple them
forever. The shared-server-with-handler-registrars model keeps each
subsystem independently maintainable as upstream evolves.

## Common tasks

### Build

```bash
git submodule update --init --recursive    # first time
make release                                # builds the extension
make debug                                  # build with -g, no opt
make test_release                           # runs the sqllogictest suite
make clean
```

The output is at `build/release/extension/harbor/harbor.duckdb_extension`.

### Smoke test against a live extension

> **Daemon-mode pitfall.** `duckdb -c '…'` exits as soon as the last
> statement returns, which tears down the harbor server before you can
> hit it from another terminal. Always include `CALL harbor_wait();` at
> the end of any non-interactive invocation. The test harness also
> accepts an `&` background form for shell loops — see below.

Interactive (REPL stays open, server runs in background):

```bash
make release
duckdb -unsigned   # opens the REPL
LOAD '/abs/path/build/release/extension/harbor/harbor.duckdb_extension';
CALL harbor_serve(bind := '127.0.0.1', port := 9494);
-- REPL is still yours; server is alive in a background thread
```

Non-interactive (script-driven smoke test):

```bash
make release
duckdb -unsigned -no-stdin -c "
  LOAD '$PWD/build/release/extension/harbor/harbor.duckdb_extension';
  CALL harbor_serve(bind := '127.0.0.1', port := 9494);
  CALL harbor_wait();          -- blocks until SIGTERM/SIGINT
" &
DUCK_PID=$!
sleep 0.5

curl -sf http://127.0.0.1:9494/health
curl -sf -H "Authorization: Bearer $TOKEN" \
     -X POST http://127.0.0.1:9494/sql \
     -H "Content-Type: application/json" \
     -d '{"sql":"SELECT 42 AS answer"}'

kill $DUCK_PID
wait $DUCK_PID 2>/dev/null
```

### Add a new SQL setting

1. Add to `src/quack/quack_extension.cpp::LoadInternal` (the settings
   registration block) — that's where all `harbor_*` settings live.
2. Document in `SPEC.md` §9.
3. Add an assertion to `test/sql/harbor.test` covering registration +
   default value, plus a roundtrip in the relevant
   `scripts/golden-*.sh` if the setting is observable on the wire.

### Add a new endpoint

1. Pick the right handler subsystem (`SqlHandlers`, `UiHandlers`,
   `AdminHandlers`, `AuthHandlers`, `QuackHandlers`).
2. Add route registration in that subsystem's `Register()`.
3. Verify it does not collide with `GET /.*` (which must remain last).
4. Document in `SPEC.md` §4 and §5.
5. Add an HTTP roundtrip assertion to the matching
   `scripts/golden-*.sh` suite.

### Update the bundled UI assets

> v0.1 uses `proxy` mode (forward `GET /.*` to `ui.duckdb.org` over
> the cpp-httplib OpenSSL HTTPS client). Bundled UI assets — UI
> compiled into the extension binary so harbor works offline — are
> post-v0.1 backlog. When bundled mode lands, this section will
> describe the asset-mirroring + regeneration workflow.

### Rebase against upstream Quack

```bash
( cd misc/duckdb-quack && git fetch && git checkout v1.5-variegata && git pull )
# Diff against the version we vendored into src/quack/
diff -ru misc/duckdb-quack/src/ src/quack/
# For each non-trivial change, decide: absorb, defer, or skip
# Update src/quack/ accordingly, preserve dual copyright headers,
# and update docs/upstream-quack-patches.md with any new edits
make test_release       # sqllogic incl. /quack runtime roundtrip
scripts/golden-sql-roundtrip.sh
git commit -m "rebase quack to <commit>"
```

### Rebase against upstream UI

Identical procedure with `misc/duckdb-ui` and `src/ui/`.

## Wire format invariants

If you're touching anything that sends bytes over the wire, hold these
in mind:

| Invariant | Why |
|---|---|
| `/quack` request and response framing matches upstream `duckdb-quack` byte-for-byte | Stock Quack clients depend on it |
| `/quack` uses `application/vnd.duckdb` Content-Type | Stock Quack clients reject other types |
| `/ddb/run`, `/ddb/tokenize`, `/ddb/interrupt` responses match upstream `duckdb-ui` byte-for-byte | The official UI is the only consumer |
| `/ddb/*` responses use `application/octet-stream` Content-Type | Match upstream UI |
| `/sql` NDJSON schema line is the authority for type decoding | Values may choose the most ergonomic exact JSON shape (e.g. BIGINT number vs string); clients must consult schema for the underlying DuckDB type |
| `BIGINT`, `HUGEINT`, `UBIGINT`, `UHUGEINT` are encoded as JSON numbers only inside `[-(2^53-1), 2^53-1]`, strings outside that range | Ergonomic common counts/ids without losing precision for large identifiers; SPEC §5.4 |
| `MAP<K,V>` is encoded as array-of-pairs, not object | Keys can be non-string; ordering matters |
| `JSON` column values are encoded as JSON-text strings, not nested JSON | Disambiguates SQL NULL from JSON null |
| `INTERVAL` is encoded as `{months, days, micros}` object with micros as string | Matches DuckDB's `interval_t` exactly |
| `BLOB` is base64 | Standard |

## Auth model invariants

| Invariant | Why |
|---|---|
| Browser-origin requests do **NOT** bypass token auth | Origin is CSRF defence, not authentication. SPEC §7. |
| `harbor_authentication_function` is called on every Quack `CONNECTION_REQUEST`, every `/sql` first request that creates an ephemeral session, every `/ddb/run` first request, and on every `POST /sql/sessions` | Identity is established once per session |
| `harbor_authorization_function` is called on every SQL-bearing request: Quack `PREPARE_REQUEST`, Quack `APPEND_REQUEST`, `/sql`, `/ddb/run`, AND on every admin endpoint with a synthetic `__HARBOR_ADMIN__:<resource>:<action>` query | Authorization is per-statement; admin authz is uniform with regular authz. Resource/action pairs (not bare verbs) so policies can grant `sessions:list` without granting `sessions:interrupt`. |
| Admin endpoints are **default-deny** when `harbor_authorization_function = harbor_nop_authorization` (the permissive default), unless `harbor_allow_admin_without_authz = true` is explicitly set | Operators who set a token but forget to write a custom authz function get safe-by-default admin |
| Path parameters and request-body fields are **never** interpolated into the synthetic `__HARBOR_ADMIN__:` strings | Concrete identifiers go in the request envelope, not the policy decision input. Stops authz-string injection. |
| `principal_id = hex(sha256(client_token))` | Auth hook returns BOOLEAN only; identity comes from credential. Same token → same principal everywhere. Logs use first 8 hex chars only. |
| Auth principal cookie (`harbor_session=<HMAC-signed>`) and DB session id are **distinct concepts** | A principal can own many DB sessions; SPEC §6 |
| Every DB session lookup verifies `owner_principal` matches the caller | Stops sessionId-guessing attacks |
| `/localToken` is **disabled** when `harbor_bind ≠ 127.0.0.1` | MotherDuck token disclosure on remote-bound servers is a privilege escalation |
| `harbor_local_dev_mode` is **forced off** when `harbor_bind ≠ 127.0.0.1` | Belt-and-suspenders against accidental exposure |
| Auth callbacks run in a fresh transient connection per call | They cannot rely on session-local state — see SPEC §7 |

## Concurrency invariants

| Invariant | Why |
|---|---|
| One `Connection` per session, full request held under session mutex | DuckDB `Connection` is not concurrent-safe across queries |
| Concurrent requests for the same `sessionId` → HTTP 409 `SESSION_BUSY` | Cleaner contract than serializing implicitly |
| `Connection::Interrupt()` is invoked on client disconnect during `/sql` streaming | Frees the worker thread |
| Idle sessions swept on a 60s tick | Bounds resource use without a dedicated reaper |

## Logging

The runtime log type is registered as `'Quack'` (inherited verbatim
from upstream `duckdb-quack`):

```sql
CALL enable_logging('Quack');
-- ... run some queries ...
SELECT * FROM duckdb_logs_parsed('Quack');

-- Persist to disk
CALL enable_logging('Quack', storage => 'file',
                    storage_config => {'path':'/var/log/harbor'});
```

> **Naming history:** the harbor design originally proposed a
> `'Harbor'` log type with `'Quack'` as a back-compat alias — see
> issue [#30](https://github.com/shreeve/duckdb-harbor/issues/30).
> That rename was deferred since the existing `'Quack'` name is
> functionally complete and stays compatible with any upstream
> tooling that filters on it. Documentation here was updated in
> v0.1.2 to match the actual registered name.

## Known gotchas

- **`SET harbor_authentication_function = '...'` must be `SET GLOBAL`.**
  The auth path runs in fresh worker connections that don't see
  session-local settings. A plain `SET` silently has no effect on
  authentication.
- **`RESET` does not undo a `SET GLOBAL`** for the same reason. Use
  `RESET GLOBAL harbor_authentication_function`.
- **`GET /.*` catch-all must be registered last.** Otherwise it shadows
  every other GET route. Add new routes BEFORE the catch-all in
  `HarborHttpServer::RegisterAll()`.
- **`duckdb -c "CALL harbor_serve(...)"` exits immediately and kills
  the server with it.** The DuckDB CLI exits as soon as `-c` finishes;
  the server thread goes with it. For non-interactive use, always
  follow `harbor_serve(...)` with `CALL harbor_wait();` (blocks until
  `SIGTERM`/`SIGINT` or `harbor_stop`). See SPEC §2 "Daemon mode".
- **`harbor_serve(...)` is single-server-per-process.** A second call
  before `harbor_stop` throws. Don't host two harbor servers from one
  DuckDB process.
- **There is no separate "admin token".** Admin endpoints route through
  the same `harbor_authorization_function` with synthetic
  `__HARBOR_ADMIN__:<resource>:<action>` query strings (e.g.
  `__HARBOR_ADMIN__:checkpoint:create`, `__HARBOR_ADMIN__:sessions:list`,
  `__HARBOR_ADMIN__:sessions:interrupt`, `__HARBOR_ADMIN__:sessions:cancel`,
  `__HARBOR_ADMIN__:catalog:list_tables`, `__HARBOR_ADMIN__:catalog:describe_table`,
  `__HARBOR_ADMIN__:server:whoami`). See SPEC §7. If you add a new admin
  endpoint, add its `__HARBOR_ADMIN__:` resource:action to the table in
  SPEC §7. Admin endpoints **default-deny** when no custom authz
  function is configured; flip `harbor_allow_admin_without_authz` to
  override.
- **Admin path-parameter SQL must use identifier escaping.** Routes
  like `/schema/:db/:table` cannot string-interpolate path params into
  SQL. Use `KeywordHelper::WriteQuoted(name, '"')` or the equivalent
  prepared-statement parameterization. Path params come from the
  network and are user-controlled.
- **cpp-httplib worker count = 128.** Inherited from upstream Quack.
  Each keep-alive connection holds a worker for its lifetime. If you
  add long-polling routes (SSE, WebSocket), increase this or the server
  can deadlock under heavy use.
- **`BinarySerializer` compatibility version is pinned to `FromIndex(7)`.**
  Inherited from upstream Quack. Don't change without coordinating with
  upstream.
- **`atexit` is fragile.** Don't add `std::atexit` handlers for harbor
  state — DuckDB internals may already be torn down by then. The
  `HarborHttpServer` destructor runs while DuckDB is still alive and is
  the right place for cleanup.
- **The Linux x64 cpp-httplib + DuckDB struct-by-value bug from
  rip-lang/packages/db does NOT apply here.** Harbor is a C++ extension
  loaded into DuckDB's process; there's no FFI boundary. We use
  DuckDB's C++ API directly.

## When you finish a substantial change

Run all six suites before opening a PR — 198 assertions total on a
clean v0.1 `main`:

```bash
make test_release                      # 45  sqllogictest assertions
scripts/golden-cookie-auth.sh          # 18  HTTP cookie + CORS + UI proxy
scripts/golden-sql-roundtrip.sh        # 32  /sql NDJSON + sessions + params
scripts/golden-admin-roundtrip.sh      # 31  admin endpoints + default-deny
scripts/golden-query-timeout.sh        # 10  harbor_query_timeout_s
scripts/golden-sql-types.sh            # 62  per-DuckDB-type encoding
```

Also:

- Update `SPEC.md` if the change is observable to a user.
- Update `README.md` if the change is observable to a casual user.
- If you added a setting, add an assertion in `test/sql/harbor.test`
  for registration + default value.
- If you touched the wire format intentionally, document the change
  in `docs/upstream-quack-patches.md` or `docs/upstream-ui-patches.md`
  (whichever applies). Release notes live on the GitHub Releases
  page; there is no in-repo `CHANGELOG.md`.

## When in doubt

- For protocol behavior: read `SPEC.md`.
- For wire format: read upstream source in `misc/duckdb-quack/src/` or
  `misc/duckdb-ui/src/`.
- For DuckDB internals: `duckdb/src/include/duckdb/` is the C++ API
  surface; the most useful headers are `main/connection.hpp`,
  `main/database.hpp`, `common/serializer/binary_serializer.hpp`,
  `common/types/data_chunk.hpp`.
- For cpp-httplib: it's vendored under `duckdb/third_party/httplib/`
  (and exposed through `duckdb_httplib::`).

For everything else, ask. Don't guess on protocol or auth; both are
load-bearing.
