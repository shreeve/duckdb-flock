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

## Forward-looking notes

- **PR-2** may collapse the `QuackExtension`/`HarborExtension` delegation,
  which would change edit #4 from "rename Name() return value" to
  "delete the QuackExtension class entirely" (`HarborExtension` becomes
  the canonical class). At that point edits #3 and #5 likely move into
  the new `HarborExtension` impl in `src/harbor_extension.cpp`, and edit
  #1 either disappears (entry symbol moves to `src/harbor_extension.cpp`)
  or stays in `src/quack/quack_extension.cpp` if it survives.
- **PR-3+** add new harbor-named SQL functions/settings (`harbor_serve`,
  `harbor_stop`, `harbor_wait`, `harbor_authentication_function` alias,
  etc.). Those land in harbor-original files (`src/harbor_lifecycle.cpp`
  etc.), **not** as edits to vendored quack files. The vendored-edit
  list in this document should stay short.
- If the vendored-edit list grows beyond ~10 entries, that's a signal
  that the architectural refactor isn't actually moving us off the
  vendored substrate. Treat it as a smell.
