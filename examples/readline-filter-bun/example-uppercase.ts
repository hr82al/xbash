#!/usr/bin/env bun
/**
 * Simplest possible filter — 4 lines of actual code.
 *
 *   bun run example-uppercase.ts &
 *   READLINE_INPUT_FILTER_LIB=../loadables/readline_filter_bun_bridge.so bash
 */
import { ReadlineFilter, Transform } from "./readline-filter";

ReadlineFilter.create({
  plugins: [
    Transform((line) => line.toUpperCase()),
  ],
});
