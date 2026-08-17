#!/bin/sh
# Build and run the sense-parser tests.
#
# parse_sense() is extracted straight out of mega_progress.c rather than copied,
# so the code under test is always the code that ships. The tools themselves
# need a real MegaRAID controller, so this parser is the only part that can be
# meaningfully tested off hardware - which is exactly why it is tested.
set -e

cd "$(dirname "$0")"

sed -n '/^static int parse_sense/,/^}/p' mega_progress.c > parse_sense.inc
if [ ! -s parse_sense.inc ]; then
    echo "FAIL: could not extract parse_sense() from mega_progress.c" >&2
    exit 1
fi

${CC:-cc} -O2 -Wall -Wextra -o test_parse_sense test_parse_sense.c
./test_parse_sense
