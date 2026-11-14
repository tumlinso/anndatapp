#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <new>
#include <string>
#include <stdexcept>
#include <vector>

import anndatapp.mudata;
#if ANNDATAPP_HAS_ZARR
import anndatapp.zarr;
#endif
using namespace anndatapp;
namespace {
bool tracking = false;
std::size_t largest = 0;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void test_allocation() {
    constexpr std::size_t n = 100000;
    std::vector<float> values(n, 1);
    std::vector<std::int32_t> indices(n, 0);
    std::int32_t offsets[] = {0, static_cast<std::int32_t>(n)};
    auto a = Array::borrow_sparse(Layout::csr, 1, 1, n, values.data(), indices.data(), offsets);
    auto selected = a.view().select({Axis::gather({0, 0}), Axis::all()});
    bool rejected = false;
    largest = 0; tracking = true;
    try { (void)selected.copy(Validation::none, 64); }
    catch (const Error& e) { rejected = e.code() == ErrorCode::invalid_argument; }
    catch (...) { tracking = false; throw; }
    tracking = false;
    require(rejected && largest < n * sizeof(float), "oversize copy allocated a result buffer or was accepted");
}
void test_export(const std::string& path, Layout layout) {
    float values[] = {1, 2, 3, 4};
    std::int32_t indices[] = {0, 1, 0, 1}, offsets[] = {0, 2, 4};
    auto m = MuData::empty(2, 1);
    m.annotations().set_obsp("pair", Array::borrow_sparse(layout, 2, 2, 4, values, indices, offsets));
    auto selected = m.view().slice(anndatapp::indices({1, 0, 1}), all);
    { std::ofstream out(path, std::ios::binary | std::ios::trunc); out << "unchanged"; }
    MuWriteOptions opts; opts.overwrite = true; opts.scratch_bytes = 128;
    bool rejected = false;
    try { write_h5mu(path, selected, opts); }
    catch (const Error& e) { rejected = e.code() == ErrorCode::invalid_argument; }
    std::ifstream in(path, std::ios::binary); std::string marker; in >> marker; in.close();
    require(rejected && marker == "unchanged", "budget failure changed published target");
    opts.scratch_bytes = 4096;
    write_h5mu(path, selected, opts);
    auto q = read_h5mu(path);
    auto copied = q.view().annotations().obsp("pair").copy();
    auto out = layout == Layout::csr ? copied.view().csr<float, std::int32_t, std::int32_t>()
                                     : copied.view().csc<float, std::int32_t, std::int32_t>();
    const float expected[] = {3, 4, 4, 1, 2, 2, 3, 4, 4};
    const std::int32_t expected_indices[] = {1, 0, 2, 1, 0, 2, 1, 0, 2};
    require(out.nnz() == 9 && out.offsets()[0] == 0 && out.offsets()[1] == 3 &&
            out.offsets()[2] == 6 && out.offsets()[3] == 9, "exported sparse shape differs");
    for (std::size_t i = 0; i < 9; ++i)
        require(out.indices()[i] == expected_indices[i] && out.values()[i] == expected[i],
                "exported sparse selection differs");
}
#if ANNDATAPP_HAS_ZARR
void test_zarr(const std::string& path) {
    float values[] = {1, 2, 3, 4};
    std::int32_t indices[] = {0, 1, 0, 1}, offsets[] = {0, 2, 4};
    auto m = MuData::empty(2, 1);
    m.annotations().set_obsp("pair", Array::borrow_sparse(Layout::csr, 2, 2, 4, values, indices, offsets));
    auto selected = m.view().slice(anndatapp::indices({1, 0, 1}), all);
    { ZarrWriteOptions initial; initial.scratch_bytes = 4096;
      auto existing = create_zarr_store(path, initial); existing.close(); }
    { std::ofstream marker(path + "/sentinel", std::ios::binary | std::ios::trunc); marker << "unchanged"; }
    MuWriteOptions opts; opts.overwrite = true; opts.scratch_bytes = 1024;
    bool rejected = false;
    try { write_mudata_zarr(path, selected, opts); }
    catch (const Error& e) { rejected = e.code() == ErrorCode::invalid_argument &&
        std::string(e.what()).find("copy exceeds numerical buffer byte budget") != std::string::npos; }
    require(rejected && std::filesystem::exists(path + "/sentinel"),
            "Zarr sparse budget failure did not preserve the existing destination");
    opts.scratch_bytes = 4096;
    write_mudata_zarr(path, selected, opts);
    auto read = read_mudata_zarr(path);
    auto pair = read.view().annotations().obsp("pair").csr<float, std::int32_t, std::int32_t>();
    require(pair.nnz() == 9 && pair.values()[0] == 3 && pair.values()[1] == 4, "selected sparse Zarr roundtrip differs");

    ZarrWriteOptions z; z.scratch_bytes = 4096;
    auto store = create_zarr_store(path + "-dense", z);
    std::vector<float> input(512);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<float>(i);
    auto source = Array::borrow_dense(input.data(), {256}, {2});
    store.write_array("/strided", source.view());
    std::vector<std::size_t> positions(256);
    for (std::size_t i = 0; i < positions.size(); ++i) positions[i] = positions.size() - 1 - i;
    auto selected_dense = source.view().select({Axis::gather(positions.data(), positions.size())});
    store.write_array("/gathered", selected_dense);
    auto strided = store.array("/strided").read();
    auto gathered = store.array("/gathered").read();
    auto a = strided.view().dense<float>(), b = gathered.view().dense<float>();
    for (std::size_t i = 0; i < positions.size(); ++i) {
        require(a.data()[i] == static_cast<float>(2*i), "strided Zarr output differs");
        require(b.data()[i] == static_cast<float>(2*positions[i]), "gathered Zarr output differs");
    }
}

#endif
}
void* operator new(std::size_t n) { if (tracking) largest = std::max(largest, n); if (auto p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
int main(int argc, char** argv) {
    try { require(argc == 2, "expected output path"); test_allocation(); test_export(argv[1], Layout::csr); test_export(argv[1], Layout::csc);
#if ANNDATAPP_HAS_ZARR
        test_zarr(std::string(argv[1]) + ".zarr");
#endif
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
