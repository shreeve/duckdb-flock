# HANDOFF — duckdb-flock

> **Purpose:** Hand off in-flight work between sessions. Read this top
> to bottom before doing anything; everything you need to continue is
> here.

**Last updated:** 2026-05-14 (afternoon)
**Last fully-merged commit on `main`:** `efd130a` — PR-2: extract httplib::Server from QuackServer into shared FlockHttpServer
**Active branch:** `pr3-ui-port-and-cookie-auth` (uncommitted; see "In flight" below)
**Project repo:** `/Users/shreeve/Data/Code/duckdb-flock` · GitHub `shreeve/duckdb-flock`

## TL;DR

flock is a DuckDB extension that turns one DuckDB process into a
multi-protocol HTTP service (Quack RPC + JSON `/sql` + DuckDB UI), all
on one port, behind one shared session/auth model. We are mid-implementation.
PR-1, PR-1.5, and PR-2 are all merged on `main`; PR-3 (UI port) is
**partially in progress** on a feature branch with vendoring complete
but no integration code written yet.

## What's on `main` right now

| Commit | What it shipped |
|---|---|
| `efd130a` | **PR-2:** Extracted `httplib::Server` from upstream's `QuackServer`/`HttpQuackServer` into a process-static `FlockHttpServer`. `SessionManager` + `AuthManager` are standalone subsystems. `flock_serve` / `flock_stop` / `flock_wait` table functions per SPEC §9 (single-server-per-process). `quack_serve` / `quack_stop` are thin shims delegating to the same backing state. `/health` and `/info` admin endpoints. CI grep guard enforces single `duckdb_httplib::Server` owner. **5/5 platform variants green.** |
| `1ae829a` | **docs:** Kept OpenSSL (not dropping); restored `proxy` as the default `flock_ui_assets` mode (`bundled` deferred to post-v0.1). Added `src/flock_crypto.{cpp,hpp}` plan for PR-3 (OpenSSL `libcrypto` wrappers; no vendored Brad Conte SHA-256). |
| `d9d12dd` | **PR-1.5:** `/quack` runtime roundtrip test in `test/sql/flock.test`. Regression baseline that PR-2 had to pass unchanged. Includes `LOAD_TESTS` opt-in for sqllogictest discovery. `docs/upstream-quack-patches.md` documents the 5 surgical edits to vendored `quack_extension.cpp`. |
| `2e032df` | **PR-1:** Vendored `duckdb-quack` source (commit `90bd70e` of branch `v1.5-variegata`) as `src/quack/`. Renamed build identifiers (`EXT_NAME=flock`, `TARGET_NAME=flock`). Five surgical edits to `quack_extension.cpp` (entry symbol, version macro, `flock_version()` registration, `Name()` returns `"flock"`, `SetDescription` text). |
| `5b8e89b` | **docs:** Locked in PR-1 plan; dropped `flockd` wrapper binary and UI proxy mode (the latter restored later in `1ae829a`). |

## In flight (PR-3 work, on `pr3-ui-port-and-cookie-auth` branch)

**Branch:** `pr3-ui-port-and-cookie-auth`
**Uncommitted local changes:** vendored UI source + version.hpp patch.

```
?? src/ui/    (1461 LOC vendored from misc/duckdb-ui/src/, plus
                src/ui/include/version.hpp patched to provide
                fallback defaults for UI_EXTENSION_SEQ_NUM /
                UI_EXTENSION_GIT_SHA — see "Critical findings" below)
```

**Nothing else has been touched on this branch. Nothing has been committed or pushed yet.**

### What was supposed to be done in PR-3 (per AGENTS.md "Implementation roadmap")

Per the user's choice of split-strategy (B): vendor + adapt UI handlers, NO cookie auth changes (those move to PR-4).

| Task | Status |
|---|---|
| Vendor `misc/duckdb-ui/src/` → `src/ui/` | DONE locally, NOT committed |
| Patch `src/ui/include/version.hpp` for build-time defines | DONE locally, NOT committed |
| **Migrate FlockHttpServer to `duckdb_httplib_openssl::Server`** | NOT STARTED — see Critical findings below; this is necessary work I discovered mid-session |
| Refactor `src/ui/http_server.cpp` (737 LOC) → new `UiHandlers` class | NOT STARTED |
| Adapt `ui_extension.cpp` settings/storage-extension into our `quack_extension.cpp::LoadInternal` | NOT STARTED |
| Wire `UiHandlers` into `FlockHttpServer::RegisterBuiltinHandlers` | NOT STARTED |
| Reconcile `/info` between AdminHandlers + UI (UI wants `X-DuckDB-UI-Extension-Version` header) | NOT STARTED |
| Update `src/CMakeLists.txt` (compile new `src/ui/` sources, exclude `ui_extension.cpp`) | NOT STARTED |
| Update CI grep guard for the new namespace migration | NOT STARTED |
| Add `docs/upstream-ui-patches.md` (mirroring `upstream-quack-patches.md` pattern) | NOT STARTED |

## Critical findings to internalize before continuing PR-3

### 1. The `httplib` namespace migration is required

I had originally claimed PR-3 wouldn't need to touch FlockHttpServer's underlying `duckdb_httplib::Server` type. **That was wrong.** Mid-session I discovered:

DuckDB compiles cpp-httplib **twice** — once without OpenSSL (namespace `duckdb_httplib`) and once with (namespace `duckdb_httplib_openssl`). The two namespaces produce **separate C++ types** (not just different builds with the same names):

- `duckdb_httplib::Request` ≠ `duckdb_httplib_openssl::Request` (compiler considers these unrelated)
- `duckdb_httplib::Server::Get(path, lambda)` requires `lambda` to take `duckdb_httplib::Request&`
- Upstream UI's handlers take `duckdb_httplib_openssl::Request&`

To register UI handlers against our shared FlockHttpServer, **either**:

**(a) Migrate FlockHttpServer to `duckdb_httplib_openssl::Server`** (clean — cascades through ~5 files of PR-2 code: `flock_http_server.{cpp,hpp}`, `quack_server.{cpp,hpp}`, `admin_handlers.{cpp,hpp}`, plus the `.github/workflows/architecture-guard.yml` grep). The `_openssl` variant is API-compatible with the plain variant — same surface plus HTTPS support. ~50-100 LOC of mechanical change cascading through the named files. **Recommended.**

**(b) Add type-conversion shims in UiHandlers** so openssl-namespace Handle methods can be called from plain-namespace lambdas. Manual field copying between Request/Response types. ~50 LOC of ugly glue per Handle. NOT recommended.

**Decision lock:** go with (a). Update PR-2's invariant accordingly. The CI grep guard at `.github/workflows/architecture-guard.yml` needs to detect ownership patterns for the openssl namespace too:

```bash
# new pattern (matches both variants)
grep -REn '(make_uniq|unique_ptr|shared_ptr)<\s*duckdb_httplib(_openssl)?::Server\s*>' src
```

The single-owner invariant still holds — it's just `duckdb_httplib_openssl::Server` instead of `duckdb_httplib::Server`. AGENTS.md "PR-2 acceptance checklist" reference to the grep needs updating to the new pattern.

### 2. The `MemoryStream` pattern in `/quack` is NOT a bug

GPT-5.5 round-8 review flagged this code as a use-after-free / wrong-bytes bug:

```cpp
MemoryStream stream;
content_reader([&](const char *data, size_t data_length) {
    stream.WriteData((data_ptr_t)data, data_length);
    return true;
});
auto response = self->HandleMessage(stream);
response->ToMemoryStream(stream);  // SAME stream — does this corrupt?
res.set_content((const char *)stream.GetData(), stream.GetPosition(), "application/vnd.duckdb");
```

**It's safe.** `QuackMessage::ToMemoryStream(write_stream)` (in `src/quack/quack_message.cpp:91`) calls `write_stream.Rewind()` on entry, resetting position to 0. The response cleanly overwrites the request bytes from offset 0. Verified by PR-1.5's CI roundtrip test passing on all platforms.

Don't be tempted to "fix" this by splitting into two streams — it's working as designed.

### 3. `Stop()` is synchronous in PR-2 (intentional)

`FlockServerState::Stop()` previously detached a destruction thread; PR-2 round-8 review found that this allowed a transient "two-servers" race where a concurrent `flock_serve` could rebind the port while the old server was still tearing down. **It's now synchronous.** This blocks the SQL caller for the duration of in-flight requests — acceptable trade-off; PR-3+ adds `Connection::Interrupt()` to bound the wait.

### 4. `Close()` drain-loops forever (intentional, with a TODO)

`FlockHttpServer::Close()` waits for `active_requests` to reach 0, with a per-attempt timeout but no global cap. A hanging query genuinely blocks shutdown until it completes. `// TODO PR-3+:` in the code references the `Connection::Interrupt()` path that will fix this. **Do not "fix" this with a hard-destroy on timeout — that re-introduces the use-after-free risk that motivated the refactor.**

### 5. Two TODOs in PR-2 code that should be addressed in PR-3 or PR-4

Both captured as `// TODO PR-3+:` comments:
- `src/flock_http_server.cpp::ListenThreadMain` swallows listen exceptions silently. Route through the `Flock` log type.
- `src/quack/quack_server.cpp::QuackHandlers::Register` uses `Access-Control-Allow-Origin: *`. Replace with the `flock_cors_origins` allow-list when cookie auth arrives in PR-4.

## Architectural decisions locked (don't re-litigate)

| Decision | Source | Rationale |
|---|---|---|
| Strategy C: vendor quack source + refactor toward target architecture | This session, round 3 with GPT-5.5 | Better feedback loops than building from scratch; quack's own tests are the regression spec. PR-1 + PR-2 validated the call. |
| Single-server-per-process | SPEC §2 | Simpler concurrency story; multi-server is YAGNI for v0.1 |
| OpenSSL via vcpkg stays | This session, after PR-1 CI green | Toolchain risk is sunk cost; PR-1 shipped clean across 5 platforms with it. Removal would now be more work than benefit. |
| Crypto in PR-4 uses OpenSSL `libcrypto` wrappers (`src/flock_crypto.{cpp,hpp}`) | This session | Eliminates ~300 LOC of vendored Brad Conte SHA-256 + hand-rolled HMAC. Also addresses CSPRNG concern (`RAND_bytes`). |
| `flock_ui_assets='proxy'` is the default; `'bundled'` is the air-gapped opt-in (post-v0.1) | This session | Proxy is simpler to implement (just `duckdb_httplib_openssl::Client`); bundled needs fetch script + embed step + version pin file |
| `flockd` wrapper binary is NOT shipped | PR-1 planning | Unwrapped `duckdb -no-stdin -init …` command is short; init script is more flexible |
| `quack_*` SQL functions/settings are kept as functional aliases of `flock_*` | SPEC §9 | Stock-quack tooling continues to work |
| `src/quack/` source is vendored (5 surgical edits documented in `docs/upstream-quack-patches.md`) | PR-1 | Future upstream rebases are bounded |
| PR-3 will (when done): vendor + adapt UI handlers, no auth changes. PR-4 will add cookie auth + crypto + login wrapper | This session, when discussing scope split | Keeps each PR's blast radius bounded |

## Implementation roadmap (in `AGENTS.md`, latest)

| PR | Status | Scope |
|---|---|---|
| PR-1 | ✓ merged (`2e032df`) | Vendor quack, rename to flock build identifiers |
| PR-1.5 | ✓ merged (`d9d12dd`) | `/quack` runtime roundtrip test + `docs/upstream-quack-patches.md` |
| PR-2 | ✓ merged (`efd130a`) | Extract httplib::Server into FlockHttpServer; SessionManager + AuthManager standalone; flock_serve/stop/wait |
| **PR-3** | **in progress on branch** | Vendor duckdb-ui source; refactor http_server.cpp → UiHandlers; OpenSSL-backed proxy mode for UI assets; **NO** cookie auth (defer to PR-4) |
| PR-4 | not started | `src/flock_crypto.{cpp,hpp}` (OpenSSL libcrypto wrappers); HMAC cookie sign/verify; `/auth/login` + `/auth/logout`; flock login wrapper at `GET /` |
| PR-5 | not started | `/sql` endpoint per SPEC §5.2-5.4 |
| PR-6 | not started | Admin handlers (`/whoami`, `/tables`, `/checkpoint`, `/sessions`, `/interrupt`) per SPEC §4 + `__FLOCK_ADMIN__:resource:action` authz |
| PR-7+ | not started | Hardening, full CI matrix expansion, golden tests, distribution |

## GPT-5.5 collaboration

We've been using a persistent multi-turn conversation via the `user-ai`
MCP tool. **Conversation ID: `duckdb-flock-spec`** (the same one used
for the original SPEC reviews — has the full project context).

| Round | Topic | Cost | Outcome |
|---|---|---|---|
| (pre-session) | 3 SPEC review rounds | $0.48 | SPEC.md ratified at v0.2 baseline |
| 1 (this session) | PR-1 design (build scaffold) | $0.13 | 7 questions resolved; "OpenSSL via vcpkg" temporarily endorsed |
| 2 | Refinements (drop proxy, drop flockd, no SQL stubs, vendor SHA-256 path) | $0.13 | PR-1 plan locked |
| 3 | Strategy A vs C decision (build clean vs fork-and-refactor) | $0.15 | Picked C-minus-renaming-risk: vendor quack, only rename build identifiers in PR-1 |
| 4 | PR-1 code review pre-CI | $0.19 | Caught 4 real bugs (constant-vector return, no SetVolatile, Name() return, sqllogictest LOAD + integer comparison) |
| 5 | PR-2 header design review | $0.21 | Caught 12 issues including: shared_ptr<FlockSession> (not optional_ptr), process-static FlockServerState::Global(), generation-counter Wait(), drain-on-close ActiveRequestGuard, GET / NOT in QuackHandlers, std::thread (not vector), concrete EvaluateAuthQuery (not template) |
| 6 | Reflection ("how clean does it look?") | $0.19 | Tightened claims (build proven ≠ runtime proven); recommended pre-PR-2 stock-quack-client roundtrip test (which became PR-1.5) |
| 7 | PR-1.5 test review | $0.19 | 4 robustness tweaks (explicit non-default port 19494, 500ms sleep, idempotent DROP TABLE, "passes unchanged" wording) |
| 8 | PR-2 implementation final review | $0.25 | Caught 3 blockers (1 real UAF in drain, 1 real race in detached destruction, 1 false alarm on MemoryStream reuse). Real ones fixed before PR-2 merge. |
| **TOTAL THIS SESSION** | | **$1.92** | 16+ real issues caught; far cheaper than the equivalent CI-debug cycles |

To resume the conversation in a new session, the `discuss` tool of
the `user-ai` MCP server takes `conversation_id: "duckdb-flock-spec"`
and the model `openai:gpt-5.5`. Past conversation context is
preserved server-side and accessible to GPT-5.5.

## Local environment notes

- **No local vcpkg or build environment is set up.** Each iteration
  requires pushing to GitHub and waiting 6-50 minutes for CI. This
  was identified as a productivity drag in PR-2 (which took 3 CI
  cycles to land green) but never addressed.
- **Recommendation for the next session:** spend ~45 minutes setting
  up vcpkg locally before starting PR-3 work in earnest. Then each
  iteration is 30s-3 min instead of CI-cycle long. Pays back across
  PR-3, PR-4, PR-5.
- macOS arm64 is the dev machine; CI runs on linux_amd64 (and
  macos/windows/wasm in the full matrix).

## Doc references (read these before changing code)

| Doc | When to read |
|---|---|
| [`SPEC.md`](./SPEC.md) | The authoritative design. Read §2 for architecture, §6 for sessions, §7 for auth, §8 for UI assets, §9 for SQL functions/settings, §11 for /info headers, §14 for roadmap. |
| [`AGENTS.md`](./AGENTS.md) | Contributor guide. Read "Implementation roadmap" + "Architecture as of PR-2" + "Critical rules" + "PR-2 acceptance checklist" before touching anything. |
| [`README.md`](./README.md) | User-facing intro. Has the implementation-status block at top warning about what works today vs what's spec'd. |
| [`docs/upstream-quack-patches.md`](./docs/upstream-quack-patches.md) | The 5 surgical edits to vendored `src/quack/quack_extension.cpp`. PR-3 will need a sibling `docs/upstream-ui-patches.md` for any edits to vendored UI source. |
| `misc/duckdb-quack/`, `misc/duckdb-ui/` | Read-only upstream reference clones. Never edit. |

## Resuming PR-3 in a new session — concrete first steps

1. `git fetch && git checkout pr3-ui-port-and-cookie-auth` — the branch already has the vendor + version.hpp patch (uncommitted).
2. Read this whole document.
3. Re-read AGENTS.md Implementation roadmap + the "Critical findings" section above.
4. (Optional but recommended) Set up local vcpkg:
   ```bash
   git clone --branch 2025.12.12 https://github.com/microsoft/vcpkg ~/vcpkg
   cd ~/vcpkg && ./bootstrap-vcpkg.sh
   export VCPKG_TOOLCHAIN_PATH=~/vcpkg/scripts/buildsystems/vcpkg.cmake
   ```
   Then `cd duckdb-flock && make release` should work locally (~30+ min first time, then incremental).
5. Start the httplib namespace migration (Critical finding #1):
   - Update `src/include/flock_http_server.hpp` and `src/flock_http_server.cpp`: change `duckdb_httplib::Server` → `duckdb_httplib_openssl::Server`. Add `#define CPPHTTPLIB_OPENSSL_SUPPORT` before the httplib include in the .cpp.
   - Update `src/quack/quack_server.{cpp,hpp}` and `src/admin_handlers.{cpp,hpp}` similarly (`Register` signatures, lambda param types).
   - Update `.github/workflows/architecture-guard.yml`'s grep pattern to detect both namespaces (or just match the new one).
   - Push as a tiny commit; verify CI green; this is a self-contained intermediate step.
6. Begin the UI handler refactor:
   - Rewrite `src/ui/http_server.cpp` into a new `UiHandlers` class (declared in `src/ui/include/ui_handlers.hpp`). Drop the upstream singleton/lifecycle logic; keep the Handle* method bodies.
   - Construct `UiHandlers` in `FlockHttpServer::RegisterBuiltinHandlers`. Order matters — UiHandlers' `GET /.*` catch-all MUST be registered LAST.
   - Reconcile `/info`: AdminHandlers should also emit `X-DuckDB-UI-Extension-Version`. UiHandlers should NOT register its own `/info`.
   - Adapt `ui_extension.cpp`'s settings + storage-extension setup into our `quack_extension.cpp::LoadInternal`. EXCLUDE `ui_extension.cpp` from CMake.
   - Don't drop `EventDispatcher` or `Watcher` — UI needs them. Move ownership onto `UiHandlers`.
7. Update `src/CMakeLists.txt` to compile the new UI sources.
8. Author `docs/upstream-ui-patches.md` (mirror the upstream-quack-patches.md format).
9. Push, watch CI, iterate.
10. After CI green: update AGENTS.md "Implementation roadmap" to mark PR-3 done, point at PR-4. Squash-merge.

## Things NOT to do

- Don't drop OpenSSL or `vcpkg.json`. We considered it; PR-1 CI green made keeping it the rational call.
- Don't try to "fix" the `MemoryStream` reuse in `quack_server.cpp::QuackHandlers::Register` (Critical finding #2).
- Don't make `FlockServerState::Stop()` async or detach destruction (Critical finding #3).
- Don't add a hard-destroy timeout to `FlockHttpServer::Close()` (Critical finding #4).
- Don't register `GET /` from UiHandlers — it's reserved for the flock login wrapper that arrives in PR-4. UI's "go to /ui/" landing page can either redirect or just not exist in PR-3.
- Don't change wire format on `/quack` for any reason. The PR-1.5 roundtrip test is the regression spec; if it fails after a PR-3 change, you broke the wrong thing.
- Don't edit upstream quack source in `src/quack/` beyond the 5 documented edits in `docs/upstream-quack-patches.md`. If you must, document the new edit in that file.

## Cumulative session metrics

- 4 PRs merged this session: PR-1, PR-1.5, docs (OpenSSL+proxy restoration), PR-2
- ~6500 LOC net into the project across those PRs
- 5/5 CI platforms green for everything that's merged
- $1.92 GPT-5.5 spend across 8 review rounds; 16+ real issues caught
- ~12 hours of session time across two days
