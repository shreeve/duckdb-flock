# Upstream `duckdb-quack` patches

harbor vendors `duckdb-quack`'s `src/` tree at branch `v1.5-variegata`
(commit `90bd70e`, fetched 2026-05). This document tracks every edit
harbor makes to vendored upstream files. When rebasing against upstream
later, expect these to conflict and need re-application.

**Last vendor pull:** 2026-05-14 against upstream commit `90bd70e`.

## Edits to `src/quack/quack_extension.cpp`

This is the only vendored file that's been modified. The file's own
header comment block also documents these edits in-place.

| # | What | Why | Conflict risk on rebase |
|---|---|---|---|
| 1 | `DUCKDB_CPP_EXTENSION_ENTRY(quack, loader)` → `DUCKDB_CPP_EXTENSION_ENTRY(harbor, loader)` | DuckDB looks up the C entry symbol by extension name. `harbor.duckdb_extension` needs `harbor_init` (etc.) symbols. | Low — at the bottom of the file in `extern "C"`, rarely changes upstream. |
| 2 | `EXT_VERSION_RPC` → `EXT_VERSION_HARBOR` (in two places: `Version()` body and `HarborVersionScalar`) | DuckDB's build system auto-defines `EXT_VERSION_<NAME_UPPERCASE>`. Upstream uses `RPC` because their Makefile sets `EXT_NAME=rpc`; we use `EXT_NAME=harbor`. | Low — token-level edits in stable functions. |
| 3 | New `HarborVersionScalar` static function and `harbor_version()` registration in `LoadInternal` | Smoke-test surface for `LOAD harbor; SELECT harbor_version();` — independent of any `quack_*` identifier. Lets `test/sql/harbor.test` verify "extension loaded and exports harbor-named symbols" without depending on upstream functions. | Medium — `LoadInternal` is the most-edited function upstream. Conflicts likely. |
| 4 | `QuackExtension::Name()` returns `"harbor"` (was `"quack"`) | The C++ Extension class identity must match the loadable extension name so DuckDB's `ExtensionManager::BeginLoad` keys correctly (otherwise `LoadStaticExtension<HarborExtension>` registers under `"quack"`). The class is still spelled `QuackExtension` to keep the diff minimal. | Low — single-line return value in a stable method. |
| 5 | `loader.SetDescription("harbor — DuckDB as an HTTP service (PR-1: vendored Quack RPC)")` (was "The DuckDB 'Quack' Client/Server Protocol") | Cosmetic; visible in `duckdb_extensions()` listing and `/info` headers later. | Low — single string literal. |

## Other vendored files — NO edits

Every other file under `src/quack/` is verbatim from upstream. If you
find a diff, that's a bug — please file an issue. To verify:

```bash
diff -ru misc/duckdb-quack/src/ src/quack/
```

Should report only `quack_extension.cpp` (the file with the five edits
above) and any new files we add (none in PR-1).

## Rebasing process

When upstream `duckdb-quack` ships a new commit on `v1.5-variegata` (or
when we want to bump the pin), follow this:

```bash
# 1. Refresh the upstream reference clone.
( cd misc/duckdb-quack && git fetch && git checkout v1.5-variegata && git pull )

# 2. Diff our vendored copy against the new upstream.
diff -ru misc/duckdb-quack/src/ src/quack/

# 3. For each non-trivial change in upstream:
#    - Inspect the diff to understand what changed and why.
#    - Apply to src/quack/ if applicable (most upstream changes
#      will be drop-in: copy the new file content over).
#    - For changes inside quack_extension.cpp specifically, re-apply
#      the five edits in this document on top of the new content.

# 4. Run the build + tests locally if possible:
make release && make test_release

# 5. Critical: verify the stock-quack-client roundtrip in
#    test/sql/harbor.test still passes. That file IS the regression
#    spec for "did our edits silently break wire format?"

# 6. Update this file's "Last vendor pull" date and upstream commit
#    hash above. Add any new edit rows to the table if needed.

# 7. Commit with a message like:
#    "rebase quack to <upstream-commit>"
```

## Architectural notes

- The `QuackExtension` / `HarborExtension` delegation pattern survived
  through v0.1. `src/harbor_extension.cpp::HarborExtension::Load`
  still constructs a `QuackExtension` delegate. A future refactor
  could collapse it (move `LoadInternal`'s body into
  `src/harbor_extension.cpp`, drop the `QuackExtension` class
  entirely), at which point edit #4 here disappears, edits #3 and #5
  move into `HarborExtension`, and edit #1's entry symbol relocates.
  Not on any v0.1+ roadmap; would just simplify the vendored-edit
  surface.
- All harbor-original SQL functions and settings (`harbor_serve`,
  `harbor_stop`, `harbor_wait`, `harbor_authentication_function`
  alias, `harbor_cors_origins`, `harbor_query_timeout_s`, etc.)
  live in harbor-original files (`src/harbor_lifecycle.{cpp,hpp}`,
  `src/quack/quack_extension.cpp::LoadInternal`'s settings block,
  etc.) — **not** as edits to vendored quack files. The
  vendored-edit list in this document is intentionally small and
  should stay that way.
- **Smell-check rule:** if the vendored-edit list grows beyond ~10
  entries, the architectural refactor isn't actually moving us off
  the vendored substrate. Treat it as a signal to refactor harbor's
  own files instead of patching upstream's.
