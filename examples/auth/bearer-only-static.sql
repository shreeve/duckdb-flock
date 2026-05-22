-- bearer-only-static.sql
--
-- Single shared bearer token loaded from a settings table. The simplest
-- step up from "use the token harbor_serve printed once on startup":
-- the token is now stored in a row, can be rotated without restarting
-- harbor, and is unaffected by harbor_serve regenerating an ephemeral
-- token on each restart.
--
-- Threat model: any client with the token has full access. Use this
-- when you have exactly one trusted caller (e.g. an internal service
-- account) and want to avoid the bigger schema of multi-tenant tokens.
--
-- Usage:
--   .read examples/auth/bearer-only-static.sql
--   -- (then update the token below or via UPDATE):
--   SET GLOBAL harbor_authentication_function = 'harbor_static_authn';
--   CALL harbor_stop();
--   CALL harbor_serve(bind := '127.0.0.1', port := 9494);
--
-- Rotating the token (no restart needed for the table; restart harbor
-- only if you also want the in-flight cookies invalidated):
--   UPDATE harbor_secrets SET token_value = 'new-secret-here'
--    WHERE name = 'shared';

CREATE TABLE IF NOT EXISTS harbor_secrets (
  name        VARCHAR PRIMARY KEY,
  token_value VARCHAR NOT NULL,
  created_at  TIMESTAMP DEFAULT current_timestamp
);

-- Replace this seed value before going live. Generate with:
--   openssl rand -hex 32
INSERT INTO harbor_secrets (name, token_value) VALUES
  ('shared', 'CHANGE-ME-BEFORE-PRODUCTION-USE')
ON CONFLICT (name) DO NOTHING;

CREATE OR REPLACE FUNCTION harbor_static_authn(session_id VARCHAR, token VARCHAR)
RETURNS BOOLEAN AS
  EXISTS (
    SELECT 1
      FROM harbor_secrets
     WHERE name = 'shared'
       AND token_value = token
  );

-- After running this file:
SET GLOBAL harbor_authentication_function = 'harbor_static_authn';

-- Sanity check:
SELECT 'auth fn registered as: ' || current_setting('harbor_authentication_function') AS status;
