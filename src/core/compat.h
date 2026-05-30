#ifndef APERTURE_CORE_COMPAT_H
#define APERTURE_CORE_COMPAT_H

// portable shims for POSIX functions MSVC names differently. include
// this instead of <strings.h>; <string.h> is pulled in for the rest of
// the str* family on every target. the file-I/O shims below cover the
// 1:1 renames (underscore-prefixed on MSVC) and the few <sys/stat.h>
// pieces MSVC omits; anything with a different shape (directory walks,
// threads) lives in its own ap_* abstraction instead.
#include <string.h>

#ifdef _WIN32
#include <direct.h>   // _mkdir, _fullpath
#include <io.h>       // _commit, _fileno, _unlink
#include <stdio.h>    // _fileno wants FILE
#include <stdlib.h>   // _MAX_PATH, _fullpath
#include <sys/stat.h> // _stat layout + the _S_IF* mode bits MSVC defines
#include <sys/utime.h>

#define strcasecmp  _stricmp
#define strncasecmp _strnicmp

// strcasestr (case-insensitive strstr) is a GNU extension with no MSVC
// equivalent. provide a small portable implementation: scan `haystack`
// for the first run that matches `needle` case-insensitively.
static inline char *ap_strcasestr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (_strnicmp(haystack, needle, nlen) == 0) {
            return (char *)haystack;
        }
    }
    return NULL;
}
#define strcasestr(h, n) ap_strcasestr((h), (n))

// MSVC names these with a leading underscore; map the POSIX spellings
// onto them so call-sites stay platform-agnostic.
#define fsync(fd)   _commit(fd)
#define fileno(f)   _fileno(f)
#define unlink(p)   _unlink(p)
#define access(p, m) _access((p), (m))

// MSVC's _access takes the same numeric mode but omits the F_OK / R_OK
// / W_OK names. F_OK (existence) is mode 0 on both.
#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif

// POSIX mkdir takes a mode; MSVC's _mkdir does not. drop the mode so
// existing mkdir(path, 0755) call-sites compile unchanged.
#define mkdir(path, mode) _mkdir(path)
#define rmdir(path) _rmdir(path)

// <utime.h> / utime are spelled with underscores on MSVC.
#define utime(p, t) _utime((p), (t))
#define utimbuf     _utimbuf

// timegm (UTC counterpart of mktime) is GNU; MSVC spells it _mkgmtime.
#include <time.h>
#define timegm(tm) _mkgmtime(tm)

// reentrant time conversions. MSVC has gmtime_s/localtime_s but with
// reversed arguments and an errno_t return; wrap them to present the
// POSIX gmtime_r/localtime_r signature (struct tm * on success, NULL on
// failure).
static inline struct tm *ap_gmtime_r(const time_t *t, struct tm *out)
{
    return gmtime_s(out, t) == 0 ? out : NULL;
}
static inline struct tm *ap_localtime_r(const time_t *t, struct tm *out)
{
    return localtime_s(out, t) == 0 ? out : NULL;
}
#define gmtime_r(t, out)    ap_gmtime_r((t), (out))
#define localtime_r(t, out) ap_localtime_r((t), (out))

// POSIX setenv/unsetenv onto MSVC's _putenv_s. setenv always overwrites
// here (the overwrite flag is dropped); aperture's call-sites pass 1.
// unsetenv clears the variable by setting it empty.
static inline int ap_setenv(const char *name, const char *value, int overwrite)
{
    (void)overwrite;
    return _putenv_s(name, value);
}
#define setenv(n, v, o) ap_setenv((n), (v), (o))
#define unsetenv(n)     _putenv_s((n), "")

// MSVC's <sys/stat.h> omits the S_IS* test macros even though it
// defines the underlying mode bits.
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

// POSIX realpath: with a NULL output buffer it mallocs the result
// (free with free()); with a caller buffer it writes up to PATH_MAX
// bytes. _fullpath has the same two modes but needs the buffer size
// for the non-NULL case, so wrap it rather than macro-substitute. it
// normalises without resolving symlinks, which the paths aperture
// canonicalises never traverse on windows.
static inline char *ap_realpath(const char *path, char *resolved)
{
    return _fullpath(resolved, path, resolved ? _MAX_PATH : 0);
}
#define realpath(path, resolved) ap_realpath((path), (resolved))

#ifndef PATH_MAX
#define PATH_MAX _MAX_PATH
#endif

#else
#include <strings.h>
#include <unistd.h>  // fsync, unlink, access on POSIX
#include <utime.h>   // utime / utimbuf on POSIX
#endif

// compile-time printf-format checking on a variadic function. GCC and
// Clang verify the arguments against the format string; MSVC has no
// equivalent and the macro expands to nothing there. `fmt` is the
// 1-based index of the format-string parameter, `args` of the first
// variadic argument.
#if defined(__GNUC__) || defined(__clang__)
#define AP_PRINTF_FMT(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define AP_PRINTF_FMT(fmt, args)
#endif

#endif
