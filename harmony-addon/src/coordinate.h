#pragma once
#include <cmath>
inline int pixelCoordinate(double ratio, int size)
{
    if (!std::isfinite(ratio) || size <= 0 || size > 4096) return -1;
    if (ratio <= 0.0) return 0;
    if (ratio >= 1.0) return size - 1;
    return static_cast<int>(ratio * (size - 1) + 0.5);
}
