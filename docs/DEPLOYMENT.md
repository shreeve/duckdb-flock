# Deploying harbor

This guide walks you through deploying harbor v0.1.0 from a local
laptop smoke test through to a production-style daemon under systemd
or a container under Docker / Incus / Kubernetes.

> **Path summary:**
> 1. Local laptop (~5 min) — confirm the binary loads and answers requests
> 2. Daemonize (~15 min) — pick systemd OR Docker, start harbor at boot
> 3. Harden (~30 min) — auth function, CORS allow-list, query timeout
> 4. Validate (~5 min) — run `scripts/validate-deployment.sh` end-to-end
> 5. Load test (~5 min) — `scripts/load-test.sh` to confirm the session pool

---

## 1 — Local laptop quickstart

Pick the right binary for your machine from the
[v0.1.0 release page](https://github.com/shreeve/duckdb-harbor/releases/tag/v0.1.0):

| Your laptop | Download |
|---|---|
| macOS Apple Silicon (M-series) | `harbor.osx_arm64.duckdb_extension` |
| macOS Intel | `harbor.osx_amd64.duckdb_extension` |
| Linux x86_64 | `harbor.linux_amd64.duckdb_extension` |
| Linux ARM64 | `harbor.linux_arm64.duckdb_extension` |
| Windows x86_64 (PowerShell) | `harbor.windows_amd64.duckdb_extension` |

> If you already have `git clone --recurse-submodules`-ed the repo and
> ran `make release`, your local binary is at
> `build/release/extension/harbor/harbor.duckdb_extension` — same thing,
> just locally built.

Make sure DuckDB v1.5.2 is installed (any newer 1.5.x works for the
`duckdb-quack v1.5-variegata` line; v2.x will need a rebuild
post-rebase).

### Smoke run

```bash
# wherever your binary lives:
EXT=$HOME/Downloads/harbor.osx_arm64.duckdb_extension

duckdb -unsigned <<'SQL'
LOAD '/Users/me/Downloads/harbor.osx_arm64.duckdb_extension';
CALL harbor_serve('harbor:127.0.0.1:9494');
CALL harbor_wait();
SQL
```

`-unsigned` is required because v0.1.0 binaries aren't yet signed by
DuckDB's community-extensions signing key (PR
[duckdb/community-extensions#1917](https://github.com/duckdb/community-extensions/pull/1917)
is the gate). After that PR merges and you `INSTALL harbor FROM
community;` you can drop the flag.

`harbor_serve` prints the auto-generated bearer token. Copy it. From
another terminal:

```bash
TOKEN="paste-the-token-here"

curl http://127.0.0.1:9494/health
# {"ok":true,"version":"...","uptime_s":2}

curl -H "Authorization: Bearer $TOKEN" \
     -X POST http://127.0.0.1:9494/sql \
     -H 'Content-Type: application/json' \
     -d '{"sql":"SELECT 42 AS the_answer"}'
# {"ok":true,"kind":"select","columns":[...],"data":[[42]],"rowCount":1,"timeMs":0}
```

Point your browser at <http://127.0.0.1:9494/> — paste the token in the
login page, hit Sign in, and the official DuckDB UI proxies through.

When you're done, `Ctrl+C` the duckdb process (sends SIGTERM, harbor
cleans up, server stops).

---

## 2 — Daemonize

Pick the path that matches where harbor will live:

### 2a. systemd (bare-metal Linux, Incus system container)

```bash
# Create a non-root user and a state directory.
sudo useradd --system --create-home --home-dir /var/lib/harbor harbor

# Drop the binary, the bootstrap SQL, and any auth recipes.
sudo cp harbor.linux_amd64.duckdb_extension /var/lib/harbor/harbor.duckdb_extension
sudo cp deploy/harbor-bootstrap.sql        /var/lib/harbor/harbor-bootstrap.sql
sudo cp -r examples/auth                   /var/lib/harbor/auth
sudo chown -R harbor:harbor /var/lib/harbor

# Edit the bootstrap SQL — auth fn, CORS, timeout, etc.
sudo -u harbor "$EDITOR" /var/lib/harbor/harbor-bootstrap.sql

# Install the unit.
sudo cp deploy/harbor.service /etc/systemd/system/harbor.service
sudo systemctl daemon-reload
sudo systemctl enable --now harbor

sudo journalctl -u harbor -f       # follow the log; first start prints the token
```

The unit is in [`deploy/harbor.service`](../deploy/harbor.service) and
the bootstrap SQL is in
[`deploy/harbor-bootstrap.sql`](../deploy/harbor-bootstrap.sql) — both
are heavily commented.

### 2b. Docker / Incus OCI container

```bash
cd deploy/

# Edit docker-compose.yml — set HARBOR_TOKEN to a real secret.
$EDITOR docker-compose.yml

# Build + run.
docker compose up --build -d

# Logs.
docker compose logs -f harbor

# Stop.
docker compose down
```

The image entry-point reads `HARBOR_TOKEN` from the environment and
seeds it into `harbor_serve`. State persists in the named
`harbor-data` volume.

For Incus, the same `docker compose` workflow works inside an Incus
OCI container. If you prefer an Incus system container with systemd,
follow path 2a inside that container.

---

## 3 — Harden

`harbor_serve` works out of the box for development but **does not
ship with production-safe defaults**. The four levers that matter
before any non-throwaway deployment.

> ## ⚠️  Threat-model reality check (read this first)
>
> **A bearer-authenticated `/sql` caller is effectively a DuckDB
> superuser** unless you configure `harbor_authorization_function`
> to constrain the SQL they can run. The default permissive
> auth that ships with `harbor_serve` does NOT protect against:
>
> - An authenticated principal running `SET GLOBAL harbor_allow_admin_without_authz = TRUE`
>   through `/sql` to unlock every admin endpoint
> - An authenticated principal running `ATTACH '/path/to/anything'`,
>   `LOAD 'arbitrary.duckdb_extension'`, `COPY (...) TO 'file://...'`,
>   or any other SQL DuckDB itself permits
> - An authenticated principal reading or modifying any table
>
> **The default-deny toggle on admin endpoints
> (`harbor_allow_admin_without_authz`) is a usability convenience for
> operators who haven't yet wired up authorization — NOT a security
> wall.** A bearer that has `/sql` access can flip the flag itself
> by running SQL.
>
> **For any non-throwaway deployment** (anything reachable by people
> you don't 100% trust at root level), do BOTH of:
>
> 1. **Configure a per-principal authentication callback** (§3a) so
>    different principals have different tokens, not one shared
>    superuser secret.
> 2. **Configure an authorization callback** (§3d) that gates BOTH
>    admin endpoints AND dangerous SQL — `SET GLOBAL`, `ATTACH`,
>    `LOAD`, `INSTALL`, `COPY (...) TO 'file://...'` — by principal.
>
> See [SPEC.md §7 "Threat model"](../SPEC.md#threat-model) for the
> full architectural treatment of what harbor protects against and
> what it explicitly leaves to the operator. See
> [`examples/auth/`](../examples/auth/) for copy-paste-ready
> recipes that cover the common patterns.

### 3a. Authentication function

The default permissive auth accepts anything. Pick a recipe from
[`examples/auth/`](../examples/auth/):

- [`bearer-only-static.sql`](../examples/auth/bearer-only-static.sql) —
  single shared token from a row.
- [`bearer-table-multi-tenant.sql`](../examples/auth/bearer-table-multi-tenant.sql) —
  one row per principal; revoke by `UPDATE active = FALSE`.
- [`bearer-with-expiry.sql`](../examples/auth/bearer-with-expiry.sql) —
  tokens carry a `valid_until` timestamp.

Load via `.read examples/auth/<file>.sql` in your bootstrap, then
`SET GLOBAL harbor_authentication_function = '<fn-name>';`.

### 3b. CORS allow-list

If browsers will hit `/sql` or `/info` from a different origin than
where harbor itself lives:

```sql
SET GLOBAL harbor_cors_origins = 'https://app.example.com';
-- multiple: 'https://a.example.com;https://b.example.com'
```

`'*'` is **explicitly rejected** by `harbor_serve` — a wildcard CORS
on a credential-bearing endpoint is unsafe. Use the exact-match
allow-list.

### 3c. Query timeout

Caps individual query wall-clock. Catches runaway queries and frees
the session for the next caller:

```sql
SET GLOBAL harbor_query_timeout_s = 30;     -- 0 = no timeout
```

Timed-out queries get HTTP 504 + `errorCode: "QUERY_TIMEOUT"` (or a
mid-stream `{"type":"error","code":"QUERY_TIMEOUT"}` line on
streaming `/sql`).

### 3d. Admin endpoint authorization

Admin endpoints (`/tables`, `/schema/:db/:t`, `/checkpoint`,
`/sessions`, `/interrupt`) **default-deny** when no custom authz
function is set. Either:

- Load
  [`examples/auth/rbac-authorization.sql`](../examples/auth/rbac-authorization.sql)
  and grant specific principals via the `harbor_principal_grants`
  table; OR
- Set `SET GLOBAL harbor_allow_admin_without_authz = TRUE` to allow
  every authenticated principal (operator opt-in for trusted
  deployments only).

### 3e. Reverse proxy + TLS

Bind harbor on `127.0.0.1` and put `nginx` / `caddy` / `traefik` in
front for TLS termination. Minimal nginx snippet:

```nginx
location / {
    proxy_pass         http://127.0.0.1:9494;
    proxy_http_version 1.1;
    proxy_set_header   Host              $host;
    proxy_set_header   X-Real-IP         $remote_addr;
    proxy_set_header   X-Forwarded-For   $proxy_add_x_forwarded_for;
    proxy_set_header   X-Forwarded-Proto $scheme;     # required for Secure cookies
    proxy_buffering    off;                            # NDJSON streaming
    chunked_transfer_encoding on;                      # NDJSON streaming
}
```

The `X-Forwarded-Proto: https` header is what triggers harbor's
`Secure` flag on the `harbor_session` cookie. `proxy_buffering off`
matters because `/sql`'s NDJSON streaming wants chunks delivered as
DuckDB produces them, not buffered into a single response.

---

## 4 — Validate

Once harbor is running (locally, in a container, behind a reverse
proxy — same script regardless), run the deployment validation:

```bash
scripts/validate-deployment.sh http://127.0.0.1:9494 "$TOKEN"

# Or with a CORS allow-list test:
scripts/validate-deployment.sh https://harbor.example.com "$TOKEN" \
    --origin https://app.example.com
```

The script runs ~30 HTTP-level assertions and exits non-zero on any
failure. Coverage:

- Liveness + identity (`/health`, `/info`)
- `/sql` happy paths (one-shot JSON, NDJSON streaming)
- Auth invariants (bad bearer, non-Bearer scheme, no auth, all → 401)
- Request validation (multi-statement rejection, missing fields)
- Mode A and Mode B params (incl. nested `LIST<INTEGER>`,
  `STRUCT(...)`, `DECIMAL`)
- Login page CSP + nonce
- Cookie roundtrip (login → cookie → `/sql` with cookie → logout)
- CORS allow-list (with `--origin`)
- `/quack` route registered
- Admin endpoint default-deny vs success

If any line fails, the script prints which assertion and what it got;
fix the deployment and re-run.

---

## 5 — Load test

Once validation is green, sanity-check the session pool under
concurrency:

```bash
# Default: 50 connections, 5 seconds sustained load
scripts/load-test.sh http://127.0.0.1:9494 "$TOKEN"

# Heavier — 200 connections, 30 seconds
scripts/load-test.sh http://127.0.0.1:9494 "$TOKEN" 200 30
```

The script auto-detects which benchmarker is on your `$PATH`, in
this preference order:

1. **`oha`** (Rust; `brew install oha` on macOS, `cargo install oha`
   anywhere) — modern, accurate, recommended.
2. **`wrk`** (C; `apt-get install wrk` on Debian/Ubuntu, `brew install
   wrk` on macOS) — classic, very fast.
3. **Pure-shell fallback** — only when neither tool is installed.
   Loops `curl` per request. The numbers will be DRAMATICALLY lower
   than reality because each `curl` invocation is a fresh process +
   TCP handshake; the shell harness, not harbor, is the bottleneck.
   The script prints a loud banner reminding you of this.

Output reports throughput, error rate, and p95 latency. Pass/fail
thresholds (applied to whichever mode runs):

- Error rate must be ≤ 0.5%.
- p95 of `SELECT 42` should be < 1 s in any reasonable deployment
  (warning, not failure — reverse-proxy and container overhead can
  push this legitimately).

If the error rate is non-zero in tool mode, dig into the
non-2xx codes. The two interesting ones are 409 `SESSION_BUSY`
(your client is reusing a single `sessionId` concurrently — design
your client correctly) and 504 `QUERY_TIMEOUT` (you're hitting the
`harbor_query_timeout_s` ceiling — adjust query or timeout).

> **Sample numbers (oha against a local laptop harbor)**: with `oha`
> on a M-series Mac running harbor on localhost, expect SELECT-42
> throughput in the **5,000–30,000 req/s** range with sub-millisecond
> p99. The pure-shell fallback typically reports 200–600 req/s on the
> same hardware due to per-curl process overhead — that's a measurement
> artifact, not a harbor limit.

---

## Common gotchas

- **`SET GLOBAL` not `SET`** for any auth-related setting. Auth runs
  in transient connections that don't see session-local settings.
  This is the most-common silent misconfiguration.
- **`-unsigned` flag on `duckdb`** until the community-extensions PR
  merges. Without it, DuckDB refuses to load community-built extensions
  by default.
- **`CALL harbor_wait();`** at the end of every non-interactive
  invocation. Without it, `duckdb -c "..."` exits as soon as the last
  statement returns and tears down the harbor server.
- **Cookie signing key is ephemeral per process** by design (SPEC §7).
  Restarting harbor logs out every browser session. Bearer tokens +
  the auth function table-row data survive — only cookies don't.
- **`/localToken` and `harbor_local_dev_mode` only work on
  loopback bind**. If you bind on `0.0.0.0`, both are forced off as a
  safety measure.
- **`harbor_serve` is single-server-per-process.** A second call before
  `harbor_stop` throws. Don't try to host two harbors from one duckdb
  CLI invocation.
- **Browser-origin requests do NOT bypass auth.** Origin is a CSRF
  defense, not authentication. SPEC §7.

---

## What v0.1 explicitly does NOT do

- **No bundled UI assets.** v0.1 proxies the catch-all `GET /.*` to
  `ui.duckdb.org` over HTTPS. Air-gapped deployments must wait for
  v0.2's bundled mode (or front harbor with their own static UI host).
- **No multi-server deployment.** harbor is single-instance per
  DuckDB process. Replication / clustering is post-v0.1.
- **No operator-controlled cookie signing key.** Cookie key is
  ephemeral per process; v0.2 will read `HARBOR_COOKIE_SIGNING_KEY`
  from the env so cookies survive process restart.
- **No spatial GEOMETRY type encoding** in `/sql` responses. Requires
  the spatial extension as a build dep; tracked for post-v0.1.

For everything else — the protocol surface in [`SPEC.md`](../SPEC.md)
is the contract. Any divergence from it is a bug.
