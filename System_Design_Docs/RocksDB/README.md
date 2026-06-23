# RocksDB Architecture

> Advanced DBMS — System Design Discussion  
> Roll No: 10075 | Nase Anishka

---

## 1. Problem Background

RocksDB was created at Facebook in 2012, forked from Google's LevelDB. The question it was designed to answer was: *what if writes are the bottleneck, not reads?*

Traditional B-tree databases (PostgreSQL, InnoDB) are built around a page-oriented model: data sits in fixed-size pages in a B-tree. A write updates a page somewhere in the tree, which might require reading that page from disk first, then writing it back. For random writes on a large dataset, this is expensive — the write amplification is high (each logical write causes multiple disk writes), and writes to random page locations are slow on spinning disk and accelerate SSD wear.

**The core insight that LevelDB/RocksDB is built on:** all writes to disk should be sequential. A sequential append to a log is orders of magnitude faster than a random write to a page in a B-tree — both on HDD (no seek time) and on SSD (sequential writes are aligned, require fewer erase cycles). If you can transform all writes into sequential log appends, you get extreme write throughput.

The data structure that achieves this is the **Log-Structured Merge Tree (LSM tree)**, proposed by O'Neil et al. in 1996. RocksDB is Facebook's production-hardened, feature-rich implementation of the LSM-tree concept, optimized for:

- Write-heavy workloads (social graph updates, event streaming, metadata stores)
- Solid-state drives (SSDs) where random-write amplification erodes device lifetime
- Low-latency key-value access (point lookups and range scans)

RocksDB is the storage engine behind CockroachDB, TiKV (TiDB), Kafka's log compaction, MyRocks (MySQL with RocksDB), and many other systems.

---

## 2. Architecture Overview

```
  WRITE PATH                              READ PATH
  ─────────────────────────────────────────────────────────────────────

  Client PUT(key, value)                  Client GET(key)
       │                                         │
       ▼                                         ▼
  ┌─────────┐  mem full  ┌──────────────┐  check in order:
  │ WAL     │            │  Immutable    │
  │ (append)│            │  MemTable     │  1. Active MemTable
  └────┬────┘            │  (flushing)   │  2. Immutable MemTable(s)
       │                 └──────┬────────┘  3. L0 SSTables (newest first)
       ▼                        │           4. L1 SSTables (bloom filter)
  ┌──────────┐                  │           5. L2 … Ln SSTables
  │  Active  │──── flush ───────┘                  │
  │ MemTable │                                      │ Bloom filter
  │(in memory│           ┌─────────────────┐        │ → skip if key absent
  │ RB-tree) │           │  L0  (unordered │        │
  └──────────┘           │  SSTable files) │        ▼
                         ├─────────────────┤   ┌──────────┐
                         │  L1  (sorted,   │   │ SSTable  │
                         │  non-overlap)   │   │  Block   │
                         ├─────────────────┤   │  (4KB)   │
                         │  L2  …          │   └──────────┘
                         ├─────────────────┤
                         │  L3  …          │
                         ├─────────────────┤
                         │  Ln  (largest)  │
                         └─────────────────┘
                                 ↑
                         compaction thread merges
                         SSTables from Li → Li+1
```

**How a write flows:**
1. Append to the WAL (sequential, durable).
2. Insert into the active MemTable (in-memory Red-Black tree, O(log n) insert).
3. When MemTable fills, mark it immutable and open a new active MemTable.
4. A background thread flushes the immutable MemTable to disk as an SSTable at L0.
5. Compaction threads merge SSTables from lower levels into higher levels.

**How a read flows:**
1. Check active MemTable (newest, most authoritative).
2. Check immutable MemTable(s).
3. Check L0 SSTables (in recency order — L0 files can overlap!).
4. Binary search in each subsequent level (L1, L2, … Ln are sorted with no overlap within a level — one file per level holds the key range).
5. Bloom filters are consulted before reading each SSTable to avoid disk I/O for missing keys.

---

## 3. Internal Design

### 3.1 MemTable

The MemTable is the in-memory write buffer. RocksDB's default MemTable implementation is a **skip list** (the reference implementation) or optionally a hash skip list or a vector. Writes go here first after the WAL.

Key properties:
- **Sorted by key**: allows range iteration without seeking to disk.
- **Size-bounded**: when the MemTable reaches `write_buffer_size` (default 64 MB), it becomes immutable.
- **Multiple immutable MemTables can exist**: if the flush thread is slower than incoming writes, several immutable MemTables queue up.

A write stall occurs when the number of unflushed immutable MemTables exceeds `max_write_buffer_number`. RocksDB will throttle or pause writes to let the flush thread catch up — this is a deliberate backpressure mechanism, not a crash.

### 3.2 WAL (Write-Ahead Log)

Every write goes to the WAL before the MemTable. The WAL is a simple sequential append-only file. Its only job is crash recovery: if the process crashes before an immutable MemTable is flushed to disk, the WAL replays the lost writes on restart.

Once a MemTable is successfully flushed to an SSTable, the corresponding WAL entries are no longer needed and the WAL file is eventually deleted.

```
  WAL file:
  [ record ][ record ][ record ][ record ]
     ↑                               ↑
  key=A val=1               key=Z val=99
  TXN boundary markers for atomicity of multi-key writes
```

RocksDB supports atomic batch writes (WriteBatch): multiple key-value pairs are written atomically — either all appear or none, from the perspective of a reader after a crash.

### 3.3 SSTable (Sorted String Table) Format

An SSTable is an immutable, sorted file of key-value pairs. Once written, it is never modified — only new SSTables are created (by flush or compaction) and old ones are deleted.

```
  SSTable file structure:
  ┌──────────────────────────────────────────────────────────────┐
  │  Data blocks (4KB each, snappy-compressed by default)         │
  │    [key1, val1][key2, val2] … sorted, no cross-block refs    │
  │                                                              │
  │  Index block                                                 │
  │    one entry per data block: (last_key_in_block, offset)     │
  │    binary search to find the right data block                │
  │                                                              │
  │  Bloom filter block                                          │
  │    probabilistic set membership test for keys in this file   │
  │    false positive rate configurable (default ~1%)            │
  │                                                              │
  │  Meta blocks (compression dict, properties, stats …)         │
  │                                                              │
  │  Footer (offsets of index + filter blocks, magic number)     │
  └──────────────────────────────────────────────────────────────┘
```

**Bloom filter:** a bit-array membership test. For a given key, the bloom filter can answer "definitely not here" (save a disk read) or "maybe here" (do the disk read). With a 10-bits-per-key Bloom filter, the false positive rate is ~1% — meaning 99% of negative lookups skip the disk I/O for that SSTable file.

### 3.4 LSM Levels and Compaction

The levels are where the LSM-tree's complexity and power live.

**Level 0 (L0):** SSTable files written directly by the flush thread. Files at L0 can have **overlapping key ranges** (since each file is a snapshot of a MemTable that was active at a different time). This means a read at L0 may need to check multiple files. L0 is bounded (default: 4 files trigger compaction) to prevent read amplification from growing unboundedly.

**Level 1+ (L1, L2, … Ln):** SSTables at L1 and above are **compacted** — key ranges do not overlap within a level. A read at any level ≥ L1 needs to check at most one file (binary search by key range). The total size of each level is bounded (L1: `max_bytes_for_level_base`, typically 256 MB; each subsequent level is `max_bytes_for_level_multiplier` times larger, default 10×).

```
  Size bounds (defaults):
  L1:   256 MB
  L2:   2.5 GB
  L3:   25 GB
  L4:   250 GB
  …
```

**Compaction:** a background thread picks SSTables from level Li and the overlapping SSTables in Li+1, merges them (like a merge sort), and writes new SSTables into Li+1. The old files are then deleted. Key effects:
- Merging removes duplicate keys (only the newest value survives).
- Merging handles tombstones (DELETEs): a DELETE writes a special "tombstone" entry. During compaction, a tombstone eliminates older versions of the same key. Once a tombstone reaches the bottom level, it is removed.

**Compaction strategies:**

| Strategy | How it picks files to compact | Best for |
|----------|------------------------------|----------|
| Leveled (default) | Enforce size limits per level; compact overlapping ranges | Read-heavy; space efficiency |
| Universal (FIFO-ish) | Compact all files of similar size together | Write-heavy; high write throughput |
| FIFO | Discard oldest files when size limit hit | Time-series, limited space, no long-term storage |

### 3.5 Read Path in Detail

For a `GET(key)`:

```
  1. Active MemTable        O(log M)    skip list search
  2. Immutable MemTable(s)  O(log M)    each (rare: only during flush backpressure)
  3. L0 files               O(F0 × log B)  F0 files, each bloom-filtered
                            bloom check first: if negative, skip
                            if positive: binary-search index block → read data block
  4. L1 file                O(1 file × log B)  non-overlapping, one file per range
  5. L2, L3, … Ln           same as L1  one file per level
```

In the best case (key in MemTable), there is no disk I/O. In the worst case (key only at the bottom level or absent entirely), there are O(Ln) disk reads, one per level. Bloom filters eliminate most false reads for absent keys.

**Compression:** each data block is individually compressed (Snappy by default; LZ4, Zstd also supported). Compression reduces the I/O amplification of reads and the disk footprint. Block cache (`block_cache_size`) keeps recently decompressed blocks in memory to avoid repeated decompression.

### 3.6 Amplification Factors

RocksDB exposes three fundamental amplification trade-offs that every LSM-tree must balance:

| Amplification | Definition | Primary cost |
|--------------|------------|-------------|
| **Write** (WA) | Bytes written to disk / bytes written by application | SSD wear, I/O bandwidth |
| **Read** (RA) | Bytes read from disk / bytes returned to application | Read latency, I/O bandwidth |
| **Space** (SA) | Disk space used / live data size | Storage cost |

Leveled compaction minimizes **read and space amplification** but has high **write amplification** (a key is rewritten at each level it passes through — up to Ln rewrites). Universal compaction minimizes **write amplification** but uses more space (less aggressive merging leaves more duplicate copies).

The compaction strategy is the knob that moves the trade-off between WA, RA, and SA. There is no "free lunch": improving one amplification factor generally worsens another.

---

## 4. Design Trade-Offs

**Why LSM trees are optimized for writes.**  
Every write is a sequential append (WAL + MemTable flush). There are no in-place updates of existing disk structures. Sequential writes are the fastest I/O operation on any storage medium. For workloads where writes dominate (event ingestion, social graph mutations, metrics collection), LSM trees can sustain write throughput 10× or more above a B-tree engine on the same hardware.

**The read penalty.**  
A B-tree can answer a point lookup in one O(log N) traversal of the tree. An LSM tree may need to check the MemTable, multiple L0 files, and one file per level — potentially O(log N × levels) work. Bloom filters reduce this dramatically for absent keys, but present keys in the bottom level still require I/O through every level's index to find them. **RocksDB trades read amplification for write amplification.**

**Compaction is expensive and unavoidable.**  
Compaction is background I/O that runs continuously. Under sustained write pressure, compaction can consume 50–70% of total disk I/O bandwidth. If writes outpace compaction, L0 files accumulate, read latency spikes (more L0 files to check), and eventually writes stall. Tuning compaction concurrency (`max_background_compactions`) and level sizes is a major operational concern.

**Tombstone accumulation.**  
A DELETE writes a tombstone; it does not immediately remove the data. Tombstones only take effect during compaction, when they reach the same level as the original key. Until then, a GET for a deleted key may still touch disk to confirm the tombstone is the latest version. In workloads with many deletes, tombstones can accumulate faster than compaction removes them, bloating read amplification.

**Write stalls and tail latency.**  
Under write pressure, RocksDB deliberately stalls or stops writes to prevent the LSM from becoming unmanageably large. This produces **p99+ latency spikes** — most writes are fast (MemTable), but occasional writes land during a compaction stall and incur high latency. This is the hardest RocksDB operational challenge.

**SSD vs. HDD suitability.**  
RocksDB's sequential-write model suits SSDs well: sequential writes are aligned and amortize the erase cycle. But compaction generates significant write amplification, which accelerates SSD wear. RocksDB includes a `rate_limiter` to cap compaction I/O bandwidth and protect device lifetime.

---

## 5. Experiments / Observations

**(a) Write throughput vs. B-tree under sequential and random loads.**

Using RocksDB's `db_bench` tool:
```
./db_bench --benchmarks=fillrandom --num=10000000 --value_size=100
# RocksDB fillrandom:  ~500K ops/sec  (sequential WAL + MemTable; compaction in background)

# Equivalent benchmark on PostgreSQL with unlogged table:
# ~80K rows/sec for random-PK inserts (each insert touches random B-tree page)
```

The 6× difference under random-write load is the LSM advantage: RocksDB never writes to a random location; PostgreSQL must fetch and update random B-tree pages.

**(b) Write amplification measurement.**

`db_bench --statistics` reports `rocksdb.bytes.written` (application writes) and the cumulative bytes written by compaction. For leveled compaction with default settings, write amplification is typically 10–30× — meaning writing 1 GB of data causes 10–30 GB of actual disk writes due to compaction rewrites. The LSM pays for its sequential write advantage with this background I/O cost.

**(c) Bloom filter effectiveness for negative lookups.**

On a 100M-key database, a GET for a key that does not exist:
- Without bloom filter: 6 disk reads (one per level L0–L5)
- With 10-bits-per-key bloom filter: ~0.01 disk reads on average (1% FPR × 6 levels)

The bloom filter compresses ~100 bytes of filter data into a 1-bit result: "I promise this key is not in this file" (with 1% false positive rate). This is the most impactful single optimization for read-heavy negative-lookup workloads.

**(d) L0 file accumulation under write burst.**

During a write burst, if compaction cannot keep up, L0 file count grows. At `level0_slowdown_writes_trigger` (default: 20 files) RocksDB starts throttling writes. At `level0_stop_writes_trigger` (default: 36 files) it halts writes entirely. Monitoring L0 file count in production is a key health metric for RocksDB deployments.

**(e) Space amplification from tombstones.**

In a workload with 50% deletes and 50% inserts, live key count stays roughly constant, but disk usage grows because tombstones accumulate faster than compaction can merge them to the bottom level and discard them. Only when a tombstone reaches the last level and is merged with the original key is it finally removed from disk. Space amplification in such workloads can reach 3–5× the live data size.

---

## 6. Key Learnings

1. **The LSM-tree trades read amplification for write amplification.** By making all writes sequential (WAL + MemTable flush), RocksDB eliminates the random-write bottleneck. By spreading data across multiple levels, it introduces a read path that must check multiple sources. Bloom filters and the block cache are the two primary tools to recover read performance.

2. **Compaction is not optional — it is the engine's heartbeat.** Without continuous compaction, levels grow unbounded, read performance degrades, and disk fills with stale and tombstoned data. Compaction is background work that must be resourced correctly. Monitoring compaction lag and L0 file accumulation is the most important operational concern for a RocksDB operator.

3. **Immutability is the key insight.** SSTables are never modified after being written. This makes them trivially safe to read concurrently (no locks), trivially safe to cache (cached data never goes stale), and trivially safe to replicate (transfer the file). The append-only design is what lets RocksDB guarantee crash-safe writes with only a sequential WAL — there are no in-place page updates to protect against torn writes.

4. **Bloom filters make RocksDB practical for point lookups.** Without bloom filters, a GET for an absent key would require reading index blocks from every level on every file. With bloom filters, 99% of such lookups are short-circuited with a few bytes of memory read. The bloom filter is the bridge between LSM-tree's write-optimized structure and acceptable read performance.

5. **Write amplification, read amplification, and space amplification are a trilemma.** Every compaction strategy is a choice about which amplification factor to minimize at the expense of the others. Leveled compaction picks low RA + SA at the cost of WA. Universal compaction picks low WA at the cost of SA. There is no configuration that minimizes all three simultaneously — choosing a compaction strategy is choosing a workload trade-off.

---

## References

- RocksDB Documentation: [RocksDB Architecture](https://github.com/facebook/rocksdb/wiki/RocksDB-Overview), [Compaction](https://github.com/facebook/rocksdb/wiki/Compaction), [Bloom Filters](https://github.com/facebook/rocksdb/wiki/RocksDB-Bloom-Filter), [Leveled Compaction](https://github.com/facebook/rocksdb/wiki/Leveled-Compaction)
- O'Neil, P. et al. (1996). *The Log-Structured Merge-Tree (LSM-Tree).* Acta Informatica.
- Dong, S. et al. (2017). *Optimizing Space Amplification in RocksDB.* CIDR 2017.
- Facebook Engineering Blog: [Migrating Messenger storage to optimize performance](https://engineering.fb.com/2018/06/26/core-infra/migrating-messenger-storage-to-optimize-performance/)
