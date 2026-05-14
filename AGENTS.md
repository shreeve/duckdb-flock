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
| **PR-4** (current) | `src/flock_crypto.{cpp,hpp}` wraps OpenSSL `libcrypto` for SHA-256 (`principal_id` derivation per SPEC §6), HMAC-SHA256 (`flock_session` cookie signing per SPEC §7), and CSPRNG (`RAND_bytes` for cookie keys + session IDs). HMAC-signed cookie issuance via `POST /auth/login`; `POST /auth/logout` clears it. UiHandlers' Origin-set check supplemented with cookie-aware auth on `/ddb/*`. flock-specific login wrapper at `GET /` (registered before UiHandlers' GET /.* catch-all). `flock_cors_origins` allow-list replaces the wildcard `Access-Control-Allow-Origin: *` on `/quack` and `/info`. | `/sql` (PR-5). Admin handlers (PR-6). `bundled` UI assets mode. |
| **PR-5** | `/sql` endpoint with `SqlHandlers` per SPEC §5.2–5.4. NDJSON streaming. Param decoding + type-encoding round trip. | Admin handlers. |
| **PR-6** | Admin handlers (`/whoami`, `/tables`, `/schema/:db/:t`, `/checkpoint`, `/sessions`, `/interrupt`) per SPEC §4. `__FLOCK_ADMIN__:resource:action` authz integration. | |
| **PR-7+** | Hardening, full CI matrix (`osx_arm64`, `osx_amd64`, `linux_amd64`, `linux_arm64`, `windows_amd64`), golden tests, doc polish, distribution. | |

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
| `src/flock_auth.{cpp,hpp}` | `AuthManager` — token + hook resolution, HMAC cookie sign/verify, `/auth/login` and `/auth/logout` handlers | Yes |
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
