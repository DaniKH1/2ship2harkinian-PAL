"""Command line interface for ml64_to_soh."""
from __future__ import annotations
import argparse
import os
import sys

from . import pak, manifest


def _load_manifest(path: str):
    files = pak.load_map(path)
    man = manifest.from_pak_files(files)
    return files, man


def cmd_identify(args):
    for path in args.paks:
        print("=" * 70)
        print(os.path.basename(path))
        if not pak.is_pak(path):
            print("  NOT a ModLoader64 pak")
            continue
        files, man = _load_manifest(path)
        zobjs = sorted(k for k in files if k.lower().endswith(".zobj"))
        if man is None:
            print("  (no package.json) zobjs:", zobjs)
            continue
        print(man.summary())
        if args.verbose:
            print("  files:")
            for k in sorted(files):
                print(f"    {len(files[k]):8d}  {k}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="ml64_to_soh",
                                 description="Convert ModLoader64 model paks to SoH/2Ship .o2r")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_id = sub.add_parser("identify", help="report what a pak contains")
    p_id.add_argument("paks", nargs="+")
    p_id.add_argument("-v", "--verbose", action="store_true")
    p_id.set_defaults(func=cmd_identify)

    # convert / render subcommands are registered lazily (heavier deps)
    try:
        from . import convert as _convert
        _convert.register(sub)
    except Exception as e:  # pragma: no cover - convert not ready yet
        if os.environ.get("ML64_DEBUG"):
            print("convert unavailable:", e, file=sys.stderr)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
