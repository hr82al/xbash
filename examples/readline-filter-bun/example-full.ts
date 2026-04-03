#!/usr/bin/env bun
/**
 * Full-featured filter composing multiple plugins.
 *
 * Features:
 *   - Fuzzy history search (Up/Down)
 *   - Auto-correct typos
 *   - @mention completions with colored display
 *   - Inline command suggestions (ghost text)
 *
 *   bun run example-full.ts &
 *   READLINE_INPUT_FILTER_LIB=../loadables/readline_filter_bun_bridge.so bash
 */
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
    // Fuzzy history navigation with arrows
    HistorySearch({ fuzzy: true }),

    // Fix common typos on the fly
    AutoCorrect({
      "teh ": "the ",
      "adn ": "and ",
      "taht ": "that ",
      "waht ": "what ",
      "hte ": "the ",
    }),

    // @mention Tab completions
    Completions("@", [
      "@alice", "@bob", "@charlie", "@dave",
      "@eve", "@frank", "@grace", "@heidi",
    ]),

    // Inline ghost-text suggestions
    Suggestions([
      "git status",
      "git commit -m \"\"",
      "git push origin main",
      "git pull origin main",
      "git log --oneline -10",
      "docker compose up -d",
      "docker ps",
      "make test",
      "bun run dev",
    ]),

    // Colored bullet display for completions
    ColorDisplay({ bullet: "→", color: "cyan" }),
  ],

  onStart(s) {
    console.error(`[full-filter] listening on ${s}`);
  },
});
