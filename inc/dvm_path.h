#pragma once
// ─── dvm_path.h — portable __FILE__-relative path resolution ─────────────────
// Replaces realpath() (POSIX-only) with a cross-platform equivalent.
// Usage:
//   dvm_abspath(__FILE__, abs, sizeof(abs))  → absolute path of source file
//   dvm_dirname(abs)                         → strips last path component in-place
//   dvm_path_join(buf, sizeof(buf), a, b)    → buf = a/b  (uses / on all platforms)

#include <stddef.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#  include <stdlib.h>   // _fullpath
#else
#  include <stdlib.h>   // realpath
#endif

// Resolve path to absolute. Returns buf on success, NULL on failure.
static inline char *dvm_abspath(const char *path, char *buf, size_t bufsz) {
#if defined(_WIN32)
    return _fullpath(buf, path, bufsz);
#else
    return realpath(path, buf);
#endif
}

// Strip the last path component from path in-place.
// Handles both '/' and '\' separators.
static inline void dvm_dirname(char *path) {
    // Find last slash (either kind)
    char *a = strrchr(path, '/');
    char *b = strrchr(path, '\\');
    char *slash = (a > b) ? a : b;
    if (slash) *slash = '\0';
}

// Join two path components into buf, always using '/'.
static inline void dvm_path_join(char *buf, size_t bufsz,
                                  const char *a, const char *b) {
    snprintf(buf, bufsz, "%s/%s", a, b);
}
