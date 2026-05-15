# duckdb-harbor

> **DuckDB as an HTTP service: Quack RPC for DuckDB clients, JSON `/sql` for apps, and the official DuckDB UI — all on one port.**

`harbor` is a single DuckDB extension. When loaded, your DuckDB instance
turns into a multi-protocol HTTP service. Browser users get the official
DuckDB UI. Other DuckDB instances can `ATTACH 'quack:host'` and run SQL
against you. Application code POSTs JSON to `/sql` and gets NDJSON back.
All on the same port, against the same in-process DuckDB, with the same
session pool and auth model.

> **Status:** v0.1 design baseline. Tracks upstream `duckdb-quack`
> (`v1.5-variegata`) and `duckdb-ui`. Targets DuckDB v1.5.2; will
> rebase to DuckDB v2.0 GA when it lands. The full design is in
> [`SPEC.md`](./SPEC.md).
>
> **Implementation status:** This README describes the v0.1 *target*.
> The repo is being implemented in staged PRs (see [`AGENTS.md`](./AGENTS.md)
> "Implementation roadmap"). What works as of `main` today: PR-1
> vendored `duckdb-quack` and built it as `harbor.duckdb_extension`;
> PR-1.5 added a `/quack` runtime roundtrip test; PR-2 extracted the
> shared `HarborHttpServer` + `harbor_serve` / `harbor_stop` / `harbor_wait`
> lifecycle + `/health` and `/info`; PR-3 vendored `duckdb-ui` and wired
> `UiHandlers` against the shared server (`/ddb/*`, `/localEvents`,
> `/localToken`, GET `/.*` proxy to `ui.duckdb.org`); **PR-4** added
> HMAC-signed `harbor_session` cookie auth (`POST /auth/login` /
> `POST /auth/logout`), the SPEC §7 cookie-aware gate on `/ddb/*`
> + `/localEvents` + the UI catch-all, principal-scoped UI connection
> pool, and the `harbor_cors_origins` allow-list (replaces the wildcard
> CORS on `/info` and `/quack`; `harbor_serve` refuses to start on
> `'*'`). The cookie signing key is **ephemeral random per process**
> in v0.1 — restart logs everyone out, by design (see SPEC §7).
>
> The browser flow now: open `http://localhost:9494/`, paste the token
> printed by `harbor_serve()`, the page POSTs to `/auth/login`, sets a
> `HttpOnly; SameSite=Strict` cookie, reloads, and the cookie-bearing
> request proxies through to `ui.duckdb.org`. For local dev only,
> `SET GLOBAL harbor_local_dev_mode = true` (with bind on loopback)
> skips the token-paste step and uses a synthetic
> `sha256("__HARBOR_LOCAL_DEV__")` principal so the connection-pool
> isolation invariant still holds; **PR-5** added the JSON `/sql`
> endpoint (`POST /sql`) with NDJSON streaming, one-shot JSON mode,
> prepared-parameter binding, explicit SQL sessions, per-principal
> session ownership checks, and `OPTIONS /sql` CORS preflight.
>
> Still pending: admin handlers (PR-6). Examples below describe the
> eventual admin API.

## Quick Start

```sql
-- Today, from a local build:
LOAD '/path/to/build/release/extension/harbor/harbor.duckdb_extension';

-- After community publication (planned):
-- INSTALL harbor FROM community;
-- LOAD harbor;

-- Start the server (returns the URI, URL, and a generated token)
CALL harbor_serve('harbor:127.0.0.1:9494');
```

```
┌─────────────────────────────┬───────────────────────┬─────────────────────────────────┐
│           uri               │         url           │             token               │
├─────────────────────────────┼───────────────────────┼─────────────────────────────────┤
│ harbor:127.0.0.1:9494        │ http://127.0.0.1:9494 │ a1b2c3d4e5f6...                 │
└─────────────────────────────┴───────────────────────┴─────────────────────────────────┘
```

Open `http://127.0.0.1:9494/`. harbor asks you to **paste the token
once**, then the official DuckDB UI loads and stays authenticated for
the session.

> **Heads-up for non-interactive use.** `harbor_serve` returns
> immediately so you can keep using your DuckDB REPL. To run harbor as a
> daemon (containers, systemd) you need to keep the DuckDB process
> alive: call `CALL harbor_wait();` after `harbor_serve`. See
> [Deployment](#deployment--incus--zfs).

## What It Does

`harbor` runs **one HTTP server on one port** and serves three protocols
simultaneously, all backed by the same in-process DuckDB instance:

```
                         ┌─────────────────────────────────┐
                         │  DuckDB process                 │
                         │  ┌───────────────────────────┐  │
                         │  │ harbor extension           │  │
   ┌──────────┐          │  │  one HTTP server          │  │
   │ Browser  │─────────▶│  │  one port (:9494)         │  │ ── /data/db.duckdb
   │ (UI)     │          │  │  shared session pool      │  │    (or :memory:)
   └──────────┘          │  │  shared auth model        │  │
                         │  └────┬──────┬───────┬───────┘  │
   ┌──────────┐          │       │      │       │          │
   │  duckdb  │─────────▶│  /quack    /sql   /ddb/*        │
   │  CLI     │          │       │      │       │          │
   └──────────┘          │       │      │       │          │
                         └───────┼──────┼───────┼──────────┘
   ┌──────────┐                  │      │       │
   │  Bun /   │──────────────────┘      │       │
   │  Python /│─────────────────────────┘       │
   │  Go /etc │                                 │
   └──────────┘                                 │
                                                │
   ┌──────────┐                                 │
   │ Browser  │─────────────────────────────────┘
   │ (UI)     │
   └──────────┘
```

| Audience | Endpoint | Protocol |
|---|---|---|
| Stock DuckDB clients (CLI, Wasm, notebooks, anything that loads the upstream `quack` extension) | `POST /quack` | Quack RPC — `application/vnd.duckdb` binary, single round-trip per query, parallel `FETCH` for big results |
| App code in any language | `POST /sql` | JSON in, NDJSON out, schema-typed, sticky sessions for transactions |
| Browser users | `GET /` | The official DuckDB UI |
| Monitoring / scripts / ops | `GET /health`, `/ready`, `/tables`, `/schema/...`, `/whoami`, `POST /checkpoint`, `GET /sessions`, `POST /interrupt` | JSON |

Writes through any one of these are immediately visible to all the others.
Zero duplication, zero sync.

## Ways to Connect

### 1. Browser — Official DuckDB UI

Open `http://localhost:9494/` and you get the full DuckDB notebook:
SQL notebooks, syntax highlighting, tab completion, query history, data
exploration. By default, `harbor` proxies the UI assets from
`ui.duckdb.org` (matching the upstream `duckdb-ui` extension behavior).
For air-gapped or restricted-egress deployments, point
`harbor_ui_proxy_url` at an internal HTTP mirror of `ui.duckdb.org`
(a 10-line nginx config); `harbor_ui_assets = 'disabled'` opts out
entirely. The proxy strips all harbor-auth headers (`Cookie`,
`Authorization`, `X-Harbor-Token`) before forwarding upstream. A
compiled-in `bundled` mode is planned for v0.2 if true
no-internal-mirror air-gap demand emerges. The `/ddb/*` binary
protocol is unaffected by the asset mode.

The first time you visit, harbor asks for the token printed by
`harbor_serve` and issues an HTTP-only cookie. After that, the UI works
unmodified.

### 2. Stock `duckdb` CLI — `ATTACH 'quack:host'`

Any DuckDB with the upstream `quack` extension installed can attach a
harbor server as a first-class catalog. The wire protocol on `/quack`
is exactly upstream Quack — harbor is a strict superset.

```bash
duckdb
```
```sql
INSTALL quack FROM core_nightly;
LOAD quack;

CREATE SECRET (
  TYPE quack,
  TOKEN 'a1b2c3d4...',                        -- the token from harbor_serve
  SCOPE 'quack:127.0.0.1:9494'
);

ATTACH 'quack:127.0.0.1:9494' AS r (TYPE quack);
SHOW TABLES FROM r;
SELECT * FROM r.users WHERE active = true LIMIT 10;

-- Transactions work end-to-end
BEGIN; INSERT INTO r.t VALUES (1); COMMIT;

-- Filter and projection pushdown happen automatically
EXPLAIN SELECT id, name FROM r.users WHERE id = 42;
```

> Stock DuckDB-Quack clients use the `quack:` URI scheme. harbor servers
> also accept `harbor:` for tooling that's harbor-aware; the two resolve
> to the same handler.

### 3. Plain HTTP — any language, any tool

The JSON API at `/sql` accepts parameterized queries and streams NDJSON
back. Works from any language that speaks HTTP.

```bash
curl -X POST http://localhost:9494/sql \
  -H "Authorization: Bearer $HARBOR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"sql":"SELECT * FROM users WHERE id = $1","params":[42]}'
```

```ndjson
{"type":"schema","sessionId":"9f3c...","columns":[{"name":"id","duckdbType":"INTEGER"},{"name":"name","duckdbType":"VARCHAR"}]}
{"type":"row","values":[42,"Alice"]}
{"type":"end","rowCount":1,"timeMs":3}
```

For a single-shot non-streamed JSON response, ask for `Accept: application/json`:

```bash
curl -X POST http://localhost:9494/sql \
  -H "Authorization: Bearer $HARBOR_TOKEN" \
  -H "Accept: application/json" \
  -d '{"sql":"SELECT 42 AS answer"}'
```

```json
{
  "ok": true,
  "kind": "select",
  "sessionId": "9f3c...",
  "columns": [{"name":"answer","duckdbType":"INTEGER"}],
  "data": [[42]],
  "rowCount": 1,
  "timeMs": 1
}
```

#### Sticky transactions

Transactions and other multi-statement state require an explicit DB
session, created up front:

```bash
# 1. Create a persistent DB session
SID=$(curl -sf -X POST http://localhost:9494/sql/sessions/new \
        -H "Authorization: Bearer $HARBOR_TOKEN" | jq -r .sessionId)

# 2. Run statements against it
for SQL in 'BEGIN' 'INSERT INTO t VALUES (1)' 'INSERT INTO t VALUES (2)' 'COMMIT'; do
  curl -X POST http://localhost:9494/sql \
    -H "Authorization: Bearer $HARBOR_TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"sql\":\"$SQL\",\"sessionId\":\"$SID\"}"
done

# 3. Optional cleanup (also auto-expires after harbor_session_ttl_s)
curl -X DELETE http://localhost:9494/sql/sessions/$SID \
  -H "Authorization: Bearer $HARBOR_TOKEN"
```

Requests without a `sessionId` get an ephemeral session and cannot
issue `BEGIN` (rejected with `BAD_REQUEST`). This prevents transactions
from being silently abandoned.

### 4. DuckDB-Wasm in a browser

DuckDB-Wasm builds with the `quack` extension can `ATTACH` a harbor
server directly. Caveats apply: same-origin policy, mixed-content
restrictions, and HTTPS for any non-localhost target.

### 5. Minimal HTTP endpoints — scripts, monitoring, ops

| Endpoint | Method | Auth | Description |
|---|---|---|---|
| `/health` | GET | public | Process health — `{ok, version, uptime_s}` |
| `/ready` | GET | public | DB readiness — runs `SELECT 1`, returns 503 on failure |
| `/info` | GET | public | Version headers (used by the DuckDB UI to detect server) |
| `/whoami` | GET | bearer/cookie | Identity + runtime info, JSON form of `whoami()` |
| `/tables` | GET | bearer/cookie + authz | List tables in `main` |
| `/schema/:db/:table` | GET | bearer/cookie + authz | Column list + types (404 if missing) |
| `/checkpoint` | POST | admin authz | Run `CHECKPOINT;` — typically before a ZFS snapshot |
| `/sessions` | GET | admin authz | Live sessions |
| `/interrupt` | POST | admin authz | Interrupt a session's running query |

### Which one should I use?

| Task | Best fit |
|---|---|
| Interactive exploration, ad-hoc queries | Browser UI |
| Cross-DB attach, joins, copying data between DuckDBs | upstream `quack` extension + `ATTACH 'quack:host'` |
| App code (transactions, prepared queries, your data layer) | `POST /sql` |
| One-off shell scripts, integrations from non-DuckDB services | `POST /sql` with `Accept: application/json` |
| Monitoring / health checks | `/health`, `/ready` |
| Cron-driven backup before snapshot | `POST /checkpoint` then `zfs snapshot` |

## Features

- **Single binary, single port.** One DuckDB extension; one HTTP server.
- **Three protocols, one DuckDB instance.** Quack RPC, JSON SQL, and the official UI all share state.
- **Session-bound transactions.** `BEGIN…COMMIT`, `SET VARIABLE`, `CREATE TEMP TABLE` all survive across HTTP calls when you pass an explicit `sessionId`.
- **Pluggable auth.** Token + per-connection authentication and per-query authorization, as user-supplied SQL macros. One model covers `/quack`, `/sql`, `/ddb/run`, and admin endpoints.
- **Streaming `/sql`.** Large result sets stream as NDJSON over chunked transfer encoding.
- **Schema-typed encoding.** `/sql` NDJSON encodes every DuckDB core type losslessly when decoded with the schema record (DECIMAL preserves width/scale, TIMESTAMP preserves precision, INTERVAL preserves all three components, BIGINT/HUGEINT as strings to dodge JS precision loss).
- **UI assets via proxy.** Default forwards to `ui.duckdb.org`; point `harbor_ui_proxy_url` at an internal mirror for restricted-egress deployments. The proxy strips all harbor-auth headers from outbound requests. A compile-in `bundled` mode is planned for v0.2 (deferred from v0.1 — most "air-gap" deployments are better served by an internal HTTP mirror, which works today with no harbor change).
- **Upstream-compatible Quack.** Stock DuckDB clients work without modification.
- **Container-ready.** Designed to be deployed as `duckdb -no-stdin -init harbor-init.sql /data/db.duckdb` against a ZFS dataset.

## How It's Built

`harbor` is a single DuckDB extension forked from two upstream projects:

| Source | License | What we use |
|---|---|---|
| [`duckdb/duckdb-quack`](https://github.com/duckdb/duckdb-quack) `v1.5-variegata` | MIT | Quack wire protocol, session pool, auth hooks, `ATTACH` catalog, the cpp-httplib server |
| [`duckdb/duckdb-ui`](https://github.com/duckdb/duckdb-ui) | MIT | DuckDB UI binary protocol (`/ddb/run`, `/ddb/tokenize`, `/ddb/interrupt`), tokenizer, asset proxy |

harbor adds the `/sql` JSON/NDJSON endpoint, an HMAC-signed auth-cookie
flow for the browser UI, the asset bundling story, the admin/ops
endpoints, the unified auth model, and the refactor that puts both
Quack and UI handlers on a single shared `cpp-httplib::Server`
instance.

The bulk of the new code is the `DataChunk → NDJSON` encoder for
`/sql` (handles every DuckDB core type per [`SPEC.md`](./SPEC.md) §5.4)
and the auth-cookie layer (HMAC sign/verify, login/logout endpoints,
harbor login wrapper at `GET /`).

## Security

harbor is **trusted-network software** by default. The auth token grants
full SQL access to the underlying DuckDB — treat it as a database
password.

- **Default bind:** `127.0.0.1` — reachable only from the local machine.
- **Public routes:** `GET /health`, `GET /ready`, `GET /info`, `GET /.*` (UI assets), and `OPTIONS /quack` (CORS preflight). Everything else requires authentication.
- **Browser-origin requests do NOT bypass token auth.** The `Origin` check is CSRF defence; the auth cookie (issued by `POST /auth/login`) carries identity.
- **`/localToken`** (used to bridge MotherDuck auth into the local UI) is automatically disabled when bound to a non-loopback address.
- **For network exposure**, front harbor with nginx or Caddy doing TLS termination — recipe in [`docs/REVERSE_PROXY.md`](./docs/REVERSE_PROXY.md). Your reverse proxy **must** forward `X-Forwarded-Proto: https` (or the browser's request must have `Origin: https://…`) so harbor sets the `Secure` attribute on issued `harbor_session` cookies. Without that header, cookies are issued without `Secure`, which is acceptable on plain-HTTP loopback dev but unsafe over a public HTTPS deployment. nginx default config (`proxy_set_header X-Forwarded-Proto $scheme;`) does the right thing.
- **CORS** is blocked by default. Allow specific origins via `harbor_cors_origins`. The setting accepts a comma-separated list of `scheme://host[:port]` entries (no path, no query, no fragment, no trailing slash); `harbor_serve` refuses to start if it sees `'*'` or a malformed entry.

Authentication and authorization are **two pluggable SQL functions**:

```sql
-- Multi-token table for per-user keys
CREATE TABLE harbor_tokens (token VARCHAR, user_name VARCHAR);
INSERT INTO harbor_tokens VALUES
  ('alice-key-...', 'alice'),
  ('bob-key-...',   'bob');

CREATE MACRO check_token(sid, client_token, server_token) AS (
  EXISTS (SELECT 1 FROM harbor_tokens WHERE token = client_token)
);
SET GLOBAL harbor_authentication_function = 'check_token';

-- Read-only authorization (admin endpoints use synthetic SQL of shape
-- '__HARBOR_ADMIN__:<resource>:<action>' — e.g. '__HARBOR_ADMIN__:checkpoint:create',
-- '__HARBOR_ADMIN__:sessions:list' — so policies can branch on prefix or pair)
CREATE MACRO authz(sid, query) AS (
  CASE
    WHEN starts_with(query, '__HARBOR_ADMIN__:')
      THEN false                                      -- no admin ops via this hook
    ELSE
      regexp_matches(upper(trim(query)),
                     '^(SELECT|FROM|WITH|EXPLAIN|DESCRIBE|SHOW)\b')
  END
);
SET GLOBAL harbor_authorization_function = 'authz';
```

Full security model in [`SPEC.md`](./SPEC.md) §7.

## Deployment — Incus + ZFS

The intended deployment shape:

```
host
├── /tank/duckdb/<tenant>/                  ← ZFS dataset (COW snapshots)
│   ├── db.duckdb
│   ├── harbor-init.sql                      ← LOAD harbor; harbor_serve(...); harbor_wait();
│   └── harbor-token
└── incus container "harbor-<tenant>"
    ├── /usr/bin/duckdb
    ├── /root/.duckdb/extensions/.../harbor.duckdb_extension
    └── /data → bind-mount of /tank/duckdb/<tenant>/
```

The container's `ENTRYPOINT` is one command:

```bash
duckdb -no-stdin -init /data/harbor-init.sql /data/db.duckdb
```

Where `harbor-init.sql` is:

```sql
LOAD harbor;
CALL harbor_serve('harbor:0.0.0.0:9494');
CALL harbor_wait();              -- blocks until SIGTERM/SIGINT
```

`harbor_wait()` is required: without it, the DuckDB CLI exits as soon as
the init script finishes and takes the server with it.

Operations collapse to ZFS verbs:

| Task | Command |
|---|---|
| Snapshot | `POST /checkpoint && zfs snapshot tank/duckdb/<tenant>@hourly-…` |
| Clone for staging | `zfs clone …@last-good …-staging && incus launch harbor-image …` |
| Restore | `zfs rollback …@some-snapshot && incus restart …` |
| Move host | `zfs send | ssh other zfs receive && incus copy …` |
| Upgrade harbor | drop new `.duckdb_extension` into image, `incus restart` |

Full guide: [`docs/DEPLOY_INCUS_ZFS.md`](./docs/DEPLOY_INCUS_ZFS.md).

## Configuration

All settings are regular DuckDB session/global options.

| Setting | Default | Purpose |
|---|---|---|
| `harbor_bind` | `127.0.0.1` | Bind address |
| `harbor_port` | `9494` | Listen port |
| `harbor_token` | (auto-generated) | Auth token |
| `harbor_authentication_function` | `harbor_check_token` | SQL function name for auth callback |
| `harbor_authorization_function` | `harbor_nop_authorization` | SQL function name for authz callback |
| `harbor_cookie_signing_key` | (auto, per-process) | HMAC key for `harbor_session` cookies |
| `harbor_max_sessions` | `1024` | Max concurrent DB sessions |
| `harbor_session_ttl_s` | `3600` | Idle session TTL |
| `harbor_query_timeout_s` | `0` | Per-query timeout (0 disables) |
| `harbor_max_request_body_bytes` | `268435456` (256 MiB) | Per-request body cap |
| `harbor_ui_assets` | `proxy` | `proxy` / `disabled` (v0.1; `bundled` planned for v0.2) |
| `harbor_ui_proxy_url` | `https://ui.duckdb.org` | Upstream URL when `harbor_ui_assets = 'proxy'` |
| `harbor_cors_origins` | `''` | Allow-list for cross-origin browser requests |
| `harbor_log_requests` | `true` | Per-request structured log |
| `harbor_log_queries` | `false` | Log full SQL of every query |

Full reference in [`SPEC.md`](./SPEC.md) §9.

## Roadmap

Explicitly out of scope for v0.1 (see [`docs/ROADMAP.md`](./docs/ROADMAP.md)
for the full list):

- Arrow stream output on `/sql` (`Accept: application/vnd.apache.arrow.stream`)
- `/metrics` Prometheus endpoint
- WebSocket transport for `/sql`
- Built-in TLS
- Multi-tenant proxy in front of N harbors (lives in a separate ops project)

## Requirements

- **DuckDB** v1.5.2 (each harbor release pins to a specific DuckDB version)
- **Reverse proxy** (nginx, Caddy, Traefik) recommended for TLS termination
  if exposing beyond localhost

## License

MIT. See [`LICENSE`](./LICENSE).

This project is a derivative work of `duckdb-quack` and `duckdb-ui`,
both © Stichting DuckDB Foundation, both MIT-licensed. Files
substantially derived from those projects retain their original
upstream MIT headers in addition to harbor's. (When `bundled` UI
assets land in v0.2, their notices will live in
`THIRD_PARTY_NOTICES.md`; v0.1 ships proxy mode only and has no
bundled assets to attribute.)
