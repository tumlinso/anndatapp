#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import anndatapp;
import anndatapp.h5ad;

namespace {
using namespace anndatapp;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kSourceRows = 4096;
constexpr std::size_t kSourceColumns = 1024;
constexpr std::size_t kRowStep = 4;
constexpr std::size_t kSelectedRows = kSourceRows / kRowStep;
constexpr std::size_t kScratchBytes = 64 * 1024;
constexpr int kTransferSamples = 5;
constexpr std::size_t kCoordinateCount = 1'000'000;
constexpr int kTransformSamples = 5;

float source_value(std::size_t row, std::size_t column) {
    return static_cast<float>(row) * 0.25f + static_cast<float>(column) * 0.5f;
}

void require(bool ok, std::string_view message) {
    if (!ok) throw std::runtime_error(std::string(message));
}

void prepare_fixture(const std::filesystem::path& path) {
    std::vector<float> values(kSourceRows * kSourceColumns);
    for (std::size_t row = 0; row < kSourceRows; ++row)
        for (std::size_t column = 0; column < kSourceColumns; ++column)
            values[row * kSourceColumns + column] = source_value(row, column);

    auto store = create_hdf5_store(path.string());
    auto borrowed = Array::borrow_dense(values.data(),
        {kSourceRows, kSourceColumns},
        {static_cast<std::ptrdiff_t>(kSourceColumns), 1});
    store.write_array("/X", borrowed.view());
    store.flush();
    store.close();

    std::cout << "fixture=HDF5-f32-contiguous"
              << " shape=" << kSourceRows << 'x' << kSourceColumns
              << " logical_bytes=" << values.size() * sizeof(float)
              << " file_bytes=" << std::filesystem::file_size(path)
              << " path=" << path.string() << '\n';
}

void verify_transfer(const std::vector<float>& output) {
    constexpr float kPadding = -12345.0f;
    const std::size_t output_stride = 2 * kSourceColumns;
    for (std::size_t row = 0; row < kSelectedRows; ++row) {
        const std::size_t source_row = row * kRowStep;
        for (std::size_t column = 0; column < kSourceColumns; ++column) {
            const auto base = row * output_stride + 2 * column;
            require(output[base] == source_value(source_row, column),
                    "selected transfer value mismatch");
            require(output[base + 1] == kPadding,
                    "strided transfer changed an output gap");
        }
    }
}

void measure_transfer(const std::filesystem::path& path) {
    BackedArray selected;
    const auto setup_start = Clock::now();
    {
        BackedOptions options;
        options.scratch_bytes = kScratchBytes;
        auto store = open_hdf5_store(path.string(), options);
        auto source = store.array("/X");
        selected = source.select({range{0, static_cast<std::ptrdiff_t>(kSourceRows),
                                        static_cast<std::ptrdiff_t>(kRowStep)}, all});
    } // The selected descriptor retains the backing session after the store owner ends.
    auto transfer = selected.prepare_transfer(kScratchBytes);
    const auto setup_end = Clock::now();

    constexpr std::size_t output_row_stride = 2 * kSourceColumns;
    std::vector<float> output(kSelectedRows * output_row_stride, -12345.0f);
    auto destination = DenseBuffer::from(output.data(),
        {kSelectedRows, kSourceColumns},
        {static_cast<std::ptrdiff_t>(output_row_stride * sizeof(float)),
         static_cast<std::ptrdiff_t>(2 * sizeof(float))});

    std::cout << "transfer=retained-backed-dense-selection"
              << " source_shape=" << kSourceRows << 'x' << kSourceColumns
              << " selection=rows[0:" << kSourceRows << ':' << kRowStep
              << "]xall output_shape=" << kSelectedRows << 'x' << kSourceColumns
              << " output_strides_bytes=" << output_row_stride * sizeof(float)
              << ',' << 2 * sizeof(float)
              << " scratch_bytes=" << kScratchBytes
              << " logical_bytes=" << kSelectedRows * kSourceColumns * sizeof(float)
              << " setup_ns="
              << std::chrono::duration_cast<std::chrono::nanoseconds>(setup_end - setup_start).count()
              << '\n';

    for (int sample = 0; sample < kTransferSamples; ++sample) {
        std::fill(output.begin(), output.end(), -12345.0f);
        const auto before = transfer.stats();
        const auto start = Clock::now();
        transfer.read_into({0, 0}, {kSelectedRows, kSourceColumns}, destination);
        const auto stop = Clock::now();
        const auto after = transfer.stats();
        verify_transfer(output);
        std::cout << "transfer_sample=" << sample + 1
                  << " elapsed_ns="
                  << std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()
                  << " read_calls=" << after.read_calls - before.read_calls
                  << " read_bytes=" << after.read_bytes - before.read_bytes
                  << " scratch_allocations=" << after.scratch_allocations - before.scratch_allocations
                  << " retained_scratch_bytes=" << after.scratch_bytes
                  << " correctness=pass\n";
    }
}

void measure_transform() {
    constexpr std::size_t shape[]{kCoordinateCount, 2};
    constexpr std::ptrdiff_t strides[]{2, 1};
    std::vector<double> input(kCoordinateCount * 2);
    std::vector<double> output(kCoordinateCount * 2, 0.0);
    for (std::size_t i = 0; i < kCoordinateCount; ++i) {
        input[2 * i] = static_cast<double>(i % 1000) * 0.01;
        input[2 * i + 1] = static_cast<double>(i % 607) * 0.125;
    }
    const auto borrowed_input = borrowed_dense(input.data(), 2, shape, strides);
    const auto borrowed_output = borrowed_dense(output.data(), 2, shape, strides);

    const SpatialAxis axes[]{SpatialAxis::x, SpatialAxis::y};
    const double matrix[]{1.25, -0.5, 2.0,
                          0.75, 2.0, -3.0};
    const auto transform = SpatialTransform::affine(axes, 2, axes, 2, matrix, 3);

    std::cout << "transform=borrowed-dense-coordinate-affine"
              << " shape=" << kCoordinateCount << "x2"
              << " input_bytes=" << input.size() * sizeof(double)
              << " output_bytes=" << output.size() * sizeof(double)
              << " samples=" << kTransformSamples << '\n';
    for (int sample = 0; sample < kTransformSamples; ++sample) {
        const auto start = Clock::now();
        transform_coordinates(transform, borrowed_input.data(), borrowed_input.stride(0),
                              borrowed_output.data(), borrowed_output.stride(0),
                              kCoordinateCount);
        const auto stop = Clock::now();
        for (const auto i : std::array<std::size_t, 3>{0, kCoordinateCount / 2,
                                                       kCoordinateCount - 1}) {
            const auto x = input[2 * i], y = input[2 * i + 1];
            require(std::abs(output[2 * i] - (1.25 * x - 0.5 * y + 2.0)) < 1e-12,
                    "coordinate transform x mismatch");
            require(std::abs(output[2 * i + 1] - (0.75 * x + 2.0 * y - 3.0)) < 1e-12,
                    "coordinate transform y mismatch");
        }
        std::cout << "transform_sample=" << sample + 1
                  << " elapsed_ns="
                  << std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()
                  << " correctness=pass\n";
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--prepare") {
            prepare_fixture(argv[2]);
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--transfer") {
            measure_transfer(argv[2]);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--transform") {
            measure_transform();
            return 0;
        }
        std::cerr << "usage: format-family-probe --prepare FILE | --transfer FILE | --transform\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
