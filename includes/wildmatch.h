/*
 * wildmatch.h - Shell-style pattern matching.
 *
 * Vendored from git (https://github.com/git/git) with minimal modifications
 * for standalone compilation. Original license: BSD.
 *
 * Original authors:
 *   Rich Salz (original implementation)
 *   Wayne Davison (modifications for '/' matching and '**' support)
 */
#ifndef WILDMATCH_H
#define WILDMATCH_H

#define WM_CASEFOLD 1
#define WM_PATHNAME 2

#define WM_NOMATCH 1
#define WM_MATCH 0

#ifdef __cplusplus
extern "C" {
#endif

int wildmatch(const char *pattern, const char *text, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif /* WILDMATCH_H */

