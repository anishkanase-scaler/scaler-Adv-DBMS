/*
 * Lab 1 — File Handling Using System Calls in Linux
 * Roll No: 10075   |   Name: Nase Anishka
 *
 * Low-level file I/O using the raw POSIX/Linux system calls
 *   open(2), read(2), write(2), close(2)
 * with NO stdio buffering (fopen/fread/fwrite) between us and the kernel.
 *
 * The program:
 *   1. opens file.txt read-only and prints the file descriptor,
 *   2. reads the whole file in 512-byte chunks and prints the byte count,
 *   3. closes the read descriptor,
 *   4. reopens file.txt in append mode (O_APPEND),
 *   5. appends one line with write(),
 *   6. closes the write descriptor.
 * Every system call's return value is checked and reported with perror().
 *
 * Build:  cmake -B build -S . && cmake --build build      (then ./build/file_handling)
 *   or:   gcc -Wall -Wextra -Wpedantic -o file_handling file_handling.c
 * Run from this directory so the relative path "file.txt" resolves.
 */

#include <stdio.h>      /* printf, perror, fprintf, fwrite */
#include <stdlib.h>     /* exit codes                      */
#include <string.h>     /* strlen                          */
#include <errno.h>      /* errno, EINTR                    */
#include <fcntl.h>      /* open(), O_* flags               */
#include <unistd.h>     /* read(), write(), close()        */

#define FILENAME     "file.txt"
#define CHUNK_SIZE   512
#define APPEND_TEXT  "This line was appended with the write() system call (roll 10075).\n"

/*
 * read() and write() are allowed to do less work than asked:
 *   - they can return -1 with errno == EINTR if a signal interrupts them,
 *   - they can transfer fewer bytes than requested (a "short" read/write).
 * write_all() wraps write() in the canonical retry/drain loop so the caller
 * can treat it as all-or-nothing. Returns 0 on success, -1 on a real error.
 */
static int write_all(int fd, const char *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, buf + total, len - total);
        if (n == -1) {
            if (errno == EINTR)
                continue;               /* interrupted by a signal — retry */
            return -1;                  /* genuine error, errno is set     */
        }
        total += (size_t)n;
    }
    return 0;
}

int main(void)
{
    char buffer[CHUNK_SIZE];

    /* ---- 1. Open file.txt for reading ------------------------------------ */
    /* O_RDONLY does NOT create the file; if it is missing, open() fails. */
    int fd = open(FILENAME, O_RDONLY);
    if (fd == -1) {
        perror("open (read)");
        fprintf(stderr, "  hint: create '%s' in this directory first.\n", FILENAME);
        return EXIT_FAILURE;
    }
    printf("opened '%s' for reading      (fd = %d)\n", FILENAME, fd);

    /* ---- 2. Read the whole file in 512-byte chunks ----------------------- */
    /* read() returns >0 for a chunk, 0 at end-of-file, -1 on error. Looping
     * until it returns 0 handles files larger than one chunk and tolerates
     * short reads in the middle of the file. */
    printf("\n----- file contents -----\n");
    long total_read = 0;
    ssize_t n;
    while ((n = read(fd, buffer, CHUNK_SIZE)) != 0) {
        if (n == -1) {
            if (errno == EINTR)
                continue;               /* interrupted — retry the read */
            perror("read");
            close(fd);
            return EXIT_FAILURE;
        }
        /* fwrite (not printf) because the bytes are raw and not NUL-terminated;
         * sharing stdout's stdio stream keeps this ordered with the printf()s. */
        fwrite(buffer, 1, (size_t)n, stdout);
        total_read += n;
    }
    printf("\n-------------------------\n");
    printf("total bytes read: %ld\n", total_read);

    /* ---- 3. Close the read descriptor ------------------------------------ */
    if (close(fd) == -1) {
        perror("close (read)");
        return EXIT_FAILURE;
    }
    printf("closed the read descriptor\n");

    /* ---- 4. Reopen in append mode ---------------------------------------- */
    /* O_APPEND makes the kernel seek to end-of-file before EVERY write, as one
     * atomic step, so existing data is never overwritten — even with several
     * processes appending at once. O_CREAT|0644 recreates it if it vanished. */
    fd = open(FILENAME, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd == -1) {
        perror("open (append)");
        return EXIT_FAILURE;
    }
    printf("\nreopened '%s' in append mode (fd = %d)\n", FILENAME, fd);

    /* ---- 5. Append a line ------------------------------------------------ */
    size_t to_write = strlen(APPEND_TEXT);
    if (write_all(fd, APPEND_TEXT, to_write) == -1) {
        perror("write");
        close(fd);
        return EXIT_FAILURE;
    }
    printf("appended %zu bytes: %s", to_write, APPEND_TEXT);

    /* ---- 6. Close the write descriptor ----------------------------------- */
    if (close(fd) == -1) {
        perror("close (append)");
        return EXIT_FAILURE;
    }
    printf("closed the write descriptor\n");

    return EXIT_SUCCESS;
}
