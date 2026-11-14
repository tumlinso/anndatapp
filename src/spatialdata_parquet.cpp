module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#ifndef ANNDATAPP_HAS_PARQUET
#define ANNDATAPP_HAS_PARQUET 0
#endif
#if ANNDATAPP_HAS_PARQUET
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/array/array_dict.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/metadata.h>
#include <nlohmann/json.hpp>
#include "spatialdata_wkb.hpp"
#endif

module anndatapp.spatialdata;
#include "spatialdata_parquet.hpp"

namespace anndatapp::spatial_detail {

#if !ANNDATAPP_HAS_PARQUET
namespace {
[[noreturn]] void unavailable(std::string_view path) {
    throw Error(ErrorCode::unsupported_encoding, std::string(path), "SpatialData Parquet support is disabled");
}
}
SpatialPoints read_parquet_points(std::string_view p, std::size_t, std::size_t, std::vector<SpatialAxis>, std::size_t) { unavailable(p); }
SpatialShapes read_parquet_shapes(std::string_view p, std::size_t) { unavailable(p); }
SpatialShapes read_parquet_shapes(std::string_view p, std::size_t, std::size_t, std::size_t) { unavailable(p); }
void write_parquet_points(std::string_view p, const SpatialPointsView&, std::size_t, std::size_t) { unavailable(p); }
void write_parquet_shapes(std::string_view p, const SpatialShapesView&, std::size_t, std::size_t) { unavailable(p); }
#else
namespace {
namespace ar = ::arrow;
namespace pa = ::parquet::arrow;
namespace fs = ::std::filesystem;
using Json = nlohmann::json;
using Geometries = ::anndatapp_spatial_wkb::Geometry;

[[noreturn]] void fail(std::string_view path, std::string message,
                       ErrorCode code = ErrorCode::unsupported_encoding) {
    throw Error(code, std::string(path), std::move(message));
}
void check(const ar::Status& status, std::string_view path) {
    if (!status.ok()) fail(path, status.ToString(), ErrorCode::io_error);
}
template<class T> T get(ar::Result<T> result, std::string_view path) {
    if (!result.ok()) fail(path, result.status().ToString(), ErrorCode::io_error);
    return std::move(result).ValueOrDie();
}
std::string attr(const std::shared_ptr<const ar::KeyValueMetadata>& metadata,
                 std::string_view key, std::string_view path, bool required = true) {
    if (!metadata) {
        if (required) fail(path, "missing Parquet schema metadata");
        return {};
    }
    auto i = metadata->FindKey(std::string(key));
    if (i < 0) {
        if (required) fail(path, "missing Parquet metadata key '" + std::string(key) + "'");
        return {};
    }
    return metadata->value(i);
}
Json json(const std::string& value, std::string_view path) {
    try { return Json::parse(value); }
    catch (const Json::exception& e) { fail(path, std::string("invalid Parquet JSON metadata: ") + e.what()); }
}

struct IndexSpec {
    std::string field;
    std::int64_t start = 0, step = 1;
};
IndexSpec index_spec(const std::shared_ptr<ar::Schema>& schema, std::string_view path) {
    IndexSpec spec;
    auto data = attr(schema->metadata(), "pandas", path, false);
    if (data.empty()) return spec;
    auto meta = json(data, path);
    if (!meta.is_object() || !meta.contains("index_columns") || !meta["index_columns"].is_array())
        fail(path, "invalid pandas index_columns metadata");
    const auto& index = meta["index_columns"];
    if (index.size() > 1) fail(path, "multi-level row identifiers are unsupported");
    if (index.empty()) return spec;
    if (index[0].is_string()) {
        spec.field = index[0].get<std::string>();
        if (schema->GetFieldIndex(spec.field) < 0) fail(path, "pandas index column is missing");
    } else if (index[0].is_object() && index[0].value("kind", "") == "range") {
        if (!index[0].contains("start") || !index[0]["start"].is_number_integer() ||
            !index[0].contains("step") || !index[0]["step"].is_number_integer())
            fail(path, "invalid pandas range index");
        spec.start = index[0]["start"].get<std::int64_t>();
        spec.step = index[0]["step"].get<std::int64_t>();
        if (!spec.step) fail(path, "pandas range index has zero step");
    } else fail(path, "unsupported pandas index metadata");
    return spec;
}

struct Part {
    std::string path;
    std::unique_ptr<pa::FileReader> reader;
    std::shared_ptr<ar::Schema> schema;
    IndexSpec index;
    std::size_t rows = 0;
};
std::vector<fs::path> parts(std::string_view path) {
    std::vector<std::pair<std::uint64_t, fs::path>> ordered;
    fs::path p(path);
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) return {p};
    if (!fs::is_directory(p, ec)) fail(path, "Parquet payload does not exist", ErrorCode::io_error);
    for (const auto& entry : fs::directory_iterator(p, ec)) {
        if (ec) fail(path, ec.message(), ErrorCode::io_error);
        auto name = entry.path().filename().string();
        if (!name.starts_with("part.") || !name.ends_with(".parquet")) continue;
        auto digits = std::string_view(name).substr(5, name.size() - 13);
        if (digits.empty()) continue;
        std::uint64_t n = 0;
        for (char c : digits) {
            if (c < '0' || c > '9' || n > (std::numeric_limits<std::uint64_t>::max() - (c-'0')) / 10)
                fail(path, "invalid Parquet part number");
            n = n * 10 + (c-'0');
        }
        ordered.emplace_back(n, entry.path());
    }
    if (ec) fail(path, ec.message(), ErrorCode::io_error);
    if (ordered.empty()) fail(path, "Parquet dataset contains no part files");
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<fs::path> result;
    for (const auto& [n, file] : ordered) {
        if (!result.empty() && n == ordered[result.size()-1].first) fail(path, "duplicate Parquet part number");
        result.push_back(file);
    }
    return result;
}

std::vector<Part> open_parts(std::string_view path) {
    std::vector<Part> result;
    for (const auto& p : parts(path)) {
        Part part;
        part.path = p.string();
        auto input = get(ar::io::ReadableFile::Open(part.path), part.path);
        part.reader = get(pa::OpenFile(input, ar::default_memory_pool()), part.path);
        part.reader->set_use_threads(false);
        part.reader->set_batch_size(65536);
        check(part.reader->GetSchema(&part.schema), part.path);
        auto nr = part.reader->parquet_reader()->metadata()->num_rows();
        if (nr < 0 || std::uint64_t(nr) > std::numeric_limits<std::size_t>::max())
            fail(part.path, "Parquet row count is too large");
        part.rows = static_cast<std::size_t>(nr);
        part.index = index_spec(part.schema, part.path);
        if (!result.empty() && !result.front().schema->Equals(*part.schema, false))
            fail(part.path, "Parquet dataset part schemas differ");
        result.push_back(std::move(part));
    }
    return result;
}

template<class Initialize, class Append>
void scan_batches(std::string_view path, std::size_t first, std::size_t count, Initialize initialize, Append append) {
    auto files = open_parts(path);

    std::size_t total = 0;
    for (const auto& f : files) {
        if (f.rows > std::numeric_limits<std::size_t>::max() - total) fail(path, "Parquet dataset row count overflow");
        total += f.rows;
    }
    if (first > total) fail(path, "Parquet selection starts past the dataset", ErrorCode::out_of_bounds);
    if (count == std::numeric_limits<std::size_t>::max()) count = total - first;
    if (count > total - first) fail(path, "Parquet selection exceeds the dataset", ErrorCode::out_of_bounds);
    initialize(files.front().schema, count);
    std::size_t base = 0;
    for (auto& f : files) {
        auto start = first > base ? std::min(first - base, f.rows) : 0;
        auto finish = first + count > base ? std::min(first + count - base, f.rows) : 0;
        base += f.rows;
        if (start >= finish) continue;
        std::size_t group_start = 0;
        auto metadata = f.reader->parquet_reader()->metadata();
        for (int g = 0; g < f.reader->num_row_groups(); ++g) {
            auto n = metadata->RowGroup(g)->num_rows();
            if (n < 0 || std::uint64_t(n) > std::numeric_limits<std::size_t>::max() - group_start)
                fail(f.path, "invalid Parquet row-group size");
            auto next = group_start + static_cast<std::size_t>(n);
            if (next <= start || group_start >= finish) { group_start = next; continue; }
        auto reader = get(f.reader->GetRecordBatchReader(std::vector<int>{g}), f.path);
        std::size_t cursor = group_start;
        for (;;) {
            std::shared_ptr<ar::RecordBatch> batch;
            check(reader->ReadNext(&batch), f.path);
            if (!batch) break;
            auto n = static_cast<std::size_t>(batch->num_rows());
            auto lo = start > cursor ? std::min(start-cursor, n) : 0;
            auto hi = finish > cursor ? std::min(finish-cursor, n) : 0;
            if (lo < hi) {
                auto sliced = batch->Slice(static_cast<std::int64_t>(lo), static_cast<std::int64_t>(hi-lo));
                std::shared_ptr<ar::Array> ids;
                if (!f.index.field.empty()) {
                    ids = sliced->GetColumnByName(f.index.field);
                    if (!ids) fail(f.path, "pandas index column is missing");
                    if (ids->null_count()) fail(f.path, "row identifiers cannot be null");

                } else {
                    ar::Int64Builder builder;
                    check(builder.Reserve(static_cast<std::int64_t>(hi-lo)), f.path);
                    for (std::size_t i = lo; i < hi; ++i) {
                        auto row = cursor + i;
                        if (row > std::uint64_t(std::numeric_limits<std::int64_t>::max())) fail(f.path, "range index overflow");
                        std::int64_t product, id;
                        if (__builtin_mul_overflow(f.index.step, std::int64_t(row), &product) ||
                            __builtin_add_overflow(f.index.start, product, &id)) fail(f.path, "range index overflow");
                        builder.UnsafeAppend(id);
                    }
                    check(builder.Finish(&ids), f.path);

                }
                append(sliced, ids);
            }
            cursor += n;
            if (cursor >= finish) break;
        }
        group_start = next;
        }
    }
}


std::shared_ptr<ar::Array> decode_index(std::shared_ptr<ar::Array> ids, std::string_view path) {
    auto type = ids->type();
    if (type->id() != ar::Type::DICTIONARY) return ids;
    auto dt = std::static_pointer_cast<ar::DictionaryType>(type);
    auto builder = get(ar::MakeBuilder(dt->value_type()), path);
    check(builder->Reserve(ids->length()), path);
    for (std::int64_t i = 0; i < ids->length(); ++i) {
        if (ids->IsNull(i)) fail(path, "row identifiers cannot be null");
        auto scalar = std::static_pointer_cast<ar::DictionaryScalar>(get(ids->GetScalar(i), path));
        auto value = get(scalar->GetEncodedValue(), path);
        if (!value->is_valid) fail(path, "row identifiers cannot be null");
        check(builder->AppendScalar(*value), path);
    }
    std::shared_ptr<ar::Array> decoded;
    check(builder->Finish(&decoded), path);
    return decoded;
}

void append_names(std::vector<std::string>& out, const std::shared_ptr<ar::Array>& ids, std::string_view path) {
    if (ids->null_count()) fail(path, "row identifiers cannot be null");
    switch (ids->type_id()) {
    case ar::Type::STRING: {
        auto src = std::static_pointer_cast<ar::StringArray>(ids);
        for (std::int64_t i = 0; i < ids->length(); ++i) { auto s = src->GetView(i); out.emplace_back(s.data(),s.size()); }
        break;
    }
    case ar::Type::LARGE_STRING: {
        auto src = std::static_pointer_cast<ar::LargeStringArray>(ids);
        for (std::int64_t i = 0; i < ids->length(); ++i) { auto s = src->GetView(i); out.emplace_back(s.data(),s.size()); }
        break;
    }
    case ar::Type::INT8: case ar::Type::INT16: case ar::Type::INT32: case ar::Type::INT64:
    case ar::Type::UINT8: case ar::Type::UINT16: case ar::Type::UINT32: case ar::Type::UINT64:
        for (std::int64_t i = 0; i < ids->length(); ++i) out.push_back(get(ids->GetScalar(i), path)->ToString());
        break;
    default: fail(path, "row identifiers must be integer or UTF-8 strings", ErrorCode::unsupported_dtype);
    }
}
std::string index_name(const std::shared_ptr<ar::Schema>& schema, std::string_view path) {
    auto data = attr(schema->metadata(), "pandas", path, false);
    if (data.empty()) return {};
    auto meta = json(data, path);
    const auto& indices = meta["index_columns"];
    if (indices.empty()) return {};
    if (indices[0].is_object()) {
        auto name = indices[0].value("name", Json());
        return name.is_string() ? name.get<std::string>() : std::string{};
    }
    auto field = indices[0].get<std::string>();
    if (meta.contains("columns") && meta["columns"].is_array())
        for (const auto& c : meta["columns"])
            if (c.is_object() && c.value("field_name", "") == field && c.contains("name") && c["name"].is_string())
                return c["name"].get<std::string>();
    return {};
}
double number(const std::shared_ptr<ar::Array>& a, std::int64_t i, std::string_view path) {
    if (a->IsNull(i)) fail(path, "null geometry coordinate or radius");
    switch (a->type_id()) {
    case ar::Type::FLOAT: return std::static_pointer_cast<ar::FloatArray>(a)->Value(i);
    case ar::Type::DOUBLE: return std::static_pointer_cast<ar::DoubleArray>(a)->Value(i);
    case ar::Type::INT8: return std::static_pointer_cast<ar::Int8Array>(a)->Value(i);
    case ar::Type::INT16: return std::static_pointer_cast<ar::Int16Array>(a)->Value(i);
    case ar::Type::INT32: return std::static_pointer_cast<ar::Int32Array>(a)->Value(i);
    case ar::Type::INT64: return static_cast<double>(std::static_pointer_cast<ar::Int64Array>(a)->Value(i));
    case ar::Type::UINT8: return std::static_pointer_cast<ar::UInt8Array>(a)->Value(i);
    case ar::Type::UINT16: return std::static_pointer_cast<ar::UInt16Array>(a)->Value(i);
    case ar::Type::UINT32: return std::static_pointer_cast<ar::UInt32Array>(a)->Value(i);
    case ar::Type::UINT64: return static_cast<double>(std::static_pointer_cast<ar::UInt64Array>(a)->Value(i));
    default: fail(path, "geometry coordinate is not numeric", ErrorCode::unsupported_dtype);
    }
}
struct ListRange { std::shared_ptr<ar::Array> values; std::int64_t first, count; };
ListRange list_range(const std::shared_ptr<ar::Array>& a, std::int64_t i, std::string_view path) {
    if (a->IsNull(i)) fail(path, "null GeoArrow geometry list");
    if (a->type_id() == ar::Type::LIST) {
        auto list = std::static_pointer_cast<ar::ListArray>(a);
        return {list->values(), list->value_offset(i), list->value_length(i)};
    }
    if (a->type_id() == ar::Type::LARGE_LIST) {
        auto list = std::static_pointer_cast<ar::LargeListArray>(a);
        return {list->values(), list->value_offset(i), list->value_length(i)};
    }
    if (a->type_id() == ar::Type::FIXED_SIZE_LIST) {
        auto list = std::static_pointer_cast<ar::FixedSizeListArray>(a);
        return {list->values(), list->value_offset(i), list->value_length()};
    }
    fail(path, "expected a GeoArrow list", ErrorCode::unsupported_dtype);
}
void native_point(Geometries& out, const std::shared_ptr<ar::Array>& a,
                  std::int64_t i, std::string_view path) {
    if (a->IsNull(i)) fail(path, "null GeoArrow point");
    if (a->type_id() == ar::Type::STRUCT) {
        auto p = std::static_pointer_cast<ar::StructArray>(a);
        auto t = std::static_pointer_cast<ar::StructType>(a->type());
        auto x = t->GetFieldIndex("x"), y = t->GetFieldIndex("y");
        if (x < 0 || y < 0 || t->num_fields() != 2) fail(path, "GeoArrow point must have x and y fields");
        out.xy.push_back(number(p->field(x), i, path));
        out.xy.push_back(number(p->field(y), i, path));
    } else if (a->type_id() == ar::Type::FIXED_SIZE_LIST) {
        auto p = list_range(a, i, path);
        if (p.count != 2) fail(path, "GeoArrow point must be two-dimensional");
        out.xy.push_back(number(p.values, p.first, path));
        out.xy.push_back(number(p.values, p.first+1, path));
    } else fail(path, "unsupported GeoArrow point representation", ErrorCode::unsupported_dtype);
}
void native_polygon(Geometries& out, const std::shared_ptr<ar::Array>& a,
                    std::int64_t i, std::string_view path) {
    auto rings = list_range(a, i, path);
    for (std::int64_t r = 0; r < rings.count; ++r) {
        auto points = list_range(rings.values, rings.first+r, path);
        for (std::int64_t p = 0; p < points.count; ++p)
            native_point(out, points.values, points.first+p, path);
        out.rings.push_back(out.xy.size()/2);
    }
    out.polygons.push_back(out.rings.size()-1);
}
void append_geometry(Geometries& out, const std::shared_ptr<ar::Array>& array, std::string_view encoding,
                    std::string_view path) {
    for (std::int64_t i = 0; i < array->length(); ++i) {
        if (array->IsNull(i)) fail(path, "null shape geometry");
        if (encoding == "WKB") {
            try {
                if (array->type_id() == ar::Type::BINARY) {
                    auto s = std::static_pointer_cast<ar::BinaryArray>(array)->GetView(i);
                    ::anndatapp_spatial_wkb::append(out, s.data(), s.size());
                } else if (array->type_id() == ar::Type::LARGE_BINARY) {
                    auto s = std::static_pointer_cast<ar::LargeBinaryArray>(array)->GetView(i);
                    ::anndatapp_spatial_wkb::append(out, s.data(), s.size());
                } else fail(path, "WKB geometry must be binary", ErrorCode::unsupported_dtype);
            } catch (const std::runtime_error& e) { fail(path, e.what()); }
        } else if (encoding == "point") {
            native_point(out, array, i, path);
            out.types.push_back(1);
        } else if (encoding == "polygon") {
            native_polygon(out, array, i, path);
            out.features.push_back(out.polygons.size()-1);
            out.types.push_back(3);
        } else if (encoding == "multipolygon") {
            auto p = list_range(array, i, path);
            for (std::int64_t j = 0; j < p.count; ++j) native_polygon(out, p.values, p.first+j, path);
            out.features.push_back(out.polygons.size()-1);
            out.types.push_back(6);
        } else fail(path, "unsupported GeoParquet geometry encoding '" + std::string(encoding) + "'");
    }
}
template<class T> Array owned_array(std::vector<T> data, std::vector<std::size_t> shape) {
    auto owner = std::make_shared<std::vector<T>>(std::move(data));
    std::vector<std::ptrdiff_t> strides(shape.size());
    std::ptrdiff_t stride = 1;
    for (std::size_t i = shape.size(); i-- > 0;) {
        strides[i] = stride;
        if (shape[i] > std::size_t(std::numeric_limits<std::ptrdiff_t>::max()) ||
            (shape[i] && stride > std::numeric_limits<std::ptrdiff_t>::max()/std::ptrdiff_t(shape[i])))
            throw Error(ErrorCode::invalid_argument, "parquet/geometry", "geometry shape is too large");
        stride *= std::ptrdiff_t(shape[i]);
    }
    return Array::share_dense(owner->data(), std::move(shape), std::move(strides), owner);
}
std::string geometry_encoding(const std::shared_ptr<ar::Schema>& schema, std::string_view path) {
    auto meta = json(attr(schema->metadata(), "geo", path), path);
    try {
        auto version = meta.at("version").get<std::string>();
        if (version != "1.0.0" && version != "1.1.0") fail(path, "unsupported GeoParquet version '" + version + "'", ErrorCode::unsupported_version);
        if (meta.at("primary_column").get<std::string>() != "geometry") fail(path, "GeoParquet primary geometry column must be 'geometry'");
        return meta.at("columns").at("geometry").at("encoding").get<std::string>();
    } catch (const Json::exception& e) { fail(path, std::string("invalid GeoParquet metadata: ") + e.what()); }
}

template<class T> struct NumericSource {
    Array owned;
    DenseView<const T> view;
    NumericSource(const ArrayView& source, std::size_t n, std::size_t budget, std::string_view path) {
        try { view = source.dense<T>(); }
        catch (const Error& e) {
            if (e.code() != ErrorCode::non_contiguous) throw;
            if (n > budget / sizeof(T)) fail(path, "scratch budget is too small for a gathered column", ErrorCode::invalid_argument);
            owned = source.copy();
            view = owned.view().dense<T>();
        }
    }
};
template<class T, class Builder> std::shared_ptr<ar::Array> numeric_array(const ColumnView& col,
    std::string_view path, std::size_t budget) {
    NumericSource<T> values(col.values(), col.size(), budget, path);
    Builder builder;
    check(builder.Reserve(static_cast<std::int64_t>(col.size())), path);
    for (std::size_t i = 0; i < col.size(); ++i) {
        if (!col.valid(i)) check(builder.AppendNull(), path);
        else check(builder.Append(values.view.data()[static_cast<std::ptrdiff_t>(i)*values.view.stride(0)]), path);
    }
    std::shared_ptr<ar::Array> out;
    check(builder.Finish(&out), path);
    return out;
}
std::shared_ptr<ar::Array> numeric_array(const ColumnView& col, std::string_view path, std::size_t budget) {
    switch (col.values().dtype()) {
    case DType::boolean: return numeric_array<bool, ar::BooleanBuilder>(col, path, budget);
    case DType::int8: return numeric_array<std::int8_t, ar::Int8Builder>(col, path, budget);
    case DType::uint8: return numeric_array<std::uint8_t, ar::UInt8Builder>(col, path, budget);
    case DType::int16: return numeric_array<std::int16_t, ar::Int16Builder>(col, path, budget);
    case DType::uint16: return numeric_array<std::uint16_t, ar::UInt16Builder>(col, path, budget);
    case DType::int32: return numeric_array<std::int32_t, ar::Int32Builder>(col, path, budget);
    case DType::uint32: return numeric_array<std::uint32_t, ar::UInt32Builder>(col, path, budget);
    case DType::int64: return numeric_array<std::int64_t, ar::Int64Builder>(col, path, budget);
    case DType::uint64: return numeric_array<std::uint64_t, ar::UInt64Builder>(col, path, budget);
    case DType::float32: return numeric_array<float, ar::FloatBuilder>(col, path, budget);
    case DType::float64: return numeric_array<double, ar::DoubleBuilder>(col, path, budget);
    }
    fail(path, "unsupported native numerical type", ErrorCode::unsupported_dtype);
}
template<class T, class Builder> std::shared_ptr<ar::Array> categorical_indices(
    const ColumnView& col, const ArrayView& code_view, std::string_view path, std::size_t budget) {
    NumericSource<T> codes(code_view, col.size(), budget, path);
    Builder builder;
    check(builder.Reserve(static_cast<std::int64_t>(col.size())), path);
    for (std::size_t i = 0; i < col.size(); ++i) {
        if (!col.valid(i)) check(builder.AppendNull(), path);
        else check(builder.Append(codes.view.data()[static_cast<std::ptrdiff_t>(i)*codes.view.stride(0)]), path);
    }
    std::shared_ptr<ar::Array> out;
    check(builder.Finish(&out), path);
    return out;
}
std::shared_ptr<ar::Array> arrow_array(const ColumnView& col, std::string_view path, std::size_t budget) {
    if (col.kind() == ColumnKind::numeric) return numeric_array(col, path, budget);
    if (col.kind() == ColumnKind::string) {
        ar::LargeStringBuilder builder;
        check(builder.Reserve(static_cast<std::int64_t>(col.size())), path);
        for (std::size_t i = 0; i < col.size(); ++i) {
            if (!col.valid(i)) check(builder.AppendNull(), path);
            else { auto s = col.string(i); check(builder.Append(s.data(), static_cast<std::int64_t>(s.size())), path); }
        }
        std::shared_ptr<ar::Array> out;
        check(builder.Finish(&out), path);
        return out;
    }
    if (col.kind() == ColumnKind::categorical) {
        if (col.size() > budget / 8) fail(path, "scratch budget is too small for categorical codes", ErrorCode::invalid_argument);
        auto codes = col.codes();
        std::shared_ptr<ar::Array> indices;
        switch (codes.dtype()) {
        case DType::int8: indices = categorical_indices<std::int8_t, ar::Int8Builder>(col, codes, path, budget); break;
        case DType::int16: indices = categorical_indices<std::int16_t, ar::Int16Builder>(col, codes, path, budget); break;
        case DType::int32: indices = categorical_indices<std::int32_t, ar::Int32Builder>(col, codes, path, budget); break;
        case DType::int64: indices = categorical_indices<std::int64_t, ar::Int64Builder>(col, codes, path, budget); break;
        default: fail(path, "categorical codes require signed integer storage", ErrorCode::unsupported_dtype);
        }
        auto dictionary = arrow_array(col.categories(), path, budget);
        auto type = ar::dictionary(indices->type(), dictionary->type(), col.ordered());
        return get(ar::DictionaryArray::FromArrays(type, indices, dictionary), path);
    }
    fail(path, "unsupported native column kind", ErrorCode::unsupported_dtype);
}
Json pandas_column(const std::string& name, const std::shared_ptr<ar::Array>& array) {
    std::string pandas_type, numpy_type;
    Json info = nullptr;
    switch (array->type_id()) {
    case ar::Type::BOOL: pandas_type = numpy_type = "bool"; break;
    case ar::Type::INT8: pandas_type = numpy_type = "int8"; break;
    case ar::Type::INT16: pandas_type = numpy_type = "int16"; break;
    case ar::Type::INT32: pandas_type = numpy_type = "int32"; break;
    case ar::Type::INT64: pandas_type = numpy_type = "int64"; break;
    case ar::Type::UINT8: pandas_type = numpy_type = "uint8"; break;
    case ar::Type::UINT16: pandas_type = numpy_type = "uint16"; break;
    case ar::Type::UINT32: pandas_type = numpy_type = "uint32"; break;
    case ar::Type::UINT64: pandas_type = numpy_type = "uint64"; break;
    case ar::Type::FLOAT: pandas_type = numpy_type = "float32"; break;
    case ar::Type::DOUBLE: pandas_type = numpy_type = "float64"; break;
    case ar::Type::STRING: case ar::Type::LARGE_STRING: pandas_type = "unicode"; numpy_type = "object"; break;
    case ar::Type::BINARY: case ar::Type::LARGE_BINARY: pandas_type = "bytes"; numpy_type = "object"; break;
    case ar::Type::DICTIONARY: {
        auto dict = std::static_pointer_cast<ar::DictionaryArray>(array);
        auto type = std::static_pointer_cast<ar::DictionaryType>(array->type());
        pandas_type = "categorical";
        numpy_type = dict->indices()->type()->ToString();
        info = {{"num_categories", dict->dictionary()->length()}, {"ordered", type->ordered()}};
        break;
    }
    default: pandas_type = numpy_type = "object"; break;
    }
    return {{"name", name}, {"field_name", name}, {"pandas_type", pandas_type},
            {"numpy_type", numpy_type}, {"metadata", info}};
}
struct WriteBatch {
    std::vector<std::string> names;
    std::vector<std::shared_ptr<ar::Array>> arrays;
};
std::string index_field(const TableView& table) {
    std::string field = "__index_level_0__";
    while (table.contains(field)) field.push_back('_');
    return field;
}
WriteBatch points_batch(const SpatialPointsView& points, const std::string& index,
                        std::size_t first, std::size_t count, std::string_view path, std::size_t budget) {
    WriteBatch batch;
    auto values = points.values();
    auto rows = Axis::stride(first, count);
    for (const auto& name : values.column_names()) {
        batch.names.push_back(name);
        batch.arrays.push_back(arrow_array(values.column(name).select(rows, false), path, budget));
    }
    auto identifiers = points.identifiers().select(rows, false);
    if (identifiers.nullable()) for (std::size_t i = 0; i < identifiers.size(); ++i)
        if (!identifiers.valid(i)) fail(path, "row identifiers cannot be null");
    batch.names.push_back(index);
    batch.arrays.push_back(arrow_array(identifiers, path, budget));
    return batch;
}
std::shared_ptr<ar::Schema> schema(const WriteBatch& batch, const std::string& index,
                                  std::string_view index_name, bool geo,
                                  const std::vector<std::string>& geometry_types = {}) {
    std::vector<std::shared_ptr<ar::Field>> fields;
    Json pandas_columns = Json::array();
    for (std::size_t i = 0; i < batch.names.size(); ++i) {
        fields.push_back(ar::field(batch.names[i], batch.arrays[i]->type()));
        auto item = pandas_column(batch.names[i], batch.arrays[i]);
        if (batch.names[i] == index) item["name"] = index_name.empty() ? Json(nullptr) : Json(std::string(index_name));
        pandas_columns.push_back(std::move(item));
    }
    Json pandas = {{"index_columns", Json::array({index})}, {"column_indexes", Json::array()},
                   {"columns", std::move(pandas_columns)},
                   {"creator", {{"library", "anndatapp"}, {"version", "1.0"}}},
                   {"pandas_version", "2.3.3"}};
    std::vector<std::string> keys{"pandas"}, values{pandas.dump()};
    if (geo) {
        Json types = geometry_types;
        Json metadata = {{"version", "1.0.0"}, {"primary_column", "geometry"},
            {"columns", {{"geometry", {{"encoding", "WKB"}, {"geometry_types", types}, {"crs", nullptr}}}}}};
        keys.push_back("geo"); values.push_back(metadata.dump());
    }
    return ar::schema(std::move(fields), ar::key_value_metadata(std::move(keys), std::move(values)));
}
std::shared_ptr<ar::Table> arrow_table(const std::shared_ptr<ar::Schema>& schema,
                                       const WriteBatch& batch) {
    std::vector<std::shared_ptr<ar::ChunkedArray>> columns;
    for (const auto& a : batch.arrays) columns.push_back(std::make_shared<ar::ChunkedArray>(a));
    return ar::Table::Make(schema, std::move(columns));
}
std::unique_ptr<pa::FileWriter> open_writer(const std::string& path,
                                           const std::shared_ptr<ar::Schema>& schema) {
    auto output = get(ar::io::FileOutputStream::Open(path), path);
    auto props = ::parquet::ArrowWriterProperties::Builder().store_schema()->build();
    auto writer = get(pa::FileWriter::Open(*schema, ar::default_memory_pool(), output,
        ::parquet::default_writer_properties(), props), path);
    if (schema->metadata()) check(writer->AddKeyValueMetadata(schema->metadata()), path);
    return writer;
}

template<class T> T at1(const DenseView<const T>& a, std::size_t i, std::string_view path) {
    if (a.rank() != 1 || i >= a.extent(0)) fail(path, "invalid geometry offset index");
    return a.data()[static_cast<std::ptrdiff_t>(i)*a.stride(0)];
}
double xy_at(const DenseView<const double>& xy, std::size_t i, std::size_t axis, std::string_view path) {
    if (xy.rank() != 2 || axis >= xy.extent(1) || i >= xy.extent(0)) fail(path, "invalid geometry coordinate index");
    return xy.data()[static_cast<std::ptrdiff_t>(i)*xy.stride(0) + static_cast<std::ptrdiff_t>(axis)*xy.stride(1)];
}
void wkb_integer(ar::LargeBinaryBuilder& builder, std::uint64_t value, std::string_view path) {
    if (value > std::numeric_limits<std::uint32_t>::max()) fail(path, "WKB count exceeds uint32");
    std::uint8_t bytes[4];
    for (int i = 0; i < 4; ++i) bytes[i] = std::uint8_t(value >> (8*i));
    check(builder.ExtendCurrent(bytes, 4), path);
}
void wkb_double(ar::LargeBinaryBuilder& builder, double value, std::string_view path) {
    std::uint64_t bits; std::memcpy(&bits, &value, sizeof(bits));
    std::uint8_t bytes[8];
    for (int i = 0; i < 8; ++i) bytes[i] = std::uint8_t(bits >> (8*i));
    check(builder.ExtendCurrent(bytes, 8), path);
}
void wkb_header(ar::LargeBinaryBuilder& builder, std::uint8_t type, std::string_view path) {
    const std::uint8_t little = 1;
    check(builder.ExtendCurrent(&little, 1), path);
    wkb_integer(builder, type, path);
}
void geometry_wkb(ar::LargeBinaryBuilder& builder, const SpatialShapesView& shapes,
                  std::size_t i, std::string_view path) {
    auto source = shapes.source_row(i);
    auto xy = shapes.coordinates().dense<double>();
    check(builder.AppendEmptyValue(), path);
    if (shapes.kind() == SpatialGeometryKind::circles) {
        wkb_header(builder, 1, path);
        wkb_double(builder, xy_at(xy, source, 0, path), path);
        wkb_double(builder, xy_at(xy, source, 1, path), path);
        return;
    }
    auto rings = shapes.ring_offsets().dense<std::uint64_t>();
    auto polygons = shapes.polygon_offsets().dense<std::uint64_t>();
    auto features = shapes.geometry_offsets().dense<std::uint64_t>();
    auto types = shapes.geometry_types().dense<std::uint8_t>();
    auto first = at1(features, source, path), last = at1(features, source+1, path);
    if (first > last || last >= polygons.extent(0)) fail(path, "invalid shape polygon offsets");
    auto type = at1(types, source, path);
    if (type != 3 && type != 6) fail(path, "unsupported polygon geometry type");
    if (type == 3 && last-first != 1) fail(path, "Polygon must contain exactly one polygon");
    wkb_header(builder, type, path);
    if (type == 6) wkb_integer(builder, last-first, path);
    for (auto p = first; p < last; ++p) {
        if (type == 6) wkb_header(builder, 3, path);
        auto rf = at1(polygons, static_cast<std::size_t>(p), path);
        auto rl = at1(polygons, static_cast<std::size_t>(p+1), path);
        if (rf > rl || rl >= rings.extent(0)) fail(path, "invalid shape ring offsets");
        wkb_integer(builder, rl-rf, path);
        for (auto r = rf; r < rl; ++r) {
            auto vf = at1(rings, static_cast<std::size_t>(r), path);
            auto vl = at1(rings, static_cast<std::size_t>(r+1), path);
            if (vf > vl || vl > xy.extent(0)) fail(path, "invalid shape coordinate offsets");
            wkb_integer(builder, vl-vf, path);
            for (auto v = vf; v < vl; ++v) {
                wkb_double(builder, xy_at(xy, static_cast<std::size_t>(v), 0, path), path);
                wkb_double(builder, xy_at(xy, static_cast<std::size_t>(v), 1, path), path);
            }
        }
    }
}
WriteBatch shapes_batch(const SpatialShapesView& shapes, const std::string& index,
                        std::size_t first, std::size_t count, std::string_view path, std::size_t budget) {
    WriteBatch batch;
    auto attrs = shapes.attributes();
    auto rows = Axis::stride(first, count);
    for (const auto& name : attrs.column_names()) {
        if (name == "geometry" || (name == "radius" && shapes.kind() == SpatialGeometryKind::circles))
            fail(path, "shape attribute conflicts with required geometry column");
        batch.names.push_back(name);
        batch.arrays.push_back(arrow_array(attrs.column(name).select(rows, false), path, budget));
    }
    ar::LargeBinaryBuilder geometries;
    check(geometries.Reserve(static_cast<std::int64_t>(count)), path);
    for (std::size_t i = 0; i < count; ++i) geometry_wkb(geometries, shapes, first+i, path);
    std::shared_ptr<ar::Array> encoded;
    check(geometries.Finish(&encoded), path);
    batch.names.push_back("geometry"); batch.arrays.push_back(encoded);
    if (shapes.kind() == SpatialGeometryKind::circles) {
        auto radii = shapes.radii().dense<double>();
        ar::DoubleBuilder builder;
        check(builder.Reserve(static_cast<std::int64_t>(count)), path);
        for (std::size_t i = 0; i < count; ++i)
            check(builder.Append(at1(radii, shapes.source_row(first+i), path)), path);
        check(builder.Finish(&encoded), path);
        batch.names.push_back("radius"); batch.arrays.push_back(encoded);
    }
    auto identifiers = shapes.identifiers().select(rows, false);
    if (identifiers.nullable()) for (std::size_t i = 0; i < identifiers.size(); ++i)
        if (!identifiers.valid(i)) fail(path, "row identifiers cannot be null");
    batch.names.push_back(index); batch.arrays.push_back(arrow_array(identifiers, path, budget));
    return batch;
}

// StreamColumn owns the final native buffers and appends one Arrow batch at a time.
// Insert after owned_array definition. Requires <variant>. No input batches are retained.
class StreamColumn {
    std::shared_ptr<ar::DataType> type_;
    std::string path_;
    std::size_t total_, pos_ = 0;
    Array values_;
    std::vector<std::uint8_t> validity_;
    bool any_null_ = false;
    std::vector<char> bytes_;
    std::vector<std::size_t> offsets_{0};
    std::unique_ptr<StreamColumn> categories_;
    // Categories are owned by this output builder, and become the final owner.
    using CategoryData = std::variant<std::monostate,
        std::vector<std::uint8_t>,
        std::vector<std::int8_t>,
        std::vector<std::int16_t>,
        std::vector<std::uint16_t>,
        std::vector<std::int32_t>,
        std::vector<std::uint32_t>,
        std::vector<std::int64_t>,
        std::vector<std::uint64_t>,
        std::vector<float>,
        std::vector<double>>;
    CategoryData category_values_;
    std::size_t category_count_ = 0;
    std::int64_t* codes_ = nullptr;
    template<class T, class A> void copy_numeric(const std::shared_ptr<ar::Array>& a) {
        auto source = std::static_pointer_cast<A>(a);
        auto dest = values_.edit().template dense<T>().data();
        for (std::int64_t i = 0; i < a->length(); ++i, ++pos_) {
            bool valid = !a->IsNull(i);
            dest[pos_] = valid ? static_cast<T>(source->Value(i)) : T{};
            if (!valid && validity_.empty()) validity_.resize(total_, 1);
            if (!valid) validity_[pos_] = 0;
            any_null_ |= !valid;
        }
    }
    template<class A> void copy_strings(const std::shared_ptr<ar::Array>& a) {
        auto source = std::static_pointer_cast<A>(a);
        for (std::int64_t i = 0; i < a->length(); ++i, ++pos_) {
            bool valid = !a->IsNull(i);
            if (valid) {
                auto v = source->GetView(i);
                if (v.size() > std::numeric_limits<std::size_t>::max()-bytes_.size())
                    fail(path_, "packed string size overflow");
                bytes_.insert(bytes_.end(), v.begin(), v.end());
            }
            offsets_.push_back(bytes_.size());
            if (!valid && validity_.empty()) validity_.resize(total_, 1);
            if (!valid) validity_[pos_] = 0;
            any_null_ |= !valid;
        }
    }
    template<class T, class A, class V=T> std::size_t intern_numeric(const std::shared_ptr<ar::Array>& a, std::int64_t i) {
        auto v = static_cast<V>(std::static_pointer_cast<A>(a)->Value(i));
        auto& data = std::get<std::vector<V>>(category_values_);
        for (std::size_t j=0; j<data.size(); ++j) if (data[j] == v || (data[j] != data[j] && v != v)) return j;
        data.push_back(v); return category_count_++;
    }
    template<class A> std::size_t intern_string(const std::shared_ptr<ar::Array>& a, std::int64_t i) {
        auto v = std::static_pointer_cast<A>(a)->GetView(i);
        for (std::size_t j=0; j<category_count_; ++j) {
            auto n = offsets_[j+1]-offsets_[j];
            if (n == v.size() && (n == 0 || std::memcmp(bytes_.data()+offsets_[j], v.data(), n)==0)) return j;
        }
        bytes_.insert(bytes_.end(), v.begin(), v.end()); offsets_.push_back(bytes_.size());
        return category_count_++;
    }
    std::size_t intern(const std::shared_ptr<ar::Array>& a, std::int64_t i) {
        if (a->IsNull(i)) fail(path_, "null categorical dictionary entry");
        switch (a->type_id()) {
        case ar::Type::BOOL: return intern_numeric<bool, ar::BooleanArray, std::uint8_t>(a,i);
        case ar::Type::INT8: return intern_numeric<std::int8_t, ar::Int8Array, std::int8_t>(a,i);
        case ar::Type::UINT8: return intern_numeric<std::uint8_t, ar::UInt8Array, std::uint8_t>(a,i);
        case ar::Type::INT16: return intern_numeric<std::int16_t, ar::Int16Array, std::int16_t>(a,i);
        case ar::Type::UINT16: return intern_numeric<std::uint16_t, ar::UInt16Array, std::uint16_t>(a,i);
        case ar::Type::INT32: return intern_numeric<std::int32_t, ar::Int32Array, std::int32_t>(a,i);
        case ar::Type::UINT32: return intern_numeric<std::uint32_t, ar::UInt32Array, std::uint32_t>(a,i);
        case ar::Type::INT64: return intern_numeric<std::int64_t, ar::Int64Array, std::int64_t>(a,i);
        case ar::Type::UINT64: return intern_numeric<std::uint64_t, ar::UInt64Array, std::uint64_t>(a,i);
        case ar::Type::FLOAT: return intern_numeric<float, ar::FloatArray, float>(a,i);
        case ar::Type::DOUBLE: return intern_numeric<double, ar::DoubleArray, double>(a,i);
        case ar::Type::STRING: return intern_string<ar::StringArray>(a,i);
        case ar::Type::LARGE_STRING: return intern_string<ar::LargeStringArray>(a,i);
        default: fail(path_, "unsupported categorical value type " + a->type()->ToString(), ErrorCode::unsupported_dtype);
        }
    }
    template<class T> Column numeric_categories() {
        auto data = std::move(std::get<std::vector<T>>(category_values_));
        auto n = data.size(); return Column::numeric(owned_array<T>(std::move(data), {n}));
    }
    Column finish_categories() {
        switch(type_->id()) {
        case ar::Type::BOOL: {
            auto& data = std::get<std::vector<std::uint8_t>>(category_values_);
            auto result = Array::empty<bool>({data.size()}); auto out = result.edit().dense<bool>().data();
            for (std::size_t i=0; i<data.size(); ++i) out[i] = data[i] != 0;
            return Column::numeric(std::move(result));
        }
        case ar::Type::INT8: return numeric_categories<std::int8_t>();
        case ar::Type::UINT8: return numeric_categories<std::uint8_t>();
        case ar::Type::INT16: return numeric_categories<std::int16_t>();
        case ar::Type::UINT16: return numeric_categories<std::uint16_t>();
        case ar::Type::INT32: return numeric_categories<std::int32_t>();
        case ar::Type::UINT32: return numeric_categories<std::uint32_t>();
        case ar::Type::INT64: return numeric_categories<std::int64_t>();
        case ar::Type::UINT64: return numeric_categories<std::uint64_t>();
        case ar::Type::FLOAT: return numeric_categories<float>();
        case ar::Type::DOUBLE: return numeric_categories<double>();
        case ar::Type::STRING: case ar::Type::LARGE_STRING:
            return Column::from_packed_strings(std::move(bytes_),std::move(offsets_));
        default: fail(path_, "unsupported categorical value type", ErrorCode::unsupported_dtype);
        }
    }
public:
    StreamColumn(std::shared_ptr<ar::DataType> type, std::size_t count, std::string_view path, bool categories = false)
        : type_(std::move(type)), path_(path), total_(count) {
        if (categories) {
            switch (type_->id()) {
            case ar::Type::BOOL: category_values_.emplace<std::vector<std::uint8_t>>(); break;
            case ar::Type::INT8: category_values_.emplace<std::vector<std::int8_t>>(); break;
            case ar::Type::UINT8: category_values_.emplace<std::vector<std::uint8_t>>(); break;
            case ar::Type::INT16: category_values_.emplace<std::vector<std::int16_t>>(); break;
            case ar::Type::UINT16: category_values_.emplace<std::vector<std::uint16_t>>(); break;
            case ar::Type::INT32: category_values_.emplace<std::vector<std::int32_t>>(); break;
            case ar::Type::UINT32: category_values_.emplace<std::vector<std::uint32_t>>(); break;
            case ar::Type::INT64: category_values_.emplace<std::vector<std::int64_t>>(); break;
            case ar::Type::UINT64: category_values_.emplace<std::vector<std::uint64_t>>(); break;
            case ar::Type::FLOAT: category_values_.emplace<std::vector<float>>(); break;
            case ar::Type::DOUBLE: category_values_.emplace<std::vector<double>>(); break;
            case ar::Type::STRING: case ar::Type::LARGE_STRING: break;
            default: fail(path_, "unsupported categorical value type " + type_->ToString(), ErrorCode::unsupported_dtype);
            }
            return;
        }
        switch (type_->id()) {
        case ar::Type::BOOL: values_ = Array::empty<bool>({count}); break;
        case ar::Type::INT8: values_ = Array::empty<std::int8_t>({count}); break;
        case ar::Type::UINT8: values_ = Array::empty<std::uint8_t>({count}); break;
        case ar::Type::INT16: values_ = Array::empty<std::int16_t>({count}); break;
        case ar::Type::UINT16: values_ = Array::empty<std::uint16_t>({count}); break;
        case ar::Type::INT32: values_ = Array::empty<std::int32_t>({count}); break;
        case ar::Type::UINT32: values_ = Array::empty<std::uint32_t>({count}); break;
        case ar::Type::INT64: values_ = Array::empty<std::int64_t>({count}); break;
        case ar::Type::UINT64: values_ = Array::empty<std::uint64_t>({count}); break;
        case ar::Type::FLOAT: values_ = Array::empty<float>({count}); break;
        case ar::Type::DOUBLE: values_ = Array::empty<double>({count}); break;
        case ar::Type::STRING: case ar::Type::LARGE_STRING: offsets_.reserve(count+1); break;
        case ar::Type::DICTIONARY: {
            auto dt = std::static_pointer_cast<ar::DictionaryType>(type_);
            categories_ = std::make_unique<StreamColumn>(dt->value_type(), 0, path_, true);
            values_ = Array::empty<std::int64_t>({count});
            codes_ = values_.edit().dense<std::int64_t>().data();
            break;
        }
        default: fail(path_, "unsupported Parquet column type " + type_->ToString(), ErrorCode::unsupported_dtype);
        }

    }
    void append(const std::shared_ptr<ar::Array>& a) {
        if (!a->type()->Equals(type_)) fail(path_, "Parquet column type changed across batches");
        if (std::size_t(a->length()) > total_ - pos_) fail(path_, "Parquet column length overflow");
        switch (type_->id()) {
        case ar::Type::BOOL: copy_numeric<bool, ar::BooleanArray>(a); break;
        case ar::Type::INT8: copy_numeric<std::int8_t, ar::Int8Array>(a); break;
        case ar::Type::UINT8: copy_numeric<std::uint8_t, ar::UInt8Array>(a); break;
        case ar::Type::INT16: copy_numeric<std::int16_t, ar::Int16Array>(a); break;
        case ar::Type::UINT16: copy_numeric<std::uint16_t, ar::UInt16Array>(a); break;
        case ar::Type::INT32: copy_numeric<std::int32_t, ar::Int32Array>(a); break;
        case ar::Type::UINT32: copy_numeric<std::uint32_t, ar::UInt32Array>(a); break;
        case ar::Type::INT64: copy_numeric<std::int64_t, ar::Int64Array>(a); break;
        case ar::Type::UINT64: copy_numeric<std::uint64_t, ar::UInt64Array>(a); break;
        case ar::Type::FLOAT: copy_numeric<float, ar::FloatArray>(a); break;
        case ar::Type::DOUBLE: copy_numeric<double, ar::DoubleArray>(a); break;
        case ar::Type::STRING: copy_strings<ar::StringArray>(a); break;
        case ar::Type::LARGE_STRING: copy_strings<ar::LargeStringArray>(a); break;
        case ar::Type::DICTIONARY: {
            auto source = std::static_pointer_cast<ar::DictionaryArray>(a);
            auto dict = source->dictionary();
            auto ordered = std::static_pointer_cast<ar::DictionaryType>(type_)->ordered();
            // Include unused categories, preserving first-seen order. Ordered dictionary
            // batches must describe the same order; otherwise no valid total order is known.
            for (std::int64_t i=0; i<dict->length(); ++i) {
                auto code = categories_->intern(dict, i);
                if (ordered && code != std::size_t(i)) fail(path_, "ordered categorical dictionaries disagree");
            }
            for (std::int64_t i=0; i<a->length(); ++i, ++pos_) {
                if (a->IsNull(i)) { codes_[pos_] = -1; continue; }
                auto index = source->GetValueIndex(i);
                if (index < 0 || index >= dict->length()) fail(path_, "invalid categorical index");
                auto code = categories_->intern(dict,index);
                if (code > std::size_t(std::numeric_limits<std::int64_t>::max())) fail(path_, "too many categorical values");
                codes_[pos_] = std::int64_t(code);
            }
            break;
        }
        default: fail(path_, "unsupported Parquet column", ErrorCode::unsupported_dtype);
        }
    }
    Column finish() {
        if (pos_ != total_) fail(path_, "incomplete Parquet column");
        if (type_->id() == ar::Type::DICTIONARY) {
            auto dt = std::static_pointer_cast<ar::DictionaryType>(type_);
            return Column::categorical(std::move(values_), categories_->finish_categories(), dt->ordered());
        }
        if (!any_null_) validity_.clear();
        if (type_->id() == ar::Type::STRING || type_->id() == ar::Type::LARGE_STRING)
            return Column::from_packed_strings(std::move(bytes_), std::move(offsets_), std::move(validity_));
        return Column::numeric(std::move(values_),std::move(validity_));
    }
};


struct StreamTable {
    std::shared_ptr<ar::Schema> schema;
    std::vector<std::unique_ptr<StreamColumn>> columns;
    std::unique_ptr<StreamColumn> ids;
    std::vector<std::string> index;
    std::string index_field;
    std::string_view path;
    bool geometry, radius;
    StreamTable(std::shared_ptr<ar::Schema> s, std::size_t n, std::string_view p, bool geo, bool rad)
      : schema(std::move(s)), path(p), geometry(geo), radius(rad) {
        index_field = index_spec(schema, path).field;
        auto type = index_field.empty() ? ar::int64() : schema->GetFieldByName(index_field)->type();
        if (type->id() == ar::Type::DICTIONARY) type = std::static_pointer_cast<ar::DictionaryType>(type)->value_type();
        ids = std::make_unique<StreamColumn>(type, n, path);
        index.reserve(n);
        for (int i = 0; i < schema->num_fields(); ++i) {
            auto field = schema->field(i);
            if (field->name() == index_field || (geometry && field->name() == "geometry") || (radius && field->name() == "radius"))
                columns.push_back(nullptr);
            else columns.push_back(std::make_unique<StreamColumn>(field->type(), n, path));
        }
    }
    void append(const std::shared_ptr<ar::RecordBatch>& batch, std::shared_ptr<ar::Array> id) {
        id = decode_index(std::move(id), path);
        append_names(index, id, path);
        ids->append(id);
        for (int i = 0; i < schema->num_fields(); ++i) if (columns[i]) columns[i]->append(batch->column(i));
    }
    Table finish() {
        auto out = Table::with_index(std::move(index), index_name(schema, path));
        for (int i = 0; i < schema->num_fields(); ++i) if (columns[i]) out.set(schema->field(i)->name(), columns[i]->finish());
        return out;
    }
};
} // namespace

SpatialPoints read_parquet_points(std::string_view path, std::size_t first,
                                 std::size_t count, std::vector<SpatialAxis> axes, std::size_t scratch_bytes) {
    if (scratch_bytes < 64) fail(path, "scratch budget must be at least 64 bytes", ErrorCode::invalid_argument);
    std::unique_ptr<StreamTable> table;
    scan_batches(path, first, count,
        [&](auto schema, auto n) { table = std::make_unique<StreamTable>(schema, n, path, false, false); },
        [&](auto batch, auto ids) { table->append(batch, ids); });
    auto ids = table->ids->finish();
    auto values = table->finish();
    return SpatialPoints(std::move(values), std::move(ids), std::move(axes));
}

SpatialShapes read_parquet_shapes(std::string_view path, std::size_t scratch_bytes) {
    return read_parquet_shapes(path, 0, std::numeric_limits<std::size_t>::max(), scratch_bytes);
}
SpatialShapes read_parquet_shapes(std::string_view path, std::size_t first, std::size_t count, std::size_t scratch_bytes) {
    if (scratch_bytes < 64) fail(path, "scratch budget must be at least 64 bytes", ErrorCode::invalid_argument);
    // Probe a single geometry to identify the element kind even for an empty selection.
    bool circles = false, known = false;
    std::string encoding;
    scan_batches(path, 0, 1,
        [&](auto schema, auto) { encoding = geometry_encoding(schema, path); },
        [&](auto batch, auto) {
            auto i = batch->schema()->GetFieldIndex("geometry");
            if (i < 0) fail(path, "GeoParquet geometry column is missing");
            auto value = batch->column(i);
            if (value->IsNull(0)) fail(path, "null shape geometry");
            if (encoding == "point") circles = true;
            else if (encoding == "polygon" || encoding == "multipolygon") circles = false;
            else if (encoding == "WKB") {
                std::string_view bytes;
                if (value->type_id() == ar::Type::BINARY) bytes = std::static_pointer_cast<ar::BinaryArray>(value)->GetView(0);
                else if (value->type_id() == ar::Type::LARGE_BINARY) bytes = std::static_pointer_cast<ar::LargeBinaryArray>(value)->GetView(0);
                else fail(path, "WKB geometry must be binary", ErrorCode::unsupported_dtype);
                try {
                    ::anndatapp_spatial_wkb::Reader reader(bytes.data(), bytes.size());
                    auto order = reader.order(); auto type = reader.u32(order);
                    if (type != 1 && type != 3 && type != 6) fail(path, "unsupported WKB geometry type");
                    circles = type == 1;
                } catch (const std::runtime_error& e) { fail(path, e.what()); }
            } else fail(path, "unsupported GeoParquet geometry encoding '" + encoding + "'");
            known = true;
        });
    if (!known) fail(path, "SpatialData shape payload must contain a row");
    std::unique_ptr<StreamTable> table;
    Geometries decoded;
    std::vector<double> radii;
    scan_batches(path, first, count,
        [&](auto schema, auto n) {
            if (schema->GetFieldIndex("geometry") < 0) fail(path, "GeoParquet geometry column is missing");
            if (circles && schema->GetFieldIndex("radius") < 0) fail(path, "circle radius column is missing");
            if (circles) radii.reserve(n);
            table = std::make_unique<StreamTable>(schema, n, path, true, circles);
        },
        [&](auto batch, auto ids) {
            auto previous = decoded.types.size();
            append_geometry(decoded, batch->GetColumnByName("geometry"), encoding, path);
            for (auto i = previous; i < decoded.types.size(); ++i)
                if ((decoded.types[i] == 1) != circles) fail(path, "mixed circle and polygon geometry");
            if (circles) {
                auto values = batch->GetColumnByName("radius");
                for (std::int64_t i = 0; i < values->length(); ++i) radii.push_back(number(values, i, path));
            }
            table->append(batch, ids);
        });
    auto ids = table->ids->finish();
    auto attrs = table->finish();
    auto vertices = decoded.xy.size()/2;
    auto coordinates = owned_array<double>(std::move(decoded.xy), {vertices, 2});
    if (circles) {
        auto n = radii.size();
        return SpatialShapes::circles(std::move(coordinates), owned_array<double>(std::move(radii), {n}), std::move(ids), std::move(attrs));
    }
    auto nr = decoded.rings.size(), np = decoded.polygons.size(), nf = decoded.features.size(), nt = decoded.types.size();
    auto ring = owned_array<std::uint64_t>(std::move(decoded.rings), {nr});
    auto polygon = owned_array<std::uint64_t>(std::move(decoded.polygons), {np});
    auto feature = owned_array<std::uint64_t>(std::move(decoded.features), {nf});
    auto types = owned_array<std::uint8_t>(std::move(decoded.types), {nt});
    return SpatialShapes::polygons(std::move(coordinates), std::move(ring), std::move(polygon),
        std::move(feature), std::move(ids), std::move(attrs), std::move(types));
}

void write_parquet_points(std::string_view path, const SpatialPointsView& points,
                          std::size_t row_group_rows, std::size_t scratch_bytes) {
    if (scratch_bytes < 64) fail(path, "scratch budget must be at least 64 bytes", ErrorCode::invalid_argument);
    if (!row_group_rows) fail(path, "Parquet row-group size must be nonzero", ErrorCode::invalid_argument);
    auto n = points.size(), batch_size = std::min<std::size_t>({row_group_rows, 65536, scratch_bytes / 16});
    auto index = index_field(points.values());
    auto first = points_batch(points, index, 0, std::min(n, batch_size), path, scratch_bytes);
    auto arrow_schema = schema(first, index, points.values().index_name(), false);
    fs::path dir(path);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) fail(path, ec.message(), ErrorCode::io_error);
    auto file = (dir / "part.0.parquet").string();
    auto writer = open_writer(file, arrow_schema);
    check(writer->WriteTable(*arrow_table(arrow_schema, first), static_cast<std::int64_t>(batch_size)), file);
    for (std::size_t pos = std::min(n, batch_size); pos < n; pos += std::min(n-pos, batch_size)) {
        auto batch = points_batch(points, index, pos, std::min(n-pos, batch_size), path, scratch_bytes);
        check(writer->WriteTable(*arrow_table(arrow_schema, batch), static_cast<std::int64_t>(batch_size)), file);
    }
    check(writer->Close(), file);
}

void write_parquet_shapes(std::string_view path, const SpatialShapesView& shapes,
                          std::size_t row_group_rows, std::size_t scratch_bytes) {
    if (scratch_bytes < 64) fail(path, "scratch budget must be at least 64 bytes", ErrorCode::invalid_argument);
    if (!row_group_rows) fail(path, "Parquet row-group size must be nonzero", ErrorCode::invalid_argument);
    auto n = shapes.size(), batch_size = std::min<std::size_t>({row_group_rows, 65536, scratch_bytes / 16});
    if (!n) fail(path, "SpatialData shape payload must contain a row", ErrorCode::invalid_argument);
    auto index = index_field(shapes.attributes());
    auto first = shapes_batch(shapes, index, 0, std::min(n, batch_size), path, scratch_bytes);
    std::vector<std::string> geometry_types;
    if (shapes.kind() == SpatialGeometryKind::circles) geometry_types.push_back("Point");
    else {
        bool polygon = false, multi = false;
        auto types = shapes.geometry_types().dense<std::uint8_t>();
        for (std::size_t i = 0; i < n; ++i) {
            auto type = at1(types, shapes.source_row(i), path);
            if (type == 3) polygon = true;
            else if (type == 6) multi = true;
            else fail(path, "unsupported polygon geometry type");
        }
        if (polygon) geometry_types.push_back("Polygon");
        if (multi) geometry_types.push_back("MultiPolygon");
    }
    auto arrow_schema = schema(first, index, shapes.attributes().index_name(), true, geometry_types);
    auto writer = open_writer(std::string(path), arrow_schema);
    check(writer->WriteTable(*arrow_table(arrow_schema, first), static_cast<std::int64_t>(batch_size)), path);
    for (std::size_t pos = std::min(n, batch_size); pos < n; pos += std::min(n-pos, batch_size)) {
        auto batch = shapes_batch(shapes, index, pos, std::min(n-pos, batch_size), path, scratch_bytes);
        check(writer->WriteTable(*arrow_table(arrow_schema, batch), static_cast<std::int64_t>(batch_size)), path);
    }
    check(writer->Close(), path);
}
#endif

} // namespace anndatapp::spatial_detail
