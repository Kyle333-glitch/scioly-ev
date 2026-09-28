"""A small, dependency-free SVG chart writer.

Plots have to be reproducible or they are not evidence of anything: a chart that
changes between two runs over the same log cannot be cited. This writer is
therefore deliberately narrow.

* **No dependencies.** Plain Python from the standard library, so the tooling
  runs on a machine that has nothing installed, which is what the project's
  ``AGENTS.md`` section 25 asks for before any library is added.
* **No timestamps, no randomness, no measured text widths.** Tick positions come
  from a fixed algorithm, every number is written with a fixed number of
  decimals, elements are emitted in a fixed order, and nothing consults a clock
  or the environment. The same input produces the same bytes.
* **No rendering engine.** SVG is text, so a reviewer can diff two charts.

The charts are plain line charts with optional phase bands, reference lines and
event markers. That is everything the telemetry needs; anything richer would be
a dashboard, which this stage is explicitly not building.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Optional, Sequence

__all__ = ["Series", "Band", "Marker", "Chart", "PALETTE"]

# A fixed palette. The order is part of the output, so it never changes.
PALETTE: tuple[str, ...] = (
    "#1f4f9c",
    "#c1440e",
    "#2f7d32",
    "#7b1fa2",
    "#00695c",
    "#b58900",
    "#455a64",
    "#ad1457",
)

_GRID_COLOR = "#d0d0d0"
_AXIS_COLOR = "#404040"
_BAND_COLOR = "#000000"
_MARKER_COLOR = "#8a1c1c"
_REFERENCE_COLOR = "#606060"
_TEXT_COLOR = "#202020"
_NOTE_COLOR = "#6a6a6a"

_LEFT = 76.0
_RIGHT = 196.0
_TOP = 46.0
_BOTTOM = 58.0


def _num(value: float) -> str:
    """Fixed-format number for the SVG, so the bytes never depend on the value's scale."""
    if not math.isfinite(value):
        return "0"
    # A coordinate that rounds away to nothing is written as zero, never "-0.000".
    rounded = round(value, 3)
    if rounded == 0.0:
        rounded = 0.0
    return f"{rounded:.3f}"


def _escape(text: str) -> str:
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def _nice_ticks(low: float, high: float, target: int = 6) -> tuple[list[float], float]:
    """A deterministic "nice" tick sequence for ``[low, high]``."""
    if not math.isfinite(low) or not math.isfinite(high):
        return [0.0, 1.0], 1.0
    if high <= low:
        high = low + 1.0
    span = high - low
    raw = span / max(1, target)
    magnitude = 10.0 ** math.floor(math.log10(raw)) if raw > 0 else 1.0
    normalized = raw / magnitude if magnitude > 0 else 1.0
    if normalized <= 1.0:
        step = magnitude
    elif normalized <= 2.0:
        step = 2.0 * magnitude
    elif normalized <= 5.0:
        step = 5.0 * magnitude
    else:
        step = 10.0 * magnitude
    if not math.isfinite(step) or step <= 0.0:
        step = span if span > 0 else 1.0

    start = math.floor(low / step) * step
    count = int(math.floor((high - start) / step)) + 1
    count = max(1, min(count, 60))
    return [start + index * step for index in range(count + 1)], step


def _tick_label(value: float, step: float) -> str:
    if step > 0:
        decimals = max(0, min(6, -int(math.floor(math.log10(step)))))
    else:
        decimals = 0
    rounded = round(value, decimals)
    if rounded == 0.0:
        rounded = 0.0  # a tick that rounds to nothing reads as "0", never "-0"
    return f"{rounded:.{decimals}f}"


@dataclass
class Series:
    """One line. ``ys`` may contain ``None`` or NaN, which break the line."""

    label: str
    xs: Sequence[float]
    ys: Sequence[Optional[float]]
    color: Optional[str] = None
    dashed: bool = False


@dataclass
class Band:
    """A shaded x-range, used for firmware phases."""

    start: float
    end: float
    label: str = ""


@dataclass
class Marker:
    """A vertical line at one x, used for events."""

    x: float
    label: str = ""


@dataclass
class Chart:
    """A line chart, built up and then rendered once."""

    title: str
    x_label: str
    y_label: str
    note: Optional[str] = None
    width: float = 960.0
    height: float = 420.0
    series: list[Series] = field(default_factory=list)
    bands: list[Band] = field(default_factory=list)
    markers: list[Marker] = field(default_factory=list)
    references: list[tuple[float, str]] = field(default_factory=list)
    y_min: Optional[float] = None
    y_max: Optional[float] = None
    x_min: Optional[float] = None
    x_max: Optional[float] = None

    # -- building ---------------------------------------------------------

    def add_series(
        self,
        label: str,
        xs: Sequence[float],
        ys: Sequence[Optional[float]],
        *,
        color: Optional[str] = None,
        dashed: bool = False,
    ) -> "Chart":
        if color is None:
            color = PALETTE[len(self.series) % len(PALETTE)]
        self.series.append(Series(label=label, xs=xs, ys=ys, color=color, dashed=dashed))
        return self

    def add_band(self, start: float, end: float, label: str = "") -> "Chart":
        self.bands.append(Band(start=start, end=end, label=label))
        return self

    def add_marker(self, x: float, label: str = "") -> "Chart":
        self.markers.append(Marker(x=x, label=label))
        return self

    def add_reference(self, y: float, label: str = "") -> "Chart":
        self.references.append((y, label))
        return self

    # -- rendering --------------------------------------------------------

    def _data_bounds(self) -> tuple[float, float, float, float]:
        xs: list[float] = []
        ys: list[float] = []
        for series in self.series:
            for x, y in zip(series.xs, series.ys):
                if y is None:
                    continue
                y_value = float(y)
                if not math.isfinite(y_value):
                    continue
                xs.append(float(x))
                ys.append(y_value)
        if not xs or not ys:
            return 0.0, 1.0, 0.0, 1.0

        x_low, x_high = min(xs), max(xs)
        if x_high <= x_low:
            x_high = x_low + 1.0
        y_low, y_high = min(ys), max(ys)
        for reference, _ in self.references:
            if math.isfinite(reference):
                y_low = min(y_low, reference)
                y_high = max(y_high, reference)
        if y_high <= y_low:
            pad = 1.0 if y_low == 0 else abs(y_low) * 0.1
            y_low -= pad
            y_high += pad
        else:
            pad = (y_high - y_low) * 0.06
            y_low -= pad
            y_high += pad
        return x_low, x_high, y_low, y_high

    def render(self) -> str:
        x_low, x_high, y_low, y_high = self._data_bounds()
        x_low = self.x_min if self.x_min is not None else x_low
        x_high = self.x_max if self.x_max is not None else x_high
        y_low = self.y_min if self.y_min is not None else y_low
        y_high = self.y_max if self.y_max is not None else y_high
        if x_high <= x_low:
            x_high = x_low + 1.0
        if y_high <= y_low:
            y_high = y_low + 1.0

        plot_left = _LEFT
        plot_right = self.width - _RIGHT
        plot_top = _TOP
        plot_bottom = self.height - _BOTTOM
        plot_width = plot_right - plot_left
        plot_height = plot_bottom - plot_top

        def map_x(value: float) -> float:
            return plot_left + (value - x_low) / (x_high - x_low) * plot_width

        def map_y(value: float) -> float:
            return plot_bottom - (value - y_low) / (y_high - y_low) * plot_height

        parts: list[str] = []
        parts.append(
            f'<?xml version="1.0" encoding="UTF-8"?>'
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{_num(self.width)}" '
            f'height="{_num(self.height)}" viewBox="0 0 {_num(self.width)} {_num(self.height)}">'
        )
        parts.append(f"<title>{_escape(self.title)}</title>")
        if self.note:
            parts.append(f"<desc>{_escape(self.note)}</desc>")
        parts.append(
            f'<rect x="0" y="0" width="{_num(self.width)}" height="{_num(self.height)}" fill="#ffffff"/>'
        )

        # Phase bands, behind everything else.
        for index, band in enumerate(self.bands):
            left = map_x(min(band.start, band.end))
            right = map_x(max(band.start, band.end))
            shade = 0.10 if index % 2 == 0 else 0.05
            parts.append(
                f'<rect x="{_num(left)}" y="{_num(plot_top)}" '
                f'width="{_num(max(right - left, 0.0))}" height="{_num(plot_height)}" '
                f'fill="{_BAND_COLOR}" fill-opacity="{shade:.2f}"/>'
            )

        # Gridlines and their labels.
        y_ticks, y_step = _nice_ticks(y_low, y_high)
        for tick in y_ticks:
            if tick < y_low - y_step * 1e-9 or tick > y_high + y_step * 1e-9:
                continue
            y = map_y(tick)
            parts.append(
                f'<line x1="{_num(plot_left)}" y1="{_num(y)}" x2="{_num(plot_right)}" '
                f'y2="{_num(y)}" stroke="{_GRID_COLOR}" stroke-width="1"/>'
            )
            parts.append(
                f'<text x="{_num(plot_left - 8)}" y="{_num(y + 4)}" text-anchor="end" '
                f'font-family="monospace" font-size="12" fill="{_TEXT_COLOR}">'
                f"{_escape(_tick_label(tick, y_step))}</text>"
            )

        x_ticks, x_step = _nice_ticks(x_low, x_high)
        for tick in x_ticks:
            if tick < x_low - x_step * 1e-9 or tick > x_high + x_step * 1e-9:
                continue
            x = map_x(tick)
            parts.append(
                f'<line x1="{_num(x)}" y1="{_num(plot_top)}" x2="{_num(x)}" '
                f'y2="{_num(plot_bottom)}" stroke="{_GRID_COLOR}" stroke-width="1"/>'
            )
            parts.append(
                f'<text x="{_num(x)}" y="{_num(plot_bottom + 18)}" text-anchor="middle" '
                f'font-family="monospace" font-size="12" fill="{_TEXT_COLOR}">'
                f"{_escape(_tick_label(tick, x_step))}</text>"
            )

        # Plot frame.
        parts.append(
            f'<rect x="{_num(plot_left)}" y="{_num(plot_top)}" width="{_num(plot_width)}" '
            f'height="{_num(plot_height)}" fill="none" stroke="{_AXIS_COLOR}" stroke-width="1"/>'
        )

        # Reference lines.
        for reference, label in self.references:
            if not math.isfinite(reference) or reference < y_low or reference > y_high:
                continue
            y = map_y(reference)
            parts.append(
                f'<line x1="{_num(plot_left)}" y1="{_num(y)}" x2="{_num(plot_right)}" '
                f'y2="{_num(y)}" stroke="{_REFERENCE_COLOR}" stroke-width="1" '
                f'stroke-dasharray="6 4"/>'
            )
            if label:
                parts.append(
                    f'<text x="{_num(plot_left + 4)}" y="{_num(y - 4)}" '
                    f'font-family="monospace" font-size="11" fill="{_REFERENCE_COLOR}">'
                    f"{_escape(label)}</text>"
                )

        # Data.
        drew_something = False
        for series in self.series:
            run: list[tuple[float, float]] = []
            segments: list[list[tuple[float, float]]] = []
            for x, y in zip(series.xs, series.ys):
                if y is None:
                    if len(run) > 1:
                        segments.append(run)
                    run = []
                    continue
                y_value = float(y)
                if not math.isfinite(y_value):
                    if len(run) > 1:
                        segments.append(run)
                    run = []
                    continue
                run.append((float(x), y_value))
            if len(run) > 1:
                segments.append(run)
            dash = ' stroke-dasharray="7 4"' if series.dashed else ""
            for segment in segments:
                points = " ".join(
                    f"{_num(map_x(x))},{_num(map_y(y))}" for x, y in segment
                )
                parts.append(
                    f'<polyline points="{points}" fill="none" stroke="{series.color}" '
                    f'stroke-width="1.8"{dash}/>'
                )
                drew_something = True

        # Event markers.
        for index, marker in enumerate(self.markers):
            x = map_x(marker.x)
            parts.append(
                f'<line x1="{_num(x)}" y1="{_num(plot_top)}" x2="{_num(x)}" '
                f'y2="{_num(plot_bottom)}" stroke="{_MARKER_COLOR}" stroke-width="1" '
                f'stroke-dasharray="2 3"/>'
            )
            if marker.label:
                y_text = plot_top + 12 + (index % 3) * 12
                parts.append(
                    f'<text x="{_num(x + 3)}" y="{_num(y_text)}" '
                    f'font-family="monospace" font-size="10" fill="{_MARKER_COLOR}">'
                    f"{_escape(marker.label)}</text>"
                )

        if not drew_something:
            parts.append(
                f'<text x="{_num(plot_left + plot_width / 2)}" '
                f'y="{_num(plot_top + plot_height / 2)}" text-anchor="middle" '
                f'font-family="monospace" font-size="14" fill="{_NOTE_COLOR}">no data</text>'
            )

        # Legend.
        legend_x = plot_right + 16
        legend_y = plot_top + 4
        parts.append(
            f'<text x="{_num(legend_x)}" y="{_num(legend_y)}" font-family="monospace" '
            f'font-size="11" fill="{_TEXT_COLOR}">legend</text>'
        )
        for index, series in enumerate(self.series):
            y = legend_y + 20 + index * 18
            dash = ' stroke-dasharray="7 4"' if series.dashed else ""
            parts.append(
                f'<line x1="{_num(legend_x)}" y1="{_num(y - 4)}" '
                f'x2="{_num(legend_x + 22)}" y2="{_num(y - 4)}" stroke="{series.color}" '
                f'stroke-width="2.2"{dash}/>'
            )
            label = series.label if len(series.label) <= 30 else series.label[:29] + "\u2026"
            parts.append(
                f'<text x="{_num(legend_x + 28)}" y="{_num(y)}" font-family="monospace" '
                f'font-size="11" fill="{_TEXT_COLOR}">{_escape(label)}</text>'
            )

        # Title and axis labels.
        parts.append(
            f'<text x="{_num(plot_left)}" y="26" font-family="monospace" font-size="15" '
            f'fill="{_TEXT_COLOR}">{_escape(self.title)}</text>'
        )
        parts.append(
            f'<text x="{_num(plot_left + plot_width / 2)}" y="{_num(self.height - 30)}" '
            f'text-anchor="middle" font-family="monospace" font-size="12" '
            f'fill="{_TEXT_COLOR}">{_escape(self.x_label)}</text>'
        )
        parts.append(
            f'<text x="16" y="{_num(plot_top + plot_height / 2)}" text-anchor="middle" '
            f'transform="rotate(-90 16 {_num(plot_top + plot_height / 2)})" '
            f'font-family="monospace" font-size="12" fill="{_TEXT_COLOR}">'
            f"{_escape(self.y_label)}</text>"
        )
        if self.note:
            parts.append(
                f'<text x="{_num(plot_left)}" y="{_num(self.height - 12)}" '
                f'font-family="monospace" font-size="11" fill="{_NOTE_COLOR}">'
                f"{_escape(self.note)}</text>"
            )

        parts.append("</svg>")
        return "\n".join(parts) + "\n"
