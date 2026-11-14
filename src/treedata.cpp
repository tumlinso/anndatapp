module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module anndatapp.treedata;

namespace anndatapp {
namespace detail {
struct TreeImpl {
    StringArray nodes;
    Array edges;
    std::vector<ElementView> node_attrs, edge_attrs;
    TreeImpl(StringArray n, Array e) : nodes(std::move(n)), edges(std::move(e)) {
        node_attrs.reserve(nodes.size());
        edge_attrs.reserve(edges.rank() ? edges.extent(0) : 0);
        for (std::size_t i = 0; i < nodes.size(); ++i) { auto value = Element::mapping(); node_attrs.push_back(value.view()); }
        for (std::size_t i = 0; i < (edges.rank() ? edges.extent(0) : 0); ++i) {
            auto value = Element::mapping(); edge_attrs.push_back(value.view());
        }
    }
};
struct TreeAccess {
    static void node(Tree& tree, std::size_t i, ElementView value) { tree.impl_->node_attrs[i] = std::move(value); }
    static void edge(Tree& tree, std::size_t i, ElementView value) { tree.impl_->edge_attrs[i] = std::move(value); }
};
struct TreeDataImpl {
    AnnData data;
    TreeAlignment alignment;
    bool overlap;
    std::optional<std::string> label;
    std::vector<std::pair<std::string, Tree>> obst, vart;
    TreeDataImpl(AnnData d, TreeAlignment a, bool o, std::optional<std::string> l)
        : data(std::move(d)), alignment(a), overlap(o), label(std::move(l)) {}
};
struct TreeDataViewImpl {
    AnnDataView data;
    TreeAlignment alignment = TreeAlignment::leaves;
    bool overlap = true;
    std::optional<std::string> label;
    std::vector<std::pair<std::string, TreeView>> obst, vart;
};
}
namespace {
[[noreturn]] void fail(ErrorCode code, std::string_view path, std::string message) {
    throw Error(code, std::string(path), std::move(message));
}
void require_tree(const std::shared_ptr<detail::TreeImpl>& p) {
    if (!p) fail(ErrorCode::invalid_argument, "tree", "empty tree");
}
void require_view(const std::shared_ptr<detail::TreeDataViewImpl>& p) {
    if (!p) fail(ErrorCode::invalid_argument, "treedata", "empty TreeData view");
}
void mapping(ElementView e, std::string_view path) {
    if (e.kind() != ElementKind::mapping) fail(ErrorCode::invalid_argument, path, "attributes must be a mapping");
}
template<class Trees> const auto& find_tree(const Trees& trees, std::string_view key, std::string_view path) {
    auto it = std::find_if(trees.begin(), trees.end(), [&](const auto& p) { return p.first == key; });
    if (it == trees.end()) fail(ErrorCode::out_of_bounds, path, "tree key not found: " + std::string(key));
    return it->second;
}
template<class Trees> std::vector<std::string> names(const Trees& trees) {
    std::vector<std::string> out;
    out.reserve(trees.size());
    for (const auto& [key, tree] : trees) { (void)tree; out.push_back(key); }
    return out;
}
std::vector<std::uint8_t> leaves(TreeView tree) {
    std::vector<std::uint8_t> result(tree.n_nodes(), 1);
    const auto edge = tree.edges().dense<std::uint64_t>();
    for (std::size_t i = 0; i < tree.n_edges(); ++i) result[edge.data()[2*i]] = 0;
    return result;
}
void validate_axis(const std::vector<std::pair<std::string, Tree>>& trees, TableView axis,
                   TreeAlignment alignment, bool allow_overlap, std::string_view path) {
    std::unordered_set<std::string> axis_names;
    for (std::size_t i = 0; i < axis.size(); ++i) axis_names.emplace(axis.name(i));
    std::unordered_set<std::string> used, keys;
    for (const auto& [key, tree] : trees) {
        if (!keys.insert(key).second)
            fail(ErrorCode::invalid_argument, path, "tree keys must be unique");
        tree.validate();
        auto v = tree.view();
        auto ids = v.node_ids();
        auto terminal = leaves(v);
        for (std::size_t i = 0; i < v.n_nodes(); ++i) {
            if (alignment == TreeAlignment::leaves && !terminal[i]) continue;
            auto id = std::string(ids.string(i));
            if (alignment != TreeAlignment::subset && !axis_names.contains(id))
                fail(ErrorCode::shape_mismatch, path, "aligned tree node is absent from the annotated axis: " + id);
            if (!allow_overlap && !used.insert(id).second)
                fail(ErrorCode::invalid_argument, path, "overlapping tree nodes are not allowed: " + id);
        }
    }
}
void update_tree_labels(detail::TreeDataImpl& data, bool obs) {
    if (!data.label) return;
    auto axis = obs ? data.data.obs() : data.data.var();
    const auto& trees = obs ? data.obst : data.vart;
    std::unordered_map<std::string, std::vector<std::string>> assigned;
    for (const auto& [key, tree] : trees) {
        auto v = tree.view();
        auto ids = v.node_ids();
        auto terminal = leaves(v);
        for (std::size_t i = 0; i < v.n_nodes(); ++i)
            if (data.alignment != TreeAlignment::leaves || terminal[i])
                assigned[std::string(ids.string(i))].push_back(key);
    }
    std::vector<std::string> values(axis.size());
    std::vector<std::uint8_t> valid(axis.size(), 0);
    for (std::size_t i = 0; i < axis.size(); ++i) {
        auto it = assigned.find(std::string(axis.name(i)));
        if (it == assigned.end()) continue;
        auto& keys = it->second;
        std::sort(keys.begin(), keys.end());
        bool first = true;
        for (const auto& key : keys) {
            if (!first) values[i] += ',';
            values[i] += key;
            first = false;
        }
        valid[i] = 1;
    }
    auto column = Column::nullable_strings(std::move(values), std::move(valid), StringMissingValue::NaN);
    if (obs) data.data.set_obs_column(*data.label, std::move(column));
    else data.data.set_var_column(*data.label, std::move(column));
}
template<class Trees> void erase_tree(Trees& trees, std::string_view key, std::string_view path) {
    auto it = std::find_if(trees.begin(), trees.end(), [&](const auto& p) { return p.first == key; });
    if (it == trees.end()) fail(ErrorCode::out_of_bounds, path, "tree key not found: " + std::string(key));
    trees.erase(it);
}
bool connected(TreeView tree, const std::vector<std::uint8_t>& keep) {
    std::size_t n = 0, e = 0;
    for (auto x : keep) n += x != 0;
    if (!n) return true;
    auto edges = tree.edges().dense<std::uint64_t>();
    for (std::size_t i = 0; i < tree.n_edges(); ++i)
        e += keep[edges.data()[2*i]] && keep[edges.data()[2*i+1]];
    return e == n - 1;
}
std::vector<std::uint8_t> selected_nodes(TreeView tree, TableView axis, TreeAlignment alignment) {
    std::unordered_set<std::string> selected;
    for (std::size_t i = 0; i < axis.size(); ++i) selected.emplace(axis.name(i));
    auto ids = tree.node_ids();
    auto terminal = leaves(tree);
    std::vector<std::uint8_t> keep(tree.n_nodes(), 0);
    for (std::size_t i = 0; i < tree.n_nodes(); ++i)
        if ((alignment != TreeAlignment::leaves || terminal[i]) && selected.contains(std::string(ids.string(i)))) keep[i] = 1;
    if (alignment == TreeAlignment::leaves) {
        auto edges = tree.edges().dense<std::uint64_t>();
        std::vector<std::size_t> offsets(tree.n_nodes()+1, 0), parents(tree.n_edges()), queue;
        for (std::size_t i = 0; i < tree.n_edges(); ++i) ++offsets[edges.data()[2*i+1]+1];
        for (std::size_t i = 1; i < offsets.size(); ++i) offsets[i] += offsets[i-1];
        auto next = offsets;
        for (std::size_t i = 0; i < tree.n_edges(); ++i)
            parents[next[edges.data()[2*i+1]]++] = edges.data()[2*i];
        queue.reserve(tree.n_nodes());
        for (std::size_t i = 0; i < keep.size(); ++i) if (keep[i]) queue.push_back(i);
        for (std::size_t i = 0; i < queue.size(); ++i) {
            auto node = queue[i];
            for (std::size_t j = offsets[node]; j < offsets[node+1]; ++j)
                if (!keep[parents[j]]) { keep[parents[j]] = 1; queue.push_back(parents[j]); }
        }
    }
    return keep;
}
Tree project_tree(TreeView source, const std::vector<std::uint8_t>& keep) {
    std::vector<std::string> ids;
    std::vector<std::uint64_t> remap(source.n_nodes(), 0), pairs;
    auto source_ids = source.node_ids();
    for (std::size_t i = 0; i < source.n_nodes(); ++i) if (keep[i]) {
        remap[i] = ids.size(); ids.emplace_back(source_ids.string(i));
    }
    auto e = source.edges().dense<std::uint64_t>();
    for (std::size_t i = 0; i < source.n_edges(); ++i) {
        auto src = e.data()[2*i], dst = e.data()[2*i+1];
        if (keep[src] && keep[dst]) { pairs.push_back(remap[src]); pairs.push_back(remap[dst]); }
    }
    auto result = Tree::from_edges(std::move(ids), std::move(pairs));
    std::size_t j = 0;
    for (std::size_t i = 0; i < source.n_nodes(); ++i) if (keep[i])
        detail::TreeAccess::node(result, j++, source.node_attributes(i));
    j = 0;
    for (std::size_t i = 0; i < source.n_edges(); ++i)
        if (keep[e.data()[2*i]] && keep[e.data()[2*i+1]])
            detail::TreeAccess::edge(result, j++, source.edge_attributes(i));
    return result;
}
}

namespace detail {
bool tree_subset_connected(TreeView tree, TableView axis) {
    return connected(tree, selected_nodes(tree, axis, TreeAlignment::nodes));
}
Tree tree_select(TreeView tree, TableView axis, TreeAlignment alignment) {
    return alignment == TreeAlignment::subset ? tree.copy() : project_tree(tree, selected_nodes(tree, axis, alignment));
}
}

TreeView::TreeView() = default;
TreeView::TreeView(std::shared_ptr<detail::TreeImpl> p) : impl_(std::move(p)) {}
std::size_t TreeView::n_nodes() const { require_tree(impl_); return impl_->nodes.size(); }
std::size_t TreeView::n_edges() const { require_tree(impl_); return impl_->edges.extent(0); }
StringArrayView TreeView::node_ids() const { require_tree(impl_); return impl_->nodes.view(); }
ArrayView TreeView::edges() const { require_tree(impl_); return impl_->edges.view(); }
ElementView TreeView::node_attributes(std::size_t i) const {
    require_tree(impl_);
    if (i >= impl_->node_attrs.size()) fail(ErrorCode::out_of_bounds, "tree/nodes", "node index out of bounds");
    return impl_->node_attrs[i];
}
ElementView TreeView::edge_attributes(std::size_t i) const {
    require_tree(impl_);
    if (i >= impl_->edge_attrs.size()) fail(ErrorCode::out_of_bounds, "tree/edges", "edge index out of bounds");
    return impl_->edge_attrs[i];
}
Tree TreeView::copy() const {
    require_tree(impl_);
    auto result = Tree::from_arrays(impl_->nodes.copy(), impl_->edges.copy());
    for (std::size_t i = 0; i < n_nodes(); ++i) result.set_node_attributes(i, node_attributes(i).copy());
    for (std::size_t i = 0; i < n_edges(); ++i) result.set_edge_attributes(i, edge_attributes(i).copy());
    return result;
}
void TreeView::validate() const {
    require_tree(impl_);
    impl_->nodes.validate();
    impl_->edges.validate();
    if (impl_->nodes.rank() != 1 || impl_->edges.rank() != 2 || impl_->edges.extent(1) != 2 ||
        impl_->edges.dtype() != DType::uint64 || impl_->edges.layout() != Layout::dense)
        fail(ErrorCode::shape_mismatch, "tree", "nodes must be a vector and edges a uint64 [n,2] array");
    auto ids = impl_->nodes.view();
    std::unordered_set<std::string> unique;
    for (std::size_t i = 0; i < ids.size(); ++i)
        if (!unique.emplace(ids.string(i)).second) fail(ErrorCode::invalid_argument, "tree/nodes", "duplicate node identifier");
    auto e = impl_->edges.view().dense<std::uint64_t>();
    if (!e.contiguous()) fail(ErrorCode::non_contiguous, "tree/edges", "edge array must be contiguous");
    auto n = ids.size(), m = impl_->edges.extent(0);
    if ((n == 0 && m != 0) || (n > 0 && m != n-1))
        fail(ErrorCode::invalid_argument, "tree/edges", "edges must form one tree");
    std::vector<std::size_t> parent(n);
    for (std::size_t i = 0; i < n; ++i) parent[i] = i;
    auto root = [&](std::size_t i) { while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; } return i; };
    for (std::size_t i = 0; i < m; ++i) {
        auto a = e.data()[2*i], b = e.data()[2*i+1];
        if (a >= n || b >= n) fail(ErrorCode::out_of_bounds, "tree/edges", "edge endpoint is outside node array");
        auto ra = root(a), rb = root(b);
        if (ra == rb) fail(ErrorCode::invalid_argument, "tree/edges", "edge cycle detected");
        parent[ra] = rb;
    }
    if (impl_->node_attrs.size() != n || impl_->edge_attrs.size() != m)
        fail(ErrorCode::shape_mismatch, "tree", "attribute counts disagree with topology");
    for (const auto& a : impl_->node_attrs) { mapping(a, "tree/node_attrs"); a.validate(); }
    for (const auto& a : impl_->edge_attrs) { mapping(a, "tree/edge_attrs"); a.validate(); }
}
Tree::Tree() : Tree(std::make_shared<detail::TreeImpl>(StringArray::from_strings({0}, {}), Array::empty<std::uint64_t>({0,2}))) {}
Tree::Tree(std::shared_ptr<detail::TreeImpl> p) : impl_(std::move(p)) {}
Tree::~Tree() = default;
Tree::Tree(Tree&&) noexcept = default;
Tree& Tree::operator=(Tree&&) noexcept = default;
Tree Tree::from_edges(std::vector<std::string> ids, std::vector<std::uint64_t> pairs) {
    if (pairs.size() % 2) fail(ErrorCode::shape_mismatch, "tree/edges", "edge endpoint count must be even");
    const auto n = ids.size();
    auto nodes = StringArray::from_strings({n}, std::move(ids));
    auto edges = Array::empty<std::uint64_t>({pairs.size()/2, 2});
    auto e = edges.edit().dense<std::uint64_t>();
    for (std::size_t i = 0; i < pairs.size(); ++i) e.data()[i] = pairs[i];
    return from_arrays(std::move(nodes), std::move(edges));
}
Tree Tree::from_arrays(StringArray nodes, Array edges) {
    auto result = Tree(std::make_shared<detail::TreeImpl>(std::move(nodes), std::move(edges)));
    result.validate();
    return result;
}
TreeView Tree::view() const { require_tree(impl_); return TreeView(impl_); }
Tree Tree::copy() const { return view().copy(); }
void Tree::set_node_attributes(std::size_t i, Element a) {
    require_tree(impl_);
    if (i >= impl_->node_attrs.size()) fail(ErrorCode::out_of_bounds, "tree/nodes", "node index out of bounds");
    mapping(a.view(), "tree/node_attrs"); a.validate(); impl_->node_attrs[i] = a.view();
}
void Tree::set_edge_attributes(std::size_t i, Element a) {
    require_tree(impl_);
    if (i >= impl_->edge_attrs.size()) fail(ErrorCode::out_of_bounds, "tree/edges", "edge index out of bounds");
    mapping(a.view(), "tree/edge_attrs"); a.validate(); impl_->edge_attrs[i] = a.view();
}
void Tree::validate() const { view().validate(); }

TreeData::TreeData(AnnData data, TreeAlignment a, bool overlap, std::optional<std::string> label)
    : impl_(std::make_unique<detail::TreeDataImpl>(std::move(data), a, overlap, std::move(label))) { validate(); }
TreeData::~TreeData() = default;
TreeData::TreeData(TreeData&&) noexcept = default;
TreeData& TreeData::operator=(TreeData&&) noexcept = default;
AnnData& TreeData::data() { return impl_->data; }
const AnnData& TreeData::data() const { return impl_->data; }
TreeAlignment TreeData::alignment() const { return impl_->alignment; }
bool TreeData::allow_overlap() const { return impl_->overlap; }
bool TreeData::has_label() const { return impl_->label.has_value(); }
std::string_view TreeData::label() const { return impl_->label ? std::string_view(*impl_->label) : std::string_view{}; }
void TreeData::set_alignment(TreeAlignment a) {
    auto old = impl_->alignment; impl_->alignment = a;
    try { validate(); } catch (...) { impl_->alignment = old; throw; }
}
void TreeData::set_allow_overlap(bool allow) {
    auto old = impl_->overlap; impl_->overlap = allow;
    try { validate(); } catch (...) { impl_->overlap = old; throw; }
}
void TreeData::set_label(std::optional<std::string> label) { impl_->label = std::move(label); }
void TreeData::set_obs_tree(std::string key, Tree tree) {
    auto& trees = impl_->obst;
    auto it = std::find_if(trees.begin(), trees.end(), [&](const auto& p) { return p.first == key; });
    if (it == trees.end()) {
        trees.emplace_back(std::move(key), std::move(tree));
        try { validate(); update_tree_labels(*impl_, true); } catch (...) { trees.pop_back(); throw; }
    } else {
        auto old = std::move(it->second); it->second = std::move(tree);
        try { validate(); update_tree_labels(*impl_, true); } catch (...) { it->second = std::move(old); throw; }
    }
}
void TreeData::set_var_tree(std::string key, Tree tree) {
    auto& trees = impl_->vart;
    auto it = std::find_if(trees.begin(), trees.end(), [&](const auto& p) { return p.first == key; });
    if (it == trees.end()) {
        trees.emplace_back(std::move(key), std::move(tree));
        try { validate(); update_tree_labels(*impl_, false); } catch (...) { trees.pop_back(); throw; }
    } else {
        auto old = std::move(it->second); it->second = std::move(tree);
        try { validate(); update_tree_labels(*impl_, false); } catch (...) { it->second = std::move(old); throw; }
    }
}
void TreeData::erase_obs_tree(std::string_view key) { erase_tree(impl_->obst, key, "obst"); update_tree_labels(*impl_, true); }
void TreeData::erase_var_tree(std::string_view key) { erase_tree(impl_->vart, key, "vart"); update_tree_labels(*impl_, false); }
TreeView TreeData::obs_tree(std::string_view key) const { return find_tree(impl_->obst, key, "obst").view(); }
TreeView TreeData::var_tree(std::string_view key) const { return find_tree(impl_->vart, key, "vart").view(); }
std::vector<std::string> TreeData::obs_tree_names() const { return names(impl_->obst); }
std::vector<std::string> TreeData::var_tree_names() const { return names(impl_->vart); }
TreeDataView TreeData::view() const {
    validate();
    auto p = std::make_shared<detail::TreeDataViewImpl>();
    p->data = impl_->data.view(); p->alignment = impl_->alignment; p->overlap = impl_->overlap; p->label = impl_->label;
    for (const auto& [k,t] : impl_->obst) p->obst.emplace_back(k,t.view());
    for (const auto& [k,t] : impl_->vart) p->vart.emplace_back(k,t.view());
    return TreeDataView(std::move(p));
}
TreeDataView TreeData::slice(Selection obs, Selection var) const { return view().slice(std::move(obs), std::move(var)); }
TreeData TreeData::copy() const { return view().copy(); }
void TreeData::validate() const {
    if (!impl_) fail(ErrorCode::invalid_argument, "treedata", "empty TreeData");
    if (impl_->alignment != TreeAlignment::leaves && impl_->alignment != TreeAlignment::nodes && impl_->alignment != TreeAlignment::subset)
        fail(ErrorCode::invalid_argument, "treedata/alignment", "unknown alignment");
    impl_->data.validate();
    validate_axis(impl_->obst, impl_->data.obs(), impl_->alignment, impl_->overlap, "obst");
    validate_axis(impl_->vart, impl_->data.var(), impl_->alignment, impl_->overlap, "vart");
}
TreeDataView::TreeDataView() = default;
TreeDataView::TreeDataView(std::shared_ptr<detail::TreeDataViewImpl> p) : impl_(std::move(p)) {}
AnnDataView TreeDataView::data() const { require_view(impl_); return impl_->data; }
TreeAlignment TreeDataView::alignment() const { require_view(impl_); return impl_->alignment; }
bool TreeDataView::allow_overlap() const { require_view(impl_); return impl_->overlap; }
bool TreeDataView::has_label() const { require_view(impl_); return impl_->label.has_value(); }
std::string_view TreeDataView::label() const { require_view(impl_); return impl_->label ? std::string_view(*impl_->label) : std::string_view{}; }
TreeView TreeDataView::obs_tree(std::string_view key) const { require_view(impl_); return find_tree(impl_->obst,key,"obst"); }
TreeView TreeDataView::var_tree(std::string_view key) const { require_view(impl_); return find_tree(impl_->vart,key,"vart"); }
std::vector<std::string> TreeDataView::obs_tree_names() const { require_view(impl_); return names(impl_->obst); }
std::vector<std::string> TreeDataView::var_tree_names() const { require_view(impl_); return names(impl_->vart); }
std::size_t TreeDataView::obs_tree_count() const { require_view(impl_); return impl_->obst.size(); }
std::size_t TreeDataView::var_tree_count() const { require_view(impl_); return impl_->vart.size(); }
std::string_view TreeDataView::obs_tree_name(std::size_t i) const {
    require_view(impl_);
    if (i >= impl_->obst.size()) fail(ErrorCode::out_of_bounds, "obst", "tree index out of bounds");
    return impl_->obst[i].first;
}
std::string_view TreeDataView::var_tree_name(std::size_t i) const {
    require_view(impl_);
    if (i >= impl_->vart.size()) fail(ErrorCode::out_of_bounds, "vart", "tree index out of bounds");
    return impl_->vart[i].first;
}
TreeDataView TreeDataView::slice(Selection obs, Selection var) const {
    require_view(impl_);
    const bool obs_all = obs.is_all(), var_all = var.is_all();
    if (obs_all && var_all) return *this;
    auto out = std::make_shared<detail::TreeDataViewImpl>();
    out->data = impl_->data.slice(std::move(obs), std::move(var));
    out->alignment = impl_->alignment; out->overlap = impl_->overlap; out->label = impl_->label;
    if (out->alignment == TreeAlignment::subset) {
        out->obst = impl_->obst; out->vart = impl_->vart; return TreeDataView(std::move(out));
    }
    // Node alignment falls back to subset when either axis induces a disconnected tree.
    if (out->alignment == TreeAlignment::nodes) {
        for (const auto& [k,t] : impl_->obst) { (void)k; if (!connected(t, selected_nodes(t,out->data.obs(),out->alignment))) out->alignment = TreeAlignment::subset; }
        for (const auto& [k,t] : impl_->vart) { (void)k; if (!connected(t, selected_nodes(t,out->data.var(),out->alignment))) out->alignment = TreeAlignment::subset; }
        if (out->alignment == TreeAlignment::subset) { out->obst = impl_->obst; out->vart = impl_->vart; return TreeDataView(std::move(out)); }
    }
    if (obs_all) out->obst = impl_->obst;
    else for (const auto& [k,t] : impl_->obst) {
        auto projected = project_tree(t, selected_nodes(t,out->data.obs(),out->alignment)); out->obst.emplace_back(k,projected.view());
    }
    if (var_all) out->vart = impl_->vart;
    else for (const auto& [k,t] : impl_->vart) {
        auto projected = project_tree(t, selected_nodes(t,out->data.var(),out->alignment)); out->vart.emplace_back(k,projected.view());
    }
    return TreeDataView(std::move(out));
}
TreeData TreeDataView::copy() const {
    require_view(impl_);
    TreeData result(impl_->data.copy(), impl_->alignment, impl_->overlap, impl_->label);
    for (const auto& [k,t] : impl_->obst) result.set_obs_tree(k,t.copy());
    for (const auto& [k,t] : impl_->vart) result.set_var_tree(k,t.copy());
    return result;
}
}
