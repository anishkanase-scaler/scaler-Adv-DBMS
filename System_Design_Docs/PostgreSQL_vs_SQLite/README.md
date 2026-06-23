# PostgreSQL vs SQLite: Architecture Comparison

> Advanced DBMS — System Design Discussion  
> Roll No: 10075 | Nase Anishka

---

## 1. Problem Background

Every database engine has to answer the same fundamental questions: where does data live, how do multiple users access it safely, and how do we survive a crash? PostgreSQL and SQLite answer those questions in opposite ways, and the differences flow from a single upfront design decision: **who runs the database engine?**

**PostgreSQL** (born from Berkeley's POSTGRES project in 1986) is built for the *server* world. It assumes the database is a shared resource that many different programs and users will hit simultaneously over a network. Its core design priority is **concurrency without compromising correctness**. Banks, SaaS platforms, and analytics pipelines use it because it handles tens of thousands of simultaneous connections while keeping every transaction ACID-compliant.

**SQLite** (created by D. Richard Hipp in 2000 for the US Navy) takes the opposite bet: get rid of the server entirely. The whole database is a single file that any program can open like a document. The SQLite project's own phrasing nails the intent:

> *"SQLite does not compete with client/server databases. SQLite competes with `fopen()`."*

SQLite is embedded — there is no daemon, no port, no configuration. It is linked directly into the application as a library. That is why it is in every Android phone, every iPhone, every major browser, and most embedded systems. It is the most widely deployed database engine in existence, with over a trillion active databases.

The entire design space of both systems follows from this one split.

---

## 2. Architecture Overview

### PostgreSQL — client/server, process-per-connection

```
   App A      App B      psql       Django ORM
     │          │          │            │
     └──────────┴──────────┴────────────┘
                           │  TCP or Unix socket
                           ▼
               ┌───────────────────────┐
               │  POSTMASTER (PID 1)    │  supervisor; listens & forks
               └───────┬───────────────┘
                  fork │       fork │       fork │
                       ▼            ▼            ▼
               ┌──────────┐ ┌──────────┐ ┌──────────┐
               │ backend  │ │ backend  │ │ backend  │  one OS process
               │ process  │ │ process  │ │ process  │  per connection
               └────┬─────┘ └────┬─────┘ └────┬─────┘
                    └────────────┼─────────────┘
                                 ▼
               ┌─────────────────────────────────────┐
               │           SHARED MEMORY              │
               │   shared_buffers  (8KB page cache)   │
               │   WAL buffers · lock tables · stats  │
               └──────────┬──────────────────────────┘
                          │ reads/writes pages         WAL-first writes
                          ▼                                  ▼
               ┌─────────────────────┐      ┌───────────────────────┐
               │  Heap + Index files  │      │  pg_wal/ (redo log)    │
               │  base/<oid> …        │      │  checkpoint state      │
               └─────────────────────┘      └───────────────────────┘
         background: bgwriter · checkpointer · walwriter · autovacuum
```

**Key decisions:**
- One OS process per connection (not threads). Processes isolate faults — a crashing session cannot corrupt shared memory.
- All backends share one page cache (`shared_buffers`). A hot page stays hot for everyone.
- WAL is written and fsynced before COMMIT returns — that is the durability guarantee.

### SQLite — embedded library, single-file

```
   ┌────────────────────────────────────────────────┐
   │                  YOUR PROCESS                    │
   │                                                  │
   │   SQL text                                       │
   │      ↓                                           │
   │   Tokenizer → Parser → Code Generator           │
   │      ↓                                           │
   │   VDBE  (virtual database engine / bytecode VM) │
   │      ↓                                           │
   │   B-Tree module   (tables = rowid B-trees,       │
   │                    indexes = separate B-trees)   │
   │      ↓                                           │
   │   Pager  (page cache + journal + locking)        │
   │      ↓                                           │
   │   VFS  (OS abstraction: read/write/fsync/lock)  │
   └───────────────────┬────────────────────────────┘
                       │
                       ▼
           ┌───────────────────────┐
           │   mydb.db              │   one file = the whole database
           │   mydb.db-wal (opt.)   │   present only in WAL mode
           └───────────────────────┘
```

The whole engine ships as a single C amalgamation file (`sqlite3.c`, ~238K lines). SQL text is compiled to **VDBE bytecode** and interpreted. There is no network layer, no server process, no configuration files. The Pager is where locking and journal management live; everything above it is storage-agnostic.

---

## 3. Internal Design

### 3.1 File Organisation

| Aspect | PostgreSQL | SQLite |
|--------|-----------|--------|
| On-disk layout | Directory ("cluster"); one file per relation | One `.db` file for the whole database |
| Page size | 8 KB (compile-time constant) | Default 4096 B (configurable per DB) |
| Table representation | Unordered heap file (rows go wherever free space exists) | B-tree keyed by rowid (leaf holds full row) |
| Index | Separate B-tree file(s) | Separate B-tree inside the same database file |
| Overflow values | TOAST (Toast Off-Attribute Storage) in sibling file | Overflow page chain within same file |
| Free-space tracking | Free Space Map file (`_fsm`) | B-tree metadata tracks free pages |

SQLite's "everything is a B-tree in one file" design has an elegant consequence: you can verify the whole schema by reading a single header:

```sql
-- SQLite: what lives at which page?
SELECT type, name, rootpage FROM sqlite_schema;
-- table  students   2
-- index  idx_email  3
-- index  idx_pk     4
-- three B-trees, three rootpages, one file
```

PostgreSQL's heap approach means rows are stored in arrival order — a range scan on a non-indexed column requires a full sequential scan unless a B-tree index exists. But it also means inserts never need to split any tree; they just append to a heap page.

### 3.2 Transaction Model and Concurrency

**PostgreSQL: MVCC with row-level granularity.**

Every row carries hidden system columns `xmin` (the transaction that inserted it) and `xmax` (the transaction that deleted/updated it). An UPDATE does *not* overwrite the old row — it writes a new row version and sets `xmax` on the old one. A reader takes a snapshot of which transactions are committed at query start, and uses `xmin`/`xmax` to decide which row version is visible to it.

Result: **readers never block writers; writers never block readers.** Many transactions run concurrently on the same table rows. Dead old row versions accumulate, so the VACUUM process reclaims them.

**SQLite: file-level locking, one writer at a time.**

SQLite coordinates concurrent processes through OS file locks. The lock progression is:

```
UNLOCKED
   → SHARED       (read; unlimited concurrent holders)
   → RESERVED     (intends to write; readers still allowed; only one at a time)
   → PENDING      (wants exclusive; blocks new SHARED grants)
   → EXCLUSIVE    (write; sole lock holder)
```

While one process holds EXCLUSIVE, all others wait. Reads in rollback-journal mode block during writes. **WAL mode** (`PRAGMA journal_mode=WAL`) improves this: readers read from the database file while a writer appends to the WAL file — reads and the single writer can overlap. But there is still only one concurrent writer.

### 3.3 Durability Mechanisms

**PostgreSQL WAL:**
1. Modification described in a WAL record (what changed, on which page, at what LSN).
2. WAL record flushed to `pg_wal/` before COMMIT is acknowledged.
3. Data page eventually flushed by bgwriter/checkpointer. On crash, WAL replays from last checkpoint.

**SQLite rollback journal:**
1. Original page content saved to `-journal` file before modification.
2. COMMIT = delete (or zero-length) the journal.
3. On crash, a "hot journal" is detected on next open and rolled back automatically.

**SQLite WAL mode:**
1. Changes append to `-wal` file; database file unchanged.
2. Readers walk the WAL backwards to find the latest committed version of a page.
3. A checkpoint periodically writes WAL pages back to the main file.

### 3.4 Index Implementation

Both databases use B-tree indexes, but with a critical difference in what the leaf nodes contain:

- **PostgreSQL**: A B-tree index leaf stores `(indexed_value, heap_tuple_id)`. To fetch the full row, the executor does a second lookup into the heap file by tuple ID. This is a "heap fetch" and is an extra I/O. A *covering index* (`INCLUDE` columns) avoids the heap fetch by putting extra columns in the leaf.
- **SQLite**: Every table is itself a B-tree. A secondary index leaf stores `(indexed_value, rowid)`. Getting the full row requires a second traversal of the table B-tree by rowid.

Both thus require a double lookup for non-primary queries — the difference is where the second lookup lands (heap file vs. table B-tree).

---

## 4. Design Trade-Offs

| Dimension | PostgreSQL | SQLite |
|-----------|-----------|--------|
| Deployment | Install server, configure, keep running | Embed library, open file |
| Concurrent writes | True multi-writer MVCC | One writer at a time |
| Network | Built-in remote access | None; same process only |
| Access control | Full role/grant/row-level security | OS file permissions only |
| Dead row cleanup | VACUUM process required | Not needed (no dead versions in heap) |
| Resource baseline | Hundreds of MB RAM, daemon process | ~1 MB library, zero overhead at rest |
| Scalability ceiling | Horizontal (streaming replication, logical decoding) | Vertical only; single file on one machine |
| Integrity enforcement | FK, triggers, check constraints, domains | FK (off by default!), triggers, check constraints |

**The VACUUM cost.** PostgreSQL's MVCC requires a background cleanup job. If VACUUM falls behind (e.g., a long-running transaction pins old snapshots), dead row versions accumulate ("table bloat"), and performance degrades. This is a real operational concern that SQLite users never face — because SQLite updates in place and has no dead versions to clean.

**The single-writer bottleneck.** SQLite's biggest limitation for server workloads is serialized writes. Under high write concurrency, processes pile up waiting for the EXCLUSIVE lock. PostgreSQL's row-level MVCC means two transactions writing to different rows of the same table proceed in parallel without blocking each other.

**Where SQLite wins by not trying.** Zero-admin means zero admin mistakes. SQLite's file format is documented and guaranteed stable across versions. Because it's in-process, there's no network round-trip latency. For read-heavy applications with occasional writes (mobile apps, local caches, test fixtures), the concurrency ceiling is never reached, and SQLite's simplicity wins decisively.

---

## 5. Experiments / Observations

**(a) SQLite's "table is a B-tree" verified.**

On a small demo database, `PRAGMA page_count` returned 4, and `sqlite_schema` listed rootpages 2, 3, 4 — three B-trees (one table, two auto-indexes) packed into 4 pages × 4096 B = 16 KB. The database is just these few pages, all in one file.

**(b) The VDBE bytecode path.**

Running `EXPLAIN` in SQLite shows the actual opcodes before execution. An indexed lookup like `SELECT * FROM users WHERE email = ?` generates a `SeekGE` + `IdxGT` instruction pair: walk the index B-tree to the right position, then use the rowid to seek the table B-tree. Two B-tree traversals for one query — the same "double lookup" cost that PostgreSQL has for a non-covering index.

**(c) PostgreSQL WAL size under a bulk insert.**

In PostgreSQL, a single `COPY` of 500K rows generates a WAL stream of roughly 40–80 MB (depending on row width), which must be fsynced before COMMIT returns at `synchronous_commit = on`. This is the durability tax: every committed byte of data passes through the WAL log. Lowering `synchronous_commit` to `off` makes commits return immediately but allows a window of data loss on crash.

**(d) Lock escalation vs. MVCC.**

In SQLite (rollback journal mode), starting a `BEGIN` and doing a `SELECT` acquires a SHARED lock but NOT a RESERVED lock. A subsequent `INSERT` upgrades to RESERVED. At this point, another connection trying `INSERT` gets `SQLITE_BUSY` immediately — it cannot obtain RESERVED. In PostgreSQL, two sessions inserting into the same table simultaneously do not block at all; only inserting the same primary key value causes a conflict (which would result in a serialization error or wait for a row lock depending on isolation).

---

## 6. Key Learnings

1. **Architecture is determined by the threat model.** PostgreSQL's process isolation, shared memory, and WAL are all responses to the threat of concurrent users corrupting shared state. SQLite's pager and file locking are responses to the threat of two processes corrupting a single file. Same SQL language, opposite threat models, opposite architectures.

2. **MVCC trades cleanup cost for concurrency.** PostgreSQL's "write a new version, never overwrite" gives outstanding concurrent read performance, but you now have a garbage collection problem (VACUUM). SQLite avoids the problem by not having multiple versions — at the cost of serializing writers.

3. **Both need two lookups for secondary index queries.** Whether it is "index leaf → heap tuple ID → heap file" (PostgreSQL) or "index leaf → rowid → table B-tree" (SQLite), the pattern is the same. The covering index optimization (put extra columns in the index leaf) is the way out of this in both systems.

4. **Simplicity has compounding returns.** SQLite's single-file, zero-server design makes it trivially embeddable, trivially testable, trivially backed-up (just `cp`), and trivially deployable. These properties alone explain why it is the most deployed database on the planet, even though it would be the wrong choice for a multi-user server workload.

5. **`synchronous_commit` and `PRAGMA synchronous` are the durability dial.** Both systems let you trade durability for write throughput by relaxing fsync discipline. Understanding where this dial sits — and what you lose when you turn it down — is one of the most practically important things to know when operating either database.

---

## References

- SQLite Documentation: [Architecture](https://www.sqlite.org/arch.html), [File Format](https://www.sqlite.org/fileformat2.html), [WAL Mode](https://www.sqlite.org/wal.html), [Locking And Concurrency](https://www.sqlite.org/lockingv3.html)
- PostgreSQL 16 Documentation: [Database File Layout](https://www.postgresql.org/docs/current/storage-file-layout.html), [MVCC](https://www.postgresql.org/docs/current/mvcc.html), [WAL](https://www.postgresql.org/docs/current/wal.html)
- D. R. Hipp: [SQLite's design philosophy](https://www.sqlite.org/whentouse.html)
