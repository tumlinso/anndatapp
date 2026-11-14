module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <array>
#include <fstream>
#include <type_traits>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module anndatapp.mudata;
#if ANNDATAPP_HAS_HDF5
import anndatapp.h5ad;
#endif
#if ANNDATAPP_HAS_ZARR
import anndatapp.zarr;
#endif

namespace anndatapp {
namespace detail {
struct MuModality {
    std::string name;
    AnnData data;
    std::shared_ptr<const std::vector<std::uint64_t>> obs, var;
};
struct MuModalityView {
    std::string name;
    AnnDataView data;
    std::shared_ptr<const std::vector<std::uint64_t>> obs, var;
};
struct MuDataImpl {
    MuAxis axis = MuAxis::observations;
    AnnData annotations;
    std::vector<MuModality> modalities;
};
struct MuDataViewImpl {
    MuAxis axis = MuAxis::observations;
    AnnDataView annotations;
    std::vector<MuModalityView> modalities;
};
}
namespace {
[[noreturn]] void fail(ErrorCode c, std::string_view path, std::string message) {
    throw Error(c, std::string(path), std::move(message));
}
void check_axis(MuAxis axis) {
    if (axis != MuAxis::observations && axis != MuAxis::variables && axis != MuAxis::both)
        fail(ErrorCode::invalid_argument, "axis", "invalid MuData shared axis");
}
void check_name(std::string_view name) {
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string_view::npos || name.find('\0') != std::string_view::npos)
        fail(ErrorCode::invalid_argument, "mod", "modality name must be nonempty and contain no slash or NUL");
}
template<class V> auto find_modality(V& modes, std::string_view name) {
    return std::find_if(modes.begin(), modes.end(), [&](const auto& m) { return m.name == name; });
}
template<class V> auto& modality_at(V& modes, std::string_view name) {
    auto p = find_modality(modes, name);
    if (p == modes.end()) fail(ErrorCode::out_of_bounds, "mod/" + std::string(name), "modality does not exist");
    return *p;
}
void check_map(MuMapView map, std::size_t global, std::size_t local,
               std::string_view path, std::size_t scratch_bytes = 65536) {
    if (map.size != global) fail(ErrorCode::shape_mismatch, path, "map length differs from global axis");
    if (map.size && !map.data) fail(ErrorCode::invalid_argument, path, "map storage is null");
    for (std::size_t i = 0; i < map.size; ++i)
        if (map.data[i] > local) fail(ErrorCode::out_of_bounds, path, "map position exceeds modality axis");
    // Validate coverage in bounded windows. Non-injective maps are legal, so
    // counts alone cannot establish that every local position is represented.
    const auto bytes = std::min<std::size_t>(scratch_bytes, 65536);
    if (local && !bytes) fail(ErrorCode::invalid_argument, path, "scratch budget cannot validate map coverage");
    for (std::size_t first = 0; first < local;) {
        const auto count = std::min(local - first, bytes * 8);
        std::vector<std::uint8_t> seen((count + 7) / 8, 0);
        for (std::size_t i = 0; i < map.size; ++i) {
            const auto value = map.data[i];
            if (value && value - 1 >= first && value - 1 - first < count) {
                const auto p = std::size_t(value - 1 - first);
                seen[p / 8] |= std::uint8_t(1u << (p % 8));
            }
        }
        for (std::size_t i = 0; i < count; ++i)
            if (!(seen[i / 8] & (1u << (i % 8))))
                fail(ErrorCode::shape_mismatch, path, "map does not cover modality axis");
        first += count;
    }
}
void check_map(const std::vector<std::uint64_t>& map, std::size_t global,
               std::size_t local, std::string_view path, std::size_t scratch_bytes = 65536) {
    check_map({map.data(), map.size()}, global, local, path, scratch_bytes);
}
void check_annotations(const AnnData& a) {
    a.validate();
    if (a.has_x() || a.has_raw() || !a.layer_names().empty())
        fail(ErrorCode::invalid_argument, "mudata", "global annotations cannot contain X, layers or raw");
}
std::vector<std::uint64_t> project_map(const std::vector<std::uint64_t>& source,
                                       const Axis& selected, std::vector<std::ptrdiff_t>& local,
                                       std::string_view path) {
    const auto n = selected.extent(source.size());
    std::vector<std::uint64_t> out(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const auto v = source[selected.index(i)];
        if (!v) continue;
        if (v - 1 > std::uint64_t(std::numeric_limits<std::ptrdiff_t>::max()))
            fail(ErrorCode::out_of_bounds, path, "modality position exceeds signed selection range");
        local.push_back(std::ptrdiff_t(v - 1));
        out[i] = local.size();
    }
    return out;
}
}

MuData::MuData() : impl_(std::make_unique<detail::MuDataImpl>()) {}
MuData::~MuData() = default;
MuData::MuData(MuData&&) noexcept = default;
MuData& MuData::operator=(MuData&&) noexcept = default;
MuData MuData::empty(std::size_t n_obs, std::size_t n_vars, MuAxis axis) {
    check_axis(axis);
    MuData m;
    m.impl_->axis = axis;
    m.impl_->annotations = AnnData::empty(n_obs, n_vars);
    return m;
}
MuAxis MuData::axis() const { return impl_->axis; }
AnnData& MuData::annotations() { return impl_->annotations; }
const AnnData& MuData::annotations() const { return impl_->annotations; }
AnnData& MuData::edit_modality(std::string_view name) { return modality_at(impl_->modalities, name).data; }
void MuData::set_maps(std::string_view name, std::vector<std::uint64_t> obs,
                      std::vector<std::uint64_t> var) {
    auto& m = modality_at(impl_->modalities, name);
    check_map(obs, impl_->annotations.n_obs(), m.data.n_obs(), "obsmap/" + m.name);
    check_map(var, impl_->annotations.n_vars(), m.data.n_vars(), "varmap/" + m.name);
    auto o = std::make_shared<const std::vector<std::uint64_t>>(std::move(obs));
    auto v = std::make_shared<const std::vector<std::uint64_t>>(std::move(var));
    m.obs = std::move(o); m.var = std::move(v);
}
void MuData::set_modality(std::string name, AnnData value, std::vector<std::uint64_t> obs,
                          std::vector<std::uint64_t> var) {
    check_name(name);
    value.validate();
    check_map(obs, impl_->annotations.n_obs(), value.n_obs(), "obsmap/" + name);
    check_map(var, impl_->annotations.n_vars(), value.n_vars(), "varmap/" + name);
    auto p = find_modality(impl_->modalities, name);
    auto o = std::make_shared<const std::vector<std::uint64_t>>(std::move(obs));
    auto v = std::make_shared<const std::vector<std::uint64_t>>(std::move(var));
    detail::MuModality m{std::move(name), std::move(value), std::move(o), std::move(v)};
    if (p == impl_->modalities.end()) impl_->modalities.push_back(std::move(m));
    else *p = std::move(m);
}
void MuData::erase_modality(std::string_view name) {
    auto p = find_modality(impl_->modalities, name);
    if (p == impl_->modalities.end()) fail(ErrorCode::out_of_bounds, "mod/" + std::string(name), "modality does not exist");
    impl_->modalities.erase(p);
}
MuDataView MuData::view() const {
    auto v = std::make_shared<detail::MuDataViewImpl>();
    v->axis = impl_->axis;
    v->annotations = impl_->annotations.view();
    v->modalities.reserve(impl_->modalities.size());
    for (const auto& m : impl_->modalities)
        v->modalities.push_back({m.name, m.data.view(), m.obs, m.var});
    return MuDataView(std::move(v));
}
MuDataView MuData::slice(Selection obs, Selection var) const { return view().slice(std::move(obs), std::move(var)); }
MuData MuData::copy() const { return view().copy(); }
void MuData::validate() const {
    check_axis(impl_->axis);
    check_annotations(impl_->annotations);
    for (const auto& m : impl_->modalities) {
        check_name(m.name);
        m.data.validate();
        check_map(*m.obs, impl_->annotations.n_obs(), m.data.n_obs(), "obsmap/" + m.name);
        check_map(*m.var, impl_->annotations.n_vars(), m.data.n_vars(), "varmap/" + m.name);
    }
}

MuDataView::MuDataView() = default;
MuDataView::MuDataView(std::shared_ptr<const detail::MuDataViewImpl> p) : impl_(std::move(p)) {}
MuAxis MuDataView::axis() const { return impl_ ? impl_->axis : MuAxis::observations; }
std::size_t MuDataView::n_obs() const { return impl_ ? impl_->annotations.n_obs() : 0; }
std::size_t MuDataView::n_vars() const { return impl_ ? impl_->annotations.n_vars() : 0; }
AnnDataView MuDataView::annotations() const { return impl_ ? impl_->annotations : AnnDataView{}; }
std::vector<std::string> MuDataView::modality_names() const {
    std::vector<std::string> names;
    if (impl_) for (const auto& m : impl_->modalities) names.push_back(m.name);
    return names;
}
AnnDataView MuDataView::modality(std::string_view name) const {
    if (!impl_) fail(ErrorCode::out_of_bounds, "mod/" + std::string(name), "modality does not exist");
    return modality_at(impl_->modalities, name).data;
}
MuMapView MuDataView::obs_map(std::string_view name) const {
    if (!impl_) fail(ErrorCode::out_of_bounds, "mod/" + std::string(name), "modality does not exist");
    const auto& m = *modality_at(impl_->modalities, name).obs;
    return {m.data(), m.size()};
}
MuMapView MuDataView::var_map(std::string_view name) const {
    if (!impl_) fail(ErrorCode::out_of_bounds, "mod/" + std::string(name), "modality does not exist");
    const auto& m = *modality_at(impl_->modalities, name).var;
    return {m.data(), m.size()};
}
MuDataView MuDataView::slice(Selection obs, Selection var) const {
    if (!impl_) return {};
    const auto rows = obs.resolve(impl_->annotations.obs(), "obs");
    const auto cols = var.resolve(impl_->annotations.var(), "var");
    auto v = std::make_shared<detail::MuDataViewImpl>();
    v->axis = impl_->axis;
    v->annotations = impl_->annotations.slice(obs, var);
    v->modalities.reserve(impl_->modalities.size());
    for (const auto& m : impl_->modalities) {
        std::vector<std::ptrdiff_t> orows, vrows;
        auto om = project_map(*m.obs, rows, orows, "obsmap/" + m.name);
        auto vm = project_map(*m.var, cols, vrows, "varmap/" + m.name);
        auto selected = m.data.slice(indices(std::move(orows)), indices(std::move(vrows)));
        auto o = std::make_shared<const std::vector<std::uint64_t>>(std::move(om));
        auto vmap = std::make_shared<const std::vector<std::uint64_t>>(std::move(vm));
        v->modalities.push_back({m.name, std::move(selected), std::move(o), std::move(vmap)});
    }
    return MuDataView(std::move(v));
}
MuData MuDataView::copy() const {
    auto m = MuData::empty(n_obs(), n_vars(), axis());
    if (!impl_) return m;
    m.annotations() = impl_->annotations.copy();
    for (const auto& x : impl_->modalities) m.set_modality(x.name, x.data.copy(), *x.obs, *x.var);
    return m;
}

namespace detail {
struct BackedMuDataImpl {
    BackedStore store;
    MuAxis axis = MuAxis::observations;
    std::vector<std::string> names;
};
struct BackedMuModalityView {
    std::string name;
    BackedAnnDataView data;
    std::vector<std::uint64_t> obs, var;
};
struct BackedMuDataViewImpl {
    std::shared_ptr<BackedMuDataImpl> owner;
    BackedTable obs, var;
    std::vector<BackedMuModalityView> modalities;
    std::vector<::anndatapp::Selection> obs_chain, var_chain;
};
}
namespace {
std::string child_path(std::string_view parent, std::string_view name) {
    check_name(name);
    return std::string(parent) + "/" + std::string(name);
}
MuAxis decode_axis(ElementView attrs) {
    if (!attrs.contains("axis")) return MuAxis::observations;
    auto a = attrs.at("axis");
    std::int64_t v;
    if (a.kind() == ElementKind::signed_integer) v = a.signed_integer();
    else if (a.kind() == ElementKind::unsigned_integer && a.unsigned_integer() <= 1)
        v = std::int64_t(a.unsigned_integer());
    else fail(ErrorCode::unsupported_encoding, "/", "MuData axis must be -1, 0 or 1");
    if (v < -1 || v > 1) fail(ErrorCode::unsupported_encoding, "/", "MuData axis must be -1, 0 or 1");
    return MuAxis(int(v));
}
std::vector<std::string> string_values(ElementView v) {
    std::vector<std::string> out;
    if (v.kind() == ElementKind::string_array) {
        auto a = v.string_array();
        if (a.rank() != 1) fail(ErrorCode::shape_mismatch, "/mod", "mod-order must be one dimensional");
        for (std::size_t i = 0; i < a.extent(0); ++i) out.emplace_back(a.string(i));
    } else if (v.kind() == ElementKind::column && v.column().kind() == ColumnKind::string) {
        auto a = v.column();
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!a.valid(i)) fail(ErrorCode::unsupported_encoding, "/mod", "mod-order cannot contain nulls");
            out.emplace_back(a.string(i));
        }
    } else if (v.kind() == ElementKind::sequence) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            auto x = v.at(i);
            if (x.kind() != ElementKind::string) fail(ErrorCode::unsupported_encoding, "/mod", "mod-order must contain strings");
            out.emplace_back(x.string());
        }
    } else fail(ErrorCode::unsupported_encoding, "/mod", "mod-order must be a string array");
    return out;
}
std::vector<std::string> modality_order(const BackedStore& store) {
    auto names = store.children("/mod");
    std::sort(names.begin(), names.end());
    auto attrs = store.attributes("/mod");
    if (!attrs.view().contains("mod-order")) return names;
    auto order = string_values(attrs.view().at("mod-order"));
    auto a = names, b = order;
    std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
    return a == b ? order : names;
}
void check_root(const BackedStore& store) {
    auto attrs = store.attributes("/");
    auto a = attrs.view();
    if (!a.contains("encoding-type") || a.at("encoding-type").kind() != ElementKind::string ||
        a.at("encoding-type").string() != "MuData")
        fail(ErrorCode::unsupported_encoding, "/", "expected MuData root encoding");
    if (!a.contains("encoding-version") || a.at("encoding-version").kind() != ElementKind::string ||
        a.at("encoding-version").string() != "0.1.0")
        fail(ErrorCode::unsupported_version, "/", "unsupported MuData root version");
    for (auto path : {"/obs", "/var", "/mod", "/obsmap", "/varmap"})
        if (!store.exists(path)) fail(ErrorCode::unsupported_encoding, path, "required MuData element is missing");
}
void check_materializable_root(const BackedStore& store) {
    auto attrs = store.attributes("/");
    if (attrs.view().contains("encoding-subtype"))
        fail(ErrorCode::unsupported_encoding, "/", "MuData root subtype is unsupported");
    for (const auto& name : store.children("/")) {
        if (name == "obs" || name == "var" || name == "obsm" || name == "varm" ||
            name == "obsp" || name == "varp" || name == "uns" || name == "obsmap" ||
            name == "varmap" || name == "mod") continue;
        fail(ErrorCode::unsupported_encoding, "/" + name, "unrecognized MuData root element cannot be materialized");
    }
}
void check_modality(const BackedStore& store, std::string_view name) {
    auto path = child_path("/mod", name);
    auto attrs = store.attributes(path);
    auto a = attrs.view();
    if (a.contains("encoding-subtype"))
        fail(ErrorCode::unsupported_encoding, path, "MuData modality subtype is unsupported");
    if (!a.contains("encoding-type") || a.at("encoding-type").kind() != ElementKind::string ||
        a.at("encoding-type").string() != "anndata")
        fail(ErrorCode::unsupported_encoding, path, "MuData modality must be AnnData");
}
template<class T> std::vector<std::uint64_t> map_values_t(ArrayView a, std::string_view path) {
    auto values = a.dense<T>();
    const auto n = a.extent(0);
    std::vector<std::uint64_t> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        if constexpr (std::is_signed_v<T>)
            if (values.data()[i] < 0) fail(ErrorCode::out_of_bounds, path, "map position is negative");
        out[i] = std::uint64_t(values.data()[i]);
    }
    return out;
}
std::vector<std::uint64_t> decode_map(Array a, std::string_view path) {
    auto v = a.view();
    if (v.layout() != Layout::dense || (v.rank() != 1 && (v.rank() != 2 || v.extent(1) != 1)))
        fail(ErrorCode::shape_mismatch, path, "MuData map must have shape (n,) or (n,1)");
    switch (v.dtype()) {
    case DType::uint8: return map_values_t<std::uint8_t>(v, path);
    case DType::uint16: return map_values_t<std::uint16_t>(v, path);
    case DType::uint32: return map_values_t<std::uint32_t>(v, path);
    case DType::uint64: return map_values_t<std::uint64_t>(v, path);
    case DType::int8: return map_values_t<std::int8_t>(v, path);
    case DType::int16: return map_values_t<std::int16_t>(v, path);
    case DType::int32: return map_values_t<std::int32_t>(v, path);
    case DType::int64: return map_values_t<std::int64_t>(v, path);
    default: fail(ErrorCode::unsupported_dtype, path, "MuData map must have integer dtype");
    }
}
std::vector<std::uint64_t> read_map(const BackedStore& store, std::string_view path) {
    return decode_map(store.array(path).read(), path);
}
std::vector<std::uint64_t> read_selected_map(const BackedStore& store, std::string_view path,
                                              Selection rows, std::size_t extent) {
    auto a = store.array(path);
    if (a.rank() != 1 && (a.rank() != 2 || a.extent(1) != 1))
        fail(ErrorCode::shape_mismatch, path, "MuData map must have shape (n,) or (n,1)");
    if (a.extent(0) != extent) fail(ErrorCode::shape_mismatch, path, "map length differs from global axis");
    std::vector<Selection> selectors;
    selectors.push_back(std::move(rows));
    if (a.rank() == 2) selectors.emplace_back(all);
    return decode_map(a.select(std::move(selectors)).read(), path);
}
void read_aligned(AnnData& a, const BackedStore& store, std::string_view root, bool obs) {
    if (!store.exists(root)) return;
    auto names = store.children(root);
    for (const auto& name : names) {
        auto e = store.element(child_path(root, name));
        if (e.kind() == ElementKind::array) {
            auto x = e.array().read();
            if (obs) a.set_obsm(name, std::move(x)); else a.set_varm(name, std::move(x));
        } else if (e.kind() == ElementKind::string_array) {
            auto x = e.string_array().read();
            if (obs) a.set_obsm(name, std::move(x)); else a.set_varm(name, std::move(x));
        } else if (e.kind() == ElementKind::table) {
            auto x = e.table().read();
            if (obs) a.set_obsm(name, std::move(x)); else a.set_varm(name, std::move(x));
        } else fail(ErrorCode::unsupported_encoding, child_path(root, name), "unsupported aligned MuData annotation");
    }
}
void read_pairwise(AnnData& a, const BackedStore& store, std::string_view root, bool obs) {
    if (!store.exists(root)) return;
    for (const auto& name : store.children(root)) {
        auto x = store.element(child_path(root, name)).array().read();
        if (obs) a.set_obsp(name, std::move(x)); else a.set_varp(name, std::move(x));
    }
}
Element strings_attribute(const std::vector<std::string>& names) {
    return Element(StringArray::from_strings({names.size()}, names));
}
void write_map(BackedStore& store, std::string_view path, MuMapView map) {
    auto attrs = Element::mapping();
    attrs.set("encoding-type", Element("array"));
    attrs.set("encoding-version", Element("0.2.0"));
    // MuData permits integer map encodings. Borrow the existing native u64
    // values: the generic transfer uses bounded scratch and does not cast or
    // allocate a second full map. The view outlives this synchronous write.
    auto values = Array::borrow_dense(map.data, {map.size}, {1}, Validation::none);
    store.write_array(path, values.view(), attrs.view());
}
void check_view(const MuDataView& m, std::size_t scratch_bytes) {
    check_axis(m.axis());
    const auto a = m.annotations();
    if (a.has_x() || a.has_raw() || !a.layer_names().empty())
        fail(ErrorCode::invalid_argument, "mudata", "global annotations cannot contain X, layers or raw");
    for (const auto& name : m.modality_names()) {
        check_name(name);
        auto mod = m.modality(name);
        auto om = m.obs_map(name), vm = m.var_map(name);
        check_map(om, m.n_obs(), mod.n_obs(), "obsmap/" + name, scratch_bytes);
        check_map(vm, m.n_vars(), mod.n_vars(), "varmap/" + name, scratch_bytes);
    }
}
// Reject indivisible strings before opening or truncating a destination. The
// ordinary writer performs the same check per element, after creating a store.
void check_string_budget(std::string_view value, std::size_t budget, std::size_t metadata,
                         std::string_view path) {
    if (metadata >= budget || value.size() > budget - metadata - 1)
        fail(ErrorCode::invalid_argument, path, "one string value exceeds the available scratch buffer");
}
struct MuBudgets { std::size_t strings; std::size_t packing; };
void check_array_budget(ArrayView a, MuBudgets budget) {
    if (a.layout() == Layout::dense) {
        bool empty = false;
        for (std::size_t axis = 0; axis < a.rank(); ++axis) empty |= a.extent(axis) == 0;
        if (empty) return;
        bool contiguous = false;
        try { contiguous = a.bytes().contiguous(); }
        catch (const Error& error) { if (error.code() != ErrorCode::non_contiguous) throw; }
        if (!contiguous && budget.packing < dtype_size(a.dtype()))
            fail(ErrorCode::invalid_argument, "array", "scratch budget after backend workspace must hold one numerical element");
        return;
    }
    try { (void)a.sparse_bytes(); }
    catch (const Error& error) {
        if (error.code() != ErrorCode::non_contiguous) throw;
        // A selected sparse view requires one packed output and bounded index
        // staging. Probe that indivisible allocation before opening the store.
        (void)a.copy(Validation::none, budget.packing);
    }
}
void check_column_budget(ColumnView c, MuBudgets budget) {
    if (c.kind() == ColumnKind::categorical) { check_array_budget(c.codes(), budget); check_column_budget(c.categories(), budget); return; }
    if (c.kind() == ColumnKind::numeric) check_array_budget(c.values(), budget);
    if (c.kind() == ColumnKind::string)
        for (std::size_t i = 0; i < c.size(); ++i)
            check_string_budget(c.string(i), budget.strings, sizeof(std::string), "column");
}
void check_table_budget(TableView t, MuBudgets budget) {
    for (std::size_t i = 0; i < t.size(); ++i)
        check_string_budget(t.name(i), budget.strings, sizeof(std::string), "table index");
    for (const auto& name : t.column_names()) check_column_budget(t.column(name), budget);
}
void check_element_budget(ElementView e, MuBudgets budget) {
    switch (e.kind()) {
    case ElementKind::string:
        check_string_budget(e.string(), budget.strings, sizeof(std::string), "metadata"); break;
    case ElementKind::string_array: {
        auto a = e.string_array();
        for (std::size_t i = 0; i < a.size(); ++i)
            check_string_budget(a.string(i), budget.strings, sizeof(std::string), "string array");
        break;
    }
    case ElementKind::array: check_array_budget(e.array(), budget); break;
    case ElementKind::column: check_column_budget(e.column(), budget); break;
    case ElementKind::table: check_table_budget(e.table(), budget); break;
    case ElementKind::mapping:
        for (std::size_t i = 0; i < e.mapping_size(); ++i)
            check_element_budget(e.at(e.mapping_key(i)), budget);
        break;
    case ElementKind::record_array: {
        auto r = e.record_array();
        for (const auto& name : r.field_names()) {
            if (r.field(name).kind == RecordFieldKind::numeric) continue;
            auto shape = r.shape();
            const auto per = sizeof(std::string) + sizeof(char*);
            const auto cap = budget.strings > sizeof(std::vector<std::string>)
                ? std::max<std::size_t>(1, (budget.strings - sizeof(std::vector<std::string>)) / (2 * per)) : 1;
            std::vector<std::string> slots;
            slots.reserve(std::min(cap, shape.empty() ? r.size() : shape.back()));
            const auto metadata = sizeof(slots) + slots.capacity() * sizeof(std::string) + sizeof(char*);
            auto text = r.text(name);
            for (std::size_t i = 0; i < r.size(); ++i) check_string_budget(text.string(i), budget.strings, metadata, "record string");
        }
        break;
    }
    default: break;
    }
}
void check_anndata_budget(AnnDataView a, MuBudgets budget) {
    check_table_budget(a.obs(), budget);
    check_table_budget(a.var(), budget);
    check_element_budget(a.uns(), budget);
    if (a.has_x()) check_array_budget(a.x(), budget);
    for (const auto& name : a.layer_names()) check_array_budget(a.layer(name), budget);
    for (const auto& name : a.obsp_names()) check_array_budget(a.obsp(name), budget);
    for (const auto& name : a.varp_names()) check_array_budget(a.varp(name), budget);
    for (const auto& name : a.obsm_names()) check_element_budget(a.obsm(name), budget);
    for (const auto& name : a.varm_names()) check_element_budget(a.varm(name), budget);
    if (a.has_raw()) {
        check_table_budget(a.raw_var(), budget);
        if (a.has_raw_x()) check_array_budget(a.raw_x(), budget);
        for (const auto& name : a.raw_varm_names()) check_element_budget(a.raw_varm(name), budget);
    }
}
void check_mudata_budget(const MuDataView& m, MuBudgets budget) {
    check_anndata_budget(m.annotations(), budget);
    for (const auto& name : m.modality_names()) check_anndata_budget(m.modality(name), budget);
}
void write_mudata(BackedStore& store, const MuDataView& m, Validation validation) {
    const auto names = m.modality_names();
    // The AnnData writer walks existing views and transfers aligned payloads in
    // bounded blocks; its output supplies every global slot except MuData maps.
    store.write_anndata("/", m.annotations(), validation);
    if (store.exists("/layers")) store.erase("/layers");
    if (store.exists("/raw")) store.erase("/raw");
    auto attrs = Element::mapping();
    attrs.set("encoding-type", Element("MuData"));
    attrs.set("encoding-version", Element("0.1.0"));
    attrs.set("encoder", Element("anndatapp"));
    attrs.set("encoder-version", Element("1.0.0"));
    attrs.set("axis", Element(std::int64_t(int(m.axis()))));
    store.set_attributes("/", attrs.view());
    auto dict = Element::mapping();
    dict.set("encoding-type", Element("dict"));
    dict.set("encoding-version", Element("0.1.0"));
    store.create_group("/obsmap", dict.view());
    store.create_group("/varmap", dict.view());
    for (const auto& name : names) {
        write_map(store, child_path("/obsmap", name), m.obs_map(name));
        write_map(store, child_path("/varmap", name), m.var_map(name));
    }
    auto mod_attrs = Element::mapping();
    mod_attrs.set("mod-order", strings_attribute(names));
    store.create_group("/mod", mod_attrs.view());
    for (const auto& name : names) store.write_anndata(child_path("/mod", name), m.modality(name), validation);
    store.flush();
}
void write_magic(std::string_view path) {
    std::fstream f(std::string(path), std::ios::in | std::ios::out | std::ios::binary);
    if (!f) fail(ErrorCode::io_error, path, "cannot open H5MU user block");
    std::array<char, 512> block{};
    const std::string header = "MuData (format-version=0.1.0;creator=anndatapp;creator-version=1.0.0)";
    std::copy(header.begin(), header.end(), block.begin());
    f.write(block.data(), std::streamsize(block.size()));
    f.flush();
    if (!f) fail(ErrorCode::io_error, path, "cannot write H5MU user block");
}
}

BackedMuData::BackedMuData() = default;
BackedMuData::BackedMuData(std::shared_ptr<detail::BackedMuDataImpl> p) : impl_(std::move(p)) {}
BackedMuData::~BackedMuData() = default;
BackedMuData::BackedMuData(BackedMuData&&) noexcept = default;
BackedMuData& BackedMuData::operator=(BackedMuData&&) noexcept = default;
bool BackedMuData::is_open() const noexcept { return impl_ && impl_->store.is_open(); }
MuAxis BackedMuData::axis() const {
    if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed");
    return impl_->axis;
}
std::size_t BackedMuData::n_obs() const { return obs().size(); }
std::size_t BackedMuData::n_vars() const { return var().size(); }
std::vector<std::string> BackedMuData::modality_names() const {
    if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed");
    return impl_->names;
}
BackedTable BackedMuData::obs() const {
    if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed");
    return impl_->store.element("/obs").table();
}
BackedTable BackedMuData::var() const {
    if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed");
    return impl_->store.element("/var").table();
}
BackedElement BackedMuData::obsm(std::string_view name) const { if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed"); return impl_->store.element(child_path("/obsm", name)); }
BackedElement BackedMuData::varm(std::string_view name) const { if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed"); return impl_->store.element(child_path("/varm", name)); }
BackedArray BackedMuData::obsp(std::string_view name) const { if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed"); return impl_->store.element(child_path("/obsp", name)).array(); }
BackedArray BackedMuData::varp(std::string_view name) const { if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed"); return impl_->store.element(child_path("/varp", name)).array(); }
BackedElement BackedMuData::uns() const { if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed"); return impl_->store.element("/uns"); }
BackedArray BackedMuData::obs_map(std::string_view name) const { if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed"); return impl_->store.array(child_path("/obsmap", name)); }
BackedArray BackedMuData::var_map(std::string_view name) const { if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed"); return impl_->store.array(child_path("/varmap", name)); }
BackedAnnData BackedMuData::modality(std::string_view name) const {
    if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed");
    if (std::find(impl_->names.begin(), impl_->names.end(), name) == impl_->names.end())
        fail(ErrorCode::out_of_bounds, "mod/" + std::string(name), "modality does not exist");
    check_modality(impl_->store, name);
    return impl_->store.anndata(child_path("/mod", name));
}
void BackedMuData::set_maps(std::string_view name, std::vector<std::uint64_t> obs_values,
                            std::vector<std::uint64_t> var_values) {
    if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed");
    if (!impl_->store.writable()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is read-only");
    auto mod = modality(name);
    check_map(obs_values, n_obs(), mod.n_obs(), "obsmap/" + std::string(name));
    check_map(var_values, n_vars(), mod.n_vars(), "varmap/" + std::string(name));
    write_map(impl_->store, child_path("/obsmap", name), {obs_values.data(), obs_values.size()});
    write_map(impl_->store, child_path("/varmap", name), {var_values.data(), var_values.size()});
}
namespace {
Selection positional(const Axis& a, std::size_t extent, std::string_view path) {
    if (a.is_all()) return all;
    const auto count = a.extent(extent);
    if (!count) return indices({});
    if (!a.is_gather()) {
        const auto max = std::size_t(std::numeric_limits<std::ptrdiff_t>::max());
        if (a.first() > max || a.step() > max) fail(ErrorCode::out_of_bounds, path, "position exceeds signed selection range");
        const auto last = a.index(count - 1);
        if (last > max) fail(ErrorCode::out_of_bounds, path, "position exceeds signed selection range");
        auto stop = a.reverse() && !last ? std::optional<std::ptrdiff_t>{} :
            std::optional<std::ptrdiff_t>{std::ptrdiff_t(a.reverse() ? last - 1 : last + 1)};
        return range{std::ptrdiff_t(a.first()), stop, a.reverse() ? -std::ptrdiff_t(a.step()) : std::ptrdiff_t(a.step())};
    }
    std::vector<std::ptrdiff_t> positions;
    positions.reserve(a.extent(extent));
    for (std::size_t i = 0; i < a.extent(extent); ++i) {
        auto p = a.index(i);
        if (p > std::size_t(std::numeric_limits<std::ptrdiff_t>::max()))
            fail(ErrorCode::out_of_bounds, path, "position exceeds signed selection range");
        positions.push_back(std::ptrdiff_t(p));
    }
    return indices(std::move(positions));
}
Axis resolve_backed(const Selection& selection, const BackedTable& axis, std::string_view path) {
    if (selection.kind() == Selection::Kind::label || selection.kind() == Selection::Kind::labels)
        return selection.resolve(axis.names(), path);
    return selection.resolve(axis.size(), path);
}
void require_view(const std::shared_ptr<const detail::BackedMuDataViewImpl>& p) {
    if (!p || !p->owner || !p->owner->store.is_open())
        fail(ErrorCode::invalid_argument, "mudata", "backed MuData view is closed");
}
BackedArray select_array(BackedArray a, const std::vector<Selection>& selections,
                         bool pairwise, std::string_view path) {
    if (!a.rank() || (pairwise && a.rank() < 2))
        fail(ErrorCode::shape_mismatch, path, "aligned array has insufficient rank");
    for (const auto& s : selections) {
        std::vector<Selection> axes(a.rank(), Selection(all));
        axes[0] = s;
        if (pairwise) axes[1] = s;
        a = a.select(std::move(axes));
    }
    return a;
}
BackedStringArray select_strings(BackedStringArray a, const std::vector<Selection>& selections,
                                 std::string_view path) {
    if (a.shape().empty()) fail(ErrorCode::shape_mismatch, path, "aligned string array has insufficient rank");
    for (const auto& s : selections) {
        std::vector<Selection> axes(a.shape().size(), Selection(all));
        axes[0] = s;
        a = a.select(std::move(axes));
    }
    return a;
}
BackedTable select_table(BackedTable t, const std::vector<Selection>& selections) {
    for (const auto& s : selections) t = t.select(s);
    return t;
}
}
BackedMuDataView BackedMuData::view() const {
    if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed");
    auto out = std::make_shared<detail::BackedMuDataViewImpl>();
    out->owner = impl_; out->obs = obs(); out->var = var();
    for (const auto& name : impl_->names) {
        auto data = modality(name).view();
        auto o = read_map(impl_->store, child_path("/obsmap", name));
        auto v = read_map(impl_->store, child_path("/varmap", name));
        check_map(o, out->obs.size(), data.n_obs(), "obsmap/" + name);
        check_map(v, out->var.size(), data.n_vars(), "varmap/" + name);
        out->modalities.push_back({name, std::move(data), std::move(o), std::move(v)});
    }
    return BackedMuDataView(std::move(out));
}
BackedMuDataView BackedMuData::slice(Selection obs_selection, Selection var_selection) const {
    if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed");
    auto o = obs(), v = var();
    const auto rows = resolve_backed(obs_selection, o, "obs");
    const auto cols = resolve_backed(var_selection, v, "var");
    auto ro = positional(rows, o.size(), "obs"), rv = positional(cols, v.size(), "var");
    auto out = std::make_shared<detail::BackedMuDataViewImpl>();
    out->owner = impl_;
    out->obs = o.select(ro); out->var = v.select(rv);
    out->obs_chain.push_back(ro); out->var_chain.push_back(rv);
    for (const auto& name : impl_->names) {
        std::vector<std::ptrdiff_t> local_obs, local_var;
        auto om = read_selected_map(impl_->store, child_path("/obsmap", name), ro, o.size());
        auto vm = read_selected_map(impl_->store, child_path("/varmap", name), rv, v.size());
        auto selected_obs = project_map(om, Axis::all(), local_obs, "obsmap/" + name);
        auto selected_var = project_map(vm, Axis::all(), local_var, "varmap/" + name);
        auto data = modality(name).slice(indices(std::move(local_obs)), indices(std::move(local_var)));
        out->modalities.push_back({name, std::move(data), std::move(selected_obs), std::move(selected_var)});
    }
    return BackedMuDataView(std::move(out));
}
BackedMuDataView::BackedMuDataView() = default;
BackedMuDataView::BackedMuDataView(std::shared_ptr<const detail::BackedMuDataViewImpl> p) : impl_(std::move(p)) {}
MuAxis BackedMuDataView::axis() const { require_view(impl_); return impl_->owner->axis; }
std::size_t BackedMuDataView::n_obs() const { require_view(impl_); return impl_->obs.size(); }
std::size_t BackedMuDataView::n_vars() const { require_view(impl_); return impl_->var.size(); }
BackedTable BackedMuDataView::obs() const { require_view(impl_); return impl_->obs; }
BackedTable BackedMuDataView::var() const { require_view(impl_); return impl_->var; }
std::vector<std::string> BackedMuDataView::modality_names() const {
    require_view(impl_);
    std::vector<std::string> names;
    for (const auto& m : impl_->modalities) names.push_back(m.name);
    return names;
}
BackedAnnDataView BackedMuDataView::modality(std::string_view name) const {
    require_view(impl_); return modality_at(impl_->modalities, name).data;
}
MuMapView BackedMuDataView::obs_map(std::string_view name) const {
    require_view(impl_); const auto& m = modality_at(impl_->modalities, name).obs; return {m.data(), m.size()};
}
MuMapView BackedMuDataView::var_map(std::string_view name) const {
    require_view(impl_); const auto& m = modality_at(impl_->modalities, name).var; return {m.data(), m.size()};
}
BackedArray BackedMuDataView::obsm_array(std::string_view name) const {
    require_view(impl_); auto p = child_path("/obsm", name);
    return select_array(impl_->owner->store.element(p).array(), impl_->obs_chain, false, p);
}
BackedStringArray BackedMuDataView::obsm_strings(std::string_view name) const {
    require_view(impl_); auto p = child_path("/obsm", name);
    return select_strings(impl_->owner->store.strings(p), impl_->obs_chain, p);
}
BackedTable BackedMuDataView::obsm_table(std::string_view name) const {
    require_view(impl_); auto p = child_path("/obsm", name);
    return select_table(impl_->owner->store.element(p).table(), impl_->obs_chain);
}
BackedArray BackedMuDataView::varm_array(std::string_view name) const {
    require_view(impl_); auto p = child_path("/varm", name);
    return select_array(impl_->owner->store.element(p).array(), impl_->var_chain, false, p);
}
BackedStringArray BackedMuDataView::varm_strings(std::string_view name) const {
    require_view(impl_); auto p = child_path("/varm", name);
    return select_strings(impl_->owner->store.strings(p), impl_->var_chain, p);
}
BackedTable BackedMuDataView::varm_table(std::string_view name) const {
    require_view(impl_); auto p = child_path("/varm", name);
    return select_table(impl_->owner->store.element(p).table(), impl_->var_chain);
}
BackedArray BackedMuDataView::obsp(std::string_view name) const {
    require_view(impl_); auto p = child_path("/obsp", name);
    return select_array(impl_->owner->store.element(p).array(), impl_->obs_chain, true, p);
}
BackedArray BackedMuDataView::varp(std::string_view name) const {
    require_view(impl_); auto p = child_path("/varp", name);
    return select_array(impl_->owner->store.element(p).array(), impl_->var_chain, true, p);
}
BackedElement BackedMuDataView::uns() const { require_view(impl_); return impl_->owner->store.element("/uns"); }
BackedMuDataView BackedMuDataView::slice(Selection obs_selection, Selection var_selection) const {
    require_view(impl_);
    const auto rows = resolve_backed(obs_selection, impl_->obs, "obs");
    const auto cols = resolve_backed(var_selection, impl_->var, "var");
    auto ro = positional(rows, impl_->obs.size(), "obs"), rv = positional(cols, impl_->var.size(), "var");
    auto out = std::make_shared<detail::BackedMuDataViewImpl>();
    out->owner = impl_->owner;
    out->obs = impl_->obs.select(ro); out->var = impl_->var.select(rv);
    out->obs_chain = impl_->obs_chain; out->obs_chain.push_back(ro);
    out->var_chain = impl_->var_chain; out->var_chain.push_back(rv);
    for (const auto& m : impl_->modalities) {
        std::vector<std::ptrdiff_t> local_obs, local_var;
        auto om = project_map(m.obs, rows, local_obs, "obsmap/" + m.name);
        auto vm = project_map(m.var, cols, local_var, "varmap/" + m.name);
        auto data = m.data.slice(indices(std::move(local_obs)), indices(std::move(local_var)));
        out->modalities.push_back({m.name, std::move(data), std::move(om), std::move(vm)});
    }
    return BackedMuDataView(std::move(out));
}
MuData BackedMuDataView::read() const {
    require_view(impl_);
    check_materializable_root(impl_->owner->store);
    auto m = MuData::empty(n_obs(), n_vars(), axis());
    auto& a = m.annotations();
    a.set_obs(obs().read()); a.set_var(var().read());
    auto& store = impl_->owner->store;
    for (auto root : {"/obsm", "/varm"}) {
        if (!store.exists(root)) continue;
        bool on_obs = std::string_view(root) == "/obsm";
        for (const auto& name : store.children(root)) {
            auto e = store.element(child_path(root, name));
            if (e.kind() == ElementKind::array) {
                auto x = on_obs ? obsm_array(name).read() : varm_array(name).read();
                if (on_obs) a.set_obsm(name, std::move(x)); else a.set_varm(name, std::move(x));
            } else if (e.kind() == ElementKind::string_array) {
                auto x = on_obs ? obsm_strings(name).read() : varm_strings(name).read();
                if (on_obs) a.set_obsm(name, std::move(x)); else a.set_varm(name, std::move(x));
            } else if (e.kind() == ElementKind::table) {
                auto x = on_obs ? obsm_table(name).read() : varm_table(name).read();
                if (on_obs) a.set_obsm(name, std::move(x)); else a.set_varm(name, std::move(x));
            } else fail(ErrorCode::unsupported_encoding, child_path(root, name), "unsupported aligned MuData annotation");
        }
    }
    if (store.exists("/obsp")) for (const auto& name : store.children("/obsp")) a.set_obsp(name, obsp(name).read());
    if (store.exists("/varp")) for (const auto& name : store.children("/varp")) a.set_varp(name, varp(name).read());
    if (store.exists("/uns")) a.set_uns(uns().read());
    for (const auto& x : impl_->modalities) m.set_modality(x.name, x.data.read(), x.obs, x.var);
    return m;
}
MuData BackedMuData::read() const {
    if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed");
    check_materializable_root(impl_->store);
    auto m = MuData::empty(n_obs(), n_vars(), impl_->axis);
    auto& a = m.annotations();
    a.set_obs(obs().read()); a.set_var(var().read());
    read_aligned(a, impl_->store, "/obsm", true); read_aligned(a, impl_->store, "/varm", false);
    read_pairwise(a, impl_->store, "/obsp", true); read_pairwise(a, impl_->store, "/varp", false);
    if (impl_->store.exists("/uns")) a.set_uns(impl_->store.element("/uns").read());
    for (const auto& name : impl_->names) {
        auto o = read_map(impl_->store, child_path("/obsmap", name));
        auto v = read_map(impl_->store, child_path("/varmap", name));
        m.set_modality(name, modality(name).read(), std::move(o), std::move(v));
    }
    return m;
}
void BackedMuData::flush() { if (!is_open()) fail(ErrorCode::invalid_argument, "mudata", "backed MuData is closed"); impl_->store.flush(); }
void BackedMuData::close() { if (impl_) impl_->store.close(); }
BackedMuData open_h5mu(std::string_view path, BackedOptions options) {
#if ANNDATAPP_HAS_HDF5
    auto p = std::make_shared<detail::BackedMuDataImpl>();
    p->store = open_hdf5_store(path, options);
    check_root(p->store);
    auto attrs = p->store.attributes("/");
    p->axis = decode_axis(attrs.view());
    p->names = modality_order(p->store);
    return BackedMuData(std::move(p));
#else
    (void)options; fail(ErrorCode::unsupported_encoding, path, "HDF5 support is disabled");
#endif
}
BackedMuData open_mudata_zarr(std::string_view path, BackedOptions options) {
#if ANNDATAPP_HAS_ZARR
    auto p = std::make_shared<detail::BackedMuDataImpl>();
    p->store = open_zarr_store(path, options);
    check_root(p->store);
    auto attrs = p->store.attributes("/");
    p->axis = decode_axis(attrs.view());
    p->names = modality_order(p->store);
    return BackedMuData(std::move(p));
#else
    (void)options; fail(ErrorCode::unsupported_encoding, path, "Zarr support is disabled");
#endif
}
MuData read_h5mu(std::string_view path, BackedOptions options) { return open_h5mu(path, options).read(); }
MuData read_mudata_zarr(std::string_view path, BackedOptions options) { return open_mudata_zarr(path, options).read(); }
void write_h5mu(std::string_view path, const MuDataView& data, MuWriteOptions options) {
#if ANNDATAPP_HAS_HDF5
    if (options.scratch_bytes < 64) fail(ErrorCode::invalid_argument, path, "scratch budget is too small");
    if (options.validation == Validation::full) check_view(data, options.scratch_bytes);
    check_mudata_budget(data, {options.scratch_bytes, options.scratch_bytes});
    WriteOptions w;
    w.validation = options.validation;
    w.mode = options.overwrite ? WriteMode::truncate : WriteMode::exclusive;
    auto store = create_hdf5_store(path, w, 512, options.scratch_bytes);
    write_mudata(store, data, options.validation);
    store.close();
    write_magic(path);
#else
    (void)data; (void)options; fail(ErrorCode::unsupported_encoding, path, "HDF5 support is disabled");
#endif
}
void write_h5mu(std::string_view path, const MuData& data, MuWriteOptions options) {
    write_h5mu(path, data.view(), options);
}
void write_mudata_zarr(std::string_view path, const MuDataView& data, MuWriteOptions options) {
#if ANNDATAPP_HAS_ZARR
    if (options.scratch_bytes < 64) fail(ErrorCode::invalid_argument, path, "scratch budget is too small");
    if (options.validation == Validation::full) check_view(data, options.scratch_bytes);
    check_mudata_budget(data, {options.scratch_bytes, options.scratch_bytes - (options.scratch_bytes / 8) * 7});
    ZarrWriteOptions w;
    w.validation = options.validation;
    w.mode = options.overwrite ? ZarrWriteMode::truncate : ZarrWriteMode::exclusive;
    w.scratch_bytes = options.scratch_bytes;
    auto store = create_zarr_store(path, w);
    write_mudata(store, data, options.validation);
    store.close();
#else
    (void)data; (void)options; fail(ErrorCode::unsupported_encoding, path, "Zarr support is disabled");
#endif
}
void write_mudata_zarr(std::string_view path, const MuData& data, MuWriteOptions options) {
    write_mudata_zarr(path, data.view(), options);
}

} // namespace anndatapp
