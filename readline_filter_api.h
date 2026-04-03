/* readline_filter_api.h -- Public API for READLINE_INPUT_FILTER_LIB shared libraries.

   Copyright (C) 2025 Free Software Foundation, Inc.

   This file is part of GNU Bash, the Bourne Again SHell.

   Bash is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Bash is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Bash.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * This header defines the C interface that a shared library (.so) must
 * implement to be loaded by bash via the READLINE_INPUT_FILTER_LIB variable.
 *
 * All functions are OPTIONAL.  Bash resolves each symbol via dlsym() and
 * only hooks the ones that exist.  A library may implement any subset.
 *
 * The library can be written in any language that produces C-shared
 * libraries: C, C++, Go (-buildmode=c-shared), Rust (cdylib), etc.
 *
 * THREAD SAFETY: All functions are called from bash's main thread only.
 * The library must not call readline functions from other threads.
 *
 * HOT-RELOAD: Reassigning READLINE_INPUT_FILTER_LIB at runtime triggers
 * cleanup() + dlclose() of the old library, then dlopen() + init() of
 * the new one.  No bash restart required.
 */

#ifndef _READLINE_FILTER_API_H_
#define _READLINE_FILTER_API_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/*
 * Called once immediately after dlopen().
 * Use this to initialize internal state, open files, start goroutines, etc.
 *
 * Return 0 on success.  Non-zero causes bash to dlclose() the library
 * and print a diagnostic to stderr.
 */
int readline_filter_init(void);

/*
 * Called once before dlclose().
 * Clean up resources: close files, stop goroutines, free memory.
 */
void readline_filter_cleanup(void);

/* ------------------------------------------------------------------ */
/* Post-keystroke hook                                                */
/* ------------------------------------------------------------------ */

/*
 * Called after each key dispatch, before the line is redisplayed.
 *
 * Parameters:
 *   keyseq       - raw bytes of the key sequence (e.g., "\x1b[A" for Up)
 *   keyseq_len   - length of keyseq in bytes
 *   line         - current contents of rl_line_buffer (NOT null-terminated
 *                  beyond line_len bytes; treat as line_len-byte buffer)
 *   line_len     - length of line in bytes (rl_end)
 *   point        - cursor position as byte offset (rl_point)
 *   mark         - mark position as byte offset (rl_mark)
 *   out_line     - output buffer; write the new line here (null-terminated)
 *   out_line_size - capacity of out_line in bytes (currently 8192)
 *   out_point    - output: set to the new cursor position
 *   out_mark     - output: set to the new mark position
 *
 * Return values:
 *    0  no modification (out_* parameters are ignored)
 *    1  apply changes: bash updates the line buffer from out_line,
 *       sets rl_point = *out_point, rl_mark = *out_mark
 *   -1  error (bash ignores and continues)
 *
 * PERFORMANCE: This function is called on every keypress.  It must
 * complete in under 1ms for responsive typing.  For Go libraries,
 * consider using debug.SetGCPercent(-1) during the call.
 */
int readline_filter_keypress(
    const char *keyseq,
    int         keyseq_len,
    const char *line,
    int         line_len,
    int         point,
    int         mark,
    char       *out_line,
    int         out_line_size,
    int        *out_point,
    int        *out_mark
);

/* ------------------------------------------------------------------ */
/* Completion generation                                              */
/* ------------------------------------------------------------------ */

/*
 * Called when the user presses Tab (or another completion key).
 *
 * Parameters:
 *   line       - full line buffer contents
 *   line_len   - length of line (rl_end)
 *   point      - cursor position (rl_point)
 *   word       - the word being completed (extracted by readline)
 *   word_start - start index of word in line
 *   word_end   - end index of word in line
 *
 * Return value:
 *   A malloc()'d NULL-terminated array of malloc()'d strings:
 *     matches[0]     = longest common prefix (LCD) of all matches
 *     matches[1..N]  = individual match strings
 *     matches[N+1]   = NULL
 *
 *   Return NULL to fall through to bash's default completion.
 *
 * MEMORY OWNERSHIP: Bash will call free() on each string in the array
 * and on the array itself.  The library MUST allocate with C malloc(),
 * NOT with a language-specific allocator.
 *
 *   Go:   use C.CString() for strings, C.malloc() for the array
 *   Rust: use libc::malloc / CString::into_raw
 *   C:    use malloc() / strdup()
 */
char **readline_filter_completions(
    const char *line,
    int         line_len,
    int         point,
    const char *word,
    int         word_start,
    int         word_end
);

/* ------------------------------------------------------------------ */
/* Completion display                                                 */
/* ------------------------------------------------------------------ */

/*
 * If exported, replaces readline's default completion display.
 * Called instead of the standard columnar match listing.
 *
 * Parameters:
 *   matches      - array where matches[0] is the LCD,
 *                  matches[1..num_matches] are the candidates
 *   num_matches  - number of candidates (not counting LCD)
 *   max_len      - length of the longest match string
 *
 * The library is responsible for all terminal output.  After returning,
 * bash will redisplay the prompt and current line.
 */
void readline_filter_display_matches(
    char **matches,
    int    num_matches,
    int    max_len
);

#ifdef __cplusplus
}
#endif

#endif /* _READLINE_FILTER_API_H_ */
