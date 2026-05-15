# HANDOFF — duckdb-flock

> **Purpose:** Current handoff only. Old PR-1 through PR-4-era tasks
> were removed because they are done and merged. Read this file after
> `AGENTS.md` and `SPEC.md` if resuming work mid-PR.

**Last updated:** 2026-05-15 05:30 MDT  
**Last fully merged `main`:** `35e6c43` — PR-11: cancel PR-10b roadmap, strengthen PR-9 rationale  
**Active branch:** `pr5-sql-endpoint`  
**Project repo:** `/Users/shreeve/Data/Code/duckdb-flock` · GitHub `shreeve/duckdb-flock`

## TL;DR

flock is a DuckDB extension that turns one DuckDB process into a
multi-protocol HTTP service on one port:

- Quack RPC (`POST /quack`) — merged and roundtrip-tested.
- DuckDB UI (`/ddb/*`, `/localEvents`, `/localToken`, `GET /.*`) —
  merged, cookie-gated, credential-strip tested.
- JSON SQL (`POST /sql`) — **PR-5 is in progress locally**.

The architecture is intentionally still `httplib + OpenSSL` for v0.1.
The proposed PR-10b migration to mbedTLS/HTTPUtil/plain-httplib was
evaluated and declined. Do not restart that migration unless the
trigger conditions in `AGENTS.md` are met.

## Merged state on `main`

Latest merged chain:

| PR | Status | Notes |
|---|---|---|
| PR-1 | merged | Vendored `duckdb-quack` as `src/quack/`; extension build name `flock`. |
| PR-1.5 | merged | `/quack` runtime roundtrip in `test/sql/flock.test`; current regression baseline. |
| PR-2 | merged | Shared `FlockHttpServer`, `SessionManager`, `AuthManager`, `flock_serve`/`stop`/`wait`, `/health`, `/info`. |
| PR-3 | merged | Vendored `duckdb-ui`; `UiHandlers`; `duckdb_httplib_openssl::` namespace; UI proxy mode. |
| PR-4 | merged | `flock_crypto`, cookie auth, `/auth/login`, `/auth/logout`, cookie-gated UI and `/ddb/*`, CORS allow-list. |
| PR-7 | merged | Corrected misleading CSPRNG comments (`GetEncryptionUtil` is OpenSSL-via-httpfs). |
| PR-8 | merged | Security fix: UI proxy strips `Cookie`, `Authorization`, `X-Flock-*`, etc. before forwarding upstream. |
| PR-10a | merged | Docs: v0.1 UI assets = proxy/disabled; bundled deferred to v0.2. |
| PR-11 | merged | Docs: cancelled PR-10b migration; strengthened why `curl` is required. |

All merged PRs were green on the 7-target CI matrix at merge time.

## Active work: PR-5 (`/sql` endpoint)

### Branch

`pr5-sql-endpoint`

### Current local status

Uncommitted local changes currently touch:

```text
src/CMakeLists.txt
src/auth_handlers.cpp
src/flock_http_server.cpp
src/flock_session.cpp
src/include/auth_handlers.hpp
src/include/flock_http_server.hpp
src/include/flock_session.hpp
src/quack/quack_extension.cpp
src/include/sql_chunk_encoder.hpp
src/include/sql_handlers.hpp
src/include/sql_json_writer.hpp
src/include/sql_param_decoder.hpp
src/sql_chunk_encoder.cpp
src/sql_handlers.cpp
src/sql_json_writer.cpp
src/sql_param_decoder.cpp
```

The branch currently **builds**:

```bash
make release
```

last completed successfully after the most recent fixes.

Existing sqllogic tests still pass:

```bash
make test_release
# 43/43 assertions pass
```

### What is implemented locally so far

1. **`SqlJsonWriter`**
   - Files: `src/include/sql_json_writer.hpp`, `src/sql_json_writer.cpp`.
   - Minimal JSON output helper.
   - Escapes quotes, backslashes, control chars, preserves valid UTF-8,
     replaces invalid UTF-8 with U+FFFD.
   - Important round-15 rule: buffer JSON into a string before writing
     to network; never half-write a JSON object.

2. **`SqlChunkEncoder`**
   - Files: `src/include/sql_chunk_encoder.hpp`, `src/sql_chunk_encoder.cpp`.
   - Emits schema, row, chunk, end, error, one-shot JSON.
   - Covers many SPEC §5.4 types:
     integer family, hugeint/uhugeint as strings, decimal as string,
     varchar, uuid, date/time/timestamp/timestamptz, interval object,
     blob base64, list/array/struct/map/union/enum, fallback extension
     types as `lossless:false`.
   - Known caveat: complex nested types compile but need more HTTP
     golden tests before trusting fully.

3. **`SqlParamDecoder`**
   - Files: `src/include/sql_param_decoder.hpp`, `src/sql_param_decoder.cpp`.
   - Parses request body and `params` array.
   - Supports Mode A implicit coercion and Mode B `{type,value}` wrapper.
   - Uses `TransformStringToLogicalType(type, context)` for Mode B.
   - Uses `duckdb::vector<Value>` intentionally so
     `PreparedStatement::Execute(vector<Value>&)` overload is selected.

4. **Principal-owned sessions**
   - Files: `src/include/flock_session.hpp`, `src/flock_session.cpp`.
   - `FlockSession` now has `owner_principal_id`.
   - New methods:
     - `CreateOwnedSession(session_id, principal_id)`
     - `LookupOwnedSession(session_id, principal_id)`
     - `DestroyOwnedSession(session_id, principal_id)`
     - `DestroyAllOwnedBy(principal_id)`
   - Legacy Quack sessions still use `CreateNewConnection()` and have
     empty owner. Do not migrate Quack in PR-5.

5. **`SqlHandlers`**
   - Files: `src/include/sql_handlers.hpp`, `src/sql_handlers.cpp`.
   - Routes:
     - `POST /sql`
     - `POST /sql/sessions/new`
     - `DELETE /sql/sessions/<id>`
   - Supports:
     - Auth via `AuthManager::AuthenticateRequest`.
     - Authz via `flock_authorization_function`.
     - Rejects client SQL beginning with `__FLOCK_ADMIN__:`.
     - Rejects multi-statement SQL.
     - Rejects ephemeral `BEGIN` / `START TRANSACTION`.
     - NDJSON streaming default.
     - One-shot JSON with `Accept: application/json`.
     - Chunk mode with `Accept: application/x-ndjson; shape=chunk`.
     - `flock_max_request_body_bytes` guard.
     - `flock_max_response_rows` in streaming path.
   - Important round-15 rule: mid-stream exceptions are caught and an
     error NDJSON line is emitted immediately in the catch; do not rely
     on httplib invoking the provider again.

6. **Wiring**
   - `src/CMakeLists.txt` includes new PR-5 files.
   - `FlockHttpServer` owns `SqlHandlers`.
   - `AuthHandlers` now also owns `OPTIONS /sql` preflight.
   - `/auth/logout?destroy_sessions=true` now destroys SQL sessions
     owned by the authenticated principal.

7. **Settings**
   - `src/quack/quack_extension.cpp` registers:
     - `flock_max_sessions`
     - `flock_max_response_rows`
     - `flock_max_request_body_bytes`

### Verification already done

Local verification after the `/sql/sessions/<id>` DELETE regex fix:

```bash
make release
make test_release                         # 43/43
scripts/golden-cookie-auth.sh             # 14/14
scripts/golden-sql-roundtrip.sh           # all assertions
```

The new `/sql` golden test covers:

- `OPTIONS /sql` CORS preflight.
- Default NDJSON row mode.
- NDJSON chunk mode.
- One-shot JSON mode.
- Missing `sql`, multi-statement reject, `__FLOCK_ADMIN__:` reject,
  oversized body reject.
- Invalid bearer reject.
- Cookie auth after `/auth/login`.
- Implicit params (`$1`) and typed wrapper params (`DECIMAL`, typed NULL).
- Representative type encodings: BIGINT string, DECIMAL string,
  INTERVAL object, BLOB base64, JSON text string.
- Explicit SQL sessions: create, transaction state across requests,
  delete, deleted-session 404.
- `/auth/logout?destroy_sessions=true` destroys owned SQL sessions.

Earlier live smoke on `127.0.0.1:19498` also manually confirmed:

- `GET /info` → 204.
- `POST /sql` simple NDJSON:
  ```ndjson
  {"type":"schema",...}
  {"type":"row","values":[42,"hello"]}
  {"type":"end","rowCount":1,"timeMs":0}
  ```
- `POST /sql` with `Accept: application/json` returned one-shot JSON.
- Bad bearer token → 401.
- Multi-statement request → 400.
- `__FLOCK_ADMIN__:` request → 400.
- `POST /sql/sessions/new` creates a session.
- Session-bound transaction flow worked:
  `BEGIN`, `CREATE TABLE`, `INSERT`, `SELECT count(*)`, `ROLLBACK`.
- Parameterized session query worked:
  `SELECT i FROM t WHERE i > $1` with `params:[1]`.
- Type smoke for `DECIMAL`, `INTERVAL`, `DATE`, `BLOB` looked correct:
  decimal string, interval object, date string, base64 blob.

Issue discovered and fixed:

- `DELETE /sql/sessions/:id` initially returned 404 because cpp-httplib's
  `:id` path-param syntax is not actually used by `Server::Delete()`;
  it routes through regex directly. Fixed locally with explicit regex
  `^/sql/sessions/([^/]+)$` and `req.matches[1]`. Re-smoked: first
  DELETE returns 200 JSON, second DELETE returns 404 `SESSION_NOT_FOUND`.

Known smoke caveat:

- The quick same-session concurrency smoke did not trigger 409 because
  the first query finished too fast. Need a better test to hold the
  session mutex long enough or skip 409 smoke until an integration test
  can drive a genuinely slow query.

## Remaining PR-5 tasks

### Correctness fixes / cleanup

1. Re-run live smoke after the DELETE regex fix:
   - `POST /sql/sessions/new`
   - `DELETE /sql/sessions/<sid>` should return 200 with JSON body.
   - second delete should return 404 JSON envelope.

2. Review `SqlHandlers` for rough edges:
   - `PreparedStatement::GetExpectedParameterTypes()` handling for
     named/non-positional parameters is basic. Verify with `$1`, `$2`.
   - `is_select_statement = stmt_type == SELECT_STATEMENT` may classify
     `EXPLAIN`, `DESCRIBE`, `SHOW`, or DML `RETURNING` too simplistically.
     PR-5 can keep this minimal if tests cover the intended surface.
   - One-shot JSON currently ignores `truncated`; streaming `end` has
     `truncated:true`. Decide whether one-shot should include the flag.
   - `SqlParamDecoder` is minimal, not a full JSON parser. Good enough
     for PR-5 if tests cover request shapes we claim to support.

3. Confirm `SqlChunkEncoder` APIs compile and behave for:
   - `BIGINT` and `UBIGINT`
   - `HUGEINT` and `UHUGEINT`
   - `DECIMAL`
   - `DOUBLE NaN/Infinity`
   - `TIMESTAMP` vs `TIMESTAMPTZ`
   - `INTERVAL`
   - `BLOB`
   - `JSON` column value as string
   - `LIST`, `STRUCT`, `MAP`

### Tests done

- `scripts/golden-sql-roundtrip.sh` added and passing.
- `test/sql/flock.test` extended with PR-5 setting defaults and now
  passes 43 assertions.

### Docs done / still needed

- `AGENTS.md` PR-5 acceptance closure section added.
- README implementation-status block updated: `/sql` works after PR-5.
- No SPEC divergence intentionally introduced. If final review finds a
  mismatch, prefer code changes over spec changes.

### Commit / PR workflow

Suggested local commit split:

1. `PR-5 step 1/N: JSON writer + chunk encoder + param decoder`
2. `PR-5 step 2/N: principal-owned sessions + SqlHandlers wiring`
3. `PR-5 step 3/N: golden SQL tests + docs`

Then:

```bash
make release
make test_release
scripts/golden-cookie-auth.sh
scripts/golden-sql-roundtrip.sh
git push -u origin pr5-sql-endpoint
gh pr create ...
```

As usual: wait for all 7 CI checks, do final GPT-5.5 spot-check, then
squash-merge.

## Design decisions / caveats to preserve

- Keep `httplib + OpenSSL` architecture. Do not restart PR-10b.
- Keep `vcpkg.json` as `["openssl", "curl"]`; both are required by
  httpfs sibling build and runtime.
- Do not touch `src/quack/quack_message.{cpp,hpp}`.
- Do not edit `misc/`.
- Do not change `/quack` wire format. `test/sql/flock.test` roundtrip
  is the regression baseline.
- For streaming `/sql`, always buffer an entire NDJSON line or chunk
  before `sink.write()`.
- Hold the session mutex for the full streaming lifetime.
- Keep `ActiveRequestGuard` alive for the full streaming provider
  lifetime, not just the route lambda.
- `/sql/cancel`, query-timeout enforcement, and admin endpoints remain
  out of PR-5 scope.

## GPT-5.5 context

Persistent AI conversation id: `duckdb-flock-spec`.

Latest relevant round:

- **Round 15** (`/sql` design check, cost ~$0.32) confirmed:
  - roll-own JSON writer is fine if tested hard;
  - stream by DataChunk/provider-call;
  - buffer-before-write;
  - emit mid-stream errors in catch immediately;
  - always prepare once, introspect param types, execute same prepared
    statement;
  - leave UiHandlers connection pool alone in PR-5;
  - include `/sql` CORS preflight in PR-5;
  - do not defer session ownership checks, 409, authz, or midstream
    error records.
- **Round 16** (`/sql` implementation review, cost ~$0.49) found real
  pre-commit blockers:
  - streaming catch only wrapped `Fetch()` and not encoder errors;
  - `/sql` body cap happened after httplib had already buffered body;
  - cookie-auth `/sql` lacked Origin/Referer CSRF gate;
  - Mode B wrapper detection was key-order dependent;
  - OPTIONS preflight did not cover `/sql/sessions/*`.
- **Round 17** (`/sql` blocker-fix follow-up, cost ~$0.31) said the
  blocker fixes were sufficient for implementation readiness pending
  normal code review / CI. It also recommended defensive handling if
  error-line encoding itself throws; that was implemented with
  `EmitStreamingErrorSafe()`.

## If resuming after a reconnect

1. `cd /Users/shreeve/Data/Code/duckdb-flock`
2. `git status -sb`
3. Confirm branch is `pr5-sql-endpoint`.
4. Run:
   ```bash
   make release
   make test_release
   ```
5. Re-run the `/sql` live smoke, especially session DELETE.
6. Continue with tests + docs.
