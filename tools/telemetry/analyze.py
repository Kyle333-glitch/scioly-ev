#!/usr/bin/env python3
"""Entry point for the telemetry analysis tooling.

Run it from the repository root:

    python tools/telemetry/analyze.py synth
    python tools/telemetry/analyze.py analyze data/raw/session1.log

Standard library only, so no environment needs setting up first. See
``tools/telemetry/README.md`` for the workflow and for what a report does and
does not mean.
"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from evtelemetry.cli import main  # noqa: E402  (path set up above, by design)

if __name__ == "__main__":
    raise SystemExit(main())
