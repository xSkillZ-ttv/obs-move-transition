#!/usr/bin/env python3
"""Compile focused tests against the actual Move filter lifecycle functions.

Requires Python 3 and a C11 compiler. Example with Zig:
  python tests/test_runtime_regressions.py --cc path/to/zig --cc-arg cc

OBS/network doubles isolate trigger registration, destruction and a bind
failure. These tests do not replace integration testing inside OBS.
"""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def extract_block(source, signature, trailing_semicolon=False):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1 + trailing_semicolon]
    raise ValueError(f"Unterminated block: {signature}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", help="C11 compiler executable")
    parser.add_argument("--cc-arg", action="append", default=[], help="Compiler driver argument, e.g. cc for Zig")
    parser.add_argument("--source-dir", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--case", choices=["all", "udp", "history", "bind", "lifecycle"], default="all")
    args = parser.parse_args()
    compiler = args.cc or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not compiler:
        parser.error("C compiler not found; pass --cc PATH")

    source = (args.source_dir / "move-filter.c").read_text(encoding="utf-8")
    header = (args.source_dir / "move-transition.h").read_text(encoding="utf-8")
    template = Path(__file__).with_name("runtime-regressions.c.in").read_text(encoding="utf-8")
    blocks = {
        "DEFINES": "\n".join(re.findall(r"^#define (?:S_[A-Z_]+|START_TRIGGER_UDP) .+$", header, re.MULTILINE)),
        "MOVE_FILTER": extract_block(header, "struct move_filter {", True),
        "UDP_SERVER": extract_block(source, "struct udp_server {", True),
        "UDP_TRIGGER": extract_block(source, "struct udp_trigger {", True),
        "UDP_CODE": source[source.index("static bool udp_socket_retry(void)") : source.index("void move_filter_update(")],
        "DESTROY": extract_block(source, "void move_filter_destroy(struct move_filter *move_filter)"),
        "UPDATE": extract_block(source, "void move_filter_update(struct move_filter *move_filter, obs_data_t *settings)"),
    }
    for name, block in blocks.items():
        template = template.replace(f"@{name}@", block)

    with tempfile.TemporaryDirectory(prefix="move-runtime-tests-") as directory:
        test_source = Path(directory) / "runtime-regressions.c"
        executable = Path(directory) / "runtime-regressions.exe"
        test_source.write_text(template, encoding="utf-8")
        subprocess.run(
            [compiler, *args.cc_arg, "-std=c11", "-Werror=implicit-function-declaration", str(test_source), "-o", str(executable)],
            check=True,
        )
        subprocess.run([str(executable), args.case], check=True)


if __name__ == "__main__":
    main()
