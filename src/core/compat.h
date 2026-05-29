#ifndef APERTURE_CORE_COMPAT_H
#define APERTURE_CORE_COMPAT_H

// portable shims for POSIX functions MSVC names differently. include
// this instead of <strings.h>; <string.h> is pulled in for the rest of
// the str* family on every target.
#include <string.h>

#ifdef _WIN32
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

#endif
