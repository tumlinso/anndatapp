module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module anndatapp.spatialdata;
#include "spatialdata_names.hpp"

namespace anndatapp {
namespace detail {
struct SpatialRasterImpl {
    SpatialElementKind kind;
    std::vector<SpatialAxis> axes;
    std::vector<Array> levels;
    std::vector<SpatialTransform> level_transforms;
    std::vector<std::string> paths, channels;
    Element channel_labels;
    std::vector<SpatialNamedTransform> transforms;
};
struct SpatialRasterViewImpl {
    SpatialElementKind kind;
    std::vector<SpatialAxis> axes;
    std::vector<ArrayView> levels;
    std::vector<SpatialTransform> level_transforms;
    std::vector<std::string> paths, channels;
    ElementView channel_labels;
    std::vector<SpatialNamedTransform> transforms;
};
struct SpatialPointsImpl {
    Table values;
    Column ids;
    std::vector<SpatialAxis> axes;
    std::string feature_key, instance_key;
    std::vector<SpatialNamedTransform> transforms;
};
struct SpatialPointsViewImpl {
    TableView values;
    ColumnView ids;
    std::vector<SpatialAxis> axes;
    std::string feature_key, instance_key;
    std::vector<SpatialNamedTransform> transforms;
};
struct SpatialShapesImpl {
    SpatialGeometryKind kind;
    Array xy, radii, rings, polygons, geometries, types;
    Column ids;
    Table attributes;
    std::vector<SpatialNamedTransform> transforms;
};
struct SpatialShapesViewImpl {
    SpatialGeometryKind kind;
    ArrayView xy, radii, rings, polygons, geometries, types;
    ColumnView ids;
    TableView attributes;
    Axis rows;
    std::vector<SpatialNamedTransform> transforms;
};
struct SpatialTable { AnnData data; SpatialAnnotation annotation; };
struct SpatialTableView { AnnDataView data; SpatialAnnotation annotation; };
struct SpatialDataImpl {
    std::vector<std::pair<std::string, SpatialRaster>> rasters;
    std::vector<std::pair<std::string, SpatialPoints>> points;
    std::vector<std::pair<std::string, SpatialShapes>> shapes;
    std::vector<std::pair<std::string, SpatialTable>> tables;
    Element metadata = Element::mapping();
};
struct SpatialDataViewImpl {
    std::vector<std::pair<std::string, SpatialRasterView>> rasters;
    std::vector<std::pair<std::string, SpatialPointsView>> points;
    std::vector<std::pair<std::string, SpatialShapesView>> shapes;
    std::vector<std::pair<std::string, SpatialTableView>> tables;
    ElementView metadata;
};
}
namespace {
[[noreturn]] void fail(std::string_view path, const char* message,
    ErrorCode code = ErrorCode::invalid_argument) { throw Error(code, std::string(path), message); }
template<class T> const T& require(const std::shared_ptr<T>& p) {
    if (!p) fail("spatial", "empty spatial view");
    return *p;
}
template<class V> auto find(V& v, std::string_view name) {
    return std::find_if(v.begin(), v.end(), [&](const auto& x) { return x.first == name; });
}
template<class V> const auto& get(const V& v, std::string_view name) {
    auto it = find(v, name);
    if (it == v.end()) fail(name, "spatial element does not exist", ErrorCode::out_of_bounds);
    return it->second;
}
template<class V, class T> void put(V& v, std::string name, T value) {
    auto it = find(v, name);
    if (it == v.end()) v.emplace_back(std::move(name), std::move(value));
    else it->second = std::move(value);
}
void transform_axes(const SpatialTransform& t, const std::vector<SpatialAxis>& axes) {
    t.validate();
    if (t.input_rank != axes.size()) fail("spatial/transform", "input axes differ from element axes");
    for (std::size_t i = 0; i < axes.size(); ++i)
        if (t.input_axes[i] != axes[i]) fail("spatial/transform", "input axes differ from element axes");
    for(std::size_t j=0;j<t.input_rank;++j)if(t.input_axes[j]==SpatialAxis::c){
        std::size_t channel=0;while(channel<t.output_rank&&t.output_axes[channel]!=SpatialAxis::c)++channel;
        if(channel==t.output_rank)fail("spatial/transform","channel axis must remain unchanged");
        for(std::size_t i=0;i<t.output_rank;++i)if(t.m[5*i+j]!=(i==channel?1.:0.))fail("spatial/transform","channel coordinates must not mix with spatial coordinates");
        for(std::size_t k=0;k<=t.input_rank;++k)if(k!=j&&t.m[5*channel+k]!=0)fail("spatial/transform","channel coordinates must remain unchanged");
    }
}
SpatialTransform identity(const std::vector<SpatialAxis>& axes) {
    double m[20]{};
    for (std::size_t i = 0; i < axes.size(); ++i) m[i*(axes.size()+1)+i] = 1;
    return SpatialTransform::affine(axes.data(), axes.size(), axes.data(), axes.size(), m, axes.size()+1);
}
void set_transform(std::vector<SpatialNamedTransform>& ts, std::string system,
    SpatialTransform t, const std::vector<SpatialAxis>& axes) {
    if (!spatial_detail::valid_element_name(system)) fail("spatial/transform", "invalid coordinate system name");
    transform_axes(t, axes);
    for (auto& v : ts) if (v.coordinate_system == system) { v.transform = t; return; }
    ts.push_back({std::move(system), t});
}
void transforms_valid(const std::vector<SpatialNamedTransform>& ts, const std::vector<SpatialAxis>& axes) {
    if (ts.empty()) fail("spatial/transform", "spatial element needs a coordinate system");
    for (std::size_t i = 0; i < ts.size(); ++i) {
        if (!spatial_detail::valid_element_name(ts[i].coordinate_system)) fail("spatial/transform", "invalid coordinate system name");
        transform_axes(ts[i].transform, axes);
        for (std::size_t j = 0; j < i; ++j)
            if (ts[i].coordinate_system == ts[j].coordinate_system) fail("spatial/transform", "duplicate coordinate system");
    }
}
const char* axis_name(SpatialAxis a) {
    switch (a) { case SpatialAxis::x: return "x"; case SpatialAxis::y: return "y";
        case SpatialAxis::z: return "z"; case SpatialAxis::c: return "c"; }
    fail("spatial", "unknown coordinate axis");
}
// Validation uses one row-index workspace; retained numeric storage and its
// logical row map keep exact integer identity without materializing selections.
struct IdentifierOrder {
    ColumnView ids;
    DenseBytesView<const std::byte> bytes;
    std::ptrdiff_t stride = 0;
    explicit IdentifierOrder(ColumnView value) : ids(std::move(value)) {
        if(ids.kind()==ColumnKind::numeric){bytes=ids.source_values().bytes();stride=bytes.stride_bytes(0);}
    }
    int compare(std::size_t a,std::size_t b) const {
        if(ids.kind()==ColumnKind::string)return ids.string(a).compare(ids.string(b));
        return std::memcmp(bytes.data()+std::ptrdiff_t(ids.source_row(a))*stride,
            bytes.data()+std::ptrdiff_t(ids.source_row(b))*stride,bytes.element_size());
    }
};
std::unique_ptr<std::size_t[]> validation_rows(std::size_t n,std::size_t scratch,const char* path){
    if(n>scratch/sizeof(std::size_t))fail(path,"validation row-index workspace exceeds scratch_bytes");
    auto rows=std::make_unique<std::size_t[]>(n);
    for(std::size_t i=0;i<n;++i)rows[i]=i;
    return rows;
}
void ids_valid(ColumnView ids, std::size_t n, bool unique,std::size_t scratch=~std::size_t{0}) {
    if (ids.size() != n) fail("spatial/identifiers", "identifier count differs from rows", ErrorCode::shape_mismatch);
    if (ids.kind() != ColumnKind::string && ids.kind() != ColumnKind::numeric)
        fail("spatial/identifiers", "identifiers must be integer or string");
    if (ids.kind() == ColumnKind::numeric && unsigned(ids.source_values().dtype()) >= unsigned(DType::float32))
        fail("spatial/identifiers", "identifiers must be integer or string");
    if (ids.kind() == ColumnKind::numeric && ids.source_values().dtype() == DType::boolean)
        fail("spatial/identifiers", "boolean instance identifiers are unsupported");
    for (std::size_t i = 0; i < n; ++i)
        if (!ids.valid(i)) fail("spatial/identifiers", "instance identifier cannot be null");
    if (!unique || n<2) return;
    auto rows=validation_rows(n,scratch,"spatial/identifiers");IdentifierOrder order(ids);
    std::sort(rows.get(),rows.get()+n,[&](auto a,auto b){return order.compare(a,b)<0;});
    for(std::size_t i=1;i<n;++i)if(order.compare(rows[i-1],rows[i])==0)
        fail("spatial/identifiers","shape identifiers must be unique");
}
void axis_valid(const Axis& axis, std::size_t n) {
    for (std::size_t i = 0; i < axis.extent(n); ++i)
        if (axis.index(i) >= n) fail("spatial/selection", "row selection is out of bounds", ErrorCode::out_of_bounds);
}
Array offsets_array(const std::vector<std::uint64_t>& v) {
    auto a = Array::empty<std::uint64_t>({v.size()}); auto p = a.edit().dense<std::uint64_t>();
    for (std::size_t i = 0; i < v.size(); ++i) p.data()[i] = v[i];
    return a;
}
void offsets_valid(ArrayView a, std::size_t count, const char* name) {
    if (a.layout() != Layout::dense || a.dtype() != DType::uint64 || a.rank() != 1 || !a.extent(0))
        fail(name, "offsets must be a nonempty uint64 vector");
    auto x = a.dense<std::uint64_t>();
    if (x.data()[0] != 0 || x.data()[std::ptrdiff_t(a.extent(0)-1)*x.stride(0)] != count)
        fail(name, "offset endpoints differ from referenced buffer");
    for (std::size_t i = 1; i < a.extent(0); ++i)
        if (x.data()[std::ptrdiff_t(i)*x.stride(0)] < x.data()[std::ptrdiff_t(i-1)*x.stride(0)]) fail(name, "offsets must be monotonic");
}
void shape_coordinates(ArrayView a) {
    if (a.layout() != Layout::dense || a.dtype() != DType::float64 || a.rank() != 2 || a.extent(1) != 2)
        fail("spatial/shapes", "coordinates must be an FP64 [n,2] array");
    auto x = a.dense<double>();
    for (std::size_t i = 0; i < a.extent(0); ++i)
        for (std::size_t j = 0; j < 2; ++j)
            if (!__builtin_isfinite(x.data()[std::ptrdiff_t(i)*x.stride(0)+std::ptrdiff_t(j)*x.stride(1)]))
                fail("spatial/shapes", "geometry coordinates must be finite");
}
template<class D> std::vector<std::string> element_names(const D& d) {
    std::vector<std::string> out;
    for (const auto& x : d.rasters) out.push_back(x.first);
    for (const auto& x : d.points) out.push_back(x.first);
    for (const auto& x : d.shapes) out.push_back(x.first);
    for (const auto& x : d.tables) out.push_back(x.first);
    return out;
}
void validate_name_set(const std::vector<std::string>& names) {
    std::vector<std::string> lower; lower.reserve(names.size());
    for (const auto& name : names) {
        if (!spatial_detail::valid_element_name(name)) fail(name, "invalid SpatialData element name");
        lower.push_back(spatial_detail::lowercase_name(name));
    }
    std::sort(lower.begin(), lower.end());
    for (std::size_t i = 1; i < lower.size(); ++i)
        if (lower[i] == lower[i-1]) fail("spatial", "element names collide ignoring case");
}
void table_names_valid(AnnDataView a) {
    validate_name_set(a.obs().column_names());validate_name_set(a.var().column_names());
    validate_name_set(a.layer_names());validate_name_set(a.obsm_names());validate_name_set(a.varm_names());
    validate_name_set(a.obsp_names());validate_name_set(a.varp_names());validate_name_set(a.uns().keys());
}
void annotation_unique(AnnDataView data,const SpatialAnnotation& annotation,std::size_t scratch){
    if(annotation.region_key.empty())return;
    const auto obs=data.obs();if(obs.size()<2)return;
    auto regions=obs.column(annotation.region_key),ids=obs.column(annotation.instance_key);
    auto categories=regions.kind()==ColumnKind::categorical?regions.categories():ColumnView{};
    auto region=[&](std::size_t i){return regions.kind()==ColumnKind::string?regions.string(i):categories.string(std::size_t(regions.code(i)));};
    auto rows=validation_rows(obs.size(),scratch,"spatial/annotation");IdentifierOrder order(ids);
    auto compare=[&](std::size_t a,std::size_t b){const auto c=region(a).compare(region(b));return c?c:order.compare(a,b);};
    std::sort(rows.get(),rows.get()+obs.size(),[&](auto a,auto b){return compare(a,b)<0;});
    for(std::size_t i=1;i<obs.size();++i)if(compare(rows[i-1],rows[i])==0)
        fail("spatial/annotation","one table cannot annotate an instance more than once");
}
}

SpatialRaster::SpatialRaster(SpatialElementKind kind, std::vector<SpatialAxis> axes)
    : impl_(std::make_unique<detail::SpatialRasterImpl>()) {
    impl_->kind = kind; impl_->axes = std::move(axes);
    impl_->transforms.push_back({"global", identity(impl_->axes)});
}
SpatialRaster::~SpatialRaster() = default;
SpatialRaster::SpatialRaster(SpatialRaster&&) noexcept = default;
SpatialRaster& SpatialRaster::operator=(SpatialRaster&&) noexcept = default;
void SpatialRaster::add_level(Array a, std::string path) {
    if (path.empty()) path = "s" + std::to_string(impl_->levels.size());
    if (!spatial_detail::valid_element_name(path)) fail(path, "invalid raster level path");
    for (const auto& p : impl_->paths) if (p == path) fail(path, "duplicate raster level path");
    auto t=identity(impl_->axes);
    if(!impl_->levels.empty()&&a.rank()==impl_->axes.size())for(std::size_t j=0;j<a.rank();++j){
        if(!a.extent(j))fail("spatial/raster","pyramid axes cannot be empty");
        t.m[5*j+j]=double(impl_->levels[0].extent(j))/double(a.extent(j));
    }
    impl_->paths.push_back(std::move(path));impl_->levels.push_back(std::move(a));impl_->level_transforms.push_back(t);
}
void SpatialRaster::set_level_transform(std::size_t i,SpatialTransform t){
    if(i>=impl_->levels.size())fail("spatial/raster","resolution level out of bounds",ErrorCode::out_of_bounds);
    transform_axes(t,impl_->axes);if(t.output_rank!=impl_->axes.size())fail("spatial/raster","pyramid transform rank differs from axes");
    for(std::size_t j=0;j<t.input_rank;++j){if(t.output_axes[j]!=impl_->axes[j]||!(t.m[5*j+j]>0))fail("spatial/raster","pyramid transforms require positive axis-aligned scales");for(std::size_t k=0;k<t.input_rank;++k)if(j!=k&&t.m[5*j+k]!=0)fail("spatial/raster","pyramid transforms must be scale and translation");}
    impl_->level_transforms[i]=t;
}
void SpatialRaster::set_channels(std::vector<std::string> v) {
    auto labels=Element::sequence();for(const auto& s:v)labels.append(Element(s));
    impl_->channels=std::move(v);impl_->channel_labels=std::move(labels);
}
void SpatialRaster::set_channel_labels(Element labels) {
    auto v=labels.view();if(v.kind()!=ElementKind::sequence)fail("spatial/image","channel labels must be a sequence");
    impl_->channels.clear();bool strings=true;
    for(std::size_t i=0;i<v.size();++i){auto x=v.at(i);if(x.kind()==ElementKind::string)impl_->channels.emplace_back(x.string());else if(x.kind()==ElementKind::signed_integer||x.kind()==ElementKind::unsigned_integer||x.kind()==ElementKind::floating)strings=false;else fail("spatial/image","channel labels must be strings or numbers");}
    if(!strings)impl_->channels.clear();impl_->channel_labels=std::move(labels);
}
void SpatialRaster::set_transform(std::string s, SpatialTransform t) { anndatapp::set_transform(impl_->transforms, std::move(s), t, impl_->axes); }
void SpatialRaster::set_transforms(std::vector<SpatialNamedTransform> ts){transforms_valid(ts,impl_->axes);impl_->transforms=std::move(ts);}
SpatialRasterView SpatialRaster::view() const {
    auto v = std::make_shared<detail::SpatialRasterViewImpl>();
    v->kind = impl_->kind; v->axes = impl_->axes; v->paths = impl_->paths;
    v->channels = impl_->channels; v->channel_labels=impl_->channel_labels.view();v->transforms = impl_->transforms;v->level_transforms=impl_->level_transforms;
    for (const auto& a : impl_->levels) v->levels.push_back(a.view());
    return SpatialRasterView(std::move(v));
}
SpatialRaster SpatialRaster::copy() const { return view().copy(); }
void SpatialRaster::validate() const {
    const auto& d = *impl_;
    if (d.kind != SpatialElementKind::image && d.kind != SpatialElementKind::labels) fail("spatial/raster", "raster kind must be image or labels");
    const auto image = d.kind == SpatialElementKind::image;
    const std::vector<SpatialAxis> a2 = image ? std::vector<SpatialAxis>{SpatialAxis::c,SpatialAxis::y,SpatialAxis::x} : std::vector<SpatialAxis>{SpatialAxis::y,SpatialAxis::x};
    const std::vector<SpatialAxis> a3 = image ? std::vector<SpatialAxis>{SpatialAxis::c,SpatialAxis::z,SpatialAxis::y,SpatialAxis::x} : std::vector<SpatialAxis>{SpatialAxis::z,SpatialAxis::y,SpatialAxis::x};
    if (d.axes != a2 && d.axes != a3) fail("spatial/raster", "raster axes must be c(z)yx for images or (z)yx for labels");
    if (d.levels.empty()) fail("spatial/raster", "raster has no resolution levels");
    for (std::size_t i = 0; i < d.levels.size(); ++i) {
        const auto& a = d.levels[i]; a.validate();
        if (a.layout() != Layout::dense || a.rank() != d.axes.size()) fail("spatial/raster", "raster rank/layout differs from axes");
        if (!image && unsigned(a.dtype()) >= unsigned(DType::float32)) fail("spatial/labels", "labels need integer or boolean data");
        for (std::size_t j = 0; j < a.rank(); ++j) {
            if (!a.extent(j)) fail("spatial/raster", "raster axes cannot be empty");
            if (i && (a.extent(j) > d.levels[i-1].extent(j) || a.dtype() != d.levels[0].dtype())) fail("spatial/raster", "pyramid dimensions must not grow and dtypes must agree");
            if (image && j == 0 && a.extent(j) != d.levels[0].extent(j)) fail("spatial/raster", "pyramid channel extents differ");
        }
    }
    if (image && !d.channels.empty() && d.channels.size() != d.levels[0].extent(0)) fail("spatial/image", "channel label count differs from channels");
    if(image && d.channel_labels.view().kind()!=ElementKind::null && d.channel_labels.view().size()!=d.levels[0].extent(0))fail("spatial/image","channel label count differs from channels");
    transforms_valid(d.transforms, d.axes);
}
SpatialRasterView::SpatialRasterView() = default;
SpatialRasterView::SpatialRasterView(std::shared_ptr<detail::SpatialRasterViewImpl> p) : impl_(std::move(p)) {}
SpatialElementKind SpatialRasterView::kind() const { return require(impl_).kind; }
const std::vector<SpatialAxis>& SpatialRasterView::axes() const { return require(impl_).axes; }
std::size_t SpatialRasterView::n_levels() const { return require(impl_).levels.size(); }
ArrayView SpatialRasterView::level(std::size_t i) const { const auto& d=require(impl_); if(i>=d.levels.size()) fail("spatial/raster", "resolution level out of bounds",ErrorCode::out_of_bounds); return d.levels[i]; }
std::string_view SpatialRasterView::level_path(std::size_t i) const { const auto& d=require(impl_); if(i>=d.paths.size()) fail("spatial/raster", "resolution level out of bounds",ErrorCode::out_of_bounds); return d.paths[i]; }
SpatialTransform SpatialRasterView::level_transform(std::size_t i) const {const auto& d=require(impl_);if(i>=d.level_transforms.size())fail("spatial/raster","resolution level out of bounds",ErrorCode::out_of_bounds);return d.level_transforms[i];}
const std::vector<std::string>& SpatialRasterView::channels() const { return require(impl_).channels; }
ElementView SpatialRasterView::channel_labels() const { return require(impl_).channel_labels; }
const std::vector<SpatialNamedTransform>& SpatialRasterView::transforms() const { return require(impl_).transforms; }
SpatialRasterView SpatialRasterView::window(const Axis* axes, std::size_t rank, std::size_t i) const {
    const auto& d=require(impl_); auto source=level(i);
    if (rank!=d.axes.size() || (rank && !axes)) fail("spatial/raster", "window rank differs from raster");
    auto v=std::make_shared<detail::SpatialRasterViewImpl>(d);
    v->levels={source.select(axes,rank)}; v->paths={d.paths[i]};v->level_transforms={identity(d.axes)};
    auto window=identity(d.axes);
    for(std::size_t j=0;j<rank;++j) {
        if(d.axes[j]==SpatialAxis::c) {
            if(!d.channels.empty()) { v->channels.clear(); for(std::size_t k=0;k<axes[j].extent(source.extent(j));++k) v->channels.push_back(d.channels.at(axes[j].index(k))); }
            if(d.channel_labels.kind()!=ElementKind::null){auto labels=Element::sequence();for(std::size_t k=0;k<axes[j].extent(source.extent(j));++k)labels.append(d.channel_labels.at(axes[j].index(k)).copy());v->channel_labels=labels.view();}
            continue;
        }
        if(axes[j].is_gather()) fail("spatial/raster", "irregular spatial gather has no affine window transform");
        window.m[5*j+j]=axes[j].reverse()?-double(axes[j].step()):double(axes[j].step());
        window.m[5*j+rank]=double(axes[j].first());
    }
    const auto to_base=compose(d.level_transforms[i],window);
    for(auto& t:v->transforms) t.transform=compose(t.transform,to_base);
    return SpatialRasterView(std::move(v));
}
SpatialRaster SpatialRasterView::copy() const {
    const auto& d=require(impl_); SpatialRaster out(d.kind,d.axes);
    for(std::size_t i=0;i<d.levels.size();++i){out.add_level(d.levels[i].copy(),d.paths[i]);out.set_level_transform(i,d.level_transforms[i]);}
    if(d.channel_labels.kind()!=ElementKind::null)out.set_channel_labels(d.channel_labels.copy());out.impl_->transforms=d.transforms; return out;
}

SpatialPoints::SpatialPoints(Table values, Column ids, std::vector<SpatialAxis> axes)
    : impl_(std::make_unique<detail::SpatialPointsImpl>()) {
    impl_->values=std::move(values); impl_->ids=std::move(ids); impl_->axes=std::move(axes);
    impl_->transforms.push_back({"global",identity(impl_->axes)});
}
SpatialPoints::~SpatialPoints()=default;
SpatialPoints::SpatialPoints(SpatialPoints&&) noexcept=default;
SpatialPoints& SpatialPoints::operator=(SpatialPoints&&) noexcept=default;
void SpatialPoints::set_feature_key(std::string s) { impl_->feature_key=std::move(s); }
void SpatialPoints::set_instance_key(std::string s) { impl_->instance_key=std::move(s); }
void SpatialPoints::set_transform(std::string s,SpatialTransform t) { anndatapp::set_transform(impl_->transforms,std::move(s),t,impl_->axes); }
void SpatialPoints::set_transforms(std::vector<SpatialNamedTransform> ts){transforms_valid(ts,impl_->axes);impl_->transforms=std::move(ts);}
SpatialPointsView SpatialPoints::view() const {
    auto v=std::make_shared<detail::SpatialPointsViewImpl>();
    v->values=impl_->values.view(); v->ids=impl_->ids.view(); v->axes=impl_->axes;
    v->feature_key=impl_->feature_key; v->instance_key=impl_->instance_key; v->transforms=impl_->transforms;
    return SpatialPointsView(std::move(v));
}
SpatialPoints SpatialPoints::copy() const { return view().copy(); }
void SpatialPoints::validate() const {
    const auto& d=*impl_; d.values.validate(); d.ids.validate(); ids_valid(d.ids.view(),d.values.size(),false);
    if(d.axes!=std::vector<SpatialAxis>{SpatialAxis::x,SpatialAxis::y} && d.axes!=std::vector<SpatialAxis>{SpatialAxis::x,SpatialAxis::y,SpatialAxis::z}) fail("spatial/points","point axes must be xy or xyz");
    auto values=d.values.view();
    for(auto axis:d.axes) {
        if(!values.contains(axis_name(axis))) fail("spatial/points","coordinate column missing");
        auto c=values.column(axis_name(axis));
        if(c.kind()!=ColumnKind::numeric)fail("spatial/points","coordinate columns must be numeric");
        const auto type=c.values().dtype();if(type!=DType::int32&&type!=DType::int64&&type!=DType::float32&&type!=DType::float64)fail("spatial/points","coordinates require int32, int64, float32 or float64");
    }
    if(!d.feature_key.empty() && !values.contains(d.feature_key)) fail("spatial/points","feature key column missing");
    if(!d.instance_key.empty() && !values.contains(d.instance_key)) fail("spatial/points","instance key column missing");
    transforms_valid(d.transforms,d.axes);
}
SpatialPointsView::SpatialPointsView()=default;
SpatialPointsView::SpatialPointsView(std::shared_ptr<detail::SpatialPointsViewImpl> p):impl_(std::move(p)){}
std::size_t SpatialPointsView::size()const{return require(impl_).values.size();}
TableView SpatialPointsView::values()const{return require(impl_).values;}
ColumnView SpatialPointsView::identifiers()const{return require(impl_).ids;}
const std::vector<SpatialAxis>& SpatialPointsView::axes()const{return require(impl_).axes;}
std::string_view SpatialPointsView::feature_key()const{return require(impl_).feature_key;}
std::string_view SpatialPointsView::instance_key()const{return require(impl_).instance_key;}
const std::vector<SpatialNamedTransform>& SpatialPointsView::transforms()const{return require(impl_).transforms;}
SpatialPointsView SpatialPointsView::select(const Axis& a)const{
    const auto& d=require(impl_); axis_valid(a,d.values.size());
    auto v=std::make_shared<detail::SpatialPointsViewImpl>(d);v->values=d.values.select(a);v->ids=d.ids.select(a);return SpatialPointsView(std::move(v));
}
SpatialPoints SpatialPointsView::copy()const{
    const auto& d=require(impl_);SpatialPoints out(d.values.copy(),d.ids.copy(),d.axes);
    out.impl_->feature_key=d.feature_key;out.impl_->instance_key=d.instance_key;out.impl_->transforms=d.transforms;return out;
}

SpatialShapes::SpatialShapes():impl_(std::make_unique<detail::SpatialShapesImpl>()){}
SpatialShapes::~SpatialShapes()=default;
SpatialShapes::SpatialShapes(SpatialShapes&&)noexcept=default;
SpatialShapes& SpatialShapes::operator=(SpatialShapes&&)noexcept=default;
SpatialShapes SpatialShapes::circles(Array xy,Array r,Column ids,Table attrs){
    SpatialShapes s;s.impl_->kind=SpatialGeometryKind::circles;s.impl_->xy=std::move(xy);s.impl_->radii=std::move(r);s.impl_->ids=std::move(ids);
    if(!attrs.size() && s.impl_->ids.size())attrs=Table::numbered(s.impl_->ids.size());
    s.impl_->attributes=std::move(attrs);s.impl_->transforms.push_back({"global",SpatialTransform::identity({SpatialAxis::x,SpatialAxis::y})});return s;
}
SpatialShapes SpatialShapes::polygons(Array xy,Array rings,Array polygons,Array geometries,Column ids,Table attrs,Array types){
    SpatialShapes s;s.impl_->kind=SpatialGeometryKind::polygons;s.impl_->xy=std::move(xy);s.impl_->rings=std::move(rings);s.impl_->polygons=std::move(polygons);s.impl_->geometries=std::move(geometries);s.impl_->ids=std::move(ids);
    if(!attrs.size() && s.impl_->ids.size())attrs=Table::numbered(s.impl_->ids.size());
    s.impl_->attributes=std::move(attrs);
    if(!types.rank()){
        auto xy_view=s.impl_->xy.view();
        if(xy_view.rank()!=2)fail("spatial/shapes", "coordinates must be a rank-2 array");
        offsets_valid(s.impl_->rings.view(),xy_view.extent(0),"spatial/shapes/rings");
        offsets_valid(s.impl_->polygons.view(),s.impl_->rings.extent(0)-1,"spatial/shapes/polygons");
        offsets_valid(s.impl_->geometries.view(),s.impl_->polygons.extent(0)-1,"spatial/shapes/geometries");
        if(s.impl_->ids.size()==~std::size_t{0} || s.impl_->geometries.extent(0)!=s.impl_->ids.size()+1)
            fail("spatial/shapes", "geometry offsets differ from rows");
        types=Array::empty<std::uint8_t>({s.impl_->ids.size()});auto t=types.edit().dense<std::uint8_t>();auto g=s.impl_->geometries.view().dense<std::uint64_t>();
        for(std::size_t i=0;i<s.impl_->ids.size();++i)t.data()[i]=g.data()[std::ptrdiff_t(i+1)*g.stride(0)]-g.data()[std::ptrdiff_t(i)*g.stride(0)]>1?6:3;
    }
    s.impl_->types=std::move(types);s.impl_->transforms.push_back({"global",SpatialTransform::identity({SpatialAxis::x,SpatialAxis::y})});return s;
}
void SpatialShapes::set_transform(std::string s,SpatialTransform t){anndatapp::set_transform(impl_->transforms,std::move(s),t,{SpatialAxis::x,SpatialAxis::y});}
void SpatialShapes::set_transforms(std::vector<SpatialNamedTransform> ts){transforms_valid(ts,{SpatialAxis::x,SpatialAxis::y});impl_->transforms=std::move(ts);}
SpatialShapesView SpatialShapes::view()const{
    auto v=std::make_shared<detail::SpatialShapesViewImpl>();const auto& d=*impl_;v->kind=d.kind;v->xy=d.xy.view();
    if(d.kind==SpatialGeometryKind::circles)v->radii=d.radii.view();
    else{v->rings=d.rings.view();v->polygons=d.polygons.view();v->geometries=d.geometries.view();v->types=d.types.view();}
    v->ids=d.ids.view();v->attributes=d.attributes.view();v->transforms=d.transforms;return SpatialShapesView(std::move(v));
}
SpatialShapes SpatialShapes::copy()const{return view().copy();}
void SpatialShapes::validate(std::size_t scratch)const{view().validate(scratch);}
SpatialShapesView::SpatialShapesView()=default;
SpatialShapesView::SpatialShapesView(std::shared_ptr<detail::SpatialShapesViewImpl> p):impl_(std::move(p)){}
SpatialGeometryKind SpatialShapesView::kind()const{return require(impl_).kind;}
std::size_t SpatialShapesView::size()const{const auto& d=require(impl_);return d.rows.extent(d.ids.size());}
ColumnView SpatialShapesView::identifiers()const{const auto& d=require(impl_);return d.ids.select(d.rows);}
TableView SpatialShapesView::attributes()const{const auto& d=require(impl_);return d.attributes.select(d.rows);}
ArrayView SpatialShapesView::coordinates()const{return require(impl_).xy;}
ArrayView SpatialShapesView::radii()const{return require(impl_).radii;}
ArrayView SpatialShapesView::ring_offsets()const{return require(impl_).rings;}
ArrayView SpatialShapesView::polygon_offsets()const{return require(impl_).polygons;}
ArrayView SpatialShapesView::geometry_offsets()const{return require(impl_).geometries;}
ArrayView SpatialShapesView::geometry_types()const{return require(impl_).types;}
std::size_t SpatialShapesView::source_row(std::size_t i)const{const auto& d=require(impl_);if(i>=size())fail("spatial/shapes","geometry out of bounds",ErrorCode::out_of_bounds);return d.rows.index(i);}
const std::vector<SpatialNamedTransform>& SpatialShapesView::transforms()const{return require(impl_).transforms;}
void SpatialShapesView::validate(std::size_t scratch)const{
    const auto& p=*this;const auto& source=require(impl_);const auto source_count=source.ids.size();
    if(source.attributes.size()!=source_count)fail("spatial/shapes","attribute count differs from geometries",ErrorCode::shape_mismatch);
    auto xy=p.coordinates();xy.validate();p.identifiers().validate();p.attributes().validate();shape_coordinates(xy);ids_valid(p.identifiers(),p.size(),true,scratch);if(p.attributes().size()!=p.size()||p.attributes().contains("z"))fail("spatial/shapes","shape attributes differ from geometries or contain reserved z");if(p.attributes().contains("geometry")||(p.kind()==SpatialGeometryKind::circles&&p.attributes().contains("radius")))fail("spatial/shapes","attribute name conflicts with geometry payload");
        if(p.kind()==SpatialGeometryKind::circles){auto radius=p.radii();radius.validate();if(radius.dtype()!=DType::float64||radius.rank()!=1||radius.extent(0)!=source_count||xy.extent(0)!=source_count)fail("spatial/shapes","circle buffers differ from geometry count");auto r=radius.dense<double>();for(std::size_t i=0;i<p.size();++i){auto row=p.source_row(i);if(row>=xy.extent(0)||row>=radius.extent(0))fail("spatial/shapes","circle row out of bounds");const double v=r.data()[std::ptrdiff_t(row)*r.stride(0)];if(!__builtin_isfinite(v)||v<=0)fail("spatial/shapes","circle radius must be finite and positive");}}
        else{auto rings=p.ring_offsets(),polygons=p.polygon_offsets(),geometries=p.geometry_offsets();rings.validate();polygons.validate();geometries.validate();offsets_valid(rings,xy.extent(0),"spatial/shapes/rings");offsets_valid(polygons,rings.extent(0)-1,"spatial/shapes/polygons");offsets_valid(geometries,polygons.extent(0)-1,"spatial/shapes/geometries");auto types=p.geometry_types();types.validate();if(source_count==~std::size_t{0}||types.dtype()!=DType::uint8||types.rank()!=1||types.extent(0)!=source_count||geometries.extent(0)!=source_count+1)fail("spatial/shapes","geometry offsets/types differ from rows");auto g=geometries.dense<std::uint64_t>();auto t=types.dense<std::uint8_t>();for(std::size_t i=0;i<p.size();++i){const auto row=p.source_row(i);if(row>=types.extent(0))fail("spatial/shapes","geometry row out of bounds");auto type=t.data()[std::ptrdiff_t(row)*t.stride(0)];if(type!=3&&type!=6)fail("spatial/shapes","unsupported geometry type");if(type==3&&g.data()[std::ptrdiff_t(row+1)*g.stride(0)]-g.data()[std::ptrdiff_t(row)*g.stride(0)]!=1)fail("spatial/shapes","Polygon must contain exactly one polygon");}}
        transforms_valid(p.transforms(),{SpatialAxis::x,SpatialAxis::y});
}
SpatialShapesView SpatialShapesView::select(const Axis& rows)const{
    const auto& d=require(impl_);axis_valid(rows,size());std::vector<std::size_t> p; p.reserve(rows.extent(size()));
    for(std::size_t i=0;i<rows.extent(size());++i)p.push_back(source_row(rows.index(i)));
    auto v=std::make_shared<detail::SpatialShapesViewImpl>(d);v->rows=Axis::gather(p.data(),p.size());return SpatialShapesView(std::move(v));
}
SpatialShapes SpatialShapesView::copy()const{
    const auto& d=require(impl_);
    if(d.kind==SpatialGeometryKind::circles){
        std::vector<std::size_t> rows;for(std::size_t i=0;i<size();++i)rows.push_back(source_row(i));
        const Axis axes[]={Axis::gather(rows.data(),rows.size()),Axis::all()};
        auto out=SpatialShapes::circles(d.xy.select(axes,2).copy(),d.radii.select({axes[0]}).copy(),identifiers().copy(),attributes().copy());out.impl_->transforms=d.transforms;return out;
    }
    auto xy=d.xy.dense<double>();auto r=d.rings.dense<std::uint64_t>();auto p=d.polygons.dense<std::uint64_t>();auto g=d.geometries.dense<std::uint64_t>();auto types=d.types.dense<std::uint8_t>();
    std::vector<double> coords;std::vector<std::uint64_t> rings{0},polygons{0},geometries{0};std::vector<std::uint8_t> ts;
    for(std::size_t i=0;i<size();++i){const auto row=source_row(i);ts.push_back(types.data()[std::ptrdiff_t(row)*types.stride(0)]);
        for(std::uint64_t j=g.data()[std::ptrdiff_t(row)*g.stride(0)];j<g.data()[std::ptrdiff_t(row+1)*g.stride(0)];++j){
            for(std::uint64_t k=p.data()[std::ptrdiff_t(j)*p.stride(0)];k<p.data()[std::ptrdiff_t(j+1)*p.stride(0)];++k){
                for(std::uint64_t v=r.data()[std::ptrdiff_t(k)*r.stride(0)];v<r.data()[std::ptrdiff_t(k+1)*r.stride(0)];++v){coords.push_back(xy.data()[std::ptrdiff_t(v)*xy.stride(0)]);coords.push_back(xy.data()[std::ptrdiff_t(v)*xy.stride(0)+xy.stride(1)]);}
                rings.push_back(coords.size()/2);
            }polygons.push_back(rings.size()-1);
        }geometries.push_back(polygons.size()-1);
    }
    auto ca=Array::empty<double>({coords.size()/2,2});auto cp=ca.edit().dense<double>();for(std::size_t i=0;i<coords.size();++i)cp.data()[i]=coords[i];
    auto ta=Array::empty<std::uint8_t>({ts.size()});auto tp=ta.edit().dense<std::uint8_t>();for(std::size_t i=0;i<ts.size();++i)tp.data()[i]=ts[i];
    auto out=SpatialShapes::polygons(std::move(ca),offsets_array(rings),offsets_array(polygons),offsets_array(geometries),identifiers().copy(),attributes().copy(),std::move(ta));out.impl_->transforms=d.transforms;return out;
}

SpatialData::SpatialData():impl_(std::make_unique<detail::SpatialDataImpl>()){}
SpatialData::~SpatialData()=default;
SpatialData::SpatialData(SpatialData&&)noexcept=default;
SpatialData& SpatialData::operator=(SpatialData&&)noexcept=default;
namespace {
void can_insert(const detail::SpatialDataImpl& d,std::string_view name,SpatialElementKind kind){
    if(!spatial_detail::valid_element_name(name))fail(name,"invalid SpatialData element name");
    const auto lower=spatial_detail::lowercase_name(name);
    for(const auto& n:element_names(d))if(spatial_detail::lowercase_name(n)==lower){
        bool same=false;
        if(kind==SpatialElementKind::image || kind==SpatialElementKind::labels){auto it=find(d.rasters,n);same=it!=d.rasters.end() && it->second.view().kind()==kind;}
        else if(kind==SpatialElementKind::points)same=find(d.points,n)!=d.points.end();
        else if(kind==SpatialElementKind::shapes)same=find(d.shapes,n)!=d.shapes.end();
        else same=find(d.tables,n)!=d.tables.end();
        if(n!=name || !same)fail(name,"element name collides with another spatial element");
    }
}
}
void SpatialData::set_raster(std::string n,SpatialRaster x){can_insert(*impl_,n,x.view().kind());put(impl_->rasters,std::move(n),std::move(x));}
void SpatialData::set_points(std::string n,SpatialPoints x){can_insert(*impl_,n,SpatialElementKind::points);put(impl_->points,std::move(n),std::move(x));}
void SpatialData::set_shapes(std::string n,SpatialShapes x){can_insert(*impl_,n,SpatialElementKind::shapes);put(impl_->shapes,std::move(n),std::move(x));}
void SpatialData::set_table(std::string n,AnnData x,SpatialAnnotation a){can_insert(*impl_,n,SpatialElementKind::table);put(impl_->tables,std::move(n),detail::SpatialTable{std::move(x),std::move(a)});}
void SpatialData::set_metadata(Element x){if(x.view().kind()!=ElementKind::mapping)fail("spatial/metadata","dataset metadata must be a mapping");impl_->metadata=std::move(x);}
void SpatialData::erase(std::string_view name){
    auto remove=[&](auto& v){auto i=find(v,name);if(i!=v.end())v.erase(i);};remove(impl_->rasters);remove(impl_->points);remove(impl_->shapes);remove(impl_->tables);
}
SpatialDataView SpatialData::view()const{
    auto v=std::make_shared<detail::SpatialDataViewImpl>();
    for(const auto& x:impl_->rasters)v->rasters.emplace_back(x.first,x.second.view());
    for(const auto& x:impl_->points)v->points.emplace_back(x.first,x.second.view());
    for(const auto& x:impl_->shapes)v->shapes.emplace_back(x.first,x.second.view());
    for(const auto& x:impl_->tables)v->tables.emplace_back(x.first,detail::SpatialTableView{x.second.data.view(),x.second.annotation});
    v->metadata=impl_->metadata.view();return SpatialDataView(std::move(v));
}
SpatialData SpatialData::copy()const{return view().copy();}
void SpatialData::validate(std::size_t scratch)const{ view().validate(scratch); }
SpatialDataView::SpatialDataView()=default;
SpatialDataView::SpatialDataView(std::shared_ptr<detail::SpatialDataViewImpl> p):impl_(std::move(p)){}
std::vector<std::string> SpatialDataView::names()const{return element_names(require(impl_));}
bool SpatialDataView::contains(std::string_view n)const{const auto& d=require(impl_);return find(d.rasters,n)!=d.rasters.end()||find(d.points,n)!=d.points.end()||find(d.shapes,n)!=d.shapes.end()||find(d.tables,n)!=d.tables.end();}
SpatialElementKind SpatialDataView::kind(std::string_view n)const{const auto& d=require(impl_);auto i=find(d.rasters,n);if(i!=d.rasters.end())return i->second.kind();if(find(d.points,n)!=d.points.end())return SpatialElementKind::points;if(find(d.shapes,n)!=d.shapes.end())return SpatialElementKind::shapes;if(find(d.tables,n)!=d.tables.end())return SpatialElementKind::table;fail(n,"spatial element does not exist",ErrorCode::out_of_bounds);}
SpatialRasterView SpatialDataView::raster(std::string_view n)const{return get(require(impl_).rasters,n);}
SpatialPointsView SpatialDataView::points(std::string_view n)const{return get(require(impl_).points,n);}
SpatialShapesView SpatialDataView::shapes(std::string_view n)const{return get(require(impl_).shapes,n);}
AnnDataView SpatialDataView::table(std::string_view n)const{return get(require(impl_).tables,n).data;}
SpatialAnnotation SpatialDataView::annotation(std::string_view n)const{return get(require(impl_).tables,n).annotation;}
ElementView SpatialDataView::metadata()const{return require(impl_).metadata;}
SpatialDataView SpatialDataView::select(const std::vector<std::string>& names)const{
    validate_name_set(names);const auto& d=require(impl_);auto v=std::make_shared<detail::SpatialDataViewImpl>();v->metadata=d.metadata;
    for(const auto& n:names){switch(kind(n)){case SpatialElementKind::image:case SpatialElementKind::labels:v->rasters.emplace_back(n,get(d.rasters,n));break;case SpatialElementKind::points:v->points.emplace_back(n,get(d.points,n));break;case SpatialElementKind::shapes:v->shapes.emplace_back(n,get(d.shapes,n));break;case SpatialElementKind::table:v->tables.emplace_back(n,get(d.tables,n));break;}}
    return SpatialDataView(std::move(v));
}
SpatialData SpatialDataView::copy()const{
    const auto& d=require(impl_);SpatialData out;
    for(const auto& x:d.rasters)out.set_raster(x.first,x.second.copy());for(const auto& x:d.points)out.set_points(x.first,x.second.copy());for(const auto& x:d.shapes)out.set_shapes(x.first,x.second.copy());for(const auto& x:d.tables)out.set_table(x.first,x.second.data.copy(),x.second.annotation);out.set_metadata(d.metadata.copy());return out;
}

void SpatialDataView::validate(std::size_t scratch)const{
    const auto& d=require(impl_);validate_name_set(element_names(d));d.metadata.validate();
    if(d.metadata.kind()!=ElementKind::mapping)fail("spatial/metadata","dataset metadata must be a mapping");
    for(const auto& x:d.rasters){const auto r=x.second;const bool image=r.kind()==SpatialElementKind::image;
        const std::vector<SpatialAxis> a2=image?std::vector<SpatialAxis>{SpatialAxis::c,SpatialAxis::y,SpatialAxis::x}:std::vector<SpatialAxis>{SpatialAxis::y,SpatialAxis::x};
        const std::vector<SpatialAxis> a3=image?std::vector<SpatialAxis>{SpatialAxis::c,SpatialAxis::z,SpatialAxis::y,SpatialAxis::x}:std::vector<SpatialAxis>{SpatialAxis::z,SpatialAxis::y,SpatialAxis::x};
        if((r.kind()!=SpatialElementKind::image&&r.kind()!=SpatialElementKind::labels)||(r.axes()!=a2&&r.axes()!=a3)||!r.n_levels())fail(x.first,"invalid raster kind, axes or resolution levels");
        auto base=r.level(0);r.channel_labels().validate();for(std::size_t i=0;i<r.n_levels();++i){auto a=r.level(i);a.validate();if(a.layout()!=Layout::dense||a.rank()!=r.axes().size()||a.dtype()!=base.dtype())fail(x.first,"raster rank, layout or dtype differs");
            if(!image&&unsigned(a.dtype())>=unsigned(DType::float32))fail(x.first,"labels require integer or boolean arrays");
            for(std::size_t j=0;j<a.rank();++j){if(!a.extent(j)||(i && a.extent(j)>r.level(i-1).extent(j)))fail(x.first,"invalid pyramid extent");if(image&&j==0&&a.extent(j)!=base.extent(j))fail(x.first,"pyramid channel extents differ");}
        }
        if(r.channel_labels().kind()!=ElementKind::null&&(!image||r.channel_labels().size()!=base.extent(0)))fail(x.first,"channel labels differ from image channels");transforms_valid(r.transforms(),r.axes());
    }
    for(const auto& x:d.points){const auto p=x.second;auto values=p.values();values.validate();p.identifiers().validate();ids_valid(p.identifiers(),values.size(),false);
        if(p.axes()!=std::vector<SpatialAxis>{SpatialAxis::x,SpatialAxis::y}&&p.axes()!=std::vector<SpatialAxis>{SpatialAxis::x,SpatialAxis::y,SpatialAxis::z})fail(x.first,"point axes must be xy or xyz");
        for(auto axis:p.axes()){if(!values.contains(axis_name(axis)))fail(x.first,"point coordinate column missing");auto c=values.column(axis_name(axis));if(c.kind()!=ColumnKind::numeric)fail(x.first,"point coordinates must be numeric");auto type=c.values().dtype();if(type!=DType::int32&&type!=DType::int64&&type!=DType::float32&&type!=DType::float64)fail(x.first,"point coordinate dtype unsupported");}
        if((!p.feature_key().empty()&&!values.contains(p.feature_key()))||(!p.instance_key().empty()&&!values.contains(p.instance_key())))fail(x.first,"point annotation key missing");transforms_valid(p.transforms(),p.axes());
    }
    for(const auto& x:d.shapes)x.second.validate(scratch);
    for(const auto& x:d.tables){auto a=x.second.data;a.validate();table_names_valid(a);const auto& t=x.second.annotation;if(t.region_key.empty()!=t.instance_key.empty())fail(x.first,"both annotation link columns are needed");if(t.region_key.empty()){if(!t.regions.empty())fail(x.first,"regions require annotation columns");continue;}auto obs=a.obs();if(!obs.contains(t.region_key)||!obs.contains(t.instance_key))fail(x.first,"annotation columns missing");auto region=obs.column(t.region_key);auto ids=obs.column(t.instance_key);if(region.kind()!=ColumnKind::string&&region.kind()!=ColumnKind::categorical)fail(x.first,"annotation regions must be strings");if(region.kind()==ColumnKind::categorical&&region.categories().kind()!=ColumnKind::string)fail(x.first,"annotation region categories must be strings");ids_valid(ids,obs.size(),false);for(std::size_t i=0;i<obs.size();++i){if(!region.valid(i))fail(x.first,"annotation region cannot be null");auto name=region.kind()==ColumnKind::string?region.string(i):region.categories().string(std::size_t(region.code(i)));if(std::find(t.regions.begin(),t.regions.end(),name)==t.regions.end())fail(x.first,"annotation region is not declared");}annotation_unique(a,t,scratch);}
}
} // namespace anndatapp
