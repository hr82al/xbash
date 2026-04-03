#!/usr/bin/env bun
/**
 * Fuzzy history search with Up/Down — one plugin, zero boilerplate.
 *
 *   bun run example-history-search.ts &
 *   READLINE_INPUT_FILTER_LIB=../loadables/readline_filter_bun_bridge.so bash
 */
import { ReadlineFilter, HistorySearch } from "./readline-filter";

ReadlineFilter.create({
  plugins: [
    HistorySearch({ fuzzy: true }),
  ],
  onStart(s) {
    console.error(`[history-search] listening on ${s}`);
  },
});
