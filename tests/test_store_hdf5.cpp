#include <hdf5.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import anndatapp.h5ad;

using namespace anndatapp;

namespace {
void require(bool ok, std::string_view message) {
    if (!ok) throw std::runtime_error(std::string(message));
}
void sparse_fixture(const std::filesystem::path& path, bool csc) {
    auto d = AnnData::empty(4, 3);
    auto x = csc ? Array::csc<float, std::int32_t, std::int64_t>(4, 3, 7)
                 : Array::csr<float, std::int32_t, std::int64_t>(4, 3, 7);
    const float csr_values[]{2, 4, 3, 5, 0, 7, 8};
    const std::int32_t csr_indices[]{1, 2, 0, 1, 2, 0, 2};
    const std::int64_t csr_offsets[]{0, 1, 3, 5, 7};
    const float csc_values[]{10, 11, 12, 13, 14, 15, 16};
    const std::int32_t csc_indices[]{3, 0, 1, 1, 2, 0, 3};
    const std::int64_t csc_offsets[]{0, 2, 4, 7};
    if (csc) {
        auto v = x.edit().csc<float, std::int32_t, std::int64_t>();
        for (std::size_t i = 0; i < 7; ++i) { v.values()[i] = csc_values[i]; v.unsafe_indices()[i] = csc_indices[i]; }
        for (std::size_t i = 0; i < 4; ++i) v.unsafe_offsets()[i] = csc_offsets[i];
    } else {
        auto v = x.edit().csr<float, std::int32_t, std::int64_t>();
        for (std::size_t i = 0; i < 7; ++i) { v.values()[i] = csr_values[i]; v.unsafe_indices()[i] = csr_indices[i]; }
        for (std::size_t i = 0; i < 5; ++i) v.unsafe_offsets()[i] = csr_offsets[i];
    }
    d.set_x(std::move(x));
    write_h5ad(path.string(), d);
}
void replace_sparse_buffer(const std::filesystem::path& path, const char* name, hid_t type, const void* data) {
    const auto file = H5Fopen(path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
    require(file >= 0, "cannot open sparse fixture for corruption");
    const auto dataset = H5Dopen2(file, name, H5P_DEFAULT);
    require(dataset >= 0, "cannot open sparse fixture buffer");
    const auto status = H5Dwrite(dataset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, data);
    H5Dclose(dataset); H5Fclose(file);
    require(status >= 0, "cannot write corrupted sparse fixture");
}
void check_sparse_scans(const std::filesystem::path& base) {
    const auto path = base.string() + ".sparse";
    for (bool csc : {false, true}) {
        sparse_fixture(path, csc);
        auto d = open_h5ad(path);
        auto selected = d.x().select({indices{3, 1, 3}, indices{2, 0, 2}}).prepare_read();
        const auto r = selected.requirements();
        const auto n = csc ? std::size_t{6} : std::size_t{9};
        require(r.nnz == n && r.layout == (csc ? Layout::csc : Layout::csr) &&
                r.index_dtype == DType::int32 && r.offset_dtype == DType::int64,
                "sparse selected requirements changed");
        std::array<float, 9> values{}; std::array<std::int32_t, 9> indices_out{};
        std::array<std::int64_t, 4> offsets{};
        selected.read_into(SparseBuffer{values.data(), indices_out.data(), offsets.data(),
            DType::float32, DType::int32, DType::int64, n, n, offsets.size()});
        if (csc) {
            const std::array<float, 6> expected_values{16,16,10,10,16,16};
            const std::array<std::int32_t, 6> expected_indices{0,2,0,2,0,2};
            for (std::size_t i = 0; i < n; ++i)
                require(values[i] == expected_values[i] && indices_out[i] == expected_indices[i],
                        "CSC selected order or duplicate multiplicity changed");
            require(offsets == std::array<std::int64_t,4>{0,2,4,6}, "CSC selected offsets changed");
        } else {
            const std::array<float, 9> expected_values{7,8,8,4,4,3,7,8,8};
            const std::array<std::int32_t, 9> expected_indices{1,0,2,0,2,1,1,0,2};
            require(values == expected_values && indices_out == expected_indices,
                    "CSR selected order or duplicate multiplicity changed");
            require(offsets == std::array<std::int64_t,4>{0,3,6,9}, "CSR selected offsets changed");
        }
        if (csc) {
            auto repeated = d.x().select({indices{1,1,3}, indices{1,1,0}}).prepare_read();
            require(repeated.requirements().nnz == 9, "CSC repeated source entries changed requirements");
            repeated.read_into(SparseBuffer{values.data(), indices_out.data(), offsets.data(),
                DType::float32, DType::int32, DType::int64, values.size(), indices_out.size(), offsets.size()});
            require(values == std::array<float,9>{12,12,13,13,12,12,13,13,10} &&
                    indices_out == std::array<std::int32_t,9>{0,1,0,1,0,1,0,1,2} &&
                    offsets == std::array<std::int64_t,4>{0,4,8,9},
                    "CSC repeated source entries or selector multiplicity changed");
        }
        d.close();
    }
    sparse_fixture(path, false);
    const std::int64_t bad_offsets[]{0,1,4,3,7};
    replace_sparse_buffer(path, "/X/indptr", H5T_NATIVE_INT64, bad_offsets);
    {
        auto d = open_h5ad(path); bool rejected = false;
        try { (void)d.x().select({indices{2}, indices{0}}).prepare_read(); }
        catch (const Error& e) { rejected = e.code() == ErrorCode::invalid_sparse_structure; }
        require(rejected, "nonmonotonic interior offsets were not checked");
    }
    sparse_fixture(path, false);
    const std::int32_t bad_indices[]{1,2,3,1,2,0,2};
    replace_sparse_buffer(path, "/X/indices", H5T_NATIVE_INT32, bad_indices);
    {
        auto d = open_h5ad(path); bool rejected = false;
        try { (void)d.x().select({indices{1}, indices{0}}).prepare_read(); }
        catch (const Error& e) { rejected = e.code() == ErrorCode::invalid_sparse_structure; }
        require(rejected, "invalid filtered-out sparse index was not checked");
    }
}
void run(const std::filesystem::path& path) {
    auto store = create_hdf5_store(path.string(), {}, 512);
    auto attrs = Element::mapping();
    attrs.set("max", Element(std::numeric_limits<std::uint64_t>::max()));
    auto floats = Array::empty<float>({2, 2});
    auto fp = floats.edit().dense<float>().data();
    fp[0] = 1.25f; fp[1] = -2.5f; fp[2] = 3; fp[3] = 4;
    attrs.set("float", Element(std::move(floats)));
    auto flags = Array::empty<bool>({2, 2});
    auto bp = flags.edit().dense<bool>().data();
    bp[0] = true; bp[1] = false; bp[2] = false; bp[3] = true;
    attrs.set("bool", Element(std::move(flags)));
    attrs.set("mod-order", Element(StringArray::from_strings({0}, {})));
    store.set_attributes("/", attrs.view());
    auto update = Element::mapping(); update.set("new", Element(std::int64_t{7}));
    store.set_attributes("/", update.view());
    auto bad = Element::mapping(); bad.set("new", Element(std::int64_t{9}));
    bad.set("unsupported", Element::mapping());
    bool rejected = false;
    try { store.set_attributes("/", bad.view()); }
    catch (const Error& e) { rejected = e.code() == ErrorCode::unsupported_encoding; }
    require(rejected, "nested attribute must be rejected");
    store.close();
    std::ifstream input(path, std::ios::binary); input.seekg(512);
    char signature[8]{}; input.read(signature, 8);
    require(std::string_view(signature, 8) == std::string_view("\x89HDF\r\n\x1a\n", 8), "HDF5 signature must follow user block");
    auto read = open_hdf5_store(path.string());
    auto result = read.attributes(); auto view = result.view();
    require(view.at("max").dtype() == DType::uint64 && view.at("max").unsigned_integer() == std::numeric_limits<std::uint64_t>::max(), "uint64 scalar");
    require(view.at("new").signed_integer() == 7, "preflight and merge must preserve earlier attributes");
    auto f = view.at("float").array(); auto fdata = f.dense<float>();
    require(f.dtype() == DType::float32 && f.rank() == 2 && f.extent(0) == 2 && f.extent(1) == 2 && fdata.data()[1] == -2.5f, "float array");
    auto b = view.at("bool").array(); auto bdata = b.dense<bool>();
    require(b.dtype() == DType::boolean && b.rank() == 2 && bdata.data()[0] && !bdata.data()[1] && bdata.data()[3], "boolean array");
    auto order = view.at("mod-order").string_array();
    require(order.rank() == 1 && order.size() == 0, "empty string array");
    read.close();
    auto limited = create_hdf5_store(path.string() + ".budget", {}, 0, 64);
    (void)limited.create_strings("/values", {9});
    std::vector<std::string> values(9, "value");
    rejected = false;
    try { limited.write_strings("/values", {0}, {9}, values); }
    catch (const Error& e) { rejected = e.code() == ErrorCode::invalid_argument; }
    require(rejected, "string pointer staging must fail when its session budget is insufficient");
    rejected = false;
    try { limited.write_string("/too-large", std::string(64, 'x')); }
    catch (const Error& e) { rejected = e.code() == ErrorCode::invalid_argument; }
    require(rejected && !limited.exists("/too-large"), "scalar transfer capacity must fail before target creation");
    limited.close();
    auto unchanged = open_hdf5_store(path.string() + ".budget");
    require(unchanged.strings("/values").string(0).empty(), "rejected string pointer staging changed payload");
    unchanged.close();
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected temporary HDF5 path");
        run(argv[1]);
        check_sparse_scans(argv[1]);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
