#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import anndatapp;

using namespace anndatapp;

namespace {
constexpr std::uint16_t source_bits[]{
    0x3800, 0xc000, 0x0001, 0x8000,
    0x4280, 0x4800, 0x7eab, 0x0400,
    0xbc00, 0x0100, 0x7bff, 0x7c00,
    0x0000, 0x3555, 0xfc00, 0x7dcb
};
constexpr std::uint16_t update_bits[]{0x8001, 0xc555, 0x0002};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint16_t expected_x(std::size_t row, std::size_t col, bool missing_chunks) {
    return missing_chunks && (row >= 2 || col >= 2) ? 0x3555 : source_bits[4 * row + col];
}

void check_array(ArrayView a, bool missing_chunks = false) {
    require(a.layout() == Layout::dense && a.dtype() == DType::float16 &&
            a.rank() == 2 && a.extent(0) == 4 && a.extent(1) == 4,
            "dense FP16 array metadata differs");
    require(a.bytes().element_size() == 2, "dense FP16 element width differs");
    const auto values = a.dense<Float16>();
    for (std::size_t i = 0; i < 4; ++i)
        for (std::size_t j = 0; j < 4; ++j)
            require(values.data()[4 * i + j].bits == expected_x(i, j, missing_chunks),
                    "dense FP16 payload bits differ");
}

void check_backed(const BackedAnnData& backed, bool missing_chunks) {
    auto x = backed.x();
    require(x.dtype() == DType::float16 && x.layout() == Layout::dense &&
            x.shape() == std::vector<std::size_t>{4, 4}, "backed FP16 metadata differs");
    check_array(x.read().view(), missing_chunks);
    check_array(backed.layer("half").read().view());
    check_array(backed.raw_x().read().view());

    auto selected = x.select({indices{3, 1}, indices{2, 0}});
    auto prepared = selected.prepare_read();
    auto requirements = prepared.requirements();
    require(requirements.dtype == DType::float16 &&
            requirements.shape == std::vector<std::size_t>{2, 2},
            "prepared FP16 requirements differ");
    Float16 reversed[4]{};
    prepared.read_into(DenseBuffer::from(reversed + 3, {2, 2}, {-4, -2}));
    require(reversed[3].bits == expected_x(3, 2, missing_chunks) &&
            reversed[2].bits == expected_x(3, 0, missing_chunks) &&
            reversed[1].bits == expected_x(1, 2, missing_chunks) &&
            reversed[0].bits == expected_x(1, 0, missing_chunks),
            "gathered FP16 read into negative strides differs");
}

void update_backed(BackedAnnData& backed, bool missing_chunks) {
    auto x = backed.x();
    auto transfer = x.prepare_transfer();
    auto value = Float16::from_bits(update_bits[0]);
    transfer.write_from({3, 3}, {1, 1}, DenseBuffer::from(&value, {1, 1}));
    Float16 actual[4]{};
    transfer.read_into({2, 2}, {2, 2}, DenseBuffer::from(actual, {2, 2}));
    require(actual[0].bits == expected_x(2, 2, missing_chunks) &&
            actual[1].bits == expected_x(2, 3, missing_chunks) &&
            actual[2].bits == expected_x(3, 2, missing_chunks) &&
            actual[3].bits == update_bits[0], "partial FP16 write changed adjacent values");

    auto source = Array::zeros<Float16>({1, 3});
    auto cells = source.edit().dense<Float16>().data();
    cells[0] = Float16::from_bits(update_bits[2]);
    cells[2] = Float16::from_bits(update_bits[1]);
    x.select({indices{2}, indices{1, 0}}).write(
        source.view().select({Axis::all(), Axis::gather({2, 0})}));
    auto updated = x.read();
    const auto data = updated.view().dense<Float16>().data();
    require(data[9].bits == update_bits[1] && data[8].bits == update_bits[2],
            "gathered FP16 write differs");
}

template<class F>
void expect_unsupported_dtype(std::string_view stage, F operation) {
    try {
        operation();
    } catch (const Error& error) {
        if (error.code() == ErrorCode::unsupported_dtype) return;
        throw std::runtime_error(std::string(stage) + " returned another error at " +
                                 std::string(error.path()) + ": " + error.what());
    }
    throw std::runtime_error(std::string(stage) + " accepted sparse FP16");
}

template<class Read, class Open, class Store, class Write>
void run(std::string_view mode, char** paths, Read read, Open open,
         Store store, Write write, bool h5ad) {
    if (mode == "sparse") {
        expect_unsupported_dtype("eager read", [&] { (void)read(paths[0]); });
        auto backed = open(paths[0], {});
        expect_unsupported_dtype("backed metadata", [&] { (void)backed.x().dtype(); });
        expect_unsupported_dtype("backed read", [&] { (void)backed.x().read(); });
        expect_unsupported_dtype("prepared read", [&] { (void)backed.x().prepare_read(); });
        expect_unsupported_dtype("prepared transfer", [&] { (void)backed.x().prepare_transfer(); });
        expect_unsupported_dtype("sparse storage geometry", [&] { (void)backed.x().storage_geometry(); });
        auto raw_store = store(paths[0]);
        expect_unsupported_dtype("raw sparse payload", [&] { (void)raw_store.array("/X/data").read(); });
        expect_unsupported_dtype("backed export", [&] { write(paths[1], backed); });
#if ANNDATAPP_HAS_HDF5 && ANNDATAPP_HAS_ZARR
        expect_unsupported_dtype("cross-backend export", [&] {
            const std::string destination(paths[1]);
            if (h5ad) write_zarr(destination + ".zarr", backed);
            else write_h5ad(destination + ".h5ad", backed);
        });
#endif
        return;
    }

    const bool missing_chunks = mode == "fill";
    auto eager = read(paths[0]);
    check_array(eager.x(), missing_chunks);
    check_array(eager.layer("half"));
    check_array(eager.obsm("half").array());
    check_array(eager.raw_x());
    write(paths[1], eager);

    {
        auto backed = open(paths[0], {});
        check_backed(backed, missing_chunks);
#if defined(__linux__)
        if (h5ad && mode == "dense") {
            auto mapping = backed.x().map_readonly();
            require(mapping.byte_size() == sizeof(source_bits), "mapped FP16 width differs");
            const auto* values = mapping.template unsafe_data<Float16>();
            for (std::size_t i = 0; i < 16; ++i)
                require(values[i].bits == source_bits[i], "mapped FP16 bits differ");
        }
#endif
        write(paths[2], backed);
    }

    auto writable = open(paths[0], {AccessMode::read_write});
    update_backed(writable, missing_chunks);
    writable.close();
}
}

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if ((mode == "sparse" && argc != 5) ||
        ((mode == "dense" || mode == "foreign" || mode == "fill") && argc != 6) ||
        (mode != "sparse" && mode != "dense" && mode != "foreign" && mode != "fill")) {
        std::cerr << "usage: float16_interop_driver dense|foreign|fill|sparse h5ad|zarr2|zarr3 "
                     "INPUT OUTPUT [BACKED_OUTPUT]\n";
        return 2;
    }
    try {
        const std::string_view backend(argv[2]);
        if (backend == "h5ad") {
#if ANNDATAPP_HAS_HDF5
            run(mode, argv + 3,
                [](const char* path) { return read_h5ad(path); },
                [](const char* path, BackedOptions options) { return open_h5ad(path, options); },
                [](const char* path) { return open_hdf5_store(path); },
                [](const char* path, const auto& data) { write_h5ad(path, data); }, true);
#else
            throw std::runtime_error("HDF5 backend is disabled");
#endif
        } else if (backend == "zarr2" || backend == "zarr3") {
#if ANNDATAPP_HAS_ZARR
            const auto format = backend == "zarr2" ? ZarrFormat::v2 : ZarrFormat::v3;
            run(mode, argv + 3,
                [](const char* path) { return read_zarr(path); },
                [](const char* path, BackedOptions options) { return open_zarr(path, options); },
                [](const char* path) { return open_zarr_store(path); },
                [format](const char* path, const auto& data) {
                    ZarrWriteOptions options;
                    options.format = format;
                    write_zarr(path, data, options);
                }, false);
#else
            throw std::runtime_error("Zarr backend is disabled");
#endif
        } else {
            throw std::runtime_error("backend must be h5ad, zarr2, or zarr3");
        }
    } catch (const Error& error) {
        std::cerr << "AnnData++ error [" << static_cast<int>(error.code()) << "] at "
                  << error.path() << ": " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
