-- rbac-authorization.sql
--
-- An authorization function that gates the `__HARBOR_ADMIN__:resource:action`
-- queries (admin endpoints) on per-principal RBAC. Pair this with any of
-- the bearer-only / bearer-table / bearer-with-expiry recipes.
--
-- Without this (or `harbor_allow_admin_without_authz = true`), admin
-- endpoints default-deny. With this, you grant specific principals the
-- right to call specific admin actions.
--
-- The authorization function receives:
--   - session_id     — opaque to your function (used by harbor for tracing)
--   - principal_id   — sha256(token) hex; the same identity returned by /whoami
--   - sql_text       — the SQL the caller is trying to run, OR the
--                      synthetic `__HARBOR_ADMIN__:resource:action` string
--                      for admin endpoints
-- and returns BOOLEAN: TRUE = allow, FALSE = deny.
--
-- Resource:action pairs harbor uses today (SPEC §7):
--   __HARBOR_ADMIN__:catalog:list_tables       (GET /tables)
--   __HARBOR_ADMIN__:catalog:describe_table    (GET /schema/:db/:t)
--   __HARBOR_ADMIN__:checkpoint:create         (POST /checkpoint)
--   __HARBOR_ADMIN__:server:whoami             (GET /whoami)
--   __HARBOR_ADMIN__:sessions:list             (GET /sessions)
--   __HARBOR_ADMIN__:sessions:interrupt        (POST /interrupt)
--   __HARBOR_ADMIN__:sessions:cancel           (POST /sql/cancel)
--
-- Usage:
--   .read examples/auth/rbac-authorization.sql
--   SET GLOBAL harbor_authorization_function = 'harbor_rbac_authz';
--   -- (no need to harbor_stop / harbor_serve for authz — the change
--   --  takes effect on the very next /sql or admin request)

CREATE TABLE IF NOT EXISTS harbor_principal_grants (
  principal_id VARCHAR NOT NULL,    -- sha256(token) in hex; see /whoami
  resource     VARCHAR NOT NULL,    -- e.g. 'sessions', 'catalog'
  action       VARCHAR NOT NULL,    -- e.g. 'list', 'interrupt'
  granted_at   TIMESTAMP DEFAULT current_timestamp,
  PRIMARY KEY (principal_id, resource, action)
);

-- Seed: grant a specific principal the right to list + interrupt sessions.
-- Get principal_id from `curl /whoami` after authenticating, OR compute
-- as `hex(sha256(token_bytes))`.
-- INSERT INTO harbor_principal_grants (principal_id, resource, action) VALUES
--   ('<principal-id-hex-here>', 'sessions', 'list'),
--   ('<principal-id-hex-here>', 'sessions', 'interrupt');

CREATE OR REPLACE FUNCTION harbor_rbac_authz(
  session_id   VARCHAR,
  principal_id VARCHAR,
  sql_text     VARCHAR
)
RETURNS BOOLEAN AS
  CASE
    -- Non-admin queries — let everything authenticated through. If you
    -- want per-table or row-level authz, replace this branch with a
    -- table-driven check parsing sql_text (much harder; not in v0.1
    -- scope and is generally a wrong layer for SQL ACLs anyway).
    WHEN sql_text NOT LIKE '__HARBOR_ADMIN__:%' THEN TRUE

    -- Admin queries — require explicit grant.
    ELSE EXISTS (
      SELECT 1
        FROM harbor_principal_grants g
       WHERE g.principal_id = principal_id
         AND g.resource     = split_part(sql_text, ':', 2)
         AND g.action       = split_part(sql_text, ':', 3)
    )
  END;

SET GLOBAL harbor_authorization_function = 'harbor_rbac_authz';

-- Operational queries:

-- List grants:
-- SELECT principal_id, resource, action, granted_at
--   FROM harbor_principal_grants
--  ORDER BY principal_id, resource, action;

-- Revoke a specific grant:
-- DELETE FROM harbor_principal_grants
--  WHERE principal_id = '<...>'
--    AND resource     = 'sessions'
--    AND action       = 'interrupt';

SELECT 'authz fn registered as: ' || current_setting('harbor_authorization_function') AS status;
