# AI Agent Guide for duckdb-flock

**Purpose:** This document orients AI assistants (and human contributors)
to the `flock` DuckDB extension codebase. The authoritative design
document is [`SPEC.md`](./SPEC.md); the user-facing introduction is
[`README.md`](./README.md). This file is the contributor's map.

## What is flock

flock is a **single DuckDB extension** that runs **one HTTP server on
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

## Implementation roadmap

flock is being implemented in staged PRs. **Do not jump ahead** unless
the PR explicitly says so. The current state of `src/` does not yet
match the target architecture described in SPEC.md §2 — it gets there
incrementally.

| PR | Scope | Explicitly excluded |
|----|---|---|
| **PR-1** | Vendor `duckdb-quack` source verbatim into `src/quack/`; rename build identifiers (`EXT_NAME`, `TARGET_NAME`, extension class) so the loadable extension is `flock.duckdb_extension`. Inherit upstream's `vcpkg.json` (openssl + curl). Keep upstream's `quack_serve`/`quack_stop`/`quack_check_token`/all settings intact. Add `flock_version()` scalar. `/quack` works against stock quack clients on day one. | Renaming SQL functions/settings to `flock_*`. `flock_serve`/`stop`/`wait` (semantics differ from `quack_serve`). Touching wire format. UI, `/sql`, admin. Architectural refactor. |
| **PR-1.5** | Enable `LOAD_TESTS` so `test/sql/flock.test` actually runs in CI. Extend the smoke test with a `/quack` runtime roundtrip block (con1 hosts via `quack_serve`, con2 attaches via `quack_query`, transactions/secrets/auth-failure all exercised, clean `quack_stop`). Document the five surgical edits to vendored `quack_extension.cpp` in [`docs/upstream-quack-patches.md`](./docs/upstream-quack-patches.md). Add the roundtrip-passes-after-refactor item to the PR-2 acceptance checklist below. | Anything that changes wire format or production behavior. PR-1.5 is purely test/doc infrastructure that PR-2's refactor can be measured against. |
| **PR-2** | Architectural refactor: extract `httplib::Server` from `QuackServer` into a new `FlockHttpServer`; rename `QuackServer` → `QuackHandlers`; extract `SessionManager` + `AuthManager` as standalone subsystems. Add `/health` and `/info` routes registered against the shared server. Introduce `flock_serve` / `flock_stop` / `flock_wait` with SPEC §9 semantics; keep `quack_*` as functional aliases. Quack roundtrip test passes throughout. CI grep guard active. | UI, `/sql`, admin handlers. |
| **PR-3** | Port `duckdb-ui` source as `UiHandlers` registered against the shared server. Migrated `FlockHttpServer` from `duckdb_httplib::Server` (plain) to `duckdb_httplib_openssl::Server` (one namespace throughout — UI handlers can register on the shared server cleanly). UI assets via `proxy` mode (forward `GET /.*` to `ui.duckdb.org` over OpenSSL-backed cpp-httplib HTTPS client — `bundled` mode is post-v0.1, see SPEC §14). Auth: upstream UI's same-Origin check (allowed Origins = loopback variants + bind host) — flock cookie auth deferred to PR-4. AdminHandlers `/info` extended with `X-DuckDB-UI-Extension-Version`. `FlockHttpServer::ShutdownHandlers()` added so long-running threads (UiHandlers' Watcher, EventDispatcher's SSE consumers) get released BEFORE the active-request drain. SSE handler captures `shared_ptr<ActiveRequestGuard>` in the chunked content provider closure. CI grep guard updated to match both namespaces. | `/sql`, admin. `bundled` UI assets mode. Cookie auth + flock_crypto + login wrapper (PR-4). |
| **PR-4** (current) | `src/flock_crypto.{cpp,hpp}` wraps OpenSSL `libcrypto` for SHA-256 (`principal_id = hex(sha256(token))` per SPEC §6), HMAC-SHA256 (`flock_session` cookie signing per SPEC §7), CSPRNG (`RAND_bytes` for ephemeral signing key + 16-byte cookie nonce), and base64url. New `AuthHandlers` registers `POST /auth/login` (synthetic sid `__FLOCK_AUTH__:login`), `POST /auth/logout`, `OPTIONS /auth/*`, `OPTIONS /quack`. AuthManager extended with `AuthenticateRequest()` returning `{ok, principal_id, source, error_code}` (precedence: `Authorization: Bearer` → `X-Flock-Token` → `Cookie` — explicit-bad bearer never falls back to cookie). UiHandlers' Origin-set check supplemented with cookie-aware auth on `/ddb/run`, `/ddb/tokenize`, `/ddb/interrupt`, AND `/localEvents`. The UI catch-all (`GET /.*`) is now cookie-gated: no valid cookie → minimal HTML login page; valid cookie → proxy through to `ui.duckdb.org` (Option B per round-11 review — single code path owns "serve UI asset"). UI connection pool keyed on `(principal_id, X-DuckDB-UI-Connection-Name)` so user-controlled connection names cannot collide across principals (round-11 blocker fix). Local-dev bypass uses fixed principal `__local_dev__` so the principal-scoped invariant holds even with `flock_local_dev_mode=true`. `flock_cors_origins` allow-list replaces the wildcard `Access-Control-Allow-Origin: *` on `/info`; `OPTIONS` preflight wired for `/quack` and `/auth/*`; `flock_serve` refuses to start if `flock_cors_origins='*'`. **Cookie signing key is ephemeral random per process** (NOT a SQL setting — closes the SQL-readable-secret leak; see SPEC §7 + §15 question 2). Settings registered: `flock_auth_cookie_ttl_s`, `flock_cors_origins`, `flock_local_dev_mode`. | `/sql` (PR-5). Admin handlers (PR-6). `bundled` UI assets mode. `(principal, ui_connection_name) → db_session_id` map in SessionManager (PR-5 once /sql lands and SessionManager genuinely needs principal scope; PR-4 only scopes UiHandlers' connection-name pool). `?destroy_sessions=true` on `/auth/logout` (logged-but-ignored in PR-4). `flock_cookie_signing_key` SQL setting (deferred to v0.2 as `FLOCK_COOKIE_SIGNING_KEY` env var). |
| **PR-5** | `/sql` endpoint with `SqlHandlers` per SPEC §5.2–5.4. NDJSON streaming. Param decoding + type-encoding round trip. | Admin handlers. |
| **PR-6** | Admin handlers (`/whoami`, `/tables`, `/schema/:db/:t`, `/checkpoint`, `/sessions`, `/interrupt`) per SPEC §4. `__FLOCK_ADMIN__:resource:action` authz integration. | |
| **PR-7+** | Hardening, full CI matrix (`osx_arm64`, `osx_amd64`, `linux_amd64`, `linux_arm64`, `windows_amd64`), golden tests, doc polish, distribution. | |
| ~~**PR-10b**~~ | **EVALUATED AND DECLINED** for v0.1 — see "PR-10b: declined" below. The OpenSSL/cpp-httplib architectural cleanup was originally planned post-PR-7. After the round-13/round-14 architectural review reduced its scope (vcpkg deps stay; the only concrete win is dropping flock's *direct* libssl/libcrypto link), the cost-benefit no longer justified the migration risk. May be revisited under specific trigger conditions; see the dedicated section below. | n/a |

When changing code, **keep docs and tests consistent with the current
implementation status**. Don't promise anything in a release that PR-N
hasn't actually delivered.

### Architecture as of PR-2

The PR-2 refactor dissolved upstream quack's `QuackServer` /
`HttpQuackServer` hierarchy and brought flock's target architecture
online. The current state of `src/`:

- `src/include/flock_http_server.hpp` + `src/flock_http_server.cpp`
  own the only `duckdb_httplib::Server` in the process. Enforced by
  the `Single duckdb_httplib::Server owner` check in
  `.github/workflows/architecture-guard.yml`.
- `src/include/flock_session.hpp` + `src/flock_session.cpp` hold the
  per-process session pool (`SessionManager` + `FlockSession`),
  shared by `QuackHandlers` (today) and future UI/SQL handlers.
- `src/include/flock_auth.hpp` + `src/flock_auth.cpp` hold the auth
  callback resolution (`AuthManager`). Cookie/principal model
  arrives in PR-3.
- `src/quack/quack_server.{cpp,hpp}` is now a stateless `QuackHandlers`
  class that registers `/quack` and `OPTIONS /quack` routes against
  the shared server. It borrows references to SessionManager + AuthManager
  via its ctor.
- `flock_serve` / `flock_stop` / `flock_wait` are the SPEC §9
  lifecycle SQL functions. `quack_serve` / `quack_stop` are kept as
  thin shims that delegate to the same `FlockServerState::Global()`,
  so stock-quack tooling continues to work.
- `/health` and `/info` are served by `AdminHandlers`. The full admin
  surface (`/whoami`, `/tables`, `/checkpoint`, etc.) lands in PR-5.

Wire format on `/quack` is byte-identical to upstream Quack — the
`/quack` roundtrip block in `test/sql/flock.test` (introduced PR-1.5)
verifies this on every CI run.

### PR-2 acceptance checklist (closed — kept here as architectural reference)

PR-2 was the architectural refactor and the highest-risk PR in the
sequence. All acceptance criteria were satisfied at merge time and
remain load-bearing invariants going forward:

- [x] Exactly one `duckdb_httplib::Server` instance exists in the
      process. It is owned by `FlockHttpServer`. `QuackHandlers` no
      longer owns or constructs an httplib server.
- [x] No `QuackServer::listen` / `QuackServer::run` / equivalent.
      Listening lifecycle lives entirely in `FlockHttpServer`
      (the entire `QuackServer` base class and `HttpQuackServer`
      derived class were deleted; `src/quack/quack_http_server.cpp`
      is gone from the tree).
- [x] `/health`, `/info`, and `/quack` are served by the same
      `FlockHttpServer` instance on the same listening socket.
- [x] The PR-1.5 `/quack` runtime roundtrip in `test/sql/flock.test`
      still passes unchanged (auth happy + failure paths,
      multi-statement transactions, secret-based auth, large-result
      FETCH chunking, idempotent `quack_stop`, post-stop IO error).
      This proves wire format and lifecycle survived the refactor.
- [x] CI grep guard active in `.github/workflows/architecture-guard.yml`:
      `grep -REn '(make_uniq|unique_ptr|shared_ptr)<\s*duckdb_httplib::Server\s*>' src`
      must produce no matches outside `src/{include/,}flock_http_server.*`
      (catches future regressions where another file accidentally
      reintroduces server ownership).
- [x] `SessionManager` and `AuthManager` are standalone classes
      constructed by `FlockHttpServer` and passed by reference to
      `QuackHandlers`. No global session state lives inside
      `QuackHandlers`.
- [x] `flock_serve`, `flock_stop`, `flock_wait` exist with SPEC §9
      semantics (single-server-per-process; generation-counter `Wait()`
      for restart races). `quack_serve`, `quack_stop` (and the rest of
      the `quack_*` functions/settings) remain as functional aliases
      delegating to the same `FlockServerState::Global()`.
- [x] AGENTS.md "Implementation roadmap" updated to reflect PR-2 done
      and PR-3 next.

Two follow-up TODOs were captured in code as comments at PR-2 merge:

1. **Listener thread exception observability**
   (`src/flock_http_server.cpp::ListenThreadMain`) — listen exceptions
   are silently swallowed. PR-3+ should route through the `Flock` log
   type with the exception string.
2. **CORS wildcard on `/quack`** (`src/quack/quack_server.cpp::Register`)
   — `Access-Control-Allow-Origin: *` is fine for `/quack`-only today
   (no cookies flow through `/quack`); PR-3 must replace with the
   configured `flock_cors_origins` allow-list when cookie auth arrives.
   **Resolved in PR-4** — `OPTIONS /quack` is owned by `AuthHandlers`
   with the allow-list, and `POST /quack` echoes the matching origin
   only when the request `Origin` is in `flock_cors_origins`.

### PR-4 acceptance closure (closed)

PR-4 added cookie auth to the UI surface, the `flock_crypto` libcrypto
wrapper, and the `flock_cors_origins` allow-list. Round-11 GPT-5.5
review surfaced two security improvements over the SPEC's earlier
draft, both of which were folded in before code landed (see SPEC §7
+ §15 question 2 for the rationale). All acceptance criteria green at
merge:

- [x] `src/flock_crypto.{cpp,hpp}` implements SHA-256, HMAC-SHA256,
      RAND_bytes, base64url, and constant-time-equal as a thin layer
      over OpenSSL `libcrypto` (already linked via PR-3).
- [x] `flock_session` cookie format is
      `v1.<b64url(principal)>.<b64url(expires_unix)>.<b64url(nonce16)>.<b64url(hmac32)>`.
      HMAC is over the exact ASCII bytes of `v1.<seg1>.<seg2>.<seg3>`
      so verification recomputes over the on-the-wire prefix. Constant-
      time MAC compare via `CRYPTO_memcmp`.
- [x] Cookie signing key is **ephemeral random per process** (32 bytes
      from `RAND_bytes`, lazy-init under mutex on first use). NO
      `flock_cookie_signing_key` SQL setting in v0.1 (security review:
      exposing the HMAC secret to authorized SQL would let any SQL
      caller mint cookies). v0.2 reintroduces operator control via
      the `FLOCK_COOKIE_SIGNING_KEY` environment variable.
- [x] `AuthManager::AuthenticateRequest` parses request credentials in
      precedence order Bearer → X-Flock-Token → Cookie. Bad bearer
      NEVER falls back to cookie (round-11 review: explicit creds
      should not be masked by ambient browser state).
- [x] `AuthHandlers` owns `POST /auth/login` (synthetic sid
      `__FLOCK_AUTH__:login`), `POST /auth/logout`, `OPTIONS /auth/*`,
      and `OPTIONS /quack`. Login accepts JSON body, Bearer header,
      or X-Flock-Token. Sets `Set-Cookie flock_session=v1...; HttpOnly;
      SameSite=Strict; Path=/; Max-Age=<ttl>` plus `Secure` when
      `X-Forwarded-Proto: https`. Logout always returns 200 (never
      reveals whether the caller had a valid cookie).
- [x] UI catch-all `GET /.*` is cookie-gated (Option B per round-11
      review — single code path owns "serve UI asset", with cookie
      check inline). No cookie + `GET /` → minimal flock login page
      (~70 lines inline HTML/CSS/JS, no external deps). No cookie +
      any other `GET` → 401. Valid cookie or `flock_local_dev_mode` →
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
      `sha256("__FLOCK_LOCAL_DEV__")` so the principal-scoped invariant
      holds even with `flock_local_dev_mode=true`.
- [x] `flock_cors_origins` allow-list replaces wildcard CORS on `/info`
      and `/quack`. Each entry must be a well-formed
      `scheme://host[:port]` (no path/query/fragment/trailing slash).
      `flock_serve` **refuses to start** if the setting is `'*'` or
      contains a malformed entry — the SQL `CALL flock_serve(...)`
      throws and the server never binds.
- [x] OPTIONS preflight on `/quack`, `/auth/login`, `/auth/logout`
      emits `Access-Control-Allow-Origin: <exact-match>` only when
      the request Origin is in the allow-list. Bare 204 with no CORS
      headers when not (browser blocks). Allowed headers per SPEC §7.
- [x] Three new settings registered in `quack_extension.cpp::LoadInternal`:
      `flock_auth_cookie_ttl_s` UBIGINT default 43200,
      `flock_cors_origins` VARCHAR default `''`,
      `flock_local_dev_mode` BOOLEAN default `false`.
- [x] `test/sql/flock.test` extended (22 → 40 assertions) with
      PR-4-specific coverage: setting registration, defaults, refuse-
      to-start scenarios for `flock_cors_origins='*'`, malformed
      origin (path), missing scheme, and non-http(s) scheme.
- [x] HTTP-level cookie roundtrip tested by
      `scripts/golden-cookie-auth.sh` (10 assertions covering /info
      no-Origin / allowed-Origin / disallowed-Origin, /auth/login
      valid/invalid/Bearer, /auth/logout cookie-clear, GET / login
      page, GET /random 401, /quack still served).
- [x] PR-1.5 `/quack` runtime roundtrip in `test/sql/flock.test`
      passes byte-for-byte after the AuthHandlers wrapping (40
      assertions total, all green; quack wire compat preserved).
- [x] All 7 CI checks green.

Two follow-up TODOs captured in code at PR-4 merge:

1. **Default-deny for unknown Authentication scheme.** AuthManager's
   try_explicit_token requires `Authorization: Bearer ` prefix; an
   `Authorization: Basic ...` header is treated as missing-credential
   and falls through to cookie/X-Flock-Token. Acceptable for v0.1 (we
   only document Bearer) but should explicitly reject other schemes
   in PR-5+ to avoid accidental "Basic" passthrough on misconfigured
   reverse proxies.
2. **Login page CSP + nonce.** The inline `<script>` runs without a
   `Content-Security-Policy` header, which means a future XSS in any
   future flock-served page could inject script. The login page itself
   has no XSS surface (no untrusted strings interpolated), but PR-5+
   should add CSP `default-src 'self'; script-src 'nonce-<random>';`
   when the SQL endpoint adds error pages with potentially-tainted
   strings.

### PR-8 acceptance closure (closed)

PR-8 was a security regression fix on the PR-4 cookie auth, surfaced
by GPT-5.5 round-13 architectural review. The pre-PR-4 `HandleProxyGet`
forwarded the entire browser `Cookie` header to `ui.duckdb.org` so
MotherDuck's domain cookies could pass through. PR-4 introduced our
own `flock_session=v1.<principal_hex>...` cookie under flock's origin;
the browser sends it on every request to flock, including
`/assets/*`, and the old passthrough forwarded it to `ui.duckdb.org`
— leaking flock auth material to a third-party origin.

- [x] `HandleProxyGet` rewritten with strict allow-list: forwards
      only `Accept`, `Accept-Encoding`, `Accept-Language`,
      `If-None-Match`, `If-Modified-Since`, `Range`. Never
      `Cookie`, `Authorization`, `X-Flock-Token`, `X-Flock-Session-Id`,
      `Origin`, or `Sec-*` headers.
- [x] `scripts/golden-cookie-auth.sh` extended (11 → 14 assertions).
      New fixture: tiny inline Python listener acting as a fake
      `ui.duckdb.org` that captures whatever flock proxies upstream.
      Three scenarios cover all three auth credential types
      (Cookie, Bearer, X-Flock-Token); each asserts the credential
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
narrow claim about flock's direct source code (no
`#include <curl/...>` in `src/`) — true, but consistent with "flock
uses libcurl via `HTTPUtil` at runtime". Two independent reasons
keep `curl` in `vcpkg.json`:

**Analytical (the one I should have caught before attempting PR-9):**
flock auto-loads `httpfs` at runtime in `src/quack/quack_client.cpp`
(`ExtensionHelper::AutoLoadExtension(db, "httpfs")` at line 127) and
then uses `HTTPUtil::Get(db)` at line 34 for the outbound HTTPS path
of `ATTACH 'quack:host'`. `HTTPUtil` is DuckDB's libcurl-backed HTTP
client (provided by httpfs). So flock USES libcurl at runtime,
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
so flock's tests (specifically the PR-1.5 `/quack` runtime roundtrip
in `test/sql/flock.test`, which exercises auto-loading httpfs) work
end-to-end in CI. httpfs's own source needs `libcurl` for HTTP and
`libssl`/`libcrypto` for HTTPS / S3 signing. So `vcpkg.json`'s
`["openssl", "curl"]` list is **not dead weight** — both are required
by the bundled httpfs build, AND we use both at runtime.

### PR-10b: declined — keep httplib + OpenSSL

Originally planned post-PR-7: rewrite `HandleProxyGet` to
`HTTPUtil`/libcurl, rewrite `flock_crypto.cpp` to wrap
`MbedTlsWrapper::ComputeSha256Hash` + `Hmac256`, migrate cpp-httplib
namespace `duckdb_httplib_openssl::` → plain `duckdb_httplib::`,
drop flock's direct `find_package(OpenSSL)` + `target_link_libraries`,
add a bounded asset cache + `flock_crypto_selftest()` smoke test
function.

After the round-14 GPT-5.5 review reduced scope (vcpkg deps stay
regardless; httpfs is a mandatory runtime dep through which OpenSSL
arrives anyway), this was **evaluated and declined for v0.1**.

Cost/benefit at the actual scope:

| Save | Concrete impact |
|---|---|
| Binary size | ~200 KB – 1 MB per platform binary; 0.6%–3% of the current 34 MB. One-time per user download. |
| Direct OpenSSL link from flock | flock_extension stops linking libssl/libcrypto; operationally the same — OpenSSL still in-process via httpfs. |
| One cpp-httplib namespace | Cleaner; marginal. |
| `flock_crypto.cpp` shorter | ~10 LOC mbedTLS wrappers vs ~50 LOC OpenSSL ceremony. But the existing OpenSSL version IS written, working, and tested. |

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
OpenSSL libcrypto in `flock_crypto.cpp` + httpfs at runtime for
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
  successfully against a flock server. The roundtrip block in
  `test/sql/flock.test` (introduced PR-1.5) is the current regression
  spec; full byte-level golden tests in `test/golden/quack/` will land
  later. Anytime you touch vendored quack code (per
  [`docs/upstream-quack-patches.md`](./docs/upstream-quack-patches.md))
  or PR-2's refactor: rerun the roundtrip block locally and in CI.
- **Never break UI wire compatibility.** The official DuckDB UI built
  against the pinned `duckdb-ui` commit must work against `GET /` and
  `POST /ddb/*` byte-for-byte. Golden tests in `test/golden/ui/`
  enforce this.
- **Route order matters.** cpp-httplib resolves routes in registration
  order. The `GET /.*` catch-all (UI assets) **must** be registered
  last. Specific routes (`/info`, `/health`, `/sql`, `/quack`, `/ddb/*`,
  admin) **must** be registered before it.
- **Run tests before every commit.** `make test` must pass.
- **Run `scripts/golden-quack-roundtrip.sh` and
  `scripts/golden-ui-roundtrip.sh` after touching any handler or
  encoder.** They catch wire-format drift early.

## Compilation pipeline

```text
src/*.cpp ──► CMake ──► build/release/extension/flock/flock.duckdb_extension
                            │
                            ├── statically links DuckDB                (via duckdb/ submodule)
                            ├── statically links cpp-httplib           (vendored from duckdb-quack)
                            └── embeds bundled UI assets               (from ui_assets_data.cpp,
                                                                       generated by scripts/fetch-ui-assets.sh)
```

The extension is a single `.duckdb_extension` file. Distribution is via
DuckDB's community extension repo (planned) and GitHub Releases (today).

## Repository layout

| Path | Role | Edit? |
|---|---|---|
| `SPEC.md` | Authoritative design | Carefully — major changes need consensus |
| `README.md` | User-facing introduction | Yes |
| `AGENTS.md` | This file | Yes |
| `LICENSE` | MIT, dual copyright (DuckDB Foundation + flock authors) | Never silently |
| `CMakeLists.txt`, `Makefile`, `vcpkg.json`, `extension_config.cmake` | Build configuration | Carefully |
| `duckdb/` | Submodule, pinned at v1.5.2 | Never |
| `extension-ci-tools/` | Submodule | Never |
| `src/flock_extension.cpp` | Entry point: registers settings, scalar functions, `flock_serve`/`flock_stop`/`flock_wait` table macros | Yes |
| `src/flock_http_server.{cpp,hpp}` | `FlockHttpServer` — owns the cpp-httplib `Server`, holds shared state | Yes |
| `src/flock_session.{cpp,hpp}` | `SessionManager` — per-session DuckDB `Connection` pool with mutex | Yes |
| `src/flock_auth.{cpp,hpp}` | `AuthManager` — token + hook resolution, HMAC cookie sign/verify (PR-4), CORS allow-list parsing (PR-4) | Yes |
| `src/flock_crypto.{cpp,hpp}` | OpenSSL libcrypto wrapper: SHA-256, HMAC-SHA256, RAND_bytes, base64url, constant-time-equal (PR-4) | Yes |
| `src/auth_handlers.{cpp,hpp}` | `AuthHandlers` — `POST /auth/login`, `POST /auth/logout`, `OPTIONS /auth/*`, `OPTIONS /quack` (PR-4) | Yes |
| `src/flock_log.{cpp,hpp}` | `'Flock'` and `'HTTP'` log type registration | Yes |
| `src/flock_wait.{cpp,hpp}` | Blocking `flock_wait()` table function — keeps the DuckDB process alive in container/daemon mode | Yes |
| `src/quack/quack_handlers.{cpp,hpp}` | `/quack` route registration + dispatch | Yes (was `QuackServer` upstream) |
| `src/quack/quack_message.{cpp,hpp}` | Wire format — match upstream | **Never** unless tracking upstream change |
| `src/quack/quack_scan.{cpp,hpp}`, `src/quack/storage/*` | Client side (`ATTACH 'flock:host'`), unchanged from upstream Quack | Carefully |
| `src/sql/sql_handlers.{cpp,hpp}` | `POST /sql` route, request parsing, NDJSON streaming | Yes |
| `src/sql/sql_chunk_encoder.{cpp,hpp}` | `DataChunk` → NDJSON per SPEC §5.4 | Yes |
| `src/ui/ui_handlers.{cpp,hpp}` | `/ddb/*`, `/info`, `/localEvents`, `/localToken` route registration | Yes (was `ui::HttpServer` upstream) |
| `src/ui/ui_assets.{cpp,hpp}` | `GET /.*` from bundled assets (when `flock_ui_assets='bundled'`) | Yes |
| `src/ui/ui_assets_data.cpp` | Generated const byte array of UI assets | **Never edit by hand** — regenerate via `scripts/fetch-ui-assets.sh` |
| `src/admin/admin_handlers.{cpp,hpp}` | `/health`, `/tables`, `/schema`, `/whoami`, `/checkpoint`, `/sessions`, `/interrupt` | Yes |
| `scripts/fetch-ui-assets.sh` | Mirror `ui.duckdb.org` → `ui_assets_data.cpp` + `UI_ASSETS_VERSION.txt` | Yes |
| `scripts/golden-quack-roundtrip.sh` | Regression: stock Quack client ↔ flock | Yes |
| `scripts/golden-ui-roundtrip.sh` | Regression: official UI ↔ flock | Yes |
| `scripts/golden-cookie-auth.sh` | Regression (PR-4): HTTP cookie + CORS roundtrip — `/auth/login`, `/auth/logout`, login page, `/info` allow-list | Yes |
| `test/unit/` | C++ unit tests for encoders, session manager, auth resolution | Yes |
| `test/integration/` | End-to-end: spin a flock, hit it, assert | Yes |
| `test/golden/quack/`, `test/golden/ui/` | Captured wire-format fixtures | Carefully — regenerate from real clients |
| `test/types/` | Per-DuckDB-type `/sql` NDJSON round-trip | Yes |
| `docs/ERRORS.md`, `docs/REVERSE_PROXY.md`, `docs/ROADMAP.md`, `docs/DEPLOY_INCUS_ZFS.md` | User docs | Yes |
| `misc/` | Read-only clones of upstream repos for reference | **Never edit** — regenerate by re-cloning |

## Reading upstream

`misc/` holds untracked clones of the upstream projects flock derives
from. They are not part of the build. Use them as reference when:

- Tracking what upstream changed since our last rebase
- Diagnosing whether a wire-format mismatch is a bug in flock or a real
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
FlockHttpServer (owns httplib::Server, listener thread, all subsystems)
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
make test                                   # runs unit + integration suites
make clean
```

The output is at `build/release/extension/flock/flock.duckdb_extension`.

### Smoke test against a live extension

> **Daemon-mode pitfall.** `duckdb -c '…'` exits as soon as the last
> statement returns, which tears down the flock server before you can
> hit it from another terminal. Always include `CALL flock_wait();` at
> the end of any non-interactive invocation. The test harness also
> accepts an `&` background form for shell loops — see below.

Interactive (REPL stays open, server runs in background):

```bash
make release
duckdb -unsigned   # opens the REPL
LOAD '/abs/path/build/release/extension/flock/flock.duckdb_extension';
CALL flock_serve('flock:127.0.0.1:9494');
-- REPL is still yours; server is alive in a background thread
```

Non-interactive (script-driven smoke test):

```bash
make release
duckdb -unsigned -no-stdin -c "
  LOAD '$PWD/build/release/extension/flock/flock.duckdb_extension';
  CALL flock_serve('flock:127.0.0.1:9494');
  CALL flock_wait();          -- blocks until SIGTERM/SIGINT
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

1. Add to `src/flock_extension.cpp` in the settings block.
2. Document in `SPEC.md` §9.
3. Add a test in `test/integration/`.

### Add a new endpoint

1. Pick the right handler subsystem (`Sql`, `Ui`, `Admin`, `Quack`).
2. Add route registration in that subsystem's `Register()`.
3. Verify it does not collide with `GET /.*` (which must remain last).
4. Document in `SPEC.md` §4 and §5.
5. Add an integration test.

### Update the bundled UI assets

```bash
scripts/fetch-ui-assets.sh
# Verify version pin
cat UI_ASSETS_VERSION.txt
# Rebuild and run golden UI tests
make release
scripts/golden-ui-roundtrip.sh
```

### Rebase against upstream Quack

```bash
( cd misc/duckdb-quack && git fetch && git checkout v1.5-variegata && git pull )
# Diff against the version we vendored into src/quack/
diff -ru misc/duckdb-quack/src/ src/quack/
# For each non-trivial change, decide: absorb, defer, or skip
# Update src/quack/ accordingly, preserve dual copyright headers
make test
scripts/golden-quack-roundtrip.sh
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
| `/sql` NDJSON schema line is the authority for type decoding | Types like `BIGINT` arrive as strings; clients must consult schema |
| `BIGINT`, `HUGEINT`, `UBIGINT`, `UHUGEINT` are encoded as JSON strings in `/sql` | JS precision; SPEC §5.4 |
| `MAP<K,V>` is encoded as array-of-pairs, not object | Keys can be non-string; ordering matters |
| `JSON` column values are encoded as JSON-text strings, not nested JSON | Disambiguates SQL NULL from JSON null |
| `INTERVAL` is encoded as `{months, days, micros}` object with micros as string | Matches DuckDB's `interval_t` exactly |
| `BLOB` is base64 | Standard |

## Auth model invariants

| Invariant | Why |
|---|---|
| Browser-origin requests do **NOT** bypass token auth | Origin is CSRF defence, not authentication. SPEC §7. |
| `flock_authentication_function` is called on every Quack `CONNECTION_REQUEST`, every `/sql` first request that creates an ephemeral session, every `/ddb/run` first request, and on every `POST /sql/sessions` | Identity is established once per session |
| `flock_authorization_function` is called on every SQL-bearing request: Quack `PREPARE_REQUEST`, Quack `APPEND_REQUEST`, `/sql`, `/ddb/run`, AND on every admin endpoint with a synthetic `__FLOCK_ADMIN__:<resource>:<action>` query | Authorization is per-statement; admin authz is uniform with regular authz. Resource/action pairs (not bare verbs) so policies can grant `sessions:list` without granting `sessions:interrupt`. |
| Admin endpoints are **default-deny** when `flock_authorization_function = flock_nop_authorization` (the permissive default), unless `flock_allow_admin_without_authz = true` is explicitly set | Operators who set a token but forget to write a custom authz function get safe-by-default admin |
| Path parameters and request-body fields are **never** interpolated into the synthetic `__FLOCK_ADMIN__:` strings | Concrete identifiers go in the request envelope, not the policy decision input. Stops authz-string injection. |
| `principal_id = hex(sha256(client_token))` | Auth hook returns BOOLEAN only; identity comes from credential. Same token → same principal everywhere. Logs use first 8 hex chars only. |
| Auth principal cookie (`flock_session=<HMAC-signed>`) and DB session id are **distinct concepts** | A principal can own many DB sessions; SPEC §6 |
| Every DB session lookup verifies `owner_principal` matches the caller | Stops sessionId-guessing attacks |
| `/localToken` is **disabled** when `flock_bind ≠ 127.0.0.1` | MotherDuck token disclosure on remote-bound servers is a privilege escalation |
| `flock_local_dev_mode` is **forced off** when `flock_bind ≠ 127.0.0.1` | Belt-and-suspenders against accidental exposure |
| Auth callbacks run in a fresh transient connection per call | They cannot rely on session-local state — see SPEC §7 |

## Concurrency invariants

| Invariant | Why |
|---|---|
| One `Connection` per session, full request held under session mutex | DuckDB `Connection` is not concurrent-safe across queries |
| Concurrent requests for the same `sessionId` → HTTP 409 `SESSION_BUSY` | Cleaner contract than serializing implicitly |
| `Connection::Interrupt()` is invoked on client disconnect during `/sql` streaming | Frees the worker thread |
| Idle sessions swept on a 60s tick | Bounds resource use without a dedicated reaper |

## Logging

```sql
CALL enable_logging('Flock');
-- ... run some queries ...
SELECT * FROM duckdb_logs_parsed('Flock');

-- Persist to disk
CALL enable_logging('Flock', storage => 'file',
                    storage_config => {'path':'/var/log/flock'});
```

The `'Quack'` log name is registered as an alias for the upstream
tooling that filters on it.

## Known gotchas

- **`SET flock_authentication_function = '...'` must be `SET GLOBAL`.**
  The auth path runs in fresh worker connections that don't see
  session-local settings. A plain `SET` silently has no effect on
  authentication.
- **`RESET` does not undo a `SET GLOBAL`** for the same reason. Use
  `RESET GLOBAL flock_authentication_function`.
- **`GET /.*` catch-all must be registered last.** Otherwise it shadows
  every other GET route. Add new routes BEFORE the catch-all in
  `FlockHttpServer::RegisterAll()`.
- **`duckdb -c "CALL flock_serve(...)"` exits immediately and kills
  the server with it.** The DuckDB CLI exits as soon as `-c` finishes;
  the server thread goes with it. For non-interactive use, always
  follow `flock_serve(...)` with `CALL flock_wait();` (blocks until
  `SIGTERM`/`SIGINT` or `flock_stop`). See SPEC §2 "Daemon mode".
- **`flock_serve(...)` is single-server-per-process.** A second call
  before `flock_stop` throws. Don't host two flock servers from one
  DuckDB process.
- **There is no separate "admin token".** Admin endpoints route through
  the same `flock_authorization_function` with synthetic
  `__FLOCK_ADMIN__:<resource>:<action>` query strings (e.g.
  `__FLOCK_ADMIN__:checkpoint:create`, `__FLOCK_ADMIN__:sessions:list`,
  `__FLOCK_ADMIN__:sessions:interrupt`, `__FLOCK_ADMIN__:sessions:cancel`,
  `__FLOCK_ADMIN__:catalog:list_tables`, `__FLOCK_ADMIN__:catalog:describe_table`,
  `__FLOCK_ADMIN__:server:whoami`). See SPEC §7. If you add a new admin
  endpoint, add its `__FLOCK_ADMIN__:` resource:action to the table in
  SPEC §7. Admin endpoints **default-deny** when no custom authz
  function is configured; flip `flock_allow_admin_without_authz` to
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
- **`atexit` is fragile.** Don't add `std::atexit` handlers for flock
  state — DuckDB internals may already be torn down by then. The
  `FlockHttpServer` destructor runs while DuckDB is still alive and is
  the right place for cleanup.
- **The Linux x64 cpp-httplib + DuckDB struct-by-value bug from
  rip-lang/packages/db does NOT apply here.** Flock is a C++ extension
  loaded into DuckDB's process; there's no FFI boundary. We use
  DuckDB's C++ API directly.

## When you finish a substantial change

1. `make test` — must pass.
2. `scripts/golden-quack-roundtrip.sh` — must pass.
3. `scripts/golden-ui-roundtrip.sh` — must pass (only if you touched
   `/ddb/*` or UI assets).
4. Update SPEC.md if the change is observable to a user.
5. Update README.md if the change is observable to a casual user.
6. If you added a setting, add a test in `test/integration/` that
   exercises it.
7. If you touched the wire format intentionally, regenerate the golden
   fixtures and document the change in `BUILD.md`.

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
