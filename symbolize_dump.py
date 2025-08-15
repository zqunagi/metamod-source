#!/usr/bin/env python3
import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def parse_debug_id(sym_first_line: str) -> str:
    parts = sym_first_line.strip().split()
    if len(parts) < 5 or parts[0] != "MODULE":
        raise ValueError(f"Unexpected .sym header format: {sym_first_line.strip()}")
    return parts[3]


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate Breakpad symbols from a .so and stackwalk a minidump."
    )
    parser.add_argument("so_path", help="Path to the .so file")
    parser.add_argument("dump_path", help="Path to the .dmp file")
    parser.add_argument(
        "--symbols-dir",
        default=str(Path.cwd() / "symbols"),
        help="Symbols directory (default: ./symbols)",
    )
    parser.add_argument(
        "--out",
        default=None,
        help="Output .txt file (default: <dump_basename>.txt next to the dump)",
    )
    args = parser.parse_args()

    so_path = Path(args.so_path).resolve()
    dump_path = Path(args.dump_path).resolve()
    symbols_dir = Path(args.symbols_dir).resolve()

    # Ensure required tools are available
    for tool in ("dump_syms", "minidump_stackwalk"):
        if shutil.which(tool) is None:
            print(f"Error: required tool '{tool}' not found in PATH.", file=sys.stderr)
            sys.exit(1)

    if not so_path.is_file():
        print(f"Error: .so file not found: {so_path}", file=sys.stderr)
        sys.exit(1)
    if not dump_path.is_file():
        print(f"Error: dump file not found: {dump_path}", file=sys.stderr)
        sys.exit(1)

    so_basename = so_path.name
    sym_tmp_path = Path.cwd() / f"{so_basename}.sym"

    # Generate .sym file
    with sym_tmp_path.open("w", encoding="utf-8") as sym_fp:
        subprocess.run(["dump_syms", str(so_path)], check=True, stdout=sym_fp)

    # Read first line to get MODULE header/debug id
    with sym_tmp_path.open("r", encoding="utf-8", errors="replace") as sym_fp:
        first_line = sym_fp.readline()

    debug_id = parse_debug_id(first_line)
    print(f"MODULE line: {first_line.strip()}")

    # Prepare symbol store path: symbols/<module>/<debug_id>/<module>.sym
    target_dir = symbols_dir / so_basename / debug_id
    target_dir.mkdir(parents=True, exist_ok=True)
    target_sym_path = target_dir / f"{so_basename}.sym"

    # Move the .sym file into the symbol store
    sym_tmp_path.replace(target_sym_path)

    # Determine output path
    if args.out:
        out_path = Path(args.out).resolve()
        out_path.parent.mkdir(parents=True, exist_ok=True)
    else:
        out_path = dump_path.with_suffix(".txt")

    # Run stackwalk
    with out_path.open("w", encoding="utf-8") as out_fp:
        subprocess.run(
            ["minidump_stackwalk", str(dump_path), str(symbols_dir)],
            check=True,
            stdout=out_fp,
        )

    print(f"Stack trace written to: {out_path}")


if __name__ == "__main__":
    main()