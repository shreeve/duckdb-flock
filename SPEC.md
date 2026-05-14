# flock — Specification

> **Status:** Draft v0.2 (2026-05). This document captures the design
> decisions for the `flock` DuckDB extension before code is written.
> Implementation details (per-function pseudocode, byte layouts) are NOT
> in scope for this document — the source is the spec for those.

`flock` is a single DuckDB extension that runs **one HTTP server on one
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

- **Single binary, single port, single DuckDB instance.** A loaded `flock`
  extension turns one DuckDB process into a multi-protocol HTTP service.
- **Upstream Quack compatibility.** Stock DuckDB clients (≥ v1.5.2 with the
  upstream `quack` extension installed) `ATTACH 'quack:host'` and Just Work
  against a flock server. We track upstream `duckdb-quack` and rebase as it
  evolves toward DuckDB v2.0 GA.
- **Application-friendly JSON SQL endpoint.** `POST /sql` is the path
  app code hits. JSON in, NDJSON out, schema-typed for round-trip safety
  on core types.
- **Official DuckDB UI included.** Browse to the port, get the real UI.
- **Pluggable auth.** Token + per-connection authentication and per-query
  authorization, as user-supplied SQL macros or scalar UDFs. One model
  applies to every SQL-bearing endpoint.
- **Container-ready deployment.** Designed to run as
  `duckdb -no-stdin -init flock-init.sql /data/db.duckdb` inside a
  minimal Incus application container against a ZFS dataset for COW
  snapshots.

### Non-goals

- **Not a query planner / not a database.** flock is a transport. All SQL
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

### Process layout

```
                    ┌────────────────────────────────────────┐
                    │          DuckDB process (single)       │
                    │                                        │
                    │  ┌──────────────────────────────────┐  │
                    │  │   FlockHttpServer (one instance) │  │
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
FlockHttpServer
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
| **Load** | `LOAD flock;` | Extension registers settings, scalar functions, and the `flock_serve` / `flock_stop` / `flock_wait` table macros. **No socket is bound yet.** |
| **Start** | `CALL flock_serve('flock:0.0.0.0:9494', token => '…')` | Construct `FlockHttpServer`, bind socket synchronously (so `EADDRINUSE` propagates to the caller), spawn listener thread. Returns `(uri, url, token)` row, identical shape to quack. **Returns immediately.** |
| **Block** | `CALL flock_wait()` | Optional. Blocks the calling SQL session until the server stops or the process receives `SIGINT`/`SIGTERM`. Required for non-interactive container deployments — see "Daemon mode" below. |
| **Quiesce** | `CALL flock_stop('flock:0.0.0.0:9494')` | `Server::stop()` (closes listening socket — no new requests accepted), wait up to `flock_stop_drain_timeout_s` (default 30s) for in-flight requests to complete naturally, then `Connection::Interrupt()` on every session that's still running a query, join listener thread, close all sessions, drop the `DatabaseInstance` `weak_ptr`. Releases any thread blocked in `flock_wait()`. |
| **Restart** | restart of host process | All state is in-memory; restart is the reload primitive. Sub-second when the DB file is warm. |
| **Repeated load** | `LOAD flock;` while already loaded | No-op. |
| **Repeated `flock_serve`** | second call before `flock_stop` | Throws. flock is **single-server-per-process**: only one `FlockHttpServer` may be live at a time, regardless of URI/port. Enforced in `flock_serve`. |
| **DuckDB shutdown** | DuckDB extension shutdown hook | If a server is running, `flock_stop` is invoked. Specifically: only the listener thread is joined and the socket closed. We do **NOT** touch DuckDB internals from the shutdown hook (some are partially destroyed by then). `atexit` is used only as a last-resort fallback for child processes. |

Auto-start at extension load is **NOT** the default. The user must
`CALL flock_serve(...)` explicitly. Reasons: testability, deployment
config visibility, no surprises in non-server contexts.

#### Daemon mode (containers, systemd)

The DuckDB CLI is fundamentally an interactive REPL. Running
`duckdb -c '…'` exits immediately after the last statement runs, which
would tear down the flock server. For non-interactive deployments,
**`flock_wait()` in an init script is the deployment pattern**:

```sql
-- /etc/flock-init.sql
LOAD flock;
CALL flock_serve('flock:0.0.0.0:9494');
CALL flock_wait();
```

```bash
duckdb -no-stdin -init /etc/flock-init.sql /data/db.duckdb
```

`flock_wait()` blocks until the server stops or `SIGTERM`/`SIGINT`
arrives. The DuckDB process stays alive. Without a trailing
`flock_wait()` (or some other blocking statement), the CLI exits when
the init script finishes and takes the server with it.

Interactive use (`duckdb` at a terminal) is unaffected: `flock_serve`
returns immediately, the REPL is still yours, and the server runs in
the background.

> A `flockd` wrapper binary that hides the `-no-stdin -init …`
> incantation is intentionally not shipped — the unwrapped command is
> short, an init script is more flexible than a fixed wrapper (operators
> can install custom auth hooks, set globals, etc.), and the decision is
> reversible if operator demand justifies it.

## 3. URIs and protocol identity

### URI scheme

flock uses **`flock:`** as its primary URI scheme (used by `flock_serve`)
and accepts **`quack:`** as an interop alias for client-side `ATTACH`
from stock upstream Quack.

| URI | Used by |
|---|---|
| `flock:host[:port]` | flock-aware tools (the eventual flock client extension). Also accepted as an alias for `quack:` on the server side. |
| `flock://host[:port]`, `flock:[ipv6]:port` | RFC-style and IPv6 forms |
| `quack:host[:port]` | **Stock DuckDB clients with the upstream `quack` extension** loaded. flock servers accept these unchanged. |

The two schemes resolve to the same handler. `flock_uri_parser()`
parses both.

> **Important interop note:** stock DuckDB clients running upstream
> Quack do **not** know the `flock:` scheme — only `quack:`. So when
> connecting *from* a vanilla `duckdb` CLI:
>
> ```sql
> ATTACH 'quack:127.0.0.1:9494' AS r (TYPE quack);
> ```
>
> The `flock:` form becomes useful only after the flock client
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
| 2 | POST | `/quack` | QuackHandlers | per-message token via `flock_authentication_function` | `application/vnd.duckdb` request/response |
| 3 | POST | `/auth/login` | AuthHandlers | bearer token (`Authorization: Bearer <token>`) | issues HMAC-signed `flock_session` cookie + returns `{principal, expires_at}` |
| 4 | POST | `/auth/logout` | AuthHandlers | cookie | clears `flock_session` cookie; optionally drops associated DB sessions |
| 5 | POST | `/sql` | SqlHandlers | bearer / cookie + authz hook | NDJSON streamed response (default) or one-shot JSON when `Accept: application/json`. `BEGIN`/`COMMIT` etc. require an explicit `sessionId`. |
| 6 | POST | `/sql/sessions/new` | SqlHandlers | bearer / cookie | explicitly creates a persistent DB session; returns `{sessionId}`. Use this when you need to control DB-session lifetime separately from a single request. |
| 7 | DELETE | `/sql/sessions/:id` | SqlHandlers | bearer / cookie + ownership | destroys the named DB session |
| 8 | POST | `/sql/cancel` | SqlHandlers | bearer / cookie + admin authz | body: `{sessionId}` |
| 9 | POST | `/ddb/run` | UiHandlers | cookie + Origin check + authz hook | UI binary protocol |
| 10 | POST | `/ddb/tokenize` | UiHandlers | cookie + Origin check | UI binary protocol |
| 11 | POST | `/ddb/interrupt` | UiHandlers | cookie + Origin check | UI binary protocol |
| 12 | GET | `/info` | UiHandlers | none | version headers, used by UI to detect server |
| 13 | GET | `/localEvents` | UiHandlers | cookie | SSE stream |
| 14 | GET | `/localToken` | UiHandlers | Referer == local URL **AND** `flock_bind == 127.0.0.1` | returns MotherDuck token if present; **404 when bound non-locally** |
| 15 | GET | `/health` | AdminHandlers | none | minimal: `{ok:true, version, uptime_s}` only |
| 16 | GET | `/ready` | AdminHandlers | none | runs `SELECT 1` against a worker connection; 503 on failure |
| 17 | GET | `/whoami` | AdminHandlers | bearer / cookie | identity + runtime info, JSON form of `whoami()` macro |
| 18 | GET | `/tables` | AdminHandlers | bearer / cookie + authz (`__FLOCK_ADMIN__:catalog:list_tables`) | list tables in `main` |
| 19 | GET | `/schema/:db/:table` | AdminHandlers | bearer / cookie + authz (`__FLOCK_ADMIN__:catalog:describe_table`; path params NOT interpolated into authz string) | column info via `pragma_table_info()` with quoted identifiers; 404 on miss |
| 20 | POST | `/checkpoint` | AdminHandlers | bearer / cookie + authz (`__FLOCK_ADMIN__:checkpoint:create`) | runs `CHECKPOINT;` and returns the new WAL state |
| 21 | GET | `/sessions` | AdminHandlers | bearer / cookie + authz (`__FLOCK_ADMIN__:sessions:list`) | live sessions: id, principal, age, last query, in-flight |
| 22 | POST | `/interrupt` | AdminHandlers | bearer / cookie + authz (`__FLOCK_ADMIN__:sessions:interrupt`) | body: `{sessionId}` — interrupts current query if any |
| 23 | GET | `/` | UiHandlers (LoginWrapper) | none (renders the wrapper; cookie issuance happens via separate `POST /auth/login`) | flock-specific login page; if cookie present, redirects to `/ui/` |
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

Compatibility: flock targets **upstream Quack at branch
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
- `Cookie: flock_session=<…>` — accepted as an alternative to bearer
- `Accept: application/x-ndjson` (default) or `Accept: application/json` (force one-shot non-streamed)
- `X-Flock-Session-Id: <id>` — alternative to `sessionId` in body

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
| `X-DuckDB-UI-Connection-Name` | identifies the UI's per-tab connection. Mapped (per authenticated principal) to a flock DB session by `UiHandlers` — see §6. |
| `X-DuckDB-UI-Database-Name` (base64) | sets `USE` database for this query |
| `X-DuckDB-UI-Schema-Name` (base64) | sets schema |
| `X-DuckDB-UI-Parameter-Count`, `X-DuckDB-UI-Parameter-Value-N` (base64) | positional params |
| `X-DuckDB-UI-Result-Row-Limit` | row cap |
| `X-DuckDB-UI-Result-Database-Name`, `-Schema-Name`, `-Table-Name` | "save result as table" feature |
| `X-DuckDB-UI-Errors-As-JSON` | toggle DuckDB's JSON-formatted error mode |

The UI bundle is taken **unchanged** from upstream `duckdb-ui` for the
`/ddb/*` request shape. flock adds an auth-cookie requirement on top:
the UI obtains the cookie via `POST /auth/login`, then `/ddb/*`
requests carry both the cookie and an `Origin` matching the local URL.
The bundled UI ships with a small flock-specific login wrapper at
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

**Round-trip promise:** flock NDJSON is lossless for **every DuckDB
core logical type listed above** when decoded using the schema record.
Extension logical types not listed here fall back to a string
representation derived from `CAST(... AS VARCHAR)` and are explicitly
marked `"lossless": false` in the schema. For full-fidelity column-store
interchange across DuckDB versions, use `/quack` (BinarySerializer); a
future `Accept: application/vnd.apache.arrow.stream` mode on `/sql` is
on the roadmap.

The complete `LogicalTypeId` matrix for DuckDB v1.5.2 is enumerated in
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
  `{name, provider, hostname, region, uptime_s, ts_now, meta:{duckdb_version, platform, flock_version, ...}}`.
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

flock distinguishes three concepts that are easy to conflate. Each has
its own identifier and lifetime:

| Concept | Identifier | Lifetime | Purpose |
|---|---|---|---|
| **Auth principal** | `principal_id = hex(sha256(client_token))` — see derivation rule below | per token | Identity. The "who". |
| **Auth cookie** | HMAC-signed opaque blob containing `(principal_id, expires_at, nonce)` | configurable, default 12h | Browser convenience. Lets the UI authenticate without re-presenting the token on every request. Server-side the cookie is stateless (HMAC-verified, not looked up). |
| **DB session** | UUID, generated by `SessionManager` | until explicit destroy or idle TTL | DuckDB `Connection` + state (transactions, temp tables, SET variables, prepared statements). Owned by exactly one `principal_id` at creation time. |

#### Principal identity derivation

`flock_authentication_function` returns BOOLEAN — it can't *report* an
identity. flock derives a deterministic principal identity from the
*credential the caller presented*:

```
principal_id = hex(sha256(client_token))
```

Properties of this scheme:

- **Deterministic.** Same token → same principal across processes,
  independent of cookie state.
- **No raw-token storage.** flock never stores or logs the raw token
  or its full hash; logs use the first 8 hex chars as a non-reversible
  abbreviation.
- **Cross-principal isolation.** A request that presents principal A's
  token but principal B's `sessionId` is rejected — `SessionManager`
  compares the lookup's derived `principal_id` against the session's
  recorded `owner_principal_id`.
- **Backward-compatible with upstream Quack hooks.** The
  `flock_authentication_function`/`quack_authentication_function`
  callback signature is unchanged.
- **Cookie principal == bearer principal** for the same token, because
  both derive from the same `sha256(client_token)`. Logging in via
  `/auth/login` gives you a cookie that addresses the same DB sessions
  a bearer-token caller with the same token would.

**Future extension** (post-v0.1): an optional
`flock_principal_function(client_token) -> VARCHAR` hook can override
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
| Idle longer than `flock_session_ttl_s` (default 3600) | Session destroyed at next 60s sweep |
| Explicit `DISCONNECT_MESSAGE` (Quack), or `DELETE /sql/sessions/:id` | Destroyed immediately |
| `flock_stop` or process restart | All sessions vanish (in-memory only) |
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
| `flock_max_sessions` | 1024 | new session creation returns `429 SESSION_LIMIT` |
| `flock_session_ttl_s` | 3600 | swept on next 60s tick |
| `flock_max_response_rows` | 0 (unlimited) | `/sql` truncates; trailer reports `truncated: true` |
| `flock_max_request_body_bytes` | 256 MiB | matches the nginx/Caddy reverse-proxy guidance for `/quack` (APPEND payloads can be large) |
| `flock_query_timeout_s` | 0 (no limit) | per-query timeout via `Connection::Interrupt()` |
| `flock_auth_cookie_ttl_s` | 43200 (12h) | cookies past this `expires_at` are rejected |

### Cancellation

Two paths to cancel:

1. **Client disconnect** during `/sql` streaming. `FlockHttpServer`
   detects the broken pipe and calls `Connection::Interrupt()` on the
   bound session, then unwinds the chunk loop.
2. **Explicit `POST /interrupt`** or `POST /sql/cancel`. Authz hook
   gated. Calls `Connection::Interrupt()` on the named session.

For Quack, `DISCONNECT_MESSAGE` clears the connection's pending result
and `Connection::Interrupt()` is invoked if a query is in flight (this
is a flock addition; upstream just clears the result).

## 7. Authentication and authorization

### Threat model

flock exposes the **full SQL surface** of the underlying DuckDB,
including read/write of every table and `ATTACH` to remote sources. The
auth token effectively grants superuser access to the database file.
Treat it as a database password.

### Defaults

| Concern | Default |
|---|---|
| Bind address | `127.0.0.1:9494` |
| Token | random 16-byte hex generated by `flock_serve` and returned in the result row |
| Authentication callback | `flock_check_token` (compares to the served token; alias `quack_check_token` retained for compat) |
| Authorization callback | `flock_nop_authorization` (always allows; alias `quack_nop_authorization` retained) |
| Origin policy on `/ddb/*` | Origin must equal local URL **AND** valid auth cookie or bearer token |
| `/localToken` | enabled only when bound to localhost |
| CORS allowed origins | empty (no cross-origin requests permitted) |

### Auth credentials

Three accepted forms of authentication on a request:

1. **`Authorization: Bearer <token>`** — for `/quack` (via
   `CONNECTION_REQUEST.AuthString`), `/sql`, `/auth/login`, admin
   endpoints, programmatic tools.
2. **`Cookie: flock_session=<signed>`** — issued after a successful
   token exchange at `POST /auth/login`. The cookie is HMAC-signed
   server-side (no server-side cookie lookup), `HttpOnly;
   SameSite=Strict; Secure` (when behind HTTPS). The signing key is
   either configured via `flock_cookie_signing_key` or auto-generated
   per-process at server start (loses validity across restarts; that's
   intentional — a restart logs everyone out).
3. **`X-Flock-Token: <token>`** — accepted as an alternative to
   `Authorization: Bearer` for environments where the latter is awkward.

### Auth flow

```
┌─ POST /auth/login --------------------------------┐
│ Authorization: Bearer <token>                     │
│ → 200 OK                                          │
│   Set-Cookie: flock_session=<signed>; HttpOnly... │
│   {"principal":"<hash>","expires_at":"..."}       │
└──────────────────────────────────────────────────┘
                       │
                       ▼
┌─ POST /sql / GET /tables / POST /ddb/run / etc. ──┐
│ Cookie: flock_session=<signed>                    │
│   OR Authorization: Bearer <token>                │
│ → handler verifies HMAC OR re-runs                │
│   flock_authentication_function                   │
└──────────────────────────────────────────────────┘
```

### Authorization

The authorization callback is invoked **on every SQL-bearing request**,
including admin endpoints (using synthetic SQL):

| Request | `query` argument to `flock_authorization_function` |
|---|---|
| `/quack` `PREPARE_REQUEST` | the user's SQL |
| `/quack` `APPEND_REQUEST` | `INSERT INTO <schema>.<table> VALUES (NULL)` (synthetic; matches upstream Quack) |
| `/sql` | the user's SQL |
| `/ddb/run` | the user's SQL |
| `/checkpoint` | `__FLOCK_ADMIN__:checkpoint:create` |
| `/sessions` (GET) | `__FLOCK_ADMIN__:sessions:list` |
| `/sql/sessions/new` (POST) | `__FLOCK_ADMIN__:sessions:create` |
| `/sql/sessions/:id` (DELETE) | `__FLOCK_ADMIN__:sessions:delete` (the destroyed session id is **not** appended; identifying which session is in the request body / URL but is not part of the policy string, to avoid path-injection into authz strings) |
| `/interrupt` | `__FLOCK_ADMIN__:sessions:interrupt` |
| `/sql/cancel` | `__FLOCK_ADMIN__:sessions:cancel` |
| `/whoami` | `__FLOCK_ADMIN__:server:whoami` |
| `/tables` | `__FLOCK_ADMIN__:catalog:list_tables` |
| `/schema/:db/:table` | `__FLOCK_ADMIN__:catalog:describe_table` (path params are NOT interpolated into the authz string; same reason) |

The synthetic strings use a stable `__FLOCK_ADMIN__:<resource>:<action>`
shape so authz macros can pattern-match on the resource axis:

```sql
CREATE MACRO authz(sid, query) AS (
  CASE
    WHEN starts_with(query, '__FLOCK_ADMIN__:sessions:') THEN sid IN (SELECT * FROM ops_team)
    WHEN starts_with(query, '__FLOCK_ADMIN__:checkpoint:') THEN sid IN (SELECT * FROM ops_team)
    WHEN starts_with(query, '__FLOCK_ADMIN__:server:') THEN true
    WHEN starts_with(query, '__FLOCK_ADMIN__:catalog:') THEN true
    WHEN starts_with(query, '__FLOCK_ADMIN__:') THEN false  -- new admin verbs are deny-by-default
    ELSE starts_with(upper(trim(query)), 'SELECT')
  END
);
```

#### Admin authorization is default-deny when no hook is configured

When `flock_authorization_function` is the default `flock_nop_authorization`
(which always returns true), admin endpoints are still gated by an
internal default-deny rule on `__FLOCK_ADMIN__:` strings, **unless**
`flock_allow_admin_without_authz = true` is explicitly set. Reasons:

- The default authz is permissive for ergonomic dev experience on
  `/sql` and `/quack`. That convenience must NOT extend to admin
  operations like `CHECKPOINT` or session interruption.
- Operators who set a custom authz function but forget to handle the
  `__FLOCK_ADMIN__:` prefix get default-deny on admin (safe), not
  accidental allow.
- Setting `flock_allow_admin_without_authz = true` is the explicit
  opt-in for "I really do want unrestricted admin access on this
  trusted-network deployment." Logged loudly at server start.

The authz hook also returns deny on:
- exception thrown
- NULL returned
- non-BOOLEAN returned

Synthetic admin strings are **never** accepted from client request
bodies. The `query` field of `/sql` requests is rejected with
`BAD_REQUEST` if it begins with `__FLOCK_ADMIN__:`. Path parameters
to admin handlers are NOT concatenated into the authz string (see
table above) — concrete identifiers go in the request envelope, not
the policy decision input.

The hook signature, identical to upstream Quack:

```sql
flock_authorization_function(sid VARCHAR, query VARCHAR) -> BOOLEAN
```

Returning `false` (or throwing) rejects with `403 FORBIDDEN`. Operators
discriminate admin operations by pattern-matching on the `__FLOCK_ADMIN__:`
prefix:

```sql
CREATE MACRO authz(sid, query) AS (
  CASE
    WHEN starts_with(query, '__FLOCK_ADMIN__:')
      THEN sid IN (SELECT sid FROM admins_table)
    ELSE
      starts_with(upper(trim(query)), 'SELECT')
  END
);
SET GLOBAL flock_authorization_function = 'authz';
```

`flock_authentication_function` and `flock_authorization_function` are
SQL macros, scalar UDFs, or extension-registered functions. They run
in a fresh, transient server-side connection per call — they cannot
rely on session-local state.

### Browser-origin requests do NOT bypass auth

Upstream `duckdb-ui`'s `Origin == local_url` check is **CSRF defence,
not authentication**. We retain the Origin check as a CSRF gate but
**always also require** a valid auth cookie or bearer token. The
browser UI bootstraps this via flock's small login wrapper at `GET /`:

1. User opens `http://localhost:9494/`.
2. flock's wrapper detects no `flock_session` cookie and serves a
   minimal login page.
3. User pastes the token printed by `flock_serve`.
4. The page POSTs the token to `/auth/login`, gets
   `Set-Cookie: flock_session=<signed>`.
5. The page redirects to `/ui/` (or wherever the bundled DuckDB UI
   entry lives), which loads the upstream UI bundle unmodified.
6. All subsequent `/ddb/*` requests carry the cookie. The `Origin`
   header is still verified.

For local dev with `bind_to == 127.0.0.1` only, `flock_local_dev_mode =
true` (default off) skips steps 2-4 and accepts requests from the local
URL without a token. Off by default to prevent accidental exposure.

### When binding non-locally

When `flock_serve` is called with a non-loopback host:

- A loud startup warning is logged.
- `/localToken` is automatically disabled (returns `404`).
- `flock_local_dev_mode` is forced off regardless of setting.
- The default authorization function logs a warning unless overridden.

Recommended posture for production: front with nginx or Caddy doing
TLS termination (recipe in [`docs/REVERSE_PROXY.md`](./docs/REVERSE_PROXY.md))
and either keep `bind_to = 127.0.0.1` with the proxy on the same host,
or use firewall rules to restrict reach.

### CORS

flock blocks all cross-origin browser requests by default. To enable
specific origins (e.g. a React app on `https://app.example.com` calling
`/sql` directly):

```sql
SET GLOBAL flock_cors_origins = 'https://app.example.com,https://other.example.com';
```

When `flock_cors_origins` is non-empty:

- `OPTIONS` preflight is honored on `/sql`, `/quack`, `/auth/*` for
  listed origins.
- Allowed headers: `Authorization`, `Content-Type`, `X-Flock-Token`,
  `X-Flock-Session-Id`, `Accept`.
- `Access-Control-Allow-Origin` is set to the **specific** matching
  origin from the allow-list, **never** `*`. Wildcard origin combined
  with credentials is forbidden by spec and by us — the server refuses
  to start if `flock_cors_origins='*'`.
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

flock serves the UI HTML/JS/CSS at `GET /.*` from one of three modes,
selected by setting `flock_ui_assets`:

| Mode | Setting | Behavior |
|---|---|---|
| `proxy` (default) | `flock_ui_assets = 'proxy'` | Forwards `GET /.*` to `ui.duckdb.org` (matches upstream `duckdb-ui` behavior). Requires outbound network from the host. UI updates take effect immediately, no rebuild needed. |
| `bundled` | `flock_ui_assets = 'bundled'` | Serves the UI from a const byte array compiled into the extension. Offline-capable. Bundle is ~10–15 MB. Refreshed by re-running `scripts/fetch-ui-assets.sh` and rebuilding the extension. Use this for air-gapped or restricted-egress deployments. |
| `disabled` | `flock_ui_assets = 'disabled'` | All `GET /.*` requests return `404`. The UI doesn't load. `/sql` and `/quack` still work. Useful for headless deployments. |

The proxy URL is configurable via `flock_ui_proxy_url` (default
`https://ui.duckdb.org`) for testing, mirroring, or pointing at a fork.

In bundled mode, `GET /config` is intercepted to inject the same
`X-DuckDB-Version`, `X-DuckDB-Platform`, `X-DuckDB-UI-Extension-Version`
headers the upstream UI proxy adds. The bundled assets pin to a specific
upstream UI commit hash, recorded in `UI_ASSETS_VERSION.txt` at the repo
root.

> **Implementation order:** PR-3 ships `proxy` (the simpler implementation
> — just a thin HTTPS-client wrapper around cpp-httplib, against the
> already-linked OpenSSL). `bundled` mode lands when an air-gapped
> deployment use case actually appears.

The flock login wrapper described in §7 lives at `GET /` (registered
before the catch-all and before `/ui/`). The unmodified upstream UI
bundle is served from `GET /ui/.*`. Once the cookie is set, the
wrapper redirects browsers to `/ui/` and the UI takes over.

**Route precedence is strict** and enforced by registration order:

1. Specific API routes (`/quack`, `/sql`, `/sql/*`, `/auth/*`, `/ddb/*`, `/info`, `/localEvents`, `/localToken`, `/health`, `/ready`, `/whoami`, `/tables`, `/schema/*`, `/checkpoint`, `/sessions`, `/interrupt`)
2. `GET /` — flock login wrapper
3. `GET /ui/.*` — UI bundle
4. `GET /.*` — root-namespace UI assets (favicon, manifest, etc.)

Any unmatched path NOT under `/ui/` and NOT a known root-namespace UI
asset returns `404`. The catch-all does NOT proxy unknown API-shaped
paths upstream.

When the UI assets bundle and the upstream UI's protocol expectations
drift apart, `flock_serve` logs a warning at startup. In practice we
re-bundle on each upstream UI release.

## 9. Configuration surface

All flock settings are regular DuckDB session/global options. Reset rule
inherited from quack: settings consulted from worker connections
(authentication, authorization) **must** be set with `SET GLOBAL` and
restored with `RESET GLOBAL`.

| Setting | Type | Default | Purpose |
|---|---|---|---|
| `flock_bind` | VARCHAR | `127.0.0.1` | Bind address. `0.0.0.0` to expose; triggers warnings + tightens defaults. |
| `flock_port` | INTEGER | `9494` | Listen port. |
| `flock_token` | VARCHAR | (auto) | Auth token; if unset, `flock_serve` generates and returns one. |
| `flock_authentication_function` | VARCHAR | `flock_check_token` | SQL function name for auth callback. |
| `flock_authorization_function` | VARCHAR | `flock_nop_authorization` | SQL function name for authz callback. |
| `flock_cookie_signing_key` | VARCHAR | (auto, per-process) | HMAC key for `flock_session` cookies. |
| `flock_auth_cookie_ttl_s` | UBIGINT | `43200` (12 h) | Cookie expiry. |
| `flock_max_sessions` | UBIGINT | `1024` | Max concurrent DB sessions. |
| `flock_session_ttl_s` | UBIGINT | `3600` | Idle DB-session TTL in seconds. |
| `flock_query_timeout_s` | UBIGINT | `0` | Per-query timeout. `0` disables. |
| `flock_max_request_body_bytes` | UBIGINT | `268435456` (256 MiB) | Per-request body cap. |
| `flock_max_response_rows` | UBIGINT | `0` (unlimited) | `/sql` row truncation cap. |
| `flock_stop_drain_timeout_s` | UBIGINT | `30` | Max seconds `flock_stop` waits for in-flight requests before interrupting them. |
| `flock_allow_admin_without_authz` | BOOLEAN | `false` | When the authz hook is the default permissive `flock_nop_authorization`, admin endpoints still default-deny unless this is set. Loud warning at startup if `true`. |
| `flock_fetch_batch_chunks` | UBIGINT | `12` | Inherited from quack — chunks per FETCH. |
| `flock_fetch_batch_bytes` | UBIGINT | `4194304` (4 MiB) | Inherited from quack. |
| `flock_ui_assets` | VARCHAR | `proxy` | `proxy` / `bundled` / `disabled`. See §8. |
| `flock_ui_proxy_url` | VARCHAR | `https://ui.duckdb.org` | Upstream URL when `flock_ui_assets = 'proxy'`. |
| `flock_local_dev_mode` | BOOLEAN | `false` | Skip token requirement for local-bound, same-Origin requests. |
| `flock_cors_origins` | VARCHAR | `''` (empty) | Comma-separated allow-list of origins for cross-origin `/sql`, `/quack`, `/auth/*`. |
| `flock_log_requests` | BOOLEAN | `true` | Per-request structured log entry. |
| `flock_log_queries` | BOOLEAN | `false` | Log full SQL of every executed query. Off by default (sensitive). |
| `whoami_*` | VARCHAR | (empty) | Inherited from quack — node identity for `/whoami`. |

Functions:

| Function | Returns | Notes |
|---|---|---|
| `flock_serve(uri, token := NULL, allow_other_hostname := false)` | row of `(uri, url, token)` | Starts the server. Validates token (≥ 4 chars), generates if NULL. **Single-server-per-process**: throws if a server is already running. |
| `flock_stop(uri)` | `BOOLEAN` | Stops the server bound to URI. Releases any thread blocked in `flock_wait()`. |
| `flock_wait()` | `BOOLEAN` | Blocks the caller until the server stops or the process receives `SIGTERM`/`SIGINT`. Used in container init scripts to keep DuckDB alive. |
| `flock_status()` | row of `(running, uri, sessions, uptime_s)` | Introspect server state. |
| `flock_identify(name, provider, hostname, region, meta)` | row | Sets `whoami_*` settings. Inherited. |
| `whoami()` | table | Inherited. |
| `flock_uri_parser(uri, ssl)` | STRUCT | Parses `flock:` or `quack:` URI. |
| `flock_check_token(sid, client_token, server_token)` | BOOLEAN | Default authentication. |
| `flock_nop_authorization(sid, query)` | BOOLEAN | Default authorization. |

Quack-named aliases (`quack_serve`, `quack_check_token`,
`quack_authentication_function`, etc.) are retained as functional
aliases to ease migration from upstream Quack and to keep upstream
Quack-aware tooling working.

## 10. Observability

### Logging

flock registers two log types in DuckDB's logging system:

- `'Flock'` (LEVEL: DEBUG) — every protocol message, all routes.
  Fields: `route`, `session_id`, `principal`, `client_query_id`,
  `query` (only if `flock_log_queries`), `duration_ms`,
  `response_status`, `error`. Inherited and extended from quack's
  `'Quack'` log type. The `'Quack'` name is also registered as an alias
  for upstream tooling.
- `'HTTP'` (LEVEL: INFO) — underlying transport, inherited from upstream.

Logs go to DuckDB's in-memory buffer by default; persist with:

```sql
CALL enable_logging('Flock', storage => 'file',
                    storage_config => {'path': '/var/log/flock'});
```

### Health vs readiness

- `GET /health` — process is alive. Returns `200` always once the server
  thread is running. No DB introspection.
- `GET /ready` — DB is reachable and accepting queries. Issues a
  `SELECT 1` against a worker connection. Returns `503` on failure.

### Metrics

A `/metrics` endpoint emitting Prometheus-format metrics is on the
roadmap but explicitly **out of scope for v0.1**. Until then, query
DuckDB's own `duckdb_logs_parsed('Flock')` for structured stats.

## 11. Compatibility & versioning

| Versioned thing | flock's commitment |
|---|---|
| **DuckDB engine** | Each flock release pins to a specific DuckDB version (initially v1.5.2). Cross-version load is refused with a clear error. |
| **Quack wire protocol** | Tracks upstream `duckdb/duckdb-quack`. Each rebase point is recorded in `BUILD.md` with the upstream commit. We do not promise wire compatibility across rebase points until DuckDB v2.0 GA. |
| **DuckDB UI protocol** | Tracks upstream `duckdb/duckdb-ui`. Same caveat as Quack. |
| **flock `/sql` JSON format** | Semantic versioning. Added fields and added type encodings are minor; removed fields, changed shapes, or changed type semantics are major. |
| **flock settings names** | Renames carry deprecation aliases for one minor version. |
| **flock URI scheme** | `flock:` and `quack:` both supported indefinitely. |

`GET /info` returns:

```
X-Flock-Version:          0.1.0
X-DuckDB-Version:         v1.5.2
X-DuckDB-Platform:        osx_arm64
X-Quack-Protocol-Version: 1
X-Ui-Extension-Version:   <upstream UI version pinned>
```

## 12. Build & deployment

### Repository layout

```
duckdb-flock/
├── LICENSE
├── THIRD_PARTY_NOTICES.md            ← UI assets and any other vendored notices
├── README.md
├── SPEC.md                           ← this file
├── AGENTS.md                         ← AI agent guide
├── CMakeLists.txt
├── extension_config.cmake
├── vcpkg.json
├── Makefile
├── duckdb/                           ← submodule, pinned to v1.5.2
├── extension-ci-tools/               ← submodule
├── src/
│   ├── flock_extension.cpp           ← entry point, settings, function registration
│   ├── flock_http_server.{cpp,hpp}   ← FlockHttpServer (owns httplib::Server)
│   ├── flock_session.{cpp,hpp}       ← SessionManager
│   ├── flock_auth.{cpp,hpp}          ← AuthManager + cookie HMAC + login/logout handlers
│   ├── flock_log.{cpp,hpp}           ← log type registration
│   ├── flock_wait.{cpp,hpp}          ← blocking flock_wait() table function
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
│   │   └── ui_login_page.{cpp,hpp}   ← flock-specific login wrapper at GET /
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
make release             # builds ./build/release/extension/flock/flock.duckdb_extension
make debug
make test                # runs unit + integration suites
```

CI matrix produces `flock.duckdb_extension` for
`osx_arm64`, `osx_amd64`, `linux_amd64`, `linux_arm64`, `windows_amd64`.
Distribution is via DuckDB's community-extension repository once that's
set up; until then, GitHub Releases.

### Deployment — Incus + ZFS

Reference layout, documented in [`docs/DEPLOY_INCUS_ZFS.md`](./docs/DEPLOY_INCUS_ZFS.md):

```
host
├── /tank/duckdb/<tenant>/                  ← ZFS dataset, COW snapshots
│   ├── db.duckdb
│   ├── flock-init.sql                      ← LOAD flock; flock_serve(...); flock_wait();
│   └── flock-token                         ← created at first boot
└── incus container "flock-<tenant>"
    ├── /usr/bin/duckdb                     ← DuckDB binary
    ├── /root/.duckdb/extensions/.../flock.duckdb_extension
    └── /data → bind-mount of /tank/duckdb/<tenant>/
```

The container's `ENTRYPOINT` is one command:

```bash
duckdb -no-stdin -init /data/flock-init.sql /data/db.duckdb
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
| Integration | spin a real flock server, exercise each route, assert envelope shapes | `test/integration/` |
| Golden Quack | byte-equal regression against captured upstream Quack client traces | `test/golden/quack/` |
| Golden UI | hit `/ddb/run` exactly as the official UI does (recorded), assert byte-equal `application/octet-stream` response | `test/golden/ui/` |
| `/sql` round-trip | for **every** DuckDB type in §5.4, INSERT a value, SELECT it back via `/sql`, decode, compare to a control INSERT through `/quack` | `test/types/` |
| Concurrency | N concurrent sessions, no cross-talk; same-session concurrent → 409 | `test/integration/` |
| Cross-principal | session_id from principal A is not usable by principal B | `test/integration/` |
| Interrupt | start a long query, hit `/interrupt`, assert it actually stops | `test/integration/` |
| Slow client | open `/sql` stream, stop reading, assert server unwinds within 5s | `test/integration/` |
| Auth bypass | every authenticated route returns `401` without token, `403` when authz says false | `test/integration/` |
| Catch-all order | `GET /.*` does not shadow `/info`, `/health`, `/ready`, etc. | `test/integration/` |
| Asset bundle | `flock_ui_assets = 'bundled'` serves the same bytes a real UI build expects | `test/integration/` |
| Identifier safety | `GET /schema/:db/:table` with malicious path parameters cannot SQL-inject | `test/integration/` |
| Daemon mode | `duckdb -no-stdin -init flock-init.sql db.duckdb` with `flock_wait()` keeps the process alive until `SIGTERM` | `test/integration/` |

CI runs all suites against each platform binary on every PR.

## 14. Roadmap (post-v0.1)

Explicitly **out of scope for v0.1**, listed for record:

- **`bundled` UI assets mode** — opt-in for air-gapped / restricted-egress
  deployments. v0.1 ships with `proxy` mode (default), which forwards
  `GET /.*` to `ui.duckdb.org` over the OpenSSL-backed cpp-httplib
  client. The bundled-asset infrastructure (`scripts/fetch-ui-assets.sh`,
  embed step, version pin file) lands when a real use case appears.
- **`flockd` wrapper binary** — a tiny C++ binary that hides the
  `duckdb -no-stdin -init …` invocation behind `flockd /data/db.duckdb`.
  Not shipped in v0.1: the unwrapped command is short, an init script is
  more flexible than a fixed wrapper, and the decision is reversible if
  operator demand justifies the extra binary.
- **Arrow stream output on `/sql`** — `Accept: application/vnd.apache.arrow.stream`
  for full-fidelity column-store interchange.
- **`/metrics`** Prometheus endpoint.
- **WebSocket transport** for `/sql` (server-pushed cancellation, lower
  per-message overhead).
- **Multi-tenant proxy in front of N flocks** — out of scope for the
  extension itself, lives in a separate ops-side project.
- **Replication / read-replicas** — almost certainly via upstream Quack
  ATTACH chains rather than anything flock-specific.
- **Built-in TLS** — only if cpp-httplib's OpenSSL story stops being a
  burden.
- **Clean fork of `duckdb-ui` upstream** — pursue contributing the
  multi-server-on-one-port refactor back upstream so the UI extension
  itself can co-exist with quack on one port without flock as the
  glue. If upstream accepts, flock loses one of its raisons d'être —
  which would be a good outcome.
- **flock client extension** — a small DuckDB extension that registers
  the `flock:` URI scheme on the *client* side so `ATTACH 'flock:host'`
  works natively without aliasing through `quack:`.

## 15. Open questions

Items that need resolution before v0.1 ships. Tracked in GitHub issues:

1. **Quack v1.5-variegata vs DuckDB v2.0 timing.** Start now and absorb
   protocol churn, or wait for v2.0 GA (~fall 2026) and start on a
   stable base? Recommend starting now and pinning to a v1.5.x quack
   commit; rebase to v2.0 as a single deliberate cutover.
2. **Cookie signing key persistence.** Auto-generated per-process means
   restarts log everyone out. For long-lived deployments, document the
   recipe to set `flock_cookie_signing_key` from a host-managed secret.
3. **Bundled UI asset version policy.** Pin per flock release vs.
   floating to upstream UI's `main`? Recommend: pin per flock release;
   one of the things `make release` does is verify the bundle hash.
4. **Flock login wrapper UX.** A single token-paste page is the
   minimum. Any nicer UX (token rotation, multiple users) should be
   delivered as separate UI work, not blocking v0.1.
