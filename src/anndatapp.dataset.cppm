module;
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

export module anndatapp.dataset;
export import anndatapp.backed;

export namespace anndatapp {

// Paths in a collection manifest are interpreted relative to the manifest.
struct DatasetComponent {
    std::string key;
    std::string path;
};

struct DatasetOptions {
    // The collection reserves one quarter of this budget for temporary
    // positional/index conversion and gives the remainder to component I/O.
    // For budgets below 128 bytes, component I/O receives the full budget.
    // Explicitly acquired selection descriptors are retained metadata.
    BackedOptions backed{};
    // Overrides are applied in memory and do not rewrite the manifest.
    std::vector<DatasetComponent> relocate;
};

namespace detail { struct AnnDataSetImpl; }
class AnnDataSet;

// The view retains its collection and its selection. Closing the owner closes
// the shared sessions and invalidates the view and component descriptors.
class AnnDataSetView {
public:
    AnnDataSetView();
    std::size_t n_obs() const;
    std::size_t n_vars() const;
    std::size_t n_components() const;
    bool has_x() const;
    std::vector<DatasetComponent> components() const;
    // Returns the complete component; selection is on this collection view.
    BackedAnnDataView component(std::size_t index) const;
    BackedAnnDataView annotations() const;
    AnnDataSetView slice(Selection obs = all, Selection var = all) const;

    // Selection metadata may allocate; numerical payload is read only by the
    // explicit transfers. requirements() reports the stacked output layout.
    ReadRequirements requirements() const;
    void read_x_into(const DenseBuffer& destination) const;
    void read_x_into(const SparseBuffer& destination) const;
    AnnData materialize() const;

private:
    std::shared_ptr<detail::AnnDataSetImpl> impl_;
    BackedAnnDataView annotations_;
    Axis obs_, var_;
    AnnDataSetView(std::shared_ptr<detail::AnnDataSetImpl>, BackedAnnDataView, Axis, Axis);
    friend class AnnDataSet;
};

class AnnDataSet {
public:
    AnnDataSet();
    ~AnnDataSet();
    AnnDataSet(AnnDataSet&&) noexcept;
    AnnDataSet& operator=(AnnDataSet&&) noexcept;
    AnnDataSet(const AnnDataSet&) = delete;
    AnnDataSet& operator=(const AnnDataSet&) = delete;
    bool is_open() const noexcept;
    std::string location() const;
    std::size_t n_obs() const;
    std::size_t n_vars() const;
    std::size_t n_components() const;
    bool has_x() const;
    AnnDataSetView view() const;
    AnnDataSetView slice(Selection obs = all, Selection var = all) const;
    // Collection-level metadata can be edited through the existing explicit
    // structure interface. Do not replace the reserved uns/AnnDataSet table.
    void set_annotation(std::string path, Element value);
    void validate() const;
    void flush();
    void close();
    // Writes a new manifest without copying the linked component files.
    void copy_manifest(std::string_view destination, bool overwrite = false) const;

private:
    std::shared_ptr<detail::AnnDataSetImpl> impl_;
    explicit AnnDataSet(std::shared_ptr<detail::AnnDataSetImpl>);
    friend AnnDataSet open_h5ads(std::string_view, DatasetOptions);
    friend AnnDataSet create_h5ads(std::string_view, std::vector<DatasetComponent>,
                                   std::string_view, DatasetOptions);
};

[[nodiscard]] AnnDataSet open_h5ads(std::string_view path, DatasetOptions options = {});
// Component paths are interpreted relative to the new manifest's parent.
// The destination is created exclusively; its parent must already exist.
[[nodiscard]] AnnDataSet create_h5ads(std::string_view path,
    std::vector<DatasetComponent> components, std::string_view sample_column = "sample",
    DatasetOptions options = {});

} // namespace anndatapp
