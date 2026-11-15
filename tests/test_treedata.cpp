#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <utility>
#include <vector>

import anndatapp.treedata;
using namespace anndatapp;

static Tree tree() {
    auto t = Tree::from_edges({"root", "a", "inner", "b", "c"}, {0, 1, 0, 2, 2, 3, 2, 4});
    auto a = Element::mapping(); a.set("missing_is_not_null", Element(nullptr));
    auto weights = Array::empty<std::int32_t>({3});
    auto w = weights.edit().dense<std::int32_t>(); w.data()[0] = 4; w.data()[1] = 5; w.data()[2] = 6;
    a.set("weights", Element(std::move(weights)));
    t.set_node_attributes(0, std::move(a));
    auto e = Element::mapping(); e.set("length", Element(1.25));
    t.set_edge_attributes(1, std::move(e));
    return t;
}

static AnnData data(std::vector<std::string> obs) {
    auto d = AnnData::empty(obs.size(), 0);
    d.set_obs(Table::with_index(std::move(obs)));
    return d;
}

int main(int argc, char** argv) {
    if (argc == 3) {
        const std::string_view mode(argv[1]);
        if (mode == "--reject-embedded-budget-h5" || mode == "--reject-embedded-budget-zarr" ||
            mode == "--reject-embedded-invalid-h5" || mode == "--reject-embedded-invalid-zarr") {
            const bool hdf5=mode.ends_with("h5"), invalid=mode.find("invalid")!=std::string_view::npos;
            std::vector<float> values(1000,1.f);
            std::vector<std::int32_t> columns(1000,0);
            std::int32_t offsets[2]={0,1000};
            auto d=AnnData::empty(1,1);
            d.set_x(Array::borrow_sparse(Layout::csr,1,1,values.size(),values.data(),columns.data(),offsets));
            TreeData source(std::move(d));
            auto selected=source.slice(indices{0,0},all);
            if (invalid) columns[0]=1; // Caller mutation invalidates a previously checked borrowed array.
            const auto budget=invalid?std::size_t(1<<20):hdf5?std::size_t(1024):std::size_t(2048);
            try {
                if (hdf5) write_h5td(argv[2],selected,TreeWriteOptions{.scratch_bytes=budget});
                else write_treedata_zarr(argv[2],selected,TreeWriteOptions{.scratch_bytes=budget});
            } catch (const Error& e) {
                return e.code()==(invalid?ErrorCode::invalid_sparse_structure:ErrorCode::invalid_argument)?0:1;
            }
            return 1;
        }
        if (mode == "--scalar-query-h5" || mode == "--scalar-query-zarr") {
            auto backed=mode.ends_with("h5")?open_h5td(argv[2]):open_treedata_zarr(argv[2]);
            auto names=backed.obs_tree_names();
            assert(names==std::vector<std::string>({"alpha","beta"}));
            auto alpha=backed.obs_tree("alpha");
            assert(alpha.n_nodes()==4 && alpha.node_attributes(0).at("depth").signed_integer()==0);
            bool rejected=false;
            try { (void)backed.obs_tree("beta"); } catch (const Error&) { rejected=true; }
            assert(rejected);
            rejected=false;
            try { (void)backed.read(); } catch (const Error&) { rejected=true; }
            assert(rejected);
            return 0;
        }
        if (mode == "--expect-unknown-zarr") {
            auto backed=open_treedata_zarr(argv[2]);
            for (int access=0;access<3;++access) {
                bool rejected=false;
                try {
                    if (access==0) (void)backed.obs_tree("alpha");
                    else if (access==1) (void)backed.read(indices{0},indices{0});
                    else (void)backed.read();
                } catch (const Error& e) {
                    rejected=e.code()==ErrorCode::unsupported_encoding && e.path()=="/obst/alpha/unknown";
                }
                assert(rejected);
            }
            return 0;
        }
        if (mode == "--expect-unsupported-integer-h5" || mode == "--expect-malformed-h5") {
            try { (void)read_h5td(argv[2]); }
            catch (const Error& e) {
                auto expected = mode == "--expect-malformed-h5" ? ErrorCode::unsupported_encoding : ErrorCode::unsupported_dtype;
                return e.code() == expected && e.path() == "/obst" ? 0 : 1;
            }
            return 1;
        }
        if (mode == "--expect-malformed-zarr" || mode == "--expect-unsupported-integer-zarr") {
            try { (void)read_treedata_zarr(argv[2]); }
            catch (const Error& e) {
                auto expected = mode == "--expect-malformed-zarr" ? ErrorCode::unsupported_encoding : ErrorCode::unsupported_dtype;
                return e.code() == expected && e.path() == "/obst/alpha/node_attrs/payload" ? 0 : 1;
            }
            return 1;
        }
        if (mode == "--select-disconnected" || mode == "--select-disconnected-h5") {
            auto backed = mode == "--select-disconnected-h5" ? open_h5td(argv[2]) : open_treedata_zarr(argv[2]);
            auto selected = backed.read(indices{0, 2}, indices{0});
            assert(selected.alignment() == TreeAlignment::subset);
            auto alpha = selected.obs_tree("alpha");
            assert(alpha.n_nodes() == 4 && alpha.n_edges() == 3);
            assert(alpha.node_ids().string(3) == "alpha-b");
            assert(alpha.node_attributes(3).at("depth").signed_integer() == 2);
            assert(alpha.edge_attributes(2).at("length").floating() == 1.5);
            assert(selected.var_tree("genes").n_nodes() == 4);
            return 0;
        }
        if (mode == "--select-only") {
            auto backed = open_treedata_zarr(argv[2]);
            auto selected = backed.read(indices{0}, indices{0});
            assert(selected.data().n_obs() == 1 && selected.data().n_vars() == 1);
            auto alpha = selected.obs_tree("alpha");
            assert(alpha.n_nodes() == 3 && alpha.n_edges() == 2);
            assert(alpha.node_ids().string(0) == "alpha-root");
            assert(alpha.node_ids().string(1) == "alpha-inner");
            assert(alpha.node_ids().string(2) == "alpha-a");
            auto root = alpha.node_attributes(0), inner = alpha.node_attributes(1), leaf = alpha.node_attributes(2);
            assert(root.at("depth").signed_integer() == 0 && root.at("active").boolean());
            assert(root.at("payload").at("kind").string() == "root");
            assert(root.at("nullable").kind() == ElementKind::null);
            assert(inner.at("depth").signed_integer() == 1 && inner.at("payload").at(0).string() == "branch");
            assert(inner.at("nullable").string() == "present");
            assert(leaf.at("depth").signed_integer() == 2 && leaf.at("payload").string() == "leaf-a");
            assert(!leaf.contains("nullable"));
            assert(alpha.edge_attributes(0).at("length").floating() == 0.5);
            assert(alpha.edge_attributes(0).at("note").string() == "trunk");
            assert(alpha.edge_attributes(1).at("length").floating() == 1.25);
            assert(alpha.edge_attributes(1).at("note").kind() == ElementKind::null);
            assert(selected.obs_tree("beta").n_nodes() == 0);
            return 0;
        }
        if (mode == "--reject-zarr-name" || mode == "--reject-zarr-attr") {
            TreeData value(data({"a", "b", "c"}), TreeAlignment::leaves, true, std::nullopt);
            auto source = tree();
            if (mode == "--reject-zarr-attr") {
                auto attrs = Element::mapping(); attrs.set("bad/key", Element(1));
                source.set_node_attributes(0, std::move(attrs));
            }
            value.set_obs_tree(mode == "--reject-zarr-name" ? "bad/name" : "good", std::move(source));
            try { write_treedata_zarr(argv[2], value); }
            catch (const Error& e) { return e.code() == ErrorCode::invalid_argument ? 0 : 1; }
            return 1;
        }
        return 2;
    }
    if (argc == 4) {
        const std::string_view mode(argv[1]);
        if (mode == "--budget-zarr" || mode == "--reject-large-h5" || mode == "--reject-large-zarr") {
            auto full = mode == "--reject-large-h5" ? read_h5td(argv[2]) : read_treedata_zarr(argv[2]);
            try {
                if (mode == "--reject-large-h5") write_h5td(argv[3], full, TreeWriteOptions{.scratch_bytes = 1024 * 1024});
                else write_treedata_zarr(argv[3], full, TreeWriteOptions{.scratch_bytes = 1024 * 1024});
            } catch (const Error& e) { return mode != "--budget-zarr" && e.code() == ErrorCode::invalid_argument ? 0 : 1; }
            if (mode != "--budget-zarr") return 1;
            auto result = read_treedata_zarr(argv[3]);
            result.validate();
            return 0;
        }
        if (mode == "--reject-budget-h5" || mode == "--reject-budget-zarr") {
            auto full = mode == "--reject-budget-h5" ? read_h5td(argv[2]) : read_treedata_zarr(argv[2]);
            try {
                if (mode == "--reject-budget-h5") write_h5td(argv[3], full, TreeWriteOptions{.scratch_bytes = 1});
                else write_treedata_zarr(argv[3], full, TreeWriteOptions{.scratch_bytes = 1});
            } catch (const Error& e) { return e.code() == ErrorCode::invalid_argument ? 0 : 1; }
            return 1;
        }
        if (mode == "--roundtrip-h5") {
            auto backed = open_h5td(argv[2]);
            auto selected = backed.read(indices{0}, indices{0});
            assert(selected.data().n_obs() == 1 && selected.data().n_vars() == 1);
            auto full = backed.read(); full.validate(); backed.close();
            write_h5td(argv[3], full);
            return 0;
        }
        if (mode == "--roundtrip-zarr" || mode == "--legacy-zarr") {
            auto backed = open_treedata_zarr(argv[2]);
            auto selected = backed.read(indices{0}, indices{0});
            assert(selected.data().n_obs() == 1 && selected.data().n_vars() == 1);
            auto full = backed.read(); full.validate(); backed.close();
            write_treedata_zarr(argv[3], full);
            return 0;
        }
        return 2;
    }
    assert(argc == 1);
    TreeData t(data({"a", "b", "c"}));
    assert(t.has_label() && t.label() == "tree");
    t.set_label(std::string{});
    assert(t.has_label() && t.label().empty() && t.view().has_label());
    t.set_label(std::nullopt);
    assert(!t.has_label());
    t.set_obs_tree("lineage", tree());
    auto retained = t.obs_tree("lineage");
    assert(retained.node_ids().string(0) == "root");
    assert(retained.edges().dense<std::uint64_t>().data()[3] == 2);
    assert(retained.node_attributes(0).contains("missing_is_not_null"));
    assert(retained.node_attributes(0).at("missing_is_not_null").kind() == ElementKind::null);
    assert(!retained.node_attributes(0).contains("absent"));
    auto p = t.slice(labels{"b"});
    auto tp = p.obs_tree("lineage");
    assert(p.data().n_obs() == 1 && tp.n_nodes() == 3 && tp.n_edges() == 2);
    assert(tp.node_ids().string(0) == "root" && tp.node_ids().string(1) == "inner" && tp.node_ids().string(2) == "b");
    assert(tp.edge_attributes(0).at("length").floating() == 1.25);
    auto source_weights = retained.node_attributes(0).at("weights").array().dense<std::int32_t>();
    auto selected_weights = tp.node_attributes(0).at("weights").array().dense<std::int32_t>();
    assert(source_weights.data() == selected_weights.data());
    auto copied = p.copy();
    assert(copied.obs_tree("lineage").n_nodes() == 3);
    assert(copied.obs_tree("lineage").node_attributes(0).at("weights").array().dense<std::int32_t>().data() != source_weights.data());
    t.erase_obs_tree("lineage");
    assert(retained.n_nodes() == 5 && retained.node_ids().string(4) == "c");

    TreeData labeled(data({"a", "b", "c", "other"}), TreeAlignment::leaves, true, std::string("group"));
    labeled.set_obs_tree("z", tree());
    assert(labeled.data().obs_column("group").string(0) == "z");
    assert(!labeled.data().obs_column("group").valid(3));
    labeled.set_obs_tree("alpha", tree());
    assert(labeled.data().obs_column("group").string(0) == "alpha,z");
    auto indexed_names = labeled.view();
    assert(indexed_names.obs_tree_count() == 2 && indexed_names.var_tree_count() == 0);
    assert(indexed_names.obs_tree_name(0) == "z" && indexed_names.obs_tree_name(1) == "alpha");
    bool bad_index = false;
    try { (void)indexed_names.obs_tree_name(2); } catch (const Error&) { bad_index = true; }
    assert(bad_index);
    labeled.erase_obs_tree("z");
    assert(labeled.data().obs_column("group").string(0) == "alpha");

    TreeData nodes(data({"root", "a", "inner", "b", "c"}), TreeAlignment::nodes);
    nodes.set_obs_tree("lineage", tree());
    auto disconnected = nodes.slice(labels{"root", "b"});
    assert(disconnected.alignment() == TreeAlignment::subset);
    assert(disconnected.obs_tree("lineage").n_nodes() == 5);
    auto connected = nodes.slice(labels{"root", "inner", "b"});
    assert(connected.alignment() == TreeAlignment::nodes);
    assert(connected.obs_tree("lineage").n_nodes() == 3);
    TreeData subset(data({"b"}), TreeAlignment::subset);
    subset.set_obs_tree("lineage", tree());
    assert(subset.slice(labels{"b"}).obs_tree("lineage").n_nodes() == 5);

    bool rejected = false;
    try { (void)Tree::from_edges({"a", "b", "c"}, {0, 1, 1, 0}); }
    catch (const Error&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { TreeData bad(data({"a", "b", "c"}), TreeAlignment::leaves, false);
          bad.set_obs_tree("one", tree()); bad.set_obs_tree("two", tree()); }
    catch (const Error&) { rejected = true; }
    assert(rejected);
    auto snapshot = [] {
        TreeData local(data({"a", "b", "c"}));
        local.set_obs_tree("lineage", tree());
        return local.view();
    }();
    assert(snapshot.obs_tree("lineage").node_ids().string(0) == "root");
    assert(snapshot.obs_tree_count() == 1 && snapshot.obs_tree_name(0) == "lineage");

    // A long chain exercises ancestor traversal without quadratic rescanning.
    constexpr std::size_t n = 2000;
    std::vector<std::string> ids; ids.reserve(n);
    std::vector<std::uint64_t> edges; edges.reserve(2*(n-1));
    for (std::size_t i = 0; i < n; ++i) {
        ids.push_back(std::to_string(i));
        if (i) { edges.push_back(i-1); edges.push_back(i); }
    }
    TreeData chain(data({std::to_string(n-1)}));
    chain.set_obs_tree("chain", Tree::from_edges(std::move(ids), std::move(edges)));
    assert(chain.slice(all).obs_tree("chain").n_nodes() == n);
    return 0;
}
