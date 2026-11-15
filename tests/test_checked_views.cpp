#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

import anndatapp;

namespace {
using namespace anndatapp;
int failures = 0;
void expect(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
template<class F> bool rejected(F&& f, ErrorCode code) {
    try { f(); }
    catch (const Error& e) { return e.code() == code; }
    return false;
}
Array malformed() {
    auto a = Array::csr<double, std::int32_t, std::int32_t>(2, 2, 1);
    auto b = a.edit().csr<double, std::int32_t, std::int32_t>();
    b.values()[0] = 1; b.unsafe_indices()[0] = 0;
    b.unsafe_offsets()[0] = 0; b.unsafe_offsets()[1] = 1; b.unsafe_offsets()[2] = 0;
    return a;
}
void nested_validation() {
    for (int slot = 0; slot < 9; ++slot) {
        auto a = AnnData::empty(2, 2);
        switch (slot) {
            case 0: a.set_x(malformed()); break;
            case 1: a.set_layer("bad", malformed()); break;
            case 2: a.set_obsp("bad", malformed()); break;
            case 3: a.set_varp("bad", malformed()); break;
            case 4: a.set_obsm("bad", malformed()); break;
            case 5: a.set_varm("bad", malformed()); break;
            case 6: a.set_raw(malformed(), Table::numbered(2)); break;
            case 7: a.set_raw(Table::numbered(2)); a.set_raw_varm("bad", malformed()); break;
            case 8: a.set_uns("bad", Element(malformed())); break;
        }
        const auto v = a.view();
        expect(rejected([&] { a.validate(); }, ErrorCode::invalid_sparse_structure), "owner rejects invalid nested sparse storage");
        expect(rejected([&] { v.validate(); }, ErrorCode::invalid_sparse_structure), "retained view rejects invalid nested sparse storage");
    }
    auto c = Column::from_packed_strings({'a'}, {0, 2}, {}, Validation::none);
    expect(rejected([&] { c.view().validate(); }, ErrorCode::invalid_argument), "column view checks packed string endpoints");
    auto t = Table::numbered(1); t.set("bad", std::move(c));
    expect(rejected([&] { t.view().validate(); }, ErrorCode::invalid_argument), "table view validates its columns");
}
void preflight() {
    auto a = AnnData::empty(1, 1);
    auto x = Array::csr<double, std::int32_t, std::int32_t>(1, 1, 1000);
    auto b = x.edit().csr<double, std::int32_t, std::int32_t>();
    for (int i = 0; i < 1000; ++i) { b.values()[i] = i; b.unsafe_indices()[i] = 0; }
    b.unsafe_offsets()[0] = 0; b.unsafe_offsets()[1] = 1000; a.set_x(std::move(x));
    auto selected = a.slice(indices{0, 0});
    const auto h = detail::write_budgets(detail::WriteBackend::hdf5, 512);
    const auto z = detail::write_budgets(detail::WriteBackend::zarr, 1024);
    expect(h.packing_bytes == 512 && z.packing_bytes == 128, "backend packing budgets match actual session budgets");
    expect(rejected([&] { detail::preflight_anndata_write(selected, Validation::full, h); }, ErrorCode::invalid_argument), "HDF preflight rejects selected sparse packing");
    expect(rejected([&] { detail::preflight_anndata_write(selected, Validation::full, z); }, ErrorCode::invalid_argument), "Zarr preflight rejects selected sparse packing");
    detail::preflight_anndata_write(a.view(), Validation::full, h);
    auto s = AnnData::empty(1, 1); s.set_uns("long", Element(std::string(64, 'x')));
    expect(rejected([&] { detail::preflight_anndata_write(s.view(), Validation::full, detail::write_budgets(detail::WriteBackend::hdf5, 64)); }, ErrorCode::invalid_argument), "preflight rejects indivisible strings");
    auto invalid = AnnData::empty(2, 2); invalid.set_x(malformed());
    expect(rejected([&] { detail::preflight_anndata_write(invalid.view(), Validation::full, h); }, ErrorCode::invalid_sparse_structure), "preflight validates before dereferencing malformed payloads");
}
}
int main() {
    try { nested_validation(); preflight(); }
    catch (const std::exception& e) { std::cerr << "unexpected: " << e.what() << '\n'; return 1; }
    return failures ? 1 : 0;
}
