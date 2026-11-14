#include <cstddef>
#include <cstdint>
#include <iostream>
#include <utility>

import anndata;

int main(int argc, char** argv) {
    using namespace anndatapp;

    auto expression = AnnData::empty(2, 1);
    expression.set_obs(Table::with_index({"cell-a", "cell-b"}));
    expression.set_var(Table::with_index({"gene"}));
    auto counts = Array::empty<float>({2, 1});
    counts.edit().dense<float>().data()[0] = 2;
    counts.edit().dense<float>().data()[1] = 5;
    expression.set_x(std::move(counts));

    auto multi = MuData::empty(2, 1);
    multi.annotations().set_obs(Table::with_index({"cell-a", "cell-b"}));
    multi.annotations().set_var(Table::with_index({"gene"}));
    multi.set_modality("rna", expression.copy(), {1, 2}, {1});
    auto repeated = multi.slice(indices{1, 0, 1}, all);
    std::cout << "MuData selected observations: " << repeated.n_obs() << '\n';

    TreeData lineage(std::move(expression));
    lineage.set_obs_tree("lineage", Tree::from_edges(
        {"ancestor", "cell-a", "cell-b"}, {0, 1, 0, 2}));
    auto branch = lineage.slice(indices{1}, all);
    std::cout << "TreeData selected nodes: " << branch.obs_tree("lineage").n_nodes() << '\n';

    SpatialData spatial;
    SpatialRaster mask(SpatialElementKind::labels, {SpatialAxis::y, SpatialAxis::x});
    auto pixels = Array::empty<std::int32_t>({2, 2});
    auto p = pixels.edit().dense<std::int32_t>();
    for (std::size_t i = 0; i < 4; ++i) p.data()[i] = std::int32_t(i % 2);
    mask.add_level(std::move(pixels));
    mask.set_transform("slide", SpatialTransform::translation(
        {SpatialAxis::y, SpatialAxis::x}, {10, 20}));
    spatial.set_raster("cells", std::move(mask));
    spatial.validate();

    // Inspection keeps components backed. Pass an existing .h5ads manifest.
    if (argc == 2) {
        auto collection = open_h5ads(argv[1]);
        std::cout << "AnnDataSet: " << collection.n_components() << " components, "
                  << collection.n_obs() << " observations, " << collection.n_vars() << " variables\n";
    }
    return 0;
}
