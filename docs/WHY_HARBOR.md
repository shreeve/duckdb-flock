# Why harbor

A natural first question when you see harbor's surface area:

> Isn't this just three existing pieces glued together?
>
> 1. The **Quack** extension (binary RPC for `ATTACH 'quack:host'`)
> 2. An **httpserver**-style extension (SQL over HTTP → JSON)
> 3. The **DuckDB UI** extension (browser-based query app)

Yes — those three are the inputs. But the integration is the value,
the way "Postgres = unix sockets + a query parser + a B-tree" is
technically accurate but structurally meaningless.

This document explains what each input gives you in stock form, what
harbor adds beyond the sum, and what harbor explicitly is **not**. It
exists so engineers evaluating harbor (or trying to understand the
~9 months of work that produced v0.1) can ground themselves on the
shape of the deliverable before reading [`SPEC.md`](../SPEC.md).

## The three inputs, stock

| Input | What it is | What it doesn't have |
|---|---|---|
| **`duckdb-quack`** | Binary RPC protocol on `/quack` for `ATTACH 'quack:host'`. Per-connection bearer auth via `CONNECTION_REQUEST`. Owns its own `httplib::Server` on its own port. | No JSON / HTTP-native SQL surface. No browser. No sessions. No admin endpoints. CORS is wildcard. No production deploy story. |
| **`httpserver`-style extensions** (QuackScience and forks) | `POST /query` → SQL → JSON. Independent server, independent port. | One-shot, no streaming. No type-correct encoding (BIGINT / HUGEINT / DECIMAL silently lose precision in JS clients). No sessions / transactions. No principal model. No admin. Wildly different per fork. |
| **`duckdb-ui`** | Browser UI on its own port. Local-loopback `Origin` auth. Connects to the local DuckDB. | Designed only for `duckdb -ui` (single user, this machine). No real auth. No CSRF defense beyond same-origin. Bundles assets ad-hoc. **Unsafe to expose to anyone but yourself.** |

Three ports, three auth models, three logs, three lifecycle stories.
That's what "stack the extensions" gets you.

## What harbor adds — the integration is the engineering work

These don't exist in any of the three sources, and don't fall out of
running them side-by-side. They're what makes harbor more than glue.

### Architectural foundation

1. **One `httplib::Server` for all three protocols.**
   PR-2 extracted server ownership into `HarborHttpServer` and made
   Quack / UI / SQL register routes against the shared instance.
   Enforced at the source level by
   [`architecture-guard.yml`](../.github/workflows/architecture-guard.yml)
   (single-owner check). Without this, none of the rest is possible —
   you can't have a unified cookie if you have three servers, and
   route-order discipline (catch-all `GET /.*` must be last) doesn't
   even apply across processes.

2. **One auth model across all three protocols.**
   `harbor_authentication_function` (BOOLEAN-returning) is called by
   Quack `CONNECTION_REQUEST`, `/sql` first request, AND `/ddb/run`
   first request. `harbor_authorization_function` is called by Quack
   `PREPARE_REQUEST` / `APPEND_REQUEST`, every `/sql`, every
   `/ddb/run`, AND every admin endpoint via synthetic
   `__HARBOR_ADMIN__:resource:action` queries. **One policy function
   controls everything.** `principal_id = hex(sha256(token))` is
   identical across all three. Stock world: three separate auth
   dances.

3. **HMAC-signed cookies as a unified session credential.**
   `harbor_session=v1.<principal_hex>.<expiry>.<nonce>.<hmac32>` is
   issued by `/auth/login` and accepted on `/sql`, `/ddb/*`, AND
   `/quack` (where applicable). Single login, three protocols.
   Cookie signing key is ephemeral per-process (no SQL-readable
   secret). None of the three sources have this — and `duckdb-ui`
   was actively unsafe to expose remotely until harbor's PR-4
   added it.

### Data plane

4. **Principal-owned SQL sessions with anti-enumeration.**
   `POST /sql/sessions/new` creates an explicit session;
   `owner_principal_id` is checked on every lookup; wrong-principal
   collapses to `404 SESSION_NOT_FOUND` (you can't enumerate other
   principals' session IDs). `/auth/logout?destroy_sessions=true`
   tears them down cleanly. `httpserver` extensions don't have
   sessions at all; Quack has them but they're per-connection,
   not per-principal.

5. **NDJSON streaming with mid-stream error invariant.**
   `/sql` streams DataChunks with the buffer-before-write
   invariant: each NDJSON line is fully built in memory before
   `sink.write()`, so an exception cleanly emits a final
   `{"type":"error","code":"...","message":"..."}` line — HTTP
   status stays 200 because headers are already on the wire.
   Three response modes: `application/x-ndjson` row-mode,
   `application/x-ndjson; shape=chunk` chunk-mode, and one-shot
   `application/json`. Stock httpserver extensions are one-shot
   only.

6. **Type-precision-correct JSON encoding, golden-tested per type.**
   `BIGINT` / `HUGEINT` / `UBIGINT` / `UHUGEINT` / `DECIMAL` as JSON
   strings (JS precision). `MAP<K,V>` as array-of-pairs (keys can
   be non-string; ordering matters). JSON columns as JSON-text
   strings (disambiguates SQL NULL from JSON null). `INTERVAL` as
   `{months, days, micros}` with `micros` as a string. `BLOB` as
   base64. [`scripts/golden-sql-types.sh`](../scripts/golden-sql-types.sh)
   round-trips every DuckDB type; 62 type assertions today. None of
   this is automatic — it's design choices forced by "we want the
   wire to be lossless."

7. **Mode-B parameter parser with nested types.**
   `{"type": "LIST<STRUCT(a INT, b VARCHAR)>", "value": [...]}`.
   Whitespace-tolerant, rejects malformed types early, validates
   against value shape. Built specifically for harbor in PR-7d.

### Policy plane

8. **Synthetic admin-authz model.**
   No "admin token." Admin endpoints route through the SAME
   `harbor_authorization_function` with synthetic
   `__HARBOR_ADMIN__:resource:action` queries. Default-deny when
   unconfigured (`harbor_allow_admin_without_authz=false`).
   Resource:action pairs (not bare verbs) so granular grants
   work. One policy function gates both data SQL AND `/whoami` /
   `/tables` / `/schema/:db/:t` / `/sessions` / `/interrupt` /
   `/checkpoint`.

9. **Query-timeout enforcement.**
   `harbor_query_timeout_s` with sweeper thread + RAII watchdog +
   generation counters. Background thread `Connection::Interrupt()`s
   queries that exceed the budget. Mid-stream timeout emits
   `QUERY_TIMEOUT` not silent truncation. Doesn't exist in any of
   the three sources.

### Wire compat + UI security

10. **PR-8 credential-strip in the UI proxy.**
    `HandleProxyGet` forwards only `Accept`,
    `Accept-Encoding=identity`, `Accept-Language`, `If-None-Match`,
    `If-Modified-Since`, `Range` to `ui.duckdb.org`. **Never**
    `Cookie`, `Authorization`, `X-Harbor-*`, `Origin`, `Sec-*`.
    This is a NEW security invariant that didn't exist in stock UI
    because stock UI didn't proxy through a third-party domain.
    Golden-tested with a fake-`ui.duckdb.org` listener in
    [`scripts/golden-cookie-auth.sh`](../scripts/golden-cookie-auth.sh).

11. **Wire-format compatibility with stock clients, golden-tested.**
    A vanilla DuckDB with stock `duckdb-quack` installed can
    `ATTACH 'quack:host'` against harbor. The official DuckDB UI
    works against `GET /` and `POST /ddb/*` byte-for-byte.
    `golden-quack-roundtrip.sh` and `golden-ui-roundtrip.sh`
    enforce this. That's not "glue," that's "we changed everything
    underneath while preserving every byte on the wire."

### Operations

12. **Production deployment story.**
    `systemd` unit, `Dockerfile`, `docker-compose.yml`,
    [`scripts/validate-deployment.sh`](../scripts/validate-deployment.sh),
    [`scripts/load-test.sh`](../scripts/load-test.sh) (auto-detects
    `oha` / `wrk`), four [`examples/auth/*.sql`](../examples/auth/)
    recipes (static / multi-tenant / expiring / RBAC),
    [`CHANGELOG.md`](../CHANGELOG.md), [`docs/DEPLOYMENT.md`](DEPLOYMENT.md),
    threat-model docs at [SPEC.md §7](../SPEC.md#threat-model),
    `INSTALL harbor FROM community`. None of the three sources have
    this — they're each "load it and it works on my machine," not
    "deploy it as a fleet of production HTTP services."

## What harbor is NOT

Boundary-setting matters as much as feature-listing. Harbor is **not**:

- **A replacement for MotherDuck.** MotherDuck is a managed cloud
  service with sharing, persistence, billing, and a hosted control
  plane. Harbor is a "host your own multi-protocol DuckDB endpoint"
  tool. They serve different audiences.

- **A database.** One DuckDB process, one storage. No
  database-level multi-tenancy. No replication. No clustering. If
  you want a sharded analytical database, harbor is not it; if you
  want one DuckDB exposed cleanly over the network, it is.

- **A SQL parser or proxy.** Harbor doesn't analyze or rewrite SQL
  by default. The authz callback _can_ inspect SQL text if the
  operator wires that up — but harbor itself is protocol-level, not
  query-level.

- **A replacement for stock Quack.** Harbor IS stock Quack (vendored
  verbatim, see [`docs/upstream-quack-patches.md`](upstream-quack-patches.md))
  plus the rest. If you only want Quack RPC and nothing else, use
  stock Quack — fewer moving parts.

## Bottom line

Harbor is **a unified multi-protocol HTTP gateway around a single
DuckDB process**, the way Supabase is a unified multi-protocol
gateway around a single Postgres. The three protocols are real and
separately-useful (Quack for programmatic-low-latency, `/sql` for
HTTP-native-ad-hoc, DuckDB UI for human-exploration), but the
**value is one auth model, one port, one config surface, one log
stream, one production deploy artifact**, with type-correct
encoding and wire compat with stock clients.

The reason this is ~9 months of architecture work rather than
~2 weeks of CMake glue is that "merge three servers into one"
forces you to redesign auth, sessions, route ordering, CORS,
lifecycle, observability, and the production story all at once —
and you only get that right by writing it from a spec, not by
stacking extensions.

## Where to read next

- [`SPEC.md`](../SPEC.md) — authoritative design, including the
  threat model in §7 that this doc references.
- [`AGENTS.md`](../AGENTS.md) — contributor map, including the
  full PR-by-PR implementation roadmap.
- [`docs/DEPLOYMENT.md`](DEPLOYMENT.md) — operator runbook for
  taking harbor from `harbor_serve` to production.
- [`examples/auth/`](../examples/auth/) — copy-paste-ready auth
  callback recipes.
