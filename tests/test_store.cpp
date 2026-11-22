#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import anndatapp;

namespace {
using namespace anndatapp;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F run, const char* message) {
    try { run(); } catch (const Error&) { return; }
    throw std::runtime_error(message);
}
Array numbers() {
    auto a = Array::empty<double>({3, 2}); auto p = a.edit().dense<double>().data();
    for (std::size_t i = 0; i < 6; ++i) p[i] = double(i + 1);
    return a;
}
AnnData fixture() {
    auto a = AnnData::empty(3, 2); a.set_x(numbers());
    a.set_obs(Table::with_index({"a", "b", "c"}));
    a.set_var(Table::with_index({"u", "v"}));
    auto raw = Array::empty<std::int32_t>({3, 4}); auto p = raw.edit().dense<std::int32_t>().data();
    for (std::size_t i = 0; i < 12; ++i) p[i] = std::int32_t(i + 10);
    a.set_raw(std::move(raw), Table::with_index({"r", "s", "t", "w"}));
    return a;
}
void values(ArrayView array, const std::vector<double>& expected) {
    auto copy = array.copy(); auto p = copy.view().dense<double>();
    std::size_t n = 1; for (std::size_t i = 0; i < p.rank(); ++i) n *= p.extent(i);
    require(n == expected.size(), "array size differs");
    for (std::size_t i = 0; i < expected.size(); ++i) require(p.data()[i] == expected[i], "array value differs");
}
void values(const Array& array, const std::vector<double>& expected) { values(array.view(), expected); }
void exercise(BackedStore store, bool nested_json) {
    require(store.is_open() && store.writable(), "created store is not writable");
    auto original = fixture();
    store.write_anndata("/mod/rna", original.view());
    store.write_anndata("/mod/prot", original.view());
    auto rna = store.anndata("/mod/rna"); auto prot = store.anndata("/mod/prot");
    auto rna_view = rna.view(); auto x = rna.x();
    values(rna.read().x(), {1, 2, 3, 4, 5, 6});
    require(rna.n_obs() == 3 && rna.n_vars() == 2, "nested dimensions differ");
    require(rna.raw_var().size() == 4, "nested raw lost independent variables");
    auto selected = rna.slice(indices{2, 0, 2}, indices{1});
    values(selected.read().x(), {6, 2, 6});
    require(selected.raw_x().read().extent(1) == 4, "raw was sliced on main variables");
    auto changed = numbers(); changed.edit().dense<double>().data()[0] = 99;
    store.write_array("/mod/prot/X", changed.view());
    values(rna_view.x().read(), {1, 2, 3, 4, 5, 6});
    rejects([&] { (void)prot.view(); }, "affected sibling dataset was not invalidated");
    auto same_root = store.anndata("/mod/rna"); auto other_root = store.anndata("/mod/prot");
    auto physical_x = store.array("/mod/rna/X");
    rna.set("/obs/batch", Element(Column::from_values<std::int32_t>({1, 2, 3})));
    require(rna.obs().read().view().contains("batch"), "rooted structural write missed its dataset");
    rejects([&] { (void)same_root.n_obs(); }, "second handle to affected root stayed valid");
    require(other_root.n_obs() == 3, "rooted structural write invalidated another modality");
    values(physical_x.read(), {1, 2, 3, 4, 5, 6});
    store.write_array("/images/image/s0", original.x());
    auto raw = store.array("/images/image/s0");
    values(raw.select({indices{2, 0}, indices{1}}).read(), {6, 2});
    auto attrs = store.attributes("/images/image/s0");
    require(!attrs.view().contains("encoding-type"), "raw array gained AnnData encoding metadata");
    auto text = StringArray::from_strings({2}, {"alpha", "beta"});
    store.write_strings("/trees/nodes", text.view());
    auto strings = store.strings("/trees/nodes");
    require(strings.select({indices{1, 0, 1}}).read().view().string(0) == "beta", "raw string gather differs");
    store.write_string("/trees/json", "{\"tree\":{\"nodes\":{}}}");
    require(store.read_string("/trees/json") == "{\"tree\":{\"nodes\":{}}}", "raw scalar JSON string differs");
    auto numeric_encoding = Element::mapping(); numeric_encoding.set("encoding-type", Element("array")); numeric_encoding.set("encoding-version", Element("0.2.0"));
    auto incremental = store.create_array("/incremental/numeric", DType::float64, {5}, numeric_encoding.view());
    auto transfer = incremental.prepare_transfer();
    double first[] = {10, 20}, second[] = {30, 40, 50};
    transfer.write_from({0}, {2}, DenseBuffer::from(first, {2}));
    transfer.write_from({2}, {3}, DenseBuffer::from(second, {3}));
    values(store.element("/incremental/numeric").array().read(), {10, 20, 30, 40, 50});
    require(transfer.stats().write_calls >= 2, "incremental numeric transfer statistics missing");
    auto string_encoding = Element::mapping(); string_encoding.set("encoding-type", Element("string-array")); string_encoding.set("encoding-version", Element("0.2.0"));
    (void)store.create_strings("/incremental/text", {4}, string_encoding.view());
    store.write_strings("/incremental/text", {0}, {2}, {"one", "two"});
    store.write_strings("/incremental/text", {2}, {2}, {"three", "four"});
    auto written = store.element("/incremental/text").string_array().read();
    require(written.view().string(0) == "one" && written.view().string(3) == "four", "incremental string ranges differ");
    rejects([&] { store.write_strings("/incremental/text", {3}, {2}, {"bad", "range"}); }, "oversized string range accepted");
    rejects([&] { store.write_strings("/incremental/text", {0}, {2}, {"short"}); }, "short string batch accepted");
    require(store.strings("/incremental/text").string(3) == "four", "rejected string range changed payload");
    auto empty_array = store.create_array("/incremental/empty", DType::int64, {0}, numeric_encoding.view());
    require(empty_array.read().extent(0) == 0, "empty incremental numeric array differs");
    (void)store.create_strings("/incremental/empty-text", {0});
    store.write_strings("/incremental/empty-text", {0}, {0}, {});
    require(store.strings("/incremental/empty-text").read().view().size() == 0, "empty incremental string array differs");
    auto scalar_encoding = Element::mapping(); scalar_encoding.set("encoding-type", Element("string")); scalar_encoding.set("encoding-version", Element("0.2.0"));
    const std::string payload = "borrowed scalar";
    store.write_string("/incremental/scalar", std::string_view(payload), scalar_encoding.view());
    require(store.element("/incremental/scalar").read().view().string() == payload, "borrowed encoded scalar string differs");
    auto scalar = Element(std::int64_t(42)); store.write_element("/metadata/value", scalar.view());
    require(store.element("/metadata/value").read().view().signed_integer() == 42, "encoded scalar differs");
    auto meta = Element::mapping(); meta.set("tag", Element("kept"));
    store.set_attributes("/", meta.view());
    auto patch = Element::mapping(); patch.set("axis", Element(std::int64_t(-1)));
    store.set_attributes("/", patch.view());
    auto merged = store.attributes();
    require(merged.view().at("tag").string() == "kept" && merged.view().at("axis").signed_integer() == -1, "attribute merge lost metadata");
    if (nested_json) {
        auto list = Element::sequence(); list.append(Element(nullptr)); list.append(Element(std::uint64_t(UINT64_MAX)));
        auto nested = Element::mapping(); nested.set("values", std::move(list));
        auto payload = Element::mapping(); payload.set("spatialdata_attrs", std::move(nested));
        store.set_attributes("/images/image/s0", payload.view());
        auto result = store.attributes("/images/image/s0"); auto sequence = result.view().at("spatialdata_attrs").at("values");
        require(sequence.size() == 2 && sequence.at(std::size_t(0)).kind() == ElementKind::null && sequence.at(std::size_t(1)).unsigned_integer() == UINT64_MAX, "nested JSON metadata changed types/values");
    }
    // A deliberate AnnData projection reads TreeData slots without treating
    // topology as ordinary uns metadata or silently exporting it as AnnData.
    store.write_anndata("/tree", original.view());
    auto tree = Element::mapping(); tree.set("encoding-type", Element("treedata")); tree.set("encoding-version", Element("0.1.0"));
    store.set_attributes("/tree", tree.view()); store.create_group("/tree/obst");
    auto tree_view = store.anndata("/tree"); values(tree_view.read().x(), {1, 2, 3, 4, 5, 6});
    auto fresh = store.anndata("/mod/rna");
    store.write_anndata("/copied", fresh.view());
    values(store.anndata("/copied").read().x(), {1, 2, 3, 4, 5, 6});
    rejects([&] { store.write_anndata("/mod/rna/child", fresh.view()); }, "overlapping source was admitted");
    require(!store.exists("/mod/rna/child"), "rejected overlap changed source structure");
    // SnapATAC2 manifests omit only root encoding metadata. Their contained
    // elements still use strict AnnData encodings.
    store.create_group("/legacy");
    auto obs = Element(original.obs().copy()), var = Element(original.var().copy()), matrix = Element(original.x().copy());
    store.write_element("/legacy/obs", obs.view()); store.write_element("/legacy/var", var.view());
    store.write_element("/legacy/X", matrix.view());
    rejects([&] { (void)store.anndata("/legacy"); }, "unversioned root admitted by strict default");
    values(store.anndata("/legacy", true).read().x(), {1, 2, 3, 4, 5, 6});
    auto partial = Element::mapping(); partial.set("encoding-type", Element("anndata"));
    store.set_attributes("/legacy", partial.view());
    rejects([&] { (void)store.anndata("/legacy", true); }, "partially versioned root accepted as legacy");
    auto old = store.array("/images/image/s0");
    store.write_array("/images/image/s0", changed.view());
    rejects([&] { (void)old.read(); }, "raw array survived structural replacement");
    rejects([&] { (void)store.exists("/bad/../path"); }, "parent traversal accepted");
    auto closed_scope = store.anndata("/mod/rna"); closed_scope.close();
    require(store.is_open(), "closing one rooted scope closed the physical store");
    values(store.anndata("/mod/rna").read().x(), {1, 2, 3, 4, 5, 6});
    auto retained = store.array("/images/image/s0"); auto prepared = retained.prepare_transfer(4096);
    store.flush(); store.close();
    rejects([&] { (void)retained.read(); }, "physical close did not invalidate raw descriptor");
    double closed_output[6]{};
    rejects([&] { prepared.read_into({0, 0}, {3, 2}, DenseBuffer::from(closed_output, {3, 2})); }, "physical close did not invalidate raw transfer context");
    rejects([&] { (void)rna.n_obs(); }, "physical close did not invalidate rooted dataset");
    (void)x;
}
void root_and_retention(BackedStore store) {
    auto original = fixture(); store.write_anndata("/", original.view());
    values(store.anndata().read().x(), {1, 2, 3, 4, 5, 6});
    auto child = store.anndata();
    auto retained = child.x(); store = BackedStore{};
    values(retained.read(), {1, 2, 3, 4, 5, 6});
    child.close(); rejects([&] { (void)retained.read(); }, "closed rooted descriptor stayed valid");
}
void raw_prepared(BackedStore store) {
    auto original = numbers(); store.write_array("/X", original.view());
    auto raw = store.array("/X"); auto geometry = raw.storage_geometry();
    require(geometry.layout == Layout::dense && geometry.dtype == DType::float64 && geometry.writable,
            "raw geometry lost layout, dtype or write capability");
    require(geometry.shape == std::vector<std::size_t>({3, 2}) && geometry.source_shape == geometry.shape,
            "raw geometry shape differs");
    auto gather = raw.select({indices{2, 0, 2}, indices{1}});
    auto selected_geometry = gather.storage_geometry();
    require(selected_geometry.shape == std::vector<std::size_t>({3, 1}) && !selected_geometry.axes[0].affine,
            "raw gathered geometry differs");
    auto gathered = gather.prepare_transfer(4096); double values_out[3]{};
    gathered.read_into({0, 0}, {3, 1}, DenseBuffer::from(values_out, {3, 1}));
    require(values_out[0] == 6 && values_out[1] == 2 && values_out[2] == 6, "raw prepared gather read differs");
    double replacements[] = {10, 20, 30};
    gathered.write_from({0, 0}, {3, 1}, DenseBuffer::from(replacements, {3, 1}));
    values(raw.read(), {1, 20, 3, 4, 5, 30});
    auto affine = raw.select({range{0, 3, 2}, all});
    auto affine_geometry = affine.storage_geometry();
    require(affine_geometry.shape == std::vector<std::size_t>({2, 2}) && affine_geometry.axes[0].affine &&
            affine_geometry.axes[0].step == 2, "raw affine geometry differs");
    auto bounded = affine.prepare_transfer(4096); double selected[4]{};
    bounded.read_into({0, 0}, {2, 2}, DenseBuffer::from(selected, {2, 2}));
    require(selected[0] == 1 && selected[1] == 20 && selected[2] == 5 && selected[3] == 30,
            "raw prepared affine read differs");
    double updated[] = {11, 22, 55, 66};
    bounded.write_from({0, 0}, {2, 2}, DenseBuffer::from(updated, {2, 2}));
    values(raw.read(), {11, 22, 3, 4, 55, 66});
    require(bounded.scratch_capacity() <= 4096 && bounded.stats().read_calls && bounded.stats().write_calls,
            "raw prepared context lost bounded accounting");
    require(!store.attributes("/X").view().contains("encoding-type"), "raw transfer injected encoding attributes");
    store.flush(); raw = BackedArray{}; gather = BackedArray{}; affine = BackedArray{};
    gathered = TransferContext{}; store = BackedStore{};
    bounded.read_into({0, 0}, {2, 2}, DenseBuffer::from(selected, {2, 2}));
    require(selected[0] == 11 && selected[3] == 66, "raw transfer lost retained physical session");
}
void raw_float16(BackedStore store) {
    auto raw = store.create_array("/images/image/s0", DType::float16, {2});
    auto transfer = raw.prepare_transfer();
    Float16 values[] = {Float16::from_bits(0x8000), Float16::from_bits(0x7e4a)};
    transfer.write_from({0}, {2}, DenseBuffer::from(values, {2}));
    auto copy = raw.read();
    auto view = copy.view().dense<Float16>();
    require(raw.storage_geometry().dtype == DType::float16 &&
            view.data()[0].bits == values[0].bits && view.data()[1].bits == values[1].bits,
            "raw dense FP16 transfer or geometry differs");
    BlockStream stream(raw.storage_geometry());
    auto request = stream.next();
    require(request && request->estimated_bytes == 4, "FP16 stream size differs");

    auto rank_zero = Array::empty<Float16>({});
    rank_zero.edit().dense<Float16>().data()[0] = values[1];
    auto attrs = Element::mapping();
    attrs.set("encoding-type", Element("array"));
    attrs.set("encoding-version", Element("0.2.0"));
    store.write_array("/scalar-array", rank_zero.view(), attrs.view());
    auto element = store.element("/scalar-array").read();
    require(element.view().kind() == ElementKind::array &&
            element.view().array().dense<Float16>().data()[0].bits == values[1].bits,
            "rank-zero FP16 array lost its encoding or payload");

    auto unsupported = [](auto operation, const char* message) {
        try { operation(); } catch (const Error& error) {
            require(error.code() == ErrorCode::unsupported_dtype, message);
            return;
        }
        throw std::runtime_error(message);
    };
    attrs.set("encoding-type", Element("numeric-scalar"));
    unsupported([&] { store.set_attributes("/scalar-array", attrs.view()); },
                "FP16 array was relabeled as a numeric scalar");
    unsupported([&] { store.write_array("/new-scalar", rank_zero.view(), attrs.view()); },
                "FP16 numeric scalar write succeeded");
    require(!store.exists("/new-scalar"), "rejected FP16 scalar write created output");

    auto sparse = AnnData::empty(1, 1);
    sparse.set_x(Array::csr<float, std::int32_t, std::int32_t>(1, 1, 0));
    store.write_anndata("/sparse", sparse.view());
    unsupported([&] { (void)store.create_array("/sparse/X/data", DType::float16, {0}); },
                "raw creation accepted FP16 sparse values");
    unsupported([&] { store.write_array("/sparse/X/data", copy.view()); },
                "raw write accepted FP16 sparse values");
    require(store.anndata("/sparse").x().dtype() == DType::float32,
            "rejected raw FP16 write replaced valid sparse values");
}
void raw_read_only(BackedStore store) {
    auto array = store.array("/X"); require(!array.storage_geometry().writable, "read-only raw geometry is writable");
    auto selected = array.select({indices{2, 0}, indices{1}}); auto transfer = selected.prepare_transfer(4096);
    double output[2]{}; transfer.read_into({0, 0}, {2, 1}, DenseBuffer::from(output, {2, 1}));
    require(output[0] == 66 && output[1] == 22, "reopened raw prepared read differs");
    rejects([&] { transfer.write_from({0, 0}, {2, 1}, DenseBuffer::from(output, {2, 1})); },
            "read-only raw context accepted a write");
}
}
int main(int argc, char** argv) {
    try {
        const auto root = std::filesystem::path(argc > 1 ? argv[1] : "store-tests");
        std::filesystem::create_directories(root);
#if ANNDATAPP_HAS_HDF5
        exercise(create_hdf5_store((root / "store.h5").string()), false);
        root_and_retention(create_hdf5_store((root / "root.h5").string()));
        raw_prepared(create_hdf5_store((root / "raw.h5").string()));
        raw_float16(create_hdf5_store((root / "raw-fp16.h5").string()));
        raw_read_only(open_hdf5_store((root / "raw.h5").string()));
#endif
#if ANNDATAPP_HAS_ZARR
        exercise(create_zarr_store((root / "store.zarr").string()), true);
        root_and_retention(create_zarr_store((root / "root.zarr").string()));
        raw_prepared(create_zarr_store((root / "raw.zarr").string()));
        raw_float16(create_zarr_store((root / "raw-fp16.zarr").string()));
        raw_read_only(open_zarr_store((root / "raw.zarr").string()));
#endif
        std::cout << "rooted store tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
