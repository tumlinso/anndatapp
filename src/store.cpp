module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

module anndatapp.backed;
import :storage;

namespace anndatapp::backed_detail {
namespace {
[[noreturn]] void fail(ErrorCode code, std::string_view path, std::string message) {
    throw Error(code, std::string(path), std::move(message));
}
std::string checked_path(std::string_view path) {
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string_view::npos ||
        (path.size() > 1 && path.back() == '/'))
        fail(ErrorCode::invalid_argument, path, "expected an absolute store path without trailing slash or NUL");
    for (std::size_t first = 1; first < path.size();) {
        auto end = path.find('/', first);
        if (end == std::string_view::npos) end = path.size();
        const auto part = path.substr(first, end - first);
        if (part.empty() || part == "." || part == "..")
            fail(ErrorCode::invalid_argument, path, "invalid store path component");
        first = end + 1;
    }
    return std::string(path);
}
std::string prefix_path(std::string_view root, std::string_view path) {
    return root == "/" ? std::string(path) : path == "/" ? std::string(root) : std::string(root) + std::string(path);
}
std::size_t shape_size(const std::vector<std::size_t>& shape, std::string_view path) {
    if (std::find(shape.begin(), shape.end(), 0) != shape.end()) return 0;
    std::size_t size = 1;
    for (auto extent : shape) {
        if (extent > std::numeric_limits<std::size_t>::max() / size)
            fail(ErrorCode::invalid_argument, path, "array extent product overflows size_t");
        size *= extent;
    }
    return size;
}
bool tree_child(std::string_view key) {
    return key == "obst" || key == "vart" || key == "label" || key == "alignment" || key == "allow_overlap";
}
// Rebase existing root-relative AnnData operations at one I/O boundary. No
// numerical loop passes through this adapter; transfers bind the physical path.
class RootBackend final : public Backend {
    std::shared_ptr<Session> parent_;
    std::string root_;
    bool projection_ = false;
    bool unversioned_ = false;
    Backend& base() const { parent_->check_open(); return *parent_->backend; }
    std::string path(std::string_view p) const { return prefix_path(root_, p); }
public:
    RootBackend(std::shared_ptr<Session> parent, std::string root, bool projection = false, bool unversioned = false)
        : parent_(std::move(parent)), root_(std::move(root)), projection_(projection), unversioned_(unversioned) {}
    std::string location() const override { return base().location(); }
    std::string identity() const override { return base().identity() + "#" + root_; }
    std::unique_ptr<Backend> fork(std::size_t scratch) override {
        auto fork = std::make_shared<Session>(); fork->backend = base().fork(scratch); fork->options = parent_->options;
        fork->options.scratch_bytes = scratch;
        return std::make_unique<RootBackend>(std::move(fork), root_, projection_, unversioned_);
    }
    bool reuse_numeric_reads() const override { return base().reuse_numeric_reads(); }
    std::size_t numeric_retained_bytes(const NodeInfo& info,std::string_view p) const override { return base().numeric_retained_bytes(info,path(p)); }
    std::unique_ptr<NumericTransfer> prepare_numeric(std::string_view p) override { return base().prepare_numeric(path(p)); }
    std::unique_ptr<NumericTransfer> prepare_numeric(std::string_view p,const NodeInfo& info) override { return base().prepare_numeric(path(p),info); }
    std::size_t retained_scratch_bytes() const override { return base().retained_scratch_bytes(); }
    std::size_t scratch_allocations() const override { return base().scratch_allocations(); }
    NodeInfo inspect(std::string_view p) override {
        auto info = base().inspect(path(p));
        if ((projection_ || unversioned_) && p == "/") { info.attrs["encoding-type"] = std::string("anndata"); info.attrs["encoding-version"] = std::string("0.1.0"); }
        return info;
    }
    Element metadata(std::string_view p) override { return base().metadata(path(p)); }
    void metadata(std::string_view p, ElementView attrs) override { base().metadata(path(p), attrs); }
    std::size_t numeric_workspace_bytes(const NodeInfo& info, bool write) override { return base().numeric_workspace_bytes(info, write); }
    std::vector<std::string> children(std::string_view p) override {
        auto keys = base().children(path(p));
        if (projection_ && p == "/") keys.erase(std::remove_if(keys.begin(), keys.end(), tree_child), keys.end());
        return keys;
    }
    void read_numeric(std::string_view p, const std::vector<std::size_t>& start, const std::vector<std::size_t>& count, void* out) override { base().read_numeric(path(p), start, count, out); }
    void write_numeric(std::string_view p, const std::vector<std::size_t>& start, const std::vector<std::size_t>& count, const void* in) override { base().write_numeric(path(p), start, count, in); }
    std::vector<std::string> read_strings(std::string_view p, const std::vector<std::size_t>& start, const std::vector<std::size_t>& count) override { return base().read_strings(path(p), start, count); }
    std::vector<std::string> read_strings_bounded(std::string_view p, const std::vector<std::size_t>& start, const std::vector<std::size_t>& count, std::size_t budget) override { return base().read_strings_bounded(path(p), start, count, budget); }
    std::vector<std::string> read_strings_gather_bounded(std::string_view p, const std::size_t* positions, std::size_t count, std::size_t budget) override {
        return base().read_strings_gather_bounded(path(p), positions, count, budget);
    }
    void write_strings(std::string_view p, const std::vector<std::size_t>& start, const std::vector<std::size_t>& count, const std::vector<std::string>& values) override { base().write_strings(path(p), start, count, values); }
    void create_group(std::string_view p, const Attributes& attrs) override { base().create_group(path(p), attrs); }
    void create_numeric(std::string_view p, DType type, const std::vector<std::size_t>& shape, const Attributes& attrs, ByteOrder order) override { base().create_numeric(path(p), type, shape, attrs, order); }
    void create_strings(std::string_view p, const std::vector<std::size_t>& shape, const Attributes& attrs) override { base().create_strings(path(p), shape, attrs); }
    void create_record(std::string_view p, const std::vector<std::size_t>& shape, const std::vector<RecordFieldInfo>& fields, const Attributes& attrs) override { base().create_record(path(p), shape, fields, attrs); }
    void read_record_numeric(std::string_view p, std::string_view field, const std::vector<std::size_t>& start, const std::vector<std::size_t>& count, void* out) override { base().read_record_numeric(path(p), field, start, count, out); }
    void write_record_numeric(std::string_view p, std::string_view field, const std::vector<std::size_t>& start, const std::vector<std::size_t>& count, const void* in) override { base().write_record_numeric(path(p), field, start, count, in); }
    std::vector<std::string> read_record_strings_bounded(std::string_view p, std::string_view field, const std::vector<std::size_t>& start, const std::vector<std::size_t>& count, std::size_t budget) override { return base().read_record_strings_bounded(path(p), field, start, count, budget); }
    void write_record_strings(std::string_view p, std::string_view field, const std::vector<std::size_t>& start, const std::vector<std::size_t>& count, const std::vector<std::string>& values) override { base().write_record_strings(path(p), field, start, count, values); }
    void attributes(std::string_view p, const Attributes& attrs) override { base().attributes(path(p), attrs); }
    void erase(std::string_view p) override { base().erase(path(p)); }
    void rename(std::string_view from, std::string_view to) override { base().rename(path(from), path(to)); }
    void copy_attributes(std::string_view from, std::string_view to) override { base().copy_attributes(path(from), path(to)); }
    void flush() override { base().flush(); }
    void close() override {} // A rooted handle closes its scope, not sibling scopes.
    std::optional<MappedArray> map_numeric(std::string_view p) override { return base().map_numeric(path(p)); }
};
std::shared_ptr<Session> store_session(const std::shared_ptr<detail::BackedStoreImpl>& impl) {
    if (!impl || !impl->session) fail(ErrorCode::invalid_argument, "store", "empty store");
    impl->session->check_open(); return impl->session;
}
std::string_view element_encoding_attribute(ElementView attrs) {
    if (attrs.kind() != ElementKind::mapping || !attrs.contains("encoding-type")) return {};
    const auto value = attrs.at("encoding-type");
    return value.kind() == ElementKind::string ? value.string() : std::string_view{};
}
void check_stored_float16(Backend& backend, std::string_view path, const NodeInfo& info) {
    if (info.dtype != DType::float16) return;
    const auto attr = info.attrs.find("encoding-type");
    const auto* code = attr == info.attrs.end() ? nullptr : std::get_if<std::string>(&attr->second);
    require_dense_float16(backend, path, code ? std::string_view(*code) : std::string_view{});
}
std::shared_ptr<detail::BackedNode> raw_node(std::shared_ptr<Session> session, std::string path, bool strings) {
    const auto info = session->backend->inspect(path);
    if (!info.exists) fail(ErrorCode::out_of_bounds, path, "stored array does not exist");
    if (info.group || info.record || info.strings != strings) fail(ErrorCode::unsupported_encoding, path, "stored payload has the wrong array kind");
    check_stored_float16(*session->backend, path, info);
    auto& revision = session->revisions[path]; if (!revision) revision = std::make_shared<Revision>();
    auto node = std::make_shared<detail::BackedNode>(); node->session = std::move(session); node->path = std::move(path);
    node->revision = revision; node->observed_revision = revision->value; node->raw_array = true;
    node->writable = node->session->options.mode == AccessMode::read_write;
    for (auto size : info.shape) node->axes.push_back(Axis{size});
    return node;
}
void ensure_parents(Backend& backend, std::string_view path) {
    for (std::size_t end = path.find('/', 1); end != std::string_view::npos; end = path.find('/', end + 1)) {
        const auto parent = path.substr(0, end); auto info = backend.inspect(parent);
        if (!info.exists) backend.create_group(parent, {});
        else if (!info.group) fail(ErrorCode::unsupported_encoding, parent, "store parent is not a group");
    }
}
// Stage one replacement before touching the old binding. This is not a
// whole-store transaction; a backend failure during publication can be partial.
template<class Write>
void replace(const std::shared_ptr<Session>& session, const std::string& target, Write write) {
    session->check_writable(); auto& backend = *session->backend; ensure_parents(backend, target);
    const auto slash = target.rfind('/'); const auto parent = slash == 0 ? std::string("/") : target.substr(0, slash);
    std::string temporary;
    for (std::size_t i = 0;; ++i) {
        temporary = child(parent, ".anndatapp-store-" + std::to_string(i));
        if (temporary != target && !backend.inspect(temporary).exists) break;
        if (i == std::numeric_limits<std::size_t>::max()) fail(ErrorCode::invalid_argument, target, "temporary store namespace exhausted");
    }
    bool publishing = false;
    try {
        write(temporary);
        publishing = true;
        if (target == "/") {
            auto attrs = backend.metadata(temporary);
            for (const auto& key : backend.children("/")) if (child("/", key) != temporary) backend.erase(child("/", key));
            for (const auto& key : backend.children(temporary)) backend.rename(child(temporary, key), child("/", key));
            backend.metadata("/", attrs.view()); backend.erase(temporary);
        } else {
            if (backend.inspect(target).exists) backend.erase(target);
            backend.rename(temporary, target);
        }
        session->invalidate(target); ++session->dataset_revision;
    } catch (...) {
        if (publishing) { session->invalidate(target); ++session->dataset_revision; }
        try { if (backend.inspect(temporary).exists) backend.erase(temporary); } catch (...) {}
        throw;
    }
}
}

Element attributes_element(const Attributes& attrs) {
    auto result = Element::mapping();
    for (const auto& [key, value] : attrs) std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::vector<std::string>>) result.set(key, Element(StringArray::from_strings({item.size()}, item)));
        else if constexpr (std::is_same_v<T, std::vector<std::int64_t>>) {
            auto a = Array::empty<std::int64_t>({item.size()}); auto p = a.edit().template dense<std::int64_t>().data();
            std::copy(item.begin(), item.end(), p); result.set(key, Element(std::move(a)));
        } else result.set(key, Element(item));
    }, value);
    return result;
}
Attributes element_attributes(ElementView attrs, std::string_view path) {
    Attributes result;
    if (attrs.kind() == ElementKind::null) return result;
    if (attrs.kind() != ElementKind::mapping) fail(ErrorCode::invalid_argument, path, "attributes must be a mapping");
    for (const auto& key : attrs.keys()) {
        auto v = attrs.at(key);
        switch (v.kind()) {
        case ElementKind::string: result[key] = std::string(v.string()); break;
        case ElementKind::boolean: result[key] = v.boolean(); break;
        case ElementKind::signed_integer: result[key] = v.signed_integer(); break;
        case ElementKind::unsigned_integer:
            if (v.unsigned_integer() > std::uint64_t(INT64_MAX)) fail(ErrorCode::unsupported_dtype, path, "attribute integer exceeds signed range");
            result[key] = std::int64_t(v.unsigned_integer()); break;
        case ElementKind::floating: result[key] = v.floating(); break;
        case ElementKind::string_array: {
            auto a = v.string_array(); if (a.rank() != 1) fail(ErrorCode::unsupported_encoding, path, "attribute string array must have rank one");
            std::vector<std::string> values; for (std::size_t i = 0; i < a.size(); ++i) values.emplace_back(a.string(i));
            result[key] = std::move(values); break;
        }
        default: fail(ErrorCode::unsupported_encoding, path, "attribute kind requires a backend-native metadata adapter");
        }
    }
    return result;
}
Element Backend::metadata(std::string_view path) { return attributes_element(inspect(path).attrs); }
void Backend::metadata(std::string_view path, ElementView attrs) { attributes(path, element_attributes(attrs, path)); }
BackedStore make_backed_store(std::unique_ptr<Backend> backend, BackedOptions options) {
    if (!backend) fail(ErrorCode::invalid_argument, "store", "backend is empty");
    if (options.scratch_bytes < 64 || options.decoded_cache_bytes) fail(ErrorCode::invalid_argument, "store", "invalid scratch budget or unsupported decoded cache");
    auto impl = std::make_shared<detail::BackedStoreImpl>(); impl->session = std::make_shared<Session>();
    impl->session->backend = std::move(backend); impl->session->options = options;
    return detail::BackedAccess::store(std::move(impl));
}
} // namespace anndatapp::backed_detail

namespace anndatapp {
using backed_detail::checked_path;
using backed_detail::store_session;
BackedStore::BackedStore() = default;
BackedStore::~BackedStore() = default;
BackedStore::BackedStore(BackedStore&&) noexcept = default;
BackedStore& BackedStore::operator=(BackedStore&&) noexcept = default;
BackedStore::BackedStore(std::shared_ptr<detail::BackedStoreImpl> impl) : impl_(std::move(impl)) {}
bool BackedStore::is_open() const noexcept { return impl_ && impl_->session && impl_->session->open; }
bool BackedStore::writable() const { return store_session(impl_)->options.mode == AccessMode::read_write; }
std::string BackedStore::location() const { return store_session(impl_)->backend->location(); }
bool BackedStore::exists(std::string_view path) const { return store_session(impl_)->backend->inspect(checked_path(path)).exists; }
std::vector<std::string> BackedStore::children(std::string_view path) const { return store_session(impl_)->backend->children(checked_path(path)); }
Element BackedStore::attributes(std::string_view path) const { return store_session(impl_)->backend->metadata(checked_path(path)); }
void BackedStore::set_attributes(std::string_view path, ElementView attrs) {
    auto s = store_session(impl_); s->check_writable(); const auto p = checked_path(path);
    const auto code = backed_detail::element_encoding_attribute(attrs);
    if (code == "numeric-scalar" && s->backend->inspect(p).dtype == DType::float16)
        backed_detail::require_dense_float16(p, code, {});
    s->backend->metadata(p, attrs); s->invalidate(p); ++s->dataset_revision;
}
void BackedStore::create_group(std::string_view path, ElementView attrs) {
    auto s = store_session(impl_); s->check_writable(); const auto p = checked_path(path); backed_detail::ensure_parents(*s->backend, p);
    const auto info = s->backend->inspect(p);
    if (info.exists && !info.group) throw Error(ErrorCode::unsupported_encoding, p, "existing store element is not a group");
    if (!info.exists) s->backend->create_group(p, {});
    if (attrs.kind() != ElementKind::null) s->backend->metadata(p, attrs);
    s->invalidate(p); ++s->dataset_revision;
}
BackedArray BackedStore::array(std::string_view path) const {
    return detail::BackedAccess::array(backed_detail::raw_node(store_session(impl_), checked_path(path), false));
}
BackedStringArray BackedStore::strings(std::string_view path) const {
    return detail::BackedAccess::strings(backed_detail::raw_node(store_session(impl_), checked_path(path), true));
}
BackedArray BackedStore::create_array(std::string_view path, DType type, std::vector<std::size_t> shape, ElementView attrs) {
    auto s = store_session(impl_); const auto p = checked_path(path);
    if (p == "/") throw Error(ErrorCode::invalid_argument, p, "store root must be a group");
    if (type == DType::float16)
        backed_detail::require_dense_float16(*s->backend, p, backed_detail::element_encoding_attribute(attrs));
    const auto size = backed_detail::shape_size(shape, p), width = dtype_size(type);
    if (!width || size > std::numeric_limits<std::size_t>::max() / width)
        throw Error(ErrorCode::invalid_argument, p, "array byte size overflows size_t");
    backed_detail::replace(s, p, [&](const std::string& temporary) {
        s->backend->create_numeric(temporary, type, shape, {}, ByteOrder::native);
        if (attrs.kind() != ElementKind::null) s->backend->metadata(temporary, attrs);
    });
    return array(p);
}
BackedStringArray BackedStore::create_strings(std::string_view path, std::vector<std::size_t> shape, ElementView attrs) {
    auto s = store_session(impl_); const auto p = checked_path(path);
    if (p == "/") throw Error(ErrorCode::invalid_argument, p, "store root must be a group");
    (void)backed_detail::shape_size(shape, p);
    backed_detail::replace(s, p, [&](const std::string& temporary) {
        s->backend->create_strings(temporary, shape, {});
        if (attrs.kind() != ElementKind::null) s->backend->metadata(temporary, attrs);
    });
    return strings(p);
}
std::string BackedStore::read_string(std::string_view path) const {
    auto a = strings(path); if (!a.shape().empty()) throw Error(ErrorCode::shape_mismatch, std::string(path), "expected a scalar string");
    return a.string(0);
}
BackedElement BackedStore::element(std::string_view path) const {
    auto s = store_session(impl_); auto p = checked_path(path);
    const auto info = s->backend->inspect(p);
    backed_detail::check_stored_float16(*s->backend, p, info);
    const auto shape = backed_detail::logical_shape(*s, p, info);
    return detail::BackedAccess::element(backed_detail::node_with_shape(std::move(s), std::move(p), shape));
}
void BackedStore::write_array(std::string_view path, ArrayView value, ElementView attrs) {
    auto s = store_session(impl_); const auto p = checked_path(path);
    if (p == "/") throw Error(ErrorCode::invalid_argument, p, "store root must be a group");
    if (value.dtype() == DType::float16)
        backed_detail::require_dense_float16(*s->backend, p, backed_detail::element_encoding_attribute(attrs));
    backed_detail::replace(s, p, [&](const std::string& temporary) {
        backed_detail::write_raw_array(*s->backend, temporary, value, s->options.scratch_bytes);
        if (attrs.kind() != ElementKind::null) s->backend->metadata(temporary, attrs);
    });
}
void BackedStore::write_strings(std::string_view path, StringArrayView value, ElementView attrs) {
    auto s = store_session(impl_); const auto p = checked_path(path);
    if (p == "/") throw Error(ErrorCode::invalid_argument, p, "store root must be a group");
    backed_detail::replace(s, p, [&](const std::string& temporary) {
        backed_detail::write_raw_strings(*s->backend, temporary, value, s->options.scratch_bytes);
        if (attrs.kind() != ElementKind::null) s->backend->metadata(temporary, attrs);
    });
}
void BackedStore::write_strings(std::string_view path, const std::vector<std::size_t>& start,
                               const std::vector<std::size_t>& count, const std::vector<std::string>& values) {
    auto s = store_session(impl_); s->check_writable(); const auto p = checked_path(path);
    const auto info = s->backend->inspect(p);
    if (!info.exists || info.group || !info.strings) throw Error(ErrorCode::unsupported_encoding, p, "string ranges require a string array");
    if (start.size() != info.shape.size() || count.size() != info.shape.size())
        throw Error(ErrorCode::shape_mismatch, p, "string range rank differs from array rank");
    for (std::size_t i = 0; i < start.size(); ++i)
        if (start[i] > info.shape[i] || count[i] > info.shape[i] - start[i])
            throw Error(ErrorCode::out_of_bounds, p, "string range exceeds array shape");
    const auto size = backed_detail::shape_size(count, p);
    if (size != values.size()) throw Error(ErrorCode::shape_mismatch, p, "string value count differs from range");
    for (const auto& value : values) backed_detail::validate_utf8_string(value, p);
    if (size) s->backend->write_strings(p, start, count, values);
}
void BackedStore::write_string(std::string_view path, std::string_view value, ElementView attrs) {
    auto s = store_session(impl_); const auto p = checked_path(path);
    if (p == "/") throw Error(ErrorCode::invalid_argument, p, "store root must be a group");
    backed_detail::validate_utf8_string(value, p);
    const auto overhead = sizeof(std::string) + sizeof(const char*) + 1;
    if (s->options.scratch_bytes < overhead || value.size() > s->options.scratch_bytes - overhead)
        throw Error(ErrorCode::invalid_argument, p, "scalar string transfer exceeds session scratch budget");
    backed_detail::replace(s, p, [&](const std::string& temporary) {
        s->backend->create_strings(temporary, {}, {});
        if (attrs.kind() != ElementKind::null) s->backend->metadata(temporary, attrs);
        std::vector<std::string> batch; batch.emplace_back(value);
        s->backend->write_strings(temporary, {}, {}, batch);
    });
}
void BackedStore::write_element(std::string_view path, ElementView value) {
    auto s = store_session(impl_); const auto p = checked_path(path);
    if (p == "/") throw Error(ErrorCode::invalid_argument, p, "use write_anndata for a dataset root");
    if (value.kind() == ElementKind::array && value.array().dtype() == DType::float16)
        backed_detail::require_dense_float16(*s->backend, p, "array");
    backed_detail::validate_serializable(value, p);
    backed_detail::replace(s, p, [&](const std::string& temporary) { backed_detail::write_element(*s->backend, temporary, value, s->options.scratch_bytes); });
}
BackedAnnData BackedStore::anndata(std::string_view root, bool allow_unversioned_root) const {
    auto parent = store_session(impl_); auto path = checked_path(root);
    const auto info = parent->backend->inspect(path);
    bool projection = false;
    const bool unversioned = allow_unversioned_root && !info.attrs.contains("encoding-type") && !info.attrs.contains("encoding-version");
    if (auto it = info.attrs.find("encoding-type"); it != info.attrs.end())
        if (const auto* value = std::get_if<std::string>(&it->second)) projection = *value == "treedata";
    if (projection && backed_detail::string_attr(info, "encoding-version", path) != "0.1.0")
        throw Error(ErrorCode::unsupported_version, path, "unsupported TreeData root version");
    auto data = backed_detail::make_store(std::make_unique<backed_detail::RootBackend>(parent, path, projection, unversioned), parent->options);
    auto session = detail::BackedAccess::get(data.view())->session;
    session->parent = parent; session->root = path;
    auto& scopes = parent->children;
    scopes.erase(std::remove_if(scopes.begin(), scopes.end(), [](const auto& scope) { return scope.expired(); }), scopes.end());
    scopes.emplace_back(session);
    return data;
}
void BackedStore::write_anndata(std::string_view root, AnnDataView data, Validation validation) {
    auto s = store_session(impl_); const auto path = checked_path(root);
    backed_detail::replace(s, path, [&](const std::string& temporary) {
        backed_detail::RootBackend target(s, temporary); target.create_group("/", {});
        backed_detail::initialize_store(target, data, s->options.scratch_bytes, validation);
    });
}
void BackedStore::write_anndata(std::string_view root, BackedAnnDataView data, Validation validation) {
    auto s = store_session(impl_); const auto path = checked_path(root);
    const auto& source = detail::BackedAccess::get(data);
    if (!source || !source->session) throw Error(ErrorCode::invalid_argument, path, "empty backed AnnData source");
    source->session->check_open();
    auto physical = source->session; while (physical->parent) physical = physical->parent;
    if (physical->backend->identity() == s->backend->identity()) {
        const auto source_root = source->session->parent ? source->session->root : std::string("/");
        if (source_root == "/" || (path.size() > source_root.size() && path.starts_with(source_root) && path[source_root.size()] == '/'))
            throw Error(ErrorCode::invalid_argument, path, "cannot stage inside an overlapping AnnData source; use a separate destination");
    }
    backed_detail::replace(s, path, [&](const std::string& temporary) {
        backed_detail::RootBackend target(s, temporary);
        backed_detail::export_store(target, data, s->options.scratch_bytes, validation);
    });
}
void BackedStore::erase(std::string_view path) {
    auto s = store_session(impl_); s->check_writable(); const auto p = checked_path(path);
    if (p == "/") throw Error(ErrorCode::invalid_argument, p, "cannot erase the store root");
    s->backend->erase(p); s->invalidate(p); ++s->dataset_revision;
}
void BackedStore::flush() { store_session(impl_)->backend->flush(); }
void BackedStore::close() { if (is_open()) { impl_->session->backend->close(); impl_->session->open = false; } }
} // namespace anndatapp
