#include "../src/spatialdata_wkb.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>

using namespace anndatapp_spatial_wkb;

int main() {
    Geometry points;
    points.xy = {1.5, -2, 4, 5};
    points.types = {1, 1};
    Geometry decoded;
    for (std::size_t i = 0; i < points.types.size(); ++i) {
        auto bytes = encode(points, i);
        append(decoded, bytes.data(), bytes.size());
    }
    assert(decoded.xy == points.xy && decoded.types == points.types);

    Geometry shapes;
    shapes.xy = {0,0, 2,0, 2,2, 0,0, .5,.5, .7,.5, .5,.5, 3,3, 4,3, 3,3};
    shapes.rings = {0,4,7,10};
    shapes.polygons = {0,2,3};
    shapes.features = {0,1,2};
    shapes.types = {3,6};
    Geometry read;
    for (std::size_t i = 0; i < shapes.types.size(); ++i) {
        auto bytes = encode(shapes, i);
        append(read, bytes.data(), bytes.size());
    }
    assert(read.xy == shapes.xy && read.rings == shapes.rings);
    assert(read.polygons == shapes.polygons && read.features == shapes.features);
    assert(read.types == shapes.types); // preserves single-part MultiPolygon

    const std::uint8_t big_endian[] = {
        0, 0,0,0,1, 0x3f,0xf0,0,0,0,0,0,0, 0xc0,0,0,0,0,0,0,0
    };
    Geometry big;
    append(big, big_endian, sizeof(big_endian));
    assert(big.xy[0] == 1 && big.xy[1] == -2);
    bool rejected = false;
    try { Geometry bad; append(bad, big_endian, sizeof(big_endian)-1); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    std::cout << "SpatialData WKB geometry tests passed\n";
}
