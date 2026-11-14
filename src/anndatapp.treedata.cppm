module;
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module anndatapp.treedata;
export import anndatapp.backed;

export namespace anndatapp {

enum class TreeAlignment { leaves, nodes, subset };
namespace detail { struct TreeImpl; struct TreeAccess; struct TreeDataImpl; struct TreeDataViewImpl; struct BackedTreeDataImpl; }
class Tree;

// Node identifiers and edge endpoint indices are exposed in packed storage.
// Each node/edge attribute is an Element mapping; absence differs from null.
class TreeView {
public:
    TreeView();
    std::size_t n_nodes() const;
    std::size_t n_edges() const;
    StringArrayView node_ids() const;
    ArrayView edges() const; // uint64, shape [n_edges, 2], indexes node_ids
    ElementView node_attributes(std::size_t node) const;
    ElementView edge_attributes(std::size_t edge) const;
    Tree copy() const;
    void validate() const;
private:
    std::shared_ptr<detail::TreeImpl> impl_;
    explicit TreeView(std::shared_ptr<detail::TreeImpl>);
    friend class Tree;
};

class Tree {
public:
    Tree();
    ~Tree();
    Tree(Tree&&) noexcept;
    Tree& operator=(Tree&&) noexcept;
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;
    // edge_pairs contains source,target indices; buffers are copied once.
    static Tree from_edges(std::vector<std::string> node_ids,
                           std::vector<std::uint64_t> edge_pairs);
    static Tree from_arrays(StringArray node_ids, Array edges);
    TreeView view() const;
    Tree copy() const;
    void set_node_attributes(std::size_t node, Element attributes);
    void set_edge_attributes(std::size_t edge, Element attributes);
    void validate() const;
private:
    std::shared_ptr<detail::TreeImpl> impl_;
    explicit Tree(std::shared_ptr<detail::TreeImpl>);
    friend class TreeView;
    friend struct detail::TreeAccess;
};

class TreeDataView;
class TreeData {
public:
    explicit TreeData(AnnData data, TreeAlignment alignment = TreeAlignment::leaves,
                      bool allow_overlap = true, std::optional<std::string> label = std::string("tree"));
    ~TreeData();
    TreeData(TreeData&&) noexcept;
    TreeData& operator=(TreeData&&) noexcept;
    TreeData(const TreeData&) = delete;
    TreeData& operator=(const TreeData&) = delete;
    AnnData& data();
    const AnnData& data() const;
    TreeAlignment alignment() const;
    bool allow_overlap() const;
    bool has_label() const;
    std::string_view label() const;
    void set_alignment(TreeAlignment);
    void set_allow_overlap(bool);
    void set_label(std::optional<std::string>);
    // When label is set, tree insertion/removal refreshes that axis column.
    void set_obs_tree(std::string key, Tree tree);
    void set_var_tree(std::string key, Tree tree);
    void erase_obs_tree(std::string_view key);
    void erase_var_tree(std::string_view key);
    TreeView obs_tree(std::string_view key) const;
    TreeView var_tree(std::string_view key) const;
    std::vector<std::string> obs_tree_names() const;
    std::vector<std::string> var_tree_names() const;
    TreeDataView view() const;
    // Slicing retains selected attributes and allocates projected topology.
    TreeDataView slice(Selection obs = all, Selection var = all) const;
    TreeData copy() const;
    void validate() const;
private:
    std::unique_ptr<detail::TreeDataImpl> impl_;
};

class TreeDataView {
public:
    TreeDataView();
    AnnDataView data() const;
    TreeAlignment alignment() const;
    bool allow_overlap() const;
    bool has_label() const;
    std::string_view label() const;
    TreeView obs_tree(std::string_view key) const;
    TreeView var_tree(std::string_view key) const;
    std::vector<std::string> obs_tree_names() const;
    std::vector<std::string> var_tree_names() const;
    // Borrowed names remain valid while this view is retained.
    std::size_t obs_tree_count() const;
    std::size_t var_tree_count() const;
    std::string_view obs_tree_name(std::size_t index) const;
    std::string_view var_tree_name(std::size_t index) const;
    // Slicing retains selected attributes and allocates projected topology.
    TreeDataView slice(Selection obs = all, Selection var = all) const;
    TreeData copy() const;
private:
    std::shared_ptr<detail::TreeDataViewImpl> impl_;
    explicit TreeDataView(std::shared_ptr<detail::TreeDataViewImpl>);
    friend class TreeData;
};

class BackedTreeData {
public:
    BackedTreeData();
    ~BackedTreeData();
    BackedTreeData(BackedTreeData&&) noexcept;
    BackedTreeData& operator=(BackedTreeData&&) noexcept;
    BackedTreeData(const BackedTreeData&) = delete;
    BackedTreeData& operator=(const BackedTreeData&) = delete;
    bool is_open() const noexcept;
    BackedAnnDataView data() const;
    TreeAlignment alignment() const;
    bool allow_overlap() const;
    bool has_label() const;
    std::string_view label() const;
    TreeView obs_tree(std::string_view key) const;
    TreeView var_tree(std::string_view key) const;
    std::vector<std::string> obs_tree_names() const;
    std::vector<std::string> var_tree_names() const;
    TreeData read(Selection obs = all, Selection var = all) const;
    void close();
private:
    std::unique_ptr<detail::BackedTreeDataImpl> impl_;
    explicit BackedTreeData(std::unique_ptr<detail::BackedTreeDataImpl>);
    friend BackedTreeData open_h5td(std::string_view, BackedOptions);
    friend BackedTreeData open_treedata_zarr(std::string_view, BackedOptions);
};

// Scratch covers library-owned serialization and transfer staging; source data
// remains caller-owned. Indivisible values that cannot fit are rejected.
struct TreeWriteOptions { std::size_t scratch_bytes = 64u * 1024u * 1024u; };

[[nodiscard]] BackedTreeData open_h5td(std::string_view path, BackedOptions = {});
[[nodiscard]] TreeData read_h5td(std::string_view path, BackedOptions = {});
void write_h5td(std::string_view path, const TreeDataView&, TreeWriteOptions = {});
void write_h5td(std::string_view path, const TreeData&, TreeWriteOptions = {});
[[nodiscard]] BackedTreeData open_treedata_zarr(std::string_view path, BackedOptions = {});
[[nodiscard]] TreeData read_treedata_zarr(std::string_view path, BackedOptions = {});
void write_treedata_zarr(std::string_view path, const TreeDataView&, TreeWriteOptions = {});
void write_treedata_zarr(std::string_view path, const TreeData&, TreeWriteOptions = {});

} // namespace anndatapp
