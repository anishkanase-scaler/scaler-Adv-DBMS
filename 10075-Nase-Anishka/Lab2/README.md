# LAB 2 — SQLITE3 vs POSTGRESQL

> Roll Number: **10075** &nbsp;&nbsp;|&nbsp;&nbsp; Name: **Nase Anishka**
>
> In this lab I poked at how two very different database engines actually
> store data on disk. SQLite3 is a single-file embedded engine and PostgreSQL
> is a full client–server system, so I wanted to feel the difference with my
> own commands instead of just reading about it.
>
> Everything below was run on macOS (Apple Silicon) with the system
> `sqlite3` (v3.51.0) and Homebrew's `postgresql@14`. Each command in the
> blocks is exactly what I typed; the output blocks are real, copied from
> my terminal.

---

# SQLITE3

I made a fresh `sample.db`, watched the file grow as I added rows, and
played with the page-level pragmas.

---

* I created the DB with a single `users` table and looked at the file size.

  ```bash
  sqlite3 sample.db "CREATE TABLE users (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    email TEXT NOT NULL,
    age INTEGER NOT NULL
  );"
  ls -lh sample.db
  ```

  ```text
  -rw-r--r--  1 anishkanase  staff   8.0K May  9 23:44 sample.db
  ```

  Even with zero rows the file is already 8 KB, because SQLite immediately
  allocates a header page and a sqlite_schema page.

---

* `PRAGMA` told me the storage layout of the empty DB.

  ```bash
  sqlite3 sample.db "PRAGMA page_size; PRAGMA page_count; PRAGMA mmap_size;"
  ```

  ```text
  4096
  2
  0
  ```

  So the page size is **4 KB**, the file holds **2 pages** (4096 × 2 = 8192 B,
  matching the 8 KB on disk) and `mmap_size = 0` means SQLite is using normal
  `read()`/`write()` syscalls, not a memory-mapped region.

---

* Then I inserted 10,000 rows using a recursive CTE and re-checked.

  ```bash
  sqlite3 sample.db <<'SQL'
  WITH RECURSIVE seq(n) AS (
    SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n < 10000
  )
  INSERT INTO users (name, email, age)
  SELECT 'user_' || n, 'user_' || n || '@scaler.com', 18 + (n % 50) FROM seq;
  SQL

  ls -lh sample.db
  sqlite3 sample.db "PRAGMA page_size; PRAGMA page_count;"
  ```

  ```text
  -rw-r--r--  1 anishkanase  staff   404K May  9 23:44 sample.db
  4096
  101
  ```

  The file jumped from 8 KB to **404 KB** and the page count went from 2 to
  **101**. 101 × 4 KB = 404 KB exactly — confirming that SQLite grew the file
  one page at a time and the on-disk size is just `page_size × page_count`.
  It is literally a stack of fixed-size pages glued together inside one file.

---

* I timed `SELECT * FROM users;` first with the default settings, then with
  `mmap_size` bumped to 30 MB (large enough to hold the whole DB in mmap).

  ```bash
  time sqlite3 sample.db "PRAGMA mmap_size = 0; SELECT * FROM users;" > /dev/null
  ```

  ```text
  sqlite3 sample.db "PRAGMA mmap_size = 0; SELECT * FROM users;" > /dev/null
    0.01s user 0.00s system 79% cpu 0.011 total
  ```

  ```bash
  time sqlite3 sample.db "PRAGMA mmap_size = 30000000; SELECT * FROM users;" > /dev/null
  ```

  ```text
  sqlite3 sample.db "PRAGMA mmap_size = 30000000; SELECT * FROM users;" > /dev/null
    0.01s user 0.00s system 82% cpu 0.009 total
  ```

  The difference is in the **2 ms range** — basically noise. With `mmap_size = 0`
  SQLite calls `read()` to copy pages from the kernel page cache into its own
  buffer; with mmap on, the same pages are mapped directly into the process
  address space and accessed without an extra copy. On a 404 KB file the
  saved copies don't add up to anything visible, especially because the file
  is already in the OS page cache after the first run.

  This was actually a useful surprise — I came in assuming "mmap = always
  faster" but the real answer is "mmap only matters when you're paying for
  copies that the OS isn't already optimising for you." For a 400 KB file
  that lives entirely in the page cache, the syscall path is already fast.

---

* `ls -i` gave me the inode number — the OS-level identity of the file.

  ```bash
  ls -i sample.db
  ```

  ```text
  97820499 sample.db
  ```

  Even though `sample.db` *is* the database, the kernel knows it as inode
  `97820499`. That number doesn't change if I rename the file.

---

* I checked the `sqlite3` process with `ps aux` while one shell was sitting
  inside `sqlite3 sample.db` and again after I exited.

  ```text
  --- while the sqlite3 prompt was open ---
  anishkanase  25762  0.1  0.0  435301488  2656  ??  SN  11:44PM  0:00.01 sqlite3 sample.db

  --- after .quit ---
  (no matching processes)
  ```

  This is the big SQLite mental model: there is **no server**. `sqlite3` is
  just a CLI process that opens the file, holds locks while it works, and
  goes away. When the process exits, nothing is "running."

---

* Lastly I added a second table `products` and confirmed it lives inside the
  *same* `sample.db` file.

  ```bash
  sqlite3 sample.db <<'SQL'
  CREATE TABLE products (id INTEGER PRIMARY KEY, name TEXT, price REAL);
  INSERT INTO products (name, price) VALUES
   ('mechanical keyboard', 4999.00),
   ('usb-c hub', 1999.00),
   ('27 inch monitor', 18999.00),
   ('noise cancelling headphones', 12999.00);
  SQL

  sqlite3 sample.db ".tables"
  ls -lh sample.db
  sqlite3 sample.db "PRAGMA page_count;"
  ```

  ```text
  products  users
  -rw-r--r--  1 anishkanase  staff   408K May  9 23:45 sample.db
  102
  ```

  One more page (101 → 102), still one file. SQLite stores both tables inside
  a single B-tree forest within the same file — there is never a "second
  file" for the second table.

---

# POSTGRESQL

For Postgres I started the Homebrew service, made a fresh database, ran
the same workload, and looked at it through the same lens.

---

* First I started the server.

  ```bash
  brew services start postgresql@14
  pg_ctl -D /opt/homebrew/var/postgresql@14 status
  ```

  ```text
  ==> Successfully started `postgresql@14`
  pg_ctl: server is running (PID: 26149)
  ```

  Already a difference from SQLite — there is a **long-running server
  process** I have to start before any client can talk to the DB.

---

* I created `adbms_lab` and seeded the same 10,000 rows.

  ```bash
  createdb adbms_lab
  psql -d adbms_lab <<'SQL'
  CREATE TABLE users (
    id SERIAL PRIMARY KEY,
    name TEXT NOT NULL,
    email TEXT NOT NULL,
    age INTEGER NOT NULL
  );
  INSERT INTO users (name, email, age)
  SELECT 'user_' || g, 'user_' || g || '@scaler.com', 18 + (g % 50)
  FROM generate_series(1, 10000) AS g;
  SELECT count(*) FROM users;
  SQL
  ```

  ```text
  CREATE TABLE
  INSERT 0 10000
   count
  -------
   10000
  ```

---

* Then I timed `SELECT * FROM users;` with `\timing on`.

  ```text
  Time: 5.309 ms
  ```

  Way slower than the SQLite number above (0.009 s ≈ 9 ms whole-process,
  with most of that being SQLite's own startup). That makes sense — Postgres
  is doing more on every query: parse, plan, send 10,000 rows over a
  Unix socket, server-side cost-based optimisation. SQLite is just opening
  a file.

---

* `SHOW block_size;` and friends gave me the storage parameters.

  ```text
       k        |                v
  ----------------+---------------------------------
   block_size     | 8192
   shared_buffers | 128MB
   data_directory | /opt/homebrew/var/postgresql@14
  ```

  Page size is **8 KB** — twice SQLite's default 4 KB. Postgres also has
  its own in-process cache called `shared_buffers` (128 MB by default),
  which is the closest cousin to SQLite's `mmap_size`, although it works
  very differently — it's a buffer pool *managed by Postgres*, sitting
  on top of the kernel page cache, not a memory map.

---

* I asked Postgres how many pages the `users` table occupies.

  ```sql
  SELECT pg_relation_size('users')        AS bytes,
         pg_relation_size('users') / 8192 AS approx_pages,
         pg_size_pretty(pg_relation_size('users')) AS pretty_size;
  ```

  ```text
   bytes  | approx_pages | pretty_size
  --------+--------------+-------------
   688128 |           84 | 672 kB
  (1 row)
  ```

  **84 pages × 8 KB = 672 KB.** Same 10,000 rows that took 101 × 4 KB = 404 KB
  in SQLite take 84 × 8 KB = 672 KB in Postgres. Postgres' rows are heavier
  because every row gets a 24-byte header (xmin/xmax/cid/ctid for MVCC) plus
  alignment padding. The MVCC headers are the cost of being able to do
  concurrent writes safely.

---

* `EXPLAIN (ANALYZE, BUFFERS)` confirmed it's a sequential scan and showed
  that all 84 pages came from the buffer pool, not from disk.

  ```text
   Seq Scan on users  (cost=0.00..152.04 rows=6804 width=72)
                      (actual time=0.011..2.141 rows=10000 loops=1)
     Buffers: shared hit=84
   Planning: Buffers: shared hit=55
   Planning Time: 0.713 ms
   Execution Time: 3.448 ms
  ```

  `shared hit=84` means the entire table was already cached in
  `shared_buffers` from the seed step, so the scan touched memory only.
  No `read=` lines — no disk I/O at all on this run.

---

* I restarted Postgres to drop `shared_buffers` and re-timed the query.

  ```text
  Cold (right after `brew services restart postgresql@14`):  Time: 10.709 ms
  Warm (immediate re-run):                                    Time: 7.351 ms
  ```

  Cold-cache is roughly **1.5×** slower because the 84 pages have to be
  pulled in from the OS page cache (and possibly disk) into
  `shared_buffers`. Once they're cached, the second run is faster. This is
  the Postgres equivalent of the SQLite mmap experiment, just at a
  different layer.

---

* I checked where Postgres actually puts table data on disk.

  ```sql
  SELECT pg_relation_filepath('users');
  ```

  ```text
   pg_relation_filepath
  ----------------------
   base/16384/16386
  ```

  Then I created a `products` table and asked the same question:

  ```sql
  SELECT pg_relation_filepath('products');
  ```

  ```text
   pg_relation_filepath
  ----------------------
   base/16384/16395
  ```

  Two **different files** under `base/<database-oid>/<relation-oid>`. Listing
  them on disk:

  ```text
  -rw-------  1 anishkanase  admin  672K  base/16384/16386     # users
  -rw-------  1 anishkanase  admin  8.0K  base/16384/16395     # products
  ```

  This is the opposite of SQLite. In Postgres each table (and each index,
  and each TOAST overflow store) is its own file on disk, named after its
  internal `oid`. That's what makes it possible for Postgres to truncate or
  drop a single table without touching the rest of the DB.

---

* Finally `ps aux | grep postgres` showed how the "server" is actually a
  whole family of processes.

  ```text
  postgres: logical replication launcher
  postgres: stats collector
  postgres: autovacuum launcher
  postgres: walwriter
  postgres: background writer
  postgres: checkpointer
  /opt/homebrew/opt/postgresql@14/bin/postgres -D /opt/homebrew/var/postgresql@14
  ```

  Seven processes running just to keep the database healthy — the
  postmaster, plus dedicated workers for the WAL, the buffer flusher, the
  checkpointer, autovacuum, the stats collector, and replication. After I
  exited `psql` with `\q`, *every one of these stayed up*. SQLite was the
  exact opposite: nothing keeps running once the CLI exits.

---

# COMPARISON

| Aspect                         | SQLite3 (`sample.db`)            | PostgreSQL (`adbms_lab`)              |
| ------------------------------ | -------------------------------- | ------------------------------------- |
| Page / block size              | 4 KB (`PRAGMA page_size = 4096`) | 8 KB (`SHOW block_size = 8192`)       |
| Pages used by 10,000 `users`   | 101 pages                        | 84 pages                              |
| On-disk size of those rows     | 404 KB                           | 672 KB                                |
| File layout                    | One file holds *every* table     | One file per table under `base/<oid>` |
| Caching mechanism              | OS page cache + optional `mmap_size` | OS page cache + `shared_buffers` (128 MB default) |
| `SELECT *` on 10k rows         | ≈ 9–11 ms total wall clock (CLI) | 5.3 ms warm / 10.7 ms cold (server)   |
| Effect of mmap / shared cache  | Negligible at this size (~2 ms)  | ~1.5× speedup once `shared_buffers` is warm |
| Process model                  | One short-lived `sqlite3` CLI    | Always-on postmaster + 6 helper processes |
| What "closing" means           | Process exits, zero leftover     | Client exits, server keeps running    |

### What this all means

The two engines disagree on almost every architectural choice, and each
choice is a deliberate trade-off:

1. **One file vs many files.** SQLite keeps the entire database — schema,
   every table, every index — inside one regular file, which is why
   `sample.db` is so easy to copy, attach, version-control, or email.
   Postgres breaks the database into a forest of per-relation files so it
   can do per-table operations (truncate, vacuum, replicate one relation)
   without touching anything else, at the cost of needing a real data
   directory and a server to manage it.

2. **Page size.** SQLite picks 4 KB to match a typical filesystem page,
   minimising read amplification on small writes. Postgres picks 8 KB
   because it expects to do bulk sequential scans over tables that hold
   wider, MVCC-tagged rows; a bigger page amortises the per-page header
   cost. The same 10,000 rows weighed 404 KB in SQLite and 672 KB in
   Postgres almost entirely because Postgres rows carry MVCC headers
   (`xmin`, `xmax`, `cid`, `ctid`) that SQLite simply doesn't need.

3. **Caching.** SQLite's `mmap_size` is a knob to skip the
   `read()`-into-userspace copy by mapping the file directly into the
   process. Postgres has a much more involved buffer manager
   (`shared_buffers`) that does its own page replacement, lock-aware
   reads, and dirty-page tracking. The empirical observation in this lab
   was that mmap didn't measurably help SQLite — the dataset was tiny
   enough that the OS page cache already won — but `shared_buffers`
   *did* show up clearly in Postgres' cold-vs-warm timings.

4. **Process model.** This is where the philosophical gap is widest. The
   `ps aux` output makes it concrete: SQLite is one CLI process that
   exists for the duration of one shell session. Postgres is a permanent
   set of cooperating processes (WAL writer, checkpointer, autovacuum,
   replication launcher) that survive any client. That's exactly why
   SQLite is great for embedded apps and tests, and Postgres is the
   default when you need durability, concurrency, replication, or
   anything resembling production traffic.

---

# FINAL THINGS I LEARNED

* A database file is just a sequence of fixed-size pages — `page_size × page_count` literally equals the bytes on disk.
* Empty SQLite DBs are not empty: they already carry header + schema pages.
* SQLite's `mmap_size = 0` default is fine for small files; you only feel mmap when the dataset is big enough that copy-out costs matter.
* On macOS / Apple Silicon, the OS page cache is aggressive enough that "cold" SQLite numbers and "warm" SQLite numbers are within milliseconds of each other.
* Postgres pages are 8 KB and rows are heavier because every row carries MVCC bookkeeping — that's the price of safe concurrent writes.
* SQLite stores everything in one file; Postgres stores each relation in its own file under `base/<db_oid>/<rel_oid>`.
* `EXPLAIN (ANALYZE, BUFFERS)` is much more honest than `\timing` for understanding *why* a query is fast or slow — it tells you whether you hit the cache or read from disk.
* Postgres' "server" is really a constellation of background processes (checkpointer, walwriter, autovacuum, etc.) that live on after `psql` exits. SQLite has no such notion — when the CLI is gone, nothing is running.

---

# HOW TO REPRODUCE

```bash
# from this directory
bash commands.sh
```

The script (re)creates `sample.db`, drops and recreates the
`adbms_lab` Postgres database, runs every command shown above, and prints
its output to the terminal.
