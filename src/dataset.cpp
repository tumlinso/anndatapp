module;
#include <algorithm>
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
    std::vector<DatasetComponent> components;
    std::vector<BackedAnnData> files;
    std::vector<std::size_t> offsets;
    std::size_t n_obs = 0, n_vars = 0;
    bool open = true, has_x = false;
    std::size_t selector_bytes = 0;
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
    out.reserve(table.size());
    std::unordered_set<std::string> seen;
    for (std::size_t i = 0; i < table.size(); ++i) {
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
struct Segment { std::size_t first, count, component; };
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
BackedArray selected_x(const Impl& p, const Axis& rows, const Axis& cols, const Segment& s) {
    return p.files[s.component].x().select({segment_rows(p, rows, s), axis_selection(cols, p.n_vars)});
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
    std::vector<bool> checked_vars(p.files.size(), false);
    for_segments(p, rows, cols, [&](const Segment& s) {
        const auto& file = p.files[s.component];
        if (!file.is_open()) fail(ErrorCode::invalid_argument, "AnnDataSet/components", "component is closed");
        if (!checked_vars[s.component]) {
            compare_indexes(p.manifest.var().select(axis_selection(cols, p.n_vars)),
                            file.var().select(axis_selection(cols, p.n_vars)),
                            "AnnDataSet/var", "component variable names differ");
            checked_vars[s.component] = true;
        }
        auto selected = detail::compose_axis(rows, Axis::stride(s.first, s.count), p.n_obs);
        compare_indexes(p.manifest.obs().select(axis_selection(selected, p.n_obs)),
                        file.obs().select(segment_rows(p, rows, s)),
                        "AnnDataSet/obs", "component observation names differ from manifest");
    });
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
template<class T> std::uint64_t load_integer(const void* data, std::size_t i) {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(data) + i * sizeof(T), sizeof(T));
    if constexpr (std::is_signed_v<T>) if (value < 0)
        fail(ErrorCode::invalid_sparse_structure, "AnnDataSet/X", "negative sparse index or offset");
    return std::uint64_t(value);
}
std::uint64_t read_integer(const void* data, DType t, std::size_t i) {
    switch (t) {
    case DType::int8: return load_integer<std::int8_t>(data, i); case DType::uint8: return load_integer<std::uint8_t>(data, i);
    case DType::int16: return load_integer<std::int16_t>(data, i); case DType::uint16: return load_integer<std::uint16_t>(data, i);
    case DType::int32: return load_integer<std::int32_t>(data, i); case DType::uint32: return load_integer<std::uint32_t>(data, i);
    case DType::int64: return load_integer<std::int64_t>(data, i); case DType::uint64: return load_integer<std::uint64_t>(data, i);
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
ReadRequirements AnnDataSetView::requirements() const {
    check(impl_);
    if (!impl_->has_x) fail(ErrorCode::invalid_argument, "AnnDataSet/X", "X is absent");
    check_selection_indexes(*impl_, obs_, var_);
    auto first = impl_->files.front().x();
    ReadRequirements out;
    out.layout = first.layout(); out.dtype = first.dtype(); out.shape = {n_obs(), n_vars()};
    if (out.layout != Layout::dense) { out.index_dtype = first.index_dtype(); out.offset_dtype = first.offset_dtype(); }
    for_segments(*impl_, obs_, var_, [&](const Segment& s) {
        auto r = selected_x(*impl_, obs_, var_, s).prepare_read().requirements();
        if (r.layout != out.layout) fail(ErrorCode::unsupported_layout, "AnnDataSet/X", "component matrix layouts differ");
        if (r.dtype != out.dtype) fail(ErrorCode::dtype_mismatch, "AnnDataSet/X", "component matrix dtypes differ");
        if (r.layout != Layout::dense) {
            if (r.index_dtype != out.index_dtype) out.index_dtype = DType::int64;
            if (r.offset_dtype != out.offset_dtype) out.offset_dtype = DType::int64;
            out.nnz = plus(out.nnz, r.nnz);
        }
    });
    if (out.layout != Layout::dense) {
        auto inner = out.layout == Layout::csr ? n_vars() : n_obs();
        if (inner && inner - 1 > integer_max(out.index_dtype)) out.index_dtype = DType::int64;
        if (out.nnz > integer_max(out.offset_dtype)) out.offset_dtype = DType::int64;
    }
    return out;
}
void AnnDataSetView::read_x_into(const DenseBuffer& dst) const {
    auto r = requirements();
    if (r.layout != Layout::dense) fail(ErrorCode::unsupported_layout, "AnnDataSet/X", "X is not dense");
    checked_shape(dst, n_obs(), n_vars(), r.dtype);
    const auto stride = row_stride(dst, n_vars());
    if (n_obs()) (void)displacement(n_obs() - 1, stride);
    auto target_for = [&](const Segment& s) {
        auto target = dst;
        target.shape = {s.count, n_vars()};
        if (!target.strides.empty()) target.strides = dst.strides;
        if (target.data) target.data = static_cast<std::byte*>(target.data) + displacement(s.first, stride);
        return target;
    };
    for_segments(*impl_, obs_, var_, [&](const Segment& s) {
        selected_x(*impl_, obs_, var_, s).prepare_read().preflight(target_for(s));
    });
    for_segments(*impl_, obs_, var_, [&](const Segment& s) {
        selected_x(*impl_, obs_, var_, s).read_into(target_for(s));
    });
}

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
    std::exception_ptr error;
    for (auto& f : impl_->files) try { f.close(); } catch (...) { if (!error) error = std::current_exception(); }
    try { impl_->manifest.close(); } catch (...) { if (!error) error = std::current_exception(); }
    if (error) std::rethrow_exception(error);
}

void AnnDataSetView::read_x_into(const SparseBuffer& dst) const {
    auto r = requirements();
    if (r.layout == Layout::dense) fail(ErrorCode::unsupported_layout, "AnnDataSet/X", "X is not sparse");
    if (dst.dtype != r.dtype || dst.index_dtype != r.index_dtype || dst.offset_dtype != r.offset_dtype)
        fail(ErrorCode::dtype_mismatch, "AnnDataSet/X", "destination dtypes differ from requirements");
    const auto major = r.layout == Layout::csr ? n_obs() : n_vars();
    if (dst.values_capacity < r.nnz || dst.indices_capacity < r.nnz || dst.offsets_capacity < plus(major, 1))
        fail(ErrorCode::shape_mismatch, "AnnDataSet/X", "sparse destination capacity is too small");
    if ((r.nnz && (!dst.values || !dst.indices)) || !dst.offsets)
        fail(ErrorCode::invalid_argument, "AnnDataSet/X", "sparse destination buffer is null");
    const auto value_width = dtype_size(r.dtype), index_width = dtype_size(r.index_dtype), offset_width = dtype_size(r.offset_dtype);
    if (r.nnz && (value_width > SIZE_MAX / r.nnz || index_width > SIZE_MAX / r.nnz))
        fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "sparse destination byte extent overflows");
    if (major && offset_width > SIZE_MAX / plus(major, 1))
        fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "offset destination byte extent overflows");
    std::size_t written = 0;
    bool preflight = true;

    // Transfer into the caller's promoted widths. CSC needs two temporary
    // offsets per component segment; oversized selections split by rows.
    auto read_piece = [&](auto&& self, Segment s, const Axis& cols, bool by_column) -> void {
        auto prepared = selected_x(*impl_, obs_, cols, s).prepare_read();
        auto part = prepared.requirements();
        if (part.layout != r.layout || part.dtype != r.dtype)
            fail(ErrorCode::invalid_sparse_structure, "AnnDataSet/X", "component layout or dtype changed after requirements were computed");
        if (written > r.nnz || part.nnz > r.nnz - written)
            fail(ErrorCode::invalid_sparse_structure, "AnnDataSet/X", "component values grew after requirements were computed");
        const auto part_major = by_column ? std::size_t{1} : s.count;
        if (offset_width > SIZE_MAX / plus(part_major, 1))
            fail(ErrorCode::out_of_bounds, "AnnDataSet/X", "sparse staging extent overflows");
        const auto ob = by_column ? (part_major + 1) * offset_width : 0;
        const auto selection = selector_cost(*impl_, cols, obs_.is_gather() ? s.count : 0);
        if (selection > impl_->selector_bytes || ob > impl_->selector_bytes - selection) {
            if (s.count <= 1)
                fail(ErrorCode::invalid_argument, "AnnDataSet/X", "sparse offset staging needs " +
                     std::to_string(plus(selection, ob)) + " bytes; budget is " + std::to_string(impl_->selector_bytes));
            auto count = s.count / 2;
            self(self, {s.first, count, s.component}, cols, by_column);
            self(self, {s.first + count, s.count - count, s.component}, cols, by_column);
            return;
        }
        auto* value_out = dst.values ? static_cast<std::byte*>(dst.values) + written * value_width : nullptr;
        auto* index_out = dst.indices ? static_cast<std::byte*>(dst.indices) + written * index_width : nullptr;
        auto* offset_out = by_column ? nullptr : static_cast<std::byte*>(dst.offsets) + s.first * offset_width;
        std::byte placeholder{};
        std::vector<std::byte> offsets(preflight ? 0 : ob);
        void* offset_in = by_column ? (preflight ? static_cast<void*>(&placeholder) : static_cast<void*>(offsets.data())) : offset_out;
        // BackedArray accepts destination integer widths independently of the
        // on-disk widths. Use the promoted output widths during the transfer:
        // duplicate selections can exceed a component's physical widths.
        SparseBuffer b{value_out, index_out, offset_in, part.dtype, dst.index_dtype, dst.offset_dtype,
                       part.nnz, part.nnz, part_major + 1};
        if (preflight) { prepared.preflight(b); written = plus(written, part.nnz); return; }
        prepared.read_into(b);
        if (by_column) for (std::size_t i = 0; i < part.nnz; ++i)
            write_integer(index_out, dst.index_dtype, i,
                          plus(std::size_t(read_integer(index_out, dst.index_dtype, i)), s.first));
        if (!by_column) for (std::size_t i = 0; i <= part_major; ++i)
            write_integer(dst.offsets, dst.offset_dtype, s.first + i,
                          plus(written, std::size_t(read_integer(offset_in, dst.offset_dtype, i))));
        written = plus(written, part.nnz);
    };
    auto run = [&] {
        if (r.layout == Layout::csr) {
            for_segments(*impl_, obs_, var_, [&](Segment s) { read_piece(read_piece, s, var_, false); });
        } else {
            for (std::size_t j = 0; j < n_vars(); ++j) {
                auto selected = Axis::stride(var_.index(j), 1);
                for_segments(*impl_, obs_, selected, [&](Segment s) { read_piece(read_piece, s, selected, true); });
                if (!preflight) write_integer(dst.offsets, dst.offset_dtype, j + 1, written);
            }
        }
        if (written != r.nnz)
            fail(ErrorCode::invalid_sparse_structure, "AnnDataSet/X", "component values changed after requirements were computed");
    };
    run(); // Check all staging capacities before touching caller buffers.
    written = 0; preflight = false;
    write_integer(dst.offsets, r.offset_dtype, 0, 0);
    run();
}

AnnData AnnDataSetView::materialize() const {
    check(impl_);
    auto data = annotations_.read();
    if (!impl_->has_x) return data;
    auto r = requirements();
    if (r.layout == Layout::dense) {
        auto out = allocate(r.dtype, r.shape);
        read_x_into({out.edit().bytes().data(), r.dtype, r.shape, {}});
        data.set_x(std::move(out));
    } else {
        auto values = allocate(r.dtype, {r.nnz});
        auto indices = allocate(r.index_dtype, {r.nnz});
        auto offsets = allocate(r.offset_dtype, {plus(r.layout == Layout::csr ? n_obs() : n_vars(), 1)});
        read_x_into({values.edit().bytes().data(), indices.edit().bytes().data(), offsets.edit().bytes().data(),
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
    p->manifest = open_manifest(name, options.backed);
    p->n_obs = p->manifest.n_obs(); p->n_vars = p->manifest.n_vars();
    p->components = manifest_components(p->manifest);
    auto manifest_path = absolute_path(name, fs::current_path());
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
    auto annotation = AnnData::empty(obs_names.size(), vars.size());
    auto obs = Table::with_index(std::move(obs_names));
    obs.set(std::string(sample_column), Column::strings(std::move(samples)));
    annotation.set_obs(std::move(obs));
    annotation.set_var(Table::with_index(std::move(vars)));
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
    auto source = absolute_path(location(), fs::current_path());
    auto dest = absolute_path(output, fs::current_path());
    if (same_path(source, dest)) fail(ErrorCode::invalid_argument, output, "cannot overwrite the open collection manifest");
    std::vector<DatasetComponent> rebased;
    rebased.reserve(impl_->components.size());
    for (const auto& c : impl_->components) {
        auto component = absolute_path(c.path, source.parent_path());
        if (same_path(component, dest)) fail(ErrorCode::invalid_argument, output, "cannot overwrite a linked component");
        auto relative = component.lexically_relative(dest.parent_path());
        rebased.push_back({c.key, relative.empty() ? component.string() : relative.string()});
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
    auto copy = open_manifest(output, options);
    std::vector<std::string> paths;
    paths.reserve(rebased.size());
    for (const auto& c : rebased) paths.push_back(c.path);
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

