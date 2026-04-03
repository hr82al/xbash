// readline_filter_example.go -- Example READLINE_INPUT_FILTER_LIB plugin in Go.
//
// Demonstrates all four optional API functions:
//   - readline_filter_init / readline_filter_cleanup (lifecycle)
//   - readline_filter_keypress (fuzzy history search with Up/Down arrows)
//   - readline_filter_completions (custom completions for @user mentions)
//   - readline_filter_display_matches (custom bullet-list display)
//
// Build:
//   go build -buildmode=c-shared -o readline_filter.so readline_filter_example.go
//
// Usage:
//   export READLINE_INPUT_FILTER_LIB=./readline_filter.so
//   bash
//
// Hot-reload after recompilation:
//   READLINE_INPUT_FILTER_LIB=./readline_filter.so

package main

/*
#include <stdlib.h>
#include <string.h>
*/
import "C"

import (
	"os"
	"runtime/debug"
	"strings"
	"unsafe"
)

// ------------------------------------------------------------------ //
// State preserved across calls (filter is a long-running process)
// ------------------------------------------------------------------ //

var (
	history       []string // lines typed during this session
	searchQuery   string   // text user typed before pressing Up
	searchMatches []string // history entries matching the query
	searchIndex   int      // current position in filtered matches
	navigating    bool     // true while cycling through matches
)

// ------------------------------------------------------------------ //
// Lifecycle
// ------------------------------------------------------------------ //

//export readline_filter_init
func readline_filter_init() C.int {
	// Reduce GC pressure for fast keypress handling.
	debug.SetGCPercent(200)

	history = nil
	navigating = false
	searchIndex = 0
	return 0
}

//export readline_filter_cleanup
func readline_filter_cleanup() {
	history = nil
	searchMatches = nil
}

// ------------------------------------------------------------------ //
// Post-keystroke hook: fuzzy history search with Up/Down
// ------------------------------------------------------------------ //

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

	// Up arrow: ESC [ A
	if len(seq) == 3 && seq[0] == 0x1b && seq[1] == '[' && seq[2] == 'A' {
		if !navigating {
			// Start new fuzzy search
			searchQuery = goLine
			navigating = true
			searchMatches = nil
			for _, h := range history {
				if searchQuery == "" || strings.Contains(
					strings.ToLower(h),
					strings.ToLower(searchQuery),
				) {
					searchMatches = append(searchMatches, h)
				}
			}
			searchIndex = len(searchMatches)
		}

		if len(searchMatches) > 0 && searchIndex > 0 {
			searchIndex--
			return writeResult(searchMatches[searchIndex], outLine, outLineSize, outPoint, outMark)
		}
		return 0
	}

	// Down arrow: ESC [ B
	if len(seq) == 3 && seq[0] == 0x1b && seq[1] == '[' && seq[2] == 'B' {
		if navigating {
			searchIndex++
			if searchIndex < len(searchMatches) {
				return writeResult(searchMatches[searchIndex], outLine, outLineSize, outPoint, outMark)
			}
			// Past the end — restore original query
			navigating = false
			searchIndex = 0
			return writeResult(searchQuery, outLine, outLineSize, outPoint, outMark)
		}
		return 0
	}

	// Enter (CR) — save to history
	if len(seq) == 1 && seq[0] == '\r' {
		trimmed := strings.TrimSpace(goLine)
		if len(trimmed) > 0 {
			// Avoid consecutive duplicates
			if len(history) == 0 || history[len(history)-1] != trimmed {
				history = append(history, trimmed)
			}
		}
		navigating = false
		return 0
	}

	// Any other key — stop navigating
	if navigating {
		navigating = false
	}

	return 0 // no modification for regular keys
}

// ------------------------------------------------------------------ //
// Completion: custom @user mentions
// ------------------------------------------------------------------ //

var knownUsers = []string{
	"@alice", "@bob", "@charlie", "@dave", "@eve",
	"@frank", "@grace", "@heidi",
}

//export readline_filter_completions
func readline_filter_completions(
	line *C.char, lineLen C.int, point C.int,
	word *C.char, wordStart C.int, wordEnd C.int,
) **C.char {
	goWord := C.GoString(word)

	// Only handle words starting with "@"
	if !strings.HasPrefix(goWord, "@") {
		return nil // fall through to bash default completion
	}

	prefix := strings.ToLower(goWord)
	var matches []string
	for _, u := range knownUsers {
		if strings.HasPrefix(strings.ToLower(u), prefix) {
			matches = append(matches, u)
		}
	}

	// Also search history for unique words starting with @
	seen := make(map[string]bool)
	for _, m := range matches {
		seen[strings.ToLower(m)] = true
	}
	for _, h := range history {
		for _, w := range strings.Fields(h) {
			if strings.HasPrefix(strings.ToLower(w), prefix) && !seen[strings.ToLower(w)] {
				matches = append(matches, w)
				seen[strings.ToLower(w)] = true
			}
		}
	}

	if len(matches) == 0 {
		return nil
	}

	// Compute LCD (longest common prefix)
	lcd := matches[0]
	for _, m := range matches[1:] {
		lcd = commonPrefix(lcd, m)
	}

	// Build C array: [LCD, match1, ..., matchN, NULL]
	// MUST use C.malloc — bash will free() each element
	count := 1 + len(matches) + 1
	arrSize := C.size_t(count) * C.size_t(unsafe.Sizeof((*C.char)(nil)))
	arr := (**C.char)(C.malloc(arrSize))
	slice := unsafe.Slice(arr, count)

	slice[0] = C.CString(lcd)
	for i, m := range matches {
		slice[i+1] = C.CString(m)
	}
	slice[len(matches)+1] = nil

	return arr
}

// ------------------------------------------------------------------ //
// Custom completion display: bullet list
// ------------------------------------------------------------------ //

//export readline_filter_display_matches
func readline_filter_display_matches(matches **C.char, numMatches C.int, maxLen C.int) {
	n := int(numMatches)
	slice := unsafe.Slice(matches, n+2)

	// Write to stderr (bash's rl_outstream)
	f := os.Stderr
	f.WriteString("\n")
	for i := 1; i <= n; i++ {
		s := C.GoString(slice[i])
		f.WriteString("  \033[36m*\033[0m " + s + "\n")
	}
}

// ------------------------------------------------------------------ //
// Helpers
// ------------------------------------------------------------------ //

func writeResult(s string, outLine *C.char, outLineSize C.int, outPoint *C.int, outMark *C.int) C.int {
	if len(s) >= int(outLineSize) {
		s = s[:int(outLineSize)-1]
	}
	cBytes := []byte(s)
	dst := unsafe.Slice((*byte)(unsafe.Pointer(outLine)), int(outLineSize))
	copy(dst, cBytes)
	dst[len(cBytes)] = 0
	*outPoint = C.int(len(cBytes))
	*outMark = 0
	return 1
}

func commonPrefix(a, b string) string {
	minLen := len(a)
	if len(b) < minLen {
		minLen = len(b)
	}
	i := 0
	for i < minLen && a[i] == b[i] {
		i++
	}
	return a[:i]
}

func main() {} // required for -buildmode=c-shared
