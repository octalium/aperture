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

// <utime.h> / utime are spelled with underscores on MSVC.
#define utime(p, t) _utime((p), (t))
#define utimbuf     _utimbuf

// MSVC's <sys/stat.h> omits the S_IS* test macros even though it
// defines the underlying mode bits.
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

// POSIX realpath(path, NULL) canonicalises and mallocs the result;
// _fullpath(NULL, path, 0) normalises and mallocs with the same
// free-with-free() contract (it does not resolve symlinks, which the
// paths aperture canonicalises never traverse on windows).
#define realpath(path, resolved) _fullpath((resolved), (path), 0)

#ifndef PATH_MAX
#define PATH_MAX _MAX_PATH
#endif

#else
#include <strings.h>
#include <unistd.h>  // fsync, unlink, access on POSIX
#include <utime.h>   // utime / utimbuf on POSIX
#endif

#endif
