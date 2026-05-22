-- bearer-with-expiry.sql
--
-- Tokens carry a `valid_until` timestamp. Expired tokens are rejected
-- without manual revocation. Useful for short-lived API keys (CI/CD
-- jobs that should self-revoke after their run-window) and for
-- enforcing rotation hygiene.
--
-- This builds on bearer-table-multi-tenant.sql; you can adopt either
-- this OR that one — not both at the same time.
--
-- Threat model: same as the multi-tenant table, plus automatic
-- revocation when the wall-clock crosses `valid_until`.
--
-- Usage:
--   .read examples/auth/bearer-with-expiry.sql
--   SET GLOBAL harbor_authentication_function = 'harbor_expiring_authn';
--   CALL harbor_stop();
--   CALL harbor_serve(bind := '127.0.0.1', port := 9494);

CREATE TABLE IF NOT EXISTS harbor_principals_expiring (
  principal_label VARCHAR NOT NULL,
  token_value     VARCHAR PRIMARY KEY,
  valid_until     TIMESTAMP NOT NULL,        -- wall-clock UTC
  active          BOOLEAN NOT NULL DEFAULT TRUE,
  created_at      TIMESTAMP DEFAULT current_timestamp,
  notes           VARCHAR
);

-- Seed examples. valid_until is wall-clock UTC; use INTERVAL math
-- to stamp out timestamps relative to "now".
INSERT INTO harbor_principals_expiring
       (principal_label, token_value,                              valid_until, notes)
VALUES
  ('ci-token',  'EXAMPLE-ci-replace-me-32-hex-bytes',
                current_timestamp + INTERVAL '7 days',  'rotates weekly'),
  ('admin-tmp', 'EXAMPLE-admin-replace-me-32-hex-bytes',
                current_timestamp + INTERVAL '1 hour',  'incident-response window')
ON CONFLICT (token_value) DO NOTHING;

CREATE OR REPLACE FUNCTION harbor_expiring_authn(session_id VARCHAR, token VARCHAR)
RETURNS BOOLEAN AS
  EXISTS (
    SELECT 1
      FROM harbor_principals_expiring
     WHERE token_value = token
       AND active      = TRUE
       AND valid_until > current_timestamp
  );

SET GLOBAL harbor_authentication_function = 'harbor_expiring_authn';

-- Find tokens expiring soon — schedule this as a daily job and notify:
-- SELECT principal_label, valid_until,
--        valid_until - current_timestamp AS time_left
--   FROM harbor_principals_expiring
--  WHERE active
--    AND valid_until BETWEEN current_timestamp
--                        AND current_timestamp + INTERVAL '3 days'
--  ORDER BY valid_until;

SELECT 'auth fn registered as: ' || current_setting('harbor_authentication_function') AS status;
