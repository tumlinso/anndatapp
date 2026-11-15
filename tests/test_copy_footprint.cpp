#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

import anndatapp.core;

namespace {
using namespace anndatapp;

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

template<class F> void expect_error(ErrorCode code, F&& f) {
    try {
        f();
    } catch (const Error& error) {
        check(error.code() == code, "copy footprint returned the wrong error code");
        return;
    }
    throw std::runtime_error("copy footprint did not reject the request");
}

void exact_budget(const ArrayView& view, std::size_t bytes) {
    const auto actual = view.copy_buffer_bytes(Validation::full, bytes);
    if (actual != bytes)
        throw std::runtime_error("copy footprint expected " + std::to_string(bytes) +
                                 " bytes, got " + std::to_string(actual));
    auto copied = view.copy(Validation::full, bytes);
    copied.validate();
    expect_error(ErrorCode::invalid_argument,
        [&] { (void)view.copy_buffer_bytes(Validation::full, bytes - 1); });
    expect_error(ErrorCode::invalid_argument,
        [&] { (void)view.copy(Validation::full, bytes - 1); });
}

Array sample(Layout layout) {
    constexpr std::size_t nnz = 6;
    auto result = layout == Layout::csr
        ? Array::csr<float, std::uint8_t, std::uint16_t>(3, 4, nnz)
        : Array::csc<float, std::uint8_t, std::uint16_t>(3, 4, nnz);
    if (layout == Layout::csr) {
        auto a = result.edit().csr<float, std::uint8_t, std::uint16_t>();
        const std::uint8_t indices[] = {0, 2, 1, 3, 2, 3};
        const std::uint16_t offsets[] = {0, 2, 4, 6};
        for (std::size_t i = 0; i < nnz; ++i) { a.values()[i] = float(i + 1); a.unsafe_indices()[i] = indices[i]; }
        for (std::size_t i = 0; i < 4; ++i) a.unsafe_offsets()[i] = offsets[i];
    } else {
        auto a = result.edit().csc<float, std::uint8_t, std::uint16_t>();
        const std::uint8_t indices[] = {0, 2, 0, 1, 1, 2};
        const std::uint16_t offsets[] = {0, 2, 4, 6, 6};
        for (std::size_t i = 0; i < nnz; ++i) { a.values()[i] = float(i + 1); a.unsafe_indices()[i] = indices[i]; }
        for (std::size_t i = 0; i < 5; ++i) a.unsafe_offsets()[i] = offsets[i];
    }
    result.validate();
    return result;
}

void csr_cases() {
    auto source = sample(Layout::csr);
    auto all = source.view().select({Axis::all(), Axis::all()});
    exact_budget(all, 38); // values + indices + four offsets

    auto affine = source.view().select({Axis::all(), Axis::stride(3, 2, 2, true)});
    exact_budget(affine, 55); // selected-major offsets + three entries
    auto affine_copy = affine.copy();
    auto affine_out = affine_copy.view().csr<float, std::uint8_t, std::uint16_t>();
    check(affine_out.nnz() == 3 && affine_out.offsets()[0] == 0 &&
          affine_out.offsets()[1] == 0 && affine_out.offsets()[2] == 2 &&
          affine_out.offsets()[3] == 3 && affine_out.indices()[0] == 1 &&
          affine_out.indices()[1] == 0 && affine_out.indices()[2] == 0 &&
          affine_out.values()[0] == 3 && affine_out.values()[1] == 4 &&
          affine_out.values()[2] == 6, "CSR affine copy changed entry order");

    auto gathered = source.view().select({Axis::gather({1, 1}), Axis::gather({3, 1, 3})});
    exact_budget(gathered, 116); // offsets, direct table, duplicate gather, and output
    auto gathered_copy = gathered.copy();
    auto gathered_out = gathered_copy.view().csr<float, std::uint8_t, std::uint16_t>();
    const std::uint8_t indices[] = {1, 0, 2, 1, 0, 2};
    const float values[] = {3, 4, 4, 3, 4, 4};
    check(gathered_out.nnz() == 6 && gathered_out.offsets()[0] == 0 &&
          gathered_out.offsets()[1] == 3 && gathered_out.offsets()[2] == 6,
          "CSR duplicate row gather offsets differ");
    for (std::size_t i = 0; i < 6; ++i)
        check(gathered_out.indices()[i] == indices[i] && gathered_out.values()[i] == values[i],
              "CSR duplicate row/column gather changed order");

    auto empty_major = source.view().select({Axis::gather({}), Axis::all()});
    exact_budget(empty_major, 10);
    auto empty_copy = empty_major.copy();
    auto empty_out = empty_copy.view().csr<float, std::uint8_t, std::uint16_t>();
    check(empty_out.rows() == 0 && empty_out.columns() == 4 && empty_out.nnz() == 0 &&
          empty_out.offsets()[0] == 0, "empty CSR selection differs");

    auto empty_minor = source.view().select({Axis::all(), Axis::gather({})});
    exact_budget(empty_minor, 40);

    auto sorted_source = Array::csr<float, std::uint8_t, std::uint16_t>(1, 8, 1);
    {
        auto a = sorted_source.edit().csr<float, std::uint8_t, std::uint16_t>();
        a.values()[0] = 9; a.unsafe_indices()[0] = 7;
        a.unsafe_offsets()[0] = 0; a.unsafe_offsets()[1] = 1;
    }
    auto sorted_gather = sorted_source.view().select({Axis::all(), Axis::gather({7})});
    exact_budget(sorted_gather, 41); // one sorted pair, not a source-sized table
}

void csc_cases() {
    auto source = sample(Layout::csc);
    auto all = source.view().select({Axis::all(), Axis::all()});
    exact_budget(all, 40); // values + indices + five offsets

    auto affine = source.view().select({Axis::stride(2, 2, 2, true), Axis::all()});
    exact_budget(affine, 70);
    auto affine_copy = affine.copy();
    auto affine_out = affine_copy.view().csc<float, std::uint8_t, std::uint16_t>();
    check(affine_out.nnz() == 4 && affine_out.offsets()[0] == 0 &&
          affine_out.offsets()[1] == 2 && affine_out.offsets()[2] == 3 &&
          affine_out.offsets()[3] == 4 && affine_out.offsets()[4] == 4 &&
          affine_out.indices()[0] == 1 && affine_out.indices()[1] == 0 &&
          affine_out.indices()[2] == 1 && affine_out.indices()[3] == 0 &&
          affine_out.values()[0] == 1 && affine_out.values()[1] == 2 &&
          affine_out.values()[2] == 3 && affine_out.values()[3] == 6,
          "CSC affine copy changed entry order");

    auto gathered = source.view().select({Axis::gather({2, 0, 2}), Axis::gather({0, 0})});
    exact_budget(gathered, 108);
    auto gathered_copy = gathered.copy();
    auto gathered_out = gathered_copy.view().csc<float, std::uint8_t, std::uint16_t>();
    check(gathered_out.nnz() == 6 && gathered_out.offsets()[0] == 0 &&
          gathered_out.offsets()[1] == 3 && gathered_out.offsets()[2] == 6,
          "CSC duplicate column gather offsets differ");
    const std::uint8_t indices[] = {1, 0, 2, 1, 0, 2};
    const float values[] = {1, 2, 2, 1, 2, 2};
    for (std::size_t i = 0; i < 6; ++i)
        check(gathered_out.indices()[i] == indices[i] && gathered_out.values()[i] == values[i],
              "CSC duplicate row/column gather changed order");
}

void output_width_overflow() {
    auto index_source = Array::csr<float, std::uint8_t, std::uint8_t>(1, 256, 1);
    {
        auto a = index_source.edit().csr<float, std::uint8_t, std::uint8_t>();
        a.values()[0] = 1; a.unsafe_indices()[0] = 255;
        a.unsafe_offsets()[0] = 0; a.unsafe_offsets()[1] = 1;
    }
    std::vector<std::size_t> repeated_column(257, 255);
    auto index_overflow = index_source.view().select({Axis::all(), Axis::gather(repeated_column.data(), repeated_column.size())});
    expect_error(ErrorCode::invalid_sparse_structure,
        [&] { (void)index_overflow.copy_buffer_bytes(Validation::full, std::numeric_limits<std::size_t>::max()); });
    expect_error(ErrorCode::invalid_sparse_structure,
        [&] { (void)index_overflow.copy(); });

    auto offset_source = Array::csc<float, std::uint8_t, std::uint8_t>(1, 1, 1);
    {
        auto a = offset_source.edit().csc<float, std::uint8_t, std::uint8_t>();
        a.values()[0] = 1; a.unsafe_indices()[0] = 0;
        a.unsafe_offsets()[0] = 0; a.unsafe_offsets()[1] = 1;
    }
    std::vector<std::size_t> repeated_column_major(257, 0);
    auto offset_overflow = offset_source.view().select({Axis::all(), Axis::gather(repeated_column_major.data(), repeated_column_major.size())});
    expect_error(ErrorCode::invalid_sparse_structure,
        [&] { (void)offset_overflow.copy_buffer_bytes(Validation::full, std::numeric_limits<std::size_t>::max()); });
    expect_error(ErrorCode::invalid_sparse_structure,
        [&] { (void)offset_overflow.copy(); });
}

}

int main() {
    try {
        csr_cases();
        csc_cases();
        output_width_overflow();
        std::cout << "copy_footprint passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
