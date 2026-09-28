"""Shared helpers for the tooling tests."""

from __future__ import annotations

import os
import sys

_TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)

from evtelemetry import cli  # noqa: E402
from evtelemetry.metrics import compute_metrics  # noqa: E402
from evtelemetry.parse import Log, parse_lines  # noqa: E402
from evtelemetry.synthetic import SPECS, SyntheticSpec, generate_lines  # noqa: E402

REPO_ROOT = cli.REPO_ROOT
SAMPLES_DIR = cli.DEFAULT_SAMPLES_DIR
FIRMWARE_LIB = os.path.join(REPO_ROOT, "lib", "evcore", "src")


def spec(run_id: str) -> SyntheticSpec:
    for candidate in SPECS:
        if candidate.run_id == run_id:
            return candidate
    raise KeyError(run_id)


def parse_spec(specification: SyntheticSpec) -> Log:
    """The synthetic capture parsed, as the analyzer sees it."""
    return parse_lines(generate_lines(specification), source=f"<{specification.run_id}>")


def metrics_for(specification: SyntheticSpec) -> dict:
    return compute_metrics(parse_spec(specification), specification.run_id)


def sample_path(name: str) -> str:
    return os.path.join(SAMPLES_DIR, name)
