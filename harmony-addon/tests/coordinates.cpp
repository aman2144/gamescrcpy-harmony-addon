#include "../src/coordinate.h"
#include <cassert>
#include <limits>
int main()
{
    assert(pixelCoordinate(0, 1080) == 0);
    assert(pixelCoordinate(1, 1080) == 1079);
    assert(pixelCoordinate(-0.1, 1080) == 0);
    assert(pixelCoordinate(1.1, 1080) == 1079);
    assert(pixelCoordinate(0.5, 1920) == 960);
    assert(pixelCoordinate(std::numeric_limits<double>::quiet_NaN(), 1080) == -1);
    assert(pixelCoordinate(std::numeric_limits<double>::infinity(), 1080) == -1);
    assert(pixelCoordinate(0.5, 0) == -1);
    assert(pixelCoordinate(0.5, 4097) == -1);
}
