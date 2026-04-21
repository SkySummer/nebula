BEGIN;

CREATE SEQUENCE IF NOT EXISTS users_user_id_seq
    START WITH 1
    INCREMENT BY 1
    MINVALUE 1;

CREATE TABLE IF NOT EXISTS users (
    user_id BIGINT PRIMARY KEY DEFAULT nextval('users_user_id_seq'),
    username TEXT NOT NULL UNIQUE,
    password_hash_algorithm TEXT NOT NULL,
    password_hash_iterations BIGINT NOT NULL,
    password_hash_salt TEXT NOT NULL,
    password_hash_derived_key TEXT NOT NULL,
    created_at_s BIGINT NOT NULL
);

ALTER TABLE users
    ALTER COLUMN user_id
    SET DEFAULT nextval('users_user_id_seq');

CREATE TABLE IF NOT EXISTS storage_objects (
    sha256 TEXT PRIMARY KEY,
    size_bytes BIGINT NOT NULL,
    object_rel_path TEXT NOT NULL,
    ref_count BIGINT NOT NULL,
    created_at_s BIGINT NOT NULL,
    updated_at_s BIGINT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_storage_objects_ref_count
    ON storage_objects (ref_count);

CREATE TABLE IF NOT EXISTS storage_nodes (
    path TEXT PRIMARY KEY,
    node_type TEXT NOT NULL DEFAULT 'file',
    sha256 TEXT REFERENCES storage_objects (sha256),
    size_bytes BIGINT,
    updated_at_s BIGINT NOT NULL,
    CONSTRAINT storage_nodes_node_type_check CHECK (node_type IN ('file', 'directory')),
    CONSTRAINT storage_nodes_type_fields_check CHECK (
        (node_type = 'file' AND sha256 IS NOT NULL AND size_bytes IS NOT NULL)
        OR (node_type = 'directory' AND sha256 IS NULL AND size_bytes IS NULL)
    )
);

CREATE INDEX IF NOT EXISTS idx_storage_nodes_sha256
    ON storage_nodes (sha256);

CREATE TABLE IF NOT EXISTS storage_upload_sessions (
    upload_id TEXT PRIMARY KEY,
    path TEXT NOT NULL,
    temp_rel_path TEXT NOT NULL,
    total_chunks BIGINT NOT NULL,
    next_chunk_index BIGINT NOT NULL,
    temp_size_bytes BIGINT NOT NULL DEFAULT 0,
    created_at_s BIGINT NOT NULL,
    updated_at_s BIGINT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_storage_upload_sessions_updated_at_s
    ON storage_upload_sessions (updated_at_s);

COMMIT;
