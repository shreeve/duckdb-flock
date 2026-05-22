# `deploy/` — harbor deployment artifacts

Ready-to-use templates for the two common deployment shapes. Both
seed harbor's auth function, CORS allow-list, and query timeout from
`harbor-bootstrap.sql` (or `harbor-bootstrap-docker.sql` for the
container path).

## systemd

`harbor.service` — for bare-metal Linux or an Incus system container.

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

# Follow the log; first start prints the token.
sudo journalctl -u harbor -f
```

The unit file (`harbor.service`) and the bootstrap SQL
(`harbor-bootstrap.sql`) are heavily commented in-place.

## Docker / Incus OCI

`Dockerfile` + `docker-compose.yml` — minimal container image. Reads
`HARBOR_TOKEN` from the environment, persists state in a named volume.

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

For Incus, the same `docker compose` workflow works inside an Incus
OCI container. If you prefer an Incus system container with systemd,
use the systemd path inside that container.

## Validation + load test

After either path is up:

```bash
scripts/validate-deployment.sh http://127.0.0.1:9494 "$TOKEN"
```

Runs ~30 HTTP-level assertions (liveness, `/sql` happy paths, auth
invariants, CORS allow-list, admin endpoint gating). Exits non-zero
on any failure.

```bash
scripts/load-test.sh http://127.0.0.1:9494 "$TOKEN"
```

Auto-detects [`oha`](https://github.com/hatoo/oha) or
[`wrk`](https://github.com/wg/wrk) for accurate throughput / latency
measurement. Falls back to a `curl` loop with a clear "this measures
shell overhead, not harbor" warning if neither is installed.
