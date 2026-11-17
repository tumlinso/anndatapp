#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

import anndatapp.dataset;
import anndatapp.h5ad;

using namespace anndatapp;
namespace fs = std::filesystem;

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void expect_error(ErrorCode code, F&& f) {
    try { f(); }
    catch (const Error& e) { require(e.code() == code, e.what()); return; }
    throw std::runtime_error("expected anndatapp::Error");
}
template<class I, class O> Array sparse(Layout layout, bool second) {
    auto a = layout == Layout::csr ? Array::csr<float, I, O>(2, 3, 3) : Array::csc<float, I, O>(2, 3, 3);
    auto v = layout == Layout::csr ? a.edit().template csr<float, I, O>() : a.edit().template csc<float, I, O>();
    const float csr1[]{1, 2, 3}, csr2[]{4, 5, 6}, csc1[]{1, 3, 2}, csc2[]{6, 4, 5};
    const I csr_i1[]{0, 2, 1}, csr_i2[]{1, 2, 0}, csc_i1[]{0, 1, 0}, csc_i2[]{1, 0, 0};
    const O csr_o[]{0, 2, 3}, csc_o[]{0, 1, 2, 3};
    auto* values = layout == Layout::csr ? (second ? csr2 : csr1) : (second ? csc2 : csc1);
    auto* index = layout == Layout::csr ? (second ? csr_i2 : csr_i1) : (second ? csc_i2 : csc_i1);
    auto* offsets = layout == Layout::csr ? csr_o : csc_o;
    for (std::size_t i = 0; i < 3; ++i) { v.values()[i] = values[i]; v.unsafe_indices()[i] = index[i]; }
    for (std::size_t i = 0; i < (layout == Layout::csr ? 3 : 4); ++i) v.unsafe_offsets()[i] = offsets[i];
    return a;
}
void write_component(const fs::path& path, Layout layout, bool second) {
    auto a = AnnData::empty(2, 3);
    if (layout == Layout::dense) {
        auto x = Array::zeros<float>({2, 3});
        float first[]{1, 0, 2, 0, 3, 0}, last[]{0, 4, 5, 6, 0, 0};
        std::copy_n(second ? last : first, 6, x.edit().dense<float>().data());
        a.set_x(std::move(x));
    } else if (second) a.set_x(sparse<std::int64_t, std::int32_t>(layout, true));
    else a.set_x(sparse<std::int32_t, std::int64_t>(layout, false));
    a.set_obs(second ? Table::with_index({"shared", "last"}) : Table::with_index({"shared", "first"}));
    a.set_var(Table::with_index({"peak-z", "peak-a", "gene-m"}));
    write_h5ad(path.string(), a);
}
const float expected[]{0, 6, 0, 2, 1, 2, 5, 0, 5, 0, 6, 0};
std::uint64_t index_at(const std::byte* data, DType dtype, std::size_t i) {
    if (dtype == DType::int32) { std::int32_t x; std::memcpy(&x, data + i * 4, 4); return x; }
    if (dtype == DType::uint32) { std::uint32_t x; std::memcpy(&x, data + i * 4, 4); return x; }
    if (dtype == DType::int64) { std::int64_t x; std::memcpy(&x, data + i * 8, 8); return x; }
    if (dtype == DType::uint64) { std::uint64_t x; std::memcpy(&x, data + i * 8, 8); return x; }
    throw std::runtime_error("unexpected sparse integer type in fixture");
}
std::vector<float> unpack(ArrayView x) {
    std::vector<float> result(x.extent(0) * x.extent(1));
    if (x.layout() == Layout::dense) {
        auto v = x.dense<float>();
        for (std::size_t i = 0; i < x.extent(0); ++i)
            for (std::size_t j = 0; j < x.extent(1); ++j)
                result[i*x.extent(1) + j] = v.data()[std::ptrdiff_t(i)*v.stride(0) + std::ptrdiff_t(j)*v.stride(1)];
    } else {
        auto v = x.sparse_bytes();
        auto major = x.layout() == Layout::csr ? x.extent(0) : x.extent(1);
        for (std::size_t m = 0; m < major; ++m)
            for (auto i = index_at(v.offsets(), x.offset_dtype(), m); i < index_at(v.offsets(), x.offset_dtype(), m + 1); ++i) {
                auto minor = index_at(v.indices(), x.index_dtype(), i);
                auto row = x.layout() == Layout::csr ? m : minor;
                auto col = x.layout() == Layout::csr ? minor : m;
                float value; std::memcpy(&value, v.values() + i * sizeof(float), sizeof(value));
                result[row*x.extent(1) + col] += value;
            }
    }
    return result;
}
void check_mismatched_name_reads(const AnnDataSetView& selected, Layout layout, const float* expected_values) {
    auto requirements = selected.requirements();
    auto prepared = selected.prepare_x();
    auto r = prepared.requirements();
    require(r.shape == std::vector<std::size_t>({2, 1}) && requirements.shape == r.shape,
            "mismatched-name requirements differ");
    require(r.layout == layout && r.dtype == DType::float32, "mismatched-name layout differs");
    auto check_values = [&](const std::vector<float>& values, const char* message) {
        require(values.size() == 2 && std::equal(values.begin(), values.end(), expected_values), message);
    };
    if (layout == Layout::dense) {
        std::vector<float> values(2, -1);
        prepared.read_into(DenseBuffer::from(values.data(), {2, 1}));
        check_values(values, "prepared read with mismatched names differs");
        std::fill(values.begin(), values.end(), -1);
        selected.read_x_into(DenseBuffer::from(values.data(), {2, 1}));
        check_values(values, "convenience read with mismatched names differs");
    } else {
        require(r.index_dtype == DType::int64 && r.offset_dtype == DType::int64,
                "mismatched-name sparse widths were not promoted");
        auto read_sparse = [&](auto&& read) {
            std::vector<float> values(std::max<std::size_t>(r.nnz, 1), -1);
            std::vector<std::int64_t> indices(std::max<std::size_t>(r.nnz, 1), -1);
            const auto major = layout == Layout::csr ? r.shape[0] : r.shape[1];
            std::vector<std::int64_t> offsets(major + 1, -1);
            SparseBuffer destination{values.data(), indices.data(), offsets.data(), r.dtype, r.index_dtype,
                                    r.offset_dtype, r.nnz, r.nnz, offsets.size()};
            read(destination);
            std::vector<float> dense(2, 0);
            for (std::size_t m = 0; m < major; ++m) {
                for (auto i = offsets[m]; i < offsets[m + 1]; ++i) {
                    auto minor = std::size_t(indices[std::size_t(i)]);
                    auto row = layout == Layout::csr ? m : minor;
                    auto col = layout == Layout::csr ? minor : m;
                    require(row < 2 && col < 1, "mismatched-name sparse coordinate is out of bounds");
                    dense[row] += values[std::size_t(i)];
                }
            }
            return dense;
        };
        check_values(read_sparse([&](const SparseBuffer& dst) { prepared.read_into(dst); }),
                     "prepared sparse read with mismatched names differs");
        check_values(read_sparse([&](const SparseBuffer& dst) { selected.read_x_into(dst); }),
                     "convenience sparse read with mismatched names differs");
    }
    auto materialized = selected.materialize();
    require(materialized.n_obs() == 2 && materialized.n_vars() == 1 && materialized.has_x(),
            "mismatched-name materialization shape differs");
    check_values(unpack(materialized.x()), "materialization with mismatched names differs");
}
void check_checked_name_rejections(const AnnDataSetView& selected, Layout layout) {
    expect_error(ErrorCode::shape_mismatch, [&] { (void)selected.prepare_x(); });
    if (layout == Layout::dense) {
        float values[]{-7, -7};
        expect_error(ErrorCode::shape_mismatch, [&] {
            selected.read_x_into(DenseBuffer::from(values, {2, 1}));
        });
        require(values[0] == -7 && values[1] == -7, "checked name mismatch modified dense output");
    } else {
        float values[8]; std::int64_t indices[8], offsets[8];
        std::fill_n(values, 8, -7); std::fill_n(indices, 8, -7); std::fill_n(offsets, 8, -7);
        SparseBuffer destination{values, indices, offsets, DType::float32, DType::int64, DType::int64, 8, 8, 8};
        expect_error(ErrorCode::shape_mismatch, [&] { selected.read_x_into(destination); });
        require(std::all_of(values, values + 8, [](float x) { return x == -7; }) &&
                std::all_of(indices, indices + 8, [](std::int64_t x) { return x == -7; }) &&
                std::all_of(offsets, offsets + 8, [](std::int64_t x) { return x == -7; }),
                "checked name mismatch modified sparse output");
    }
}
void check_selected(const AnnDataSetView& selected, Layout layout) {
    require(selected.n_obs() == 4 && selected.n_vars() == 3, "selected shape differs");
    auto prepared = selected.prepare_x();
    auto moved = std::move(prepared);
    expect_error(ErrorCode::invalid_argument, [&] { (void)prepared.requirements(); });
    auto r = moved.requirements();
    require(moved.requirements().shape == r.shape, "repeated prepared requirements differ");
    require(r.layout == layout && r.dtype == DType::float32, "requirements differ");
    std::vector<float> dense(12);
    if (layout == Layout::dense) {
        auto target = DenseBuffer::from(dense.data() + 9, {4, 3}, {-12, 4});
        auto bad = target; bad.dtype = DType::float64;
        expect_error(ErrorCode::dtype_mismatch, [&] { moved.read_into(bad); });
        require(std::all_of(dense.begin(), dense.end(), [](float x) { return x == 0; }), "failed dense preflight changed output");
        moved.preflight(target); moved.read_into(target); moved.read_into(target);
        for (std::size_t i = 0; i < 4; ++i)
            for (std::size_t j = 0; j < 3; ++j)
                require(dense[(3 - i) * 3 + j] == expected[i * 3 + j], "strided dense selection differs");
    } else {
        require(r.index_dtype == DType::int64 && r.offset_dtype == DType::int64, "mixed sparse widths were not promoted");
        std::vector<float> values(r.nnz);
        std::vector<std::int64_t> indices(r.nnz), offsets((layout == Layout::csr ? 4 : 3) + 1);
        SparseBuffer target{values.data(), indices.data(), offsets.data(), r.dtype, r.index_dtype, r.offset_dtype,
                            values.size(), indices.size(), offsets.size()};
        moved.preflight(target); moved.read_into(target); moved.read_into(target);
        for (std::size_t major = 0; major + 1 < offsets.size(); ++major) {
            require(offsets[major] <= offsets[major + 1], "sparse offsets are not monotone");
            for (auto i = offsets[major]; i < offsets[major + 1]; ++i) {
                auto minor = std::size_t(indices[std::size_t(i)]);
                auto row = layout == Layout::csr ? major : minor;
                auto col = layout == Layout::csr ? minor : major;
                require(row < 4 && col < 3, "sparse index out of bounds");
                dense[row * 3 + col] += values[std::size_t(i)];
            }
        }
        for (std::size_t i = 0; i < 12; ++i) require(dense[i] == expected[i], "sparse selection differs");
        auto too_small = SparseBuffer{values.data(), indices.data(), offsets.data(), r.dtype, r.index_dtype, r.offset_dtype,
                                      r.nnz ? r.nnz - 1 : 0, indices.size(), offsets.size()};
        if (r.nnz) {
            auto old = values;
            expect_error(ErrorCode::shape_mismatch, [&] { moved.read_into(too_small); });
            require(values == old, "failed prepared transfer changed output");
        }
    }
    auto materialized = selected.materialize();
    require(materialized.n_obs() == 4 && materialized.n_vars() == 3 && materialized.has_x(), "materialized shape differs");
    auto values = unpack(materialized.x());
    for (std::size_t i = 0; i < values.size(); ++i)
        require(values[i] == expected[i], "materialized values differ");
    require(materialized.obs().name(1) == "shared" && materialized.obs().name(3) == "last", "observation order differs");
}
void check_collection(const fs::path& root, Layout layout) {
    auto dir = root / (layout == Layout::dense ? "dense" : layout == Layout::csr ? "csr" : "csc");
    fs::create_directories(dir / "copies");
    write_component(dir / "first.h5ad", layout, false);
    write_component(dir / "second.h5ad", layout, true);
    DatasetOptions options;
    options.backed.mode = AccessMode::read_write;
    auto d = create_h5ads((dir / "data.h5ads").string(), {{"first", "first.h5ad"}, {"second", "second.h5ad"}}, "sample", options);
    require(d.n_obs() == 4 && d.n_vars() == 3 && d.n_components() == 2 && d.has_x(), "collection shape differs");
    d.validate();
    // Component 1 is visited first, component 0 late, and component 1 repeats.
    auto selected = d.slice(indices{3, 0, 2, 3}, indices{2, 0, 2});
    check_selected(selected, layout);
    auto reverse_requirements = d.slice(range{std::nullopt, std::nullopt, -1}, all).prepare_x().requirements();
    require(reverse_requirements.layout == layout && reverse_requirements.shape == std::vector<std::size_t>({4, 3}),
            "reverse component requirements differ");
    if (layout != Layout::dense)
        require(reverse_requirements.index_dtype == DType::int64 && reverse_requirements.offset_dtype == DType::int64,
                "late reference component did not promote independent sparse widths");

    auto later_only = d.slice(indices{3, 2, 3}, indices{2, 0, 2}).prepare_x().requirements();
    require(later_only.layout == layout && later_only.dtype == DType::float32 &&
            later_only.shape == std::vector<std::size_t>({3, 3}), "later-only requirements differ");
    if (layout != Layout::dense)
        require(later_only.index_dtype == DType::int64 && later_only.offset_dtype == DType::int64,
                "unselected reference widths were not included independently");

    auto empty_rows = d.slice(range{0, 0, 1}, all).prepare_x().requirements();
    require(empty_rows.layout == layout && empty_rows.dtype == DType::float32 && empty_rows.nnz == 0 &&
            empty_rows.shape == std::vector<std::size_t>({0, 3}), "empty-row metadata differs");
    if (layout != Layout::dense)
        require(empty_rows.index_dtype == DType::int32 && empty_rows.offset_dtype == DType::int64,
                "empty-row reference sparse widths differ");

    auto empty_later_columns = d.slice(indices{3}, range{0, 0, 1}).prepare_x().requirements();
    require(empty_later_columns.layout == layout && empty_later_columns.dtype == DType::float32 &&
            empty_later_columns.nnz == 0 && empty_later_columns.shape == std::vector<std::size_t>({1, 0}),
            "empty later-component columns differ");
    if (layout != Layout::dense)
        require(empty_later_columns.index_dtype == DType::int64 && empty_later_columns.offset_dtype == DType::int64,
                "empty later-component reference widths were not promoted");

    auto duplicate = d.slice("shared", all);
    require(duplicate.n_obs() == 2, "duplicate label selection differs");
    auto duplicate_values = unpack(duplicate.materialize().x());
    const float duplicate_expected[]{1, 0, 2, 0, 4, 5};
    require(std::equal(duplicate_values.begin(), duplicate_values.end(), duplicate_expected),
            "default duplicate label selection resolved to the wrong rows");
    auto reversed = d.slice(range{std::nullopt, std::nullopt, -1}, range{std::nullopt, std::nullopt, -1}).materialize();
    auto reversed_values = unpack(reversed.x());
    const float reversed_expected[]{0, 0, 6, 5, 4, 0, 0, 3, 0, 2, 0, 1};
    require(std::equal(reversed_values.begin(), reversed_values.end(), reversed_expected), "reverse affine selection differs");
    auto empty = d.slice(range{0, 0, 1}, all).materialize();
    require(empty.n_obs() == 0 && empty.n_vars() == 3, "empty selection differs");
    expect_error(ErrorCode::invalid_argument, [&] { (void)d.slice(labels{"shared"}, all); });
    expect_error(ErrorCode::invalid_argument, [&] { d.copy_manifest((dir / "first.h5ad").string(), true); });
    auto manifest = dir / "copies" / "copy.h5ads";
    d.copy_manifest(manifest.string());
    auto copied = open_h5ads(manifest.string());
    check_selected(copied.slice(indices{3, 0, 2, 3}, indices{2, 0, 2}), layout);
    copied.close();
    expect_error(ErrorCode::invalid_argument, [&] { d.set_annotation("/uns/AnnDataSet", Element()); });
    auto survivor = d.view();
    auto stale = survivor.prepare_x();
    d.set_annotation("/uns/extra", Element("updated"));
    expect_error(ErrorCode::invalid_argument, [&] { (void)stale.requirements(); });
    auto closing = d.view().prepare_x();
    d.close();
    expect_error(ErrorCode::invalid_argument, [&] { (void)survivor.n_obs(); });
    expect_error(ErrorCode::invalid_argument, [&] { (void)closing.requirements(); });

    DatasetOptions bounded;
    bounded.backed.scratch_bytes = 512;
    auto limited = open_h5ads((dir / "data.h5ads").string(), bounded);
    auto wide = limited.slice(indices{3, 0}, indices{0, 1, 2, 0, 1});
    std::vector<float> untouched(10, -19);
    if (layout == Layout::dense) {
        expect_error(ErrorCode::invalid_argument, [&] { wide.read_x_into(DenseBuffer::from(untouched.data(), {2, 5})); });
        require(std::all_of(untouched.begin(), untouched.end(), [](float x) { return x == -19; }), "capacity failure modified dense output");
    } else {
        expect_error(ErrorCode::invalid_argument, [&] { (void)wide.requirements(); });
    }
    // 512 bytes leaves too little selector budget for retained piece descriptors;
    // this exercises bounded regeneration for all three physical layouts.
    auto narrow = limited.slice(indices{3, 0, 2, 3}, indices{2, 0, 2});
    check_selected(narrow, layout);
    auto narrow_later = limited.slice(indices{3, 2, 3}, indices{2, 0, 2}).prepare_x().requirements();
    require(narrow_later.layout == layout && narrow_later.shape == std::vector<std::size_t>({3, 3}),
            "low-budget later-only requirements differ");
    if (layout != Layout::dense)
        require(narrow_later.index_dtype == DType::int64 && narrow_later.offset_dtype == DType::int64,
                "low-budget fallback lost unselected reference widths");
    limited.close();
    expect_error(ErrorCode::io_error, [&] {
        DatasetOptions missing;
        missing.relocate = {{"first", "does-not-exist.h5ad"}};
        (void)open_h5ads(manifest.string(), std::move(missing));
    });
}
void check_narrow_widths(const fs::path& root, Layout layout) {
    auto dir = root / (layout == Layout::csr ? "narrow-csr" : "narrow-csc");
    fs::create_directories(dir);
    auto a = AnnData::empty(2, 1);
    auto x = layout == Layout::csr ? Array::csr<float, std::int8_t, std::int8_t>(2, 1, 1)
                                   : Array::csc<float, std::int8_t, std::int8_t>(2, 1, 1);
    auto v = layout == Layout::csr ? x.edit().csr<float, std::int8_t, std::int8_t>()
                                   : x.edit().csc<float, std::int8_t, std::int8_t>();
    v.values()[0] = 1; v.unsafe_indices()[0] = 0;
    v.unsafe_offsets()[0] = 0; v.unsafe_offsets()[1] = 1;
    if (layout == Layout::csr) v.unsafe_offsets()[2] = 1;
    a.set_x(std::move(x));
    a.set_obs(Table::with_index({"r0", "r1"}));
    a.set_var(Table::with_index({"v"}));
    write_h5ad((dir / "part.h5ad").string(), a);
    auto d = create_h5ads((dir / "data.h5ads").string(), {{"part", "part.h5ad"}});
    std::vector<std::ptrdiff_t> duplicates(129, 0);
    auto minor = layout == Layout::csr ? d.slice(indices{0}, indices(duplicates))
                                       : d.slice(indices(duplicates), indices{0});
    auto r = minor.requirements();
    require(r.nnz == 129 && r.index_dtype == DType::int64 && r.offset_dtype == DType::int64,
            "minor-axis duplicates did not promote sparse widths");
    std::vector<float> values(129, -1);
    std::vector<std::int64_t> positions(129, -1), offsets(2, -1);
    auto destination = SparseBuffer{values.data(), positions.data(), offsets.data(), r.dtype, r.index_dtype,
                                    r.offset_dtype, 128, positions.size(), offsets.size()};
    expect_error(ErrorCode::shape_mismatch, [&] { minor.read_x_into(destination); });
    require(values[0] == -1 && positions[0] == -1 && offsets[0] == -1,
            "capacity failure modified promoted output");
    destination.values_capacity = values.size();
    minor.read_x_into(destination);
    require(offsets[0] == 0 && offsets[1] == 129, "promoted offsets differ");
    for (std::size_t i = 0; i < 129; ++i)
        require(values[i] == 1 && positions[i] == std::int64_t(i), "promoted minor indices differ");

    auto major = layout == Layout::csr ? d.slice(indices(duplicates), indices{0})
                                       : d.slice(indices{0}, indices(duplicates));
    r = major.requirements();
    require(r.nnz == 129 && r.index_dtype == DType::int8 && r.offset_dtype == DType::int64,
            "major-axis duplicates did not promote offsets");
    std::vector<std::int8_t> narrow_positions(129, -1);
    std::vector<std::int64_t> wide_offsets(130, -1);
    major.read_x_into({values.data(), narrow_positions.data(), wide_offsets.data(), r.dtype, r.index_dtype,
                       r.offset_dtype, values.size(), narrow_positions.size(), wide_offsets.size()});
    for (std::size_t i = 0; i < 129; ++i)
        require(values[i] == 1 && narrow_positions[i] == 0 && wide_offsets[i] == std::int64_t(i),
                "promoted major offsets differ");
    require(wide_offsets[129] == 129, "final promoted offset differs");
}
void check_deferred_validation(const fs::path& root, Layout layout) {
    auto dir = root / (layout == Layout::dense ? "deferred-dense" :
                       layout == Layout::csr ? "deferred-csr" : "deferred-csc");
    fs::create_directories(dir);
    write_component(dir / "first.h5ad", layout, false);
    write_component(dir / "second.h5ad", layout, true);
    auto created = create_h5ads((dir / "data.h5ads").string(),
                                {{"first", "first.h5ad"}, {"second", "second.h5ad"}});
    created.close();
    BackedOptions options;
    options.mode = AccessMode::read_write;
    auto second = open_h5ad((dir / "second.h5ad").string(), options);
    auto edit = second.edit_structure();
    edit.set_obs(Table::with_index({"changed", "last"}));
    edit.set_var(Table::with_index({"peak-z", "changed", "gene-m"}));
    edit.apply();
    second.close();

    DatasetOptions dataset_options;
    require(!dataset_options.check_axis_names && dataset_options.backed.validation == Validation::full,
            "default name-check policy or backed validation changed");
    auto collection = open_h5ads((dir / "data.h5ads").string(), dataset_options);
    auto selected = collection.slice(indices{3, 0}, indices{0, 2});
    auto good = unpack(selected.materialize().x());
    require(good[0] == 6 && good[1] == 0 && good[2] == 1 && good[3] == 2,
            "unaffected selection failed deferred validation");
    expect_error(ErrorCode::shape_mismatch, [&] { collection.validate(); });
    auto bad_obs = collection.slice(indices{0, 2}, indices{0});
    auto bad_var = collection.slice(indices{1, 3}, indices{1});
    const float obs_values[]{1, 0}, var_values[]{3, 0};
    check_mismatched_name_reads(bad_obs, layout, obs_values);
    check_mismatched_name_reads(bad_var, layout, var_values);

    DatasetOptions checked_options;
    checked_options.check_axis_names = true;
    auto checked = open_h5ads((dir / "data.h5ads").string(), checked_options);
    expect_error(ErrorCode::shape_mismatch, [&] { checked.validate(); });
    check_checked_name_rejections(checked.slice(indices{0, 2}, indices{0}), layout);
    check_checked_name_rejections(checked.slice(indices{1, 3}, indices{1}), layout);
    checked.close();
}
void check_create_requires_common_var_names(const fs::path& root) {
    auto dir = root / "create-common-var-names";
    fs::create_directories(dir);
    write_component(dir / "first.h5ad", Layout::dense, false);
    auto second = AnnData::empty(2, 3);
    second.set_obs(Table::with_index({"shared", "last"}));
    second.set_var(Table::with_index({"peak-z", "different", "gene-m"}));
    second.set_x(Array::zeros<float>({2, 3}));
    write_h5ad((dir / "second.h5ad").string(), second);
    DatasetOptions options;
    options.check_axis_names = false;
    options.backed.validation = Validation::full;
    auto manifest = dir / "invalid.h5ads";
    expect_error(ErrorCode::shape_mismatch, [&] {
        (void)create_h5ads(manifest.string(), {{"first", "first.h5ad"}, {"second", "second.h5ad"}},
                           "sample", options);
    });
    require(!fs::exists(manifest), "create accepted mismatched common variable names");
}
void check_multi_batch_indexes(const fs::path& root) {
    auto dir = root / "multi-batch-indexes";
    fs::create_directories(dir);
    std::vector<std::string> names;
    for (std::size_t j = 0; j < 128; ++j) names.push_back("variable-" + std::to_string(j));
    for (std::size_t k = 0; k < 2; ++k) {
        auto a = AnnData::empty(2, names.size());
        a.set_obs(Table::with_index(k ? std::vector<std::string>{"c2", "c3"}
                                     : std::vector<std::string>{"c0", "c1"}));
        a.set_var(Table::with_index(names));
        auto x = Array::empty<float>({2, names.size()});
        auto* values = x.edit().dense<float>().data();
        for (std::size_t i = 0; i < 2; ++i)
            for (std::size_t j = 0; j < names.size(); ++j) values[i*names.size()+j] = float(k*1000+i*200+j);
        a.set_x(std::move(x));
        write_h5ad((dir / (k ? "second.h5ad" : "first.h5ad")).string(), a);
    }
    auto manifest = dir / "data.h5ads";
    auto created = create_h5ads(manifest.string(), {{"first", "first.h5ad"}, {"second", "second.h5ad"}});
    created.close();
    DatasetOptions options; options.backed.scratch_bytes = 4096; options.check_axis_names = true;
    auto d = open_h5ads(manifest.string(), options);
    auto reversed = d.slice(indices{0, 2}, range{std::nullopt, std::nullopt, -1}).prepare_x();
    std::vector<float> values(2*names.size(), -1);
    reversed.read_into(DenseBuffer::from(values.data(), {2, names.size()}));
    for (std::size_t j = 0; j < names.size(); ++j)
        require(values[j] == float(127-j) && values[names.size()+j] == float(1127-j),
                "multi-batch reverse index selection differs");
    auto duplicates = d.slice(indices{2, 0, 2}, indices{127, 0, 127}).prepare_x();
    float repeated[9]{};
    duplicates.read_into(DenseBuffer::from(repeated, {3, 3}));
    const float expected[]{1127, 1000, 1127, 127, 0, 127, 1127, 1000, 1127};
    require(std::equal(repeated, repeated+9, expected), "duplicate index selection differs");
    auto empty = d.slice(indices{0, 2}, range{0, 0, 1}).prepare_x();
    require(empty.requirements().shape == std::vector<std::size_t>({2, 0}), "empty variable selection differs");
    d.close();

    BackedOptions writable; writable.mode = AccessMode::read_write;
    auto second = open_h5ad((dir / "second.h5ad").string(), writable);
    names[100] = "changed-late-in-index";
    auto edit = second.edit_structure();
    edit.set_var(Table::with_index(names)); edit.apply(); second.close();
    auto changed = open_h5ads(manifest.string(), options);
    auto valid = changed.slice(indices{0, 2}, indices{0, 1}).prepare_x();
    float small[4]{}; valid.read_into(DenseBuffer::from(small, {2, 2}));
    require(small[0] == 0 && small[1] == 1 && small[2] == 1000 && small[3] == 1001,
            "unaffected index selection failed");
    auto invalid = changed.slice(indices{0, 2}, all);
    std::fill(values.begin(), values.end(), -1);
    expect_error(ErrorCode::shape_mismatch, [&] { invalid.read_x_into(DenseBuffer::from(values.data(), {2, names.size()})); });
    require(std::all_of(values.begin(), values.end(), [](float x) { return x == -1; }),
            "late multi-batch index mismatch modified output");
}

void check_streamed_observation_indexes(const fs::path& root) {
    auto dir = root / "streamed-observation-indexes";
    fs::create_directories(dir);
    std::vector<std::string> last_names;
    for (std::size_t k = 0; k < 3; ++k) {
        std::vector<std::string> names;
        for (std::size_t j = 0; j < 48; ++j)
            names.push_back("observation-" + std::to_string(k) + "-" + std::to_string(j) + std::string(75, 'x'));
        auto a = AnnData::empty(names.size(), 1);
        a.set_obs(Table::with_index(names));
        a.set_var(Table::with_index({"variable"}));
        auto x = Array::empty<float>({names.size(), 1});
        for (std::size_t j = 0; j < names.size(); ++j) x.edit().dense<float>().data()[j] = float(k * 48 + j);
        a.set_x(std::move(x));
        write_h5ad((dir / (std::to_string(k) + ".h5ad")).string(), a);
        if (k == 2) last_names = std::move(names);
    }
    auto manifest = dir / "data.h5ads";
    auto created = create_h5ads(manifest.string(), {{"a", "0.h5ad"}, {"b", "1.h5ad"}, {"c", "2.h5ad"}});
    created.close();
    DatasetOptions options; options.backed.scratch_bytes = 4096; options.check_axis_names = true;
    auto data = open_h5ads(manifest.string(), options);
    std::vector<std::ptrdiff_t> positions;
    for (std::ptrdiff_t j = 47; j >= 0; --j) {
        positions.push_back(96 + j); positions.push_back(j); positions.push_back(48 + j);
        if (j % 7 == 0) positions.push_back(96 + j);
    }
    auto view = data.slice(indices(positions), all);
    std::vector<float> output(positions.size(), -1);
    view.read_x_into(DenseBuffer::from(output.data(), {positions.size(), 1}));
    for (std::size_t j = 0; j < positions.size(); ++j)
        require(output[j] == float(positions[j]), "streamed mixed observation selection differs");
    data.close();

    BackedOptions writable; writable.mode = AccessMode::read_write;
    auto last = open_h5ad((dir / "2.h5ad").string(), writable);
    last_names[0] = "changed-observation";
    auto edit = last.edit_structure(); edit.set_obs(Table::with_index(last_names)); edit.apply(); last.close();
    auto changed = open_h5ads(manifest.string(), options);
    auto invalid = changed.slice(indices(positions), all);
    std::fill(output.begin(), output.end(), -1);
    expect_error(ErrorCode::shape_mismatch, [&] { invalid.read_x_into(DenseBuffer::from(output.data(), {positions.size(), 1})); });
    require(std::all_of(output.begin(), output.end(), [](float x) { return x == -1; }),
            "late streamed observation mismatch modified output");
}

void check_many_components(const fs::path& root) {
    std::vector<DatasetComponent> components;
    for (std::size_t i = 0; i < 256; ++i)
        components.push_back({"sample-" + std::to_string(i), "dense/first.h5ad"});
    auto data = create_h5ads((root / "many.h5ads").string(), std::move(components));
    auto empty = data.slice(range{0, 0, 1}, all).prepare_x();
    require(empty.requirements().shape == std::vector<std::size_t>({0, 3}), "empty many-component selection differs");
    auto tiny = data.slice(indices{510, 0, 510}, indices{2, 0}).prepare_x();
    float values[6]{};
    tiny.read_into(DenseBuffer::from(values, {3, 2}));
    const float expected[]{2, 1, 2, 1, 2, 1};
    require(std::equal(values, values + 6, expected), "tiny many-component selection differs");
}
void check_batched_then_gathered(const fs::path& root) {
    auto tail = AnnData::empty(2048, 3);
    tail.set_var(Table::with_index({"peak-z", "peak-a", "gene-m"}));
    tail.set_x(Array::zeros<float>({2048, 3}));
    write_h5ad((root / "tail.h5ad").string(), tail);
    std::vector<DatasetComponent> components;
    for (std::size_t i = 0; i < 248; ++i)
        components.push_back({"head-" + std::to_string(i), "dense/first.h5ad"});
    components.push_back({"tail", "tail.h5ad"});
    DatasetOptions options; options.backed.scratch_bytes = 4 * 65536; options.check_axis_names = true;
    auto data = create_h5ads((root / "mixed.h5ads").string(), std::move(components), "sample", options);
    std::vector<std::ptrdiff_t> rows;
    for (std::ptrdiff_t i = 0; i < 248; ++i) rows.push_back(i * 2);
    for (std::ptrdiff_t i = 0; i < 2048; ++i) rows.push_back(496 + i);
    auto prepared = data.slice(indices(std::move(rows)), all).prepare_x();
    require(prepared.requirements().shape == std::vector<std::size_t>({2296, 3}), "mixed batch/gather shape differs");
    std::vector<float> values(2296 * 3, -1);
    prepared.read_into(DenseBuffer::from(values.data(), {2296, 3}));
    for (std::size_t i = 0; i < 248; ++i)
        require(values[3*i] == 1 && values[3*i + 1] == 0 && values[3*i + 2] == 2, "mixed component values differ");
    require(std::all_of(values.begin() + 248*3, values.end(), [](float x) { return x == 0; }),
            "mixed gathered values differ");
}
void check_reference(const fs::path& input, const fs::path& output) {
    auto d = open_h5ads(input.string());
    require(d.n_obs() == 4 && d.n_vars() == 3, "reference shape differs");
    require(d.view().components()[0].key == "first", "reference key order differs");
    auto selected = d.slice(indices{3, 0, 2, 3}, indices{2, 0, 2});
    auto r = selected.requirements();
    require(r.layout == Layout::csr && r.dtype == DType::float32, "reference X layout differs");
    auto result = selected.materialize();
    require(result.n_obs() == 4 && result.has_x(), "reference materialization differs");
    auto values = unpack(result.x());
    for (std::size_t i = 0; i < values.size(); ++i) require(values[i] == expected[i], "reference selected values differ");
    d.copy_manifest(output.string());
}
void create_reference(const fs::path& input, const fs::path& output) {
    auto parent = fs::absolute(output).parent_path();
    auto first = fs::relative(fs::absolute(input / "first.h5ad"), parent);
    auto second = fs::relative(fs::absolute(input / "second.h5ad"), parent);
    auto d = create_h5ads(output.string(), {{"first", first.string()}, {"second", second.string()}});
    require(d.n_obs() == 4 && d.n_vars() == 3, "created reference shape differs");
    d.close();
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--create") {
            create_reference(argv[2], argv[3]);
        } else if (argc == 4 && std::string(argv[1]) == "--interop") {
            check_reference(argv[2], argv[3]);
        } else {
            if (argc != 2) throw std::runtime_error("expected a scratch directory");
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            auto root = fs::path(argv[1]) / std::to_string(stamp);
            fs::create_directories(root);
            check_collection(root, Layout::dense);
            check_collection(root, Layout::csr);
            check_collection(root, Layout::csc);
            check_many_components(root);
            check_multi_batch_indexes(root);
            check_streamed_observation_indexes(root);
            check_batched_then_gathered(root);
            check_create_requires_common_var_names(root);
            check_narrow_widths(root, Layout::csr);
            check_narrow_widths(root, Layout::csc);
            check_deferred_validation(root, Layout::dense);
            check_deferred_validation(root, Layout::csr);
            check_deferred_validation(root, Layout::csc);
        }
        std::cout << "AnnDataSet checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
