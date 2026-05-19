# Changelog

All notable changes to the `harbor` DuckDB extension are documented
here. Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versioning follows [SemVer](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Documentation

- **`docs/WHY_HARBOR.md`** — new positioning doc answering the natural
  evaluator question "isn't this just the Quack extension + an
  httpserver extension + the DuckDB UI extension?" Covers what each
  input gives you in stock form, the 12 things harbor adds beyond
  the sum (architectural foundation / data plane / policy plane /
  wire compat / operations), and what harbor explicitly is *not*.
  Cross-linked from `README.md` (after "What It Does"), `AGENTS.md`
  (after the protocol table), and `SPEC.md` §2 (Architecture).

## [0.1.3] — 2026-05-19

### Documentation

- **Threat model documentation** — three-place treatment cross-linked
  for different audiences:
  - `SPEC.md` §7 "Threat model" expanded with an explicit table of
    "what harbor protects against vs. what it does NOT", calling
    out that bearer-authenticated `/sql` is effectively superuser
    unless `harbor_authorization_function` constrains the SQL,
    and that `harbor_allow_admin_without_authz`'s default-deny is a
    usability convenience (not a security wall) that an authenticated
    bearer can flip via `SET GLOBAL` from `/sql` itself.
  - `docs/DEPLOYMENT.md` §3 "Harden" gains a prominent threat-model
    callout at the top of the section, stating the two callbacks
    operators must configure for any non-throwaway deployment
    (per-principal authn + SQL-content/admin-endpoint authz).
  - `examples/auth/README.md` gains a "Why these recipes matter"
    section that motivates each recipe with a concrete threat
    it closes.

  All three cross-link each other. Pure documentation — no code
  or test changes.

  Discovered during interactive testing of v0.1.2: a regular
  bearer-authenticated `/sql` caller can run `SET GLOBAL
  harbor_allow_admin_without_authz = TRUE` and unlock every admin
  endpoint. The model is the same as every other database with a
  SQL surface (PostgreSQL, MySQL, etc.), but the documentation
  previously didn't make this load-bearing fact explicit.

## [0.1.2] — 2026-05-19

### Added

- `harbor_stop()` no-arg overload ([#28](https://github.com/shreeve/duckdb-harbor/issues/28)).
  Single-server-per-process semantics make the no-arg form unambiguous:
  it stops whatever's currently running, with a clear error if nothing
  is. The existing `harbor_stop(uri)` form is preserved for callers
  scripted with a literal URI and for symmetry with `quack_stop(uri)`.
- Auto-load `httpfs` on harbor extension load ([#29](https://github.com/shreeve/duckdb-harbor/issues/29)).
  Side-loaded local builds (downloaded from the GitHub Release, or
  `LOAD '/abs/path/harbor.duckdb_extension'`) historically required an
  explicit `LOAD httpfs;` before `CALL harbor_serve(...)`, because
  `harbor_serve` uses libcrypto (HMAC, CSPRNG, CSP-nonce) that lives in
  the httpfs extension. The community-installed `harbor` (`INSTALL harbor
  FROM community`) auto-pulled httpfs as a transitive dep; side-loaded
  builds didn't, leading to a confusing `Invalid Configuration Error:
  DuckDB currently has a read-only crypto module loaded` on first use.
  Auto-load closes that footgun for both install paths. Best-effort:
  if `AutoLoadExtension` can't find httpfs (custom DuckDB build,
  network-isolated env), the operator still gets DuckDB's clearer
  error from `harbor_serve` itself.

## [0.1.1] — 2026-05-18

### Fixed

- **UI proxy 500s on compressed asset bodies.** The browser sends
  `Accept-Encoding: gzip, deflate, br` on every UI asset fetch.
  cpp-httplib's HTTPS client (used by harbor's outbound `GET /.*`
  proxy) doesn't reliably decode compressed response bodies — the
  read fails with `Error::Read` ("Failed to read connection") and
  the user sees a 500 in the browser, with the DuckDB UI showing
  blank assets and a "Connection to DuckDB Lost" modal. Harbor now
  forces `Accept-Encoding: identity` upstream regardless of what
  the browser asked. The browser↔harbor hop is localhost (or a
  same-DC reverse proxy where compression is the proxy's job), so
  the bandwidth savings on that hop are nil. End-to-end: full
  uncompressed pass-through. Pure compression-pass-through is
  queued for v0.2 via the libcurl/`HTTPUtil` migration (see
  AGENTS.md PR-10b).
- **`/localEvents` 401 on same-origin EventSource (browser CSRF
  defense was too strict).** Per the Fetch spec, browsers do NOT
  send `Origin` on same-origin "no-cors" requests like
  `new EventSource('/localEvents')` — but the route handler was
  rejecting an empty Origin pre-auth. Now: empty Origin is
  accepted (auth-gated below); cross-origin with disallowed
  Origin still rejects pre-auth. The DuckDB UI's SSE long-poll
  for catalog events now stays open instead of immediately 401'ing,
  which was the proximate cause of the "Connection to DuckDB Lost"
  modal.
- **`/localToken` 401 on `Referer: http://127.0.0.1:<port>/` with
  harbor bound on `127.0.0.1`.** The Referer-prefix check
  hardcoded `http://localhost:<port>` regardless of bind host.
  Browsers connecting via `http://127.0.0.1:<port>` (the default
  printed by `harbor_serve`) never matched. Now checks against
  the same loopback-variant set that `IsAllowedOrigin` uses
  (`localhost` / `127.0.0.1` / `[::1]` / configured bind host).
  Cross-origin Referer still 401s.

### Added

- **Retry-once on transport-layer errors in `HandleProxyGet`.** A
  single retry with a fresh `httplib::Client` on `Error::Read`,
  `Error::Write`, or `Error::Connection` masks transient TLS
  hiccups, edge-server resets, and similar cpp-httplib quirks.
  If the retry also fails, the original error propagates.
- **`golden-cookie-auth.sh` PR-8 A assertion updated** to verify
  that harbor sends `Accept-Encoding: identity` upstream (the
  positive guard for the compression-bug fix), instead of the
  previous assertion that the browser's gzip preference was
  forwarded verbatim.

## [0.1.0] — 2026-05 (post-release notes)

The original 0.1.0 entry below is preserved for historical context.
Several CI/tooling improvements landed between 0.1.0 and 0.1.1
without changing extension behavior — they all originally appeared
in the 0.1.0 [Unreleased] section before the rename:

### CI / tooling (post-0.1.0)

- CI `paths-ignore` extended to cover every path that can't affect
  the compiled `.duckdb_extension` binary or the tests CI runs
  against it. Now skipped: `scripts/**`, `examples/**`, `deploy/**`,
  `.gitignore`, `.gitattributes`, `CITATION.cff` (in addition to
  the previous list of `**.md`, `docs/**`, `CHANGELOG.md`,
  `.editorconfig`, `LICENSE`). Commits that touch ONLY non-build
  paths now skip the 9-platform matrix entirely; mixed commits
  (code + docs) still rebuild as before. Workflow-file changes
  (`.github/workflows/**`) deliberately stay outside the ignore
  list so workflow edits validate themselves.
- `scripts/load-test.sh` upgraded to auto-detect a real HTTP
  benchmarker (`oha` preferred, `wrk` fallback) and use it instead
  of the per-request `curl` shell loop. The previous shell-loop mode
  is still available via `--shell` flag, but the script prints a
  banner explaining the per-curl process-spawn overhead is the
  bottleneck (so the shell-mode numbers are 1-2 orders of magnitude
  below reality). Also: handles `NO_COLOR=1` env (oha argparse needs
  `--no-color true` literal). `docs/DEPLOYMENT.md` Load-test section
  rewritten to document expected ranges (5,000-30,000 req/s with
  oha on localhost).

### Added

- **Deployment kit** (post-v0.1.0, no source changes):
  - `docs/DEPLOYMENT.md` — end-to-end guide from local laptop smoke
    test through systemd / Docker / Incus deployments, with hardening
    + validation + load-test sections.
  - `scripts/validate-deployment.sh` — 28-assertion HTTP smoke test
    pointable at any deployed harbor (reuses the golden-test pattern
    against a remote URL). Validates liveness, /sql happy paths, all
    auth invariants, Mode A + Mode B params, login-page CSP+nonce,
    cookie roundtrip incl. CSRF defense, CORS allow-list, /quack
    route, admin endpoint default-deny.
  - `scripts/load-test.sh` — concurrent /sql hammering with
    per-request latency tracking. Reports throughput, error rate,
    p50/p95/p99/max. Pass/fail at error rate > 0.5%.
  - `deploy/Dockerfile` + `deploy/docker-compose.yml` +
    `deploy/harbor-bootstrap-docker.sql` — single-stage container
    that downloads DuckDB CLI v1.5.2 and the matching harbor binary
    from the v0.1.0 release.
  - `deploy/harbor.service` + `deploy/harbor-bootstrap.sql` — systemd
    unit + bootstrap SQL with hardening defaults (restricted
    `User=`, `ProtectSystem=strict`, `NoNewPrivileges`, etc.).
  - `examples/auth/` — production-grade auth callback recipes:
    `bearer-only-static.sql`, `bearer-table-multi-tenant.sql`,
    `bearer-with-expiry.sql`, `rbac-authorization.sql`. Each
    self-contained and copy-paste-loadable.

## [0.1.0] — 2026-05

Initial release. Tracks upstream `duckdb-quack` `v1.5-variegata` and
`duckdb-ui`. Targets DuckDB v1.5.2.

### Added

- **Quack RPC** (`POST /quack`) — wire-compat with stock `duckdb-quack`
  clients. Vendored from `duckdb/duckdb-quack` `v1.5-variegata`
  (commit `90bd70e`); see [`docs/upstream-quack-patches.md`](docs/upstream-quack-patches.md)
  for the surgical edits.
- **DuckDB UI** (`/ddb/*`, `/localEvents`, `/localToken`, `GET /.*`) —
  vendored from `duckdb/duckdb-ui`. Cookie-gated, with the UI proxy
  stripping harbor credentials before forwarding upstream
  ([PR-8 invariant](docs/upstream-ui-patches.md)).
- **JSON SQL** (`POST /sql`) — NDJSON streaming (row + chunk modes),
  one-shot JSON, prepared-parameter binding with full nested-type
  Mode B (`LIST<T>`, `ARRAY<T,N>`, `MAP<K,V>`, `STRUCT(...)`),
  principal-owned `/sql/sessions`, `/sql/cancel`,
  `harbor_query_timeout_s` runtime enforcement.
- **Admin** (`/health`, `/ready`, `/whoami`, `/info`, `/tables`,
  `/schema/:db/:t`, `/checkpoint`, `/sessions`, `/interrupt`,
  `/sql/cancel`) — centralized `__HARBOR_ADMIN__:resource:action`
  default-deny + `harbor_allow_admin_without_authz` operator opt-in.
- **Auth** (`/auth/login`, `/auth/logout`) — Bearer / X-Harbor-Token /
  HMAC-signed `harbor_session` cookie. Non-Bearer `Authorization`
  schemes return `UNSUPPORTED_AUTH_SCHEME` 401 (no fallback to
  ambient state). Login page has CSP + per-request CSPRNG nonce.
- **Lifecycle** (`harbor_serve` / `harbor_stop` / `harbor_wait`) —
  single-server-per-process; daemon-mode `harbor_wait()` blocks until
  SIGTERM/SIGINT.
- **CORS allow-list** (`harbor_cors_origins`) — replaces the wildcard
  `*` on `/info`, `/quack`, `/auth/*`, `/sql`. `harbor_serve` refuses
  to start on `'*'` or malformed entries.
- **Local-dev bypass** (`harbor_local_dev_mode`) — synthetic
  `__HARBOR_LOCAL_DEV__` principal so the connection-pool isolation
  invariant still holds. Forced off when `harbor_bind ≠ 127.0.0.1`.
- **Logging** — `'Quack'` log type registered (inherited verbatim from
  upstream `duckdb-quack`). A planned rename to `'Harbor'` with `'Quack'`
  preserved as a back-compat alias was tracked in issue
  [#30](https://github.com/shreeve/duckdb-harbor/issues/30) and
  deferred — the existing name is functionally complete.

### Verified

- 9-platform CI matrix: `linux_amd64`, `linux_arm64`, `osx_amd64`,
  `osx_arm64`, `windows_amd64`, `windows_amd64_mingw`, `wasm_mvp`,
  `wasm_eh`, `wasm_threads`. Plus matrix-generation +
  architecture-guard checks (single `duckdb_httplib::Server` owner
  enforced).
- 198 test assertions across 6 suites: `test/sql/harbor.test` (45),
  `golden-cookie-auth.sh` (18), `golden-sql-roundtrip.sh` (32),
  `golden-admin-roundtrip.sh` (31), `golden-query-timeout.sh` (10),
  `golden-sql-types.sh` (62).

### Known limitations / post-v0.1 backlog

- UI assets distributed via `proxy` mode (forward to `ui.duckdb.org`).
  The `bundled` mode (UI assets compiled into the extension) is
  post-v0.1.
- Cookie signing key is **ephemeral random per process** — restart
  logs everyone out, by design. Operator-controlled
  `HARBOR_COOKIE_SIGNING_KEY` env var is post-v0.1.
- Byte-level Quack/UI fixtures (`test/golden/quack/`,
  `test/golden/ui/`) — runtime wire compat is currently covered
  end-to-end by `test/sql/harbor.test`'s `/quack` block + the
  `scripts/golden-*.sh` suite.
- Spatial `GEOMETRY` type encoding — requires the spatial extension
  as a build dep.
- `/sql` route still surfaces generic `UNAUTHORIZED` instead of
  `UNSUPPORTED_AUTH_SCHEME` (security property is preserved; the
  errorCode propagation is a v0.2 cleanup).
- Error envelope shape inconsistency between `/auth/*`
  (`{"error":"<code>","message":...}`) and `/sql` / admin
  (`{"ok":false,"errorCode":"<code>","error":<msg>}`) — pre-existing
  tech debt, post-v0.1 normalization.

[0.1.1]: https://github.com/shreeve/duckdb-harbor/releases/tag/v0.1.1
[0.1.0]: https://github.com/shreeve/duckdb-harbor/releases/tag/v0.1.0
