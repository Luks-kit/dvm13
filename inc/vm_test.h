#pragma once


#include <stdio.h>
#ifdef _WIN32
    #define DEV_NULL "NUL"
#else
    #define DEV_NULL "/dev/null"
#endif
// We use a file descriptor backup because freopen wipes the original stream
#ifdef _WIN32
    #include <io.h>
    #define DUP(fd) _dup(fd)
    #define DUP2(fd1, fd2) _dup2(fd1, fd2)
    #define FILENO(f) _fileno(f)
#else
    #include <unistd.h>
    #define DUP(fd) dup(fd)
    #define DUP2(fd1, fd2) dup2(fd1, fd2)
    #define FILENO(f) fileno(f)
#endif
static int stderr_save = -1;
static void silence_stderr(void) {
    fflush(stderr);
    stderr_save = DUP(FILENO(stderr)); // Save the actual stderr handle
    freopen(DEV_NULL, "w", stderr);    // Redirect stderr to null
}
static void restore_stderr(void) {
    fflush(stderr);
    if (stderr_save != -1) {
        DUP2(stderr_save, FILENO(stderr)); // Restore the handle
        close(stderr_save);
        stderr_save = -1;
    }
}
