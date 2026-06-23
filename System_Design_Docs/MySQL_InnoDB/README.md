# MySQL / InnoDB Storage Engine

> Advanced DBMS — System Design Discussion  
> Roll No: 10075 | Nase Anishka

---

## 1. Problem Background

MySQL was the dominant open-source SQL database for web applications throughout the 2000s, but for most of that time its default storage engine — **MyISAM** — had a fatal flaw: no transactions, no crash recovery, and only table-level locking. A failed INSERT could leave a table in a partially written state, and two concurrent writes on different rows of the same table blocked each other.

**InnoDB** (originally by Heikki Tuuri at Innobase Oy, later acquired by Oracle in 2005) was built to provide what MyISAM could not:

- **ACID transactions**: commit means committed; rollback means gone.
- **Crash recovery**: a committed transaction survives a hard power loss.
- **Row-level locking**: concurrent writes on different rows do not block each other.
- **MVCC**: concurrent reads do not block writes.

MySQL made InnoDB the default storage engine in version 5.5 (2010). The pluggable storage-engine architecture means MySQL's SQL layer (parser, optimizer) is separate from how data is stored — InnoDB is one pluggable engine. This design choice lets the SQL layer remain stable while storage engines evolve independently.

Understanding InnoDB matters not just for MySQL, but because it represents the "Oracle-style" approach to MVCC (in-place updates with undo logs) as opposed to PostgreSQL's "heap-tuple-version" approach — and that architectural difference has cascading consequences for performance, cleanup, and locking.

---

## 2. Architecture Overview

```
              SQL layer: Parser → Optimizer → Executor
                                    │
                              handler calls
                                    │
                                    ▼
  ┌─────────────────────────────────────────────────────────────────────┐
  │                     InnoDB Storage Engine                            │
  │                                                                      │
  │  ┌──────────────────── IN MEMORY ─────────────────────────────────┐ │
  │  │                                                                  │ │
  │  │   BUFFER POOL  (16KB pages)                                      │ │
  │  │   ┌──────────────────────────────────────────────────────────┐  │ │
  │  │   │  LRU list:                                                │  │ │
  │  │   │  [── young (hot) ~5/8 ──────][── old ~3/8 ──]            │  │ │
  │  │   │                              ▲ midpoint insert            │  │ │
  │  │   └──────────────────────────────────────────────────────────┘  │ │
  │  │                                                                  │ │
  │  │   Change buffer  ·  Log buffer  ·  Adaptive hash index (AHI)    │ │
  │  └─────────────────────────────────────────────────────────────────┘ │
  │          │ flush dirty pages                   │ WAL (log-first)     │
  │          ▼                                     ▼                     │
  │  ┌──────────────────────┐       ┌─────────────────────────────┐     │
  │  │  Tablespace (.ibd)    │       │  REDO LOG (#innodb_redo/)    │     │
  │  │  ┌────────────────┐  │       │  physiological, by LSN       │     │
  │  │  │ Clustered index│  │       └─────────────────────────────┘     │
  │  │  │  B+tree        │  │       ┌─────────────────────────────┐     │
  │  │  │ (= the table)  │  │       │  UNDO LOGS (rollback segs)   │     │
  │  │  ├────────────────┤  │       │  old row versions for MVCC   │     │
  │  │  │ Secondary index│  │       └─────────────────────────────┘     │
  │  │  │  B+trees       │  │       ┌─────────────────────────────┐     │
  │  │  └────────────────┘  │       │  Doublewrite buffer           │     │
  │  │                      │       │  (torn-page protection)       │     │
  │  └──────────────────────┘       └─────────────────────────────┘     │
  └─────────────────────────────────────────────────────────────────────┘
```

**Data flow.** A query from the SQL layer arrives at InnoDB via a "handler" API call. InnoDB reads and writes 16KB pages through the buffer pool. Any modification is first logged to the redo log (WAL), then applied in memory. The old row version is written to the undo log before the in-place update. Dirty pages are flushed to the tablespace later. The doublewrite buffer guards against torn-page writes.

---

## 3. Internal Design

### 3.1 Clustered Index — The Table Is the Index

The most important structural difference in InnoDB: **every table is a B+tree sorted by its primary key, and the leaf nodes of that B+tree contain the full row data**. There is no separate "heap" like in PostgreSQL.

```
   Clustered index (PK = user_id)

              [50 | 150 | 300]               ← internal page: separator keys
             /      |      |     \
    [1..49]  [50..149] [150..299] [300..]    ← leaf pages
   ┌────────┐ ┌────────┐
   │pk=1    │ │pk=50   │
   │name=…  │ │name=…  │   full row in leaf
   │email=… │ │email=… │
   └────────┘ └────────┘
```

How InnoDB chooses the clustering key:
1. Your explicit `PRIMARY KEY`
2. The first `UNIQUE NOT NULL` index if no PK is declared
3. A hidden 6-byte auto-increment `DB_ROW_ID` (`GEN_CLUST_INDEX`) if neither exists

**Consequence:** a primary key range scan (`WHERE id BETWEEN 100 AND 200`) reads physically contiguous leaf pages — extremely fast. Random primary key values (e.g., UUIDs) scatter insertions across the whole tree, causing page splits and fragmentation. This is the #1 reason InnoDB best-practice says: **use auto-increment integer primary keys**.

### 3.2 Secondary Indexes and the Double Lookup

A secondary index is a separate B+tree. Its leaf nodes do **not** store physical row pointers. Instead, they store `(indexed_columns, primary_key_value)`.

```
  Secondary index on (email):
  Leaf node: (email='a@b.com', pk=42)

  To fetch the full row:
  Step 1: walk email-index B+tree → find (email='a@b.com' → pk=42)
  Step 2: walk clustered index B+tree by pk=42 → retrieve full row
```

This two-step lookup is called a "double lookup" or "bookmark lookup." It means every secondary index query costs two B+tree traversals, not one.

**Covering index avoidance:** if the `SELECT` only needs columns that are already in the secondary index, InnoDB answers entirely from the index (shown as `Using index` in `EXPLAIN`). Adding frequently-queried columns to an index with `INCLUDE` (MySQL 8.0+) achieves this without including them in the sort key.

**Implication of storing the PK in every secondary index:** a fat primary key (e.g., a 36-byte UUID string) bloats all secondary indexes, since the PK value is duplicated in each one. Prefer compact PKs.

### 3.3 Buffer Pool and Scan-Resistant LRU

The buffer pool (`innodb_buffer_pool_size`) caches 16KB pages. Its LRU eviction is deliberately scan-resistant:

```
  LRU list:
  ┌─────────────────────────────────────────────────────────────┐
  │   young sublist (~5/8 of pool)  │   old sublist (~3/8)      │
  │       (hot pages stay here)      │   new pages land here     │
  └─────────────────────────────────│───────────────────────────┘
                                     ▲ midpoint insertion
```

A newly read page goes to the **head of the old sublist** (the midpoint), not the absolute head. It gets promoted to the young sublist only if it is accessed **again** after `innodb_old_blocks_time` (default 1000 ms). A full table scan reads many pages once — they land in the old sublist and age out, leaving the genuinely hot working set undisturbed.

The **Adaptive Hash Index (AHI)** is an in-memory hash table built automatically over frequently-accessed B+tree paths. When InnoDB notices that the same B+tree path is being traversed repeatedly, it builds a hash entry for it. A hash lookup replaces several B+tree comparisons — O(1) vs. O(log N) — for that hot key. AHI is transparent; it cannot be queried directly but appears in `SHOW ENGINE INNODB STATUS`.

The **Change Buffer** defers modifications to secondary-index pages that are not in the buffer pool. Instead of fetching the secondary-index page from disk to apply the update, InnoDB records the pending change in the change buffer and merges it when the page is next loaded. This turns random secondary-index I/O into sequential batch I/O.

### 3.4 Redo Log and Undo Log

InnoDB maintains two distinct logs that solve two distinct problems. This is a source of frequent confusion:

| | Redo Log | Undo Log |
|-|----------|----------|
| Stored where | `#innodb_redo/` (MySQL 8.0.30+) | Rollback segments in undo tablespaces |
| Identified by | LSN (Log Sequence Number) | Undo record addresses |
| Used for | Crash recovery — replay committed changes | Rollback + MVCC old-version reconstruction |
| Direction | Roll forward | Roll back |
| Cleaned by | Checkpoint advances past it | Purge thread removes no-longer-needed versions |

**Redo log operation:**
1. Before writing a dirty page to disk, InnoDB ensures the corresponding redo log record is already on disk (WAL invariant).
2. The log is **physiological**: it identifies both the physical location (page ID) and a logical description of the change within the page.
3. At COMMIT, the log buffer is flushed and fsynced to `#innodb_redo/`.
4. On crash restart, InnoDB reads from the last checkpoint and replays all redo records forward, reconstructing committed changes.

**`innodb_flush_log_at_trx_commit` values:**
- `1` (default): fsync on every commit — full ACID, slowest.
- `2`: write to OS cache on commit, fsync ~once/second — fast, can lose ~1s of commits on OS crash (not hardware crash).
- `0`: flush to OS cache only occasionally — fastest, can lose ~1s even on normal crash.

Group commit lets multiple transactions share a single fsync, which dramatically improves throughput at `=1` under high concurrency.

**Undo log operation:**
1. Before updating a row, InnoDB copies the old column values to the undo log.
2. `ROLLBACK` re-applies the old values from the undo log.
3. MVCC reads use the undo log to reconstruct old row versions (via `DB_ROLL_PTR` — see §3.5).

### 3.5 MVCC — Oracle-Style via Undo

Each clustered-index row carries two hidden system columns:
- `DB_TRX_ID` (6 bytes): the XID of the last transaction to insert or update this row.
- `DB_ROLL_PTR` (7 bytes): a pointer to the undo record holding the previous version.

```
  Row visible chain:

  [current row: name='Bob', DB_TRX_ID=50, DB_ROLL_PTR→]
                                                        │
                                    [undo: name='Alice', created by TRX=30, prev→]
                                                                                  │
                                    [undo: name='Alex', created by TRX=10, prev=NULL]
```

When a transaction with a snapshot that only sees TXs up to 40 reads this row, it follows `DB_ROLL_PTR` back through the undo chain until it finds a version installed by a committed TX ≤ 40. It lands on `name='Alice'`.

**Key contrast with PostgreSQL:**
- PostgreSQL: dead versions stay in the heap file, cleaned by VACUUM.
- InnoDB: dead versions are in the undo log, cleaned by the background **purge thread**.

In both cases a long-running transaction that pins an old snapshot prevents cleanup — in PostgreSQL this causes table bloat; in InnoDB this causes undo log (history list) growth.

### 3.6 Locking and Isolation

InnoDB does row-level locking on **index records**, not on rows directly. Three lock types build on each other:

```
  Records:      ┆  10  ┆  20  ┆  30  ┆  40  ┆
                    ↑         ↑
  Record lock:  locks the exact record (20 or 30)

  Gap lock:     locks the GAP between records — e.g. (20, 30)
                prevents inserts into that range (no row exists to lock)

  Next-key lock: record lock on 30 + gap lock on (20, 30)
                 locks both the record and the gap before it
```

Under the default isolation level **REPEATABLE READ**, InnoDB takes next-key locks on all index entries scanned by a range query. This prevents phantom reads (a new row inserted into the scanned range) without needing SERIALIZABLE.

**Intention locks** are table-level locks (`IS` / `IX`) that indicate "some row in this table is locked." They allow the lock manager to quickly check coarse-grained compatibility without scanning all row locks.

**Deadlock detection:** InnoDB maintains a waits-for graph and detects cycles automatically, rolling back the "cheapest" transaction (smallest undo log size). `SHOW ENGINE INNODB STATUS` shows the most recent deadlock.

**Isolation level summary:**

| Level | Snapshot per | Phantom prevention |
|-------|-------------|-------------------|
| READ UNCOMMITTED | — | None (dirty reads) |
| READ COMMITTED | Statement | Next-key locks released after each statement |
| REPEATABLE READ (default) | Transaction | Next-key locks held for transaction duration |
| SERIALIZABLE | Transaction | All reads become locking reads |

---

## 4. Design Trade-Offs

**Clustered index — fast PK access, painful UUID inserts.**  
Row data lives in PK-order leaf pages. PK lookups read one path down the B+tree and land directly on the data — one traversal, no heap fetch. But inserting random PKs (UUID, hash) scatters writes across the tree, causes page splits and "page fragmentation," and defeats the InnoDB buffer pool's ability to keep contiguous pages warm. Auto-increment PKs solve this by always appending to the rightmost leaf page.

**Double lookup cost of secondary indexes.**  
Every secondary-index query that needs columns outside the index touches two B+trees. Under heavy secondary-index read workloads, this doubles I/O compared to a covering index. PostgreSQL has the same problem (index → heap tuple ID → heap page), but InnoDB's clustered B+tree is at least guaranteed to have the data in a predictable location.

**Why both undo and redo?**  
They are not redundant — they do opposite jobs. Redo rolls forward (replay committed changes lost in a crash). Undo rolls back (undo uncommitted changes, serve old versions to concurrent readers). You cannot combine them: one rolls forward and the other rolls back. Removing either would sacrifice either durability or atomicity/MVCC.

**InnoDB MVCC (in-place + undo) vs PostgreSQL MVCC (new heap tuple):**  
| Aspect | InnoDB | PostgreSQL |
|--------|--------|-----------|
| Update | Modify row in place; save old value to undo log | Write new tuple in heap; old tuple stays |
| Old-version storage | Undo tablespace | Heap file |
| Cleanup process | Purge thread (undo records) | VACUUM (heap tuples) |
| Primary index bloat | No (clustered B+tree stays compact) | Yes (dead tuples in heap) |
| Secondary index impact | Not touched if indexed column unchanged (HOT-equivalent) | Index must be updated for any column change (unless HOT) |

**Next-key locking vs. pure row locking.**  
Next-key locks give REPEATABLE READ true phantom-free semantics, which is a stronger guarantee than the SQL standard requires for REPEATABLE READ. The cost: a range scan `WHERE age > 25` locks the gap after 25 — an INSERT of `age = 26` by a concurrent transaction blocks. This causes occasional unexpected lock waits in range-heavy workloads.

---

## 5. Experiments / Observations

These examples are based on documented InnoDB behavior (MySQL 8.0/8.4 reference manual), since MySQL was not installed locally.

**(a) Clustered vs. double-lookup cost visible in EXPLAIN.**

```sql
-- PK lookup: walks clustered index, row is in the leaf — one tree
EXPLAIN SELECT * FROM users WHERE id = 42;
-- type: const   key: PRIMARY   Extra: (nothing)

-- Secondary index: walks email index, then clustered index by PK — two trees
EXPLAIN SELECT * FROM users WHERE email = 'a@b.com';
-- type: ref   key: idx_email   Extra: (row fetched via PK)

-- Covering index: answered from email-index alone — one tree
EXPLAIN SELECT email FROM users WHERE email = 'a@b.com';
-- type: ref   key: idx_email   Extra: Using index
```

The `Using index` flag in the covering case is the proof that no clustered-index lookup happened.

**(b) History list length as undo health indicator.**

```sql
SHOW ENGINE INNODB STATUS\G
-- History list length 427
```

This number grows when transactions commit but the purge thread cannot remove old undo records because a long-running transaction is still using an old snapshot. It should stay near zero in a healthy system. A rising history list with no active long transactions is a signal of purge thread falling behind.

**(c) Next-key lock blocking a phantom insert.**

```sql
-- Session 1
START TRANSACTION;
SELECT * FROM orders WHERE amount > 100 FOR UPDATE;
-- InnoDB places next-key locks on all index records with amount > 100
-- and on the gap after the last such record

-- Session 2
INSERT INTO orders(amount) VALUES (150);
-- BLOCKED: the gap (100, +∞) is locked by session 1
-- Session 2 waits until session 1 commits or rolls back
```

This shows why REPEATABLE READ in InnoDB prevents phantoms: the gap lock blocks the insert that would otherwise create a phantom row.

**(d) AHI reducing repeated B+tree traversals.**

After a hot key is accessed repeatedly, `INNODB STATUS` shows `Hash table size: N, used cells: M, node heap has K buffers` and the `searches/s` metric. AHI effectively caches the leaf-node address for hot primary keys, reducing the typical 3–4 level B+tree traversal to a single hash lookup for those keys.

---

## 6. Key Learnings

1. **"The table is the index" is the most consequential InnoDB design choice.** Storing the entire row in the B+tree leaf node means PK access needs one traversal and no heap lookup. Everything else — double lookup for secondary indexes, UUID fragmentation, the case for short PKs — flows from this.

2. **Undo and redo solve orthogonal problems and cannot replace each other.** Redo is a forward-rolling durability log; undo is a backward-rolling atomicity and MVCC log. Understanding which is which, and when each is read, is essential to reasoning about crash recovery and long-transaction performance.

3. **MVCC in InnoDB happens in the undo tablespace, not in the main data file.** Old versions live in undo records, not in the clustered index. The purge thread is InnoDB's equivalent of VACUUM. A long-running transaction blocks purge and causes undo growth, which shows up as a growing history list length.

4. **Next-key locking is InnoDB's way of making REPEATABLE READ stronger than the standard.** Most RDBMSes allow phantoms at REPEATABLE READ; InnoDB's gap locks prevent them at the cost of occasionally holding locks on ranges no row occupies, which can block concurrent inserts.

5. **The buffer pool's midpoint insertion strategy is a deliberate scan-resistance mechanism.** Without it, a single full table scan would evict the entire working set. The 1-second timer before promotion to the young sublist ensures that pages touched only once by a sequential scan never displace truly hot data.

---

## References

- MySQL 8.4 Reference Manual: [InnoDB Architecture](https://dev.mysql.com/doc/refman/8.4/en/innodb-architecture.html), [Clustered and Secondary Indexes](https://dev.mysql.com/doc/refman/8.4/en/innodb-index-types.html), [MVCC](https://dev.mysql.com/doc/refman/8.4/en/innodb-multi-versioning.html), [Buffer Pool](https://dev.mysql.com/doc/refman/8.4/en/innodb-buffer-pool.html), [Redo Log](https://dev.mysql.com/doc/refman/8.4/en/innodb-redo-log.html), [InnoDB Locking](https://dev.mysql.com/doc/refman/8.4/en/innodb-locking.html)
- Jeremy Cole: [The physical structure of InnoDB index pages](https://blog.jcole.us/2013/01/07/the-physical-structure-of-innodb-index-pages/)
