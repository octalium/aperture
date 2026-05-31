#ifndef APERTURE_CORE_SIGNAL_H
#define APERTURE_CORE_SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

// install `handler` for `sig`, hiding the platform split: sigaction
// (with an empty mask, no restart flags) on POSIX, the C signal() call
// on windows where MSVC's CRT offers only that. `sig` is a standard
// signal number (SIGINT, SIGTERM, ...).
void ap_signal_install(int sig, void (*handler)(int));

#ifdef __cplusplus
}
#endif

#endif
