-- bearer-table-multi-tenant.sql
--
-- Token-per-tenant table. Each row represents one principal (a person
-- or service account); each token is unique. The auth function returns
-- TRUE iff the supplied token matches an active row.
--
-- harbor's principal_id is hex(sha256(token)) per SPEC §6, so you DO
-- NOT need to track principal IDs in this table — they're derived from
-- the token automatically. The `principal_label` column is just a
-- human-readable handle for logs and admin UIs.
--
-- Threat model: token compromise grants the compromised tenant's
-- access only. Revocation is `UPDATE ... SET active = FALSE`. To
-- rotate a token, INSERT a new row and let the old one expire or
-- deactivate.
--
-- Usage:
--   .read examples/auth/bearer-table-multi-tenant.sql
--   SET GLOBAL harbor_authentication_function = 'harbor_table_authn';
--   CALL harbor_stop();
--   CALL harbor_serve(bind := '127.0.0.1', port := 9494);

CREATE TABLE IF NOT EXISTS harbor_principals (
  principal_label VARCHAR NOT NULL,        -- 'alice', 'svc-etl', 'tenant-acme'
  token_value     VARCHAR PRIMARY KEY,     -- the bearer string
  active          BOOLEAN NOT NULL DEFAULT TRUE,
  created_at      TIMESTAMP DEFAULT current_timestamp,
  notes           VARCHAR
);

-- Seed examples — replace with real values via INSERT/UPDATE.
-- Generate tokens with:  openssl rand -hex 32
INSERT INTO harbor_principals (principal_label, token_value, notes) VALUES
  ('alice',          'EXAMPLE-alice-replace-me-32-hex-bytes', 'human user'),
  ('svc-etl',        'EXAMPLE-svc-replace-me-32-hex-bytes',   'nightly ETL job'),
  ('tenant-acme',    'EXAMPLE-acme-replace-me-32-hex-bytes',  'customer Acme Corp')
ON CONFLICT (token_value) DO NOTHING;

CREATE OR REPLACE FUNCTION harbor_table_authn(session_id VARCHAR, token VARCHAR)
RETURNS BOOLEAN AS
  EXISTS (
    SELECT 1
      FROM harbor_principals
     WHERE token_value = token
       AND active      = TRUE
  );

SET GLOBAL harbor_authentication_function = 'harbor_table_authn';

-- Operational queries:

-- List active principals (and their derived principal_id which is what
-- shows up in /whoami and logs):
-- SELECT principal_label,
--        substring(sha256(token_value::BLOB)::VARCHAR FROM 3 FOR 16) AS principal_id_prefix,
--        created_at
--   FROM harbor_principals
--  WHERE active;

-- Revoke a principal:
-- UPDATE harbor_principals SET active = FALSE WHERE principal_label = 'alice';

-- Sanity check:
SELECT 'auth fn registered as: ' || current_setting('harbor_authentication_function') AS status;
