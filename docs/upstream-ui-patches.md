# Upstream `duckdb-ui` patches

harbor vendors `duckdb-ui`'s `src/` tree at branch `main` (commit
`3339df3`, fetched 2026-05). This document tracks every edit harbor
makes to vendored upstream files. Mirrors the `upstream-quack-patches.md`
pattern.

**Last vendor pull:** 2026-05-14 against upstream commit `3339df3`.

## Edits to `src/ui/include/version.hpp`

| # | What | Why | Conflict risk on rebase |
|---|---|---|---|
| 1 | Replace `#error` directives with fallback `#define` defaults for `UI_EXTENSION_SEQ_NUM` (`"0"`) and `UI_EXTENSION_GIT_SHA` (`"embedded-in-harbor"`) | Upstream's CMakeLists.txt computes these from `git rev-list --count HEAD` and `git rev-parse --short=10 HEAD`. harbor doesn't have a separate UI extension build cycle, so we provide reasonable fallbacks. To override, pass `-DUI_EXTENSION_SEQ_NUM=N -DUI_EXTENSION_GIT_SHA=...` via `EXT_FLAGS` in the Makefile. | Low — this is a tiny header that rarely changes upstream. |

## Edits to `src/ui/include/watcher.hpp` and `src/ui/watcher.cpp`

| # | What | Why | Conflict risk on rebase |
|---|---|---|---|
| 2 | Constructor signature changed from `Watcher(HttpServer &server)` to `Watcher(weak_ptr<DatabaseInstance> db, EventDispatcher &dispatcher, uint32_t polling_interval_ms)`. Member `HttpServer &server` removed; replaced with the three direct dependencies. The body of `Watch()` updated to call `ddb_instance.lock()` and `dispatcher.SendXxxEvent()` instead of `server.LockDatabaseInstance()` / `server.event_dispatcher->SendXxxEvent()`. Also, `polling_interval` reads from the constructor-injected member instead of `GetPollingInterval(*con.context)`. | Decouples Watcher from upstream's HttpServer class (which we deleted in PR-3 — see entries below). UiHandlers can own the Watcher directly without inheriting upstream's HttpServer interface. | Medium — Watcher's `Watch()` body has the catalog-polling logic that upstream is most likely to evolve. |

## Files DELETED entirely from the vendored tree

| File | Why |
|---|---|
| `src/ui/http_server.cpp` (737 LOC upstream) | Refactored into `src/ui/ui_handlers.cpp`. Upstream's `HttpServer` was a singleton owning its own `duckdb_httplib_openssl::Server` + listener thread + atexit + Run() lifecycle. harbor's architecture (per SPEC §2 + PR-2) puts the single httplib::Server on HarborHttpServer; UiHandlers is a stateless route registrar that registers against the shared server. The handler bodies (HandleGetLocalToken, HandleInterrupt, HandleRun, DoHandleRun, HandleTokenize, HandleGet → HandleProxyGet) are preserved nearly verbatim in `ui_handlers.cpp`. |
| `src/ui/include/http_server.hpp` | Replaced by `src/ui/include/ui_handlers.hpp` (declares the new UiHandlers class). |

## Files EXCLUDED from the build (still on disk for reference)

| File | Why |
|---|---|
| `src/ui/ui_extension.cpp` | Has its own `DUCKDB_CPP_EXTENSION_ENTRY(ui, ...)` C entry symbol that would conflict with harbor's `DUCKDB_CPP_EXTENSION_ENTRY(harbor, ...)`. The pieces of `LoadInternal` we need (UI extension settings registration, `"ui"` StorageExtension registration via `UIStorageExtensionInfo`) were absorbed into `src/quack/quack_extension.cpp::LoadInternal`. |

## Other vendored files — NO edits

Every other file under `src/ui/` is verbatim from upstream. To verify:

```bash
diff -ru misc/duckdb-ui/src/ src/ui/
```

Expected differences: `version.hpp`, `watcher.{hpp,cpp}`, `ui_handlers.{cpp,hpp}` (new), `http_server.{cpp,hpp}` (deleted).

## Rebasing process

When upstream `duckdb-ui` ships a new commit on `main`:

```bash
# 1. Refresh the upstream reference clone.
( cd misc/duckdb-ui && git fetch && git checkout main && git pull )

# 2. Diff our vendored copy against the new upstream.
diff -ru misc/duckdb-ui/src/ src/ui/

# 3. For each non-trivial change in upstream:
#    - Inspect the diff.
#    - Apply to src/ui/ if applicable (most upstream changes will be
#      drop-in: copy the new file content over).
#    - For changes inside http_server.cpp specifically: there is no
#      harbor counterpart of that file anymore — instead, port the
#      diff into src/ui/ui_handlers.cpp manually. Most route handler
#      bodies should diff cleanly because we preserved them verbatim.
#    - For changes inside watcher.cpp: re-apply edit #2 above.
#    - For changes inside version.hpp: re-apply edit #1.
#    - For changes inside ui_extension.cpp: absorb into our
#      src/quack/quack_extension.cpp::LoadInternal as needed.

# 4. Run the build + tests locally:
make release && make test_release

# 5. Critical: run a smoke test against the running server to verify
#    /info returns X-DuckDB-UI-Extension-Version (the official UI
#    bundled at ui.duckdb.org checks for this header):
./build/release/duckdb -unsigned -no-stdin -c "
  LOAD '$PWD/build/release/extension/harbor/harbor.duckdb_extension';
  CALL quack_serve('quack:localhost:19495', token='smoke');
  CALL harbor_wait();
" &
sleep 2 && curl -sf -i http://localhost:19495/info | grep X-DuckDB-UI

# 6. Update this file's "Last vendor pull" date and upstream commit
#    hash above. Add any new edit rows if needed.

# 7. Commit with a message like:
#    "rebase ui to <upstream-commit>"
```

## Architectural notes

- Cookie auth (HMAC-signed `harbor_session` cookie, `POST /auth/login`,
  harbor login wrapper at `GET /`) shipped in v0.1. `HandleInterrupt`,
  `HandleRun`, `HandleTokenize`, `HandleGetLocalToken`, and
  `/localEvents` all run a cookie-aware auth path through
  `AuthorizeUiRequest()` that supplements the Origin-set check; the
  Origin check uses the `harbor_cors_origins` allow-list. PR-7c added
  Content-Security-Policy + per-request CSPRNG nonce on the login
  page itself.
- `HandleProxyGet` does runtime HTTPS proxying to `ui.duckdb.org` over
  the cpp-httplib OpenSSL HTTPS client. PR-8 hardened it to strip
  every harbor credential header (`Cookie`, `Authorization`,
  `X-Harbor-*`, `Origin`, `Sec-*`) before forwarding upstream, with a
  positive allow-list of asset-fetch headers (`Accept`,
  `Accept-Encoding`, `Accept-Language`, `If-None-Match`,
  `If-Modified-Since`, `Range`).
- **Bundled UI assets mode is genuinely post-v0.1.** SPEC §8 reserves
  it for air-gapped deployments. When it lands, `HandleProxyGet`
  becomes one of two implementations selected at request time by a
  `harbor_ui_assets` setting; the proxy implementation here stays as
  the other branch. No code or setting exists for this yet.
- **Smell-check rule:** if the vendored-edit list grows beyond ~10
  entries, the architectural integration isn't actually moving us
  off the vendored substrate. Treat it as a signal to refactor
  harbor's own files instead of patching upstream's.
