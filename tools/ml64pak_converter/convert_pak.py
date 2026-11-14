import argparse
from pathlib import Path

from ml64_gui import analyze


def main():
    parser = argparse.ArgumentParser(description="Convert supported ModLoader64 .pak mods to 2Ship .o2r archives.")
    parser.add_argument("--pak", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--game", choices=("OOT", "MM"))
    args = parser.parse_args()

    if not args.pak.is_file():
        parser.error(f"Input .pak does not exist: {args.pak}")
    args.out_dir.mkdir(parents=True, exist_ok=True)

    summary, jobs = analyze(str(args.pak))
    convertible = [job for job in jobs if "skipped" not in job.label.lower()]
    if args.game:
        convertible = [job for job in convertible if args.game in job.games]
    if not convertible:
        game_name = f" for {args.game}" if args.game else ""
        raise RuntimeError(f"No supported player model or equipment found{game_name} in {args.pak.name}")

    used_names = set()
    output_paths = []
    try:
        for index, job in enumerate(convertible, start=1):
            stem = job.stem or f"asset_{index}"
            name = stem
            suffix = 2
            while name.casefold() in used_names:
                name = f"{stem}_{suffix}"
                suffix += 1
            used_names.add(name.casefold())

            output_path = args.out_dir / f"{name}.o2r"
            if output_path.exists():
                raise RuntimeError(f"Refusing to overwrite an existing archive: {output_path}")
            output_paths.append(output_path)
            if args.game and job.produce_for_game is not None:
                stats = job.produce_for_game(output_path, args.game)
            else:
                stats = job.produce(output_path)
            if not stats.get("ok"):
                raise RuntimeError(f"{job.label}: {stats.get('reason', 'conversion failed')}")
            print(f"Converted {job.label} -> {output_path.name}")
    except Exception:
        for output_path in output_paths:
            if output_path.exists():
                output_path.unlink()
        raise

    print(f"Converted {len(convertible)} item(s) from {summary.get('name', args.pak.name)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"Conversion failed: {error}", flush=True)
        raise SystemExit(1)
