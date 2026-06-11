-- Upgrade from 0.5.1 to 0.6.0

-- Verify pg_textsearch library is loaded
DO $$
BEGIN
    IF pg_catalog.current_setting('pg_textsearch.library_version', true)
       IS NULL
    THEN
        RAISE EXCEPTION
            'pg_textsearch library not loaded. '
            'Add pg_textsearch to shared_preload_libraries and restart.';
    END IF;
END $$;

-- New function: force-merge all segments into one
CREATE FUNCTION bm25_force_merge(index_name text)
RETURNS void
AS 'MODULE_PATHNAME', 'tp_force_merge'
LANGUAGE C VOLATILE STRICT;

DO $$
BEGIN
    RAISE INFO 'pg_textsearch: upgraded to v0.6.0';
END
$$;
