#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import anndata;

using namespace anndatapp;

namespace {
constexpr std::uint16_t values[] = {0x0001, 0x8000, 0x3e00, 0x7e51, 0xc100, 0x7c00};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Array payload(std::vector<std::size_t> shape, bool reverse = false) {
    auto result = Array::empty<Float16>(std::move(shape));
    auto dst = result.edit().dense<Float16>();
    for (std::size_t i = 0; i < 6; ++i) dst.data()[i] = Float16::from_bits(values[reverse ? 5 - i : i]);
    return result;
}

AnnData matrix(bool reverse = false) {
    auto data = AnnData::empty(2, 3);
    data.set_x(payload({2, 3}, reverse));
    return data;
}

void check_payload(const ArrayView& a) {
    require(a.dtype() == DType::float16 && a.layout() == Layout::dense, "dense dtype changed");
    const auto v = a.dense<Float16>();
    for (std::size_t i = 0; i < 6; ++i) require(v.data()[i].bits == values[i], "FP16 bits changed");
}

void check_slice(const BackedArray& array) {
    auto prepared = array.select({indices{1, 0}, indices{2, 0}}).prepare_read();
    Float16 dst[4];
    prepared.read_into(DenseBuffer::from(dst, {2, 2}));
    require(dst[0].bits == values[5] && dst[1].bits == values[3] &&
            dst[2].bits == values[2] && dst[3].bits == values[0], "selected FP16 read changed bits or order");
}

void check_nested(const std::filesystem::path& root, bool zarr) {
    auto mu = MuData::empty(2, 3);
    mu.set_modality("rna", matrix(), {1, 2}, {1, 2, 3});
    const auto mu_path = (root / (zarr ? "mu.zarr" : "mu.h5mu")).string();
    if (zarr) write_mudata_zarr(mu_path, mu);
    else write_h5mu(mu_path, mu);
    auto backed_mu = zarr ? open_mudata_zarr(mu_path) : open_h5mu(mu_path);
    check_slice(backed_mu.modality("rna").x());
    auto mu_copy = backed_mu.read();
    check_payload(mu_copy.view().modality("rna").x());

    TreeData tree(matrix());
    const auto tree_path = (root / (zarr ? "tree.zarr" : "tree.h5td")).string();
    if (zarr) write_treedata_zarr(tree_path, tree);
    else write_h5td(tree_path, tree);
    auto backed_tree = zarr ? open_treedata_zarr(tree_path) : open_h5td(tree_path);
    check_slice(backed_tree.data().x());
    auto tree_copy = backed_tree.read();
    check_payload(tree_copy.data().x());
}

#if ANNDATAPP_TEST_HDF5
void check_collection(const std::filesystem::path& root) {
    write_h5ad((root / "first.h5ad").string(), matrix());
    write_h5ad((root / "second.h5ad").string(), matrix(true));
    const auto manifest = (root / "data.h5ads").string();
    auto collection = create_h5ads(manifest, {{"first", "first.h5ad"}, {"second", "second.h5ad"}});
    collection.close();
    auto reopened = open_h5ads(manifest);
    auto selected = reopened.slice(indices{3, 0, 2}, indices{2, 0});
    auto plan = selected.prepare_x();
    const auto requirements = plan.requirements();
    require(requirements.dtype == DType::float16 && requirements.layout == Layout::dense &&
            requirements.shape == std::vector<std::size_t>{3, 2}, "collection requirements differ");
    Float16 out[6];
    auto buffer = DenseBuffer::from(out + 4, {3, 2}, {-4, 2});
    plan.read_into(buffer);
    plan.read_into(buffer);
    auto packed = selected.materialize();
    const auto view = packed.x().dense<Float16>();
    const std::uint16_t expected[] = {values[0], values[2], values[2], values[0], values[3], values[5]};
    for (std::size_t i = 0; i < 6; ++i) {
        require(out[(2 - i / 2) * 2 + i % 2].bits == expected[i], "strided FP16 collection transfer differs");
        require(view.data()[i].bits == expected[i], "FP16 collection materialization differs");
    }
}
#endif

#if ANNDATAPP_TEST_ZARR
template<class F> void expect_unsupported_dtype(F&& operation) {
    try { operation(); }
    catch (const Error& e) {
        require(e.code() == ErrorCode::unsupported_dtype, "expected unsupported_dtype");
        return;
    }
    throw std::runtime_error("expected dtype rejection");
}

void check_spatial(const std::filesystem::path& root) {
    SpatialRaster image(SpatialElementKind::image, {SpatialAxis::c, SpatialAxis::y, SpatialAxis::x});
    image.add_level(payload({1, 2, 3}));
    SpatialData data;
    data.set_raster("image", std::move(image));
    SpatialRaster labels(SpatialElementKind::labels, {SpatialAxis::y, SpatialAxis::x});
    labels.add_level(Array::zeros<std::uint16_t>({2, 3}));
    data.set_raster("labels", std::move(labels));
    const auto path = (root / "spatial.zarr").string();
    write_spatialdata(path, data);
    auto backed = open_spatialdata(path);
    auto plan = backed.raster("image").select({all, range{1, 2}, indices{2, 0}}).prepare_read();
    Float16 out[2];
    plan.read_into(DenseBuffer::from(out, {1, 1, 2}));
    require(out[0].bits == values[5] && out[1].bits == values[3], "FP16 image window differs");
    auto eager = backed.read();
    check_payload(eager.view().raster("image").level(0));
    backed.close();

    SpatialRaster invalid(SpatialElementKind::labels, {SpatialAxis::y, SpatialAxis::x});
    invalid.add_level(payload({2, 3}));
    data.set_raster("labels", std::move(invalid));
    const auto bad_path = root / "invalid-labels.zarr";
    expect_unsupported_dtype([&] {
        write_spatialdata(bad_path.string(), data, SpatialWriteOptions{.validation = Validation::none});
    });
    require(!std::filesystem::exists(bad_path), "FP16 labels created a partial store");

    {
        BackedOptions options;
        options.mode = AccessMode::read_write;
        auto store = open_zarr_store(path, options);
        auto bad = payload({2, 3});
        store.write_array("/labels/labels/s0", bad.view());
    }
    expect_unsupported_dtype([&] { (void)open_spatialdata(path); });
}
#endif
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected an output directory");
        const std::filesystem::path root(argv[1]);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
#if ANNDATAPP_TEST_HDF5
        check_nested(root, false);
        check_collection(root);
#endif
#if ANNDATAPP_TEST_ZARR
        check_nested(root, true);
        check_spatial(root);
#endif
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
