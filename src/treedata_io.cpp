module;
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <unordered_set>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#if ANNDATAPP_HAS_HDF5 || ANNDATAPP_HAS_ZARR
#include "treedata_json.hpp"
#endif

module anndatapp.treedata;

namespace anndatapp {
namespace detail {
bool tree_subset_connected(TreeView, TableView);
Tree tree_select(TreeView, TableView, TreeAlignment);
struct BackedTreeDataImpl {
    BackedStore store;
    BackedAnnData data;
    TreeAlignment alignment = TreeAlignment::leaves;
    bool overlap = true;
    std::optional<std::string> label = std::string("tree");
};
}
namespace {
[[noreturn]] void fail(std::string_view path, std::string message) { throw Error(ErrorCode::io_error, std::string(path), std::move(message)); }
#if ANNDATAPP_HAS_HDF5 || ANNDATAPP_HAS_ZARR
using Json = nlohmann::ordered_json;
Json parse_tree_value(std::string_view text, std::string_view path) {
    try { return detail::parse_tree_json(text); }
    catch (const std::out_of_range& e) { throw Error(ErrorCode::unsupported_dtype,std::string(path),e.what()); }
    catch (const Json::out_of_range& e) { throw Error(ErrorCode::unsupported_dtype,std::string(path),e.what()); }
    catch (const Json::parse_error& e) { throw Error(ErrorCode::unsupported_encoding,std::string(path),e.what()); }
}
Element from_json(const Json& j) {
    if (j.is_null()) return Element();
    if (j.is_boolean()) return Element(j.get<bool>());
    if (j.is_number_unsigned()) { auto value=j.get<std::uint64_t>(); if(value<=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return Element(static_cast<std::int64_t>(value)); return Element(value); }
    if (j.is_number_integer()) return Element(j.get<std::int64_t>());
    if (j.is_number_float()) return Element(j.get<double>());
    if (j.is_string()) return Element(j.get<std::string>());
    if (j.is_array()) { auto e = Element::sequence(); for (const auto& x : j) e.append(from_json(x)); return e; }
    if (j.is_object()) { auto e = Element::mapping(); for (auto it=j.begin(); it!=j.end(); ++it) e.set(it.key(),from_json(it.value())); return e; }
    fail("tree", "unsupported JSON value");
}
template<class T> Json dense_json(ArrayView a) {
    auto d=a.dense<T>();
    auto rec = [&](auto&& self, std::size_t axis, std::ptrdiff_t offset) -> Json {
        if (axis == d.rank()) return d.data()[offset];
        Json out=Json::array(); for (std::size_t i=0;i<d.extent(axis);++i) out.push_back(self(self,axis+1,offset+static_cast<std::ptrdiff_t>(i)*d.stride(axis))); return out;
    };
    return rec(rec,0,0);
}
Json array_json(ArrayView a) {
    if (a.layout()!=Layout::dense) fail("tree", "tree attributes require dense arrays");
    switch(a.dtype()) {
#define CASE(dt, type) case DType::dt: return dense_json<type>(a)
    CASE(boolean,bool); CASE(int8,std::int8_t); CASE(uint8,std::uint8_t); CASE(int16,std::int16_t); CASE(uint16,std::uint16_t); CASE(int32,std::int32_t); CASE(uint32,std::uint32_t); CASE(int64,std::int64_t); CASE(uint64,std::uint64_t); CASE(float32,float); CASE(float64,double);
#undef CASE
    } fail("tree", "unsupported array type");
}
Tree tree_from_json(const Json& g) {
    if (!g.is_object() || !g.contains("nodes") || !g.contains("edges") ||
        !g["nodes"].is_object() || !g["edges"].is_object()) fail("tree", "invalid tree JSON");
    std::vector<std::string> nodes;
    std::unordered_map<std::string, std::uint64_t> index;
    for (auto it=g["nodes"].begin(); it!=g["nodes"].end(); ++it) {
        index.emplace(it.key(), nodes.size());
        nodes.push_back(it.key());
    }
    std::vector<std::uint64_t> edges;
    std::vector<Json> attrs;
    for (auto s=g["edges"].begin(); s!=g["edges"].end(); ++s) {
        if (!s.value().is_object() || !index.count(s.key())) fail("tree", "invalid edge source");
        for (auto t=s.value().begin(); t!=s.value().end(); ++t) {
            if (!index.count(t.key())) fail("tree", "invalid edge target");
            edges.push_back(index.at(s.key()));
            edges.push_back(index.at(t.key()));
            attrs.push_back(t.value());
        }
    }
    auto out=Tree::from_edges(std::move(nodes), std::move(edges));
    std::size_t i=0;
    for (auto it=g["nodes"].begin(); it!=g["nodes"].end(); ++it)
        out.set_node_attributes(i++, from_json(it.value()));
    for (i=0; i<attrs.size(); ++i) out.set_edge_attributes(i, from_json(attrs[i]));
    return out;
}
std::vector<std::pair<std::string,Tree>> read_json_trees(std::string_view text, std::string_view path) {
    auto j=parse_tree_value(text, path);
    if (!j.is_object()) fail("tree", "tree collection must be a JSON object");
    std::vector<std::pair<std::string,Tree>> out;
    for (auto it=j.begin(); it!=j.end(); ++it) out.emplace_back(it.key(), tree_from_json(it.value()));
    return out;
}
void read_columns(BackedStore& s, const std::string& path, std::vector<Element>& attrs) {
    if (!s.exists(path)) return;
    for (auto& key:s.children(path)) {
        auto col=s.element(path+"/"+key).read();
        auto v=col.view();
        if (v.kind()==ElementKind::string_array) {
            auto a=v.string_array();
            if (a.rank()!=1 || a.size()!=attrs.size()) fail(path,"attribute column length mismatch");
            for (std::size_t i=0; i<attrs.size(); ++i) {
                auto x=a.string(i);
                if (!x.empty()) attrs[i].set(key,from_json(parse_tree_value(x,path+"/"+key)));
            }
        } else if (v.kind()==ElementKind::array) {
            auto a=v.array();
            if (a.rank()!=1 || a.extent(0)!=attrs.size()) fail(path,"attribute column length mismatch");
            auto j=array_json(a);
            for (std::size_t i=0; i<attrs.size(); ++i) attrs[i].set(key,from_json(j[i]));
        } else fail(path,"unsupported attribute column");
    }
}
bool scalar_trees(BackedStore& s, const std::string& path) {
    if (!s.exists(path)) return false;
    auto attrs=s.attributes(path);
    auto a=attrs.view();
    return a.contains("encoding-type") && a.at("encoding-type").kind()==ElementKind::string &&
           a.at("encoding-type").string()=="string";
}
std::vector<std::pair<std::string,Tree>> read_trees(BackedStore& s, const std::string& path,
                                                    std::string_view only = {}, bool topology_only = false) {
    if (!s.exists(path)) return {};
    if (scalar_trees(s,path)) return read_json_trees(s.read_string(path),path);
    std::vector<std::pair<std::string,Tree>> out;
    for (auto& name:s.children(path)) {
        if (!only.empty() && name!=only) continue;
        auto p=path+"/"+name;
        auto n=s.strings(p+"/nodes").read();
        auto es=s.strings(p+"/edges").read();
        auto nv=n.view(), ev=es.view();
        if (nv.rank()!=1 || ev.rank()!=2 || ev.extent(1)!=2) fail(p,"invalid node or edge shape");
        std::vector<std::string> nodes;
        std::unordered_map<std::string,std::uint64_t> index;
        for (std::size_t i=0; i<nv.size(); ++i) {
            nodes.emplace_back(nv.string(i));
            if (!index.emplace(nodes.back(),i).second) fail(p,"duplicate node");
        }
        std::vector<std::uint64_t> edges;
        for (std::size_t i=0; i<ev.size(); ++i) {
            auto it=index.find(std::string(ev.string(i)));
            if (it==index.end()) fail(p,"edge endpoint missing");
            edges.push_back(it->second);
        }
        auto t=Tree::from_edges(std::move(nodes),std::move(edges));
        if (topology_only) { out.emplace_back(name,std::move(t)); continue; }
        std::vector<Element> na,ea;
        for (std::size_t i=0; i<t.view().n_nodes(); ++i) na.push_back(Element::mapping());
        for (std::size_t i=0; i<t.view().n_edges(); ++i) ea.push_back(Element::mapping());
        read_columns(s,p+"/node_attrs",na);
        read_columns(s,p+"/edge_attrs",ea);
        for (std::size_t i=0; i<na.size(); ++i) t.set_node_attributes(i,std::move(na[i]));
        for (std::size_t i=0; i<ea.size(); ++i) t.set_edge_attributes(i,std::move(ea[i]));
        out.emplace_back(name,std::move(t));
    }
    return out;
}
// Select source rows before decoding each column. Backend chunk geometry may
// still require reading whole chunks containing the requested rows.
void read_selected_columns(BackedStore& store, const std::string& path,
                           std::size_t source_count, const std::vector<std::ptrdiff_t>& rows,
                           std::vector<Element>& attrs) {
    if(!store.exists(path)) return;
    for(const auto& key:store.children(path)) {
        const auto p=path+"/"+key; auto backed=store.element(p); auto kind=backed.kind();
        if(kind==ElementKind::string_array) {
            auto source=backed.string_array(); auto shape=source.shape();
            if(shape.size()!=1 || shape[0]!=source_count) fail(p,"attribute column length mismatch");
            auto column=source.select({indices(rows)}).read(); auto values=column.view();
            for(std::size_t i=0;i<rows.size();++i) { auto value=values.string(i); if(!value.empty()) attrs[i].set(key,from_json(parse_tree_value(value,p))); }
        } else if(kind==ElementKind::array) {
            auto source=backed.array(); if(source.rank()!=1 || source.extent(0)!=source_count) fail(p,"attribute column length mismatch");
            auto column=source.select({indices(rows)}).read(); auto values=array_json(column.view());
            for(std::size_t i=0;i<rows.size();++i) attrs[i].set(key,from_json(values[i]));
        } else fail(p,"unsupported attribute column");
    }
}
Tree read_projected_tree(BackedStore& store, const std::string& path, TreeView source,
                         TableView axis, TreeAlignment alignment) {
    auto projected=detail::tree_select(source,axis,alignment);
    auto selected=projected.view();
    std::unordered_set<std::string> kept;
    auto names=selected.node_ids();
    for (std::size_t i=0; i<names.size(); ++i) kept.emplace(names.string(i));
    std::vector<std::ptrdiff_t> node_rows,edge_rows;
    auto ids=source.node_ids();
    if (source.n_nodes()>static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
        source.n_edges()>static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
        fail(path,"tree exceeds selection index range");
    std::vector<std::uint8_t> keep(source.n_nodes());
    for (std::size_t i=0; i<source.n_nodes(); ++i) {
        if (kept.contains(std::string(ids.string(i)))) {
            keep[i]=1;
            node_rows.push_back(static_cast<std::ptrdiff_t>(i));
        }
    }
    auto edges=source.edges().dense<std::uint64_t>();
    for (std::size_t i=0; i<source.n_edges(); ++i) {
        auto src=edges.data()[i*edges.stride(0)];
        auto dst=edges.data()[i*edges.stride(0)+edges.stride(1)];
        if (keep[src] && keep[dst]) edge_rows.push_back(static_cast<std::ptrdiff_t>(i));
    }
    if (node_rows.size()!=selected.n_nodes() || edge_rows.size()!=selected.n_edges())
        fail(path,"projected tree row mapping mismatch");
    std::vector<Element> nodes,edges_attrs;
    for (std::size_t i=0; i<node_rows.size(); ++i) nodes.push_back(Element::mapping());
    for (std::size_t i=0; i<edge_rows.size(); ++i) edges_attrs.push_back(Element::mapping());
    read_selected_columns(store,path+"/node_attrs",source.n_nodes(),node_rows,nodes);
    read_selected_columns(store,path+"/edge_attrs",source.n_edges(),edge_rows,edges_attrs);
    for (std::size_t i=0; i<nodes.size(); ++i) projected.set_node_attributes(i,std::move(nodes[i]));
    for (std::size_t i=0; i<edges_attrs.size(); ++i) projected.set_edge_attributes(i,std::move(edges_attrs[i]));
    return projected;
}
Element encoding(const char* type, const char* version) { auto e=Element::mapping(); e.set("encoding-type",Element(type)); e.set("encoding-version",Element(version)); return e; }
// A counting pass and an emitting pass share the same serializer, so the
// indivisible HDF5 JSON scalar can be admitted before opening the destination.
struct JsonSink {
    std::string* output;
    std::size_t size=0, limit;
    void add(std::string_view text) {
        if (text.size()>limit-size) throw Error(ErrorCode::invalid_argument,"tree", "TreeData JSON exceeds scratch budget");
        size+=text.size();
        if (output) output->append(text);
    }
    void character(char c) { add(std::string_view(&c,1)); }
    void string(std::string_view text) {
        static constexpr char hex[]="0123456789abcdef";
        character('"');
        for (unsigned char c:text) {
            if (c=='"' || c=='\\') { character('\\'); character(static_cast<char>(c)); }
            else if (c<0x20) { char escape[6]={'\\','u','0','0',hex[c>>4],hex[c&15]}; add({escape,6}); }
            else character(static_cast<char>(c));
        }
        character('"');
    }
    template<class T> void number(T value) {
        if constexpr (std::is_floating_point_v<T>) {
            if (std::isnan(value)) { add("NaN"); return; }
            if (std::isinf(value)) { add(value<0?"-Infinity":"Infinity"); return; }
        }
        std::array<char,128> buf;
        auto result=std::to_chars(buf.data(),buf.data()+buf.size(),value);
        if (result.ec!=std::errc{}) throw Error(ErrorCode::unsupported_dtype,"tree","cannot serialize number");
        std::string_view text(buf.data(),static_cast<std::size_t>(result.ptr-buf.data()));
        add(text);
        if constexpr (std::is_floating_point_v<T>)
            if (text.find_first_of(".eE")==std::string_view::npos) add(".0");
    }
};
template<class T> void emit_dense(JsonSink& out, ArrayView array) {
    auto d=array.dense<T>();
    auto visit=[&](auto&& self,std::size_t axis,std::ptrdiff_t offset)->void {
        if (axis==d.rank()) {
            if constexpr (std::is_same_v<T,bool>) out.add(d.data()[offset]?"true":"false");
            else out.number(d.data()[offset]);
            return;
        }
        out.character('[');
        for (std::size_t i=0;i<d.extent(axis);++i) {
            if (i) out.character(',');
            self(self,axis+1,offset+static_cast<std::ptrdiff_t>(i)*d.stride(axis));
        }
        out.character(']');
    };
    visit(visit,0,0);
}
void emit_element(JsonSink& out, ElementView value) {
    switch(value.kind()) {
    case ElementKind::null: out.add("null"); return;
    case ElementKind::boolean: out.add(value.boolean()?"true":"false"); return;
    case ElementKind::signed_integer: out.number(value.signed_integer()); return;
    case ElementKind::unsigned_integer: out.number(value.unsigned_integer()); return;
    case ElementKind::floating: out.number(value.floating()); return;
    case ElementKind::string: out.string(value.string()); return;
    case ElementKind::mapping:
        out.character('{');
        for (std::size_t i=0;i<value.mapping_size();++i) {
            if (i) out.character(','); auto key=value.mapping_key(i);
            out.string(key); out.character(':'); emit_element(out,value.at(key));
        }
        out.character('}'); return;
    case ElementKind::sequence:
        out.character('[');
        for (std::size_t i=0;i<value.size();++i) { if (i) out.character(','); emit_element(out,value.at(i)); }
        out.character(']'); return;
    case ElementKind::string_array: {
        auto a=value.string_array(); std::size_t pos=0;
        auto visit=[&](auto&& self,std::size_t axis)->void {
            if (axis==a.rank()) { out.string(a.string(pos++)); return; }
            out.character('[');
            for (std::size_t i=0;i<a.extent(axis);++i) { if (i) out.character(','); self(self,axis+1); }
            out.character(']');
        };
        visit(visit,0); return;
    }
    case ElementKind::array: {
        auto a=value.array(); if (a.layout()!=Layout::dense) throw Error(ErrorCode::unsupported_layout,"tree","tree attribute array must be dense");
        switch(a.dtype()) {
#define EMIT_CASE(dt,type) case DType::dt: emit_dense<type>(out,a); return
        EMIT_CASE(boolean,bool); EMIT_CASE(int8,std::int8_t); EMIT_CASE(uint8,std::uint8_t);
        EMIT_CASE(int16,std::int16_t); EMIT_CASE(uint16,std::uint16_t); EMIT_CASE(int32,std::int32_t);
        EMIT_CASE(uint32,std::uint32_t); EMIT_CASE(int64,std::int64_t); EMIT_CASE(uint64,std::uint64_t);
        EMIT_CASE(float32,float); EMIT_CASE(float64,double);
#undef EMIT_CASE
        }
        break;
    }
    default: break;
    }
    throw Error(ErrorCode::unsupported_encoding,"tree","unsupported tree attribute kind");
}
void emit_tree(JsonSink& out, TreeView tree, std::size_t index_budget) {
    const auto n=tree.n_nodes(), m=tree.n_edges();
    const auto max=std::numeric_limits<std::size_t>::max();
    if (n>max/2 || 2*n>max-m || 2*n+m>index_budget/sizeof(std::size_t))
        throw Error(ErrorCode::invalid_argument,"tree","tree adjacency exceeds scratch budget");
    const auto none=max;
    std::vector<std::size_t> head(n,none), tail(n,none), next(m,none);
    auto e=tree.edges().dense<std::uint64_t>();
    for (std::size_t i=0;i<m;++i) {
        auto src=e.data()[i*e.stride(0)];
        if (head[src]==none) head[src]=i;
        else next[tail[src]]=i;
        tail[src]=i;
    }
    auto ids=tree.node_ids();
    out.add("{\"edges\":{");
    for (std::size_t i=0;i<n;++i) {
        if (i) out.character(',');
        out.string(ids.string(i)); out.add(":{");
        bool first=true;
        for (auto edge=head[i];edge!=none;edge=next[edge]) {
            if (!first) out.character(','); first=false;
            auto dst=e.data()[edge*e.stride(0)+e.stride(1)];
            out.string(ids.string(dst)); out.character(':'); emit_element(out,tree.edge_attributes(edge));
        }
        out.character('}');
    }
    out.add("},\"nodes\":{");
    for (std::size_t i=0;i<n;++i) {
        if (i) out.character(',');
        out.string(ids.string(i)); out.character(':'); emit_element(out,tree.node_attributes(i));
    }
    out.add("}}");
}
void emit_tree_collection(JsonSink& out, const TreeDataView& td, bool obs, std::size_t index_budget) {
    const auto count=obs?td.obs_tree_count():td.var_tree_count();
    out.character('{');
    for (std::size_t i=0;i<count;++i) {
        if (i) out.character(',');
        auto name=obs?td.obs_tree_name(i):td.var_tree_name(i);
        out.string(name); out.character(':');
        emit_tree(out,obs?td.obs_tree(name):td.var_tree(name),index_budget);
    }
    out.character('}');
}
std::size_t preflight_hdf5_trees(const TreeDataView& td,bool obs,std::size_t budget) {
    JsonSink count{nullptr,0,budget/4};
    emit_tree_collection(count,td,obs,budget/4);
    std::string probe;
    probe.reserve(count.size);
    if (probe.capacity()>budget/4)
        throw Error(ErrorCode::invalid_argument,"tree","TreeData JSON allocation exceeds scratch budget");
    return count.size;
}
// Gather borrowed keys only; the growth check accounts for old and new
// vector allocations being live during reserve.
std::vector<std::string_view> column_keys(TreeView tree, bool nodes, std::size_t budget) {
    std::vector<std::string_view> keys;
    const auto count=nodes?tree.n_nodes():tree.n_edges();
    for (std::size_t i=0;i<count;++i) {
        auto attrs=nodes?tree.node_attributes(i):tree.edge_attributes(i);
        for (std::size_t j=0;j<attrs.mapping_size();++j) {
            auto key=attrs.mapping_key(j);
            if (std::find(keys.begin(),keys.end(),key)!=keys.end()) continue;
            if (keys.size()==keys.capacity()) {
                auto next=keys.capacity()?keys.capacity()*2:1;
                if (next<keys.capacity() || next>budget/sizeof(std::string_view) ||
                    keys.capacity()>budget/sizeof(std::string_view)-next)
                    throw Error(ErrorCode::invalid_argument,"tree","attribute keys exceed scratch budget");
                keys.reserve(next);
                if (keys.capacity()>budget/sizeof(std::string_view))
                    throw Error(ErrorCode::invalid_argument,"tree","attribute keys exceed scratch budget");
            }
            keys.push_back(key);
        }
    }
    return keys;
}
void write_node_ids(BackedStore& store,const std::string& path,StringArrayView ids,std::size_t budget) {
    const std::size_t limit=budget/8;
    std::size_t first=0;
    while (first<ids.size()) {
        std::vector<std::string> values;
        values.reserve(std::min({std::size_t(64),ids.size()-first,limit/(2*sizeof(std::string))}));
        std::size_t used=values.capacity()*sizeof(std::string);
        while (first+values.size()<ids.size()) {
            auto value=ids.string(first+values.size());
            auto payload=std::max<std::size_t>(value.size()+1,16);
            if (!values.empty() && (values.size()==values.capacity() || used>limit || payload>limit-used)) break;
            if (used>limit || payload>limit-used)
                throw Error(ErrorCode::invalid_argument,path,"node ID batch exceeds scratch budget");
            values.emplace_back(value);
            used+=std::max<std::size_t>(values.back().capacity()+1,16);
        }
        store.write_strings(path,{first},{values.size()},values);
        first+=values.size();
    }
}
void write_edge_ids(BackedStore& store,const std::string& path,TreeView tree,std::size_t budget) {
    auto ids=tree.node_ids(); auto edges=tree.edges().dense<std::uint64_t>();
    const std::size_t limit=budget/8;
    std::size_t first=0;
    while (first<tree.n_edges()) {
        std::vector<std::string> values;
        values.reserve(2*std::min({std::size_t(32),tree.n_edges()-first,limit/(4*sizeof(std::string))}));
        std::size_t used=values.capacity()*sizeof(std::string);
        while (first+values.size()/2<tree.n_edges()) {
            const auto i=first+values.size()/2;
            auto src=ids.string(edges.data()[i*edges.stride(0)]);
            auto dst=ids.string(edges.data()[i*edges.stride(0)+edges.stride(1)]);
            auto bytes=std::max<std::size_t>(src.size()+1,16)+std::max<std::size_t>(dst.size()+1,16);
            if (!values.empty() && (values.size()+2>values.capacity() || used>limit || bytes>limit-used)) break;
            if (used>limit || bytes>limit-used)
                throw Error(ErrorCode::invalid_argument,path,"edge ID batch exceeds scratch budget");
            values.emplace_back(src);
            values.emplace_back(dst);
            used+=std::max<std::size_t>(values[values.size()-2].capacity()+1,16);
            used+=std::max<std::size_t>(values.back().capacity()+1,16);
        }
        auto rows=values.size()/2;
        store.write_strings(path,{first,0},{rows,2},values);
        first+=rows;
    }
}
template<class T,class F> void write_numeric_column(BackedStore& store,const std::string& path,
                                                     TreeView tree,bool nodes,std::size_t count,
                                                     std::string_view key,std::size_t budget,F get) {
    auto attrs=encoding("array","0.2.0");
    auto target=store.create_array(path,dtype_of<T>(),{count},attrs.view());
    if (!count) return;
    auto transfer=target.prepare_transfer();
    const auto batch_size=std::max<std::size_t>(1,std::min<std::size_t>(4096,budget/(16*sizeof(T))));
    auto values=std::make_unique<T[]>(batch_size);
    for (std::size_t first=0;first<count;first+=batch_size) {
        auto n=std::min(batch_size,count-first);
        for (std::size_t i=0;i<n;++i) {
            auto attrs=nodes?tree.node_attributes(first+i):tree.edge_attributes(first+i);
            values[i]=get(attrs.at(key));
        }
        transfer.write_from({first},{n},DenseBuffer::from(values.get(),{n}));
    }
}
void write_columns(BackedStore& s,const std::string& path,TreeView tree,bool nodes,std::size_t budget) {
    const auto count=nodes?tree.n_nodes():tree.n_edges();
    auto keys=column_keys(tree,nodes,budget/8);
    if (keys.empty()) return;
    auto dict=encoding("dict","0.1.0");
    s.create_group(path,dict.view());
    for (auto key:keys) {
        ElementKind kind=ElementKind::null; bool dense=count>0;
        for (std::size_t i=0;i<count;++i) {
            auto attrs=nodes?tree.node_attributes(i):tree.edge_attributes(i);
            if (!attrs.contains(key)) { dense=false; break; }
            auto next=attrs.at(key).kind();
            if (!i) kind=next;
            else if (next!=kind) { dense=false; break; }
        }
        const auto p=path+"/"+std::string(key);
        if (dense && kind==ElementKind::boolean)
            write_numeric_column<bool>(s,p,tree,nodes,count,key,budget,[](ElementView e){return e.boolean();});
        else if (dense && kind==ElementKind::signed_integer)
            write_numeric_column<std::int64_t>(s,p,tree,nodes,count,key,budget,[](ElementView e){return e.signed_integer();});
        else if (dense && kind==ElementKind::unsigned_integer)
            write_numeric_column<std::uint64_t>(s,p,tree,nodes,count,key,budget,[](ElementView e){return e.unsigned_integer();});
        else if (dense && kind==ElementKind::floating)
            write_numeric_column<double>(s,p,tree,nodes,count,key,budget,[](ElementView e){return e.floating();});
        else {
            auto attrs=encoding("string-array","0.2.0");
            s.create_strings(p,{count},attrs.view());
            std::vector<std::string> batch;
            batch.reserve(std::min<std::size_t>(64,budget/(16*sizeof(std::string))));
            std::size_t first=0, used=batch.capacity()*sizeof(std::string);
            auto flush=[&] {
                if (batch.empty()) return;
                s.write_strings(p,{first},{batch.size()},batch);
                first+=batch.size();
                batch.clear();
                used=batch.capacity()*sizeof(std::string);
            };
            for (std::size_t i=0;i<count;++i) {
                auto values=nodes?tree.node_attributes(i):tree.edge_attributes(i);
                std::string json;
                if (values.contains(key)) {
                    JsonSink measure{nullptr,0,budget/8}; emit_element(measure,values.at(key));
                    json.reserve(measure.size);
                    JsonSink out{&json,0,measure.size}; emit_element(out,values.at(key));
                }
                const auto payload=std::max<std::size_t>(json.capacity()+1,16);
                if (!batch.empty() && (batch.size()==batch.capacity() || used>budget/8 || payload>budget/8-used)) flush();
                batch.push_back(std::move(json));
                used+=payload;
            }
            flush();
        }
    }
}
void write_trees(BackedStore& s, const std::string& path, const TreeDataView& td, bool obs, bool zarr, std::size_t budget) {
    if (!zarr) {
        auto size=preflight_hdf5_trees(td,obs,budget);
        std::string json;
        json.reserve(size);
        JsonSink out{&json,0,size};
        emit_tree_collection(out,td,obs,budget/4);
        auto attrs=encoding("string","0.2.0");
        s.write_string(path,json,attrs.view());
        return;
    }
    const auto count=obs?td.obs_tree_count():td.var_tree_count();
    s.create_group(path);
    for (std::size_t i=0;i<count;++i) {
        auto name=obs?td.obs_tree_name(i):td.var_tree_name(i);
        auto t=obs?td.obs_tree(name):td.var_tree(name);
        auto p=path+"/"+std::string(name);
        s.create_group(p);
        auto attrs=encoding("string-array","0.2.0");
        s.create_strings(p+"/nodes",{t.n_nodes()},attrs.view());
        auto ids=t.node_ids();
        write_node_ids(s,p+"/nodes",ids,budget);
        s.create_strings(p+"/edges",{t.n_edges(),2},attrs.view());
        write_edge_ids(s,p+"/edges",t,budget);
        write_columns(s,p+"/node_attrs",t,true,budget);
        write_columns(s,p+"/edge_attrs",t,false,budget);
    }
}
void validate_zarr_tree_key(std::string_view key, std::string_view context) {
    if(key.empty() || key=="." || key==".." || key.find('/')!=std::string_view::npos || key.find('\0')!=std::string_view::npos)
        throw Error(ErrorCode::invalid_argument,std::string(context),"tree and attribute keys must be nonempty Zarr path components without slash or NUL");
}
void preflight_zarr(const TreeDataView& data,std::size_t budget) {
    for (bool obs : {true,false}) {
        const auto count=obs?data.obs_tree_count():data.var_tree_count();
        for (std::size_t n=0;n<count;++n) {
            auto name=obs?data.obs_tree_name(n):data.var_tree_name(n);
            validate_zarr_tree_key(name,obs?"obst":"vart");
            if (name.size()>budget/32) throw Error(ErrorCode::invalid_argument,"tree","tree name exceeds scratch budget");
            auto t=obs?data.obs_tree(name):data.var_tree(name);
            auto ids=t.node_ids();
            for (std::size_t i=0;i<ids.size();++i) {
                auto id=ids.string(i);
                if (id.size()>budget/64-1) throw Error(ErrorCode::invalid_argument,"tree","node ID exceeds scratch budget");
                std::string probe(id);
                if (probe.capacity()+1>budget/64)
                    throw Error(ErrorCode::invalid_argument,"tree","node ID exceeds scratch budget");
            }
            for (bool nodes : {true,false}) {
                auto keys=column_keys(t,nodes,budget/8);
                for (auto key:keys) {
                    validate_zarr_tree_key(key,nodes?"node_attrs":"edge_attrs");
                    if (key.size()>budget/32) throw Error(ErrorCode::invalid_argument,"tree","attribute name exceeds scratch budget");
                }
                const auto rows=nodes?t.n_nodes():t.n_edges();
                for (std::size_t i=0;i<rows;++i) {
                    auto attrs=nodes?t.node_attributes(i):t.edge_attributes(i);
                    for (std::size_t k=0;k<attrs.mapping_size();++k) {
                        auto key=attrs.mapping_key(k);
                        JsonSink measure{nullptr,0,budget/8};
                        emit_element(measure,attrs.at(key));
                        std::string probe; probe.reserve(measure.size);
                        if (probe.capacity()>budget/8)
                            throw Error(ErrorCode::invalid_argument,"tree","attribute value exceeds scratch budget");
                    }
                }
            }
        }
    }
}
const char* alignment_name(TreeAlignment a) { switch(a) { case TreeAlignment::leaves:return "leaves"; case TreeAlignment::nodes:return "nodes"; case TreeAlignment::subset:return "subset"; } fail("alignment","invalid alignment"); }
TreeAlignment parse_alignment(std::string_view s) { if(s=="leaves") return TreeAlignment::leaves; if(s=="nodes") return TreeAlignment::nodes; if(s=="subset") return TreeAlignment::subset; fail("alignment","unknown alignment"); }
std::unique_ptr<detail::BackedTreeDataImpl> load(BackedStore s) {
    auto p=std::make_unique<detail::BackedTreeDataImpl>();
    p->store=std::move(s);
    auto attrs=p->store.attributes("/");
    auto av=attrs.view();
    if (!av.contains("encoding-type") || av.at("encoding-type").kind()!=ElementKind::string ||
        av.at("encoding-type").string()!="treedata")
        throw Error(ErrorCode::unsupported_encoding,"/","not a TreeData store");
    if (!av.contains("encoding-version") || av.at("encoding-version").kind()!=ElementKind::string ||
        av.at("encoding-version").string()!="0.1.0")
        throw Error(ErrorCode::unsupported_version,"/","unsupported TreeData version");
    p->data=p->store.anndata("/");
    if (p->store.exists("/alignment"))
        p->alignment=parse_alignment(p->store.element("/alignment").read().view().string());
    if (p->store.exists("/allow_overlap"))
        p->overlap=p->store.element("/allow_overlap").read().view().boolean();
    if (p->store.exists("/label")) {
        auto e=p->store.element("/label").read();
        if (e.view().kind()==ElementKind::string) p->label=e.view().string();
        else if (e.view().kind()==ElementKind::null) p->label.reset();
        else fail("label","invalid label");
    }
    return p;
}
void write(BackedStore s, const TreeDataView& td, bool zarr, std::size_t budget) {
    s.write_anndata("/",td.data());
    if (!td.data().has_raw() && s.exists("/raw")) s.erase("/raw");
    auto a=Element::mapping();
    a.set("encoding-type",Element("treedata"));
    a.set("encoding-version",Element("0.1.0"));
    s.set_attributes("/",a.view());
    auto al=Element(alignment_name(td.alignment()));
    s.write_element("/alignment",al.view());
    auto overlap=Element(td.allow_overlap());
    s.write_element("/allow_overlap",overlap.view());
    auto label=td.has_label()?Element(std::string(td.label())):Element();
    s.write_element("/label",label.view());
    write_trees(s,"/obst",td,true,zarr,budget);
    write_trees(s,"/vart",td,false,zarr,budget);
    s.flush();
    s.close();
}
#endif
}
BackedTreeData::BackedTreeData()=default; BackedTreeData::~BackedTreeData()=default; BackedTreeData::BackedTreeData(BackedTreeData&&) noexcept=default; BackedTreeData& BackedTreeData::operator=(BackedTreeData&&) noexcept=default; BackedTreeData::BackedTreeData(std::unique_ptr<detail::BackedTreeDataImpl> p):impl_(std::move(p)) {}
bool BackedTreeData::is_open() const noexcept { return impl_ && impl_->store.is_open(); }
BackedAnnDataView BackedTreeData::data() const { if(!is_open()) fail("/","store is closed"); return impl_->data.view(); }
TreeAlignment BackedTreeData::alignment() const { if(!is_open()) fail("/","store is closed"); return impl_->alignment; }
bool BackedTreeData::allow_overlap() const { if(!is_open()) fail("/","store is closed"); return impl_->overlap; }
bool BackedTreeData::has_label() const { if(!is_open()) fail("/","store is closed"); return impl_->label.has_value(); }
std::string_view BackedTreeData::label() const { if(!is_open()) fail("/","store is closed"); return impl_->label ? std::string_view(*impl_->label) : std::string_view(); }
#if ANNDATAPP_HAS_HDF5 || ANNDATAPP_HAS_ZARR
// Zarr tree groups are loaded individually on demand. A scalar JSON collection
// (HDF5 and legacy Zarr) must be decoded as a unit on each access; no tree cache is retained.
std::vector<std::string> tree_names(detail::BackedTreeDataImpl& p, bool obs) {
    auto path=obs?"/obst":"/vart";
    if(!p.store.exists(path)) return {};
    if(!scalar_trees(p.store,path)) return p.store.children(path);
    auto trees=read_trees(p.store,path); std::vector<std::string> names;
    for(auto& [k,t]:trees) names.push_back(k); return names;
}
TreeView find_tree(detail::BackedTreeDataImpl& p,bool obs,std::string_view key) {
    auto path=obs?"/obst":"/vart";
    if(key.empty() && !scalar_trees(p.store,path)) throw Error(ErrorCode::out_of_bounds,std::string(key),"tree not found");
    auto trees=read_trees(p.store,path,key);
    for(auto& [k,t]:trees) if(k==key) return t.view();
    throw Error(ErrorCode::out_of_bounds,std::string(key),"tree not found");
}
#endif
TreeView BackedTreeData::obs_tree(std::string_view key) const { if(!is_open()) fail("/","store is closed");
#if ANNDATAPP_HAS_HDF5 || ANNDATAPP_HAS_ZARR
return find_tree(*impl_,true,key);
#else
(void)key;
fail("/","storage support is disabled");
#endif
}
TreeView BackedTreeData::var_tree(std::string_view key) const { if(!is_open()) fail("/","store is closed");
#if ANNDATAPP_HAS_HDF5 || ANNDATAPP_HAS_ZARR
return find_tree(*impl_,false,key);
#else
(void)key;
fail("/","storage support is disabled");
#endif
}
std::vector<std::string> BackedTreeData::obs_tree_names() const { if(!is_open()) fail("/","store is closed");
#if ANNDATAPP_HAS_HDF5 || ANNDATAPP_HAS_ZARR
return tree_names(*impl_,true);
#else
fail("/","storage support is disabled");
#endif
}
std::vector<std::string> BackedTreeData::var_tree_names() const { if(!is_open()) fail("/","store is closed");
#if ANNDATAPP_HAS_HDF5 || ANNDATAPP_HAS_ZARR
return tree_names(*impl_,false);
#else
fail("/","storage support is disabled");
#endif
}
TreeData BackedTreeData::read(Selection obs,Selection var) const {
    auto d=data().slice(obs,var).read();
#if ANNDATAPP_HAS_HDF5 || ANNDATAPP_HAS_ZARR
    auto a=impl_->alignment;
    const bool obs_scalar=scalar_trees(impl_->store,"/obst"), var_scalar=scalar_trees(impl_->store,"/vart");
    // HDF5 and legacy Zarr collections are indivisible JSON scalars.
    auto ot=read_trees(impl_->store,"/obst",{},!obs_scalar), vt=read_trees(impl_->store,"/vart",{},!var_scalar);
    if (a==TreeAlignment::nodes) {
        for (auto& [k,t]:ot) if (!detail::tree_subset_connected(t.view(),d.obs())) a=TreeAlignment::subset;
        for (auto& [k,t]:vt) if (!detail::tree_subset_connected(t.view(),d.var())) a=TreeAlignment::subset;
    }
    TreeData result(std::move(d),a,impl_->overlap,impl_->label);
    for(auto& [k,t]:ot) {
        if(obs_scalar) result.set_obs_tree(k,a==TreeAlignment::subset?t.copy():detail::tree_select(t.view(),result.data().obs(),a));
        else result.set_obs_tree(k,read_projected_tree(impl_->store,"/obst/"+k,t.view(),result.data().obs(),a));
    }
    for(auto& [k,t]:vt) {
        if(var_scalar) result.set_var_tree(k,a==TreeAlignment::subset?t.copy():detail::tree_select(t.view(),result.data().var(),a));
        else result.set_var_tree(k,read_projected_tree(impl_->store,"/vart/"+k,t.view(),result.data().var(),a));
    }
    return result;
#else
    fail("/","storage support is disabled");
#endif
}
void BackedTreeData::close() { if(impl_) impl_->store.close(); }
BackedTreeData open_h5td(std::string_view p,BackedOptions o) {
#if ANNDATAPP_HAS_HDF5
return BackedTreeData(load(detail::open_hdf5_store_impl(p,o)));
#else
(void)o;
fail(p,"HDF5 support is disabled");
#endif
}
TreeData read_h5td(std::string_view p,BackedOptions o) { return open_h5td(p,o).read(); }
void write_h5td(std::string_view p,const TreeDataView& d,TreeWriteOptions options) {
#if ANNDATAPP_HAS_HDF5
if (options.scratch_bytes<1024) throw Error(ErrorCode::invalid_argument,std::string(p),"scratch budget is too small");
if (d.has_label() && d.label().size()>options.scratch_bytes/4)
    throw Error(ErrorCode::invalid_argument,"label","label exceeds scratch budget");
preflight_hdf5_trees(d,true,options.scratch_bytes);
preflight_hdf5_trees(d,false,options.scratch_bytes);
write(detail::create_hdf5_store_impl(p,false,false,4,std::nullopt,Validation::full,0,options.scratch_bytes/2),d,false,options.scratch_bytes);
#else
(void)d; (void)options;
fail(p,"HDF5 support is disabled");
#endif
}
void write_h5td(std::string_view p,const TreeData& d,TreeWriteOptions options) { write_h5td(p,d.view(),options); }
BackedTreeData open_treedata_zarr(std::string_view p,BackedOptions o) {
#if ANNDATAPP_HAS_ZARR
return BackedTreeData(load(detail::open_zarr_store_impl(p,o)));
#else
(void)o;
fail(p,"Zarr support is disabled");
#endif
}
TreeData read_treedata_zarr(std::string_view p,BackedOptions o) { return open_treedata_zarr(p,o).read(); }
void write_treedata_zarr(std::string_view p,const TreeDataView& d,TreeWriteOptions options) {
#if ANNDATAPP_HAS_ZARR
if (options.scratch_bytes<2048) throw Error(ErrorCode::invalid_argument,std::string(p),"scratch budget is too small");
if (d.has_label() && d.label().size()>options.scratch_bytes/4)
    throw Error(ErrorCode::invalid_argument,"label","label exceeds scratch budget");
preflight_zarr(d,options.scratch_bytes);
write(detail::create_zarr_store_impl(p,3,false,options.scratch_bytes/2,"zstd",3,Validation::full),d,true,options.scratch_bytes);
#else
(void)d; (void)options;
fail(p,"Zarr support is disabled");
#endif
}
void write_treedata_zarr(std::string_view p,const TreeData& d,TreeWriteOptions options) { write_treedata_zarr(p,d.view(),options); }
}
