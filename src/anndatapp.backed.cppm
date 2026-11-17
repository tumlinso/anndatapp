module;
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module anndatapp.backed;
export import anndatapp.core;

export namespace anndatapp {
enum class AccessMode { read_only, read_write };
struct BackedOptions {
    AccessMode mode = AccessMode::read_only;
    Validation validation = Validation::full;
    std::size_t scratch_bytes = 64u * 1024u * 1024u;
    std::size_t decoded_cache_bytes = 0;
};
struct DenseBuffer {
    void* data = nullptr;
    DType dtype = DType::float64;
    std::vector<std::size_t> shape;
    // Byte strides, including negative strides. data is the first logical cell.
    // Empty strides request contiguous row-major storage.
    std::vector<std::ptrdiff_t> strides;
    template<class T> static DenseBuffer from(T* data, std::vector<std::size_t> shape,
                                              std::vector<std::ptrdiff_t> byte_strides = {}) {
        return {data, dtype_of<T>(), std::move(shape), std::move(byte_strides)};
    }
};
struct SparseBuffer {
    void* values = nullptr;
    void* indices = nullptr;
    void* offsets = nullptr;
    DType dtype = DType::float64;
    DType index_dtype = DType::int32;
    DType offset_dtype = DType::int32;
    std::size_t values_capacity = 0;
    std::size_t indices_capacity = 0;
    std::size_t offsets_capacity = 0;
};
struct ReadRequirements {
    Layout layout = Layout::dense;
    DType dtype = DType::float64;
    DType index_dtype = DType::int32;
    DType offset_dtype = DType::int32;
    std::vector<std::size_t> shape;
    std::size_t nnz = 0;
};
enum class StorageOrganization { unknown, contiguous, chunked, sharded };
struct PhysicalArrayGeometry {
    StorageOrganization organization = StorageOrganization::unknown;
    std::vector<std::size_t> shape, chunks, inner_chunks, write_unit;
};
// Physical coordinates never change with selection. Non-affine selections
// have affine=false; start/step describe only affine source mappings.
struct StorageAxis {
    bool affine = true;
    std::ptrdiff_t start = 0, step = 1;
};
struct StorageGeometry {
    Layout layout = Layout::dense;
    DType dtype = DType::float64;
    std::vector<std::size_t> shape, source_shape;
    std::vector<StorageAxis> axes;
    PhysicalArrayGeometry data, indices, offsets;
    bool parallel_reads = false, parallel_writes = false;
    bool writable = false;
};
struct TransferStats {
    std::size_t read_calls = 0, write_calls = 0;
    std::size_t read_bytes = 0, write_bytes = 0;
    // Counted backend staging and context-tile allocation events, not all
    // allocations. scratch_bytes is retained capacity at this observation.
    std::size_t scratch_allocations = 0, scratch_bytes = 0;
};
namespace detail {
struct BackedNode;
struct BackedDataset;
struct BackedStoreImpl;
struct PreparedReadImpl;
struct StructureEditImpl;
struct BackedAccess;
struct TransferContextImpl;
}
// One context per worker. Serialize handle/context preparation and session
// operations, including structural edits and close. Numerical buffers remain
// caller-owned. Structural replacement and close invalidate affected contexts;
// numerical payload edits do not.
class TransferContext {
public:
    TransferContext();
    ~TransferContext();
    TransferContext(TransferContext&&) noexcept;
    TransferContext& operator=(TransferContext&&) noexcept;
    TransferContext(const TransferContext&) = delete;
    TransferContext& operator=(const TransferContext&) = delete;
    StorageGeometry geometry() const;
    ReadRequirements requirements(const std::vector<std::size_t>& start,
                                  const std::vector<std::size_t>& count);
    void read_into(const std::vector<std::size_t>& start,
                   const std::vector<std::size_t>& count, const DenseBuffer&);
    void read_into(const std::vector<std::size_t>& start,
                   const std::vector<std::size_t>& count, const SparseBuffer&);
    void write_from(const std::vector<std::size_t>& start,
                    const std::vector<std::size_t>& count, const DenseBuffer&);
    TransferStats stats() const;
    std::size_t scratch_capacity() const;
private:
    std::unique_ptr<detail::TransferContextImpl> impl_;
    explicit TransferContext(std::unique_ptr<detail::TransferContextImpl>);
    friend class BackedArray;
    friend struct detail::BackedAccess;
};
// Retained read-only file bytes. Typed interpretation is an explicit Linux/Clang
// extension contract; it does not establish C++ object lifetime in file storage.
class MappedArray {
public:
    MappedArray() = default;
    const std::byte* data() const noexcept { return data_; }
    std::size_t byte_size() const noexcept { return byte_size_; }
    DType dtype() const noexcept { return dtype_; }
    const std::vector<std::size_t>& shape() const noexcept { return shape_; }
    template<class T> const T* unsafe_data() const {
        if (dtype_ != dtype_of<T>()) throw Error(ErrorCode::dtype_mismatch, "mapping", "mapped dtype differs");
        return reinterpret_cast<const T*>(data_);
    }
    template<class T> const T* unsafe_dense() const { return unsafe_data<T>(); }
    MappedArray(std::shared_ptr<const void> owner, const std::byte* data,
                std::size_t bytes, DType dtype, std::vector<std::size_t> shape)
        : owner_(std::move(owner)), data_(data), byte_size_(bytes), dtype_(dtype), shape_(std::move(shape)) {}
private:
    std::shared_ptr<const void> owner_;
    const std::byte* data_ = nullptr;
    std::size_t byte_size_ = 0;
    DType dtype_ = DType::float64;
    std::vector<std::size_t> shape_;
};
class PreparedRead {
public:
    void preflight(const DenseBuffer&) const;
    void preflight(const SparseBuffer&) const;
    PreparedRead();
    ReadRequirements requirements() const;
    Array read() const;
    void read_into(const DenseBuffer&) const;
    void read_into(const SparseBuffer&) const;
private:
    std::shared_ptr<detail::PreparedReadImpl> impl_;
    explicit PreparedRead(std::shared_ptr<detail::PreparedReadImpl>);
    friend class BackedArray;
    friend struct detail::BackedAccess;
};
class BackedArray {
public:
    std::uint64_t content_revision() const;
    BackedArray();
    Layout layout() const;
    DType dtype() const;
    std::size_t rank() const;
    std::size_t extent(std::size_t axis) const;
    std::vector<std::size_t> shape() const;
    DType index_dtype() const;
    DType offset_dtype() const;
    BackedArray select(std::vector<Selection> axes) const;
    Array read() const;
    PreparedRead prepare_read() const;
    StorageGeometry storage_geometry() const;
    TransferContext prepare_transfer(std::size_t scratch_bytes = 0) const;
    void read_into(const DenseBuffer&) const;
    void read_into(const SparseBuffer&) const;
    // Only an eligible full array can map. Failure is explicit and never falls
    // back to an allocating read. The returned owner is read-only.
    MappedArray map_readonly() const;
    // A non-contiguous sparse source is copied before its values are written.
    void write(const ArrayView& source) const;
private:
    friend struct detail::BackedAccess;
    std::shared_ptr<detail::BackedNode> node_;
    explicit BackedArray(std::shared_ptr<detail::BackedNode>);
    friend class BackedElement;
    friend class BackedColumn;
    friend class BackedAnnDataView;
    friend class BackedAnnData;
};
class BackedStringArray {
public:
    BackedStringArray();
    std::vector<std::size_t> shape() const;
    std::size_t size() const;
    std::string string(std::size_t flat_index) const;
    BackedStringArray select(std::vector<Selection> axes) const;
    StringArray read() const;
private:
    friend struct detail::BackedAccess;
    std::shared_ptr<detail::BackedNode> node_;
    explicit BackedStringArray(std::shared_ptr<detail::BackedNode>);
    friend class BackedElement;
};
class BackedColumn {
public:
    BackedColumn();
    std::size_t size() const;
    ColumnKind kind() const;
    bool nullable() const;
    bool ordered() const;
    BackedArray values() const;
    // Logical categorical codes and dictionaries are read-only. Explicit
    // unsafe handles edit the physical encoding; callers maintain invariants.
    BackedArray codes() const;
    BackedColumn categories() const;
    BackedArray unsafe_codes() const;
    BackedColumn unsafe_categories() const;
    BackedColumn select(Selection rows) const;
    Column read() const;
private:
    friend struct detail::BackedAccess;
    std::shared_ptr<detail::BackedNode> node_;
    explicit BackedColumn(std::shared_ptr<detail::BackedNode>);
    friend class BackedTable;
    friend class BackedElement;
};
class BackedTable {
public:
    BackedTable();
    std::size_t size() const;
    std::string index_name() const;
    std::string name(std::size_t row) const;
    std::vector<std::string> names() const;
    std::vector<std::string> column_names() const;
    bool contains(std::string_view column) const;
    BackedColumn column(std::string_view column) const;
    BackedTable select(Selection rows) const;
    Table read() const;
private:
    friend struct detail::BackedAccess;
    std::shared_ptr<detail::BackedNode> node_;
    explicit BackedTable(std::shared_ptr<detail::BackedNode>);
    friend class BackedElement;
    friend class BackedAnnDataView;
    friend class BackedAnnData;
};
class BackedElement {
public:
    BackedElement();
    ElementKind kind() const;
    std::vector<std::string> keys() const;
    bool contains(std::string_view key) const;
    BackedElement at(std::string_view key) const;
    BackedArray array() const;
    BackedStringArray string_array() const;
    BackedColumn column() const;
    BackedTable table() const;
    std::vector<std::string> field_names() const;
    Element read_field(std::string_view name) const;
    Element read() const;
private:
    friend struct detail::BackedAccess;
    std::shared_ptr<detail::BackedNode> node_;
    explicit BackedElement(std::shared_ptr<detail::BackedNode>);
    friend class BackedAnnDataView;
    friend class BackedAnnData;
};
class BackedAnnDataView {
public:
    BackedAnnDataView();
    std::size_t n_obs() const;
    std::size_t n_vars() const;
    bool has_x() const;
    BackedArray x() const;
    BackedTable obs() const;
    BackedTable var() const;
    BackedArray layer(std::string_view key) const;
    BackedElement obsm(std::string_view key) const;
    BackedElement varm(std::string_view key) const;
    BackedArray obsp(std::string_view key) const;
    BackedArray varp(std::string_view key) const;
    std::vector<std::string> layer_names() const;
    std::vector<std::string> obsm_names() const;
    std::vector<std::string> varm_names() const;
    std::vector<std::string> obsp_names() const;
    std::vector<std::string> varp_names() const;
    BackedElement uns() const;
    bool has_raw() const;
    bool has_raw_x() const;
    BackedArray raw_x() const;
    BackedTable raw_var() const;
    BackedElement raw_varm(std::string_view key) const;
    std::vector<std::string> raw_varm_names() const;
    BackedAnnDataView slice(Selection obs = all, Selection var = all) const;
    AnnData read() const;
    AnnData to_memory() const { return read(); }
private:
    friend struct detail::BackedAccess;
    std::shared_ptr<detail::BackedDataset> impl_;
    explicit BackedAnnDataView(std::shared_ptr<detail::BackedDataset>);
    friend class BackedAnnData;
    friend class StructureEdit;
};
class StructureEdit {
public:
    StructureEdit();
    ~StructureEdit();
    StructureEdit(StructureEdit&&) noexcept;
    StructureEdit& operator=(StructureEdit&&) noexcept;
    StructureEdit(const StructureEdit&) = delete;
    StructureEdit& operator=(const StructureEdit&) = delete;
    // Paths use AnnData slots, e.g. /X, /layers/counts, /obs/new_column.
    void set(std::string path, Element value);
    void set(std::string path, BackedArray source);
    void set(std::string path, BackedElement source);
    void set(std::string path, BackedTable source);
    void set(std::string path, BackedColumn source);
    void erase(std::string path);
    void set_obs(Table value);
    void set_var(Table value);
    // Reindex subsets/permutations and transform every aligned slot, including
    // pairwise arrays and raw observation alignment. Duplicate positions remain.
    void reindex(Selection obs = all, Selection var = all);
    void apply();
private:
    std::unique_ptr<detail::StructureEditImpl> impl_;
    explicit StructureEdit(std::unique_ptr<detail::StructureEditImpl>);
    friend class BackedAnnData;
};
class BackedAnnData {
public:
    BackedAnnData();
    ~BackedAnnData();
    BackedAnnData(BackedAnnData&&) noexcept;
    BackedAnnData& operator=(BackedAnnData&&) noexcept;
    BackedAnnData(const BackedAnnData&) = delete;
    BackedAnnData& operator=(const BackedAnnData&) = delete;
    bool is_open() const noexcept;
    bool writable() const;
    std::string location() const;
    BackedAnnDataView view() const;
    BackedAnnDataView slice(Selection obs = all, Selection var = all) const;
    std::size_t n_obs() const;
    std::size_t n_vars() const;
    bool has_x() const;
    BackedArray x() const;
    BackedTable obs() const;
    BackedTable var() const;
    BackedElement uns() const;
    BackedArray layer(std::string_view key) const;
    BackedElement obsm(std::string_view key) const;
    BackedElement varm(std::string_view key) const;
    BackedArray obsp(std::string_view key) const;
    BackedArray varp(std::string_view key) const;
    std::vector<std::string> layer_names() const;
    std::vector<std::string> obsm_names() const;
    std::vector<std::string> varm_names() const;
    std::vector<std::string> obsp_names() const;
    std::vector<std::string> varp_names() const;
    bool has_raw() const;
    bool has_raw_x() const;
    BackedArray raw_x() const;
    BackedTable raw_var() const;
    BackedElement raw_varm(std::string_view key) const;
    std::vector<std::string> raw_varm_names() const;
    AnnData read() const;
    AnnData to_memory() const { return read(); }
    void validate_all() const;
    void flush();
    void close();
    StructureEdit edit_structure();
    void set(std::string path, Element value);
    void set(std::string path, BackedArray source);
    void set(std::string path, BackedElement source);
    void set(std::string path, BackedTable source);
    void set(std::string path, BackedColumn source);
    void erase(std::string path);
private:
    friend struct detail::BackedAccess;
    explicit BackedAnnData(std::shared_ptr<detail::BackedDataset>);
    std::shared_ptr<detail::BackedDataset> impl_;
};
// Generic hierarchical storage. Acquired descriptors retain the physical
// session; explicit close invalidates them. Paths are absolute within the store.
class BackedStore {
public:
    BackedStore();
    ~BackedStore();
    BackedStore(BackedStore&&) noexcept;
    BackedStore& operator=(BackedStore&&) noexcept;
    BackedStore(const BackedStore&) = delete;
    BackedStore& operator=(const BackedStore&) = delete;
    bool is_open() const noexcept;
    bool writable() const;
    std::string location() const;
    bool exists(std::string_view path) const;
    std::vector<std::string> children(std::string_view path = "/") const;
    Element attributes(std::string_view path = "/") const;
    // Merge supplied attributes; unrelated metadata remains in the store.
    void set_attributes(std::string_view path, ElementView attributes);
    void create_group(std::string_view path, ElementView attributes = {});
    BackedArray array(std::string_view path) const;
    BackedStringArray strings(std::string_view path) const;
    BackedArray create_array(std::string_view path, DType, std::vector<std::size_t> shape,
                            ElementView attributes = {});
    BackedStringArray create_strings(std::string_view path, std::vector<std::size_t> shape,
                                    ElementView attributes = {});
    std::string read_string(std::string_view path) const;
    BackedElement element(std::string_view path) const;
    void write_array(std::string_view path, ArrayView, ElementView attributes = {});
    void write_strings(std::string_view path, StringArrayView, ElementView attributes = {});
    // The batch is caller-owned. Backend workspace uses the session budget;
    // callers reserve their own packing buffers separately from that budget.
    void write_strings(std::string_view path, const std::vector<std::size_t>& start,
                       const std::vector<std::size_t>& count, const std::vector<std::string>& values);
    void write_string(std::string_view path, std::string_view value, ElementView attributes = {});
    void write_element(std::string_view path, ElementView);
    // Legacy root admission is explicit for linked AnnDataSet manifests only;
    // element codecs and ordinary H5AD entry points remain version-strict.
    BackedAnnData anndata(std::string_view root = "/", bool allow_unversioned_root = false) const;
    void write_anndata(std::string_view root, AnnDataView, Validation = Validation::full);
    void write_anndata(std::string_view root, BackedAnnDataView, Validation = Validation::full);
    void erase(std::string_view path);
    void flush();
    void close();
private:
    friend struct detail::BackedAccess;
    explicit BackedStore(std::shared_ptr<detail::BackedStoreImpl>);
    std::shared_ptr<detail::BackedStoreImpl> impl_;
};
namespace detail {
// Internal cross-module bridges, excluded from the public API contract.
// Retained numeric-plan object/path/vector capacities, counting shared gathers
// conservatively per piece. Excludes source sessions, allocator/control-block
// overhead and RSS; no backend access or numerical payload allocation.
std::size_t prepared_read_retained_bytes(const PreparedRead&);

enum class WriteBackend { hdf5, zarr };
struct WriteBudgets { std::size_t scratch_bytes, packing_bytes; };
WriteBudgets write_budgets(WriteBackend, std::size_t scratch_bytes);
PreparedRead prepare_backed_x_read(const BackedAnnData&, ::anndatapp::Selection obs, ::anndatapp::Selection var);
ReadRequirements backed_x_metadata(const BackedAnnData&);
// Serialized collection execution; ordinary worker contexts retain private backends.
// False leaves a valid checked piece but admits no optimized execution for it.
bool prepare_backed_x_read(const BackedAnnData&, ::anndatapp::Selection obs, ::anndatapp::Selection var,
    TransferContext&, std::size_t retained_budget, PreparedRead&);
std::size_t backed_read_context_bytes(const TransferContext&);
void refresh_backed_read_context(TransferContext&);
void preflight_backed_read(TransferContext&, const PreparedRead&, const SparseBuffer&);
void read_preflighted_backed(TransferContext&, const PreparedRead&, const SparseBuffer&);
void compare_backed_observation_indexes(const BackedAnnData&, const BackedAnnData* files,
    std::size_t file_count, const std::size_t* offsets, const ::anndatapp::Axis& rows, std::size_t selector_bytes);
void compare_backed_component_indexes(const BackedAnnData&, const BackedAnnData*, std::size_t file_count,
    const std::size_t* ids, std::size_t count, const ::anndatapp::Selection& columns);
void compare_backed_table_indexes(const BackedTable&, const BackedTable&);
void compare_backed_table_indexes(const BackedTable&, const BackedTable*, std::size_t count);
void compare_backed_component_indexes(const BackedTable&, const BackedAnnData*, std::size_t file_count,
                                      const std::size_t* ids, std::size_t count, const ::anndatapp::Selection& columns);
void preflight_anndata_write(AnnDataView, Validation, WriteBudgets);
void preflight_zarr_attributes(ElementView);
void preflight_array_write(ArrayView, Validation, WriteBudgets, std::string_view path);

BackedAnnData open_h5ad_backed(std::string_view path, BackedOptions options);
BackedAnnData open_zarr_backed(std::string_view path, BackedOptions options);
void write_zarr_backed(std::string_view, const BackedAnnDataView&, int format, bool exclusive, std::size_t scratch_bytes, std::string_view compression, int compression_level, Validation validation);
void write_zarr_eager(std::string_view, const AnnDataView&, int format, bool exclusive, std::size_t scratch_bytes, std::string_view compression, int compression_level, Validation validation);
void write_h5ad_backed(std::string_view, const BackedAnnDataView&, bool exclusive, bool gzip, int gzip_level, std::optional<ByteOrder> byte_order, Validation validation);
BackedStore open_hdf5_store_impl(std::string_view, BackedOptions);
BackedStore create_hdf5_store_impl(std::string_view, bool, bool, int, std::optional<ByteOrder>, Validation, std::size_t, std::size_t = 64u * 1024u * 1024u);
BackedStore open_zarr_store_impl(std::string_view, BackedOptions);
BackedStore create_zarr_store_impl(std::string_view, int, bool, std::size_t, std::string_view, int, Validation);
}
} // namespace anndatapp
