# Changelog

All notable changes to the `harbor` DuckDB extension are documented
here. Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versioning follows [SemVer](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

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
- **Logging** — `'Harbor'` log type registered (`'Quack'` alias kept
  for upstream-tooling compatibility).

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

[0.1.0]: https://github.com/shreeve/duckdb-harbor/releases/tag/v0.1.0
