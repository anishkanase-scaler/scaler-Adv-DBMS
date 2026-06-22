# LAB 4 — RED-BLACK TREE & FULL B-TREE IN C++

> Roll Number: **10075** &nbsp;&nbsp;|&nbsp;&nbsp; Name: **Nase Anishka**

This lab implements two data structures at the heart of database indexing: a **Red-Black Tree** (in-memory self-balancing BST used by `std::map` and PostgreSQL's in-memory structures) and a **full B-Tree** (the on-disk index structure used by PostgreSQL, MySQL, and SQLite), with complete insert, split, borrow, merge, and delete.

---

## WHAT I BUILT

### Part 1 — Red-Black Tree
A self-balancing BST guaranteeing O(log n) height via four invariants:
1. Every node is Red or Black.
2. Root is always Black.
3. No two consecutive Red nodes (a Red node's parent must be Black).
4. Every path from any node to its NULL leaves has the same number of Black nodes.

Operations: `insert` (with `fix_insert` — recolor / rotate), `remove` (with `fix_delete` — double-black repair), inorder traversal showing key + color.

### Part 2 — Full B-Tree (minimum degree T = 2)
A B-Tree where each internal node holds `T-1` to `2T-1` keys and `T` to `2T` children. With T=2 this is a 2-3-4 tree. PostgreSQL uses the same structure with T sized to fill one 8 KB page (~128 keys per node).

Operations:
- `insert` — split-on-the-way-down, promotes median to parent
- `search` — O(log_T n) walk
- `remove` — three cases: leaf delete, internal-node predecessor/successor replace, underflow `fill` (borrow-left / borrow-right / merge)

---

## FILES IN THIS FOLDER

| File | Purpose |
|------|---------|
| `main.cpp` | Red-Black Tree (Part 1) + B-Tree (Part 2) implementation |
| `CMakeLists.txt` | CMake build config |
| `.gitignore` | Ignore build artefacts |
| `run_output.txt` | Captured output from a real run |
| `README.md` | This file |

---

## HOW TO BUILD AND RUN

```bash
# Direct g++
g++ -std=c++17 -Wall -o rbtree_btree main.cpp && ./rbtree_btree

# CMake
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/rbtree_btree
```

---

## PART 1: RED-BLACK TREE — DEEP DIVE

### Rotations
Left-rotate pivots on x; right-rotate mirrors it. Both run in O(1) — three pointer rewires.

```
    x                y
   / \    →         / \
  A   y            x   C
     / \          / \
    B   C        A   B
```

### fix_insert — 3 cases per side
After inserting a new RED node z:

| Case | Condition | Action |
|------|-----------|--------|
| 1 | Uncle is RED | Recolor parent + uncle BLACK, grandparent RED, move z up |
| 2 | Uncle is BLACK, z is inner child | Rotate parent, fall through to Case 3 |
| 3 | Uncle is BLACK, z is outer child | Rotate grandparent, swap colors |

### fix_delete — double-black repair
When a BLACK node is removed, a "double-black" deficit propagates upward through 4 symmetric cases until absorbed by a RED node or the root.

### Demo output
```
Inorder after inserts (key + R/B color):
1R 5B 10R 15B 20B 25R 30B
After removing 20:
1R 5B 10R 15B 25B 30B
After removing 10:
1B 5R 15B 25B 30B
```

Inorder is always sorted (BST property). Colors shift after each remove to restore RB invariants.

---

## PART 2: FULL B-TREE — DEEP DIVE

### Why B-Trees for databases
An RB-Tree stores 1 key per node — n keys need n pointer dereferences. A B-Tree with T=128 (PostgreSQL's page-filling fanout) fetches 255 keys per disk read. A 16M-row table needs height ≤ 3.

### Split on the way down (insert)
Before descending into a full child, split it immediately so the parent always has room for the promoted median:

```
Parent: [10, 30]           →   Parent: [10, 20, 30]
Child:  [15, 17, 20, 25]       Left:   [15, 17]   Right: [25]
                               (median 20 promoted)
```

### Delete — three cases

**Case A — key is in a leaf:** direct erase.

**Case B — key is in an internal node:**
- Left child has ≥ T keys → replace with in-order predecessor, delete predecessor from left child.
- Right child has ≥ T keys → replace with in-order successor.
- Both children have T-1 keys → merge, then delete from merged child.

**Case C — key is not in this node:**
`fill()` ensures child has ≥ T keys before descending:
- Borrow from left sibling via parent separator key
- Borrow from right sibling via parent separator key
- Merge with sibling (consuming a parent key)

### Demo output
```
Inorder after inserts:
1 3 5 6 7 10 12 17 20 25 30
Search 17: found
Search 99: not found
Inorder after removing 6:
1 3 5 7 10 12 17 20 25 30
Inorder after removing 20:
1 3 5 7 10 12 17 25 30
Inorder after removing 3 and 1:
5 7 10 12 17 25 30
```

Inorder stays sorted through all operations — each remove triggers the correct borrow or merge.

---

## RED-BLACK TREE vs B-TREE

| Property | Red-Black Tree | B-Tree (degree T) |
|----------|----------------|-------------------|
| Storage | In-memory | Designed for disk (node = one page) |
| Node size | 1 key | Up to `2T-1` keys |
| Height | O(log₂ n) | O(log_T n) — much shorter for large T |
| Cache friendly | Poor (pointer chasing) | Excellent (sequential keys per node) |
| DB use | In-memory maps, `std::map` | On-disk indexes (PostgreSQL, MySQL, InnoDB) |
| Balance mechanism | Color + rotation | Child split / sibling borrow / merge |

PostgreSQL B-Tree index pages are 8 KB — exactly the `page_size` from Lab 2. One disk I/O fetches an entire node.

---

## KEY TAKEAWAYS

- Red-Black Trees maintain balance via color + rotation. Four invariants guarantee height ≤ 2 log₂(n+1).
- B-Trees minimize disk I/O by packing many keys into one node per page read. Higher T → shorter tree → fewer seeks.
- B-Tree delete has three cases: leaf erase, internal-node predecessor/successor replace, and underflow borrow/merge. These run thousands of times per second on every indexed write in a real database.
- The `fill` helper (borrow-left / borrow-right / merge) ensures the descend never underflows the target child.
