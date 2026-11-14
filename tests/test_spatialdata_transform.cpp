#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>

import anndatapp;

namespace {
bool track_allocations = false;
std::size_t allocation_count = 0;

void* allocate_aligned(std::size_t size, std::size_t alignment) {
    if (track_allocations) ++allocation_count;
    size = size ? size : 1;
    const auto remainder = size % alignment;
    if (remainder) {
        const auto extra = alignment - remainder;
        if (size > std::numeric_limits<std::size_t>::max() - extra)
            throw std::bad_alloc();
        size += extra;
    }
    if (auto* p = std::aligned_alloc(alignment, size)) return p;
    throw std::bad_alloc();
}

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template<class F>
std::size_t allocations_during(F&& operation) {
    allocation_count = 0;
    track_allocations = true;
    try {
        operation();
    } catch (...) {
        track_allocations = false;
        throw;
    }
    track_allocations = false;
    return allocation_count;
}

void expect_transform_error(auto&& operation, const char* message) {
    try {
        operation();
        expect(false, message);
    } catch (const anndatapp::Error& e) {
        expect(e.code() == anndatapp::ErrorCode::invalid_argument, message);
        expect(e.path() == "spatial/transform", "transform errors report their path");
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << message << " (different exception: "
                  << e.what() << ")\n";
        ++failures;
    } catch (...) {
        expect(false, message);
    }
}

bool close(double actual, double expected) {
    return std::fabs(actual - expected) <= 1e-12;
}

void expect_near(double actual, double expected, const char* message) {
    expect(close(actual, expected), message);
}

void factory_constructors() {
    using anndatapp::SpatialAxis;
    using anndatapp::SpatialTransform;

    const auto identity = SpatialTransform::identity({SpatialAxis::y, SpatialAxis::x});
    expect(identity.input_rank == 2 && identity.output_rank == 2 &&
           identity.input_axes[0] == SpatialAxis::y &&
           identity.input_axes[1] == SpatialAxis::x &&
           identity.output_axes[0] == SpatialAxis::y &&
           identity.output_axes[1] == SpatialAxis::x,
           "identity retains the declared named-axis order");
    expect(identity.m[0] == 1 && identity.m[1] == 0 && identity.m[2] == 0 &&
           identity.m[5] == 0 && identity.m[6] == 1 && identity.m[7] == 0,
           "identity stores its linear map and zero translation");

    const auto translated = SpatialTransform::translation(
        {SpatialAxis::y, SpatialAxis::x}, {4.0, -2.0});
    const auto scaled = SpatialTransform::scale(
        {SpatialAxis::x, SpatialAxis::y}, {2.0, 3.0});
    const double input[]{2.0, 5.0};
    double translated_output[2]{}, scaled_output[2]{};
    anndatapp::transform_coordinates(translated, input, 2, translated_output, 2, 1);
    anndatapp::transform_coordinates(scaled, input, 2, scaled_output, 2, 1);
    expect(translated_output[0] == 6.0 && translated_output[1] == 3.0,
           "translation applies offsets in the declared axis order");
    expect(scaled_output[0] == 4.0 && scaled_output[1] == 15.0,
           "scale applies one factor to each declared axis");
}

void affine_2d_signed_strides_and_roundtrip() {
    using anndatapp::SpatialAxis;
    using anndatapp::SpatialTransform;

    const SpatialAxis input_axes[]{SpatialAxis::y, SpatialAxis::x};
    const SpatialAxis output_axes[]{SpatialAxis::x, SpatialAxis::y};
    const double matrix[]{2.0, -3.0, 5.0,
                          -4.0, 0.5, -2.0};
    const auto transform = SpatialTransform::affine(
        input_axes, 2, output_axes, 2, matrix, 3);
    expect(transform.input_axes[0] == SpatialAxis::y &&
           transform.input_axes[1] == SpatialAxis::x &&
           transform.output_axes[0] == SpatialAxis::x &&
           transform.output_axes[1] == SpatialAxis::y,
           "affine preserves different input and output axis orders");

    // Logical rows are stored in reverse order, with a gap between each row.
    double input_storage[]{0.5, -2.0, 88.0, -1.0, 4.0, 77.0, 3.0, 2.0, 66.0};
    std::array<double, 12> output_storage;
    output_storage.fill(-77.0);
    anndatapp::transform_coordinates(transform, input_storage + 6, -3,
                                     output_storage.data() + 8, -4, 3);
    expect(output_storage[8] == 5.0 && output_storage[9] == -13.0 &&
           output_storage[4] == -9.0 && output_storage[5] == 4.0 &&
           output_storage[0] == 12.0 && output_storage[1] == -5.0,
           "2D affine transforms independent rows with signed strides");
    expect(output_storage[2] == -77.0 && output_storage[3] == -77.0 &&
           output_storage[6] == -77.0 && output_storage[7] == -77.0 &&
           output_storage[10] == -77.0 && output_storage[11] == -77.0,
           "strided transform writes only the caller-provided coordinate rows");

    const auto inverse = transform.inverse();
    expect(inverse.input_axes[0] == SpatialAxis::x &&
           inverse.input_axes[1] == SpatialAxis::y &&
           inverse.output_axes[0] == SpatialAxis::y &&
           inverse.output_axes[1] == SpatialAxis::x,
           "inverse swaps the named input and output axes");
    std::array<double, 12> roundtrip_storage;
    roundtrip_storage.fill(-91.0);
    anndatapp::transform_coordinates(inverse, output_storage.data() + 8, -4,
                                     roundtrip_storage.data() + 8, -4, 3);
    expect_near(roundtrip_storage[8], 3.0, "inverse recovers first y coordinate");
    expect_near(roundtrip_storage[9], 2.0, "inverse recovers first x coordinate");
    expect_near(roundtrip_storage[4], -1.0, "inverse recovers second y coordinate");
    expect_near(roundtrip_storage[5], 4.0, "inverse recovers second x coordinate");
    expect_near(roundtrip_storage[0], 0.5, "inverse recovers third y coordinate");
    expect_near(roundtrip_storage[1], -2.0, "inverse recovers third x coordinate");
}

void named_axis_composition() {
    using anndatapp::SpatialAxis;
    using anndatapp::SpatialTransform;

    const SpatialAxis xy[]{SpatialAxis::x, SpatialAxis::y};
    const double before_matrix[]{2.0, 1.0, 3.0,
                                 -1.0, 1.0, 4.0};
    const auto before = SpatialTransform::affine(
        xy, 2, xy, 2, before_matrix, 3);
    const SpatialAxis yx[]{SpatialAxis::y, SpatialAxis::x};
    const double after_matrix[]{3.0, 2.0, 5.0,
                                -2.0, 4.0, 6.0};
    const auto after = SpatialTransform::affine(
        yx, 2, xy, 2, after_matrix, 3);
    const auto composed = anndatapp::compose(after, before);
    expect(composed.input_axes[0] == SpatialAxis::x &&
           composed.input_axes[1] == SpatialAxis::y &&
           composed.output_axes[0] == SpatialAxis::x &&
           composed.output_axes[1] == SpatialAxis::y,
           "composition retains the external named-axis order");
    expect(composed.m[0] == 1.0 && composed.m[1] == 5.0 &&
           composed.m[2] == 23.0 && composed.m[5] == 10.0 &&
           composed.m[6] == 2.0 && composed.m[7] == 10.0,
           "composition reorders intermediate coordinates by axis name");

    const double input[]{1.0, 2.0};
    double output[2]{};
    anndatapp::transform_coordinates(composed, input, 2, output, 2, 1);
    expect(output[0] == 34.0 && output[1] == 24.0,
           "composed 2D affine map matches the manually evaluated coordinates");
}

void three_dimensional_and_rectangular_maps() {
    using anndatapp::SpatialAxis;
    using anndatapp::SpatialTransform;

    const SpatialAxis input_axes[]{SpatialAxis::z, SpatialAxis::x, SpatialAxis::y};
    const SpatialAxis output_axes[]{SpatialAxis::y, SpatialAxis::z, SpatialAxis::x};
    // Matrix rows are laid out in reverse order to exercise affine's signed row stride.
    const double matrix_storage[]{1.0, -2.0, 4.0, 1.0,
                                  -1.0, 3.0, 2.0, -2.0,
                                  2.0, 1.0, -1.0, 4.0};
    const auto transform = SpatialTransform::affine(
        input_axes, 3, output_axes, 3, matrix_storage + 8, -4);
    const double input[]{2.0, 3.0, -1.0};
    double output[3]{};
    anndatapp::transform_coordinates(transform, input, 3, output, 3, 1);
    expect(output[0] == 12.0 && output[1] == 3.0 && output[2] == -7.0,
           "3D affine handles a nontrivial named-axis permutation");

    const auto inverse = transform.inverse();
    double roundtrip[3]{};
    anndatapp::transform_coordinates(inverse, output, 3, roundtrip, 3, 1);
    expect_near(roundtrip[0], 2.0, "3D inverse recovers z");
    expect_near(roundtrip[1], 3.0, "3D inverse recovers x");
    expect_near(roundtrip[2], -1.0, "3D inverse recovers y");

    const SpatialAxis input3[]{SpatialAxis::z, SpatialAxis::y, SpatialAxis::x};
    const SpatialAxis output2[]{SpatialAxis::x, SpatialAxis::z};
    const double reduce_matrix[]{2.0, -1.0, 3.0, 1.0,
                                 -1.0, 4.0, 2.0, -3.0};
    const auto reduce = SpatialTransform::affine(
        input3, 3, output2, 2, reduce_matrix, 4);
    const double source3[]{2.0, 5.0, -1.0};
    double reduced[2]{};
    anndatapp::transform_coordinates(reduce, source3, 3, reduced, 2, 1);
    expect(reduce.input_rank == 3 && reduce.output_rank == 2 &&
           reduced[0] == -3.0 && reduced[1] == 13.0,
           "rectangular 3D-to-2D affine maps use each declared matrix dimension");

    const SpatialAxis input2[]{SpatialAxis::x, SpatialAxis::y};
    const SpatialAxis output3[]{SpatialAxis::c, SpatialAxis::z, SpatialAxis::x};
    const double expand_matrix[]{1.0, 1.0, 2.0,
                                 2.0, -1.0, 0.0,
                                 -1.0, 3.0, 5.0};
    const auto expand = SpatialTransform::affine(
        input2, 2, output3, 3, expand_matrix, 3);
    const double source2[]{4.0, -2.0};
    double expanded[3]{};
    anndatapp::transform_coordinates(expand, source2, 2, expanded, 3, 1);
    expect(expand.input_rank == 2 && expand.output_rank == 3 &&
           expanded[0] == 4.0 && expanded[1] == 10.0 && expanded[2] == -5.0,
           "rectangular 2D-to-3D affine maps support rank expansion");
}

void validation_and_errors() {
    using anndatapp::SpatialAxis;
    using anndatapp::SpatialTransform;

    expect_transform_error([&] {
        (void)SpatialTransform::identity({SpatialAxis::x, SpatialAxis::x});
    }, "duplicate identity axes are rejected");
    expect_transform_error([&] {
        (void)SpatialTransform::identity(
            {SpatialAxis::x, SpatialAxis::y, SpatialAxis::z,
             SpatialAxis::c, SpatialAxis::x});
    }, "coordinate ranks above four are rejected");
    expect_transform_error([&] {
        (void)SpatialTransform::identity({static_cast<SpatialAxis>(255)});
    }, "unknown spatial axes are rejected");
    expect_transform_error([&] {
        (void)SpatialTransform::translation({SpatialAxis::x, SpatialAxis::y}, {1.0});
    }, "translation offset rank must match its axes");
    expect_transform_error([&] {
        (void)SpatialTransform::scale(
            {SpatialAxis::x}, {std::numeric_limits<double>::quiet_NaN()});
    }, "non-finite scale factors are rejected");
    expect_transform_error([&] {
        (void)SpatialTransform::translation(
            {SpatialAxis::x}, {std::numeric_limits<double>::infinity()});
    }, "non-finite translations are rejected");

    const SpatialAxis xy[]{SpatialAxis::x, SpatialAxis::y};
    const double nonfinite_matrix[]{1.0, 0.0, 0.0,
                                    0.0, std::numeric_limits<double>::quiet_NaN(), 0.0};
    expect_transform_error([&] {
        (void)SpatialTransform::affine(xy, 2, xy, 2, nonfinite_matrix, 3);
    }, "non-finite affine matrix entries are rejected");

    auto corrupted = SpatialTransform::identity({SpatialAxis::x, SpatialAxis::y});
    corrupted.m[1] = std::numeric_limits<double>::infinity();
    expect_transform_error([&] { corrupted.validate(); },
                           "validate rejects non-finite entries in an existing transform");

    const SpatialAxis z[]{SpatialAxis::z};
    const double one[]{1.0, 0.0};
    const auto before = SpatialTransform::affine(z, 1, z, 1, one, 2);
    const SpatialAxis x[]{SpatialAxis::x};
    const auto after = SpatialTransform::affine(x, 1, x, 1, one, 2);
    expect_transform_error([&] { (void)anndatapp::compose(after, before); },
                           "composition rejects unmatched intermediate axis names");

    const auto singular = SpatialTransform::scale(
        {SpatialAxis::x, SpatialAxis::y}, {2.0, 0.0});
    expect_transform_error([&] { (void)singular.inverse(); },
                           "singular square transforms have no inverse");
    const SpatialAxis input3[]{SpatialAxis::x, SpatialAxis::y, SpatialAxis::z};
    const SpatialAxis output2[]{SpatialAxis::x, SpatialAxis::y};
    const double rectangular_matrix[]{1.0, 0.0, 0.0, 0.0,
                                      0.0, 1.0, 0.0, 0.0};
    const auto rectangular = SpatialTransform::affine(
        input3, 3, output2, 2, rectangular_matrix, 4);
    expect_transform_error([&] { (void)rectangular.inverse(); },
                           "rectangular transforms have no inverse");

    const auto valid = SpatialTransform::identity({SpatialAxis::x, SpatialAxis::y});
    double input[]{1.0, 2.0, 3.0, 4.0};
    double output[4]{};
    expect_transform_error([&] {
        anndatapp::transform_coordinates(valid, nullptr, 2, output, 2, 1);
    }, "nonempty transforms reject null coordinate buffers");
    expect_transform_error([&] {
        anndatapp::transform_coordinates(valid, input, 0, output, 2, 2);
    }, "multi-row transforms reject zero input row stride");
    expect_transform_error([&] {
        anndatapp::transform_coordinates(valid, input, 2, output, 0, 2);
    }, "multi-row transforms reject zero output row stride");
    expect_transform_error([&] {
        anndatapp::transform_coordinates(valid, input, 2, input, 2, 1);
    }, "transforms reject overlapping input and output buffers");
}

void numerical_operations_do_not_allocate() {
    using anndatapp::SpatialAxis;
    using anndatapp::SpatialTransform;

    SpatialTransform fresh_identity;
    auto allocations = allocations_during([&] {
        fresh_identity = SpatialTransform::identity({SpatialAxis::x, SpatialAxis::y});
    });
    expect(allocations == 0, "fresh identity construction performs no C++ heap allocation");
    SpatialTransform repeated_identity;
    allocations = allocations_during([&] {
        repeated_identity = SpatialTransform::identity({SpatialAxis::x, SpatialAxis::y});
    });
    expect(allocations == 0, "repeated identity construction performs no C++ heap allocation");

    const SpatialAxis xy[]{SpatialAxis::x, SpatialAxis::y};
    const SpatialAxis yx[]{SpatialAxis::y, SpatialAxis::x};
    const double before_matrix[]{2.0, 1.0, 3.0, -1.0, 1.0, 4.0};
    const double after_matrix[]{3.0, 2.0, 5.0, -2.0, 4.0, 6.0};
    const auto before = SpatialTransform::affine(xy, 2, xy, 2, before_matrix, 3);
    const auto after = SpatialTransform::affine(yx, 2, xy, 2, after_matrix, 3);
    SpatialTransform fresh_composition;
    allocations = allocations_during([&] {
        fresh_composition = anndatapp::compose(after, before);
    });
    expect(allocations == 0, "fresh valid composition performs no C++ heap allocation");
    SpatialTransform repeated_composition;
    allocations = allocations_during([&] {
        repeated_composition = anndatapp::compose(after, before);
    });
    expect(allocations == 0, "repeated valid composition performs no C++ heap allocation");

    const auto invertible = SpatialTransform::affine(
        xy, 2, xy, 2, before_matrix, 3);
    SpatialTransform fresh_inverse;
    allocations = allocations_during([&] { fresh_inverse = invertible.inverse(); });
    expect(allocations == 0, "fresh valid inverse performs no C++ heap allocation");
    SpatialTransform repeated_inverse;
    allocations = allocations_during([&] { repeated_inverse = invertible.inverse(); });
    expect(allocations == 0, "repeated valid inverse performs no C++ heap allocation");

    const auto transform = SpatialTransform::translation(
        {SpatialAxis::y, SpatialAxis::x}, {2.0, -3.0});
    const double input[]{1.0, 4.0, -2.0, 5.0};
    double output[4]{};
    allocations = allocations_during([&] {
        anndatapp::transform_coordinates(transform, input, 2, output, 2, 2);
    });
    expect(allocations == 0, "fresh valid coordinate transform performs no C++ heap allocation");
    allocations = allocations_during([&] {
        anndatapp::transform_coordinates(transform, input, 2, output, 2, 2);
    });
    expect(allocations == 0, "repeated valid coordinate transform performs no C++ heap allocation");
    expect(output[0] == 3.0 && output[1] == 1.0 &&
           output[2] == 0.0 && output[3] == 2.0,
           "allocation-free transform writes directly to caller-owned output");
}

} // namespace

void* operator new(std::size_t size) {
    if (track_allocations) ++allocation_count;
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

int main() {
    try {
        factory_constructors();
        affine_2d_signed_strides_and_roundtrip();
        named_axis_composition();
        three_dimensional_and_rectangular_maps();
        validation_and_errors();
        numerical_operations_do_not_allocate();
    } catch (const std::exception& e) {
        track_allocations = false;
        std::cerr << "UNEXPECTED: " << e.what() << '\n';
        return 2;
    }
    if (failures) return 1;
    std::cout << "SpatialTransform tests passed\n";
    return 0;
}
