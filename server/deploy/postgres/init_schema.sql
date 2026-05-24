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
    role TEXT NOT NULL DEFAULT 'user',
    status TEXT NOT NULL DEFAULT 'active',
    created_at_s BIGINT NOT NULL,
    token_version BIGINT NOT NULL DEFAULT 1,
    quota_bytes BIGINT NOT NULL DEFAULT 21474836480,
    CONSTRAINT users_username_format_check CHECK (
        char_length(username) BETWEEN 3 AND 32
        AND username ~ '^[A-Za-z0-9_]+$'
    ),
    CONSTRAINT users_password_hash_algorithm_check CHECK (password_hash_algorithm IN ('pbkdf2_sha256')),
    CONSTRAINT users_password_hash_iterations_range_check CHECK (
        password_hash_iterations BETWEEN 10000 AND 2000000
    ),
    CONSTRAINT users_password_hash_salt_format_check CHECK (
        char_length(password_hash_salt) BETWEEN 22 AND 86
        AND (char_length(password_hash_salt) % 4) <> 1
        AND password_hash_salt ~ '^[A-Za-z0-9_-]+$'
    ),
    CONSTRAINT users_password_hash_derived_key_format_check CHECK (
        char_length(password_hash_derived_key) BETWEEN 43 AND 171
        AND (char_length(password_hash_derived_key) % 4) <> 1
        AND password_hash_derived_key ~ '^[A-Za-z0-9_-]+$'
    ),
    CONSTRAINT users_role_check CHECK (role IN ('owner', 'admin', 'user')),
    CONSTRAINT users_status_check CHECK (status IN ('active', 'disabled')),
    CONSTRAINT users_created_at_s_positive_check CHECK (created_at_s > 0),
    CONSTRAINT users_token_version_positive_check CHECK (token_version > 0),
    CONSTRAINT users_quota_bytes_positive_check CHECK (quota_bytes > 0)
);

CREATE TABLE IF NOT EXISTS storage_objects (
    sha256 TEXT PRIMARY KEY,
    size_bytes BIGINT NOT NULL,
    object_rel_path TEXT NOT NULL,
    ref_count BIGINT NOT NULL,
    created_at_s BIGINT NOT NULL,
    updated_at_s BIGINT NOT NULL,
    CONSTRAINT storage_objects_sha256_format_check CHECK (sha256 ~ '^[0-9a-f]{64}$'),
    CONSTRAINT storage_objects_size_bytes_nonnegative_check CHECK (size_bytes >= 0),
    CONSTRAINT storage_objects_object_rel_path_check CHECK (
        object_rel_path = 'objects/' || substr(sha256, 1, 2) || '/' || substr(sha256, 3, 2) || '/' || sha256
    ),
    CONSTRAINT storage_objects_ref_count_nonnegative_check CHECK (ref_count >= 0),
    CONSTRAINT storage_objects_created_at_s_positive_check CHECK (created_at_s > 0),
    CONSTRAINT storage_objects_updated_at_s_order_check CHECK (updated_at_s >= created_at_s)
);

CREATE INDEX IF NOT EXISTS idx_storage_objects_ref_count
    ON storage_objects (ref_count);

CREATE TABLE IF NOT EXISTS storage_nodes (
    path TEXT PRIMARY KEY,
    node_type TEXT NOT NULL DEFAULT 'file',
    sha256 TEXT REFERENCES storage_objects (sha256),
    size_bytes BIGINT,
    updated_at_s BIGINT NOT NULL,
    CONSTRAINT storage_nodes_path_format_check CHECK (
        path <> ''
        AND path LIKE '/users/%'
        AND path !~ '/$'
        AND path !~ '//'
        AND path !~ '(^|/)\\.\\.?(/|$)'
        AND path !~ '[[:cntrl:]]'
    ),
    CONSTRAINT storage_nodes_node_type_check CHECK (node_type IN ('file', 'directory')),
    CONSTRAINT storage_nodes_sha256_format_check CHECK (
        sha256 IS NULL OR sha256 ~ '^[0-9a-f]{64}$'
    ),
    CONSTRAINT storage_nodes_size_bytes_nonnegative_check CHECK (
        size_bytes IS NULL OR size_bytes >= 0
    ),
    CONSTRAINT storage_nodes_updated_at_s_positive_check CHECK (updated_at_s > 0),
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
    updated_at_s BIGINT NOT NULL,
    CONSTRAINT storage_upload_sessions_path_format_check CHECK (
        path <> ''
        AND path LIKE '/users/%'
        AND path !~ '/$'
        AND path !~ '//'
        AND path !~ '(^|/)\\.\\.?(/|$)'
        AND path !~ '[[:cntrl:]]'
    ),
    CONSTRAINT storage_upload_sessions_temp_rel_path_check CHECK (
        temp_rel_path <> ''
        AND temp_rel_path LIKE 'temp/%'
        AND temp_rel_path LIKE '%.part'
        AND temp_rel_path !~ '//'
        AND temp_rel_path !~ '(^|/)\\.\\.?(/|$)'
        AND temp_rel_path !~ '[[:cntrl:]]'
    ),
    CONSTRAINT storage_upload_sessions_total_chunks_positive_check CHECK (total_chunks > 0),
    CONSTRAINT storage_upload_sessions_next_chunk_index_range_check CHECK (
        next_chunk_index >= 0
        AND next_chunk_index <= total_chunks
    ),
    CONSTRAINT storage_upload_sessions_temp_size_bytes_nonnegative_check CHECK (temp_size_bytes >= 0),
    CONSTRAINT storage_upload_sessions_created_at_s_positive_check CHECK (created_at_s > 0),
    CONSTRAINT storage_upload_sessions_updated_at_s_order_check CHECK (updated_at_s >= created_at_s)
);

CREATE INDEX IF NOT EXISTS idx_storage_upload_sessions_updated_at_s
    ON storage_upload_sessions (updated_at_s);

CREATE TABLE IF NOT EXISTS storage_download_tickets (
    ticket TEXT PRIMARY KEY,
    user_id BIGINT NOT NULL REFERENCES users (user_id),
    canonical_path TEXT NOT NULL,
    created_at_s BIGINT NOT NULL,
    expires_at_s BIGINT NOT NULL,
    CONSTRAINT storage_download_tickets_ticket_format_check CHECK (
        ticket ~ '^[0-9a-f]{32}$'
    ),
    CONSTRAINT storage_download_tickets_user_id_positive_check CHECK (user_id > 0),
    CONSTRAINT storage_download_tickets_canonical_path_check CHECK (
        canonical_path <> ''
        AND canonical_path <> '/'
        AND canonical_path LIKE '/%'
        AND canonical_path !~ '/$'
        AND canonical_path !~ '//'
        AND canonical_path !~ '(^|/)\\.\\.?(/|$)'
        AND canonical_path !~ '[[:cntrl:]]'
    ),
    CONSTRAINT storage_download_tickets_created_at_s_positive_check CHECK (created_at_s > 0),
    CONSTRAINT storage_download_tickets_expiry_check CHECK (expires_at_s > created_at_s)
);

CREATE INDEX IF NOT EXISTS idx_storage_download_tickets_expires_at_s
    ON storage_download_tickets (expires_at_s);

COMMIT;
