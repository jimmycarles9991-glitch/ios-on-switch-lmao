"""Least squares with its residuals, and the robust centre and spread of repeated timings."""

import math
import statistics
from dataclasses import dataclass


@dataclass(frozen=True)
class Line:
    slope: float
    intercept: float
    rms: float  # residual root mean square, in y's units
    stderr: float  # the slope's standard error (0 with two points)
    n: int


def line(xs: list[float], ys: list[float]) -> Line:
    if len(xs) != len(ys) or len(set(xs)) < 2:
        raise ValueError('a line needs two distinct x values')
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys, strict=True)) / sxx
    intercept = my - slope * mx
    sq = sum((y - intercept - slope * x) ** 2 for x, y in zip(xs, ys, strict=True))
    stderr = math.sqrt(sq / (n - 2) / sxx) if n > 2 else 0.0
    return Line(slope, intercept, math.sqrt(sq / n), stderr, n)


def median(xs: list[float]) -> float:
    return statistics.median(xs)


def relative_spread(xs: list[float]) -> float:
    """Give the median absolute deviation over the median: the repeats' noise as a fraction."""
    m = median(xs)
    return median([abs(x - m) for x in xs]) / m if m else 0.0
