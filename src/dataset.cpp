module;
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#if ANNDATAPP_HAS_HDF5
#include <sys/stat.h>
#endif
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module anndatapp.dataset;
#if ANNDATAPP_HAS_HDF5
import anndatapp.h5ad;
#endif

namespace anndatapp {
namespace detail {
struct AnnDataSetImpl {
    BackedAnnData manifest;
#if ANNDATAPP_HAS_HDF5
    std::filesystem::path manifest_path;
    struct stat manifest_identity{};
#endif
    std::vector<DatasetComponent> components;
    std::vector<BackedAnnData> files;
    std::vector<std::size_t> offsets;
    std::size_t n_obs = 0, n_vars = 0;
    bool open = true, has_x = false;
    bool check_axis_names = false;
    std::size_t selector_bytes = 0;
    PreparedAnnDataSetReadImpl* live_context_plans = nullptr;
};
struct DatasetSegment { std::size_t first, count, component; };
struct PreparedDatasetPiece {
    DatasetSegment segment;
    std::size_t column, nnz, context;
    PreparedRead read;
};
struct PreparedDatasetContext {
    std::size_t component, bytes = 0;
    bool rejected = false;
    mutable TransferContext transfer;
};
struct PreparedAnnDataSetReadImpl {
    std::shared_ptr<AnnDataSetImpl> owner;
    BackedAnnDataView annotations;
    Axis obs, var;
    ReadRequirements requirements;
    std::vector<PreparedDatasetPiece> pieces;
    std::vector<PreparedDatasetContext> contexts;
    std::size_t context_bytes = 0;
    bool retained = false;
    bool reuse_contexts = true;
    PreparedAnnDataSetReadImpl *previous = nullptr, *next = nullptr;
    bool linked = false;
    ~PreparedAnnDataSetReadImpl() {
        if (!linked) return;
        if (previous) previous->next = next;
        else owner->live_context_plans = next;
        if (next) next->previous = previous;
    }
};
}

namespace {
namespace fs = std::filesystem;
using Impl = detail::AnnDataSetImpl;

[[noreturn]] void fail(ErrorCode code, std::string_view path, std::string message) {
    throw Error(code, std::string(path), std::move(message));
}
void check(const std::shared_ptr<Impl>& p) {
    if (!p || !p->open || !p->manifest.is_open())
        fail(ErrorCode::invalid_argument, "AnnDataSet", "collection is closed or empty");
}
std::string path_text(std::string_view path) {
    if (path.empty() || path.find('\0') != std::string_view::npos)
        fail(ErrorCode::invalid_argument, "AnnDataSet", "path must be nonempty and contain no NUL bytes");
    return std::string(path);
}
#if ANNDATAPP_HAS_HDF5
BackedAnnData open_manifest(std::string_view path, BackedOptions options) {
    // SnapATAC2 manifests omit root encoding attributes. This opt-in is
    // confined to collection manifests; ordinary H5AD admission stays strict.
    auto store = open_hdf5_store(path, options);
    return store.anndata("/", true);
}
struct stat manifest_identity(const fs::path& path) {
    struct stat info{};
    if (::stat(path.c_str(), &info) < 0)
        fail(ErrorCode::io_error, path.string(), "cannot inspect collection manifest identity");
    return info;
}
void check_manifest_identity(const Impl& p) {
    auto current = manifest_identity(p.manifest_path);
    if (current.st_dev != p.manifest_identity.st_dev || current.st_ino != p.manifest_identity.st_ino)
        fail(ErrorCode::io_error, p.manifest_path.string(), "collection manifest path refers to a different file");
}
#endif
fs::path absolute_path(const fs::path& path, const fs::path& base) {
    std::error_code ec;
    auto p = path.is_absolute() ? path : base / path;
    p = fs::absolute(p, ec);
    if (ec) fail(ErrorCode::io_error, path.string(), "cannot resolve path: " + ec.message());
    return p.lexically_normal();
}
bool same_path(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::exists(a, ec) && !ec && fs::exists(b, ec) && !ec) {
        bool same = fs::equivalent(a, b, ec);
        if (!ec) return same;
    }
    return a.lexically_normal() == b.lexically_normal();
}
std::string column_string(ColumnView c, std::size_t row, std::string_view path) {
    if (!c.valid(row)) fail(ErrorCode::unsupported_encoding, path, "collection paths and keys cannot be null");
    if (c.kind() == ColumnKind::string) return std::string(c.string(row));
    if (c.kind() == ColumnKind::categorical) {
        auto categories = c.categories();
        if (categories.kind() != ColumnKind::string)
            fail(ErrorCode::unsupported_encoding, path, "categorical dictionary must contain strings");
        return std::string(categories.string(static_cast<std::size_t>(c.code(row))));
    }
    fail(ErrorCode::unsupported_encoding, path, "collection paths and keys must be strings");
}
std::vector<DatasetComponent> manifest_components(const BackedAnnData& manifest) {
    auto uns = manifest.uns();
    if (!uns.contains("AnnDataSet"))
        fail(ErrorCode::unsupported_encoding, "/uns/AnnDataSet", "collection reference table is missing");
    auto table = uns.at("AnnDataSet").table();
    if (!table.contains("keys") || !table.contains("file_path"))
        fail(ErrorCode::unsupported_encoding, "/uns/AnnDataSet", "reference table needs keys and file_path columns");
    auto keys = table.column("keys").read();
    auto paths = table.column("file_path").read();
    auto k = keys.view(), p = paths.view();
    std::vector<DatasetComponent> out;
    const auto count = table.size();
    out.reserve(count);
    std::unordered_set<std::string> seen;
    for (std::size_t i = 0; i < count; ++i) {
        auto key = column_string(k, i, "/uns/AnnDataSet/keys");
        auto path = column_string(p, i, "/uns/AnnDataSet/file_path");
        if (key.empty() || key.find('\0') != std::string::npos || !seen.insert(key).second)
            fail(ErrorCode::invalid_argument, "/uns/AnnDataSet/keys", "component keys must be unique, nonempty strings");
        (void)path_text(path);
        out.push_back({std::move(key), std::move(path)});
    }
    if (out.empty()) fail(ErrorCode::invalid_argument, "/uns/AnnDataSet", "collection has no components");
    return out;
}
Table component_table(const std::vector<DatasetComponent>& components) {
    auto t = Table::numbered(components.size());
    std::vector<std::string> keys, paths;
    keys.reserve(components.size()); paths.reserve(components.size());
    for (const auto& c : components) { keys.push_back(c.key); paths.push_back(c.path); }
    t.set("keys", Column::strings(std::move(keys)));
    t.set("file_path", Column::strings(std::move(paths)));
    return t;
}
Selection axis_selection(const Axis& axis, std::size_t extent) {
    if (axis.is_all()) return all;
    const auto count = axis.extent(extent);
    if (!count) return range{0, 0, 1};
    if (!axis.is_gather() && axis.first() <= std::size_t(PTRDIFF_MAX) && axis.step() <= std::size_t(PTRDIFF_MAX)) {
        const auto last = axis.index(count - 1), step = axis.step();
        std::optional<std::ptrdiff_t> stop;
        if (axis.reverse()) { if (last >= step) stop = std::ptrdiff_t(last - step); }
        else if (step <= extent - 1 - last) stop = std::ptrdiff_t(last + step);
        return range{std::ptrdiff_t(axis.first()), stop,
                     axis.reverse() ? -std::ptrdiff_t(step) : std::ptrdiff_t(step)};
    }
    std::vector<std::ptrdiff_t> rows;
    rows.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto p = axis.index(i);
        if (p > std::size_t(std::numeric_limits<std::ptrdiff_t>::max()))
            fail(ErrorCode::out_of_bounds, "AnnDataSet/axis", "position exceeds signed index range");
        rows.push_back(std::ptrdiff_t(p));
    }
    return indices(std::move(rows));
}
Axis resolve(const Selection& s, std::size_t n, const BackedTable& names, std::string_view path) {
    if (s.kind() == Selection::Kind::label || s.kind() == Selection::Kind::labels)
        return s.resolve(names.names(), path);
    return s.resolve(n, path);
}
using Segment = detail::DatasetSegment;
// The selection itself is retained by the view. These budgets cover only
// temporary positional vectors created while preparing a component transfer.
constexpr std::size_t selector_width = 4 * sizeof(std::ptrdiff_t);
std::size_t selector_cost(const Impl& p, const Axis& cols, std::size_t rows) {
    const auto n = cols.is_gather() ? cols.extent(p.n_vars) : 0;
    if (n > SIZE_MAX / selector_width || rows > SIZE_MAX / selector_width - n)
        fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "selection staging byte count overflows");
    return (n + rows) * selector_width;
}
template<class F> void for_segments(const Impl& p, const Axis& rows, const Axis& cols, F&& f) {
    const auto n = rows.extent(p.n_obs);
    if (!n) return;
    const auto column_cost = selector_cost(p, cols, 0);
    if (column_cost > p.selector_bytes)
        fail(ErrorCode::invalid_argument, "AnnDataSet/X", "variable selection staging needs " +
             std::to_string(column_cost) + " bytes; budget is " + std::to_string(p.selector_bytes));
    const auto capacity = rows.is_gather() ? (p.selector_bytes - column_cost) / selector_width : n;
    if (!capacity) fail(ErrorCode::invalid_argument, "AnnDataSet/X", "one row of selection staging needs " +
                        std::to_string(selector_width) + " bytes; remaining budget is " + std::to_string(p.selector_bytes - column_cost));
    for (std::size_t i = 0; i < n;) {
        auto row = rows.index(i);
        if (row >= p.n_obs) fail(ErrorCode::out_of_bounds, "AnnDataSet/obs", "observation position is out of range");
        auto it = std::upper_bound(p.offsets.begin(), p.offsets.end(), row);
        auto component = std::size_t(it - p.offsets.begin() - 1);
        std::size_t end = i + 1;
        if (!rows.is_gather()) {
            const auto step = rows.is_all() ? std::size_t{1} : rows.step();
            if (!step) fail(ErrorCode::invalid_argument, "AnnDataSet/obs", "axis stride is zero");
            const auto distance = !rows.is_all() && rows.reverse()
                ? row - p.offsets[component] : p.offsets[component + 1] - 1 - row;
            const auto additional = distance / step;
            end = additional >= n - i - 1 ? n : i + additional + 1;
        } else while (end < n && end - i < capacity) {
            auto next = rows.index(end);
            if (next < p.offsets[component] || next >= p.offsets[component + 1]) break;
            ++end;
        }
        f(Segment{i, end - i, component});
        i = end;
    }
}
Selection segment_rows(const Impl& p, const Axis& rows, const Segment& s) {
    auto first = rows.index(s.first) - p.offsets[s.component];
    // Preserve affine mappings without building a per-row index list.
    if (!rows.is_gather()) {
        const auto step = rows.is_all() ? std::size_t{1} : rows.step();
        const auto limit = std::size_t(std::numeric_limits<std::ptrdiff_t>::max());
        if (first <= limit && step <= limit) {
            const bool reverse = !rows.is_all() && rows.reverse();
            const auto last = rows.index(s.first + s.count - 1) - p.offsets[s.component];
            std::optional<std::ptrdiff_t> stop;
            if (reverse) { if (last >= step) stop = std::ptrdiff_t(last - step); }
            else if (step <= p.files[s.component].n_obs() - 1 - last)
                stop = std::ptrdiff_t(last + step);
            return range{std::ptrdiff_t(first), stop, reverse ? -std::ptrdiff_t(step) : std::ptrdiff_t(step)};
        }
    }
    std::vector<std::ptrdiff_t> out;
    out.reserve(s.count);
    for (std::size_t i = 0; i < s.count; ++i)
        out.push_back(std::ptrdiff_t(rows.index(s.first + i) - p.offsets[s.component]));
    return indices(std::move(out));
}
PreparedRead prepared_x(const Impl& p, const Axis& rows, const Axis& cols, const Segment& s) {
    return detail::prepare_backed_x_read(p.files[s.component], segment_rows(p, rows, s),
                                       axis_selection(cols, p.n_vars));
}
void compare_indexes(const BackedTable& left, const BackedTable& right,
                     std::string_view path, const char* message) {
    try { detail::compare_backed_table_indexes(left, right); }
    catch (const Error& e) {
        if (e.code() == ErrorCode::shape_mismatch) fail(e.code(), path, message);
        throw;
    }
}
void check_selection_indexes(const Impl& p, const Axis& rows, const Axis& cols) {
    const auto count = rows.extent(p.n_obs);
    if (!count) return;
    {
        // Variable comparisons retain only component IDs; each peer table is
        // acquired and released inside the bounded reference-index batch.
        const auto selection = selector_cost(p, cols, 0);
        if (selection > p.selector_bytes)
            fail(ErrorCode::invalid_argument, "AnnDataSet/X", "variable selection staging exceeds budget");
        const auto capacity = std::min({count, p.files.size(), (p.selector_bytes - selection) / sizeof(std::size_t)});
        auto columns = axis_selection(cols, p.n_vars);
        auto components = capacity > 1 ? std::make_unique<std::size_t[]>(capacity) : nullptr;
        std::size_t used = 0;
        auto compare_ids = [&](const std::size_t* ids, std::size_t n) {
            try {
                detail::compare_backed_component_indexes(p.manifest, p.files.data(), p.files.size(), ids, n, columns);
            } catch (const Error& e) {
                if (e.code() == ErrorCode::shape_mismatch)
                    fail(e.code(), "AnnDataSet/var", "component variable names differ");
                throw;
            }
        };
        auto compare = [&] {
            if (!used) return;
            auto* begin = components.get();
            std::sort(begin, begin + used);
            used = std::size_t(std::unique(begin, begin + used) - begin);
            compare_ids(begin, used);
            used = 0;
        };
        for_segments(p, rows, cols, [&](const Segment& s) {
            const auto& file = p.files[s.component];
            if (!file.is_open()) fail(ErrorCode::invalid_argument, "AnnDataSet/components", "component is closed");
            if (capacity < 2) {
                compare_ids(&s.component, 1);
            } else {
                components[used++] = s.component;
                if (used == capacity) compare();
            }
        });
        compare();
    }
    // Variable selections and ID storage have been released before row
    // gathers consume the full collection selector budget.
    try {
        detail::compare_backed_observation_indexes(p.manifest, p.files.data(), p.files.size(),
            p.offsets.data(), rows, p.selector_bytes - selector_cost(p, cols, 0));
    } catch (const Error& e) {
        if (e.code() == ErrorCode::shape_mismatch)
            fail(e.code(), "AnnDataSet/obs", "component observation names differ from manifest");
        throw;
    }
}
void checked_shape(const DenseBuffer& dst, std::size_t m, std::size_t n, DType type) {
    if (dst.dtype != type) fail(ErrorCode::dtype_mismatch, "AnnDataSet/X", "destination dtype differs from X");
    if (dst.shape.size() != 2 || dst.shape[0] != m || dst.shape[1] != n)
        fail(ErrorCode::shape_mismatch, "AnnDataSet/X", "destination shape differs from selection");
    if (!dst.strides.empty() && dst.strides.size() != 2)
        fail(ErrorCode::shape_mismatch, "AnnDataSet/X", "destination must have two byte strides");
    if (m && n && !dst.data) fail(ErrorCode::invalid_argument, "AnnDataSet/X", "destination is null");
}
std::ptrdiff_t row_stride(const DenseBuffer& dst, std::size_t n) {
    if (!dst.strides.empty()) return dst.strides[0];
    const auto width = dtype_size(dst.dtype);
    if (n && width > std::size_t(std::numeric_limits<std::ptrdiff_t>::max()) / n)
        fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "destination stride exceeds signed range");
    return std::ptrdiff_t(n * width);
}
std::ptrdiff_t displacement(std::size_t row, std::ptrdiff_t stride) {
    if (row > std::size_t(std::numeric_limits<std::ptrdiff_t>::max()))
        fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "destination offset exceeds signed range");
    std::ptrdiff_t out;
    if (__builtin_mul_overflow(std::ptrdiff_t(row), stride, &out))
        fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "destination offset exceeds signed range");
    return out;
}

template<class T> Array allocate(std::vector<std::size_t> shape) { return Array::empty<T>(std::move(shape)); }
Array allocate(DType dtype, std::vector<std::size_t> shape) {
    switch (dtype) {
    case DType::boolean: return allocate<bool>(std::move(shape));
    case DType::int8: return allocate<std::int8_t>(std::move(shape));
    case DType::uint8: return allocate<std::uint8_t>(std::move(shape));
    case DType::int16: return allocate<std::int16_t>(std::move(shape));
    case DType::uint16: return allocate<std::uint16_t>(std::move(shape));
    case DType::int32: return allocate<std::int32_t>(std::move(shape));
    case DType::uint32: return allocate<std::uint32_t>(std::move(shape));
    case DType::int64: return allocate<std::int64_t>(std::move(shape));
    case DType::uint64: return allocate<std::uint64_t>(std::move(shape));
    case DType::float32: return allocate<float>(std::move(shape));
    case DType::float64: return allocate<double>(std::move(shape));
    }
    fail(ErrorCode::unsupported_dtype, "AnnDataSet/X", "unsupported dtype");
}
std::size_t plus(std::size_t a, std::size_t b) {
    if (b > std::numeric_limits<std::size_t>::max() - a)
        fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "combined size overflows");
    return a + b;
}
std::uint64_t integer_max(DType t) {
    switch (t) {
    case DType::int8: return INT8_MAX; case DType::uint8: return UINT8_MAX;
    case DType::int16: return INT16_MAX; case DType::uint16: return UINT16_MAX;
    case DType::int32: return INT32_MAX; case DType::uint32: return UINT32_MAX;
    case DType::int64: return INT64_MAX; case DType::uint64: return UINT64_MAX;
    default: fail(ErrorCode::dtype_mismatch, "AnnDataSet/X", "sparse index dtype must be an integer");
    }
}
template<class T> void store_integer(void* data, std::size_t i, std::uint64_t value) {
    T v = static_cast<T>(value);
    std::memcpy(static_cast<std::byte*>(data) + i * sizeof(T), &v, sizeof(T));
}
void write_integer(void* data, DType t, std::size_t i, std::uint64_t value) {
    if (value > integer_max(t)) fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "sparse index or offset overflows destination dtype");
    switch (t) {
    case DType::int8: store_integer<std::int8_t>(data, i, value); break; case DType::uint8: store_integer<std::uint8_t>(data, i, value); break;
    case DType::int16: store_integer<std::int16_t>(data, i, value); break; case DType::uint16: store_integer<std::uint16_t>(data, i, value); break;
    case DType::int32: store_integer<std::int32_t>(data, i, value); break; case DType::uint32: store_integer<std::uint32_t>(data, i, value); break;
    case DType::int64: store_integer<std::int64_t>(data, i, value); break; case DType::uint64: store_integer<std::uint64_t>(data, i, value); break;
    default: fail(ErrorCode::dtype_mismatch, "AnnDataSet/X", "sparse index dtype must be an integer");
    }
}
using Plan = detail::PreparedAnnDataSetReadImpl;
void check_plan(const Plan* p) {
    if (!p) fail(ErrorCode::invalid_argument, "AnnDataSet/X", "prepared read is empty");
    check(p->owner);
    (void)p->annotations.n_obs();
    for (const auto& file : p->owner->files) (void)file.n_obs();
}
void drop_contexts(Plan& p) {
    std::vector<detail::PreparedDatasetContext>().swap(p.contexts);
    p.context_bytes = 0; p.reuse_contexts = false;
}
void reject_context(Plan& p, std::size_t index) {
    auto& slot = p.contexts[index];
    if (slot.bytes > p.context_bytes)
        fail(ErrorCode::invalid_argument, "AnnDataSet/X", "prepared context accounting is inconsistent");
    p.context_bytes -= slot.bytes; slot.bytes = 0; slot.rejected = true;
    slot.transfer = TransferContext{};
}
std::size_t context_storage(const Plan& p) {
    if (p.contexts.capacity() > SIZE_MAX / sizeof(detail::PreparedDatasetContext))
        fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "prepared context capacity overflows");
    return plus(p.context_bytes, p.contexts.capacity() * sizeof(detail::PreparedDatasetContext));
}
// Account retained descriptor payload and leave room for selector and offset
// staging. If descriptors do not fit, rebuild them one piece at a time.
bool retain_piece(Plan& p, Segment s, std::size_t column, std::size_t nnz, std::size_t context,
                  PreparedRead read, std::size_t& charged) {
    if (!p.retained) return false;
    const auto& owner = *p.owner;
    const auto cols = column == SIZE_MAX ? p.var : Axis::stride(p.var.index(column), 1);
    const auto selection = selector_cost(owner, cols, p.obs.is_gather() ? s.count : 0);
    const auto staging = plus(selection, column == SIZE_MAX ? 0 : 2 * dtype_size(p.requirements.offset_dtype));
    const auto bytes = detail::prepared_read_retained_bytes(read);
    const auto limit = owner.selector_bytes;
    const auto next = p.pieces.size() == p.pieces.capacity()
        ? (p.pieces.capacity() ? plus(p.pieces.capacity(), p.pieces.capacity()) : 1) : p.pieces.capacity();
    if (next > SIZE_MAX / sizeof(detail::PreparedDatasetPiece))
        fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "prepared piece capacity overflows");
    const auto vector_bytes = next * sizeof(detail::PreparedDatasetPiece);
    const auto old_bytes = next != p.pieces.capacity() ? p.pieces.capacity() * sizeof(detail::PreparedDatasetPiece) : 0;
    auto need = plus(plus(charged, bytes), plus(staging, plus(vector_bytes, old_bytes)));
    need = plus(need, context_storage(p));
    if (need > limit && p.reuse_contexts) {
        drop_contexts(p); context = SIZE_MAX;
        need = plus(plus(charged, bytes), plus(staging, plus(vector_bytes, old_bytes)));
    }
    if (need > limit) {
        std::vector<detail::PreparedDatasetPiece>().swap(p.pieces); p.retained = false;
        drop_contexts(p); return false;
    }
    if (next != p.pieces.capacity()) p.pieces.reserve(next);
    const auto actual = p.pieces.capacity() * sizeof(detail::PreparedDatasetPiece);
    need = plus(plus(charged, bytes), plus(staging, actual));
    need = plus(need, context_storage(p));
    if (need > limit && p.reuse_contexts) { drop_contexts(p); context = SIZE_MAX; need = plus(plus(charged, bytes), plus(staging, actual)); }
    if (need > limit) {
        std::vector<detail::PreparedDatasetPiece>().swap(p.pieces); p.retained = false;
        drop_contexts(p); return false;
    }
    charged += bytes;
    if (!p.reuse_contexts) context = SIZE_MAX;
    p.pieces.push_back({s, column, nnz, context, std::move(read)});
    return true;
}
void reserve_piece_staging(Plan& p, Segment s, const Axis& cols, bool csc, std::size_t charged) {
    if (!p.retained) return;
    const auto staging = plus(selector_cost(*p.owner, cols, p.obs.is_gather() ? s.count : 0),
                              csc ? 2 * dtype_size(p.requirements.offset_dtype) : 0);
    const auto vector_bytes = p.pieces.capacity() * sizeof(detail::PreparedDatasetPiece);
    auto need = plus(charged, plus(context_storage(p), plus(vector_bytes, staging)));
    if (need > p.owner->selector_bytes && p.reuse_contexts) {
        drop_contexts(p); need = plus(charged, plus(vector_bytes, staging));
    }
    if (need > p.owner->selector_bytes) {
        std::vector<detail::PreparedDatasetPiece>().swap(p.pieces); p.retained = false;
        drop_contexts(p);
    }
}
PreparedRead prepare_piece(Plan& p, Segment s, const Axis& cols, std::size_t charged,
                           std::size_t& context) {
    context = SIZE_MAX;
    if (!p.retained || !p.reuse_contexts) return prepared_x(*p.owner, p.obs, cols, s);
    const auto limit = p.owner->selector_bytes;
    const auto staging = selector_cost(*p.owner, cols, p.obs.is_gather() ? s.count : 0);
    const auto pieces_capacity = p.pieces.capacity();
    const auto next_pieces = p.pieces.size() == pieces_capacity
        ? (pieces_capacity ? plus(pieces_capacity, pieces_capacity) : 1) : pieces_capacity;
    if (next_pieces > SIZE_MAX / sizeof(detail::PreparedDatasetPiece))
        fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "prepared piece capacity overflows");
    const auto piece_bytes = next_pieces * sizeof(detail::PreparedDatasetPiece);
    const auto old_piece_bytes = next_pieces == pieces_capacity ? 0 : pieces_capacity * sizeof(detail::PreparedDatasetPiece);
    std::size_t index = 0;
    while (index < p.contexts.size() && p.contexts[index].component != s.component) ++index;
    if (index == p.contexts.size()) {
        const auto capacity = p.contexts.capacity();
        const auto next = p.contexts.size() == capacity ? (capacity ? plus(capacity, capacity) : 1) : capacity;
        if (next > SIZE_MAX / sizeof(detail::PreparedDatasetContext))
            fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "prepared context capacity overflows");
        const auto bytes = next * sizeof(detail::PreparedDatasetContext);
        const auto old = next == capacity ? 0 : capacity * sizeof(detail::PreparedDatasetContext);
        const auto need = plus(plus(charged, p.context_bytes),
                               plus(staging, plus(plus(piece_bytes, old_piece_bytes), plus(bytes, old))));
        if (need > limit) return prepared_x(*p.owner, p.obs, cols, s);
        if (next != capacity) p.contexts.reserve(next);
        const auto actual = p.contexts.capacity() * sizeof(detail::PreparedDatasetContext);
        if (plus(plus(charged, p.context_bytes), plus(staging, plus(plus(piece_bytes, old_piece_bytes), actual))) > limit) {
            return prepared_x(*p.owner, p.obs, cols, s);
        }
        p.contexts.push_back({s.component, 0, false, {}});
    }
    auto& slot = p.contexts[index];
    if (slot.rejected) return prepared_x(*p.owner, p.obs, cols, s);
    auto used = plus(charged, plus(context_storage(p), plus(staging, plus(piece_bytes, old_piece_bytes))));
    if (used < slot.bytes || used - slot.bytes > limit) {
        reject_context(p, index); return prepared_x(*p.owner, p.obs, cols, s);
    }
    const auto budget = limit - (used - slot.bytes);
    PreparedRead read;
    const bool admitted = detail::prepare_backed_x_read(p.owner->files[s.component],
        segment_rows(*p.owner, p.obs, s), axis_selection(cols, p.owner->n_vars), slot.transfer, budget, read);
    if (!admitted) {
        if (read.requirements().layout != Layout::csr) drop_contexts(p);
        else reject_context(p, index);
        return read;
    }
    const auto bytes = detail::backed_read_context_bytes(slot.transfer);
    if (bytes > budget) { reject_context(p, index); return read; }
    if (p.context_bytes < slot.bytes)
        fail(ErrorCode::invalid_argument, "AnnDataSet/X", "prepared context accounting is inconsistent");
    p.context_bytes = plus(p.context_bytes - slot.bytes, bytes); slot.bytes = bytes;
    context = index;
    return read;
}
template<class F> void walk_pieces(const Plan& p, F&& f, const bool* keep = nullptr) {
    const auto& owner = *p.owner;
    const bool csc = p.requirements.layout == Layout::csc;
    const auto offset_width = csc ? dtype_size(p.requirements.offset_dtype) : 0;
    auto walk = [&](auto&& self, Segment s, const Axis& cols, std::size_t column) -> void {
        if (keep && !*keep) return;
        const auto selection = selector_cost(owner, cols, p.obs.is_gather() ? s.count : 0);
        const auto ob = csc ? plus(offset_width, offset_width) : 0;
        if (selection > owner.selector_bytes || ob > owner.selector_bytes - selection) {
            if (s.count <= 1) fail(ErrorCode::invalid_argument, "AnnDataSet/X", "sparse offset and selector staging exceeds budget");
            auto count = s.count / 2;
            self(self, {s.first, count, s.component}, cols, column);
            self(self, {s.first + count, s.count - count, s.component}, cols, column);
            return;
        }
        f(s, cols, column);
    };
    if (!csc) for_segments(owner, p.obs, p.var, [&](Segment s) { walk(walk, s, p.var, SIZE_MAX); });
    else for (std::size_t j = 0; j < p.requirements.shape[1] && (!keep || *keep); ++j) {
        auto col = Axis::stride(p.var.index(j), 1);
        for_segments(owner, p.obs, col, [&](Segment s) { walk(walk, s, col, j); });
    }
}
} // namespace

AnnDataSetView::AnnDataSetView() = default;
AnnDataSetView::AnnDataSetView(std::shared_ptr<Impl> p, BackedAnnDataView a, Axis obs, Axis var)
    : impl_(std::move(p)), annotations_(std::move(a)), obs_(std::move(obs)), var_(std::move(var)) {}
std::size_t AnnDataSetView::n_obs() const { check(impl_); return obs_.extent(impl_->n_obs); }
std::size_t AnnDataSetView::n_vars() const { check(impl_); return var_.extent(impl_->n_vars); }
std::size_t AnnDataSetView::n_components() const { check(impl_); return impl_->files.size(); }
bool AnnDataSetView::has_x() const { check(impl_); return impl_->has_x; }
std::vector<DatasetComponent> AnnDataSetView::components() const { check(impl_); return impl_->components; }
BackedAnnDataView AnnDataSetView::component(std::size_t i) const {
    check(impl_);
    if (i >= impl_->files.size()) fail(ErrorCode::out_of_bounds, "AnnDataSet/components", "component index is out of bounds");
    return impl_->files[i].view();
}
BackedAnnDataView AnnDataSetView::annotations() const { check(impl_); return annotations_; }
AnnDataSetView AnnDataSetView::slice(Selection obs, Selection var) const {
    check(impl_);
    auto o = resolve(obs, n_obs(), annotations_.obs(), "AnnDataSet/obs");
    auto v = resolve(var, n_vars(), annotations_.var(), "AnnDataSet/var");
    return {impl_, annotations_.slice(std::move(obs), std::move(var)),
            detail::compose_axis(obs_, o, impl_->n_obs), detail::compose_axis(var_, v, impl_->n_vars)};
}
PreparedAnnDataSetRead::PreparedAnnDataSetRead() = default;
PreparedAnnDataSetRead::~PreparedAnnDataSetRead() = default;
PreparedAnnDataSetRead::PreparedAnnDataSetRead(PreparedAnnDataSetRead&&) noexcept = default;
PreparedAnnDataSetRead& PreparedAnnDataSetRead::operator=(PreparedAnnDataSetRead&&) noexcept = default;
PreparedAnnDataSetRead::PreparedAnnDataSetRead(std::unique_ptr<Plan> p) : impl_(std::move(p)) {}

PreparedAnnDataSetRead AnnDataSetView::prepare_x() const {
    check(impl_);
    (void)annotations_.n_obs();
    if (!impl_->has_x) fail(ErrorCode::invalid_argument, "AnnDataSet/X", "X is absent");
    if (impl_->check_axis_names) check_selection_indexes(*impl_, obs_, var_);
    auto p = std::make_unique<Plan>();
    p->owner = impl_; p->annotations = annotations_; p->obs = obs_; p->var = var_;
    auto& out = p->requirements;
    out.shape = {n_obs(), n_vars()};
    p->retained = true;
    bool selected = false, reference_selected = false;
    std::size_t charged = 0;
    for_segments(*impl_, obs_, var_, [&](const Segment& s) {
        reserve_piece_staging(*p, s, var_, false, charged);
        std::size_t context;
        auto read = prepare_piece(*p, s, var_, charged, context);
        auto r = read.requirements();
        if (!selected) {
            out.layout = r.layout; out.dtype = r.dtype;
            out.index_dtype = r.index_dtype; out.offset_dtype = r.offset_dtype;
            selected = true;
        }
        if (s.component == 0) reference_selected = true;
        if (r.layout != out.layout) fail(ErrorCode::unsupported_layout, "AnnDataSet/X", "component matrix layouts differ");
        if (r.dtype != out.dtype) fail(ErrorCode::dtype_mismatch, "AnnDataSet/X", "component matrix dtypes differ");
        if (r.layout != Layout::dense) {
            if (r.index_dtype != out.index_dtype) out.index_dtype = DType::int64;
            if (r.offset_dtype != out.offset_dtype) out.offset_dtype = DType::int64;
            out.nnz = plus(out.nnz, r.nnz);
        }
        if (out.layout != Layout::csc) retain_piece(*p, s, SIZE_MAX, r.nnz, context, std::move(read), charged);
    });
    if (!reference_selected) {
        auto reference = detail::backed_x_metadata(impl_->files.front());
        if (!selected) {
            out.layout = reference.layout; out.dtype = reference.dtype;
            out.index_dtype = reference.index_dtype; out.offset_dtype = reference.offset_dtype;
        } else {
            if (reference.layout != out.layout) fail(ErrorCode::unsupported_layout, "AnnDataSet/X", "component matrix layouts differ");
            if (reference.dtype != out.dtype) fail(ErrorCode::dtype_mismatch, "AnnDataSet/X", "component matrix dtypes differ");
            if (out.layout != Layout::dense) {
                if (reference.index_dtype != out.index_dtype) out.index_dtype = DType::int64;
                if (reference.offset_dtype != out.offset_dtype) out.offset_dtype = DType::int64;
            }
        }
    }
    if (out.layout != Layout::dense) {
        auto inner = out.layout == Layout::csr ? n_vars() : n_obs();
        if (inner && inner - 1 > integer_max(out.index_dtype)) out.index_dtype = DType::int64;
        if (out.nnz > integer_max(out.offset_dtype)) out.offset_dtype = DType::int64;
        if ((inner && inner - 1 > integer_max(out.index_dtype)) || out.nnz > integer_max(out.offset_dtype))
            fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "sparse output exceeds supported integer widths");
    }
    if (out.layout == Layout::csc && p->retained) {
        std::size_t nnz = 0;
        walk_pieces(*p, [&](Segment s, const Axis& cols, std::size_t column) {
            if (!p->retained) return;
            reserve_piece_staging(*p, s, cols, true, charged);
            if (!p->retained) return;
            auto read = prepared_x(*impl_, obs_, cols, s);
            const auto count = read.requirements().nnz;
            nnz = plus(nnz, count);
            retain_piece(*p, s, column, count, SIZE_MAX, std::move(read), charged);
        }, &p->retained);
        if (p->retained && nnz != out.nnz)
            fail(ErrorCode::invalid_sparse_structure, "AnnDataSet/X", "component values changed during preparation");
    }
    for (const auto& context : p->contexts) if (!context.rejected) {
        p->next = impl_->live_context_plans;
        if (p->next) p->next->previous = p.get();
        impl_->live_context_plans = p.get(); p->linked = true;
        break;
    }
    return PreparedAnnDataSetRead(std::move(p));
}
ReadRequirements PreparedAnnDataSetRead::requirements() const { check_plan(impl_.get()); return impl_->requirements; }
ReadRequirements AnnDataSetView::requirements() const { return prepare_x().requirements(); }
void AnnDataSetView::read_x_into(const DenseBuffer& dst) const { prepare_x().read_into(dst); }
void AnnDataSetView::read_x_into(const SparseBuffer& dst) const { prepare_x().read_into(dst); }

AnnDataSet::AnnDataSet() = default;
AnnDataSet::AnnDataSet(std::shared_ptr<Impl> p) : impl_(std::move(p)) {}
AnnDataSet::~AnnDataSet() = default;
AnnDataSet::AnnDataSet(AnnDataSet&&) noexcept = default;
AnnDataSet& AnnDataSet::operator=(AnnDataSet&&) noexcept = default;
bool AnnDataSet::is_open() const noexcept { return impl_ && impl_->open && impl_->manifest.is_open(); }
std::string AnnDataSet::location() const { check(impl_); return impl_->manifest.location(); }
std::size_t AnnDataSet::n_obs() const { check(impl_); return impl_->n_obs; }
std::size_t AnnDataSet::n_vars() const { check(impl_); return impl_->n_vars; }
std::size_t AnnDataSet::n_components() const { check(impl_); return impl_->files.size(); }
bool AnnDataSet::has_x() const { check(impl_); return impl_->has_x; }
AnnDataSetView AnnDataSet::view() const { check(impl_); return {impl_, impl_->manifest.view(), Axis::all(), Axis::all()}; }
AnnDataSetView AnnDataSet::slice(Selection obs, Selection var) const { return view().slice(std::move(obs), std::move(var)); }
void AnnDataSet::set_annotation(std::string path, Element value) {
    check(impl_);
    if (path == "/uns" || path == "/uns/AnnDataSet" || path.starts_with("/uns/AnnDataSet/") ||
        path == "/obs" || path == "/var" || path == "/obs/_index" || path == "/var/_index" ||
        path == "/obs/" + impl_->manifest.obs().index_name() || path == "/var/" + impl_->manifest.var().index_name() ||
        path == "/X" || path == "/")
        fail(ErrorCode::invalid_argument, path, "collection structure or reference table cannot be replaced through annotations");
    impl_->manifest.set(std::move(path), std::move(value));
}
void AnnDataSet::validate() const {
    check(impl_);
    auto vars = impl_->manifest.var();
    if (vars.size() != impl_->n_vars || impl_->manifest.n_obs() != impl_->n_obs)
        fail(ErrorCode::shape_mismatch, "AnnDataSet", "manifest axes differ from component axes");
    std::size_t rows = 0;
    auto obs = impl_->manifest.obs();
    for (const auto& file : impl_->files) {
        if (!file.is_open()) fail(ErrorCode::invalid_argument, "AnnDataSet/components", "component is closed");
        compare_indexes(vars, file.var(), "AnnDataSet/var", "component variable names differ");
        if (file.has_x() != impl_->has_x) fail(ErrorCode::shape_mismatch, "AnnDataSet/X", "components disagree on X presence");
        auto source = file.obs();
        if (rows > impl_->n_obs || source.size() > impl_->n_obs - rows)
            fail(ErrorCode::shape_mismatch, "AnnDataSet/obs", "component row counts differ from manifest");
        compare_indexes(obs.select(axis_selection(Axis::stride(rows, source.size()), impl_->n_obs)), source,
                        "AnnDataSet/obs", "component observation names differ from manifest");
        rows = plus(rows, file.n_obs());
    }
    if (rows != impl_->n_obs) fail(ErrorCode::shape_mismatch, "AnnDataSet/obs", "component row counts differ from manifest");
}
void AnnDataSet::flush() { check(impl_); impl_->manifest.flush(); }
void AnnDataSet::close() {
    if (!impl_ || !impl_->open) return;
    impl_->open = false;
    for (auto* plan = impl_->live_context_plans; plan; plan = plan->next) drop_contexts(*plan);
    std::exception_ptr error;
    for (auto& f : impl_->files) try { f.close(); } catch (...) { if (!error) error = std::current_exception(); }
    try { impl_->manifest.close(); } catch (...) { if (!error) error = std::current_exception(); }
    if (error) std::rethrow_exception(error);
}

namespace {
template<class F> void each_prepared(const Plan& p, F&& f) {
    if (p.retained) {
        for (const auto& piece : p.pieces) {
            if (p.reuse_contexts && piece.context != SIZE_MAX && piece.context >= p.contexts.size())
                fail(ErrorCode::invalid_argument, "AnnDataSet/X", "prepared context is missing");
            auto* context = p.reuse_contexts && piece.context != SIZE_MAX && !p.contexts[piece.context].rejected
                ? &p.contexts[piece.context].transfer : nullptr;
            f(piece.segment, piece.column, piece.read, context, &piece.nnz);
        }
    } else {
        walk_pieces(p, [&](Segment s, const Axis& cols, std::size_t column) {
            auto read = prepared_x(*p.owner, p.obs, cols, s);
            f(s, column, read, nullptr, nullptr);
        });
    }
}
template<class T> void add_to_integers(void* data, std::size_t n, std::size_t value) {
    auto* bytes = static_cast<std::byte*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        T x; std::memcpy(&x, bytes + i * sizeof(T), sizeof(T));
        x = T(std::uint64_t(x) + value);
        std::memcpy(bytes + i * sizeof(T), &x, sizeof(T));
    }
}
void add_to_integers(void* data, DType type, std::size_t n, std::size_t value) {
    switch (type) {
    case DType::int8: return add_to_integers<std::int8_t>(data,n,value);
    case DType::uint8: return add_to_integers<std::uint8_t>(data,n,value);
    case DType::int16: return add_to_integers<std::int16_t>(data,n,value);
    case DType::uint16: return add_to_integers<std::uint16_t>(data,n,value);
    case DType::int32: return add_to_integers<std::int32_t>(data,n,value);
    case DType::uint32: return add_to_integers<std::uint32_t>(data,n,value);
    case DType::int64: return add_to_integers<std::int64_t>(data,n,value);
    case DType::uint64: return add_to_integers<std::uint64_t>(data,n,value);
    default: fail(ErrorCode::dtype_mismatch, "AnnDataSet/X", "sparse index dtype must be an integer");
    }
}
void dense_plan(const Plan& p, const DenseBuffer& dst, bool transfer) {
    const auto& r = p.requirements;
    if (r.layout != Layout::dense) fail(ErrorCode::unsupported_layout, "AnnDataSet/X", "X is not dense");
    checked_shape(dst, r.shape[0], r.shape[1], r.dtype);
    const auto stride = row_stride(dst, r.shape[1]);
    if (r.shape[0]) (void)displacement(r.shape[0] - 1, stride);
    each_prepared(p, [&](Segment s, std::size_t, const PreparedRead& read, TransferContext*, const std::size_t*) {
        auto target = dst; target.shape = {s.count, r.shape[1]};
        if (target.data) target.data = static_cast<std::byte*>(target.data) + displacement(s.first, stride);
        if (transfer) read.read_into(target); else read.preflight(target);
    });
}
void sparse_plan(const Plan& p, const SparseBuffer& dst, bool transfer) {
    const auto& r = p.requirements;
    if (r.layout == Layout::dense) fail(ErrorCode::unsupported_layout, "AnnDataSet/X", "X is not sparse");
    if (dst.dtype != r.dtype || dst.index_dtype != r.index_dtype || dst.offset_dtype != r.offset_dtype)
        fail(ErrorCode::dtype_mismatch, "AnnDataSet/X", "destination dtypes differ from requirements");
    const auto major = r.layout == Layout::csr ? r.shape[0] : r.shape[1];
    if (dst.values_capacity < r.nnz || dst.indices_capacity < r.nnz || dst.offsets_capacity < plus(major, 1))
        fail(ErrorCode::shape_mismatch, "AnnDataSet/X", "sparse destination capacity is too small");
    if ((r.nnz && (!dst.values || !dst.indices)) || !dst.offsets)
        fail(ErrorCode::invalid_argument, "AnnDataSet/X", "sparse destination buffer is null");
    const auto vw = dtype_size(r.dtype), iw = dtype_size(r.index_dtype), ow = dtype_size(r.offset_dtype);
    if (r.nnz && (vw > SIZE_MAX / r.nnz || iw > SIZE_MAX / r.nnz))
        fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "sparse destination byte extent overflows");
    if (major && ow > SIZE_MAX / plus(major, 1))
        fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "offset destination byte extent overflows");
    const auto limit = std::size_t(PTRDIFF_MAX);
    if (r.nnz * vw > limit || r.nnz * iw > limit || plus(major, 1) * ow > limit)
        fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "sparse destination byte span exceeds signed range");
    if (!transfer && p.reuse_contexts)
        for (const auto& context : p.contexts)
            if (!context.rejected) detail::refresh_backed_read_context(context.transfer);
    const bool csc = r.layout == Layout::csc;
    std::size_t written = 0, previous_column = 0;
    std::array<std::byte, 16> offsets{};
    if (transfer) write_integer(dst.offsets, r.offset_dtype, 0, 0);
    each_prepared(p, [&](Segment s, std::size_t column, const PreparedRead& read,
                         TransferContext* context, const std::size_t* cached_nnz) {
        std::size_t part_nnz;
        if (context) part_nnz = *cached_nnz;
        else {
            auto part = read.requirements();
            if (part.layout != r.layout || part.dtype != r.dtype)
                fail(ErrorCode::invalid_sparse_structure, "AnnDataSet/X", "component changed after preparation");
            part_nnz = part.nnz;
        }
        if (written > r.nnz || part_nnz > r.nnz - written)
            fail(ErrorCode::invalid_sparse_structure, "AnnDataSet/X", "component changed after preparation");
        if (transfer && csc) while (previous_column < column)
            write_integer(dst.offsets, r.offset_dtype, ++previous_column, written);
        auto* values = dst.values ? static_cast<std::byte*>(dst.values) + written * vw : nullptr;
        auto* indices = dst.indices ? static_cast<std::byte*>(dst.indices) + written * iw : nullptr;
        auto* out = csc ? offsets.data() : static_cast<std::byte*>(dst.offsets) + s.first * ow;
        SparseBuffer b{values, indices, out, r.dtype, dst.index_dtype, dst.offset_dtype,
                       part_nnz, part_nnz, (csc ? 1 : s.count) + 1};
        if (!transfer) {
            if (context) detail::preflight_backed_read(*context, read, b);
            else read.preflight(b);
        }
        else {
            if (context) detail::read_preflighted_backed(*context, read, b);
            else read.read_into(b);
            if (csc) add_to_integers(indices, r.index_dtype, part_nnz, s.first);
            else add_to_integers(out, r.offset_dtype, s.count + 1, written);
        }
        written = plus(written, part_nnz);
    });
    if (written != r.nnz) fail(ErrorCode::invalid_sparse_structure, "AnnDataSet/X", "component values changed after preparation");
    if (transfer && csc) while (previous_column < major)
        write_integer(dst.offsets, r.offset_dtype, ++previous_column, written);
}
}
void PreparedAnnDataSetRead::preflight(const DenseBuffer& dst) const { check_plan(impl_.get()); dense_plan(*impl_, dst, false); }
void PreparedAnnDataSetRead::preflight(const SparseBuffer& dst) const { check_plan(impl_.get()); sparse_plan(*impl_, dst, false); }
void PreparedAnnDataSetRead::read_into(const DenseBuffer& dst) const { preflight(dst); dense_plan(*impl_, dst, true); }
void PreparedAnnDataSetRead::read_into(const SparseBuffer& dst) const { preflight(dst); sparse_plan(*impl_, dst, true); }

AnnData AnnDataSetView::materialize() const {
    check(impl_);
    auto data = annotations_.read();
    if (!impl_->has_x) return data;
    auto prepared = prepare_x();
    auto r = prepared.requirements();
    if (r.layout == Layout::dense) {
        auto out = allocate(r.dtype, r.shape);
        prepared.read_into({out.edit().bytes().data(), r.dtype, r.shape, {}});
        data.set_x(std::move(out));
    } else {
        auto values = allocate(r.dtype, {r.nnz});
        auto indices = allocate(r.index_dtype, {r.nnz});
        auto offsets = allocate(r.offset_dtype, {plus(r.layout == Layout::csr ? n_obs() : n_vars(), 1)});
        prepared.read_into({values.edit().bytes().data(), indices.edit().bytes().data(), offsets.edit().bytes().data(),
                     r.dtype, r.index_dtype, r.offset_dtype, r.nnz, r.nnz, offsets.extent(0)});
        data.set_x(Array::from_sparse_buffers(r.layout, std::move(values), std::move(indices), std::move(offsets),
                                             n_obs(), n_vars()));
    }
    return data;
}

AnnDataSet open_h5ads(std::string_view path, DatasetOptions options) {
#if ANNDATAPP_HAS_HDF5
    auto name = path_text(path);
    auto p = std::make_shared<Impl>();
    p->check_axis_names = options.check_axis_names;
    // Keep the access spelling in the open handle and pin the path used for byte copies.
    std::error_code ec;
    p->manifest_path = fs::absolute(name, ec);
    if (ec) fail(ErrorCode::io_error, name, "cannot resolve path: " + ec.message());
    p->manifest_identity = manifest_identity(p->manifest_path);
    p->manifest = open_manifest(name, options.backed);
    check_manifest_identity(*p);
    p->n_obs = p->manifest.n_obs(); p->n_vars = p->manifest.n_vars();
    p->components = manifest_components(p->manifest);
    const auto& manifest_path = p->manifest_path;
    auto parent = manifest_path.parent_path();
    std::unordered_map<std::string, std::string> overrides;
    for (const auto& o : options.relocate) {
        if (o.key.empty() || !overrides.emplace(o.key, path_text(o.path)).second)
            fail(ErrorCode::invalid_argument, "AnnDataSet/relocate", "relocation keys must be nonempty and unique");
    }
    p->offsets.push_back(0);
    auto child_options = options.backed;
    child_options.mode = AccessMode::read_only;
    // Reserve a portion of the advertised scratch budget for collection-owned
    // grouping, positional conversion and sparse index-width conversion.
    p->selector_bytes = options.backed.scratch_bytes >= 128 ? options.backed.scratch_bytes / 4 : 0;
    child_options.scratch_bytes -= p->selector_bytes;
    for (auto& c : p->components) {
        auto it = overrides.find(c.key);
        if (it != overrides.end()) { c.path = it->second; overrides.erase(it); }
        auto resolved = absolute_path(c.path, parent);
        if (same_path(resolved, manifest_path))
            fail(ErrorCode::invalid_argument, c.key, "component path refers to the collection manifest");
        try { p->files.push_back(open_h5ad(resolved.string(), child_options)); }
        catch (const Error& e) { throw Error(e.code(), resolved.string(), "component '" + c.key + "': " + e.what()); }
        p->offsets.push_back(plus(p->offsets.back(), p->files.back().n_obs()));
    }
    if (!overrides.empty()) fail(ErrorCode::invalid_argument, "AnnDataSet/relocate", "override refers to an unknown component key");
    p->has_x = p->files.front().has_x();
    if (p->offsets.back() != p->n_obs)
        fail(ErrorCode::shape_mismatch, "AnnDataSet/obs", "component row counts differ from manifest");
    for (const auto& file : p->files) {
        if (file.n_vars() != p->n_vars)
            fail(ErrorCode::shape_mismatch, "AnnDataSet/var", "component variable counts differ from manifest");
        if (file.has_x() != p->has_x)
            fail(ErrorCode::shape_mismatch, "AnnDataSet/X", "components disagree on X presence");
    }
    return AnnDataSet(std::move(p));
#else
    (void)options;
    fail(ErrorCode::unsupported_encoding, path, "HDF5 support is disabled");
#endif
}

AnnDataSet create_h5ads(std::string_view path, std::vector<DatasetComponent> components,
                        std::string_view sample_column, DatasetOptions options) {
#if ANNDATAPP_HAS_HDF5
    auto name = path_text(path);
    if (sample_column.empty() || sample_column.find('\0') != std::string_view::npos)
        fail(ErrorCode::invalid_argument, "AnnDataSet/sample", "sample column must be nonempty");
    if (components.empty()) fail(ErrorCode::invalid_argument, "AnnDataSet", "collection needs at least one component");
    if (!options.relocate.empty()) fail(ErrorCode::invalid_argument, "AnnDataSet/relocate", "relocation overrides apply when opening an existing manifest");
    auto dest = absolute_path(name, fs::current_path());
    auto parent = dest.parent_path();
    std::unordered_set<std::string> keys;
    std::vector<std::string> obs_names, vars, samples;
    bool have_vars = false;
    auto child_options = options.backed;
    child_options.mode = AccessMode::read_only;
    bool first_x = false, have_x = false;
    for (const auto& c : components) {
        if (c.key.empty() || c.key.find('\0') != std::string::npos || !keys.insert(c.key).second)
            fail(ErrorCode::invalid_argument, "AnnDataSet/keys", "component keys must be unique, nonempty strings");
        auto resolved = absolute_path(path_text(c.path), parent);
        if (same_path(resolved, dest)) fail(ErrorCode::invalid_argument, c.key, "component path refers to output manifest");
        BackedAnnData child;
        try { child = open_h5ad(resolved.string(), child_options); }
        catch (const Error& e) { throw Error(e.code(), resolved.string(), "component '" + c.key + "': " + e.what()); }
        auto child_vars = child.var().names();
        if (!have_vars) { vars = std::move(child_vars); have_vars = true; }
        else if (vars != child_vars) fail(ErrorCode::shape_mismatch, c.key, "component variable names differ");
        if (!have_x) { first_x = child.has_x(); have_x = true; }
        else if (first_x != child.has_x()) fail(ErrorCode::shape_mismatch, c.key, "components disagree on X presence");
        auto child_obs = child.obs().names();
        if (child_obs.size() > SIZE_MAX - obs_names.size()) fail(ErrorCode::out_of_bounds, c.key, "observation count overflows");
        samples.insert(samples.end(), child_obs.size(), c.key);
        obs_names.insert(obs_names.end(), child_obs.begin(), child_obs.end());
        child.close();
    }
    auto obs = Table::with_index(std::move(obs_names));
    obs.set(std::string(sample_column), Column::strings(std::move(samples)));
    auto annotation = detail::make_anndata(std::move(obs), Table::with_index(std::move(vars)));
    annotation.set_uns("AnnDataSet", Element(component_table(components)));
    WriteOptions write;
    write.mode = WriteMode::exclusive;
    write.validation = options.backed.validation;
    write_h5ad(name, annotation, write);
    return open_h5ads(name, std::move(options));
#else
    (void)components; (void)sample_column; (void)options;
    fail(ErrorCode::unsupported_encoding, path, "HDF5 support is disabled");
#endif
}

void AnnDataSet::copy_manifest(std::string_view destination, bool overwrite) const {
#if ANNDATAPP_HAS_HDF5
    check(impl_);
    auto output = path_text(destination);
    const auto& source = impl_->manifest_path;
    check_manifest_identity(*impl_);
    auto dest = absolute_path(output, fs::current_path());
    if (same_path(source, dest)) fail(ErrorCode::invalid_argument, output, "cannot overwrite the open collection manifest");
    std::vector<std::string> paths;
    paths.reserve(impl_->files.size());
    for (const auto& file : impl_->files) {
        const fs::path component = file.location();
        if (same_path(component, dest)) fail(ErrorCode::invalid_argument, output, "cannot overwrite a linked component");
        auto relative = component.lexically_relative(dest.parent_path());
        paths.push_back(relative.empty() ? component.string() : relative.string());
    }
    // SnapATAC2 writes index_type and other backend attributes that are not
    // AnnData encodings. Preserve them byte-for-byte instead of asking the
    // generic cross-store exporter to interpret or silently discard them.
    if (impl_->manifest.writable()) impl_->manifest.flush();
    std::error_code ec;
    auto mode = overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none;
    if (!fs::copy_file(source, dest, mode, ec) || ec)
        fail(ErrorCode::io_error, output, "cannot copy collection manifest: " + ec.message());
    BackedOptions options;
    options.mode = AccessMode::read_write;
    auto copy = open_manifest(dest.string(), options);
    // Replace only the path column; preserve the table index, column order,
    // other columns and their backend-specific metadata in the byte copy.
    copy.set("/uns/AnnDataSet/file_path", Element(Column::strings(std::move(paths))));
    copy.flush();
    copy.close();
#else
    (void)overwrite;
    fail(ErrorCode::unsupported_encoding, destination, "HDF5 support is disabled");
#endif
}

} // namespace anndatapp

