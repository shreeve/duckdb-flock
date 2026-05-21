# harbor — Specification

> **Status:** Draft v0.2 (2026-05). This document captures the design
> decisions for the `harbor` DuckDB extension before code is written.
> Implementation details (per-function pseudocode, byte layouts) are NOT
> in scope for this document — the source is the spec for those.

`harbor` is a single DuckDB extension that runs **one HTTP server on one
port** and serves three protocols simultaneously, against one shared
in-process DuckDB instance:

| Protocol | Endpoints | Audience | Wire format |
|---|---|---|---|
| **Quack RPC** | `POST /quack`, `OPTIONS /quack` | DuckDB clients (CLI, Wasm, notebooks) | `application/vnd.duckdb` (BinarySerializer) |
| **JSON SQL** | `POST /sql`, `POST /auth/login`, `POST /auth/logout`, `POST /sql/sessions/new`, `DELETE /sql/sessions/:id`, `POST /sql/cancel` | Application code (any HTTP client) | JSON request, NDJSON response |
| **DuckDB UI** | `POST /ddb/*`, `GET /info`, `GET /localEvents`, `GET /localToken`, `GET /.*` | Browser running the official DuckDB UI | `application/octet-stream` (BinarySerializer), JSON (`/info`), SSE (`/localEvents`), proxied/bundled HTML/CSS/JS (`GET /.*`) |
| **Convenience / ops** | `GET /health`, `GET /ready`, `GET /tables`, `GET /schema/:db/:t`, `GET /whoami`, `POST /checkpoint`, `GET /sessions`, `POST /interrupt` | Monitoring, scripts, ops humans | JSON |

All routes share one cpp-httplib `Server`, one DuckDB `DatabaseInstance`,
one session pool, and one auth/authz model.

## 1. Goals & non-goals

### Goals

- **Single binary, single port, single DuckDB instance.** A loaded `harbor`
  extension turns one DuckDB process into a multi-protocol HTTP service.
- **Upstream Quack compatibility.** Stock DuckDB clients (≥ v1.5.3 with the
  upstream `quack` extension installed) `ATTACH 'quack:host'` and Just Work
  against a harbor server. We track upstream `duckdb-quack` and rebase as it
  evolves toward DuckDB v2.0 GA.
- **Application-friendly JSON SQL endpoint.** `POST /sql` is the path
  app code hits. JSON in, NDJSON out, schema-typed for round-trip safety
  on core types.
- **Official DuckDB UI included.** Browse to the port, get the real UI.
- **Pluggable auth.** Token + per-connection authentication and per-query
  authorization, as user-supplied SQL macros or scalar UDFs. One model
  applies to every SQL-bearing endpoint.
- **Container-ready deployment.** Designed to run as
  `duckdb -no-stdin -init harbor-init.sql /data/db.duckdb` inside a
  minimal Incus application container against a ZFS dataset for COW
  snapshots.

### Non-goals

- **Not a query planner / not a database.** harbor is a transport. All SQL
  semantics are DuckDB's.
- **No native TLS.** TLS is terminated at a reverse proxy (nginx, Caddy,
  Traefik). cpp-httplib supports SSL but bringing OpenSSL into the
  extension binary is more headache than it's worth.
- **No row-level security, no per-table ACLs, no cell masking.** The
  authorization hook sees the full query text — anything finer is your
  application's problem.
- **No replication, no clustering, no leader election.** One process per
  database. Replication is `zfs send | zfs receive`.
- **No new query language, no reinvented dialect.** It's DuckDB SQL.

## 2. Architecture

> For the higher-level "why does this exist when stock Quack + an
> httpserver extension + duckdb-ui already exist" framing — input
> deltas, what harbor adds beyond the sum, and what harbor is
> explicitly *not* — see
> [`docs/WHY_HARBOR.md`](docs/WHY_HARBOR.md). This section is the
> architectural truth; that doc is the positioning summary.

### Process layout

```
                    ┌────────────────────────────────────────┐
                    │          DuckDB process (single)       │
                    │                                        │
                    │  ┌──────────────────────────────────┐  │
                    │  │   HarborHttpServer (one instance) │  │
                    │  │                                  │  │
   ┌──────────┐     │  │  cpp-httplib Server (port 9494)  │  │
   │ Browser  │────▶│  │                                  │  │
   │ (UI)     │     │  │  ┌──────────┬──────────┬──────┐  │  │
   └──────────┘     │  │  │ Quack    │ Sql      │ Ui   │  │  │
                    │  │  │ Handlers │ Handlers │ Hand-│  │  │
   ┌──────────┐     │  │  │          │          │ lers │  │  │
   │ duckdb   │────▶│  │  └────┬─────┴────┬─────┴──┬───┘  │  │
   │ CLI      │     │  │       │          │        │      │  │
   └──────────┘     │  │       └────┬─────┴────────┘      │  │
                    │  │     SessionManager  AuthManager  │  │
   ┌──────────┐     │  │            │                     │  │
   │ Bun /    │────▶│  └────────────┼─────────────────────┘  │
   │ app code │     │               ▼                        │
   └──────────┘     │       DuckDB DatabaseInstance          │
                    │       (one .duckdb file or :memory:)   │
                    └────────────────────────────────────────┘
                                    │
                            (file on ZFS dataset)
```

### Component ownership

```
HarborHttpServer
├── owns    cpp_httplib::Server         (the listening socket + thread pool)
├── owns    SessionManager              (per-session DuckDB Connection pool)
├── owns    AuthManager                 (token + hook + auth-cookie issuance)
├── owns    InterruptManager            (sessionId → live PendingQueryResult)
├── owns    EventDispatcher             (UI SSE on /localEvents)
├── borrows weak_ptr<DatabaseInstance>  (does NOT extend its lifetime)
└── owns    QuackHandlers, SqlHandlers, UiHandlers, AdminHandlers
            (each registers its routes against the shared Server)
```

This is a deliberate departure from upstream:

- Upstream **`duckdb-quack`** has its `QuackServer` directly own the
  `httplib::Server`. We refactor: `QuackServer` becomes `QuackHandlers`,
  drops its server, and registers its routes against the shared one.
- Upstream **`duckdb-ui`** has its `ui::HttpServer` directly own a
  separate `httplib::Server`. We refactor: handlers extracted, server
  ownership moved up.

The handler subsystems are independent objects. None depends on another.
They share only `SessionManager`, `AuthManager`, and a `weak_ptr` to the
`DatabaseInstance`.

### Lifecycle

| Phase | Trigger | What happens |
|---|---|---|
| **Load** | `LOAD harbor;` | Extension registers settings, scalar functions, and the `harbor_serve` / `harbor_stop` / `harbor_wait` table macros. **No socket is bound yet.** |
| **Start** | `CALL harbor_serve('harbor:0.0.0.0:9494', token => '…')` | Construct `HarborHttpServer`, bind socket synchronously (so `EADDRINUSE` propagates to the caller), spawn listener thread. Returns `(uri, url, token)` row, identical shape to quack. **Returns immediately.** |
| **Block** | `CALL harbor_wait()` | Optional. Blocks the calling SQL session until the server stops or the process receives `SIGINT`/`SIGTERM`. Required for non-interactive container deployments — see "Daemon mode" below. |
| **Quiesce** | `CALL harbor_stop('harbor:0.0.0.0:9494')` | `Server::stop()` (closes listening socket — no new requests accepted), wait up to `harbor_stop_drain_timeout_s` (default 30s) for in-flight requests to complete naturally, then `Connection::Interrupt()` on every session that's still running a query, join listener thread, close all sessions, drop the `DatabaseInstance` `weak_ptr`. Releases any thread blocked in `harbor_wait()`. |
| **Restart** | restart of host process | All state is in-memory; restart is the reload primitive. Sub-second when the DB file is warm. |
| **Repeated load** | `LOAD harbor;` while already loaded | No-op. |
| **Repeated `harbor_serve`** | second call before `harbor_stop` | Throws. harbor is **single-server-per-process**: only one `HarborHttpServer` may be live at a time, regardless of URI/port. Enforced in `harbor_serve`. |
| **DuckDB shutdown** | DuckDB extension shutdown hook | If a server is running, `harbor_stop` is invoked. Specifically: only the listener thread is joined and the socket closed. We do **NOT** touch DuckDB internals from the shutdown hook (some are partially destroyed by then). `atexit` is used only as a last-resort fallback for child processes. |

Auto-start at extension load is **NOT** the default. The user must
`CALL harbor_serve(...)` explicitly. Reasons: testability, deployment
config visibility, no surprises in non-server contexts.

#### Daemon mode (containers, systemd)

The DuckDB CLI is fundamentally an interactive REPL. Running
`duckdb -c '…'` exits immediately after the last statement runs, which
would tear down the harbor server. For non-interactive deployments,
**`harbor_wait()` in an init script is the deployment pattern**:

```sql
-- /etc/harbor-init.sql
LOAD harbor;
CALL harbor_serve('harbor:0.0.0.0:9494');
CALL harbor_wait();
```

```bash
duckdb -no-stdin -init /etc/harbor-init.sql /data/db.duckdb
```

`harbor_wait()` blocks until the server stops or `SIGTERM`/`SIGINT`
arrives. The DuckDB process stays alive. Without a trailing
`harbor_wait()` (or some other blocking statement), the CLI exits when
the init script finishes and takes the server with it.

Interactive use (`duckdb` at a terminal) is unaffected: `harbor_serve`
returns immediately, the REPL is still yours, and the server runs in
the background.

> A `harbord` wrapper binary that hides the `-no-stdin -init …`
> incantation is intentionally not shipped — the unwrapped command is
> short, an init script is more flexible than a fixed wrapper (operators
> can install custom auth hooks, set globals, etc.), and the decision is
> reversible if operator demand justifies it.

## 3. URIs and protocol identity

### URI scheme

harbor uses **`harbor:`** as its primary URI scheme (used by `harbor_serve`)
and accepts **`quack:`** as an interop alias for client-side `ATTACH`
from stock upstream Quack.

| URI | Used by |
|---|---|
| `harbor:host[:port]` | harbor-aware tools (the eventual harbor client extension). Also accepted as an alias for `quack:` on the server side. |
| `harbor://host[:port]`, `harbor:[ipv6]:port` | RFC-style and IPv6 forms |
| `quack:host[:port]` | **Stock DuckDB clients with the upstream `quack` extension** loaded. harbor servers accept these unchanged. |

The two schemes resolve to the same handler. `harbor_uri_parser()`
parses both.

> **Important interop note:** stock DuckDB clients running upstream
> Quack do **not** know the `harbor:` scheme — only `quack:`. So when
> connecting *from* a vanilla `duckdb` CLI:
>
> ```sql
> ATTACH 'quack:127.0.0.1:9494' AS r (TYPE quack);
> ```
>
> The `harbor:` form becomes useful only after the harbor client
> extension ships. README and examples favor `quack:` for stock-client
> samples.

### Default port

**`9494`**, matching upstream Quack. No reason to diverge; the protocol
on `/quack` is wire-compatible.

### MIME types

| Endpoint | Content-Type (request) | Content-Type (response) |
|---|---|---|
| `POST /quack` | `application/vnd.duckdb` | `application/vnd.duckdb` |
| `POST /sql` | `application/json` | `application/x-ndjson` (chunked, default) or `application/json` (when `Accept: application/json`) |
| `POST /auth/login`, `/auth/logout`, `/sql/sessions/new`, `/sql/cancel` | `application/json` | `application/json` |
| `POST /ddb/run`, `/tokenize`, `/interrupt` | application/x-www-form-urlencoded body, headers carry params | `application/octet-stream` (BinarySerializer) |
| `GET /info` | — | empty body, headers carry version info |
| `GET /localEvents` | — | `text/event-stream` (SSE) |
| `GET /localToken` | — | `text/plain` |
| `GET /.*` (catch-all) | — | proxied from `ui.duckdb.org` (or served from bundle) |
| `GET /health`, `/ready`, `/tables`, `/schema/...`, `/whoami`, `/sessions` | — | `application/json` |
| `POST /checkpoint`, `/interrupt` | empty / JSON | `application/json` |

## 4. Routes (canonical reference)

Routes are registered in this order. Order matters in cpp-httplib —
the catch-all (`GET /.*`) **must** be last, and admin routes must be
registered before it.

| # | Method | Path | Handler | Auth | Notes |
|---|---|---|---|---|---|
| 1 | OPTIONS | `/quack` | QuackHandlers | none | CORS preflight |
| 2 | POST | `/quack` | QuackHandlers | per-message token via `harbor_authentication_function` | `application/vnd.duckdb` request/response |
| 3 | POST | `/auth/login` | AuthHandlers | bearer token (`Authorization: Bearer <token>`) | issues HMAC-signed `harbor_session` cookie + returns `{principal, expires_at}` |
| 4 | POST | `/auth/logout` | AuthHandlers | cookie | clears `harbor_session` cookie; optionally drops associated DB sessions |
| 5 | POST | `/sql` | SqlHandlers | bearer / cookie + authz hook | NDJSON streamed response (default) or one-shot JSON when `Accept: application/json`. `BEGIN`/`COMMIT` etc. require an explicit `sessionId`. |
| 6 | POST | `/sql/sessions/new` | SqlHandlers | bearer / cookie | explicitly creates a persistent DB session; returns `{sessionId}`. Use this when you need to control DB-session lifetime separately from a single request. |
| 7 | DELETE | `/sql/sessions/:id` | SqlHandlers | bearer / cookie + ownership | destroys the named DB session |
| 8 | POST | `/sql/cancel` | SqlHandlers | bearer / cookie + admin authz | body: `{sessionId}` |
| 9 | POST | `/ddb/run` | UiHandlers | cookie + Origin check + authz hook | UI binary protocol |
| 10 | POST | `/ddb/tokenize` | UiHandlers | cookie + Origin check | UI binary protocol |
| 11 | POST | `/ddb/interrupt` | UiHandlers | cookie + Origin check | UI binary protocol |
| 12 | GET | `/info` | UiHandlers | none | version headers, used by UI to detect server |
| 13 | GET | `/localEvents` | UiHandlers | cookie | SSE stream |
| 14 | GET | `/localToken` | UiHandlers | Referer == local URL **AND** `harbor_bind == 127.0.0.1` | returns MotherDuck token if present; **404 when bound non-locally** |
| 15 | GET | `/health` | AdminHandlers | none | minimal: `{ok:true, version, uptime_s}` only |
| 16 | GET | `/ready` | AdminHandlers | none | runs `SELECT 1` against a worker connection; 503 on failure |
| 17 | GET | `/whoami` | AdminHandlers | bearer / cookie | identity + runtime info, JSON form of `whoami()` macro |
| 18 | GET | `/tables` | AdminHandlers | bearer / cookie + authz (`__HARBOR_ADMIN__:catalog:list_tables`) | list tables in `main` |
| 19 | GET | `/schema/:db/:table` | AdminHandlers | bearer / cookie + authz (`__HARBOR_ADMIN__:catalog:describe_table`; path params NOT interpolated into authz string) | column info via `pragma_table_info()` with quoted identifiers; 404 on miss |
| 20 | POST | `/checkpoint` | AdminHandlers | bearer / cookie + authz (`__HARBOR_ADMIN__:checkpoint:create`) | runs `CHECKPOINT;` and returns the new WAL state |
| 21 | GET | `/sessions` | AdminHandlers | bearer / cookie + authz (`__HARBOR_ADMIN__:sessions:list`) | live sessions: id, principal, age, last query, in-flight |
| 22 | POST | `/interrupt` | AdminHandlers | bearer / cookie + authz (`__HARBOR_ADMIN__:sessions:interrupt`) | body: `{sessionId}` — interrupts current query if any |
| 23 | GET | `/` | UiHandlers (LoginWrapper) | none (renders the wrapper; cookie issuance happens via separate `POST /auth/login`) | harbor-specific login page; if cookie present, redirects to `/ui/` |
| 24 | GET | `/ui/.*` | UiHandlers (proxy) or BundledAssets | none (assets are static) | the unmodified upstream UI; matches before catch-all |
| 25 | GET | `/.*` | UiHandlers (proxy) or BundledAssets | none (assets are static) | **MUST be registered last**; serves root-namespace UI assets (favicon, manifest, etc.). Unmatched non-UI paths return `404` — the catch-all does NOT proxy unknown API-shaped paths upstream. |

**Public routes (no auth):** `OPTIONS /quack`, `GET /info`, `GET /health`,
`GET /ready`, `GET /.*` (UI assets). `GET /localToken` is conditionally
public on localhost-bound deployments only.

**All other routes require authentication.** See §7.

## 5. Wire protocols

### 5.1 Quack RPC (`POST /quack`)

Inherited from `duckdb-quack` v1.5-variegata. Reference:
`misc/duckdb-quack/rpc.pdf` and `src/quack_message.{cpp,hpp}` in
upstream. We do not modify the wire format. Message types:

```
CONNECTION_REQUEST     ← client provides auth token
CONNECTION_RESPONSE    → server returns connection_id
PREPARE_REQUEST        ← client sends SQL
PREPARE_RESPONSE       → server returns column types + first chunk batch + result_uuid
FETCH_REQUEST          ← client requests next batch (parallelizable)
FETCH_RESPONSE         → server returns next batch or end
APPEND_REQUEST         ← client sends DataChunk for INSERT
SUCCESS_RESPONSE       → server ack
DISCONNECT_MESSAGE     ← client closes session
ERROR_RESPONSE         → on any failure
```

All encoded with DuckDB's `BinarySerializer`
(`SerializationCompatibility::FromIndex(7)`). Header is
`{type, connection_id}`.

Compatibility: harbor targets **upstream Quack at branch
`v1.5-variegata`** as of the rebase point. We document the upstream
commit hash in `BUILD.md`. Wire-format breaking changes from upstream
are tracked and absorbed at rebase time. We do not promise compatibility
with arbitrary future Quack versions until DuckDB v2.0 GA.

### 5.2 JSON SQL (`POST /sql`)

Designed for app code. Single round trip for small queries, NDJSON
streaming for large ones.

#### Request

```json
{
  "sql": "SELECT * FROM users WHERE id = $1",
  "params": [42],
  "sessionId": "9f3c..."
}
```

| Field | Required | Type | Notes |
|---|---|---|---|
| `sql` | yes | string | A single SQL statement. Multi-statement requests are rejected with `errorCode: BAD_REQUEST`. |
| `params` | no | array | Positional parameters bound as `$1`, `$2`, … . Encoded per §5.4. |
| `sessionId` | no | string | If present, must match a live DB session owned by the authenticated principal. Sticky state (`BEGIN`/`COMMIT`, `SET`, temp tables, prepared statements) survives between requests with the same `sessionId`. If absent, a fresh ephemeral DB session is created and torn down at end of request. |

**Transactions and ephemeral sessions.** A request without a `sessionId`
runs in an ephemeral session that is destroyed at end of request.
Issuing `BEGIN` (or any statement that opens a transaction) in such a
request is rejected with `errorCode: BAD_REQUEST` and message
`"transactions require an explicit sessionId; create one with POST /sql/sessions/new"`.
This prevents transactions from being silently abandoned.

To run a multi-statement transaction:

```bash
SID=$(curl -sf -X POST .../sql/sessions/new \
        -H "Authorization: Bearer $TOKEN" | jq -r .sessionId)
curl -X POST .../sql -d "{\"sql\":\"BEGIN\",\"sessionId\":\"$SID\"}" ...
curl -X POST .../sql -d "{\"sql\":\"INSERT ...\",\"sessionId\":\"$SID\"}" ...
curl -X POST .../sql -d "{\"sql\":\"COMMIT\",\"sessionId\":\"$SID\"}" ...
curl -X DELETE .../sql/sessions/$SID                                  # optional cleanup
```

Headers:
- `Authorization: Bearer <token>` — required if a token is configured (see §7)
- `Cookie: harbor_session=<…>` — accepted as an alternative to bearer
- `Accept: application/x-ndjson` (default) or `Accept: application/json` (force one-shot non-streamed)
- `X-Harbor-Session-Id: <id>` — alternative to `sessionId` in body

#### Response — NDJSON streaming (default)

Chunked transfer encoding, one JSON object per line:

```ndjson
{"type":"schema","sessionId":"9f3c...","columns":[{"name":"id","duckdbType":"INTEGER"},{"name":"name","duckdbType":"VARCHAR"}]}
{"type":"row","values":[1,"Alice"]}
{"type":"row","values":[2,"Bob"]}
{"type":"end","rowCount":2,"timeMs":4}
```

Chunk-bundled mode (`Accept: application/x-ndjson; shape=chunk`):

```ndjson
{"type":"schema",...}
{"type":"chunk","rows":[[1,"Alice"],[2,"Bob"]]}
{"type":"end","rowCount":2,"timeMs":4}
```

Mid-stream errors:

```ndjson
{"type":"schema",...}
{"type":"row",...}
{"type":"error","code":"DUCKDB_OUT_OF_MEMORY","message":"...","detail":{...}}
```

The HTTP status is `200` once headers are sent; mid-stream errors
**cannot** change it. Clients must read until `{"type":"end"}` or
`{"type":"error"}`.

#### Response — one-shot JSON (`Accept: application/json`)

```json
{
  "ok": true,
  "kind": "select",
  "sessionId": "9f3c...",
  "columns": [{"name":"id","duckdbType":"INTEGER"}, {"name":"name","duckdbType":"VARCHAR"}],
  "data": [[1,"Alice"],[2,"Bob"]],
  "rowCount": 2,
  "timeMs": 4
}
```

`kind` is `"select"` when the statement produced columns and `"write"`
otherwise (DDL, DML without RETURNING, etc.).

#### Error envelope (HTTP 4xx/5xx, never mid-stream)

```json
{
  "ok": false,
  "error": "Table 'users' does not exist",
  "errorCode": "TABLE_NOT_FOUND",
  "errorDetails": {"table":"users","schema":"main"},
  "sessionId": "9f3c...",
  "timeMs": 1
}
```

`errorCode` is a small stable enum: `SQL_ERROR`, `SQL_SYNTAX`,
`TYPE_ERROR`, `TABLE_NOT_FOUND`, `IO_ERROR`, `BAD_REQUEST`,
`UNAUTHORIZED`, `FORBIDDEN`, `SESSION_BUSY`, `SESSION_NOT_FOUND`,
`SESSION_LIMIT`, `INTERNAL`. The full table lives in
[`docs/ERRORS.md`](./docs/ERRORS.md).

#### `/sql` parameter encoding (request)

Two modes, picked by request shape:

**Mode A — implicit (default).** `params` is a JSON array of bare
values. Server prepares the statement, inspects expected parameter
types, and coerces each JSON value:

| JSON | Coerced to (per expected DuckDB type) |
|---|---|
| `null` | NULL |
| `true`/`false` | BOOLEAN |
| number (integer-shaped, ≤ `Number.MAX_SAFE_INTEGER`) | TINYINT/SMALLINT/INTEGER/BIGINT/UBIGINT etc. |
| number (float) | FLOAT/DOUBLE |
| string | VARCHAR, or parsed into DECIMAL/UUID/DATE/TIME/TIMESTAMP/INTERVAL/BLOB(base64)/JSON per expected type |
| array | LIST/ARRAY (recursively coerced) |
| object | STRUCT (field-keyed, recursively coerced) |

Unsafe-magnitude integers (above `2^53`) **must** be sent as strings;
silently truncating is rejected with `BAD_REQUEST`.

**Mode B — typed wrapper (explicit).** Use when the prepared
statement has no expected type to coerce against (e.g. `SELECT $1`),
or when you want to be unambiguous:

```json
{
  "sql": "SELECT $1, $2",
  "params": [
    {"type": "DECIMAL(38,4)", "value": "12345678901234.5678"},
    {"type": "INTERVAL", "value": {"months": 14, "days": 3, "micros": "123456789"}}
  ]
}
```

Wrapper objects always have `{type, value}`. The `type` string is
DuckDB's `LogicalType::ToString()` form (e.g. `INTEGER`,
`DECIMAL(W,S)`, `TIMESTAMPTZ`, `LIST<INTEGER>`). The `value` is encoded
per §5.4. Explicit-typed `NULL` is `{type: "...", value: null}`.

Mixing modes within a single `params` array is allowed.

When the prepared statement reports an `ANY`/unknown parameter type
(e.g. bare `SELECT $1` with no cast), Mode A falls back to:

| JSON shape | Inferred DuckDB type |
|---|---|
| `null` | NULL (typed as VARCHAR for catalog purposes) |
| boolean | BOOLEAN |
| integer-shaped number | BIGINT |
| float number | DOUBLE |
| string | VARCHAR |
| array, object | typed wrapper required (Mode B) |

If the fallback is unsuitable, use Mode B's typed wrapper.

### 5.3 DuckDB UI (`POST /ddb/...`)

Inherited from `duckdb-ui` http_server.cpp. We do not modify the wire
format. The four message types are `SuccessResult`, `ErrorResult`,
`EmptyResult`, `TokenizeResult`, all encoded with DuckDB's
`BinarySerializer`, response Content-Type `application/octet-stream`.

UI request parameters arrive as **HTTP headers**, not body fields. The
canonical set:

| Header | Used by |
|---|---|
| `X-DuckDB-UI-Connection-Name` | identifies the UI's per-tab connection. Mapped (per authenticated principal) to a harbor DB session by `UiHandlers` — see §6. |
| `X-DuckDB-UI-Database-Name` (base64) | sets `USE` database for this query |
| `X-DuckDB-UI-Schema-Name` (base64) | sets schema |
| `X-DuckDB-UI-Parameter-Count`, `X-DuckDB-UI-Parameter-Value-N` (base64) | positional params |
| `X-DuckDB-UI-Result-Row-Limit` | row cap |
| `X-DuckDB-UI-Result-Database-Name`, `-Schema-Name`, `-Table-Name` | "save result as table" feature |
| `X-DuckDB-UI-Errors-As-JSON` | toggle DuckDB's JSON-formatted error mode |

The UI bundle is taken **unchanged** from upstream `duckdb-ui` for the
`/ddb/*` request shape. harbor adds an auth-cookie requirement on top:
the UI obtains the cookie via `POST /auth/login`, then `/ddb/*`
requests carry both the cookie and an `Origin` matching the local URL.
The bundled UI ships with a small harbor-specific login wrapper at
`GET /` that handles the token-paste and cookie issuance before the
unmodified UI bundle takes over — see §8. This is the only modification
to the UI surface.

### 5.4 Type encoding (NDJSON `/sql`)

The `schema` line is the authoritative type record. Each column entry
has at minimum `{name, duckdbType}`; nested types add child-type
metadata so the row decoder can recurse.

#### Schema column shape

```json
{
  "name": "col",
  "duckdbType": "DECIMAL(18,2)",
  "lossless": true,                  // false for extension types we fall back to VARCHAR

  // For DECIMAL:
  "decimal": {"width": 18, "scale": 2},

  // For LIST<T> / ARRAY<T,N>:
  "child": { ... recursive column shape, no name ... },
  "arrayLength": 4,                  // ARRAY only

  // For STRUCT:
  "fields": [{"name":"a", ...recursive}, {"name":"b", ...recursive}],

  // For MAP<K,V>:
  "keyType":   { ... recursive },
  "valueType": { ... recursive },
  "encoding": "pairs",

  // For UNION:
  "members": [{"name":"a", ...recursive}, {"name":"b", ...recursive}],

  // For ENUM:
  "values": ["draft","submitted","approved"],

  // For extension types (GEOMETRY, etc):
  "extension": "spatial",
  "encoding":  "wkb-base64"
}
```

#### Row encoding rules

| DuckDB type | JSON representation | Round-trip notes |
|---|---|---|
| `NULL` | `null` | |
| `BOOLEAN` | `true` / `false` | |
| `TINYINT`, `SMALLINT`, `INTEGER`, `UTINYINT`, `USMALLINT`, `UINTEGER` | JSON number | safe in IEEE-754 |
| `BIGINT`, `UBIGINT`, `HUGEINT`, `UHUGEINT` | **string** | JS-safe; e.g. `"9223372036854775807"` |
| `FLOAT`, `DOUBLE` | JSON number; `NaN`/±Infinity as strings `"NaN"`/`"Infinity"`/`"-Infinity"` | |
| `DECIMAL(W,S)` | string | preserves width/scale; e.g. `"12345.6789"` |
| `VARCHAR` | JSON string | |
| `UUID` | canonical lowercase string | `"550e8400-e29b-41d4-a716-446655440000"` |
| `DATE` | `"YYYY-MM-DD"` | |
| `TIME` | `"HH:MM:SS.ffffff"` | up to 6 fractional digits |
| `TIME WITH TIME ZONE` (`TIMETZ`) | `"HH:MM:SS.ffffff+HH:MM"` | RFC 3339 time form |
| `TIMESTAMP` | `"YYYY-MM-DDTHH:MM:SS.ffffff"` | **no** trailing `Z` (no TZ semantics) |
| `TIMESTAMP_S`, `TIMESTAMP_MS`, `TIMESTAMP_NS` | same form, fractional digits sized to precision (0 / 3 / 9) | |
| `TIMESTAMP WITH TIME ZONE` (`TIMESTAMPTZ`) | RFC 3339 in UTC, `"YYYY-MM-DDTHH:MM:SS.ffffffZ"` | |
| `INTERVAL` | object: `{"months":N, "days":N, "micros":"N"}` | matches DuckDB's `interval_t` (micros as string for precision) |
| `BLOB` | base64 string | |
| `BIT` | string of `'0'`/`'1'` | |
| `JSON` | string containing canonical JSON text | not raw nested JSON, to disambiguate from SQL NULL |
| `LIST<T>` | JSON array, recursive | |
| `ARRAY<T,N>` | JSON array of length N, recursive | |
| `STRUCT` | JSON object keyed by field name, recursive | schema lists field types |
| `MAP<K,V>` | array of `[K,V]` pairs | not object — keys can be non-string |
| `ENUM` | string label | |
| `UNION` | object: `{"tag":"member_name","value":...}` | tag is the active member |
| `GEOMETRY` (spatial extension) | base64 WKB | `schema.encoding="wkb-base64"`, `schema.extension="spatial"` |

**Round-trip promise:** harbor NDJSON is lossless for **every DuckDB
core logical type listed above** when decoded using the schema record.
Extension logical types not listed here fall back to a string
representation derived from `CAST(... AS VARCHAR)` and are explicitly
marked `"lossless": false` in the schema. For full-fidelity column-store
interchange across DuckDB versions, use `/quack` (BinarySerializer); a
future `Accept: application/vnd.apache.arrow.stream` mode on `/sql` is
on the roadmap.

The complete `LogicalTypeId` matrix for DuckDB v1.5.3 is enumerated in
`test/types/` — every entry has a round-trip test.

### 5.5 Convenience routes

- `GET /health` — `{ "ok": true, "version": "0.1.0", "uptime_s": 1234 }`.
  Public. No DB info, no DB path, no token, no extension list, no
  bind address, no auth principal — exactly the four fields shown.
  Anything else risks information disclosure on a remote-bound deploy.
- `GET /ready` — runs `SELECT 1` against a worker connection.
  Returns `200 {"ok":true}` on success, `503 {"ok":false}` on failure.
  Public, but **no error detail**: a "ready" probe shouldn't be the
  vector that leaks "the disk is full" or "the file is locked at
  /var/lib/foo/baz.duckdb" to anyone who can curl the port. Operators
  diagnose readiness failures via `/health`, the logs, and `/whoami`
  (which is auth-gated).
- `GET /whoami` — JSON projection of quack's `whoami()` macro:
  `{name, provider, hostname, region, uptime_s, ts_now, meta:{duckdb_version, platform, harbor_version, ...}}`.
- `GET /tables` — `{ "tables": ["users", "orders", ...] }`.
- `GET /schema/:db/:table` — `{ "columns": [{name,type,nullable,default}] }` or 404.
- `POST /checkpoint` — `{ "ok": true, "wal_size_bytes": N }`.
- `GET /sessions` — admin authz only:
  `{ "sessions": [{id, principal, created_at, last_used_at, queries_run, in_flight: bool}] }`.
- `POST /interrupt` — admin authz only: body `{sessionId}`, calls
  `Connection::Interrupt()` on the matching session, returns
  `{ok:true, was_running: bool}`.

## 6. Sessions and concurrency

### Three layers, kept separate

harbor distinguishes three concepts that are easy to conflate. Each has
its own identifier and lifetime:

| Concept | Identifier | Lifetime | Purpose |
|---|---|---|---|
| **Auth principal** | `principal_id = hex(sha256(client_token))` — see derivation rule below | per token | Identity. The "who". |
| **Auth cookie** | HMAC-signed opaque blob containing `(principal_id, expires_at, nonce)` | configurable, default 12h | Browser convenience. Lets the UI authenticate without re-presenting the token on every request. Server-side the cookie is stateless (HMAC-verified, not looked up). |
| **DB session** | UUID, generated by `SessionManager` | until explicit destroy or idle TTL | DuckDB `Connection` + state (transactions, temp tables, SET variables, prepared statements). Owned by exactly one `principal_id` at creation time. |

#### Principal identity derivation

`harbor_authentication_function` returns BOOLEAN — it can't *report* an
identity. harbor derives a deterministic principal identity from the
*credential the caller presented*:

```
principal_id = hex(sha256(client_token))
```

Properties of this scheme:

- **Deterministic.** Same token → same principal across processes,
  independent of cookie state.
- **No raw-token storage.** harbor never stores or logs the raw token
  or its full hash; logs use the first 8 hex chars as a non-reversible
  abbreviation.
- **Cross-principal isolation.** A request that presents principal A's
  token but principal B's `sessionId` is rejected — `SessionManager`
  compares the lookup's derived `principal_id` against the session's
  recorded `owner_principal_id`.
- **Backward-compatible with upstream Quack hooks.** The
  `harbor_authentication_function`/`quack_authentication_function`
  callback signature is unchanged.
- **Cookie principal == bearer principal** for the same token, because
  both derive from the same `sha256(client_token)`. Logging in via
  `/auth/login` gives you a cookie that addresses the same DB sessions
  a bearer-token caller with the same token would.

**Future extension** (post-v0.1): an optional
`harbor_principal_function(client_token) -> VARCHAR` hook can override
the derivation rule, letting deployments map multiple tokens to one
principal name (e.g. team-shared credentials). Not in v0.1.

#### Unknown / foreign session ID returns 404

A request that presents a `sessionId` the server doesn't recognize
*or* a `sessionId` not owned by the current principal returns
`404 SESSION_NOT_FOUND` (not `403`). This prevents session-id
enumeration: an attacker cannot probe which IDs exist by observing
403-vs-404 differences.

Cross-concept rules:

- **One auth cookie can map to many DB sessions.** A browser tab opens
  the UI, gets a cookie, then accumulates DB sessions one per UI
  connection name (per `X-DuckDB-UI-Connection-Name`).
- **A DB session is owned by exactly one principal.** A request that
  presents a different principal's credentials but the right `sessionId`
  is rejected with `UNAUTHORIZED`. Session IDs are not capability
  tokens.
- **The UI's `X-DuckDB-UI-Connection-Name` is mapped to a DB session id
  scoped by principal.** `UiHandlers` keeps a `(principal_hash,
  ui_connection_name) → db_session_id` map. Different principals using
  the same `ui_connection_name` get different DB sessions.

### Session lifecycle

| Event | Effect |
|---|---|
| `CONNECTION_REQUEST` (Quack), or `POST /sql/sessions/new`, or first `POST /ddb/run` for a new `(principal, ui_connection_name)`, or `POST /sql` with no `sessionId` | Create DB session, return id |
| `POST /sql` with `sessionId` not owned by current principal | `403 UNAUTHORIZED`, no leak |
| Idle longer than `harbor_session_ttl_s` (default 3600) | Session destroyed at next 60s sweep |
| Explicit `DISCONNECT_MESSAGE` (Quack), or `DELETE /sql/sessions/:id` | Destroyed immediately |
| `harbor_stop` or process restart | All sessions vanish (in-memory only) |
| `POST /auth/logout` with `?destroy_sessions=true` | All DB sessions owned by this principal destroyed |

### Concurrency rule

**A single DB session serializes its own requests.** The session mutex
is held for the full duration of any request that touches it,
including the streaming response body. Reasons:

- DuckDB `Connection` is not thread-safe across simultaneous queries.
- Transaction state (`BEGIN`/`COMMIT`) makes interleaving undefined.
- Quack's parallel `FETCH` model is for *result-set* parallelism within
  one query, not for two queries on one session.

A second request that arrives for the same `sessionId` while the first
is in-flight is rejected with HTTP `409` and
`errorCode: SESSION_BUSY`. Clients can retry. The cost of rejecting is
much smaller than the bug surface of allowing.

### Limits

| Setting | Default | Effect when exceeded |
|---|---|---|
| `harbor_max_sessions` | 1024 | new session creation returns `429 SESSION_LIMIT` |
| `harbor_session_ttl_s` | 3600 | swept on next 60s tick |
| `harbor_max_response_rows` | 0 (unlimited) | `/sql` truncates; trailer reports `truncated: true` |
| `harbor_max_request_body_bytes` | 256 MiB | matches the nginx/Caddy reverse-proxy guidance for `/quack` (APPEND payloads can be large) |
| `harbor_query_timeout_s` | 0 (no limit) | per-query timeout via `Connection::Interrupt()` |
| `harbor_auth_cookie_ttl_s` | 43200 (12h) | cookies past this `expires_at` are rejected |

### Cancellation

Two paths to cancel:

1. **Client disconnect** during `/sql` streaming. `HarborHttpServer`
   detects the broken pipe and calls `Connection::Interrupt()` on the
   bound session, then unwinds the chunk loop.
2. **Explicit `POST /interrupt`** or `POST /sql/cancel`. Authz hook
   gated. Calls `Connection::Interrupt()` on the named session.

For Quack, `DISCONNECT_MESSAGE` clears the connection's pending result
and `Connection::Interrupt()` is invoked if a query is in flight (this
is a harbor addition; upstream just clears the result).

## 7. Authentication and authorization

### Three auth modes

The auth posture is determined entirely by the `token` argument to
`harbor_serve`. No separate `mode` enum and no `harbor_local_dev_mode`
SQL setting; SET on `harbor_local_dev_mode` hard-errors with a
migration message pointing at `token := NULL`.

| Form | Mode | Behavior |
|---|---|---|
| `harbor_serve('uri')` | **3 (random)** | Auto-generate a 16-byte hex static token. Default authn (`harbor_check_token` compares request token against this generated value). Returned in the result row. |
| `harbor_serve('uri', token := 'x')` | **2 (static)** | Operator-supplied static token. Default authn. |
| `harbor_serve('uri', token := NULL)` | **1 (open dev)** | Unauthenticated. Refuses to start unless the bind is loopback. ALL HTTP routes (`/sql`, `/quack`, `/ddb/*`, `/localEvents`, UI) accept any caller and assign the synthetic principal `harbor.local-dev`. |
| `harbor_serve('uri', token := '')` | — | **Hard error.** Empty string is almost always an env-var-plumbing accident; rejected loudly with a migration-teaching message. |
| Custom `harbor_authentication_function` set + any `token` argument | — | **Hard error.** The custom callback decides validity; a static token would be dead config that misleads operators. Either omit `token` (callback decides) or unset the custom callback (use static-token auth). |

#### Authn function snapshot

The resolved values of `harbor_authentication_function` and
`harbor_authorization_function` (with `quack_*` fallbacks plus the
`harbor_check_token` / `harbor_nop_authorization` defaults) are
captured ONCE at `harbor_serve` startup and used for the running
server's lifetime. `SET GLOBAL` on these settings while a server is
running has NO effect until the next `harbor_serve`. Without this
snapshot, an authenticated SQL caller could change the authn
function mid-process and broaden auth for everyone else.

#### Authorization is orthogonal

All three modes pair freely with `harbor_authorization_function`
(default `harbor_nop_authorization` returns true for everything
except `__HARBOR_ADMIN__:resource:action` synthetic queries, which
are default-deny unless `harbor_allow_admin_without_authz=true`).
Production-grade multi-tenant deployment = Mode 3 (custom authn) +
custom authz; see [`examples/auth/`](examples/auth/) for recipes.

### Threat model

harbor exposes the **full SQL surface** of the underlying DuckDB,
including read/write of every table and `ATTACH` to remote sources. The
auth token effectively grants superuser access to the database file.
Treat it as a database password.

What this means concretely, with **default permissive auth**
(`harbor_check_token` + `harbor_nop_authorization`, which is what
`harbor_serve` ships with):

| Threat | Protected by harbor? |
|---|---|
| Network attackers without a valid bearer / cookie / X-Harbor-Token | ✅ — every authenticated endpoint requires a credential, with HMAC-signed cookies for browser flows |
| Cross-site request forgery via ambient cookie | ✅ — `/sql` and `/ddb/*` cookie path requires `Origin` in `harbor_cors_origins` (or same-origin for `/ddb/*`); `/localEvents` rejects cross-origin Origin |
| Bearer-leak in the UI proxy | ✅ — `HandleProxyGet` strips `Cookie`, `Authorization`, `X-Harbor-*`, `Origin`, `Sec-*` before forwarding upstream (PR-8 invariant) |
| Authenticated principal running `SET GLOBAL` to grant themselves admin access | ❌ — see below |
| Authenticated principal running `ATTACH '/path/to/anything'`, `LOAD 'arbitrary.duckdb_extension'`, `COPY (...) TO 'file://...'`, etc. | ❌ — these are normal SQL that the default authz callback allows |
| Authenticated principal reading and modifying any table | ❌ — by design; that's the whole point of the SQL surface |

**Bearer-authenticated `/sql` is effectively superuser unless you
configure `harbor_authorization_function` to constrain the SQL
content.** `harbor_allow_admin_without_authz` and the default-deny
on admin endpoints are operator convenience, not a security wall:

- **What default-deny on admin IS:** a safety net for operators who
  haven't yet configured `harbor_authorization_function`. They get
  `403 FORBIDDEN` on `/tables`, `/sessions`, `/checkpoint`, etc.
  instead of those endpoints silently working for any authenticated
  caller. Less surprise during initial setup.
- **What default-deny on admin IS NOT:** a barrier against an
  authenticated bearer that already has `/sql` access. That bearer
  can run `SET GLOBAL harbor_allow_admin_without_authz = TRUE`
  through `/sql` itself and unlock every admin endpoint. The flag
  is in DuckDB's normal settings space; SQL controls it.

This mirrors every other database with a SQL surface: PostgreSQL,
MySQL, SQLite, and DuckDB itself all assume a "trusted SQL caller"
threat model. The auth model promises *who* can issue SQL; it does
not promise *what SQL* they can issue, except via the explicit
`harbor_authorization_function` hook.

**Production deployments must configure `harbor_authorization_function`**
to gate both:

1. Admin endpoints — by checking `__HARBOR_ADMIN__:resource:action`
   queries against an RBAC table (or equivalent); see
   [`examples/auth/rbac-authorization.sql`](examples/auth/rbac-authorization.sql).
2. Dangerous SQL on `/sql` — by inspecting the SQL text and
   rejecting `SET GLOBAL`, `ATTACH`, `LOAD`, `INSTALL`, and
   `COPY (...) TO 'file://...'` from non-admin principals. Pair
   with one of the [`bearer-*.sql`](examples/auth/) recipes for
   per-principal token bookkeeping.

For the operator-facing rollout checklist, see
[`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md) §3 "Harden". For
copy-paste recipes, see [`examples/auth/`](examples/auth/).

### Defaults

| Concern | Default |
|---|---|
| Bind address | `127.0.0.1:9494` |
| Token | random 16-byte hex generated by `harbor_serve` and returned in the result row |
| Authentication callback | `harbor_check_token` (compares to the served token; alias `quack_check_token` retained for compat) |
| Authorization callback | `harbor_nop_authorization` (always allows; alias `quack_nop_authorization` retained) |
| Origin policy on `/ddb/*` | Origin must equal local URL **AND** valid auth cookie or bearer token |
| `/localToken` | enabled only when bound to localhost |
| CORS allowed origins | empty (no cross-origin requests permitted) |

### Auth credentials

Three accepted forms of authentication on a request:

1. **`Authorization: Bearer <token>`** — for `/quack` (via
   `CONNECTION_REQUEST.AuthString`), `/sql`, `/auth/login`, admin
   endpoints, programmatic tools.
2. **`Cookie: harbor_session=<signed>`** — issued after a successful
   token exchange at `POST /auth/login`. The cookie is HMAC-signed
   server-side (no server-side cookie lookup), `HttpOnly;
   SameSite=Strict; Secure` (when behind HTTPS, detected via
   `X-Forwarded-Proto: https`). The signing key is auto-generated
   per-process at server start (32 random bytes from
   `RAND_bytes`) and held only in memory — so a process restart
   invalidates every outstanding cookie ("a restart logs everyone
   out"). v0.1 deliberately exposes **no SQL setting** for the
   signing key: setting it via SQL would let any authorized SQL
   caller read the secret and mint cookies, which expands the SQL
   surface's blast radius. v0.2+ may introduce
   `HARBOR_COOKIE_SIGNING_KEY` as an environment variable for
   operators who need cookie continuity across rolling restarts.

   The cookie payload is a dotted, version-tagged base64url
   structure:

   ```
   v1 . b64url(principal_hex)
      . b64url(expires_unix_ascii)
      . b64url(nonce16)
      . b64url(hmac32)
   ```

   The HMAC is computed over the **exact ASCII bytes** of the first
   three encoded segments joined by `.` (`v1.<seg1>.<seg2>.<seg3>`),
   so verification recomputes over the on-the-wire prefix and
   constant-time-compares against the decoded fourth segment. The
   payload contains no secrets — `principal_hex` is already a
   one-way hash of the token — so HMAC + plaintext is sufficient
   (no AEAD needed; authenticity and expiry are the requirements).
3. **`X-Harbor-Token: <token>`** — accepted as an alternative to
   `Authorization: Bearer` for environments where the latter is awkward.

### Auth flow

```
┌─ POST /auth/login --------------------------------┐
│ Authorization: Bearer <token>                     │
│ → 200 OK                                          │
│   Set-Cookie: harbor_session=<signed>; HttpOnly... │
│   {"principal":"<hash>","expires_at":"..."}       │
└──────────────────────────────────────────────────┘
                       │
                       ▼
┌─ POST /sql / GET /tables / POST /ddb/run / etc. ──┐
│ Cookie: harbor_session=<signed>                    │
│   OR Authorization: Bearer <token>                │
│ → handler verifies HMAC OR re-runs                │
│   harbor_authentication_function                   │
└──────────────────────────────────────────────────┘
```

### Authorization

The authorization callback is invoked **on every SQL-bearing request**,
including admin endpoints (using synthetic SQL):

| Request | `query` argument to `harbor_authorization_function` |
|---|---|
| `/quack` `PREPARE_REQUEST` | the user's SQL |
| `/quack` `APPEND_REQUEST` | `INSERT INTO <schema>.<table> VALUES (NULL)` (synthetic; matches upstream Quack) |
| `/sql` | the user's SQL |
| `/ddb/run` | the user's SQL |
| `/checkpoint` | `__HARBOR_ADMIN__:checkpoint:create` |
| `/sessions` (GET) | `__HARBOR_ADMIN__:sessions:list` |
| `/sql/sessions/new` (POST) | `__HARBOR_ADMIN__:sessions:create` |
| `/sql/sessions/:id` (DELETE) | `__HARBOR_ADMIN__:sessions:delete` (the destroyed session id is **not** appended; identifying which session is in the request body / URL but is not part of the policy string, to avoid path-injection into authz strings) |
| `/interrupt` | `__HARBOR_ADMIN__:sessions:interrupt` |
| `/sql/cancel` | `__HARBOR_ADMIN__:sessions:cancel` |
| `/whoami` | `__HARBOR_ADMIN__:server:whoami` |
| `/tables` | `__HARBOR_ADMIN__:catalog:list_tables` |
| `/schema/:db/:table` | `__HARBOR_ADMIN__:catalog:describe_table` (path params are NOT interpolated into the authz string; same reason) |

The synthetic strings use a stable `__HARBOR_ADMIN__:<resource>:<action>`
shape so authz macros can pattern-match on the resource axis:

```sql
CREATE MACRO authz(sid, query) AS (
  CASE
    WHEN starts_with(query, '__HARBOR_ADMIN__:sessions:') THEN sid IN (SELECT * FROM ops_team)
    WHEN starts_with(query, '__HARBOR_ADMIN__:checkpoint:') THEN sid IN (SELECT * FROM ops_team)
    WHEN starts_with(query, '__HARBOR_ADMIN__:server:') THEN true
    WHEN starts_with(query, '__HARBOR_ADMIN__:catalog:') THEN true
    WHEN starts_with(query, '__HARBOR_ADMIN__:') THEN false  -- new admin verbs are deny-by-default
    ELSE starts_with(upper(trim(query)), 'SELECT')
  END
);
```

#### Admin authorization is default-deny when no hook is configured

When `harbor_authorization_function` is the default `harbor_nop_authorization`
(which always returns true), admin endpoints are still gated by an
internal default-deny rule on `__HARBOR_ADMIN__:` strings, **unless**
`harbor_allow_admin_without_authz = true` is explicitly set. Reasons:

- The default authz is permissive for ergonomic dev experience on
  `/sql` and `/quack`. That convenience must NOT extend to admin
  operations like `CHECKPOINT` or session interruption.
- Operators who set a custom authz function but forget to handle the
  `__HARBOR_ADMIN__:` prefix get default-deny on admin (safe), not
  accidental allow.
- Setting `harbor_allow_admin_without_authz = true` is the explicit
  opt-in for "I really do want unrestricted admin access on this
  trusted-network deployment." Logged loudly at server start.

The authz hook also returns deny on:
- exception thrown
- NULL returned
- non-BOOLEAN returned

Synthetic admin strings are **never** accepted from client request
bodies. The `query` field of `/sql` requests is rejected with
`BAD_REQUEST` if it begins with `__HARBOR_ADMIN__:`. Path parameters
to admin handlers are NOT concatenated into the authz string (see
table above) — concrete identifiers go in the request envelope, not
the policy decision input.

The hook signature, identical to upstream Quack:

```sql
harbor_authorization_function(sid VARCHAR, query VARCHAR) -> BOOLEAN
```

Returning `false` (or throwing) rejects with `403 FORBIDDEN`. Operators
discriminate admin operations by pattern-matching on the `__HARBOR_ADMIN__:`
prefix:

```sql
CREATE MACRO authz(sid, query) AS (
  CASE
    WHEN starts_with(query, '__HARBOR_ADMIN__:')
      THEN sid IN (SELECT sid FROM admins_table)
    ELSE
      starts_with(upper(trim(query)), 'SELECT')
  END
);
SET GLOBAL harbor_authorization_function = 'authz';
```

`harbor_authentication_function` and `harbor_authorization_function` are
SQL macros, scalar UDFs, or extension-registered functions. They run
in a fresh, transient server-side connection per call — they cannot
rely on session-local state.

### Browser-origin requests do NOT bypass auth

Upstream `duckdb-ui`'s `Origin == local_url` check is **CSRF defence,
not authentication**. We retain the Origin check as a CSRF gate but
**always also require** a valid auth cookie or bearer token. The
browser UI bootstraps this via harbor's small login wrapper at `GET /`:

1. User opens `http://localhost:9494/`.
2. harbor's wrapper detects no `harbor_session` cookie and serves a
   minimal login page.
3. User pastes the token printed by `harbor_serve`.
4. The page POSTs the token to `/auth/login`, gets
   `Set-Cookie: harbor_session=<signed>`.
5. The page redirects to `/ui/` (or wherever the bundled DuckDB UI
   entry lives), which loads the upstream UI bundle unmodified.
6. All subsequent `/ddb/*` requests carry the cookie. The `Origin`
   header is still verified.

For local dev with `bind_to == 127.0.0.1` only, `harbor_local_dev_mode =
true` (default off) skips steps 2-4 and accepts requests from the local
URL without a token. Off by default to prevent accidental exposure.

### When binding non-locally

When `harbor_serve` is called with a non-loopback host:

- A loud startup warning is logged.
- `/localToken` is automatically disabled (returns `404`).
- `harbor_local_dev_mode` is forced off regardless of setting.
- The default authorization function logs a warning unless overridden.

Recommended posture for production: front with nginx or Caddy doing
TLS termination (recipe in [`docs/REVERSE_PROXY.md`](./docs/REVERSE_PROXY.md))
and either keep `bind_to = 127.0.0.1` with the proxy on the same host,
or use firewall rules to restrict reach.

### CORS

harbor blocks all cross-origin browser requests by default. To enable
specific origins (e.g. a React app on `https://app.example.com` calling
`/sql` directly):

```sql
SET GLOBAL harbor_cors_origins = 'https://app.example.com,https://other.example.com';
```

When `harbor_cors_origins` is non-empty:

- `OPTIONS` preflight is honored on `/sql`, `/quack`, `/auth/*` for
  listed origins.
- Allowed headers: `Authorization`, `Content-Type`, `X-Harbor-Token`,
  `X-Harbor-Session-Id`, `Accept`.
- `Access-Control-Allow-Origin` is set to the **specific** matching
  origin from the allow-list, **never** `*`. Wildcard origin combined
  with credentials is forbidden by spec and by us — the server refuses
  to start if `harbor_cors_origins='*'`.
- `Access-Control-Allow-Credentials: true` is sent only when the
  request origin matches an entry in the allow-list.

### Identifier safety in admin handlers

Admin routes that interpolate path parameters (`GET /schema/:db/:table`)
**must** quote those parameters as DuckDB identifiers using
`KeywordHelper::WriteOptionallyQuoted` (or pass them as bound values
when a function-shaped alternative exists, e.g. `pragma_table_info($1)`).
Never string-concatenate path params into SQL. This invariant is
codified in `AGENTS.md` and verified by an integration test.

## 8. UI assets

harbor serves the UI HTML/JS/CSS at `GET /.*` from one of two modes
(v0.1; see "Future modes" below for what's planned post-v0.1), selected
by setting `harbor_ui_assets`:

| Mode | Setting | Behavior |
|---|---|---|
| `proxy` (default) | `harbor_ui_assets = 'proxy'` | Forwards `GET /.*` to `ui.duckdb.org` (matches upstream `duckdb-ui` behavior). Requires outbound network from the host (or from `harbor_ui_proxy_url` if pointed at an internal mirror). UI updates take effect immediately, no rebuild needed. |
| `disabled` | `harbor_ui_assets = 'disabled'` | All `GET /.*` requests return `404`. The UI doesn't load. `/sql` and `/quack` still work. Useful for headless deployments. |

The proxy URL is configurable via `harbor_ui_proxy_url` (default
`https://ui.duckdb.org`) for testing, mirroring, or pointing at a fork.
Air-gapped or restricted-egress deployments are expected to self-host
an HTTP mirror of `ui.duckdb.org` (a 10-line nginx config) and point
`harbor_ui_proxy_url` at it. That covers the primary `bundled`-mode use
case with significantly less complexity in harbor itself.

### Header invariants on the proxy path

harbor's proxy MUST NOT forward request headers that could carry harbor
auth material to `ui.duckdb.org` (or any configured upstream). The
proxy uses a **strict allow-list** of safe asset-fetch headers
(`Accept`, `Accept-Encoding`, `Accept-Language`, `If-None-Match`,
`If-Modified-Since`, `Range`); every other request header is dropped
before the outbound request. Specifically the following are **never**
forwarded:

- `Cookie` — would leak `harbor_session=v1.<principal_hex>...`
- `Authorization` — would leak Bearer token
- `X-Harbor-Token` — same
- `X-Harbor-Session-Id` — future SQL session id
- `Origin` — would expose the user's local harbor URL upstream
- `Sec-*` fetch-metadata headers

Tested in `scripts/golden-cookie-auth.sh` (PR-8) by spawning a tiny
mock listener, pointing `ui_remote_url` at it, and asserting the
captured upstream request never contains any of the above.

If a future need to forward an upstream-set domain cookie back to
upstream emerges, the right shape is a **positive filter** that keeps
only cookies upstream itself set (tracked by inspecting `Set-Cookie`
in prior responses) — NOT a `Cookie` passthrough that includes
whatever the browser happens to send.

### Future modes (post-v0.1)

A `bundled` mode (compile-in const byte array of the UI) was originally
planned for v0.1. Per the post-PR-4 architectural review (see §14
"Roadmap"), it has been deferred to v0.2. Most "air-gapped"
deployments can self-host an internal HTTP mirror of `ui.duckdb.org`
and point `harbor_ui_proxy_url` at it (10-line nginx config), which
covers the primary `bundled`-mode use case with significantly less
complexity in harbor itself.

A small in-process **bounded asset cache** (~150 LOC) on top of
`proxy` mode that pass-throughs `ETag` / `Cache-Control` /
`Last-Modified` and serves `304 Not Modified` revalidation against
upstream was originally bundled with the post-PR-7 architectural
cleanup PR. With that PR declined (see §14), the bounded asset cache
is reclassified as an independently-shippable v0.2 enhancement —
worth doing on its own merits when latency to `ui.duckdb.org` becomes
a measured user pain point, but not load-bearing for v0.1.

> **Implementation order:** PR-3 shipped `proxy` mode against
> `ui.duckdb.org`. PR-8 added the credential-strip allow-list
> invariant on the proxy. v0.1 ships exactly those two pieces. True
> air-gap-with-no-mirror deployments — the only remaining
> `bundled`-only use case — are deferred to v0.2 if real demand
> appears.

The harbor login wrapper described in §7 lives at `GET /` (registered
before the catch-all and before `/ui/`). The unmodified upstream UI
bundle is served from `GET /ui/.*`. Once the cookie is set, the
wrapper redirects browsers to `/ui/` and the UI takes over.

**Route precedence is strict** and enforced by registration order:

1. Specific API routes (`/quack`, `/sql`, `/sql/*`, `/auth/*`, `/ddb/*`, `/info`, `/localEvents`, `/localToken`, `/health`, `/ready`, `/whoami`, `/tables`, `/schema/*`, `/checkpoint`, `/sessions`, `/interrupt`)
2. `GET /` — harbor login wrapper
3. `GET /ui/.*` — UI bundle
4. `GET /.*` — root-namespace UI assets (favicon, manifest, etc.)

Any unmatched path NOT under `/ui/` and NOT a known root-namespace UI
asset returns `404`. The catch-all does NOT proxy unknown API-shaped
paths upstream.

When the UI assets bundle and the upstream UI's protocol expectations
drift apart, `harbor_serve` logs a warning at startup. In practice we
re-bundle on each upstream UI release.

## 9. Configuration surface

All harbor settings are regular DuckDB session/global options. Reset rule
inherited from quack: settings consulted from worker connections
(authentication, authorization) **must** be set with `SET GLOBAL` and
restored with `RESET GLOBAL`.

| Setting | Type | Default | Purpose |
|---|---|---|---|
| `harbor_bind` | VARCHAR | `127.0.0.1` | Bind address. `0.0.0.0` to expose; triggers warnings + tightens defaults. |
| `harbor_port` | INTEGER | `9494` | Listen port. |
| `harbor_token` | VARCHAR | (auto) | Auth token; if unset, `harbor_serve` generates and returns one. |
| `harbor_authentication_function` | VARCHAR | `harbor_check_token` | SQL function name for auth callback. |
| `harbor_authorization_function` | VARCHAR | `harbor_nop_authorization` | SQL function name for authz callback. |
| `harbor_auth_cookie_ttl_s` | UBIGINT | `43200` (12 h) | Cookie expiry. |
| `harbor_max_sessions` | UBIGINT | `1024` | Max concurrent DB sessions. |
| `harbor_session_ttl_s` | UBIGINT | `3600` | Idle DB-session TTL in seconds. |
| `harbor_query_timeout_s` | UBIGINT | `0` | Per-query timeout. `0` disables. |
| `harbor_max_request_body_bytes` | UBIGINT | `268435456` (256 MiB) | Per-request body cap. |
| `harbor_max_response_rows` | UBIGINT | `0` (unlimited) | `/sql` row truncation cap. |
| `harbor_stop_drain_timeout_s` | UBIGINT | `30` | Max seconds `harbor_stop` waits for in-flight requests before interrupting them. |
| `harbor_allow_admin_without_authz` | BOOLEAN | `false` | When the authz hook is the default permissive `harbor_nop_authorization`, admin endpoints still default-deny unless this is set. Loud warning at startup if `true`. |
| `harbor_fetch_batch_chunks` | UBIGINT | `12` | Inherited from quack — chunks per FETCH. |
| `harbor_fetch_batch_bytes` | UBIGINT | `4194304` (4 MiB) | Inherited from quack. |
| `harbor_ui_assets` | VARCHAR | `proxy` | `proxy` / `bundled` / `disabled`. See §8. |
| `harbor_ui_proxy_url` | VARCHAR | `https://ui.duckdb.org` | Upstream URL when `harbor_ui_assets = 'proxy'`. |
| `harbor_local_dev_mode` | BOOLEAN | `false` | Skip token requirement for local-bound, same-Origin requests. |
| `harbor_cors_origins` | VARCHAR | `''` (empty) | Comma-separated allow-list of origins for cross-origin `/sql`, `/quack`, `/auth/*`. |
| `harbor_log_requests` | BOOLEAN | `true` | Per-request structured log entry. |
| `harbor_log_queries` | BOOLEAN | `false` | Log full SQL of every executed query. Off by default (sensitive). |
| `whoami_*` | VARCHAR | (empty) | Inherited from quack — node identity for `/whoami`. |

Functions:

| Function | Returns | Notes |
|---|---|---|
| `harbor_serve(uri, token := NULL, allow_other_hostname := false)` | row of `(uri, url, token)` | Starts the server. Validates token (≥ 4 chars), generates if NULL. **Single-server-per-process**: throws if a server is already running. |
| `harbor_stop(uri)` | `BOOLEAN` | Stops the server bound to URI. Releases any thread blocked in `harbor_wait()`. |
| `harbor_wait()` | `BOOLEAN` | Blocks the caller until the server stops or the process receives `SIGTERM`/`SIGINT`. Used in container init scripts to keep DuckDB alive. |
| `harbor_status()` | row of `(running, uri, sessions, uptime_s)` | Introspect server state. |
| `harbor_identify(name, provider, hostname, region, meta)` | row | Sets `whoami_*` settings. Inherited. |
| `whoami()` | table | Inherited. |
| `harbor_uri_parser(uri, ssl)` | STRUCT | Parses `harbor:` or `quack:` URI. |
| `harbor_check_token(sid, client_token, server_token)` | BOOLEAN | Default authentication. |
| `harbor_nop_authorization(sid, query)` | BOOLEAN | Default authorization. |

Quack-named aliases (`quack_serve`, `quack_check_token`,
`quack_authentication_function`, etc.) are retained as functional
aliases to ease migration from upstream Quack and to keep upstream
Quack-aware tooling working.

## 10. Observability

### Logging

harbor registers two log types in DuckDB's logging system:

- `'Quack'` (LEVEL: DEBUG) — every protocol message, all routes.
  Fields: `route`, `session_id`, `principal`, `client_query_id`,
  `query` (only if `harbor_log_queries`), `duration_ms`,
  `response_status`, `error`. Name inherited verbatim from upstream
  `duckdb-quack`. (A planned rename to `'Harbor'` with `'Quack'`
  preserved as a back-compat alias was tracked in issue
  [#30](https://github.com/shreeve/duckdb-harbor/issues/30) and
  deferred — the existing name is functionally complete.)
- `'HTTP'` (LEVEL: INFO) — underlying transport, inherited from upstream.

Logs go to DuckDB's in-memory buffer by default; persist with:

```sql
CALL enable_logging('Quack', storage => 'file',
                    storage_config => {'path': '/var/log/harbor'});
```

### Health vs readiness

- `GET /health` — process is alive. Returns `200` always once the server
  thread is running. No DB introspection.
- `GET /ready` — DB is reachable and accepting queries. Issues a
  `SELECT 1` against a worker connection. Returns `503` on failure.

### Metrics

A `/metrics` endpoint emitting Prometheus-format metrics is on the
roadmap but explicitly **out of scope for v0.1**. Until then, query
DuckDB's own `duckdb_logs_parsed('Harbor')` for structured stats.

## 11. Compatibility & versioning

| Versioned thing | harbor's commitment |
|---|---|
| **DuckDB engine** | Each harbor release pins to a specific DuckDB version. Cross-version load is refused with a clear error. |
| **Quack wire protocol** | Tracks upstream `duckdb/duckdb-quack`. Each rebase point is recorded in `BUILD.md` with the upstream commit. We do not promise wire compatibility across rebase points until DuckDB v2.0 GA. |
| **DuckDB UI protocol** | Tracks upstream `duckdb/duckdb-ui`. Same caveat as Quack. |
| **harbor `/sql` JSON format** | Semantic versioning. Added fields and added type encodings are minor; removed fields, changed shapes, or changed type semantics are major. |
| **harbor settings names** | Renames carry deprecation aliases for one minor version. |
| **harbor URI scheme** | `harbor:` and `quack:` both supported indefinitely. |

`GET /info` returns:

```
X-Harbor-Version:          0.1.0
X-DuckDB-Version:         v1.5.3
X-DuckDB-Platform:        osx_arm64
X-Quack-Protocol-Version: 1
X-Ui-Extension-Version:   <upstream UI version pinned>
```

## 12. Build & deployment

### Repository layout

```
duckdb-harbor/
├── LICENSE
├── THIRD_PARTY_NOTICES.md            ← UI assets and any other vendored notices
├── README.md
├── SPEC.md                           ← this file
├── AGENTS.md                         ← AI agent guide
├── CMakeLists.txt
├── extension_config.cmake
├── vcpkg.json
├── Makefile
├── duckdb/                           ← submodule, pinned to v1.5.3
├── extension-ci-tools/               ← submodule
├── src/
│   ├── harbor_extension.cpp           ← entry point, settings, function registration
│   ├── harbor_http_server.{cpp,hpp}   ← HarborHttpServer (owns httplib::Server)
│   ├── harbor_session.{cpp,hpp}       ← SessionManager
│   ├── harbor_auth.{cpp,hpp}          ← AuthManager + cookie HMAC + login/logout handlers
│   ├── harbor_log.{cpp,hpp}           ← log type registration
│   ├── harbor_wait.{cpp,hpp}          ← blocking harbor_wait() table function
│   ├── quack/                        ← derived from duckdb-quack
│   │   ├── quack_handlers.{cpp,hpp}
│   │   ├── quack_message.{cpp,hpp}   ← unchanged from upstream
│   │   ├── quack_scan.{cpp,hpp}
│   │   ├── storage/
│   │   └── ...
│   ├── sql/
│   │   ├── sql_handlers.{cpp,hpp}    ← /sql, /sql/sessions/new, /sql/cancel
│   │   ├── sql_param_decoder.{cpp,hpp}
│   │   └── sql_chunk_encoder.{cpp,hpp}
│   ├── ui/                           ← derived from duckdb-ui
│   │   ├── ui_handlers.{cpp,hpp}
│   │   ├── ui_assets.{cpp,hpp}
│   │   ├── ui_assets_data.cpp        ← generated; const byte array
│   │   └── ui_login_page.{cpp,hpp}   ← harbor-specific login wrapper at GET /
│   └── admin/
│       └── admin_handlers.{cpp,hpp}
├── scripts/
│   ├── fetch-ui-assets.sh
│   ├── golden-quack-roundtrip.sh
│   └── golden-ui-roundtrip.sh
├── test/
│   ├── unit/
│   ├── integration/
│   ├── golden/
│   └── types/
├── docs/
│   ├── ERRORS.md
│   ├── REVERSE_PROXY.md
│   ├── ROADMAP.md
│   └── DEPLOY_INCUS_ZFS.md
└── misc/                             ← NOT versioned; clones of upstream for reference
    ├── duckdb-quack/                 ← v1.5-variegata
    └── duckdb-ui/
```

### Build

Standard DuckDB extension build via `extension-ci-tools`:

```bash
make release             # builds ./build/release/extension/harbor/harbor.duckdb_extension
make debug
make test                # runs unit + integration suites
```

CI matrix produces `harbor.duckdb_extension` for
`osx_arm64`, `osx_amd64`, `linux_amd64`, `linux_arm64`, `windows_amd64`.
Distribution is via DuckDB's community-extension repository once that's
set up; until then, GitHub Releases.

### Deployment — Incus + ZFS

Reference layout, documented in [`docs/DEPLOY_INCUS_ZFS.md`](./docs/DEPLOY_INCUS_ZFS.md):

```
host
├── /tank/duckdb/<tenant>/                  ← ZFS dataset, COW snapshots
│   ├── db.duckdb
│   ├── harbor-init.sql                      ← LOAD harbor; harbor_serve(...); harbor_wait();
│   └── harbor-token                         ← created at first boot
└── incus container "harbor-<tenant>"
    ├── /usr/bin/duckdb                     ← DuckDB binary
    ├── /root/.duckdb/extensions/.../harbor.duckdb_extension
    └── /data → bind-mount of /tank/duckdb/<tenant>/
```

The container's `ENTRYPOINT` is one command:

```bash
duckdb -no-stdin -init /data/harbor-init.sql /data/db.duckdb
```

Snapshot policy: pre-snapshot `POST /checkpoint`, then
`zfs snapshot tank/duckdb/<tenant>@hourly-...`. Cron one-liner:

```cron
0 * * * * curl -sf -H "Authorization: Bearer $TOKEN" \
            -X POST http://127.0.0.1:9494/checkpoint \
          && zfs snapshot tank/duckdb/<tenant>@hourly-$(date +\%Y\%m\%d-\%H)
```

## 13. Testing

| Test class | What it verifies | Location |
|---|---|---|
| Unit | encoders, session manager, auth resolution in isolation | `test/unit/` |
| Integration | spin a real harbor server, exercise each route, assert envelope shapes | `test/integration/` |
| Golden Quack | byte-equal regression against captured upstream Quack client traces | `test/golden/quack/` |
| Golden UI | hit `/ddb/run` exactly as the official UI does (recorded), assert byte-equal `application/octet-stream` response | `test/golden/ui/` |
| `/sql` round-trip | for **every** DuckDB type in §5.4, INSERT a value, SELECT it back via `/sql`, decode, compare to a control INSERT through `/quack` | `test/types/` |
| Concurrency | N concurrent sessions, no cross-talk; same-session concurrent → 409 | `test/integration/` |
| Cross-principal | session_id from principal A is not usable by principal B | `test/integration/` |
| Interrupt | start a long query, hit `/interrupt`, assert it actually stops | `test/integration/` |
| Slow client | open `/sql` stream, stop reading, assert server unwinds within 5s | `test/integration/` |
| Auth bypass | every authenticated route returns `401` without token, `403` when authz says false | `test/integration/` |
| Catch-all order | `GET /.*` does not shadow `/info`, `/health`, `/ready`, etc. | `test/integration/` |
| Asset bundle | `harbor_ui_assets = 'bundled'` serves the same bytes a real UI build expects | `test/integration/` |
| Identifier safety | `GET /schema/:db/:table` with malicious path parameters cannot SQL-inject | `test/integration/` |
| Daemon mode | `duckdb -no-stdin -init harbor-init.sql db.duckdb` with `harbor_wait()` keeps the process alive until `SIGTERM` | `test/integration/` |

CI runs all suites against each platform binary on every PR.

## 14. Roadmap (post-v0.1)

Explicitly **out of scope for v0.1**, listed for record:

- **`bundled` UI assets mode** — compile-in const byte array of the
  UI (~10–15 MB) for air-gapped deployments with no internal mirror.
  Originally planned for v0.1; deferred per the post-PR-4
  architectural review on the grounds that (1) most "air-gapped"
  deployments can self-host an internal HTTP mirror of `ui.duckdb.org`
  and point `harbor_ui_proxy_url` at it, eliminating the in-harbor
  bundle pipeline entirely, and (2) the bundle pipeline
  (`scripts/fetch-ui-assets.sh`, `ui_assets_data.cpp` codegen,
  `UI_ASSETS_VERSION.txt` pin, asset goldens, ~5–10 MB binary tax,
  per-release refresh CI step) is significant complexity for the
  shrinking residual use case. v0.2 will re-examine if a true
  air-gap-with-no-mirror operator emerges.
- ~~**OpenSSL/cpp-httplib architectural cleanup**~~ — **evaluated and
  declined** for v0.1 after the round-13/round-14 architectural
  review. The original framing (two HTTP stacks, double OpenSSL link,
  drop OpenSSL from vcpkg) collapsed once empirical investigation
  showed `vcpkg.json`'s `[openssl, curl]` entries are required by the
  bundled `httpfs` sibling extension regardless. The remaining
  concrete win — harbor_extension stops *directly* linking
  libssl/libcrypto — is a 200 KB – 1 MB per-platform binary
  reduction, against ~500 LOC of risky migration work touching
  crypto + proxy + server namespace + CMake, plus mbedTLS extension-
  ABI uncertainty (un-`DUCKDB_API`-annotated symbols), plus risk to
  the PR-3 UI golden roundtrip and the PR-8 credential-strip
  invariant. Decision: keep the working architecture; spend the
  attention on PR-5 (`/sql`) instead. See AGENTS.md "PR-10b: declined"
  for the full cost-benefit table and the trigger conditions that
  would justify revisiting (downstream CVE, upstream `duckdb-ui`
  itself migrating, real binary-size constraint, or a "drop httpfs
  entirely" use case).
- **`harbord` wrapper binary** — a tiny C++ binary that hides the
  `duckdb -no-stdin -init …` invocation behind `harbord /data/db.duckdb`.
  Not shipped in v0.1: the unwrapped command is short, an init script is
  more flexible than a fixed wrapper, and the decision is reversible if
  operator demand justifies the extra binary.
- **Arrow stream output on `/sql`** — `Accept: application/vnd.apache.arrow.stream`
  for full-fidelity column-store interchange.
- **`/metrics`** Prometheus endpoint.
- **WebSocket transport** for `/sql` (server-pushed cancellation, lower
  per-message overhead).
- **Multi-tenant proxy in front of N harbors** — out of scope for the
  extension itself, lives in a separate ops-side project.
- **Replication / read-replicas** — almost certainly via upstream Quack
  ATTACH chains rather than anything harbor-specific.
- **Built-in TLS** — only if cpp-httplib's OpenSSL story stops being a
  burden.
- **Clean fork of `duckdb-ui` upstream** — pursue contributing the
  multi-server-on-one-port refactor back upstream so the UI extension
  itself can co-exist with quack on one port without harbor as the
  glue. If upstream accepts, harbor loses one of its raisons d'être —
  which would be a good outcome.

## 15. Open questions

Items that need resolution before v0.1 ships. Tracked in GitHub issues:

1. **Quack v1.5-variegata vs DuckDB v2.0 timing.** Start now and absorb
   protocol churn, or wait for v2.0 GA (~fall 2026) and start on a
   stable base? Recommend starting now and pinning to a v1.5.x quack
   commit; rebase to v2.0 as a single deliberate cutover.
2. **Cookie signing key persistence.** v0.1 ships ephemeral keys only
   (32 random bytes per process; restart invalidates every cookie).
   The SQL-readable `harbor_cookie_signing_key` setting was dropped
   in PR-4 review because exposing the HMAC secret to authorized SQL
   would let any SQL caller mint cookies. v0.2+ introduces
   `HARBOR_COOKIE_SIGNING_KEY` as a process-environment variable for
   operators who need cookie continuity across rolling restarts —
   placed in the env (not the SQL surface) so it sits at the same
   trust boundary as the binary itself.
3. **Bundled UI asset version policy.** Pin per harbor release vs.
   floating to upstream UI's `main`? Recommend: pin per harbor release;
   one of the things `make release` does is verify the bundle hash.
4. **Harbor login wrapper UX.** A single token-paste page is the
   minimum. Any nicer UX (token rotation, multiple users) should be
   delivered as separate UI work, not blocking v0.1.
