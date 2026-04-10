BEGIN;

CREATE SEQUENCE IF NOT EXISTS users_user_id_seq
    START WITH 1
    INCREMENT BY 1
    MINVALUE 1;

CREATE TABLE IF NOT EXISTS users (
    user_id BIGINT PRIMARY KEY DEFAULT nextval('users_user_id_seq'),
    username TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL,
    created_at_s BIGINT NOT NULL
);

ALTER TABLE users
    ALTER COLUMN user_id
    SET DEFAULT nextval('users_user_id_seq');

COMMIT;
