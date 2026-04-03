#!/usr/bin/env python3
"""Pipe filter that uppercases the line. For testing READLINE_INPUT_FILTER."""
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
