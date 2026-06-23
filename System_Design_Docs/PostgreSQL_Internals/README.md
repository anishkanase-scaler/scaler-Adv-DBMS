# PostgreSQL Internal Architecture

> Advanced DBMS — System Design Discussion  
> Roll No: 10075 | Nase Anishka

---

## 1. Problem Background

PostgreSQL was built to answer a very specific question: *how do you let many users read and write shared data at the same time, without ever letting one user see another's uncommitted work, and without losing committed data even if the machine loses power immediately after COMMIT returns?*

The Berkeley POSTGRES project (1986) came out of Stonebraker's dissatisfaction with the relational systems of the time, which either sacrificed concurrency (table-level locking) or durability (in-memory updates without WAL). PostgreSQL's architecture is essentially a collection of decisions made to solve those exact problems:

- **Concurrency without blocking**: MVCC — let each transaction see a consistent snapshot; reads never block writers; writers never block readers.
- **Durability without slow I/O on every write**: WAL — log changes first, flush pages lazily, replay the log after crashes.
- **Efficient access for both point lookups and range scans**: B-tree indexes over an unordered heap.
- **Query execution without hand-coded plans**: a cost-based planner driven by table statistics.

Everything else in PostgreSQL's architecture is a consequence of these four choices.

---

## 2. Architecture Overview

```
                           ┌──── QUERY FROM CLIENT ────┐
                           │ (psql / JDBC / libpq …)    │
                           └────────────┬───────────────┘
                                        │ TCP or Unix socket
                                        ▼
                            ┌───────────────────────┐
                            │      POSTMASTER         │
                            │  (supervisor; pid 1)    │
                            │  fork() per connection  │
                            └───────────┬────────────┘
                                        │ fork
                                        ▼
  ┌───────────────────────────────────────────────────────────────────────┐
  │                        BACKEND PROCESS                                 │
  │                                                                        │
  │  SQL text → Tokenizer → Parser → Rewriter → Planner → Executor        │
  │                                      ↑                                 │
  │                               pg_statistic                             │
  │                            (column histograms,                         │
  │                             n_distinct, correlation …)                 │
  └─────────────────────────────┬──────────────────────────────────────────┘
                                │ reads / writes 8KB pages
                                ▼
  ┌────────────────────────────────────────────────────────────────────────┐
  │                        SHARED MEMORY                                    │
  │                                                                        │
  │    shared_buffers          WAL buffers       Lock table                │
  │    (8KB page cache,        (in-flight WAL     (per-relation and        │
  │     clock-sweep evict)      records before     per-tuple locks)         │
  │                             flush to pg_wal)                            │
  └──────────────┬────────────────────────┬───────────────────────────────┘
                 │ flush dirty pages       │ WAL: log-first writes
                 ▼                         ▼
  ┌──────────────────────┐    ┌────────────────────────────────────┐
  │  DATA FILES           │    │  pg_wal/  (Write-Ahead Log)         │
  │  base/<dboid>/<rel>   │    │  sequence of LSN-addressed records  │
  │  one file per         │    │  checkpoint pointer                  │
  │  heap / index         │    └────────────────────────────────────┘
  └──────────────────────┘
   background: bgwriter · checkpointer · walwriter · autovacuum · stats
```

**Data flow summary.** A query arrives at the backend, gets parsed and planned (the planner consults `pg_statistic`), and the executor runs it by reading 8KB pages from `shared_buffers`. On a miss, the buffer manager fetches from disk. Any modification is described in a WAL record and appended to the WAL buffer, which is flushed to `pg_wal/` (at latest at COMMIT). The actual data page stays dirty in `shared_buffers` and is written to disk later by the bgwriter or checkpointer. If the machine crashes, replay of the WAL from the last checkpoint restores all committed changes.

---

## 3. Internal Design

### 3.1 Buffer Manager

The buffer manager is the page cache for 8KB blocks. Its job is to keep frequently used pages in RAM and write dirty pages to disk without blocking query execution.

```
  shared_buffers pool   (N slots, each 8KB)

  ┌─────┬─────┬─────┬─────┬─────┬──  …  ──┐
  │ pg  │ pg  │ pg  │ pg  │ pg  │         │   Buffer descriptors
  │ #4  │ #7  │ #2  │ #9  │ #1  │         │   (pinned? dirty? usage_count)
  └──┬──┴──┬──┴─────┴──┬──┴─────┴─────────┘
     │     │           │
     │   dirty         pinned by running query (can't evict)
     │
     └── written to disk by bgwriter/checkpointer

  Clock-sweep eviction:
  A "clock hand" sweeps descriptors in a circle.
  Each pass decrements usage_count.
  When usage_count hits 0 and the page is unpinned, it is a victim candidate.
```

**Clock-sweep** is simpler than LRU: no linked-list operations on every access. Each page descriptor has a `usage_count` (capped at 5). On access, increment it. On eviction sweep, decrement it; if zero, evict. Pages accessed repeatedly accumulate count and survive multiple sweeps.

The buffer manager maintains a **hash table** from `(database_oid, relation_oid, block_number)` to buffer slot, so lookups are O(1). A **buffer lock** (lightweight shared/exclusive lock) protects each slot against concurrent access.

### 3.2 Heap and Tuple Layout

A PostgreSQL table is a **heap file**: pages in no particular order, rows inserted wherever free space exists. Each 8KB page looks like:

```
 ┌────────────────────────────────────────────────── 8192 bytes ─┐
 │ PageHeader (24 B)  │ ItemId array (4B each)  │                  │
 │ pd_lsn, pd_lower,  │ item[0] item[1] item[2] │   free space     │
 │ pd_upper, pd_flags │  (offset+length pairs)  │                  │
 │                    │                          │ tuple[2] tuple[1]│ tuple[0]│
 └───────────────────────────────────────────────────────────────┘
   ItemIds grow downward ─────────────────────────── Tuples grow upward ─┘
```

Each tuple (row) in the page begins with a **tuple header** containing:
- `t_xmin`: XID of the transaction that inserted this tuple
- `t_xmax`: XID of the transaction that deleted/updated this tuple (0 if live)
- `t_ctid`: physical location `(page, item)` — points to the newest version of itself after an UPDATE
- Null bitmap, attribute offset array, then the actual column data

This layout is central to MVCC.

### 3.3 MVCC — Snapshot Isolation via Tuple Visibility

When a transaction starts, it takes a **snapshot**: a record of which transaction IDs are currently active. The snapshot is essentially `{xmin, xmax, active_xids[]}`:
- Any tuple with `t_xmin >= xmax_snap` is too new — invisible.
- Any tuple with `t_xmin` in `active_xids` is uncommitted — invisible.
- A tuple is visible if its `t_xmin` is committed and its `t_xmax` is either zero, or uncommitted, or committed-but-after-snapshot.

```
  Snapshot at time T = (xmin=10, xmax=15, active={12,13})

  Tuple A: t_xmin=8,  t_xmax=0   → visible   (committed before, still live)
  Tuple B: t_xmin=11, t_xmax=0   → visible   (committed, 11 < 15 and not active)
  Tuple C: t_xmin=12, t_xmax=0   → invisible (active — uncommitted)
  Tuple D: t_xmin=8,  t_xmax=11  → invisible (deleted by committed txn 11, which was before snapshot end)
  Tuple E: t_xmin=8,  t_xmax=16  → visible   (deleted by future txn — we don't see the delete yet)
```

**UPDATE in PostgreSQL** = `INSERT` a new tuple + set `t_xmax` on the old one. The old tuple stays in the heap until VACUUM reclaims it. This is why MVCC produces "dead tuples" that bloat tables if not cleaned up.

**VACUUM** walks the heap, finds tuples where `t_xmax` is committed and no snapshot can possibly need the old version, removes them, and updates the **Free Space Map** so their space can be reused. `autovacuum` runs this in the background automatically.

### 3.4 B-Tree Index (`nbtree`)

PostgreSQL's B-tree indexes (`src/backend/access/nbtree/`) implement Lehman-Yao concurrent B-trees (1981), which allow concurrent inserts and searches without coarse-grained locking by linking sibling pages at each level:

```
                  [ 40 | 80 ]             ← root (internal page)
                /     |      \
          [10|20|30] [50|60|70] [90|100]  ← leaf pages
               ↔          ↔         ↔     ← sibling links (doubly linked)
```

Each index page has a **high key**: the largest key that can appear on this page. When a search descends and finds that the key it is looking for exceeds the high key (because a concurrent split just happened), it follows the right-sibling link instead of going back up. This is the Lehman-Yao "link trick."

**Page split on insert:**
1. Split the full leaf into two halves.
2. Insert the new pivot into the parent (may trigger recursive splits upward).
3. The new right sibling is linked in before the parent pointer is updated, so concurrent readers following the sibling link still find the correct page.

**Index-only scan:** if all columns needed by the query are present in the index (whether naturally or via `INCLUDE`), the executor never touches the heap. But it still must check the **Visibility Map** (a one-bit-per-page bitmap) to confirm the page has no dead tuples, otherwise it cannot trust the index entry without going to the heap.

### 3.5 WAL (Write-Ahead Logging)

The WAL is a sequential append-only log of change records, each identified by a **Log Sequence Number** (LSN, a byte offset in the log stream).

```
  WAL record structure:
  ┌──────────────┬──────────────┬──────────────────────────────────┐
  │ xl_tot_len   │ xl_xid       │ xl_info + rm_id + lsn + data …   │
  │ (total bytes)│ (transaction)│ resource manager + change payload │
  └──────────────┴──────────────┴──────────────────────────────────┘

  WAL file stream:
  … 000000010000000000000042  000000010000000000000043 …
           LSN segment                LSN segment
```

**Rule:** a dirty page may not be written to disk until its WAL record has been flushed to disk (the "WAL-before-data" invariant). This guarantees that crash recovery can always replay the log to reconstruct any committed page.

**Crash recovery:**
1. Find the latest checkpoint record in `pg_control`.
2. Replay all WAL records after the checkpoint LSN, in order, re-applying every change to the data files.
3. Any transaction that was active at crash time had no COMMIT record → it is rolled back using the WAL (which includes enough information to undo).

**`innodb_flush_log_at_trx_commit` equivalent in Postgres:** `synchronous_commit`. At `on` (default), WAL is fsynced at COMMIT. At `off`, commits return immediately but the last ~200ms of committed transactions may be lost on a hard crash.

### 3.6 Query Planner and Statistics

The planner is a **cost-based optimizer**: it enumerates possible join orders and access methods, estimates the cost of each (in "page I/O units"), and picks the cheapest.

Estimates depend on `pg_statistic` / `pg_stats`, populated by `ANALYZE`:
- `n_distinct`: estimated number of unique values (used to estimate selectivity of `=` predicates)
- `null_frac`: fraction of rows that are NULL
- `correlation`: correlation of physical vs. logical order (affects index-vs-seq-scan decision)
- `most_common_vals` / `most_common_freqs`: exact counts for frequent values
- Histogram buckets for range predicates

A concrete example of how statistics drive the plan:

```sql
EXPLAIN ANALYZE
  SELECT c.name, COUNT(*) 
  FROM orders o JOIN customers c ON o.customer_id = c.customer_id
  WHERE c.country = 'India'
  GROUP BY c.name;
```

If `pg_stats` shows `country = 'India'` has selectivity 0.05 (5% of customers), the planner estimates 2500 matching rows from a 50K-row table. It compares:
- Hash join customers→orders with a 2500-row build side (fits in `work_mem`? ✓ → cheap)
- Nested loop with index scan on `orders(customer_id)` per customer (2500 × log N)

The planner picks whichever has lower total cost. If the estimate is wrong (say, `India` is actually 30% of rows and 15000 not 2500), the plan may be suboptimal — this is the fundamental limitation of cost-based planning with estimated statistics.

---

## 4. Design Trade-Offs

**MVCC vs. locking.** MVCC gives readers a consistent snapshot without blocking. The cost is dead tuple accumulation and the VACUUM maintenance burden. On read-heavy workloads with many concurrent SELECTs, MVCC wins clearly. On write-heavy workloads with long-running transactions, old snapshots hold back VACUUM and bloat grows.

**Heap (unordered) storage vs. clustered storage.** PostgreSQL's heap means inserts are fast (append to any page with free space, no B-tree splits for the table itself). Range scans on a non-indexed column need a full table scan. InnoDB's clustered index stores rows in PK order — range scans by PK are dramatically faster, but random PK inserts cause splits and fragmentation.

**One process per connection.** Process isolation means a crash in one session cannot corrupt shared memory. But each process costs ~5–10 MB of RAM and takes ~1ms to fork. At 500+ concurrent connections, the per-process overhead becomes significant, which is why PgBouncer (connection pooler) is standard in high-connection environments.

**B-tree heap-fetch vs. covering index.** A secondary index lookup always requires a heap fetch to get columns not in the index (unless it is a covering/index-only scan). This is two I/Os (index page → heap page) instead of one. For write-heavy tables, adding `INCLUDE` columns to indexes trades index bloat for eliminating heap fetches.

**Statistics quality vs. maintenance cost.** Better statistics = better plans. But `ANALYZE` takes time and a share of table I/O. `autovacuum` runs `ANALYZE` too. On a rapidly changing table, statistics lag behind reality, and the planner sees stale estimates. Tuning `default_statistics_target` per column trades `ANALYZE` cost for plan quality.

---

## 5. Experiments / Observations

**(a) Seeing dead tuples with MVCC.**

After running 10K updates on a table, `SELECT n_dead_tup FROM pg_stat_user_tables` shows a large number of dead tuples. Running `VACUUM` and re-checking shows the count drop to near zero. This directly demonstrates that MVCC produces garbage that must be explicitly collected.

**(b) Clock-sweep eviction under memory pressure.**

With a small `shared_buffers` (e.g., 16 MB) and a large sequential scan, `pg_stat_bgwriter.buffers_clean` rises sharply — the bgwriter is evicting pages to make room. Increasing `shared_buffers` reduces `buffers_clean`. The clock-sweep counter (`usage_count`) means pages accessed even once survive one sweep, so a hot working set stays warm even under moderate scan pressure.

**(c) Index-only scan and Visibility Map.**

On a freshly loaded, fully VACUUMed table, `EXPLAIN (ANALYZE, BUFFERS)` on a covering-index query shows `Heap Fetches: 0` — the Visibility Map marks every page as all-visible, so the executor trusts the index and skips the heap entirely. After bulk-inserting rows (making some pages "not all-visible"), the same query shows non-zero `Heap Fetches` until VACUUM runs again.

**(d) WAL volume per operation.**

A single `UPDATE` on a 100-byte row generates approximately 200–300 bytes of WAL (header + old + new values, depending on whether full-page images are written). At `wal_log_hints = on` and after a checkpoint, the first write to any page generates a Full Page Image in WAL — the entire 8KB page — to enable recovery even if the original page was partially written. This is why WAL size can spike after a checkpoint.

**(e) Plan sensitivity to statistics.**

Running `EXPLAIN` before and after `ANALYZE` on a freshly loaded table shows dramatically different cost estimates. Before `ANALYZE`, the planner defaults to assuming `n_distinct = 200` (a generic guess), producing suboptimal plans like sequential scans where an index would be much faster. After `ANALYZE`, correct `n_distinct` values lead the planner to choose the index.

---

## 6. Key Learnings

1. **The heap-WAL-buffer-manager triangle is inseparable.** You cannot understand any one of them in isolation. The buffer manager holds dirty pages; WAL records changes before those pages hit disk; the heap is where the pages live. Remove any leg and the ACID guarantees collapse.

2. **MVCC's "invisible" cost is VACUUM.** The concurrency benefit of never overwriting old tuples is real and large. But it shifts the cost elsewhere: dead tuples accumulate, bloat tables, slow scans, and must be reclaimed. Understanding when VACUUM falls behind — and why — is one of the most important operational skills for a PostgreSQL DBA.

3. **Statistics determine plan quality more than indexes alone.** An index exists; whether the planner uses it depends on its estimate of selectivity, which comes from `pg_statistic`. A badly out-of-date statistics table can make the planner choose a sequential scan over an index even when the index would be 100× faster.

4. **WAL is the source of truth, not the heap files.** After a crash, the heap files are in an unknown state. WAL is what makes them trustworthy again. Streaming replication, point-in-time recovery, and logical decoding are all built on top of the same WAL stream — it is the backbone of PostgreSQL's reliability story.

5. **B-tree concurrency (Lehman-Yao) makes index maintenance cheap.** The sibling-link trick means concurrent inserts and reads on the same index level do not require locking the entire tree — only individual page locks are needed. This is why PostgreSQL can sustain thousands of indexed writes per second without index-level contention.

---

## References

- PostgreSQL 16 Documentation: [Buffer Manager](https://www.postgresql.org/docs/current/runtime-config-resource.html#RUNTIME-CONFIG-RESOURCE-MEMORY), [MVCC](https://www.postgresql.org/docs/current/mvcc.html), [WAL](https://www.postgresql.org/docs/current/wal.html), [B-Tree indexes](https://www.postgresql.org/docs/current/btree.html), [pg_statistic](https://www.postgresql.org/docs/current/catalog-pg-statistic.html)
- Lehman, P. L. & Yao, S. B. (1981). *Efficient Locking for Concurrent Operations on B-Trees.* ACM TODS.
- PostgreSQL source: `src/backend/storage/buffer/`, `src/backend/access/nbtree/`, `src/backend/access/transam/`
