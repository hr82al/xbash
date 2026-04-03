/**
 * readline-filter — React-style Bun library for bash readline customization.
 *
 * Composable plugins with hooks for state management.
 * Communicates with readline_filter_bun_bridge.so via Unix domain socket.
 *
 * @example
 * ```ts
 * import { ReadlineFilter, HistorySearch, AutoCorrect, Completions } from "./readline-filter";
 *
 * ReadlineFilter.create({
 *   plugins: [
 *     HistorySearch({ fuzzy: true }),
 *     AutoCorrect({ "teh ": "the " }),
 *     Completions("@", ["@alice", "@bob"]),
 *   ],
 * });
 * ```
 */

import { unlinkSync } from "fs";
import { readFileSync } from "fs";
import { homedir } from "os";

// ================================================================== //
//  Keys                                                               //
// ================================================================== //

export type KeySeq = number[];

export const Keys = {
  UP:        [27, 91, 65],
  DOWN:      [27, 91, 66],
  RIGHT:     [27, 91, 67],
  LEFT:      [27, 91, 68],
  HOME:      [27, 91, 72],
  END:       [27, 91, 70],
  DELETE:    [27, 91, 51, 126],
  ENTER:     [13],
  TAB:       [9],
  BACKSPACE: [127],
  CTRL_A: [1], CTRL_B: [2], CTRL_C: [3], CTRL_D: [4], CTRL_E: [5],
  CTRL_F: [6], CTRL_K: [11], CTRL_L: [12], CTRL_N: [14], CTRL_P: [16],
  CTRL_R: [18], CTRL_U: [21], CTRL_W: [23],
} as const;

export function keyIs(keyseq: KeySeq, key: readonly number[]): boolean {
  return keyseq.length === key.length && keyseq.every((b, i) => b === key[i]);
}

export function keyChar(keyseq: KeySeq): string | null {
  return keyseq.length === 1 && keyseq[0] >= 32 && keyseq[0] < 127
    ? String.fromCharCode(keyseq[0]) : null;
}

// ================================================================== //
//  Event types                                                        //
// ================================================================== //

export interface KeypressEvent {
  keyseq: KeySeq;
  line: string;
  point: number;
  mark: number;
}

export interface LineState {
  line: string;
  point: number;
  mark: number;
}

export interface CompleteEvent {
  line: string;
  point: number;
  word: string;
  wordStart: number;
  wordEnd: number;
}

export interface DisplayEvent {
  matches: string[];
  numMatches: number;
  maxLen: number;
}

// ================================================================== //
//  Hooks system                                                       //
// ================================================================== //

type KeypressHandler = (e: KeypressEvent) => LineState | null | void;
type CompleteHandler = (e: CompleteEvent) => string[] | null | void;
type DisplayHandler = (e: DisplayEvent) => void;

interface HookStore {
  state: Map<number, any>;
  refs: Map<number, { current: any }>;
  effects: Array<{ setup: () => (() => void) | void; cleanup?: () => void }>;
  keypressHandlers: KeypressHandler[];
  completeHandlers: CompleteHandler[];
  displayHandlers: DisplayHandler[];
  hookIndex: number;
}

let _currentHooks: HookStore | null = null;

function getHooks(): HookStore {
  if (!_currentHooks) throw new Error("Hooks can only be called inside a plugin");
  return _currentHooks;
}

/**
 * useState — React-style state hook.
 * Preserves state across keypress invocations within a plugin.
 */
export function useState<T>(initial: T): [T, (v: T | ((prev: T) => T)) => void] {
  const hooks = getHooks();
  const idx = hooks.hookIndex++;

  if (!hooks.state.has(idx)) {
    hooks.state.set(idx, initial);
  }

  const value = hooks.state.get(idx) as T;
  const setter = (v: T | ((prev: T) => T)) => {
    const next = typeof v === "function" ? (v as (prev: T) => T)(hooks.state.get(idx)) : v;
    hooks.state.set(idx, next);
  };

  return [value, setter];
}

/**
 * useRef — React-style ref hook.
 * Mutable container that persists across invocations.
 */
export function useRef<T>(initial: T): { current: T } {
  const hooks = getHooks();
  const idx = hooks.hookIndex++;

  if (!hooks.refs.has(idx)) {
    hooks.refs.set(idx, { current: initial });
  }

  return hooks.refs.get(idx)!;
}

/**
 * useKeypress — Register a keypress handler.
 * Multiple handlers run in order; first non-null result wins.
 */
export function useKeypress(handler: KeypressHandler): void {
  const hooks = getHooks();
  hooks.keypressHandlers.push(handler);
}

/**
 * useKey — Register a handler for a specific key.
 */
export function useKey(
  key: readonly number[],
  handler: (e: KeypressEvent) => LineState | null | void,
): void {
  useKeypress((e) => keyIs(e.keyseq, key) ? handler(e) : null);
}

/**
 * useCompletion — Register a Tab completion handler.
 * Multiple handlers run in order; first non-null result wins.
 */
export function useCompletion(handler: CompleteHandler): void {
  const hooks = getHooks();
  hooks.completeHandlers.push(handler);
}

/**
 * useDisplay — Register a custom completion display handler.
 * Last registered handler wins.
 */
export function useDisplay(handler: DisplayHandler): void {
  const hooks = getHooks();
  hooks.displayHandlers.push(handler);
}

/**
 * useEffect — Run a setup function once (at plugin init).
 * Return a cleanup function to run on teardown.
 */
export function useEffect(setup: () => (() => void) | void): void {
  const hooks = getHooks();
  hooks.effects.push({ setup });
}

// ================================================================== //
//  Plugin type                                                        //
// ================================================================== //

/**
 * A Plugin is a function that registers hooks.
 * Like a React component, it runs once to set up its handlers.
 */
export type Plugin = () => void;

// ================================================================== //
//  Built-in plugins                                                   //
// ================================================================== //

/**
 * HistorySearch — Fuzzy Up/Down arrow history navigation.
 *
 * @example
 * ```ts
 * HistorySearch()                           // defaults
 * HistorySearch({ fuzzy: true })            // fuzzy matching
 * HistorySearch({ file: "~/.zsh_history" }) // custom history file
 * ```
 */
export function HistorySearch(opts?: {
  file?: string;
  fuzzy?: boolean;
  maxEntries?: number;
}): Plugin {
  return () => {
    const file = opts?.file ?? `${homedir()}/.bash_history`;
    const fuzzy = opts?.fuzzy ?? true;
    const maxEntries = opts?.maxEntries ?? 10000;

    const fileHistory = useRef<string[]>([]);
    const session = useRef<string[]>([]);
    const query = useRef("");
    const matches = useRef<string[]>([]);
    const idx = useRef(0);
    const nav = useRef(false);

    useEffect(() => {
      try {
        const raw = readFileSync(file, "utf-8");
        const lines = raw.split("\n").filter((l) => l.trim().length > 0);
        const seen = new Set<string>();
        const unique: string[] = [];
        for (let i = lines.length - 1; i >= 0 && unique.length < maxEntries; i--) {
          if (!seen.has(lines[i])) {
            seen.add(lines[i]);
            unique.unshift(lines[i]);
          }
        }
        fileHistory.current = unique;
      } catch {}
    });

    useKey(Keys.UP, ({ line }) => {
      if (!nav.current) {
        query.current = line;
        nav.current = true;
        const all = [...fileHistory.current, ...session.current];
        const q = query.current.toLowerCase();
        matches.current = fuzzy && q
          ? all.filter((h) => h.toLowerCase().includes(q))
          : all;
        idx.current = matches.current.length;
      }
      if (matches.current.length > 0 && idx.current > 0) {
        idx.current--;
        const entry = matches.current[idx.current];
        return { line: entry, point: entry.length, mark: 0 };
      }
      return null;
    });

    useKey(Keys.DOWN, () => {
      if (!nav.current) return null;
      idx.current++;
      if (idx.current < matches.current.length) {
        const entry = matches.current[idx.current];
        return { line: entry, point: entry.length, mark: 0 };
      }
      nav.current = false;
      return { line: query.current, point: query.current.length, mark: 0 };
    });

    useKey(Keys.ENTER, ({ line }) => {
      const trimmed = line.trim();
      if (trimmed) session.current.push(trimmed);
      nav.current = false;
      return null;
    });

    // Any other key stops navigation
    useKeypress(({ keyseq }) => {
      if (nav.current && !keyIs(keyseq, Keys.UP) && !keyIs(keyseq, Keys.DOWN) && !keyIs(keyseq, Keys.ENTER)) {
        nav.current = false;
      }
      return null;
    });
  };
}

/**
 * AutoCorrect — Fix typos as you type.
 *
 * @example
 * ```ts
 * AutoCorrect({ "teh ": "the ", "adn ": "and " })
 * ```
 */
export function AutoCorrect(rules: Record<string, string>): Plugin {
  return () => {
    useKeypress(({ line, point, mark }) => {
      let result = line;
      let changed = false;
      for (const [typo, fix] of Object.entries(rules)) {
        if (result.includes(typo)) {
          result = result.replaceAll(typo, fix);
          changed = true;
        }
      }
      if (changed) {
        return { line: result, point: Math.min(point, Buffer.byteLength(result)), mark };
      }
      return null;
    });
  };
}

/**
 * Completions — Custom Tab completions for a prefix.
 *
 * @example
 * ```ts
 * Completions("@", ["@alice", "@bob", "@charlie"])
 * Completions("--", () => ["--verbose", "--help", "--version"])
 * ```
 */
export function Completions(
  prefix: string,
  items: string[] | (() => string[]),
): Plugin {
  return () => {
    useCompletion(({ word }) => {
      if (!word.startsWith(prefix)) return null;
      const list = typeof items === "function" ? items() : items;
      const matching = list.filter((item) =>
        item.toLowerCase().startsWith(word.toLowerCase()),
      );
      if (matching.length === 0) return null;
      return [lcd(matching), ...matching];
    });
  };
}

/**
 * Suggestions — Inline ghost-text command suggestions.
 * Shows the first matching suggestion while keeping cursor at user's position.
 *
 * @example
 * ```ts
 * Suggestions(["git status", "git commit -m", "docker ps"])
 * ```
 */
export function Suggestions(commands: string[]): Plugin {
  return () => {
    useKeypress(({ line, point, mark }) => {
      if (line.length === 0 || point !== line.length) return null;
      const match = commands.find((cmd) => cmd.startsWith(line) && cmd !== line);
      if (match) return { line: match, point, mark };
      return null;
    });
  };
}

/**
 * ColorDisplay — Colored bullet-list completion display.
 *
 * @example
 * ```ts
 * ColorDisplay()
 * ColorDisplay({ bullet: "→", color: "green" })
 * ```
 */
export function ColorDisplay(opts?: {
  bullet?: string;
  color?: "red" | "green" | "yellow" | "blue" | "magenta" | "cyan" | "white";
}): Plugin {
  const colors: Record<string, string> = {
    red: "31", green: "32", yellow: "33", blue: "34",
    magenta: "35", cyan: "36", white: "37",
  };
  return () => {
    const bullet = opts?.bullet ?? "●";
    const code = colors[opts?.color ?? "cyan"] ?? "36";
    useDisplay(({ matches, numMatches }) => {
      process.stderr.write("\n");
      for (let i = 1; i <= numMatches; i++) {
        process.stderr.write(`  \x1b[${code}m${bullet}\x1b[0m ${matches[i]}\n`);
      }
    });
  };
}

/**
 * Transform — Apply a transform to every keypress.
 *
 * @example
 * ```ts
 * Transform((line) => line.toUpperCase())
 * Transform((line) => line.replace(/\s+/g, " "))
 * ```
 */
export function Transform(fn: (line: string) => string): Plugin {
  return () => {
    useKeypress(({ line, point, mark }) => {
      const result = fn(line);
      if (result === line) return null;
      return { line: result, point: Math.min(point, Buffer.byteLength(result)), mark };
    });
  };
}

/**
 * KeyMap — Bind handlers to specific keys.
 *
 * @example
 * ```ts
 * KeyMap({
 *   [Keys.CTRL_U]: ({ line, point }) => ({ line: line.slice(point), point: 0, mark: 0 }),
 *   [Keys.CTRL_K]: ({ line, point }) => ({ line: line.slice(0, point), point, mark: 0 }),
 * })
 * ```
 */
export function KeyMap(bindings: Record<string, KeypressHandler>): Plugin {
  return () => {
    for (const [keyStr, handler] of Object.entries(bindings)) {
      const key = keyStr.split(",").map(Number);
      useKey(key, handler);
    }
  };
}

// ================================================================== //
//  ReadlineFilter — main entry point                                  //
// ================================================================== //

export interface FilterConfig {
  /** Plugins to compose (order matters — first match wins). */
  plugins: Plugin[];
  /** Unix socket path. Default: /tmp/readline-filter.sock */
  socket?: string;
  /** Called when server starts. */
  onStart?: (socket: string) => void;
  /** Called on errors. */
  onError?: (error: Error) => void;
}

export const ReadlineFilter = {
  /**
   * Create and start a readline filter server with composed plugins.
   *
   * @example
   * ```ts
   * ReadlineFilter.create({
   *   plugins: [
   *     HistorySearch({ fuzzy: true }),
   *     AutoCorrect({ "teh ": "the " }),
   *     Completions("@", ["@alice", "@bob"]),
   *     ColorDisplay(),
   *   ],
   * });
   * ```
   */
  create(config: FilterConfig): () => void {
    const socketPath = config.socket ?? "/tmp/readline-filter.sock";

    // Initialize all plugins and collect their hooks
    const stores: HookStore[] = [];
    for (const plugin of config.plugins) {
      const store: HookStore = {
        state: new Map(),
        refs: new Map(),
        effects: [],
        keypressHandlers: [],
        completeHandlers: [],
        displayHandlers: [],
        hookIndex: 0,
      };
      _currentHooks = store;
      plugin();
      _currentHooks = null;
      stores.push(store);
    }

    // Run effects
    for (const store of stores) {
      for (const effect of store.effects) {
        effect.cleanup = effect.setup() ?? undefined;
      }
    }

    // Collect all handlers (order preserved)
    const allKeypress: KeypressHandler[] = stores.flatMap((s) => s.keypressHandlers);
    const allComplete: CompleteHandler[] = stores.flatMap((s) => s.completeHandlers);
    const allDisplay: DisplayHandler[] = stores.flatMap((s) => s.displayHandlers);

    // Remove stale socket
    try { unlinkSync(socketPath); } catch {}

    const server = Bun.listen({
      unix: socketPath,
      socket: {
        async data(socket, rawData) {
          const text = rawData.toString();
          const lines = text.split("\n").filter((l: string) => l.trim().length > 0);

          for (const line of lines) {
            try {
              const req = JSON.parse(line);
              let resp: string;

              switch (req.type) {
                case "keypress":
                  resp = handleKeypress(allKeypress, req);
                  break;
                case "complete":
                  resp = handleComplete(allComplete, req);
                  break;
                case "display":
                  handleDisplay(allDisplay, req);
                  resp = "{}";
                  break;
                default:
                  resp = "{}";
              }

              socket.write(resp + "\n");
            } catch (err) {
              config.onError?.(err instanceof Error ? err : new Error(String(err)));
              socket.write("{}\n");
            }
          }
        },
        open() {},
        close() {},
        error(_, err) { config.onError?.(err); },
      },
    });

    config.onStart?.(socketPath);

    return () => {
      // Run effect cleanups
      for (const store of stores) {
        for (const effect of store.effects) {
          effect.cleanup?.();
        }
      }
      server.stop();
      try { unlinkSync(socketPath); } catch {}
    };
  },
};

// ================================================================== //
//  Internal dispatch                                                  //
// ================================================================== //

function handleKeypress(handlers: KeypressHandler[], req: any): string {
  const event: KeypressEvent = {
    keyseq: req.keyseq ?? [],
    line: req.line ?? "",
    point: req.point ?? 0,
    mark: req.mark ?? 0,
  };

  for (const handler of handlers) {
    const result = handler(event);
    if (result) {
      return JSON.stringify({
        modified: true,
        line: result.line,
        point: result.point,
        mark: result.mark,
      });
    }
  }

  return '{"modified":false}';
}

function handleComplete(handlers: CompleteHandler[], req: any): string {
  const event: CompleteEvent = {
    line: req.line ?? "",
    point: req.point ?? 0,
    word: req.word ?? "",
    wordStart: req.wordStart ?? 0,
    wordEnd: req.wordEnd ?? 0,
  };

  for (const handler of handlers) {
    const result = handler(event);
    if (result && result.length > 0) {
      return JSON.stringify({ matches: result });
    }
  }

  return '{"matches":null}';
}

function handleDisplay(handlers: DisplayHandler[], req: any): void {
  if (handlers.length === 0) return;
  const event: DisplayEvent = {
    matches: req.matches ?? [],
    numMatches: req.numMatches ?? 0,
    maxLen: req.maxLen ?? 0,
  };
  // Last registered display handler wins
  handlers[handlers.length - 1](event);
}

// ================================================================== //
//  Utilities                                                          //
// ================================================================== //

/** Compute longest common prefix of strings. */
function lcd(strings: string[]): string {
  if (strings.length === 0) return "";
  let prefix = strings[0];
  for (const s of strings.slice(1)) {
    let i = 0;
    while (i < prefix.length && i < s.length && prefix[i] === s[i]) i++;
    prefix = prefix.slice(0, i);
  }
  return prefix;
}
