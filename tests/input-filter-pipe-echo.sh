#!/bin/bash
# Echo filter — returns the line unchanged.  For testing the pipe protocol.
while IFS=$'\t' read -r keyseq point end mark hex_line; do
    printf '%s\t%s\t%s\n' "$point" "$mark" "$hex_line"
done
