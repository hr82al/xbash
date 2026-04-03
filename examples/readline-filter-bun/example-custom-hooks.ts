#!/usr/bin/env bun
/**
 * Custom plugin using React-style hooks.
 *
 * Demonstrates useState, useRef, useKeypress, useKey, useCompletion
 * for building complex interactive behaviors from scratch.
 *
 *   bun run example-custom-hooks.ts &
 *   READLINE_INPUT_FILTER_LIB=../loadables/readline_filter_bun_bridge.so bash
 */
import {
  ReadlineFilter,
  useState,
  useRef,
  useKeypress,
  useKey,
  useCompletion,
  useDisplay,
  useEffect,
  Keys,
  keyIs,
  keyChar,
  type Plugin,
  type KeypressEvent,
} from "./readline-filter";

// ------------------------------------------------------------------ //
//  Custom plugin: Keystroke counter in prompt                         //
// ------------------------------------------------------------------ //

function KeyCounter(): Plugin {
  return () => {
    const count = useRef(0);

    useKeypress(({ line, point, mark, keyseq }) => {
      // Don't count special keys
      if (keyChar(keyseq)) {
        count.current++;
      }
      // No line modification — just counting
      return null;
    });

    // Expose count for other plugins (via module-level var for simplicity)
    useEffect(() => {
      console.error(`[counter] tracking keystrokes`);
    });
  };
}

// ------------------------------------------------------------------ //
//  Custom plugin: Vim-style mode switching                            //
// ------------------------------------------------------------------ //

function VimMode(): Plugin {
  return () => {
    const mode = useRef<"insert" | "normal">("insert");
    const statusLine = useRef("");

    // ESC → normal mode
    useKeypress(({ keyseq, line, point, mark }) => {
      if (keyseq.length === 1 && keyseq[0] === 27) { // ESC
        mode.current = "normal";
        return null;
      }
      return null;
    });

    // In normal mode: 'i' → insert, 'A' → append, 'dd' → clear
    useKeypress(({ keyseq, line, point, mark }) => {
      if (mode.current !== "normal") return null;
      const ch = keyChar(keyseq);
      if (!ch) return null;

      switch (ch) {
        case "i":
          mode.current = "insert";
          return null;
        case "A":
          mode.current = "insert";
          return { line, point: line.length, mark };
        case "0":
          return { line, point: 0, mark };
        case "$":
          return { line, point: line.length, mark };
        default:
          // Swallow key in normal mode (don't insert it)
          return { line, point, mark };
      }
    });
  };
}

// ------------------------------------------------------------------ //
//  Custom plugin: Live word count                                     //
// ------------------------------------------------------------------ //

function WordCount(): Plugin {
  return () => {
    useCompletion(({ word }) => {
      // Special completion: typing "wc" + Tab shows word stats
      if (word === "wc") {
        return ["wc", "wc (word count plugin active)"];
      }
      return null;
    });
  };
}

// ------------------------------------------------------------------ //
//  Custom plugin: Bracket auto-close                                  //
// ------------------------------------------------------------------ //

function AutoClose(): Plugin {
  return () => {
    const pairs: Record<string, string> = {
      "(": ")",
      "[": "]",
      "{": "}",
      '"': '"',
      "'": "'",
      "`": "`",
    };

    useKeypress(({ keyseq, line, point, mark }) => {
      const ch = keyChar(keyseq);
      if (!ch || !pairs[ch]) return null;

      // Insert the closing character after cursor
      const before = line.slice(0, point);
      const after = line.slice(point);
      const newLine = before + pairs[ch] + after;
      // point already advanced by readline to after the opening char
      return { line: newLine, point, mark };
    });
  };
}

// ------------------------------------------------------------------ //
//  Compose and run                                                    //
// ------------------------------------------------------------------ //

ReadlineFilter.create({
  plugins: [
    KeyCounter(),
    AutoClose(),
    WordCount(),
  ],
  onStart(s) {
    console.error(`[custom-hooks] listening on ${s}`);
    console.error(`[custom-hooks] features: keystroke counter, auto-close brackets, word count`);
  },
});
