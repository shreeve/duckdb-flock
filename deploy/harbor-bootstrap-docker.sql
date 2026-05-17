-- Container-friendly bootstrap. Reads HARBOR_TOKEN from the env via a
-- duckdb_settings trick. For production, mount a real bootstrap with a
-- proper auth function (examples/auth/) on top of this file.

ATTACH '/var/lib/harbor/harbor.db' AS harbor;
USE harbor;

LOAD '/var/lib/harbor/harbor.duckdb_extension';

-- Read the seed token from the env. If unset, fall back to
-- harbor_serve()'s auto-generated token (printed on stdout, captured
-- by the container's logs).
SET VARIABLE harbor_seed_token = getenv('HARBOR_TOKEN');

-- harbor_serve accepts an explicit `token` parameter or auto-generates
-- one. Use the env-supplied seed if present.
CALL CASE
       WHEN getvariable('harbor_seed_token') = '' THEN
         harbor_serve('harbor:0.0.0.0:9494')
       ELSE
         harbor_serve('harbor:0.0.0.0:9494', token := getvariable('harbor_seed_token'))
     END;

CALL harbor_wait();
