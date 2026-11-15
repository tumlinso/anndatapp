module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module anndatapp.spatialdata;
#if ANNDATAPP_HAS_ZARR
import anndatapp.zarr;
import anndatapp.backed;
#endif
#include "spatialdata_parquet.hpp"
#include "spatialdata_names.hpp"

namespace anndatapp {
namespace {
[[noreturn]] void fail(std::string_view p,const char* message,ErrorCode code=ErrorCode::unsupported_encoding){throw Error(code,std::string(p),message);}
std::string child(std::string_view p,std::string_view n){return p=="/"?"/"+std::string(n):std::string(p)+"/"+std::string(n);}
const char* axis_name(SpatialAxis a){switch(a){case SpatialAxis::x:return "x";case SpatialAxis::y:return "y";case SpatialAxis::z:return "z";case SpatialAxis::c:return "c";}fail("spatial/axes","unknown coordinate axis");}
SpatialAxis axis_value(std::string_view a){if(a=="x")return SpatialAxis::x;if(a=="y")return SpatialAxis::y;if(a=="z")return SpatialAxis::z;if(a=="c")return SpatialAxis::c;fail("spatial/axes","unsupported coordinate axis");}
std::string text(ElementView v,std::string_view p){if(v.kind()!=ElementKind::string)fail(p,"expected text metadata");return std::string(v.string());}
double number(ElementView v,std::string_view p){
    if(v.kind()==ElementKind::floating)return v.floating();if(v.kind()==ElementKind::signed_integer)return double(v.signed_integer());if(v.kind()==ElementKind::unsigned_integer)return double(v.unsigned_integer());fail(p,"expected numerical metadata");
}
std::size_t sequence_size(ElementView v,std::string_view p){if(v.kind()!=ElementKind::sequence)fail(p,"expected metadata sequence");return v.size();}
std::vector<SpatialAxis> parse_axes(ElementView v,bool objects){
    std::vector<SpatialAxis> out;for(std::size_t i=0;i<sequence_size(v,"spatial/axes");++i){auto a=v.at(i);out.push_back(axis_value(text(objects?a.at("name"):a,"spatial/axes")));}return out;
}
Element axes_metadata(const SpatialAxis* axes,std::size_t n,bool objects){
    auto out=Element::sequence();for(std::size_t i=0;i<n;++i){if(!objects)out.append(Element(axis_name(axes[i])));else{auto a=Element::mapping();a.set("name",Element(axis_name(axes[i])));a.set("type",Element(axes[i]==SpatialAxis::c?"channel":"space"));if(axes[i]!=SpatialAxis::c)a.set("unit",Element("unit"));out.append(std::move(a));}}return out;
}
Element coordinate_system(const SpatialAxis* axes,std::size_t n,std::string name){auto out=Element::mapping();out.set("name",Element(std::move(name)));out.set("axes",axes_metadata(axes,n,true));return out;}
SpatialTransform parse_transform(ElementView value,std::size_t depth=0){
    if(depth>64)fail("spatial/transform","transform sequence nesting exceeds 64 levels");
    const auto type=text(value.at("type"),"spatial/transform/type");
    auto in=parse_axes(value.at("input").at("axes"),true),out=parse_axes(value.at("output").at("axes"),true);
    if(in.empty()||in.size()>4||out.empty()||out.size()>4)fail("spatial/transform","unsupported transform rank");
    double m[20]{};const auto ni=in.size(),no=out.size();
    if(type=="sequence"){
        auto items=value.at("transformations");if(!sequence_size(items,"spatial/transform/transformations"))fail("spatial/transform","empty transform sequence");
        auto t=parse_transform(items.at(0),depth+1);for(std::size_t i=1;i<items.size();++i)t=compose(parse_transform(items.at(i),depth+1),t);
        if(t.input_rank!=ni||t.output_rank!=no)fail("spatial/transform","sequence axes differ from outer axes");
        for(std::size_t j=0;j<ni;++j)if(t.input_axes[j]!=in[j])fail("spatial/transform","sequence input axes differ");
        for(std::size_t i=0;i<no;++i)if(t.output_axes[i]!=out[i])fail("spatial/transform","sequence output axes differ");
        return t;
    }
    if(type=="affine"){
        auto rows=value.at("affine");if(sequence_size(rows,"spatial/transform/affine")!=no)fail("spatial/transform","affine row count differs from output rank");
        for(std::size_t i=0;i<no;++i){auto r=rows.at(i);if(sequence_size(r,"spatial/transform/affine")!=ni+1)fail("spatial/transform","affine column count differs from input rank");for(std::size_t j=0;j<=ni;++j)m[i*(ni+1)+j]=number(r.at(j),"spatial/transform/affine");}
    }else if(type=="identity"||type=="scale"||type=="translation"||type=="mapAxis"){
        if(type!="mapAxis" && in!=out)fail("spatial/transform","identity/scale/translation axes differ");
        auto values=type=="scale"||type=="translation"?value.at(type):ElementView{};
        if(type=="scale"||type=="translation")if(sequence_size(values,"spatial/transform")!=ni)fail("spatial/transform","transform vector rank differs");
        for(std::size_t i=0;i<no;++i){SpatialAxis source=out[i];if(type=="mapAxis"){auto map=value.at("mapAxis");if(map.contains(axis_name(out[i])))source=axis_value(text(map.at(axis_name(out[i])),"spatial/transform/mapAxis"));}
            std::size_t j=0;while(j<ni&&in[j]!=source)++j;if(j==ni)fail("spatial/transform","mapped output axis has no input");m[i*(ni+1)+j]=type=="scale"?number(values.at(j),"spatial/transform/scale"):1;
            if(type=="translation")m[i*(ni+1)+ni]=number(values.at(i),"spatial/transform/translation");
        }
    }else fail("spatial/transform","unsupported coordinate transformation type");
    return SpatialTransform::affine(in.data(),ni,out.data(),no,m,ni+1);
}
std::vector<SpatialNamedTransform> parse_transforms(ElementView values,const std::vector<SpatialAxis>& axes){
    std::vector<SpatialNamedTransform> out;
    for(std::size_t i=0;i<sequence_size(values,"spatial/coordinateTransformations");++i){auto value=values.at(i);auto system=text(value.at("output").at("name"),"spatial/transform/output/name");if(!spatial_detail::valid_element_name(system))fail(system,"invalid coordinate system name");auto t=parse_transform(value);if(t.input_rank!=axes.size())fail(system,"transform input rank differs from element");for(std::size_t j=0;j<axes.size();++j)if(t.input_axes[j]!=axes[j])fail(system,"transform input axes differ from element");for(const auto& previous:out)if(previous.coordinate_system==system)fail(system,"duplicate coordinate system");out.push_back({std::move(system),t});}
    if(out.empty())fail("spatial/coordinateTransformations","spatial element has no coordinate transformation");return out;
}
Element transforms_metadata(const std::vector<SpatialNamedTransform>& values){
    auto out=Element::sequence();for(const auto& value:values){const auto& t=value.transform;t.validate();auto v=Element::mapping();v.set("type",Element("affine"));std::string input_name;for(std::size_t j=0;j<t.input_rank;++j)input_name+=axis_name(t.input_axes[j]);v.set("input",coordinate_system(t.input_axes,t.input_rank,std::move(input_name)));v.set("output",coordinate_system(t.output_axes,t.output_rank,value.coordinate_system));auto rows=Element::sequence();for(std::size_t i=0;i<t.output_rank;++i){auto row=Element::sequence();for(std::size_t j=0;j<=t.input_rank;++j)row.append(Element(t.m[5*i+j]));rows.append(std::move(row));}v.set("affine",std::move(rows));out.append(std::move(v));}return out;
}
Element version_attrs(const char* version){auto out=Element::mapping();out.set("version",Element(version));return out;}
Element string_list(const std::vector<std::string>& values){auto out=Element::sequence();for(const auto& x:values)out.append(Element(x));return out;}
std::vector<std::string> parse_string_list(ElementView v){std::vector<std::string> out;for(std::size_t i=0;i<sequence_size(v,"spatial");++i)out.push_back(text(v.at(i),"spatial"));return out;}
std::string optional_text(ElementView attrs,std::string_view name){if(!attrs.contains(name)||attrs.at(name).kind()==ElementKind::null)return {};return text(attrs.at(name),name);}
SpatialAnnotation read_annotation(ElementView attrs){SpatialAnnotation a;a.region_key=optional_text(attrs,"region_key");a.instance_key=optional_text(attrs,"instance_key");if(attrs.contains("region")&&attrs.at("region").kind()!=ElementKind::null){auto regions=attrs.at("region");if(regions.kind()==ElementKind::string)a.regions.push_back(std::string(regions.string()));else a.regions=parse_string_list(regions);}return a;}
void validate_element_version(ElementView attrs,const char* current,const char* legacy,bool modern,std::string_view path){
    if(!attrs.contains("spatialdata_attrs")||!attrs.at("spatialdata_attrs").contains("version"))fail(path,"missing element format version");
    const auto version=text(attrs.at("spatialdata_attrs").at("version"),path);
    if(version!=(modern?current:legacy))fail(path,"unsupported or mismatched element format version",ErrorCode::unsupported_version);
}
std::string sidecar(const BackedStore& s,std::string_view path,const char* name){return (std::filesystem::path(s.location())/std::string(path.substr(1))/name).string();}
}
namespace detail {
struct SpatialStoredElement {
    std::string name,path;
    SpatialElementKind kind;
    std::vector<SpatialAxis> axes;
    std::vector<std::string> levels;
    std::vector<SpatialTransform> level_transforms;
    Element channels;
    std::vector<SpatialNamedTransform> transforms;
    SpatialAnnotation annotation;
    std::string feature_key,instance_key;
};
struct BackedSpatialDataImpl {
    BackedStore store;
    BackedOptions options;
    std::vector<SpatialStoredElement> elements;
    Element metadata;
};
}
namespace {
const detail::SpatialStoredElement& stored(const detail::BackedSpatialDataImpl* d,std::string_view name){if(!d||!d->store.is_open())fail("spatial","spatial store is closed",ErrorCode::invalid_argument);for(const auto& x:d->elements)if(x.name==name)return x;fail(name,"spatial element not found",ErrorCode::out_of_bounds);}
detail::SpatialStoredElement inspect_raster(BackedStore& store,std::string name,std::string path,SpatialElementKind kind,bool modern){
    auto attributes=store.attributes(path);auto attrs=attributes.view();auto ome=attrs.contains("ome")?attrs.at("ome"):attrs;
    auto ms=ome.at("multiscales");if(sequence_size(ms,path)!=1)fail(path,"raster must contain exactly one multiscales entry");auto scale=ms.at(0);
    const auto version=optional_text(ome,"version").empty()?optional_text(scale,"version"):optional_text(ome,"version");
    if(modern){if(version!="0.5-dev-spatialdata")fail(path,"unsupported current raster NGFF version",ErrorCode::unsupported_version);validate_element_version(attrs,"0.3","0.2",true,path);}
    else if(version!="0.4"&&version!="0.4-dev-spatialdata")fail(path,"unsupported legacy raster NGFF version",ErrorCode::unsupported_version);
    detail::SpatialStoredElement out;out.name=std::move(name);out.path=std::move(path);out.kind=kind;out.axes=parse_axes(scale.at("axes"),true);out.transforms=parse_transforms(scale.at("coordinateTransformations"),out.axes);
    auto datasets=scale.at("datasets");
    for(std::size_t i=0;i<sequence_size(datasets,out.path);++i){
        auto entry=datasets.at(i);
        auto p=text(entry.at("path"),out.path);
        if(!spatial_detail::valid_element_name(p))fail(out.path,"raster level must be a child array");

        auto raw=store.array(child(out.path,p));
        if(raw.rank()!=out.axes.size())fail(out.path,"raster array rank differs from axes");
        auto ts=entry.at("coordinateTransformations");
        if(!sequence_size(ts,out.path)||text(ts.at(0).at("type"),out.path)!="scale")fail(out.path,"raster level must start with scale metadata");
        auto factors=ts.at(0).at("scale");
        if(sequence_size(factors,out.path)!=out.axes.size())fail(out.path,"pyramid scale rank differs");

        double matrix[20]{};
        const auto rank=out.axes.size();
        for(std::size_t j=0;j<rank;++j){
            const auto factor=number(factors.at(j),out.path);
            if(!(factor>0))fail(out.path,"pyramid scales must be positive");
            matrix[j*(rank+1)+j]=factor;
        }
        for(std::size_t k=1;k<ts.size();++k){
            auto part=ts.at(k);
            if(text(part.at("type"),out.path)!="translation")fail(out.path,"unsupported per-level coordinate transformation");
            auto shift=part.at("translation");
            if(sequence_size(shift,out.path)!=rank)fail(out.path,"pyramid translation rank differs");
            for(std::size_t j=0;j<rank;++j)
                matrix[j*(rank+1)+rank]+=number(shift.at(j),out.path);
        }
        out.level_transforms.push_back(SpatialTransform::affine(out.axes.data(),rank,out.axes.data(),rank,matrix,rank+1));
        out.levels.push_back(std::move(p));
    }
    if(out.levels.empty())fail(out.path,"raster has no resolution levels");
    if(kind==SpatialElementKind::image){auto omero=ome.contains("omero")?ome.at("omero"):attrs.contains("omero")?attrs.at("omero"):ElementView{};
        if(omero.kind()==ElementKind::mapping&&omero.contains("channels")){out.channels=Element::sequence();auto cs=omero.at("channels");for(std::size_t i=0;i<sequence_size(cs,out.path);++i)out.channels.append(cs.at(i).at("label").copy());}
        else if(attrs.contains("channels_metadata")){out.channels=Element::sequence();auto cs=attrs.at("channels_metadata").at("channels");for(std::size_t i=0;i<sequence_size(cs,out.path);++i)out.channels.append(cs.at(i).at("label").copy());}
    }
    return out;
}
}

bool spatial_parquet_available()noexcept{
#if ANNDATAPP_HAS_PARQUET
    return true;
#else
    return false;
#endif
}
BackedSpatialData::BackedSpatialData()=default;
BackedSpatialData::~BackedSpatialData()=default;
BackedSpatialData::BackedSpatialData(BackedSpatialData&&)noexcept=default;
BackedSpatialData& BackedSpatialData::operator=(BackedSpatialData&&)noexcept=default;
BackedSpatialData::BackedSpatialData(std::unique_ptr<detail::BackedSpatialDataImpl> p):impl_(std::move(p)){}
std::vector<std::string> BackedSpatialData::names()const{if(!impl_||!impl_->store.is_open())fail("spatial","spatial store is closed",ErrorCode::invalid_argument);std::vector<std::string> out;for(const auto& x:impl_->elements)out.push_back(x.name);return out;}
SpatialElementKind BackedSpatialData::kind(std::string_view n)const{return stored(impl_.get(),n).kind;}
std::vector<std::string> BackedSpatialData::raster_level_paths(std::string_view n)const{const auto& x=stored(impl_.get(),n);if(x.kind!=SpatialElementKind::image&&x.kind!=SpatialElementKind::labels)fail(n,"element is not a raster",ErrorCode::invalid_argument);return x.levels;}
SpatialTransform BackedSpatialData::raster_level_transform(std::string_view n,std::size_t level)const{const auto& x=stored(impl_.get(),n);if(level>=x.level_transforms.size())fail(n,"resolution level out of bounds",ErrorCode::out_of_bounds);return x.level_transforms[level];}
std::vector<SpatialAxis> BackedSpatialData::axes(std::string_view n)const{return stored(impl_.get(),n).axes;}
std::vector<SpatialNamedTransform> BackedSpatialData::transforms(std::string_view n)const{return stored(impl_.get(),n).transforms;}
SpatialAnnotation BackedSpatialData::annotation(std::string_view n)const{const auto& x=stored(impl_.get(),n);if(x.kind!=SpatialElementKind::table)fail(n,"element is not a table",ErrorCode::invalid_argument);return x.annotation;}
BackedArray BackedSpatialData::raster(std::string_view n,std::size_t level)const{const auto& x=stored(impl_.get(),n);if(x.kind!=SpatialElementKind::image&&x.kind!=SpatialElementKind::labels)fail(n,"element is not a raster",ErrorCode::invalid_argument);if(level>=x.levels.size())fail(n,"resolution level out of bounds",ErrorCode::out_of_bounds);return impl_->store.array(child(x.path,x.levels[level]));}
BackedAnnData BackedSpatialData::table(std::string_view n)const{const auto& x=stored(impl_.get(),n);if(x.kind!=SpatialElementKind::table)fail(n,"element is not a table",ErrorCode::invalid_argument);return impl_->store.anndata(x.path);}
SpatialPoints BackedSpatialData::read_points(std::string_view n,std::size_t first,std::size_t count)const{const auto& x=stored(impl_.get(),n);if(x.kind!=SpatialElementKind::points)fail(n,"element is not points",ErrorCode::invalid_argument);auto p=spatial_detail::read_parquet_points(sidecar(impl_->store,x.path,"points.parquet"),first,count,x.axes,impl_->options.scratch_bytes);p.set_feature_key(x.feature_key);p.set_instance_key(x.instance_key);p.set_transforms(x.transforms);if(impl_->options.validation==Validation::full)p.validate();return p;}
SpatialShapes BackedSpatialData::read_shapes(std::string_view n,std::size_t first,std::size_t count)const{const auto& x=stored(impl_.get(),n);if(x.kind!=SpatialElementKind::shapes)fail(n,"element is not shapes",ErrorCode::invalid_argument);auto p=spatial_detail::read_parquet_shapes(sidecar(impl_->store,x.path,"shapes.parquet"),first,count,impl_->options.scratch_bytes);p.set_transforms(x.transforms);if(impl_->options.validation==Validation::full)p.validate(impl_->options.scratch_bytes);return p;}
SpatialData BackedSpatialData::read()const{
    if(!impl_||!impl_->store.is_open())fail("spatial","spatial store is closed",ErrorCode::invalid_argument);
    if(!spatial_parquet_available())for(const auto& x:impl_->elements)if(x.kind==SpatialElementKind::points||x.kind==SpatialElementKind::shapes)fail(x.path,"SpatialData vector payloads require Arrow/Parquet",ErrorCode::unavailable_filter);
    SpatialData out;out.set_metadata(impl_->metadata.copy());
    for(const auto& x:impl_->elements){
        switch(x.kind){
        case SpatialElementKind::image:
        case SpatialElementKind::labels:{
            SpatialRaster r(x.kind,x.axes);
            for(std::size_t i=0;i<x.levels.size();++i){
                const auto& p=x.levels[i];
                r.add_level(impl_->store.array(child(x.path,p)).read(),p);
                r.set_level_transform(i,x.level_transforms[i]);
            }
            if(x.channels.view().kind()!=ElementKind::null)r.set_channel_labels(x.channels.copy());
            r.set_transforms(x.transforms);
            out.set_raster(x.name,std::move(r));
            break;
        }
        case SpatialElementKind::points:
            out.set_points(x.name,read_points(x.name,0,std::numeric_limits<std::size_t>::max()));
            break;
        case SpatialElementKind::shapes:
            out.set_shapes(x.name,read_shapes(x.name));
            break;
        case SpatialElementKind::table:
            out.set_table(x.name,impl_->store.anndata(x.path).read(),x.annotation);
            break;
        }
    }
    if(impl_->options.validation==Validation::full)out.validate(impl_->options.scratch_bytes);
    return out;
}
void BackedSpatialData::close(){if(impl_)impl_->store.close();}
BackedSpatialData open_spatialdata(std::string_view path,BackedOptions options){
#if ANNDATAPP_HAS_ZARR
    auto d=std::make_unique<detail::BackedSpatialDataImpl>();d->store=open_zarr_store(path,options);d->options=options;d->metadata=d->store.attributes();auto root=d->metadata.view();std::string version;
    if(root.contains("spatialdata_attrs"))version=optional_text(root.at("spatialdata_attrs"),"version");
    const bool modern=version=="0.2";if(!version.empty()&&version!="0.1"&&version!="0.2")fail(path,"unsupported SpatialData container version",ErrorCode::unsupported_version);
    for(const auto* group:{"images","labels","points","shapes","tables"}){
        const auto parent=std::string("/")+group;if(!d->store.exists(parent))continue;
        for(const auto& name:d->store.children(parent)){
            if(!spatial_detail::valid_element_name(name))fail(name,"invalid SpatialData element name");
            for(const auto& previous:d->elements)if(spatial_detail::lowercase_name(previous.name)==spatial_detail::lowercase_name(name))fail(name,"case-insensitive spatial element collision");
            const auto p=child(parent,name);
            if(std::string_view(group)=="images"||std::string_view(group)=="labels"){d->elements.push_back(inspect_raster(d->store,name,p,std::string_view(group)=="images"?SpatialElementKind::image:SpatialElementKind::labels,modern));continue;}
            detail::SpatialStoredElement x;x.name=name;x.path=p;auto a=d->store.attributes(p);auto attrs=a.view();
            if(std::string_view(group)=="tables"){
                x.kind=SpatialElementKind::table;
                if(text(attrs.at("spatialdata-encoding-type"),p)!="ngff:regions_table")fail(p,"invalid spatial table marker");
                if(text(attrs.at("version"),p)!=(modern?"0.2":"0.1"))fail(p,"unsupported spatial table version",ErrorCode::unsupported_version);
                x.annotation=read_annotation(attrs);
            }else{
                const bool points=std::string_view(group)=="points";x.kind=points?SpatialElementKind::points:SpatialElementKind::shapes;
                if(text(attrs.at("encoding-type"),p)!=(points?"ngff:points":"ngff:shapes"))fail(p,"invalid vector element marker");
                validate_element_version(attrs,points?"0.2":"0.3",points?"0.1":"0.2",modern,p);
                x.axes=parse_axes(attrs.at("axes"),false);x.transforms=parse_transforms(attrs.at("coordinateTransformations"),x.axes);
                if(points){auto m=attrs.at("spatialdata_attrs");x.feature_key=optional_text(m,"feature_key");x.instance_key=optional_text(m,"instance_key");}
            }
            d->elements.push_back(std::move(x));
        }
    }
    return BackedSpatialData(std::move(d));
#else
    (void)options;fail(path,"SpatialData storage requires the Zarr backend",ErrorCode::unavailable_filter);
#endif
}
SpatialData read_spatialdata(std::string_view path,SpatialReadOptions options){BackedOptions b;b.validation=options.validation;b.scratch_bytes=options.scratch_bytes;return open_spatialdata(path,b).read();}

namespace {
Element raster_attrs(const SpatialRasterView& r){
    auto attrs=Element::mapping();
    attrs.set("spatialdata_attrs",version_attrs("0.3"));
    auto ome=Element::mapping();
    ome.set("version",Element("0.5-dev-spatialdata"));
    auto multiscales=Element::sequence();
    auto ms=Element::mapping();
    ms.set("name",Element("image"));
    ms.set("axes",axes_metadata(r.axes().data(),r.axes().size(),true));
    auto datasets=Element::sequence();
    for(std::size_t i=0;i<r.n_levels();++i){
        auto ds=Element::mapping();
        ds.set("path",Element(std::string(r.level_path(i))));
        auto ts=Element::sequence();
        auto scale=Element::mapping();
        scale.set("type",Element("scale"));
        auto factors=Element::sequence();
        const auto level=r.level_transform(i);
        for(std::size_t j=0;j<r.axes().size();++j)factors.append(Element(level.m[5*j+j]));
        scale.set("scale",std::move(factors));
        ts.append(std::move(scale));

        auto translation=Element::mapping();
        translation.set("type",Element("translation"));
        auto offsets=Element::sequence();
        for(std::size_t j=0;j<r.axes().size();++j)
            offsets.append(Element(level.m[5*j+level.input_rank]));
        translation.set("translation",std::move(offsets));
        ts.append(std::move(translation));
        ds.set("coordinateTransformations",std::move(ts));
        datasets.append(std::move(ds));
    }
    ms.set("datasets",std::move(datasets));
    ms.set("coordinateTransformations",transforms_metadata(r.transforms()));
    multiscales.append(std::move(ms));
    ome.set("multiscales",std::move(multiscales));

    if(r.kind()==SpatialElementKind::labels){
        auto label=Element::mapping();
        label.set("version",Element("0.5"));
        ome.set("image-label",std::move(label));
    }else{
        auto omero=Element::mapping();
        auto cs=Element::sequence();
        auto labels=r.channel_labels();
        const auto count=labels.kind()==ElementKind::null?r.level(0).extent(0):labels.size();
        for(std::size_t i=0;i<count;++i){
            auto c=Element::mapping();
            c.set("label",labels.kind()==ElementKind::null?Element(std::uint64_t(i)):labels.at(i).copy());
            cs.append(std::move(c));
        }
        omero.set("channels",std::move(cs));
        ome.set("omero",std::move(omero));
    }
    attrs.set("ome",std::move(ome));
    return attrs;
}
Element vector_attrs(const char* marker,const char* version,const std::vector<SpatialAxis>& axes,const std::vector<SpatialNamedTransform>& ts,std::string_view feature={},std::string_view instance={}){
    auto attrs=Element::mapping();attrs.set("encoding-type",Element(marker));attrs.set("axes",axes_metadata(axes.data(),axes.size(),false));attrs.set("coordinateTransformations",transforms_metadata(ts));auto spatial=version_attrs(version);if(!feature.empty())spatial.set("feature_key",Element(std::string(feature)));if(!instance.empty())spatial.set("instance_key",Element(std::string(instance)));attrs.set("spatialdata_attrs",std::move(spatial));return attrs;
}
Element table_attrs(const SpatialAnnotation& annotation){
    auto attrs=Element::mapping();
    attrs.set("spatialdata-encoding-type",Element("ngff:regions_table"));
    attrs.set("version",Element("0.2"));
    attrs.set("region",annotation.regions.empty()?Element(nullptr):string_list(annotation.regions));
    attrs.set("region_key",annotation.region_key.empty()?Element(nullptr):Element(annotation.region_key));
    attrs.set("instance_key",annotation.instance_key.empty()?Element(nullptr):Element(annotation.instance_key));
    return attrs;
}
}
void write_spatialdata(std::string_view path,const SpatialData& data,SpatialWriteOptions options){write_spatialdata(path,data.view(),options);}
void write_spatialdata(std::string_view path,const SpatialDataView& data,SpatialWriteOptions options){
#if ANNDATAPP_HAS_ZARR
    // Validation examines retained descriptors without copying numerical data.
    // Capability preflight precedes exclusive/truncate creation, so Arrow-off
    // cannot partially erase a store.
    if(options.validation==Validation::full)data.validate(options.scratch_bytes);
    const auto names=data.names();
    if(!spatial_parquet_available())for(const auto& n:names)
        if(data.kind(n)==SpatialElementKind::points||data.kind(n)==SpatialElementKind::shapes)
            fail(n,"SpatialData vector payloads require Arrow/Parquet",ErrorCode::unavailable_filter);
    if(!options.parquet_row_group_rows)fail(path,"Parquet row-group size must be nonzero",ErrorCode::invalid_argument);
    auto budgets=detail::write_budgets(detail::WriteBackend::zarr,options.scratch_bytes);
    for(const auto& n:names){
        auto kind=data.kind(n);
        if(kind==SpatialElementKind::image||kind==SpatialElementKind::labels){
            auto r=data.raster(n);
            auto attrs=raster_attrs(r);
            detail::preflight_zarr_attributes(attrs.view());
            for(std::size_t i=0;i<r.n_levels();++i)
                detail::preflight_array_write(r.level(i),options.validation,budgets,n);
        }else if(kind==SpatialElementKind::points){
            auto p=data.points(n);
            auto attrs=vector_attrs("ngff:points","0.2",p.axes(),p.transforms(),p.feature_key(),p.instance_key());
            detail::preflight_zarr_attributes(attrs.view());
            spatial_detail::preflight_parquet_points(n,p,options.parquet_row_group_rows,options.scratch_bytes);
        }else if(kind==SpatialElementKind::shapes){
            auto p=data.shapes(n);
            auto attrs=vector_attrs("ngff:shapes","0.3",{SpatialAxis::x,SpatialAxis::y},p.transforms());
            detail::preflight_zarr_attributes(attrs.view());
            spatial_detail::preflight_parquet_shapes(n,p,options.parquet_row_group_rows,options.scratch_bytes);
        }else{
            detail::preflight_anndata_write(data.table(n),options.validation,budgets);
            auto annotation=data.annotation(n);
            auto attrs=table_attrs(annotation);
            detail::preflight_zarr_attributes(attrs.view());
        }
    }
    ZarrWriteOptions z;
    z.format=ZarrFormat::v3;
    z.mode=options.exclusive?ZarrWriteMode::exclusive:ZarrWriteMode::truncate;
    z.validation=options.validation;
    z.scratch_bytes=options.scratch_bytes;

    auto root=data.metadata().copy();
    auto spatial=root.view().contains("spatialdata_attrs")?root.at("spatialdata_attrs").copy():Element::mapping();
    if(spatial.view().kind()!=ElementKind::mapping)fail(path,"spatialdata_attrs must be a mapping");
    spatial.set("version",Element("0.2"));
    root.set("spatialdata_attrs",std::move(spatial));
    detail::preflight_zarr_attributes(root.view());
    auto store=create_zarr_store(path,z);
    store.set_attributes("/",root.view());

    std::vector<std::string> labels;
    for(const auto& n:names){
        const auto kind=data.kind(n);
        const std::string group=kind==SpatialElementKind::image?"/images":kind==SpatialElementKind::labels?"/labels":kind==SpatialElementKind::points?"/points":kind==SpatialElementKind::shapes?"/shapes":"/tables";
        if(!store.exists(group))store.create_group(group);
        const auto p=child(group,n);

        if(kind==SpatialElementKind::image||kind==SpatialElementKind::labels){
            auto r=data.raster(n);
            auto attrs=raster_attrs(r);
            store.create_group(p,attrs.view());
            for(std::size_t i=0;i<r.n_levels();++i)
                store.write_array(child(p,r.level_path(i)),r.level(i));
            if(kind==SpatialElementKind::labels)labels.push_back(n);
        }else if(kind==SpatialElementKind::points){
            auto v=data.points(n);
            auto attrs=vector_attrs("ngff:points","0.2",v.axes(),v.transforms(),v.feature_key(),v.instance_key());
            store.create_group(p,attrs.view());
            spatial_detail::write_parquet_points(sidecar(store,p,"points.parquet"),v,options.parquet_row_group_rows,options.scratch_bytes);
        }else if(kind==SpatialElementKind::shapes){
            auto v=data.shapes(n);
            auto attrs=vector_attrs("ngff:shapes","0.3",{SpatialAxis::x,SpatialAxis::y},v.transforms());
            store.create_group(p,attrs.view());
            spatial_detail::write_parquet_shapes(sidecar(store,p,"shapes.parquet"),v,options.parquet_row_group_rows,options.scratch_bytes);
        }else{
            auto table=data.table(n);
            auto annotation=data.annotation(n);
            store.write_anndata(p,table,options.validation);
            auto attrs=table_attrs(annotation);
            store.set_attributes(p,attrs.view());

            const auto annotation_path=child(child(p,"uns"),"spatialdata_attrs");
            if(annotation.region_key.empty()){
                if(store.exists(annotation_path))store.erase(annotation_path);
            }else{
                auto m=Element::mapping();
                m.set("region",Element(StringArray::from_strings({annotation.regions.size()},annotation.regions)));
                m.set("region_key",Element(annotation.region_key));
                m.set("instance_key",Element(annotation.instance_key));
                store.write_element(annotation_path,m.view());
            }
        }
    }
    if(!labels.empty()){
        auto attrs=Element::mapping();
        auto ome=Element::mapping();
        ome.set("labels",string_list(labels));
        attrs.set("ome",std::move(ome));
        store.set_attributes("/labels",attrs.view());
    }
    store.flush();store.close();
#else
    (void)data;(void)options;fail(path,"SpatialData storage requires the Zarr backend",ErrorCode::unavailable_filter);
#endif
}
} // namespace anndatapp
