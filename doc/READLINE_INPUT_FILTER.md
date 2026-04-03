# READLINE_INPUT_FILTER

Customize the readline input line with an external program, called after every keypress.

## Overview

The `READLINE_INPUT_FILTER` shell variable specifies an external program
that acts as a persistent coprocess filter for the readline input line.
After every keypress, bash sends the current line state to the filter's
stdin and reads back modifications from its stdout.

The filter is launched once when readline initializes and communicates
via pipes -- no fork/exec overhead per keypress.

## Shell Variables

### READLINE_INPUT_FILTER

The command to run as the filter coprocess.  Set before starting an
interactive bash session:

```bash
export READLINE_INPUT_FILTER='/path/to/my-filter'
bash   # start a new shell with the filter active
```

To disable, unset the variable and restart bash:

```bash
unset READLINE_INPUT_FILTER
exec bash
```

### READLINE_INPUT_FILTER_TIMEOUT

Timeout for filter responses, in milliseconds.  Controls how long bash
waits for the filter to respond after each keypress.

| Value   | Behavior                                                      |
|---------|---------------------------------------------------------------|
| (unset) | Default: 10ms timeout                                        |
| `0`     | Wait forever (blocking) -- use only if filter is guaranteed fast |
| `N`     | Wait N milliseconds, then fall back to standard behavior      |

```bash
export READLINE_INPUT_FILTER_TIMEOUT=50    # 50ms -- more time for slow filters
export READLINE_INPUT_FILTER_TIMEOUT=0     # infinite -- never time out
export READLINE_INPUT_FILTER_TIMEOUT=5     # 5ms -- strict latency budget
```

### Fallback Behavior

When the filter does not respond within the timeout:

- The keypress is processed normally by readline (standard behavior).
- The filter is **not** disabled -- the next keypress will try the filter again.
- No warning is printed; timeouts are silent and transparent to the user.

The filter is only disabled permanently when:

- The filter process dies (EPIPE on write).
- The filter closes its stdout (EOF on read).

In these cases, a warning is printed to stderr and bash continues working
normally without filtering.

## Protocol

### Request (bash -> filter, one line per keypress)

```
{hex_keyseq}\t{point}\t{end}\t{mark}\t{hex_line}\n
```

| Field       | Type | Description                                           |
|-------------|------|-------------------------------------------------------|
| hex_keyseq  | hex  | Full key sequence, hex-encoded (see Key Sequences)    |
| point       | int  | Cursor position (byte offset into line buffer)        |
| end         | int  | Line length in bytes                                  |
| mark        | int  | Mark position (byte offset)                           |
| hex_line    | hex  | Line buffer contents, hex-encoded                     |

### Response (filter -> bash, one line)

```
{new_point}\t{new_mark}\t{hex_new_line}\n
```

| Field        | Type | Description                    |
|--------------|------|--------------------------------|
| new_point    | int  | New cursor position            |
| new_mark     | int  | New mark position              |
| hex_new_line | hex  | New line buffer, hex-encoded   |

An empty line (`\n` alone) means "no changes" -- the current line is kept as-is.

### Hex Encoding

Every byte is represented as two lowercase hexadecimal characters:

```
"hello"  -> "68656c6c6f"
""       -> ""
"\x1b"   -> "1b"
```

### Key Sequences

The `hex_keyseq` field contains the full key sequence that triggered the
command, hex-encoded.  This allows the filter to distinguish between
regular keys and special keys:

| Key        | Raw bytes       | hex_keyseq   |
|------------|-----------------|--------------|
| `a`        | `0x61`          | `61`         |
| Enter      | `0x0d`          | `0d`         |
| Ctrl+A     | `0x01`          | `01`         |
| Up arrow   | `ESC [ A`       | `1b5b41`     |
| Down arrow | `ESC [ B`       | `1b5b42`     |
| Right      | `ESC [ C`       | `1b5b43`     |
| Left       | `ESC [ D`       | `1b5b44`     |
| Home       | `ESC [ H`       | `1b5b48`     |
| End        | `ESC [ F`       | `1b5b46`     |
| Delete     | `ESC [ 3 ~`     | `1b5b337e`   |
| F1         | `ESC O P`       | `1b4f50`     |

The filter can decode the key sequence to implement key-specific behavior,
such as custom history search on Up/Down arrows.

## Example Filters

### Example 1: Auto-Uppercase (Python)

Every character you type is converted to uppercase in real time.

```python
#!/usr/bin/env python3
"""Filter that uppercases the entire input line."""
import sys

for line in sys.stdin:
    parts = line.rstrip('\n').split('\t')
    if len(parts) != 5:
        print('', flush=True)
        continue

    keyseq_hex, point, end, mark, hex_buf = parts
    buf = bytes.fromhex(hex_buf).decode('utf-8', errors='replace')
    new_buf = buf.upper()
    new_hex = new_buf.encode('utf-8').hex()
    print(f"{point}\t{mark}\t{new_hex}", flush=True)
```

### Example 2: Auto-Correct Typos (Python)

Common typos are automatically fixed as you type.

```python
#!/usr/bin/env python3
"""Filter that auto-corrects common typos."""
import sys

REPLACEMENTS = {
    'teh ': 'the ',
    'adn ': 'and ',
    'taht ': 'that ',
    'waht ': 'what ',
    'hte ': 'the ',
}

for line in sys.stdin:
    parts = line.rstrip('\n').split('\t')
    if len(parts) != 5:
        print('', flush=True)
        continue

    keyseq_hex, point, end, mark, hex_buf = parts
    buf = bytes.fromhex(hex_buf).decode('utf-8', errors='replace')

    changed = False
    for typo, fix in REPLACEMENTS.items():
        if typo in buf:
            buf = buf.replace(typo, fix)
            changed = True

    if changed:
        new_hex = buf.encode('utf-8').hex()
        new_point = min(int(point), len(buf.encode('utf-8')))
        print(f"{new_point}\t{mark}\t{new_hex}", flush=True)
    else:
        print('', flush=True)
```

### Example 3: Custom Fuzzy History Search with Up/Down Arrows (Python)

Replaces the standard Up/Down history navigation with fuzzy search.
When you type a prefix and press Up, the filter searches history for
matching entries instead of just cycling through sequentially.

```python
#!/usr/bin/env python3
"""Custom history search with Up/Down arrows.

Standard behavior: Up/Down cycle through history sequentially.
This filter: Up/Down search history entries matching what you've typed.

Usage:
    export READLINE_INPUT_FILTER='python3 /path/to/history_filter.py'
    export READLINE_INPUT_FILTER_TIMEOUT=50
    bash
"""
import sys
import os
import re

UP_ARROW = "1b5b41"       # ESC [ A
DOWN_ARROW = "1b5b42"     # ESC [ B

def load_history():
    """Load bash history from ~/.bash_history."""
    path = os.path.expanduser("~/.bash_history")
    try:
        with open(path, 'r', errors='replace') as f:
            lines = [l.strip() for l in f if l.strip()]
        # Deduplicate preserving order (most recent last)
        seen = set()
        unique = []
        for line in lines:
            if line not in seen:
                seen.add(line)
                unique.append(line)
        return unique
    except FileNotFoundError:
        return []

history = load_history()
search_query = ""     # the text user typed before pressing Up
match_index = -1      # current position in filtered results
matches = []          # filtered history entries
navigating = False    # True while user is pressing Up/Down

for line in sys.stdin:
    parts = line.rstrip('\n').split('\t')
    if len(parts) != 5:
        print('', flush=True)
        continue

    keyseq_hex, point, end, mark, hex_buf = parts
    buf = bytes.fromhex(hex_buf).decode('utf-8', errors='replace')

    if keyseq_hex == UP_ARROW:
        if not navigating:
            # Start a new search: save what user typed as query
            search_query = buf
            navigating = True
            # Find all history entries containing the query
            if search_query:
                matches = [h for h in history if search_query.lower() in h.lower()]
            else:
                matches = list(history)
            match_index = len(matches)  # start from the end (most recent)

        # Move up (backward in history)
        if matches and match_index > 0:
            match_index -= 1
            result = matches[match_index]
            new_hex = result.encode('utf-8').hex()
            new_point = len(result.encode('utf-8'))
            print(f"{new_point}\t{mark}\t{new_hex}", flush=True)
        else:
            print('', flush=True)

    elif keyseq_hex == DOWN_ARROW:
        if navigating and matches:
            match_index += 1
            if match_index < len(matches):
                result = matches[match_index]
                new_hex = result.encode('utf-8').hex()
                new_point = len(result.encode('utf-8'))
                print(f"{new_point}\t{mark}\t{new_hex}", flush=True)
            else:
                # Past the end: restore the original query
                match_index = len(matches)
                navigating = False
                new_hex = search_query.encode('utf-8').hex()
                new_point = len(search_query.encode('utf-8'))
                print(f"{new_point}\t{mark}\t{new_hex}", flush=True)
        else:
            print('', flush=True)

    else:
        # Any other key: stop navigating, let readline handle it
        navigating = False
        matches = []
        match_index = -1
        print('', flush=True)
```

### Example 4: Echo Filter / No-Op (Bash)

Useful for testing -- passes the line through unchanged.

```bash
#!/bin/bash
# Echo filter: returns the line unchanged.
while IFS=$'\t' read -r keyseq point end mark hex_line; do
    printf '%s\t%s\t%s\n' "$point" "$mark" "$hex_line"
done
```

### Example 5: Live Transliteration (Python)

Converts Latin characters to Cyrillic as you type.

```python
#!/usr/bin/env python3
"""Transliterate latin to cyrillic while typing."""
import sys

TRANSLIT = str.maketrans(
    "abcdefghijklmnopqrstuvwxyz",
    "\u0430\u0431\u0446\u0434\u0435\u0444\u0433\u0445"
    "\u0438\u0439\u043a\u043b\u043c\u043d\u043e\u043f"
    "\u0440\u0441\u0442\u0443\u0432\u0432\u044a\u044b"
    "\u0437\u0436"
)

for line in sys.stdin:
    parts = line.rstrip('\n').split('\t')
    if len(parts) != 5:
        print('', flush=True)
        continue

    keyseq_hex, point, end, mark, hex_buf = parts
    buf = bytes.fromhex(hex_buf).decode('utf-8', errors='replace')
    new_buf = buf.translate(TRANSLIT)
    new_hex = new_buf.encode('utf-8').hex()
    orig_chars = len(buf[:int(point)].encode('utf-8'))
    new_point = len(new_buf[:orig_chars].encode('utf-8'))
    print(f"{new_point}\t{mark}\t{new_hex}", flush=True)
```

### Example 6: Command Prefix Suggestion (Python)

Shows inline completion from a static command list.

```python
#!/usr/bin/env python3
"""Show inline command suggestions from a static list."""
import sys

COMMANDS = [
    'git status', 'git commit -m ""', 'git push origin main',
    'docker compose up -d', 'docker ps', 'make test',
    'cd /var/log', 'tail -f syslog', 'grep -r TODO .',
]

for line in sys.stdin:
    parts = line.rstrip('\n').split('\t')
    if len(parts) != 5:
        print('', flush=True)
        continue

    keyseq_hex, point, end, mark, hex_buf = parts
    buf = bytes.fromhex(hex_buf).decode('utf-8', errors='replace')

    match = None
    for cmd in COMMANDS:
        if cmd.startswith(buf) and cmd != buf and len(buf) > 0:
            match = cmd
            break

    if match:
        new_hex = match.encode('utf-8').hex()
        print(f"{point}\t{mark}\t{new_hex}", flush=True)
    else:
        print('', flush=True)
```

## Error Handling and Fallback

| Scenario | Detection | Behavior |
|----------|-----------|----------|
| Filter doesn't respond in time | `select()` timeout | **Fallback**: keypress handled normally by readline, filter stays active |
| Filter crashes/exits | `write()` returns EPIPE | Filter disabled, warning to stderr |
| Filter closes stdout | `read()` returns EOF | Filter disabled, warning to stderr |
| Malformed response | Parse failure | Response ignored, line unchanged |
| Point/mark out of range | Value > rl_end or < 0 | Clamped to valid range [0, rl_end] |

**Key principle**: timeouts are graceful.  The filter is never disabled due to
slow responses -- it just falls back to standard readline behavior for that
keypress.  Only a dead filter (EPIPE/EOF) causes permanent deactivation.

## Tips for Writing Filters

1. **Always flush output** -- use `flush=True` in Python, `stdbuf -oL` for
   other programs, or write to a non-buffered stream.  Without flushing,
   responses stay in stdio buffers and bash sees a timeout.

2. **Respond to every request** -- even if you don't want to change anything,
   send an empty line (`\n`).  Not responding triggers a timeout and fallback.

3. **Be fast** -- by default you have 10ms to respond.  Use
   `READLINE_INPUT_FILTER_TIMEOUT` to increase if needed.  Heavy computation
   will cause noticeable typing lag.

4. **Handle UTF-8 correctly** -- `point`, `end`, and `mark` are byte offsets
   into the line buffer, not character offsets.  Multi-byte UTF-8 characters
   occupy multiple bytes.

5. **Don't write to stderr** -- the filter's stderr is inherited from bash,
   so debug output goes to the terminal and will mess up the display.
   Redirect debug output to a file instead:
   ```python
   import sys
   debug = open('/tmp/filter-debug.log', 'a')
   print("debug message", file=debug, flush=True)
   ```

6. **Use the key sequence** -- decode `hex_keyseq` to identify special keys.
   This enables key-specific behavior (e.g., custom Up/Down history search).
   ```python
   keyseq = bytes.fromhex(keyseq_hex)
   if keyseq == b'\x1b[A':    # Up arrow
       ...
   elif keyseq == b'\x1b[B':  # Down arrow
       ...
   ```

7. **Maintain state** -- the filter is a long-running process.  Use variables
   to track state across keypresses (e.g., search position in history,
   undo stack, toggle modes).

8. **Use `READLINE_INPUT_FILTER_TIMEOUT=0`** for complex filters that need
   more time (e.g., calling external APIs, searching large datasets).
   But be aware this blocks typing until the filter responds.

---

# READLINE_INPUT_FILTER_LIB

High-performance readline customization via a shared library (.so), loaded
with `dlopen()`.  Zero IPC overhead — functions are called directly.

## Overview

`READLINE_INPUT_FILTER_LIB` specifies a path to a shared library that
bash loads at runtime.  The library can customize:

- **Keystroke handling** -- modify the line buffer after every keypress
- **Tab completion** -- generate custom completion candidates
- **Completion display** -- replace the default columnar listing
- **History navigation** -- custom Up/Down arrow behavior (via keystroke hook)

The library can be written in **C**, **Go** (`-buildmode=c-shared`),
**Rust** (`cdylib`), or any language that produces C-compatible shared objects.

## Quick Start

```bash
# C example
gcc -shared -fPIC -o filter.so my_filter.c
export READLINE_INPUT_FILTER_LIB=./filter.so
bash

# Go example
go build -buildmode=c-shared -o filter.so my_filter.go
export READLINE_INPUT_FILTER_LIB=./filter.so
bash
```

## Hot-Reload

The library can be reloaded without restarting bash.  Reassigning the
variable triggers `dlclose()` of the old library and `dlopen()` of the new:

```bash
# Recompile in another terminal, then:
READLINE_INPUT_FILTER_LIB=./filter.so    # hot-reload!

# Or unload completely:
unset READLINE_INPUT_FILTER_LIB           # back to standard behavior
```

## Priority

If both `READLINE_INPUT_FILTER` (pipe) and `READLINE_INPUT_FILTER_LIB`
(shared library) are set, the **library takes priority** and the pipe
coprocess is not started.

## API Reference

All functions are **optional**.  Bash resolves each via `dlsym()` and only
hooks the ones found.  See `readline_filter_api.h` for the full header.

### readline_filter_init

```c
int readline_filter_init(void);
```

Called once after `dlopen()`.  Initialize internal state here.
Return 0 on success; non-zero causes the library to be unloaded.

### readline_filter_cleanup

```c
void readline_filter_cleanup(void);
```

Called before `dlclose()`.  Free resources, stop goroutines, close files.

### readline_filter_keypress

```c
int readline_filter_keypress(
    const char *keyseq, int keyseq_len,
    const char *line, int line_len,
    int point, int mark,
    char *out_line, int out_line_size,
    int *out_point, int *out_mark);
```

Called after every key dispatch, before redisplay.

| Parameter     | Description                                    |
|---------------|------------------------------------------------|
| keyseq        | Raw key sequence bytes (e.g., `\x1b[A` for Up)|
| keyseq_len    | Length of keyseq                               |
| line          | Current line buffer contents                   |
| line_len      | Length of line (rl_end)                         |
| point         | Cursor position, byte offset (rl_point)        |
| mark          | Mark position, byte offset (rl_mark)           |
| out_line      | Output buffer for modified line (8192 bytes)   |
| out_line_size | Capacity of out_line                           |
| out_point     | Output: new cursor position                    |
| out_mark      | Output: new mark position                      |

**Return values:**
- `0` -- no modification
- `1` -- apply out_line, out_point, out_mark
- `-1` -- error (ignored)

**Key sequences for special keys:**

| Key        | Bytes           |
|------------|-----------------|
| Up arrow   | `\x1b[A`        |
| Down arrow | `\x1b[B`        |
| Right      | `\x1b[C`        |
| Left       | `\x1b[D`        |
| Home       | `\x1b[H`        |
| End        | `\x1b[F`        |
| Tab        | `\x09`          |
| Enter      | `\x0d`          |
| Ctrl+A     | `\x01`          |

### readline_filter_completions

```c
char **readline_filter_completions(
    const char *line, int line_len, int point,
    const char *word, int word_start, int word_end);
```

Called when the user presses Tab.  Return a `malloc()`'d array:

```
matches[0]     = longest common prefix (LCD)
matches[1..N]  = individual matches
matches[N+1]   = NULL
```

Return `NULL` to fall through to bash's default completion.

**Memory ownership:** Bash calls `free()` on each string and the array.
Use C `malloc()`/`strdup()`, Go `C.CString()`/`C.malloc()`, Rust
`libc::malloc`/`CString::into_raw`.

### readline_filter_display_matches

```c
void readline_filter_display_matches(
    char **matches, int num_matches, int max_len);
```

If exported, replaces readline's default columnar completion display.
`matches[0]` is the LCD, `matches[1..num_matches]` are the candidates.

## C Example (Minimal)

```c
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Uppercase every character as you type. */
int readline_filter_keypress(
    const char *keyseq, int keyseq_len,
    const char *line, int line_len,
    int point, int mark,
    char *out_line, int out_line_size,
    int *out_point, int *out_mark)
{
    int i;
    if (line_len <= 0 || line_len >= out_line_size)
        return 0;
    for (i = 0; i < line_len; i++)
        out_line[i] = toupper((unsigned char)line[i]);
    out_line[line_len] = '\0';
    *out_point = point;
    *out_mark = mark;
    return 1;
}
```

Build: `gcc -shared -fPIC -o upper.so upper.c`

## Go Example (Full-Featured)

```go
package main

/*
#include <stdlib.h>
*/
import "C"
import (
    "strings"
    "unsafe"
)

var history []string
var navigating bool
var searchQuery string
var searchMatches []string
var searchIndex int

//export readline_filter_init
func readline_filter_init() C.int {
    history = nil
    navigating = false
    return 0
}

//export readline_filter_cleanup
func readline_filter_cleanup() {
    history = nil
}

//export readline_filter_keypress
func readline_filter_keypress(
    keyseq *C.char, keyseqLen C.int,
    line *C.char, lineLen C.int,
    point C.int, mark C.int,
    outLine *C.char, outLineSize C.int,
    outPoint *C.int, outMark *C.int,
) C.int {
    seq := C.GoBytes(unsafe.Pointer(keyseq), keyseqLen)
    goLine := C.GoStringN(line, lineLen)

    // Up arrow: fuzzy history search
    if len(seq) == 3 && seq[0] == 0x1b && seq[1] == '[' && seq[2] == 'A' {
        if !navigating {
            searchQuery = goLine
            navigating = true
            searchMatches = nil
            for _, h := range history {
                if strings.Contains(strings.ToLower(h), strings.ToLower(searchQuery)) {
                    searchMatches = append(searchMatches, h)
                }
            }
            searchIndex = len(searchMatches)
        }
        if len(searchMatches) > 0 && searchIndex > 0 {
            searchIndex--
            return writeLine(searchMatches[searchIndex], outLine, outLineSize, outPoint, outMark)
        }
        return 0
    }

    // Down arrow
    if len(seq) == 3 && seq[0] == 0x1b && seq[1] == '[' && seq[2] == 'B' {
        if navigating {
            searchIndex++
            if searchIndex < len(searchMatches) {
                return writeLine(searchMatches[searchIndex], outLine, outLineSize, outPoint, outMark)
            }
            navigating = false
            return writeLine(searchQuery, outLine, outLineSize, outPoint, outMark)
        }
        return 0
    }

    // Enter — save to history
    if len(seq) == 1 && seq[0] == '\r' {
        if trimmed := strings.TrimSpace(goLine); len(trimmed) > 0 {
            history = append(history, trimmed)
        }
        navigating = false
        return 0
    }

    if navigating { navigating = false }
    return 0
}

//export readline_filter_completions
func readline_filter_completions(
    line *C.char, lineLen C.int, point C.int,
    word *C.char, wordStart C.int, wordEnd C.int,
) **C.char {
    goWord := C.GoString(word)
    if !strings.HasPrefix(goWord, "@") {
        return nil
    }

    users := []string{"@alice", "@bob", "@charlie", "@dave"}
    prefix := strings.ToLower(goWord)
    var matches []string
    for _, u := range users {
        if strings.HasPrefix(strings.ToLower(u), prefix) {
            matches = append(matches, u)
        }
    }
    if len(matches) == 0 { return nil }

    lcd := matches[0]
    for _, m := range matches[1:] {
        i := 0
        for i < len(lcd) && i < len(m) && lcd[i] == m[i] { i++ }
        lcd = lcd[:i]
    }

    count := 1 + len(matches) + 1
    arr := (**C.char)(C.malloc(C.size_t(count) * C.size_t(unsafe.Sizeof((*C.char)(nil)))))
    slice := unsafe.Slice(arr, count)
    slice[0] = C.CString(lcd)
    for i, m := range matches {
        slice[i+1] = C.CString(m)
    }
    slice[len(matches)+1] = nil
    return arr
}

func writeLine(s string, outLine *C.char, sz C.int, pt *C.int, mk *C.int) C.int {
    if len(s) >= int(sz) { s = s[:int(sz)-1] }
    dst := unsafe.Slice((*byte)(unsafe.Pointer(outLine)), int(sz))
    copy(dst, []byte(s))
    dst[len(s)] = 0
    *pt = C.int(len(s))
    *mk = 0
    return 1
}

func main() {}
```

Build: `go build -buildmode=c-shared -o filter.so filter.go`

## Contracts and Safety

| Aspect | Rule |
|--------|------|
| **Memory** | `readline_filter_completions` must return `malloc()`'d memory. Bash calls `free()`. |
| **Thread safety** | All functions are called from bash's main thread. Do not call readline from other threads. |
| **Crashes** | A crash in the library crashes bash. The library is responsible for correctness. |
| **Performance** | `readline_filter_keypress` must complete in <1ms for responsive typing. |
| **Go GC** | Use `debug.SetGCPercent()` to control GC pauses. Pre-warm goroutines in `init`. |
| **Signals** | Go's runtime may install signal handlers. Test with bash's job control. |

## Troubleshooting

**Library not loading:**
```bash
READLINE_INPUT_FILTER_LIB=./filter.so bash
# Check stderr for "READLINE_INPUT_FILTER_LIB: <dlerror message>"
```

**Symbol not found:**
```bash
nm -D filter.so | grep readline_filter
# Should show T (text/code) entries for exported functions
```

**Go library panics:**
A Go `panic()` in an exported function will crash the entire bash process.
Use `recover()` at the top of each exported function:
```go
//export readline_filter_keypress
func readline_filter_keypress(...) C.int {
    defer func() { recover() }()
    // ... your code ...
}
```

---

# SQLite History Plugin

Substring history search with Up/Down arrows, backed by SQLite3 for speed.

## Features

- **Substring search**: matches any part of the command, not just the prefix
- **Each Up arrow**: finds the next matching entry (not just sequential cycling)
- **Persistent storage**: `~/.config/bash/history.sql` (SQLite3)
- **Auto-import**: imports `~/.bash_history` on first run (preserves timestamps)
- **Deduplication**: repeated commands update their timestamp (most recent first)
- **Zero changes to bash core**: pure `.so` plugin via `READLINE_INPUT_FILTER_LIB`

## Quick Start

```bash
# Build
gcc -shared -fPIC -O2 -o history_sqlite.so \
    examples/loadables/readline_filter_history_sqlite.c -lsqlite3

# Use
export READLINE_INPUT_FILTER_LIB=./history_sqlite.so
bash
```

Type `git`, press Up — finds all history entries containing "git" anywhere.

## How It Works

```
1. User types "docker" and presses Up
2. Plugin queries: SELECT command FROM history
                    WHERE command LIKE '%docker%'
                    ORDER BY timestamp DESC
                    LIMIT 1 OFFSET 0
3. Result: "docker compose up -d" → replaces the line
4. User presses Up again → OFFSET 1 → next match
5. User presses Down → OFFSET decreases or restores original text
6. User presses Enter → command saved to SQLite with current timestamp
7. Any other key → stops navigation, resumes normal editing
```

## Database Schema

```sql
CREATE TABLE history (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    command   TEXT NOT NULL UNIQUE,
    timestamp INTEGER DEFAULT (strftime('%s', 'now'))
);
```

- `UNIQUE` on command enables `ON CONFLICT DO UPDATE` for deduplication
- `timestamp` ordering ensures most recently used commands appear first

## Import from ~/.bash_history

On first run (empty database), the plugin imports `~/.bash_history`:

- Parses `#timestamp` lines (bash's HISTTIMEFORMAT)
- Preserves original timestamps where available
- Uses `INSERT OR IGNORE` to skip duplicates
- Runs in a single transaction for speed (~100K entries in <1 second)

## Requirements

- `libsqlite3` (runtime library)
- `libsqlite3-dev` (for compilation only)

Debian/Ubuntu: `sudo apt install libsqlite3-dev`

## Inspecting the Database

```bash
sqlite3 ~/.config/bash/history.sql

-- Recent commands
SELECT command, datetime(timestamp, 'unixepoch') FROM history
ORDER BY timestamp DESC LIMIT 20;

-- Search
SELECT command FROM history WHERE command LIKE '%docker%'
ORDER BY timestamp DESC;

-- Stats
SELECT count(*) FROM history;
```

---

# Bun Integration (React-Style API)

Write readline filters in TypeScript with a React-inspired API: composable
plugins, hooks for state, built-in components for common patterns.

```
bash ←dlopen→ readline_filter_bun_bridge.so ←unix socket (~130µs)→ Bun server
```

## Quick Start

```bash
# 1. Build the C bridge
gcc -shared -fPIC -o bridge.so examples/loadables/readline_filter_bun_bridge.c

# 2. Write a filter (server.ts)
cat > server.ts << 'FILTER'
import { ReadlineFilter, Transform } from "./readline-filter";
ReadlineFilter.create({ plugins: [Transform(line => line.toUpperCase())] });
FILTER

# 3. Run
bun run server.ts &
READLINE_INPUT_FILTER_LIB=./bridge.so bash
```

## Composing Plugins

Plugins are functions that register hooks — like React components.
Combine them declaratively with `ReadlineFilter.create()`:

```typescript
import {
  ReadlineFilter,
  HistorySearch,
  AutoCorrect,
  Completions,
  Suggestions,
  ColorDisplay,
} from "./readline-filter";

ReadlineFilter.create({
  plugins: [
    HistorySearch({ fuzzy: true }),
    AutoCorrect({ "teh ": "the ", "adn ": "and " }),
    Completions("@", ["@alice", "@bob", "@charlie"]),
    Suggestions(["git status", "docker ps", "make test"]),
    ColorDisplay({ bullet: "→", color: "cyan" }),
  ],
});
```

Plugins run in order.  For keypress/completion, first non-null result wins.
For display, last registered handler wins.

## Built-in Plugins

### HistorySearch

Fuzzy Up/Down arrow history navigation.

```typescript
HistorySearch()                              // defaults (~/.bash_history)
HistorySearch({ fuzzy: true })               // substring matching
HistorySearch({ file: "~/.zsh_history" })    // custom file
HistorySearch({ maxEntries: 5000 })          // limit loaded entries
```

### AutoCorrect

Fix typos in real time as you type.

```typescript
AutoCorrect({
  "teh ": "the ",
  "adn ": "and ",
  "taht ": "that ",
})
```

### Completions

Custom Tab completions for a prefix.

```typescript
Completions("@", ["@alice", "@bob", "@charlie"])
Completions("--", () => fetchFlags())        // dynamic with function
```

### Suggestions

Inline ghost-text suggestions from a command list.

```typescript
Suggestions(["git status", "git commit -m", "docker ps"])
```

### ColorDisplay

Colored bullet-list completion display.

```typescript
ColorDisplay()                               // defaults: cyan ●
ColorDisplay({ bullet: "→", color: "green" })
```

Colors: `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`, `white`.

### Transform

Apply a transformation to every line.

```typescript
Transform(line => line.toUpperCase())
Transform(line => line.replace(/\s+/g, " "))
```

## Hooks (Custom Plugins)

Build your own plugins with React-style hooks:

### useState

```typescript
function MyPlugin(): Plugin {
  return () => {
    const [count, setCount] = useState(0);
    useKeypress(() => { setCount(c => c + 1); return null; });
  };
}
```

### useRef

```typescript
function MyPlugin(): Plugin {
  return () => {
    const data = useRef<string[]>([]);
    useEffect(() => { data.current = loadData(); });
    useKeypress(({ line }) => { /* use data.current */ return null; });
  };
}
```

### useKeypress / useKey

```typescript
// Handle all keypresses
useKeypress(({ keyseq, line, point, mark }) => {
  // Return { line, point, mark } to modify, or null
  return null;
});

// Handle a specific key
useKey(Keys.CTRL_U, ({ line, point }) => {
  return { line: line.slice(point), point: 0, mark: 0 };
});
```

### useCompletion

```typescript
useCompletion(({ word }) => {
  if (word.startsWith("@")) {
    return ["@", "@alice", "@bob"]; // [LCD, ...matches]
  }
  return null; // bash default
});
```

### useDisplay

```typescript
useDisplay(({ matches, numMatches }) => {
  process.stderr.write("\n");
  for (let i = 1; i <= numMatches; i++)
    process.stderr.write(`  * ${matches[i]}\n`);
});
```

### useEffect

```typescript
useEffect(() => {
  console.error("Plugin initialized");
  return () => console.error("Plugin cleaned up");
});
```

## Full Custom Plugin Example

```typescript
import {
  ReadlineFilter, useState, useRef, useKeypress, useKey,
  useCompletion, useEffect, Keys, keyChar,
  type Plugin,
} from "./readline-filter";

// Auto-close brackets and quotes
function AutoClose(): Plugin {
  return () => {
    const pairs: Record<string, string> = {
      "(": ")", "[": "]", "{": "}", '"': '"', "'": "'",
    };
    useKeypress(({ keyseq, line, point, mark }) => {
      const ch = keyChar(keyseq);
      if (!ch || !pairs[ch]) return null;
      const newLine = line.slice(0, point) + pairs[ch] + line.slice(point);
      return { line: newLine, point, mark };
    });
  };
}

ReadlineFilter.create({
  plugins: [AutoClose()],
});
```

## Key Utilities

```typescript
import { Keys, keyIs, keyChar } from "./readline-filter";

keyIs(event.keyseq, Keys.UP)       // true for Up arrow
keyIs(event.keyseq, Keys.ENTER)    // true for Enter
keyIs(event.keyseq, Keys.CTRL_R)   // true for Ctrl+R
keyChar([65])                       // "A"
keyChar([27, 91, 65])              // null (special key)
```

Available constants: `Keys.UP`, `DOWN`, `LEFT`, `RIGHT`, `HOME`, `END`,
`DELETE`, `ENTER`, `TAB`, `BACKSPACE`, `CTRL_A`..`CTRL_W`.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `READLINE_INPUT_FILTER_LIB` | (none) | Path to `readline_filter_bun_bridge.so` |
| `READLINE_FILTER_SOCKET` | `/tmp/readline-filter.sock` | Unix socket path |
| `READLINE_INPUT_FILTER_TIMEOUT` | `10` | Response timeout (ms), `0` = infinite |

## Performance

| Method | Round-trip |
|--------|-----------|
| Direct .so (Go/C) | ~1-10µs |
| **Unix socket (Bun)** | **~100-200µs** |
| TCP loopback | ~334µs |

~100-200µs is imperceptible — well under the 10ms default timeout.

## Hot-Reload

```bash
# Restart Bun server (bridge reconnects automatically)
pkill -f "bun run server.ts" && bun run server.ts &

# Reload the C bridge
READLINE_INPUT_FILTER_LIB=./bridge.so

# Unload everything
unset READLINE_INPUT_FILTER_LIB
```

## Protocol Reference

Newline-delimited JSON over Unix domain socket.

| Direction | Format |
|-----------|--------|
| Keypress → | `{"type":"keypress","keyseq":[27,91,65],"line":"git","point":3,"mark":0}` |
| Keypress ← | `{"modified":true,"line":"git status","point":10,"mark":0}` |
| Complete → | `{"type":"complete","line":"git","point":3,"word":"git","wordStart":0,"wordEnd":3}` |
| Complete ← | `{"matches":["git","git status","git commit"]}` or `{"matches":null}` |
| Display → | `{"type":"display","matches":[...],"numMatches":3,"maxLen":10}` |
| Display ← | `{}` |

## Troubleshooting

```bash
# Server not running
bun run server.ts &
sleep 0.1
READLINE_INPUT_FILTER_LIB=./bridge.so

# Increase timeout for slow handlers
export READLINE_INPUT_FILTER_TIMEOUT=50

# Debug protocol
socat -v UNIX-LISTEN:/tmp/readline-filter.sock,fork -

# Check exported symbols
nm -D bridge.so | grep readline_filter
```
