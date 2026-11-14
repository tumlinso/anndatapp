#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <new>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import anndatapp.mudata;

using namespace anndatapp;
namespace {
bool track_largest_allocation = false;
std::size_t largest_allocation = 0;
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}
AnnData matrix(std::vector<std::string> obs, std::vector<std::string> var,
               std::initializer_list<double> values) {
    auto a = AnnData::empty(obs.size(), var.size());
    a.set_obs(Table::with_index(std::move(obs)));
    a.set_var(Table::with_index(std::move(var)));
    auto x = Array::empty<double>({a.n_obs(), a.n_vars()});
    auto d = x.edit().dense<double>();
    std::size_t i = 0;
    for (double v : values) d.data()[i++] = v;
    a.set_x(std::move(x));
    return a;
}
std::vector<std::uint64_t> map(MuMapView v) {
    return v.size ? std::vector<std::uint64_t>(v.data, v.data + v.size) : std::vector<std::uint64_t>{};
}
void model_and_slices() {
    auto m = MuData::empty(3, 4);
    m.annotations().set_obs(Table::with_index({"a", "b", "c"}));
    m.annotations().set_var(Table::with_index({"g1", "g2", "p1", "p2"}));
    m.set_modality("rna", matrix({"a", "c"}, {"g1", "g2"}, {1, 2, 3, 4}),
                   {1, 0, 2}, {1, 2, 0, 0});
    m.set_modality("protein", matrix({"b", "c"}, {"p1", "p2"}, {5, 6, 7, 8}),
                   {0, 1, 2}, {0, 0, 1, 2});
    m.validate();
    auto old = m.view();
    check(old.modality_names() == std::vector<std::string>({"rna", "protein"}), "modality order");
    check(map(old.obs_map("rna")) == std::vector<std::uint64_t>({1, 0, 2}), "map contains absent membership");
    m.set_maps("rna", {2, 0, 1}, {1, 2, 0, 0});
    check(map(m.view().obs_map("rna")) == std::vector<std::uint64_t>({2, 0, 1}) &&
          map(old.obs_map("rna")) == std::vector<std::uint64_t>({1, 0, 2}),
          "explicit map update leaves retained snapshot intact");
    m.set_maps("rna", {1, 0, 2}, {1, 2, 0, 0});
    auto v = old.slice(indices({2, 0, 2}), indices({3, 1, 1}));
    check(v.n_obs() == 3 && v.n_vars() == 3, "selected global axes");
    check(map(v.obs_map("rna")) == std::vector<std::uint64_t>({1, 2, 3}), "RNA projected observations preserve duplicates");
    check(map(v.var_map("rna")) == std::vector<std::uint64_t>({0, 1, 2}), "RNA projected variables preserve duplicates");
    check(map(v.obs_map("protein")) == std::vector<std::uint64_t>({1, 0, 2}), "protein projection preserves absences");
    auto r = v.modality("rna");
    check(r.n_obs() == 3 && r.n_vars() == 2, "modality axes follow global mapping");
    auto rcopy = r.x().copy();
    auto x = rcopy.view().dense<double>();
    check(x.data()[0] == 4 && x.data()[1] == 4 && x.data()[2] == 2 && x.data()[3] == 2 &&
          x.data()[4] == 4 && x.data()[5] == 4, "selected values preserve order and duplicate indices");
    auto independent = v.copy();
    m.edit_modality("rna").edit_x().dense<double>().data()[3] = 99;
    check(v.modality("rna").x().copy().view().dense<double>().data()[0] == 99,
          "retained view sees writes through existing binding");
    check(independent.view().modality("rna").x().copy().view().dense<double>().data()[0] == 4,
          "explicit copy has independent values");
    m.erase_modality("rna");
    check(old.modality("rna").n_obs() == 2 && old.modality_names().size() == 2,
          "view retains old binding after modality removal");
    check(m.view().modality_names() == std::vector<std::string>({"protein"}), "remove updates owner");
    bool bad = false;
    try { m.set_modality("bad", matrix({"a"}, {"g1"}, {0}), {2, 0, 0}, {1, 0, 0, 0}); }
    catch (const Error& e) { bad = e.code() == ErrorCode::out_of_bounds; }
    check(bad, "out-of-range map rejected");
    bad = false;
    try { m.set_modality("bad", matrix({"a"}, {"g1"}, {0}), {0, 0, 0}, {1, 0, 0, 0}); }
    catch (const Error& e) { bad = e.code() == ErrorCode::shape_mismatch; }
    check(bad, "unmapped local observation rejected");
}
void budget_io(const std::string& path) {
    constexpr std::size_t n = 1024;
    std::vector<double> payload(n * n, 7.0);
    auto m = MuData::empty(n, 1);
    m.annotations().set_obsp("pair", Array::borrow_dense(payload.data(), {n, n},
                                                           {std::ptrdiff_t(n), 1}));
    auto a = AnnData::empty(n, 1);
    std::vector<std::uint64_t> obs(n);
    for (std::size_t i = 0; i < n; ++i) obs[i] = i + 1;
    m.set_modality("rna", std::move(a), std::move(obs), {1});
    largest_allocation = 0;
    track_largest_allocation = true;
    auto retained = m.view();
    track_largest_allocation = false;
    check(largest_allocation < n * sizeof(std::uint64_t), "retained view does not copy map payloads");
    { std::ofstream out(path, std::ios::binary); out << "unchanged"; }
    MuWriteOptions options;
    options.overwrite = true;
    options.scratch_bytes = 1;
    bool rejected = false;
    try { write_h5mu(path, m, options); } catch (const Error&) { rejected = true; }
    std::ifstream before(path, std::ios::binary);
    std::string marker; before >> marker;
    check(rejected && marker == "unchanged", "insufficient budget leaves destination untouched");
    before.close();
    m.annotations().set_uns("long", Element(std::string(256, 'x')));
    options.scratch_bytes = 64;
    rejected = false;
    try { write_h5mu(path, m, options); } catch (const Error&) { rejected = true; }
    std::ifstream before_string(path, std::ios::binary);
    marker.clear(); before_string >> marker;
    check(rejected && marker == "unchanged", "oversized string leaves destination untouched");
    before_string.close();
    m.annotations().set_uns(Element::mapping());
    options.scratch_bytes = 4096;
    largest_allocation = 0;
    track_largest_allocation = true;
    try { write_h5mu(path, m, options); }
    catch (...) { track_largest_allocation = false; throw; }
    track_largest_allocation = false;
    check(largest_allocation < payload.size() * sizeof(double),
          "bounded write does not allocate a full numerical payload");
    std::cerr << "largest C++ allocation during 8 MiB pairwise write: " << largest_allocation << " bytes\n";
    auto result = open_h5mu(path);
    auto v = result.obsp("pair").select({range{0, 1}, range{0, 1}}).read();
    check(v.view().dense<double>().data()[0] == 7.0, "bounded pairwise write retains values");
    result.close();

    auto sparse_data = MuData::empty(8, 0);
    auto sparse = Array::csr<double, std::int32_t, std::int32_t>(8, 8, 8);
    auto sd = sparse.edit().csr<double, std::int32_t, std::int32_t>();
    for (int i = 0; i < 8; ++i) {
        sd.values()[i] = i + 1; sd.unsafe_indices()[i] = i; sd.unsafe_offsets()[i] = i;
    }
    sd.unsafe_offsets()[8] = 8;
    sparse_data.annotations().set_obsp("sparse", std::move(sparse));
    auto selected = sparse_data.slice(indices({7, 5, 3, 1}), all);
    { std::ofstream out(path, std::ios::binary | std::ios::trunc); out << "unchanged"; }
    options.scratch_bytes = 64;
    rejected = false;
    try { write_h5mu(path, selected, options); } catch (const Error&) { rejected = true; }
    std::ifstream before_sparse(path, std::ios::binary);
    marker.clear(); before_sparse >> marker;
    check(rejected && marker == "unchanged", "oversized sparse packing leaves destination untouched");
    before_sparse.close();
    options.scratch_bytes = 4096;
    write_h5mu(path, selected, options);
    auto sparse_result = open_h5mu(path).obsp("sparse").read();
    auto sparse_view = sparse_result.view().csr<double, std::int32_t, std::int32_t>();
    check(sparse_view.nnz() == 4 && sparse_view.values()[0] == 8 && sparse_view.values()[3] == 2,
          "selected sparse write succeeds when packing fits budget");
}
void fixture_io(const std::string& input, const std::string& output, bool zarr) {
    auto b = zarr ? open_mudata_zarr(input) : open_h5mu(input);
    check(b.is_open() && b.n_obs() == 3 && b.n_vars() == 4, "backed global dimensions");
    check(b.axis() == MuAxis::observations, "backed axis defaults to observations");
    check(b.modality_names() == std::vector<std::string>({"rna", "protein"}), "backed modality order");
    check(b.obs().names() == std::vector<std::string>({"a", "c", "b"}), "backed global observation names");
    check(b.modality("rna").has_raw(), "backed modality raw available");
    auto selected = b.slice(indices({1, 0, 1}), indices({3, 1, 1}));
    check(selected.n_obs() == 3 && selected.n_vars() == 3, "backed slice global axes");
    check(map(selected.obs_map("rna")) == std::vector<std::uint64_t>({1, 2, 3}),
          "backed slice projects duplicate RNA observations");
    check(map(selected.var_map("protein")) == std::vector<std::uint64_t>({1, 0, 0}),
          "backed slice retains missing protein variables");
    auto selected_embedding = selected.obsm_array("embedding").read();
    auto embedding = selected_embedding.view().dense<double>();
    check(embedding.data()[0] == 2 && embedding.data()[2] == 0 && embedding.data()[4] == 2,
          "backed aligned annotation follows duplicate selection");
    auto selected_pair = selected.obsp("connectivity").read();
    auto pair = selected_pair.view().dense<float>();
    check(pair.data()[0] == 1 && pair.data()[2] == 1 && pair.data()[4] == 1,
          "backed pairwise annotation selects both axes");
    auto selected_copy = selected.read();
    check(selected_copy.view().modality("rna").n_obs() == 3 &&
          selected_copy.view().modality("rna").n_vars() == 2,
          "explicit backed-view materialization preserves projected axes");
    auto nested = selected.slice(indices({2, 0}), all);
    check(nested.n_obs() == 2 && map(nested.obs_map("rna")) == std::vector<std::uint64_t>({1, 2}),
          "nested backed selections compose maps");
    auto reverse = b.slice(range{std::nullopt, std::nullopt, -1}, all);
    check(map(reverse.obs_map("rna")) == std::vector<std::uint64_t>({0, 1, 2}),
          "backed reverse range projects missing memberships");
    auto reverse_x = reverse.modality("rna").x().read();
    check(reverse_x.view().dense<float>().data()[0] == 3 &&
          reverse_x.view().dense<float>().data()[2] == 1, "backed reverse range reorders modality rows");
    auto keep = b.modality("rna").x();
    auto m = b.read();
    m.validate();
    auto v = m.view();
    check(map(v.obs_map("rna")) == std::vector<std::uint64_t>({1, 2, 0}), "reference RNA obs map");
    check(map(v.obs_map("protein")) == std::vector<std::uint64_t>({0, 2, 1}), "reference protein obs map");
    check(v.annotations().obs().column("quality").values().dense<std::int32_t>().data()[2] == 30,
          "global observation metadata");
    check(v.annotations().uns().at("project").string() == "fixture", "global nested metadata");
    auto x = v.modality("rna").x().dense<float>();
    check(x.data()[0] == 1 && x.data()[3] == 4, "reference modality values");
    b.close();
    bool closed = false;
    try { (void)keep.read(); }
    catch (const Error&) { closed = true; }
    check(closed, "close invalidates retained backed handles");
    if (zarr) write_mudata_zarr(output, m); else write_h5mu(output, m);
    auto q = zarr ? read_mudata_zarr(output) : read_h5mu(output);
    check(q.view().modality("rna").has_raw(), "written modality preserves raw");
    check(map(q.view().var_map("protein")) == std::vector<std::uint64_t>({0, 0, 1, 2}),
          "written variable map");
    BackedOptions options; options.mode = AccessMode::read_write;
    auto writable = zarr ? open_mudata_zarr(output, options) : open_h5mu(output, options);
    auto before = writable.view();
    writable.set_maps("rna", {2, 1, 0}, {1, 2, 0, 0});
    check(map(before.obs_map("rna")) == std::vector<std::uint64_t>({1, 2, 0}),
          "backed map update retains prior snapshot");
    auto after = writable.view();
    auto changed = map(after.obs_map("rna"));
    if (changed != std::vector<std::uint64_t>({2, 1, 0})) {
        std::cerr << "observed updated obs map:"; for (auto x : changed) std::cerr << ' ' << x; std::cerr << '\n';
    }
    check(changed == std::vector<std::uint64_t>({2, 1, 0}), "backed map update persists explicitly");
    writable.set_maps("rna", {1, 2, 0}, {1, 2, 0, 0});
    bool rejected = false;
    try { writable.set_maps("rna", {3, 2, 0}, {1, 2, 0, 0}); }
    catch (const Error& e) { rejected = e.code() == ErrorCode::out_of_bounds; }
    check(rejected && map(writable.view().obs_map("rna")) == std::vector<std::uint64_t>({1, 2, 0}),
          "invalid backed map fails before changing stored mappings");
    writable.close();
}
void axis_io(const std::string& input, const std::string& output, bool zarr, int axis) {
    auto b = zarr ? open_mudata_zarr(input) : open_h5mu(input);
    check(int(b.axis()) == axis, "shared-axis mode read from reference");
    auto m = b.read(); m.validate();
    check(int(m.axis()) == axis, "shared-axis mode retained in memory");
    b.close();
    if (zarr) write_mudata_zarr(output, m); else write_h5mu(output, m);
    auto q = zarr ? read_mudata_zarr(output) : read_h5mu(output);
    check(int(q.axis()) == axis, "shared-axis mode written and reopened");
}
void rejection(const std::string& input, bool zarr, std::string_view defect) {
    bool rejected = false;
    try {
        auto data = zarr ? open_mudata_zarr(input) : open_h5mu(input);
        if (defect == "subtype") (void)data.modality("rna");
        if (defect == "unknown") (void)data.read();
    } catch (const Error& e) { rejected = e.code() == ErrorCode::unsupported_encoding; }
    check(rejected, "unsupported root, modality subtype or shared axis is rejected");
}
}
void* operator new(std::size_t size) {
    if (track_largest_allocation) largest_allocation = std::max(largest_allocation, size);
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--budget") budget_io(argv[2]);
        else if (argc == 5 && std::string(argv[1]) == "--io") fixture_io(argv[3], argv[4], std::string(argv[2]) == "zarr");
        else if (argc == 5 && std::string(argv[1]) == "--reject")
            rejection(argv[3], std::string(argv[2]) == "zarr", argv[4]);
        else if (argc == 6 && std::string(argv[1]) == "--axis-io")
            axis_io(argv[3], argv[4], std::string(argv[2]) == "zarr", std::stoi(argv[5]));
        else model_and_slices();
    }
    catch (const std::exception& e) { std::cerr << "unexpected exception: " << e.what() << '\n'; return 1; }
    return failures ? 1 : 0;
}
