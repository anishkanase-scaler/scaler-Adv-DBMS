#!/usr/bin/env bash
# Lab 2 — SQLite3 vs PostgreSQL experiments
# Author: Nase Anishka (Roll No. 10075)
# Run from this directory: bash commands.sh
# Tested on macOS (sqlite3 3.51.0, PostgreSQL 14 via Homebrew).

set -u

banner() {
  echo
  echo "========================================="
  echo "  $1"
  echo "========================================="
}

# ---------------------------------------------------------------
banner "SQLITE3 EXPERIMENTS"
# ---------------------------------------------------------------

DB=sample.db
rm -f "$DB"

echo
echo "[1] Create empty users table and check size of an empty DB"
sqlite3 "$DB" "CREATE TABLE users (
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  email TEXT NOT NULL,
  age INTEGER NOT NULL
);"
ls -lh "$DB"

echo
echo "[2] Pragmas on the empty DB (page_size / page_count / mmap_size)"
sqlite3 "$DB" "PRAGMA page_size; PRAGMA page_count; PRAGMA mmap_size;"

echo
echo "[3] Insert 10,000 rows using a recursive CTE"
sqlite3 "$DB" <<'SQL'
BEGIN;
WITH RECURSIVE seq(n) AS (
  SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n < 10000
)
INSERT INTO users (name, email, age)
SELECT 'user_' || n,
       'user_' || n || '@scaler.com',
       18 + (n % 50)
FROM seq;
COMMIT;
SQL

echo
echo "[4] Size and pragmas after inserts"
ls -lh "$DB"
sqlite3 "$DB" "PRAGMA page_size; PRAGMA page_count; PRAGMA mmap_size;"

echo
echo "[5] Time SELECT * with mmap_size = 0 (the default)"
time sqlite3 "$DB" "PRAGMA mmap_size = 0; SELECT * FROM users;" > /dev/null

echo
echo "[6] Time the same SELECT with mmap_size = 30 MB"
time sqlite3 "$DB" "PRAGMA mmap_size = 30000000; SELECT * FROM users;" > /dev/null

echo
echo "[7] Inode of the SQLite file"
ls -i "$DB"

echo
echo "[8] ps aux | grep sqlite3 (process is only alive while the shell is open)"
echo "(start sqlite3 in a second terminal with: sqlite3 $DB  and run ps aux | grep sqlite3 there)"

echo
echo "[9] Add a second table 'products' inside the same DB file"
sqlite3 "$DB" <<'SQL'
CREATE TABLE products (
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  price REAL NOT NULL
);
INSERT INTO products (name, price) VALUES
 ('mechanical keyboard', 4999.00),
 ('usb-c hub', 1999.00),
 ('27 inch monitor', 18999.00),
 ('noise cancelling headphones', 12999.00);
SQL
sqlite3 "$DB" ".tables"
ls -lh "$DB"
sqlite3 "$DB" "PRAGMA page_count;"

# ---------------------------------------------------------------
banner "POSTGRESQL EXPERIMENTS"
# ---------------------------------------------------------------

echo
echo "[1] Make sure the Homebrew Postgres service is running"
brew services start postgresql@14 >/dev/null 2>&1 || true
pg_ctl -D /opt/homebrew/var/postgresql@14 status | head -2

echo
echo "[2] Create the database and seed 10,000 rows"
psql -d postgres -c "DROP DATABASE IF EXISTS adbms_lab;" >/dev/null
createdb adbms_lab
psql -d adbms_lab <<'SQL'
CREATE TABLE users (
  id SERIAL PRIMARY KEY,
  name TEXT NOT NULL,
  email TEXT NOT NULL,
  age INTEGER NOT NULL
);
INSERT INTO users (name, email, age)
SELECT 'user_' || g,
       'user_' || g || '@scaler.com',
       18 + (g % 50)
FROM generate_series(1, 10000) AS g;
SELECT count(*) FROM users;
SQL

echo
echo "[3] Time SELECT * FROM users using \\timing"
psql -d adbms_lab -c "\timing on" -c "\o /dev/null" -c "SELECT * FROM users;"

echo
echo "[4] Storage parameters: block_size, shared_buffers, data_directory"
psql -d adbms_lab -c "SHOW block_size;"
psql -d adbms_lab -c "SHOW shared_buffers;"
psql -d adbms_lab -c "SHOW data_directory;"

echo
echo "[5] Page count and on-disk size of the users table"
psql -d adbms_lab -c "
  SELECT pg_relation_size('users')        AS bytes,
         pg_relation_size('users') / 8192 AS approx_pages,
         pg_size_pretty(pg_relation_size('users')) AS pretty_size;
"

echo
echo "[6] EXPLAIN (ANALYZE, BUFFERS) — show buffer hits/reads"
psql -d adbms_lab -c "EXPLAIN (ANALYZE, BUFFERS) SELECT * FROM users;"

echo
echo "[7] On-disk file path of users (relative to data_directory)"
psql -d adbms_lab -c "SELECT pg_relation_filepath('users');"

echo
echo "[8] Add a second table 'products' and check that it lives in a SEPARATE file"
psql -d adbms_lab <<'SQL'
CREATE TABLE products (
  id SERIAL PRIMARY KEY,
  name TEXT NOT NULL,
  price NUMERIC NOT NULL
);
INSERT INTO products (name, price) VALUES
 ('mechanical keyboard', 4999),
 ('usb-c hub', 1999),
 ('27 inch monitor', 18999),
 ('noise cancelling headphones', 12999);
SELECT pg_relation_filepath('products');
SQL

echo
echo "[9] ps aux | grep postgres — multiple background processes"
ps aux | grep -i postgres | grep -v grep

echo
echo "[10] Quit psql with \\q and re-run ps aux | grep postgres — server keeps running"
echo "(the psql client is gone, but checkpointer / walwriter / autovacuum / etc. are still there)"

banner "DONE"
