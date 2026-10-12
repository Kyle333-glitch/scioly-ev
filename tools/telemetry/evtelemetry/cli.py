"""The command line: ``synth`` for fixtures, ``analyze`` for a capture,
``distribution`` for a series of hand measurements.

The interface is a few subcommands on purpose. There is no daemon, no notebook,
no server and no dashboard: the workflow is "generate the fixtures once, run the
analyzer over a capture, and compute the series distribution from the hand
measurements". `AGENTS.md` section 17 asks for diagnostics that are inspectable,
and a directory of text and SVG is about as inspectable as it gets.

Exit codes are meaningful so the tool can be used from a script:

* ``0`` - it worked;
* ``1`` - the command line was wrong;
* ``2`` - the input could not be read or could not be parsed.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from typing import NoReturn, Optional, Sequence

from . import TOOL_VERSION, schema
from .distribution import measurement_distribution
from .endpoints import MeasurementError, MEASUREMENT_TEMPLATE, compare_endpoints, load_measurements
from .metrics import compute_metrics
from .parse import TelemetryFormatError, read_log
from .report import (
    ReportError,
    is_synthetic,
    reconcile_root_reports,
    reconcile_stale_run_dirs,
    run_report,
    summarize_distribution,
    write_comparison_report,
    write_distribution_report,
    write_endpoints_report,
    write_manifest,
    summarize_comparison,
    summarize_run,
)
from .synthetic import SPECS, SyntheticWriteError, write_samples

__all__ = ["main", "DEFAULT_REPORT_DIR", "DEFAULT_SAMPLES_DIR", "REPO_ROOT"]

# Defaults are anchored to the repository root rather than to the current
# directory. A relative default would silently mean something different if the
# tool were run from another directory, and the fixture writer would happily
# create a second `tools/telemetry/samples` inside whatever directory it was
# started from - which is exactly the kind of surprise a tool that writes files
# must not have.
_HERE = os.path.dirname(os.path.abspath(__file__))  # <repo>/tools/telemetry/evtelemetry
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(_HERE)))

# The synthetic fixtures are committed, so their default location is fixed.
DEFAULT_SAMPLES_DIR = os.path.join(REPO_ROOT, "tools", "telemetry", "samples")

# Generated reports live under `data/`, which `.gitignore` keeps out of the
# repository: a report is an artifact, the raw capture is the record.
DEFAULT_REPORT_DIR = os.path.join(REPO_ROOT, "data", "reports", "analysis")

_RUN_ID_SAFE = re.compile(r"[^A-Za-z0-9._-]+")

_EXIT_OK = 0
_EXIT_USAGE = 1
_EXIT_INPUT = 2


class CliError(Exception):
    """A problem the operator can act on, reported without a stack trace."""


class _Parser(argparse.ArgumentParser):
    """An ArgumentParser whose errors exit with the documented usage code.

    `argparse` exits 2 on a bad command line, which would collide with this
    tool's "the input could not be parsed" code. Making the command-line case
    exit 1 keeps the documented contract true, so a script can tell a mistyped
    invocation from a log it cannot read.
    """

    def error(self, message: str) -> "NoReturn":  # type: ignore[override]
        self.print_usage(sys.stderr)
        print(f"error: {message}", file=sys.stderr)
        raise SystemExit(_EXIT_USAGE)


def build_parser() -> argparse.ArgumentParser:
    parser = _Parser(
        prog="analyze.py",
        description=(
            "Analyze captured telemetry from the Science Olympiad EV firmware, and "
            "generate synthetic fixtures to validate the tooling with."
        ),
        epilog=(
            "A log says what the firmware believed. It is not evidence about the "
            "vehicle: physical facts belong in VERIFIED_STATE.md and need measurement."
        ),
    )
    parser.add_argument("--version", action="version", version=f"telemetry tools {TOOL_VERSION}")
    subparsers = parser.add_subparsers(dest="command", required=True)

    synth = subparsers.add_parser(
        "synth",
        help="write the synthetic sample captures (fixtures, not data)",
        description=(
            "Writes deterministic synthetic captures under the given directory. They exist "
            "to validate the parser and the plots before any real log exists."
        ),
    )
    synth.add_argument(
        "--out",
        default=DEFAULT_SAMPLES_DIR,
        help=f"directory to write the fixtures into (default: {DEFAULT_SAMPLES_DIR})",
    )
    synth.add_argument("--force", action="store_true", help="rewrite files that already exist")

    analyze = subparsers.add_parser(
        "analyze",
        help="parse captured logs and write metrics, summaries and plots",
        description=(
            "Parses one or more captured logs, computes descriptive metrics, writes a "
            "report per run, and compares runs when more than one is given."
        ),
    )
    analyze.add_argument("logs", nargs="+", help="captured log file(s)")
    analyze.add_argument(
        "--out",
        default=DEFAULT_REPORT_DIR,
        help=f"report directory to write into (default: {DEFAULT_REPORT_DIR})",
    )
    analyze.add_argument(
        "--run-id",
        action="append",
        default=[],
        metavar="ID",
        help=(
            "run id for the corresponding log, repeatable and positional; the default is "
            "the log file's name without its extension"
        ),
    )
    analyze.add_argument(
        "--measurements",
        metavar="FILE",
        help="CSV of physical measurements taken by hand, for the endpoint summary",
    )
    analyze.add_argument(
        "--print-measurement-template",
        action="store_true",
        help="print the measurement CSV template and exit",
    )
    analyze.add_argument("--no-plots", action="store_true", help="write metrics without SVG plots")
    analyze.add_argument(
        "--continue-on-error",
        action="store_true",
        help=(
            "record unreadable lines and keep going instead of refusing the log; metrics "
            "then describe a partial capture, and the report says so"
        ),
    )

    distribution = subparsers.add_parser(
        "distribution",
        help="describe the spread across a series of hand measurements",
        description=(
            "Reads the hand-measurement CSV and reports the distribution of the accepted "
            "runs: mean, median and spread, and the residual against each run's recorded "
            "target. It is arithmetic over a person's measurements, not a measurement."
        ),
    )
    distribution.add_argument(
        "measurements", help="CSV of physical measurements taken by hand"
    )
    distribution.add_argument(
        "--out",
        default=DEFAULT_REPORT_DIR,
        help=f"report directory to write into (default: {DEFAULT_REPORT_DIR})",
    )
    return parser


def _run_id_for(path: str, index: int, provided: Sequence[str]) -> str:
    if index < len(provided) and provided[index].strip():
        return provided[index].strip()
    return os.path.splitext(os.path.basename(path))[0]


def _safe_run_name(run_id: str) -> str:
    """The directory name a run id is written under. Not one-to-one."""
    return _RUN_ID_SAFE.sub("_", run_id).strip("._-") or "run"


def _preflight_run_dirs(out_dir: str, run_ids: Sequence[str]) -> list[str]:
    """One output directory per run id, or a refusal naming the collision.

    Naming is lossy: characters outside ``[A-Za-z0-9._-]`` fold to ``_``, and on
    Windows a path is case-insensitive and ignores trailing dots and spaces, so
    distinct inputs can map to one directory - and the later report then silently
    replaces the earlier one, while the manifest lists both. This is checked for
    every input *before anything is written*, and the comparison is folded the way
    a case-insensitive filesystem folds, because a report tree generated on one
    machine is read on another.
    """
    dirs: list[str] = []
    owners: dict[str, str] = {}
    for run_id in run_ids:
        safe = _safe_run_name(run_id)
        key = safe.casefold().rstrip(" .")
        previous = owners.get(key)
        if previous is not None:
            raise CliError(
                f"run ids {previous!r} and {run_id!r} would both write into "
                f"{os.path.join(out_dir, 'runs', safe)!r} (the directory naming is "
                "not one-to-one); give one of them a distinct --run-id so the two "
                "reports do not replace each other"
            )
        owners[key] = run_id
        dirs.append(os.path.join(out_dir, "runs", safe))
    return dirs


def _run_dir(out_dir: str, run_id: str) -> str:
    return os.path.join(out_dir, "runs", _safe_run_name(run_id))


def _cmd_synth(args: argparse.Namespace) -> int:
    try:
        written = write_samples(args.out, force=args.force)
    except SyntheticWriteError as error:
        raise CliError(str(error)) from None
    print(f"wrote {len(written)} SYNTHETIC fixture(s) into {args.out}")
    for path in written:
        print(f"  {path}")
    print("")
    print(
        "These files are generated fixtures. They are not measured data and must never be "
        "cited as evidence about the vehicle."
    )
    return _EXIT_OK


def _cmd_distribution(args: argparse.Namespace) -> int:
    try:
        table = load_measurements(args.measurements)
    except FileNotFoundError:
        raise CliError(f"no such measurement file: {args.measurements}") from None
    except MeasurementError as error:
        raise CliError(f"the measurement file could not be read\n{error}") from None

    payload = measurement_distribution(table)
    try:
        write_distribution_report(args.out, payload)
    except ReportError as error:
        raise CliError(str(error)) from None

    print(summarize_distribution(payload))
    print(f"distribution written to {os.path.join(args.out, 'distribution.txt')}")
    return _EXIT_OK


def _cmd_analyze(args: argparse.Namespace) -> int:
    if args.print_measurement_template:
        sys.stdout.write(MEASUREMENT_TEMPLATE)
        return _EXIT_OK

    # Every input is validated, read and analyzed *before* anything is written.
    # Two logs are often one capture split in two, and a report tree that contains
    # run 1 while run 2 failed to parse (with no manifest) is a tree that looks
    # finished and is not.
    run_ids = [
        _run_id_for(path, index, args.run_id) for index, path in enumerate(args.logs)
    ]
    run_dirs = _preflight_run_dirs(args.out, run_ids)

    prepared = []
    for index, path in enumerate(args.logs):
        try:
            log = read_log(path, continue_on_error=args.continue_on_error)
        except FileNotFoundError:
            raise CliError(f"no such log file: {path}") from None
        except OSError as error:
            raise CliError(f"could not read {path}: {error}") from None
        except TelemetryFormatError as error:
            raise CliError(f"the log is not telemetry this tool understands\n{error}") from None
        prepared.append((run_ids[index], run_dirs[index], log, compute_metrics(log, run_ids[index])))

    table = None
    if args.measurements:
        try:
            table = load_measurements(args.measurements)
        except FileNotFoundError:
            raise CliError(f"no such measurement file: {args.measurements}") from None
        except MeasurementError as error:
            raise CliError(f"the measurement file could not be read\n{error}") from None

    metrics_by_run = []
    for run_id, run_dir, log, metrics in prepared:
        try:
            run_report(run_dir, metrics, log, include_plots=not args.no_plots)
        except ReportError as error:
            raise CliError(str(error)) from None

        metrics_by_run.append(metrics)
        print(summarize_run(metrics))
        print(f"report written to {run_dir}")
        print("")

    wrote_comparison = False
    if len(metrics_by_run) > 1:
        print(summarize_comparison(metrics_by_run))
        write_comparison_report(args.out, metrics_by_run)
        wrote_comparison = True

    wrote_endpoints = False
    if table is not None:
        endpoints_summary = compare_endpoints(metrics_by_run, table)
        try:
            write_endpoints_report(args.out, endpoints_summary)
        except ReportError as error:
            raise CliError(str(error)) from None
        wrote_endpoints = True
        print(
            f"endpoint comparison written to {os.path.join(args.out, 'endpoints.txt')} "
            f"({len(endpoints_summary['runs'])} run(s))"
        )
        refused = endpoints_summary["refused_comparisons"]
        for entry in refused:
            print(f"  no comparison for {entry['run_id']}: {entry['reason']}")
        print("")

    # M15: the new generation owns the root now. Managed root pairs the
    # current inputs did not produce are removed (fixed slots only), and run
    # directories no current input maps to are reconciled to their unknown
    # content or removed when empty. This runs after every current artifact is
    # written and before the manifest claims the tree, so a manifest never
    # describes a generation the directory contradicts. A failure before this
    # point (bad input, preflight collision, unreadable log) writes nothing
    # and therefore removes nothing.
    reconcile_root_reports(
        args.out,
        keep_comparison=wrote_comparison,
        keep_endpoints=wrote_endpoints,
    )
    reconcile_stale_run_dirs(args.out, run_dirs)

    write_manifest(
        args.out,
        {
            "tool_version": TOOL_VERSION,
            "schema_version": schema.SCHEMA_VERSION,
            "schema_prefixes": [schema.SAMPLE_PREFIX, schema.EVENT_PREFIX],
            "inputs": [
                {
                    "run_id": metrics["run_id"],
                    "path": metrics["source"]["path"],
                    "sha256": metrics["source"]["sha256"],
                    "samples": metrics["counts"]["samples"],
                    "events": metrics["counts"]["events"],
                    "parse_errors": metrics["counts"]["parse_errors"],
                    # Provenance and segmentation travel in the manifest too: they
                    # are what says whether a report may be compared with anything,
                    # and a manifest that omits them is how a fixture's numbers get
                    # quoted as a bench result.
                    "provenance_declared": metrics["capture"]["provenance_declared"],
                    "synthetic": is_synthetic(
                        metrics["run_id"],
                        metrics["source"]["path"],
                        metrics["capture"]["provenance_declared"],
                    ),
                    "sessions": metrics["capture"]["session_count"],
                    "analyzed_session_index": metrics["capture"]["selection"][
                        "selected_session_index"
                    ],
                    "runs_in_session": metrics["capture"]["selection"]["runs_in_session"],
                    "mixed_sessions": metrics["capture"]["selection"]["mixed_sessions"],
                    "mixed_runs": metrics["capture"]["selection"]["mixed_runs"],
                    "end_position_source": metrics["position"]["end_position_source"],
                    "stop_verified": metrics["run_window"]["final_stop_dwell_verified"],
                }
                for metrics in metrics_by_run
            ],
            "measurements_source": args.measurements,
            "plots_written": not args.no_plots,
            "notes": (
                "A report describes what the firmware reported. Physical facts are recorded in "
                "VERIFIED_STATE.md after measurement, never here."
            ),
        },
    )
    print(f"manifest written to {os.path.join(args.out, 'manifest.json')}")
    return _EXIT_OK


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        if args.command == "synth":
            return _cmd_synth(args)
        if args.command == "analyze":
            return _cmd_analyze(args)
        if args.command == "distribution":
            return _cmd_distribution(args)
    except CliError as error:
        print(f"error: {error}", file=sys.stderr)
        return _EXIT_INPUT
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return _EXIT_INPUT

    parser.print_usage(sys.stderr)
    return _EXIT_USAGE
