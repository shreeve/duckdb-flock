-- Bootstrap SQL fed to duckdb at systemd / docker entrypoint time.
-- See deploy/harbor.service for how this is invoked.
--
-- Edit each marked section before enabling in production. Specifically:
--   1. The `LOAD` path must point to your platform's harbor binary.
--   2. The auth function MUST be set to a non-default value.
--   3. The CORS allow-list MUST be set if browsers will hit /sql or /info.
--   4. The bind address determines whether `/localToken` and the
--      `harbor_local_dev_mode` bypass are even available.

-- 1. Database file. Use a real on-disk database for state; switch to
--    `:memory:` only for ephemeral test deployments.
ATTACH '/var/lib/harbor/harbor.db' AS harbor;
USE harbor;

-- 2. Load the extension. Adjust path for your platform.
LOAD '/var/lib/harbor/harbor.duckdb_extension';

-- 3. Auth function. Pick one of the recipes from examples/auth/ —
--    none of them are loaded by default, so without this step harbor
--    accepts any token harbor_serve generated which is not what you
--    want in production.
.read /var/lib/harbor/auth/bearer-table-multi-tenant.sql

-- 4. CORS allow-list. Replace with your real frontend origin(s).
--    Multiple origins are semicolon-separated. `harbor_serve` REFUSES
--    to start if this is `'*'` — that's intentional.
SET GLOBAL harbor_cors_origins = 'https://app.example.com';

-- 5. Query timeout (seconds). 0 = no timeout. 30 is a sane default
--    for OLTP-ish workloads; bump for analytics.
SET GLOBAL harbor_query_timeout_s = 30;

-- 6. Cookie TTL (seconds). 12h matches the default; increase for
--    long-running browser sessions, decrease for sensitive deployments.
SET GLOBAL harbor_auth_cookie_ttl_s = 43200;

-- 7. Admin endpoint default-deny. Uncomment ONLY if you are certain
--    you want every authenticated principal to reach admin endpoints
--    without an authz function. Otherwise: load
--    examples/auth/rbac-authorization.sql first.
-- SET GLOBAL harbor_allow_admin_without_authz = TRUE;

-- 8. Start the server. The bind host MUST be 127.0.0.1 if a reverse
--    proxy on the same host fronts harbor (recommended). Bind on
--    0.0.0.0 ONLY if you've verified your firewall + reverse proxy +
--    auth setup are correct.
CALL harbor_serve('harbor:127.0.0.1:9494');

-- 9. Block the duckdb process so systemd's `Type=simple` keeps it
--    running. Without this, duckdb would exit and systemd would
--    immediately restart the unit. harbor_wait returns when harbor
--    receives SIGTERM (which is what systemd sends on `systemctl stop`).
CALL harbor_wait();
