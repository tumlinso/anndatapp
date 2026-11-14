module;
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>

module anndatapp.spatialdata;

namespace anndatapp {
namespace {
[[noreturn]] void transform_error(const char* message) {
    throw Error(ErrorCode::invalid_argument, "spatial/transform", message);
}
std::size_t axis_index(const SpatialAxis* axes, std::size_t n, SpatialAxis axis) {
    for (std::size_t i = 0; i < n; ++i) if (axes[i] == axis) return i;
    transform_error("intermediate coordinate axes do not match");
}
void validate_axes(const SpatialAxis* axes, std::size_t n) {
    if (!n || n > 4) transform_error("coordinate rank must be between one and four");
    for (std::size_t i = 0; i < n; ++i) {
        if (unsigned(axes[i]) > unsigned(SpatialAxis::c)) transform_error("unknown coordinate axis");
        for (std::size_t j = 0; j < i; ++j)
            if (axes[i] == axes[j]) transform_error("coordinate axes must be unique");
    }
}
}

SpatialTransform SpatialTransform::identity(std::initializer_list<SpatialAxis> axes) {
    SpatialTransform t;
    t.input_rank = t.output_rank = axes.size();
    validate_axes(axes.begin(), axes.size());
    std::size_t i = 0;
    for (auto a : axes) { t.input_axes[i] = t.output_axes[i] = a; t.m[5*i+i] = 1; ++i; }
    return t;
}
SpatialTransform SpatialTransform::affine(const SpatialAxis* input, std::size_t ni,
    const SpatialAxis* output, std::size_t no, const double* a, std::ptrdiff_t row_stride) {
    if (!input || !output || !a) transform_error("null transform metadata or matrix");
    validate_axes(input, ni); validate_axes(output, no);
    SpatialTransform t; t.input_rank = ni; t.output_rank = no;
    for (std::size_t j = 0; j < ni; ++j) t.input_axes[j] = input[j];
    for (std::size_t i = 0; i < no; ++i) {
        t.output_axes[i] = output[i];
        for (std::size_t j = 0; j <= ni; ++j) t.m[5*i+j] = a[std::ptrdiff_t(i)*row_stride + std::ptrdiff_t(j)];
    }
    t.validate(); return t;
}
SpatialTransform SpatialTransform::translation(std::initializer_list<SpatialAxis> axes,
    std::initializer_list<double> offsets) {
    if (axes.size() != offsets.size()) transform_error("translation rank differs from axes");
    auto t = identity(axes); std::size_t i = 0;
    for (auto x : offsets) t.m[5*i++ + t.input_rank] = x;
    t.validate(); return t;
}
SpatialTransform SpatialTransform::scale(std::initializer_list<SpatialAxis> axes,
    std::initializer_list<double> factors) {
    if (axes.size() != factors.size()) transform_error("scale rank differs from axes");
    auto t = identity(axes); std::size_t i = 0;
    for (auto x : factors) { t.m[5*i+i] = x; ++i; }
    t.validate(); return t;
}
void SpatialTransform::validate() const {
    validate_axes(input_axes, input_rank); validate_axes(output_axes, output_rank);
    for (std::size_t i = 0; i < output_rank; ++i)
        for (std::size_t j = 0; j <= input_rank; ++j)
            if (!__builtin_isfinite(m[5*i+j])) transform_error("transform entries must be finite");
}
SpatialTransform compose(const SpatialTransform& after, const SpatialTransform& before) {
    after.validate(); before.validate();
    if (after.input_rank != before.output_rank) transform_error("intermediate coordinate ranks differ");
    std::size_t reorder[4]{};
    for (std::size_t k = 0; k < after.input_rank; ++k)
        reorder[k] = axis_index(before.output_axes, before.output_rank, after.input_axes[k]);
    SpatialTransform t; t.input_rank = before.input_rank; t.output_rank = after.output_rank;
    for (std::size_t j = 0; j < t.input_rank; ++j) t.input_axes[j] = before.input_axes[j];
    for (std::size_t i = 0; i < t.output_rank; ++i) {
        t.output_axes[i] = after.output_axes[i];
        for (std::size_t j = 0; j < t.input_rank; ++j)
            for (std::size_t k = 0; k < after.input_rank; ++k)
                t.m[5*i+j] += after.m[5*i+k] * before.m[5*reorder[k]+j];
        double b = after.m[5*i+after.input_rank];
        for (std::size_t k = 0; k < after.input_rank; ++k)
            b += after.m[5*i+k] * before.m[5*reorder[k]+before.input_rank];
        t.m[5*i+t.input_rank] = b;
    }
    t.validate(); return t;
}
SpatialTransform SpatialTransform::inverse() const {
    validate();
    if (input_rank != output_rank) transform_error("a nonsquare coordinate map has no inverse");
    const auto n = input_rank;
    double a[4][8]{};
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) a[i][j] = m[5*i+j];
        a[i][n+i] = 1;
    }
    for (std::size_t j = 0; j < n; ++j) {
        std::size_t p = j;
        for (std::size_t i = j+1; i < n; ++i)
            if (__builtin_fabs(a[i][j]) > __builtin_fabs(a[p][j])) p = i;
        if (a[p][j] == 0) transform_error("singular coordinate map has no inverse");
        if (p != j) for (std::size_t k = 0; k < 2*n; ++k) { const double v = a[p][k]; a[p][k] = a[j][k]; a[j][k] = v; }
        const double d = a[j][j];
        for (std::size_t k = 0; k < 2*n; ++k) a[j][k] /= d;
        for (std::size_t i = 0; i < n; ++i) if (i != j) {
            const double x = a[i][j];
            for (std::size_t k = 0; k < 2*n; ++k) a[i][k] -= x*a[j][k];
        }
    }
    SpatialTransform t; t.input_rank = t.output_rank = n;
    for (std::size_t i = 0; i < n; ++i) {
        t.input_axes[i] = output_axes[i]; t.output_axes[i] = input_axes[i];
        for (std::size_t j = 0; j < n; ++j) {
            t.m[5*i+j] = a[i][n+j];
            t.m[5*i+n] -= a[i][n+j]*m[5*j+n];
        }
    }
    t.validate(); return t;
}
void transform_coordinates(const SpatialTransform& t, const double* x,
    std::ptrdiff_t sx, double* y, std::ptrdiff_t sy, std::size_t n) {
    t.validate();
    if (n && (!x || !y)) transform_error("null coordinate buffer");
    if (n > 1 && (sx == 0 || sy == 0)) transform_error("coordinate rows must have nonzero strides");
    if (n && x == y) transform_error("input and output coordinate buffers must not overlap");
    for (std::size_t k = 0; k < n; ++k) {
        const double* p = x + std::ptrdiff_t(k)*sx;
        double* q = y + std::ptrdiff_t(k)*sy;
        for (std::size_t i = 0; i < t.output_rank; ++i) {
            double v = t.m[5*i+t.input_rank];
            for (std::size_t j = 0; j < t.input_rank; ++j) v += t.m[5*i+j]*p[j];
            q[i] = v;
        }
    }
}
} // namespace anndatapp
