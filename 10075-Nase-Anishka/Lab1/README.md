# LAB 1 — FILE HANDLING USING SYSTEM CALLS

> Roll Number: **10075** &nbsp;&nbsp;|&nbsp;&nbsp; Name: **Nase Anishka**
>
> A small C program that does file I/O with the **raw Linux/POSIX system
> calls** — `open`, `read`, `write`, `close` — and nothing from stdio
> (`fopen`/`fread`/`fwrite`) in between. It opens `file.txt` read-only,
> drains it in 512-byte chunks while counting bytes, closes it, reopens
> it in **append** mode, writes one line with `write()`, and closes it
> again. Every single system-call return value is checked and reported
> with `perror()`, including the short-read, short-write, and `EINTR`
> cases that the textbook one-liners quietly ignore.

---

# WHY THIS MATTERS FOR A DB COURSE

Everything a database does to durable storage eventually bottoms out in
exactly these four calls (plus their cousins `pread`/`pwrite`/`fsync`/
`lseek`). The whole point of this course's later labs — the buffer pool,
the page cache, the on-disk B-tree — is to sit *on top of* `read()` and
`write()` and amortise them.

* A buffer pool is a cache over `pread`/`pwrite` against the data file's
  file descriptor. The eviction labs only matter because the calls
  underneath are slow.
* **`O_APPEND`** is exactly how a write-ahead log (WAL) is implemented:
  the kernel guarantees each record lands at end-of-file *atomically*,
  so the log never overwrites itself even under concurrency.
* The return value of `write()` being a *count*, not a boolean, is why
  durability is hard — a "successful" `write()` only copies into the
  kernel page cache; getting bytes onto the platter needs `fsync()`.
* `read()` returning **fewer bytes than asked** (a short read) is the
  same reason a storage engine never assumes one call fills its buffer
  — it loops. This lab loops too, on purpose.

So Lab 1 is the floor that the rest of the term is built on: if you
don't trust your `read`/`write` handling, nothing above it is reliable.

---

# THE SYSTEM CALLS USED

| Call      | Header      | Purpose                                            | Return value |
|-----------|-------------|----------------------------------------------------|--------------|
| `open()`  | `<fcntl.h>` | Open/create a file, hand back a file descriptor    | `fd` ≥ 0, or `-1` |
| `read()`  | `<unistd.h>`| Copy up to *n* bytes from the fd into a buffer     | bytes read (`0` = EOF), or `-1` |
| `write()` | `<unistd.h>`| Copy up to *n* bytes from a buffer to the fd       | bytes written, or `-1` |
| `close()` | `<unistd.h>`| Release the fd and its kernel resources            | `0`, or `-1` |

A **file descriptor** is just a small non-negative integer that indexes
into the kernel's per-process open-file table. `0`, `1`, `2` are always
stdin/stdout/stderr, so the first file you open is almost always **fd 3**
— which is exactly what the sample output below shows.

---

# FILES IN THIS FOLDER

* `file_handling.c` — the program (≈ 130 lines, heavily commented).
* `CMakeLists.txt` — C11 build with `-Wall -Wextra -Wpedantic`.
* `file.txt` — the sample input the program reads. **Note:** running the
  program appends a line to this file, so after a run it will have grown
  by one line — that is the lab working, not a bug. The copy committed
  here is the pristine "before" state.
* `run_output.txt` — captured stdout from a real first run against the
  pristine `file.txt`, so the output is visible without rebuilding.
* `.gitignore` — build artefacts.
* `README.md` — this file.

---

# BUILD AND RUN

```bash
cd 10075-Nase-Anishka/Lab1
cmake -B build -S .
cmake --build build
./build/file_handling
```

Or, without CMake, the classic one-liner:

```bash
gcc -Wall -Wextra -Wpedantic -o file_handling file_handling.c
./file_handling
```

Run it **from this directory** so the relative path `"file.txt"`
resolves. These are POSIX calls, so it also compiles and runs on macOS
(which is how the sample output was captured) — `fcntl.h`/`unistd.h`
exist on every Unix.

---

# WHAT THE PROGRAM DOES, STEP BY STEP

1. **Open for reading** — `open("file.txt", O_RDONLY)`. `O_RDONLY` does
   *not* create the file, so a missing file is a hard error here; the
   program prints a hint and exits.
2. **Read in 512-byte chunks** — a `while` loop calls `read()` until it
   returns `0` (EOF). Each chunk is echoed to the terminal with `fwrite`
   and added to a running byte total. Looping (instead of one big read)
   is what makes the program correct for files bigger than the buffer
   and resilient to short reads.
3. **Close** the read descriptor with `close()`.
4. **Reopen in append mode** — `open(..., O_WRONLY | O_APPEND | O_CREAT, 0644)`.
   `O_APPEND` is the important flag: it makes the kernel seek to
   end-of-file before *every* write, atomically, so existing data is
   never clobbered.
5. **Append a line** with `write()`, via a `write_all()` helper that
   loops until all bytes are written.
6. **Close** the write descriptor.

---

# SAMPLE OUTPUT

(from `run_output.txt`, first run against the pristine `file.txt`)

```text
opened 'file.txt' for reading      (fd = 3)

----- file contents -----
Lab 1 — File Handling Using System Calls in Linux.
This file is read by file_handling.c using open() and read().
Each run of the program appends one more line below using write().

-------------------------
total bytes read: 182
closed the read descriptor

reopened 'file.txt' in append mode (fd = 3)
appended 66 bytes: This line was appended with the write() system call (roll 10075).
closed the write descriptor
```

`total bytes read: 182` even though the three lines look like fewer than
182 characters — that's because the em-dash `—` is a 3-byte UTF-8
sequence. `read()` deals in **bytes, not characters**, which is itself a
useful thing to see. The descriptor is `3` both times because the read
descriptor (also 3) was closed before the append `open()`, freeing the
slot.

---

# IMPLEMENTATION NOTES

## I loop `read()` instead of calling it once

The naive version is `read(fd, buf, 512)` once and assume it returned
the whole file. That's wrong twice over: a file can be larger than the
buffer, and even for a small file `read()` is *permitted* to return
fewer bytes than available (e.g. when reading from a pipe or socket, or
when interrupted). Looping until `read()` returns `0` is the only
correct way, and it's the same loop a real storage engine uses.

## `write_all()` handles short writes

Symmetrically, `write()` may write fewer bytes than requested. For a
66-byte string it essentially never will — but writing the loop anyway
(`write_all`) is the habit that matters, because the day it *does*
short-write (a full pipe, a signal) the naive code silently drops data.

## `EINTR` is retried, not treated as an error

If a signal arrives mid-call, `read`/`write` can return `-1` with
`errno == EINTR` having done no work. That is *not* a real failure, so
both loops `continue` on `EINTR` and retry. Treating `EINTR` as fatal is
one of the most common real-world file-I/O bugs.

## `fwrite` to display, not `printf("%s", buffer)`

`read()` does **not** NUL-terminate the buffer, so printing it as a C
string with `%s` would read past the data. I echo the exact `n` bytes
with `fwrite(buffer, 1, n, stdout)` instead. (Sharing stdout's stdio
stream with the `printf()` status lines also keeps everything ordered;
mixing a raw `write(STDOUT_FILENO, …)` with buffered `printf` can
interleave out of order.)

## System calls vs. the stdio library

`open/read/write/close` are **system calls** — they trap directly into
the kernel and are unbuffered. `fopen/fread/fwrite` are **library
functions** that wrap them and add a userspace buffer to cut the number
of kernel crossings. For this lab the unbuffered calls are the point;
in real code you pick stdio for convenience and the raw calls when you
need control over exactly when bytes cross into the kernel (which is the
entire game in a database).

---

# OBSERVATIONS

* The OS hands out a fresh, unique file descriptor on each `open()`
  (here, `3` — the first slot after stdin/stdout/stderr).
* Data is read straight off the descriptor into a buffer with no stdio
  layer in the way.
* `O_APPEND` preserves existing content: every run adds a line at the
  end and never overwrites — visible by running the program twice and
  watching `file.txt` grow.
* Checking every return value (and reporting via `perror()`, which
  prints the human-readable `errno` string) is what turns a crash into a
  diagnosable message.
* A file descriptor left open is a leaked kernel resource, so each is
  closed as soon as its work is done.

---

# WHAT I LEARNED

* `read()` and `write()` return a **count**, not success/failure, and
  that count can be smaller than you asked for. Internalising that — and
  always looping — is the single most important takeaway, and it's
  exactly why databases never trust a single I/O call.
* A "successful" `write()` only means the bytes reached the kernel's
  page cache, not the disk. Durability needs `fsync()`. That gap is the
  whole reason write-ahead logging and `O_APPEND` exist.
* `O_APPEND` does the end-of-file seek **atomically** in the kernel, so
  it's safe under concurrency in a way that "seek to end, then write"
  from userspace is not. That's a tiny flag with big implications for
  log files.
* `EINTR` is a normal, retryable condition, not an error — I'd never
  have known to handle it from the lab's one-line description alone.
* File descriptors are reused: closing fd 3 and opening again gives back
  fd 3. The number is just the lowest free slot in the table.
* `read()` counts **bytes, not characters** — the multi-byte em-dash
  making the count 182 was a neat, accidental demonstration of that.
