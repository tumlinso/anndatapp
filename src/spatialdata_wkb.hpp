#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// Operation-local decoded GeoParquet geometry. Offsets are in vertices,
// rings, and polygons respectively. Point batches use xy directly.
namespace anndatapp_spatial_wkb {

struct Geometry {
    std::vector<double> xy;
    std::vector<std::uint64_t> rings{0}, polygons{0}, features{0};
    std::vector<std::uint8_t> types;
};

class Reader {
public:
    Reader(const void* p, std::size_t n) : p_(static_cast<const std::uint8_t*>(p)), n_(n) {}

    std::uint8_t byte() {
        if (!n_) throw std::runtime_error("truncated WKB");
        --n_;
        return *p_++;
    }
    std::uint32_t u32(bool little) {
        if (n_ < 4) throw std::runtime_error("truncated WKB integer");
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= std::uint32_t(byte()) << (8 * (little ? i : 3-i));
        return v;
    }
    double f64(bool little) {
        if (n_ < 8) throw std::runtime_error("truncated WKB coordinate");
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= std::uint64_t(byte()) << (8 * (little ? i : 7-i));
        double x;
        std::memcpy(&x, &v, sizeof(x));
        return x;
    }
    bool order() {
        auto v = byte();
        if (v > 1) throw std::runtime_error("invalid WKB byte order");
        return v == 1;
    }
    std::size_t remaining() const { return n_; }
private:
    const std::uint8_t* p_;
    std::size_t n_;
};

inline void polygon(Reader& r, bool le, Geometry& out) {
    auto nr = r.u32(le);
    if (nr > r.remaining() / 4) throw std::runtime_error("invalid WKB ring count");
    for (std::uint32_t j = 0; j < nr; ++j) {
        auto np = r.u32(le);
        if (np > r.remaining() / 16) throw std::runtime_error("invalid WKB vertex count");
        if (np > (std::numeric_limits<std::size_t>::max() - out.xy.size()) / 2)
            throw std::runtime_error("WKB geometry is too large");
        for (std::uint32_t k = 0; k < np; ++k) {
            out.xy.push_back(r.f64(le));
            out.xy.push_back(r.f64(le));
        }
        out.rings.push_back(out.xy.size() / 2);
    }
    out.polygons.push_back(out.rings.size() - 1);
}

// Parse one 2D Point, Polygon or MultiPolygon. Append only to a homogeneous
// Point batch or a homogeneous Polygon/MultiPolygon batch.
inline void append(Geometry& out, const void* data, std::size_t size) {
    if (!data && size) throw std::runtime_error("null WKB data");
    Reader r(data, size);
    bool le = r.order();
    auto type = r.u32(le);
    if (type != 1 && type != 3 && type != 6) throw std::runtime_error("unsupported WKB geometry type");
    if (!out.types.empty() && ((out.types.front() == 1) != (type == 1)))
        throw std::runtime_error("mixed WKB point and polygon types");
    if (type == 1) {
        out.xy.push_back(r.f64(le));
        out.xy.push_back(r.f64(le));
    } else if (type == 3) {
        polygon(r, le, out);
        out.features.push_back(out.polygons.size() - 1);
    } else {
        auto np = r.u32(le);
        if (np > r.remaining() / 9) throw std::runtime_error("invalid WKB polygon count");
        for (std::uint32_t i = 0; i < np; ++i) {
            bool child_le = r.order();
            if (r.u32(child_le) != 3) throw std::runtime_error("WKB multipolygon member is not a polygon");
            polygon(r, child_le, out);
        }
        out.features.push_back(out.polygons.size() - 1);
    }
    if (r.remaining()) throw std::runtime_error("unexpected trailing WKB bytes");
    out.types.push_back(static_cast<std::uint8_t>(type));
}

inline void u32(std::string& out, std::size_t v) {
    if (v > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("WKB count exceeds uint32");
    for (int i = 0; i < 4; ++i) out.push_back(char((v >> (8 * i)) & 0xff));
}
inline void f64(std::string& out, double x) {
    std::uint64_t v;
    std::memcpy(&v, &x, sizeof(v));
    for (int i = 0; i < 8; ++i) out.push_back(char((v >> (8 * i)) & 0xff));
}
inline void header(std::string& out, std::size_t type) {
    out.push_back(1);
    u32(out, type);
}
inline void polygon(std::string& out, const Geometry& g, std::size_t p) {
    if (p + 1 >= g.polygons.size() || g.polygons[p] > g.polygons[p+1] || g.polygons[p+1] >= g.rings.size())
        throw std::runtime_error("invalid polygon offsets");
    u32(out, g.polygons[p+1] - g.polygons[p]);
    for (auto r = g.polygons[p]; r < g.polygons[p+1]; ++r) {
        auto a = g.rings[r], b = g.rings[r+1];
        if (a > b || b > g.xy.size() / 2) throw std::runtime_error("invalid ring offsets");
        u32(out, b - a);
        for (auto k = a; k < b; ++k) { f64(out, g.xy[2*k]); f64(out, g.xy[2*k+1]); }
    }
}
inline std::string encode(const Geometry& g, std::size_t i) {
    if (i >= g.types.size() || g.xy.size() % 2) throw std::runtime_error("invalid WKB geometry index");
    auto type = g.types[i];
    std::string out;
    header(out, type);
    if (type == 1) {
        if (i >= g.xy.size()/2) throw std::runtime_error("invalid WKB point index");
        f64(out, g.xy[2*i]); f64(out, g.xy[2*i+1]);
    } else {
        if ((type != 3 && type != 6) || i+1 >= g.features.size() || g.features[i] > g.features[i+1])
            throw std::runtime_error("invalid feature offsets");
        auto first = g.features[i], last = g.features[i+1];
        if (last >= g.polygons.size()) throw std::runtime_error("invalid feature offsets");
        if (type == 3) {
            if (last - first != 1) throw std::runtime_error("polygon must contain one polygon");
            polygon(out, g, first);
        } else {
            u32(out, last-first);
            for (auto p = first; p < last; ++p) { header(out, 3); polygon(out, g, p); }
        }
    }
    return out;
}

} // namespace anndatapp_spatial_wkb
