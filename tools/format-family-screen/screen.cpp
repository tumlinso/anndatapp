#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <unistd.h>
#include <utility>
#include <vector>

import anndatapp.dataset;
import anndatapp.h5ad;

using namespace anndatapp;
namespace fs = std::filesystem;

namespace {

struct Usage {
    long rss_peak_kib = 0;
    long minor_faults = 0;
    long major_faults = 0;
};

struct Measurement {
    double elapsed_ns = 0;
    long rss_peak_kib_before = 0;
    long rss_peak_kib_after = 0;
    long rss_peak_kib_delta = 0;
    long minor_faults_delta = 0;
    long major_faults_delta = 0;
};

Usage usage() {
    rusage value{};
    if (getrusage(RUSAGE_SELF, &value) != 0)
        throw std::runtime_error("getrusage(RUSAGE_SELF) failed");
    return {value.ru_maxrss, value.ru_minflt, value.ru_majflt};
}

template<class F>
Measurement measure(F&& operation) {
    const auto before = usage();
    const auto start = std::chrono::steady_clock::now();
    operation();
    const auto finish = std::chrono::steady_clock::now();
    const auto after = usage();
    return {
        std::chrono::duration<double, std::nano>(finish - start).count(),
        before.rss_peak_kib,
        after.rss_peak_kib,
        after.rss_peak_kib - before.rss_peak_kib,
        after.minor_faults - before.minor_faults,
        after.major_faults - before.major_faults
    };
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

const char* dtype_name(DType type) {
    switch (type) {
    case DType::boolean: return "bool";
    case DType::int8: return "int8";
    case DType::uint8: return "uint8";
    case DType::int16: return "int16";
    case DType::uint16: return "uint16";
    case DType::int32: return "int32";
    case DType::uint32: return "uint32";
    case DType::int64: return "int64";
    case DType::uint64: return "uint64";
    case DType::float32: return "float32";
    case DType::float64: return "float64";
    }
    return "unknown";
}

const char* layout_name(Layout layout) {
    switch (layout) {
    case Layout::dense: return "dense";
    case Layout::csr: return "csr";
    case Layout::csc: return "csc";
    }
    return "unknown";
}

std::string json_string(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const unsigned char c : value) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                result += "\\u00";
                result.push_back(hex[c >> 4]);
                result.push_back(hex[c & 15]);
            } else result.push_back(static_cast<char>(c));
        }
    }
    result.push_back('"');
    return result;
}

void print_measurement(const Measurement& m) {
    std::cout << "{\"elapsed_ns\":" << m.elapsed_ns
              << ",\"rss_peak_kib_before\":" << m.rss_peak_kib_before
              << ",\"rss_peak_kib_after\":" << m.rss_peak_kib_after
              << ",\"rss_peak_kib_delta\":" << m.rss_peak_kib_delta
              << ",\"minor_faults_delta\":" << m.minor_faults_delta
              << ",\"major_faults_delta\":" << m.major_faults_delta << '}';
}

void print_shape(const std::vector<std::size_t>& shape) {
    std::cout << '[';
    for (std::size_t i = 0; i < shape.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << shape[i];
    }
    std::cout << ']';
}

std::size_t checked_bytes(std::size_t count, std::size_t width) {
    if (width && count > std::numeric_limits<std::size_t>::max() / width)
        throw std::runtime_error("buffer byte size overflows size_t");
    return count * width;
}

std::size_t word_count(std::size_t count, std::size_t width) {
    const auto bytes = checked_bytes(count, width);
    if (bytes > std::numeric_limits<std::size_t>::max() - (sizeof(std::uint64_t) - 1))
        throw std::runtime_error("aligned buffer size overflows size_t");
    return (bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
}

std::vector<std::ptrdiff_t> spread_positions(std::size_t extent, std::size_t limit) {
    const auto count = std::min(extent, limit);
    if (extent && extent - 1 > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
        throw std::runtime_error("selector extent exceeds signed position range");
    std::vector<std::ptrdiff_t> positions;
    positions.reserve(count);
    if (!count) return positions;
    if (count == 1) {
        positions.push_back(static_cast<std::ptrdiff_t>((extent - 1) / 2));
        return positions;
    }
    const auto denominator = count - 1;
    const auto distance = extent - 1;
    const auto quotient = distance / denominator;
    const auto remainder = distance % denominator;
    for (std::size_t i = 0; i < count; ++i) {
        // The quotient product is bounded by extent; the remainder product is
        // bounded by limit squared (64 squared for this screen).
        const auto position = quotient * i + (remainder * i) / denominator;
        positions.push_back(static_cast<std::ptrdiff_t>(position));
    }
    return positions;
}

struct SparseDestination {
    std::vector<std::uint64_t> values;
    std::vector<std::uint64_t> indices;
    std::vector<std::uint64_t> offsets;
    SparseBuffer descriptor{};
};

SparseDestination make_destination(const ReadRequirements& requirements) {
    require(requirements.layout != Layout::dense, "sparse screen requires sparse X");
    require(requirements.shape.size() == 2, "sparse requirements must have rank two");
    const auto major = requirements.layout == Layout::csr
        ? requirements.shape[0] : requirements.shape[1];
    const auto offset_count = major + 1;
    if (offset_count == 0) throw std::runtime_error("sparse offset count overflows size_t");

    SparseDestination result;
    result.values.resize(word_count(requirements.nnz, dtype_size(requirements.dtype)));
    result.indices.resize(word_count(requirements.nnz, dtype_size(requirements.index_dtype)));
    result.offsets.resize(word_count(offset_count, dtype_size(requirements.offset_dtype)));
    result.descriptor = {
        result.values.empty() ? nullptr : result.values.data(),
        result.indices.empty() ? nullptr : result.indices.data(),
        result.offsets.data(),
        requirements.dtype,
        requirements.index_dtype,
        requirements.offset_dtype,
        requirements.nnz,
        requirements.nnz,
        offset_count
    };
    return result;
}

std::uint64_t checksum_bytes(const void* data, std::size_t size, std::uint64_t hash) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    constexpr std::uint64_t prime = 1099511628211ull;
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * prime;
    return hash;
}

std::uint64_t checksum(const SparseDestination& buffer, const ReadRequirements& r) {
    constexpr std::uint64_t seed = 14695981039346656037ull;
    auto hash = checksum_bytes(buffer.values.data(), checked_bytes(r.nnz, dtype_size(r.dtype)), seed);
    hash = checksum_bytes(buffer.indices.data(), checked_bytes(r.nnz, dtype_size(r.index_dtype)), hash);
    const auto major = r.layout == Layout::csr ? r.shape[0] : r.shape[1];
    return checksum_bytes(buffer.offsets.data(), checked_bytes(major + 1, dtype_size(r.offset_dtype)), hash);
}

template<class View, class Requirements, class Read>
void run_sparse_read(View& selected, std::string_view case_name,
                     std::string_view input_path, std::size_t selected_rows,
                     std::size_t selected_genes, Requirements&& get_requirements,
                     Read&& read_into) {
    const auto requirements = get_requirements(selected);
    require(requirements.layout == Layout::csr,
            "selected sparse transfer screen expects CSR X");
    auto destination = make_destination(requirements);
    const auto measurement = measure([&] { read_into(selected, destination.descriptor); });
    const auto value_checksum = checksum(destination, requirements);

    std::cout << "{\"case\":" << json_string(case_name)
              << ",\"input\":" << json_string(input_path)
              << ",\"layout\":\"csr\",\"shape\":";
    print_shape(requirements.shape);
    std::cout << ",\"selected_rows\":" << selected_rows
              << ",\"selected_genes\":" << selected_genes
              << ",\"nnz\":" << requirements.nnz
              << ",\"value_dtype\":" << json_string(dtype_name(requirements.dtype))
              << ",\"index_dtype\":" << json_string(dtype_name(requirements.index_dtype))
              << ",\"offset_dtype\":" << json_string(dtype_name(requirements.offset_dtype))
              << ",\"correctness\":\"cross_version_checksum_only\""
              << ",\"checksum_fnv1a64\":" << value_checksum
              << ",\"measurement\":";
    print_measurement(measurement);
    std::cout << "}\n";
}

std::uint64_t touch_pages(const MappedArray& mapped, std::size_t page_size) {
    if (mapped.byte_size() && !mapped.data())
        throw std::runtime_error("nonempty physical mapping returned a null pointer");
    const auto* bytes = reinterpret_cast<const volatile unsigned char*>(mapped.data());
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < mapped.byte_size(); i += page_size)
        hash = (hash ^ bytes[i]) * prime;
    if (mapped.byte_size() && (mapped.byte_size() - 1) % page_size != 0)
        hash = (hash ^ bytes[mapped.byte_size() - 1]) * prime;
    return hash;
}

struct MappingResult {
    std::string name;
    std::string path;
    BackedArray source;
    MappedArray mapping;
    Measurement map_measurement;
    Measurement first_touch_measurement;
    Measurement warm_touch_measurement;
    std::uint64_t first_checksum = 0;
    std::uint64_t warm_checksum = 0;
};

MappingResult map_child(BackedStore& store, std::string name, std::string path,
                        std::size_t page_size) {
    MappingResult result;
    result.name = std::move(name);
    result.path = std::move(path);
    result.source = store.array(result.path);
    result.map_measurement = measure([&] { result.mapping = result.source.map_readonly(); });
    result.first_touch_measurement = measure([&] {
        result.first_checksum = touch_pages(result.mapping, page_size);
    });
    result.warm_touch_measurement = measure([&] {
        result.warm_checksum = touch_pages(result.mapping, page_size);
    });
    require(result.first_checksum == result.warm_checksum,
            "physical mapping checksum changed between first and warm touch");
    return result;
}

void print_mapping_result(const MappingResult& result) {
    const auto shape = result.source.shape();
    std::cout << "{\"name\":" << json_string(result.name)
              << ",\"path\":" << json_string(result.path)
              << ",\"dtype\":" << json_string(dtype_name(result.mapping.dtype()))
              << ",\"shape\":";
    print_shape(shape);
    std::cout << ",\"byte_size\":" << result.mapping.byte_size()
              << ",\"map\":";
    print_measurement(result.map_measurement);
    std::cout << ",\"first_touch\":{\"checksum_fnv1a64\":"
              << result.first_checksum << ",\"measurement\":";
    print_measurement(result.first_touch_measurement);
    std::cout << "},\"warm_touch\":{\"checksum_fnv1a64\":"
              << result.warm_checksum << ",\"measurement\":";
    print_measurement(result.warm_touch_measurement);
    std::cout << "}}";
}

struct Inputs {
    std::string manifest;
    std::string component;
};

bool is_monolithic_case(std::string_view name) {
    return name == "mono-open" || name == "mono-read";
}

Inputs resolve_inputs(int argc, char** argv) {
    const std::string_view case_name = argv[1];
    const fs::path first(argv[2]);
    if (first != "-" && fs::is_directory(first)) {
        const bool mono = is_monolithic_case(case_name);
        auto component = argc >= 4
            ? fs::path(argv[3])
            : fs::path(mono ? "monolithic.h5ad" : "part-0.h5ad");
        if (component.is_relative()) component = first / component;
        return {
            mono || case_name == "map" ? "-" : (first / "collection.h5ads").string(),
            component.string()
        };
    }
    if (argc < 4) throw std::runtime_error("a component or monolithic H5AD path is required");
    return {argv[2], argv[3]};
}

void print_usage(const char* program) {
    std::cerr << "usage: " << program
              << " <open|inspect|view|read|map|mono-open|mono-read>"
                 " <collection.h5ads|fixture-directory|-> [component-or-monolithic.h5ad]\n";
}

void run_collection_case(std::string_view case_name, const Inputs& input) {
    if (case_name == "open") {
        AnnDataSet data;
        const auto measurement = measure([&] { data = open_h5ads(input.manifest); });
        std::cout << "{\"case\":\"open\",\"manifest\":" << json_string(input.manifest)
                  << ",\"n_obs\":" << data.n_obs() << ",\"n_vars\":" << data.n_vars()
                  << ",\"n_components\":" << data.n_components()
                  << ",\"has_x\":" << (data.has_x() ? "true" : "false")
                  << ",\"measurement\":";
        print_measurement(measurement);
        std::cout << "}\n";
        return;
    }

    if (case_name == "map") {
        auto store = open_hdf5_store(input.component);
        auto logical = store.anndata().x();
        const auto layout = logical.layout();
        const auto value_dtype = logical.dtype();
        const auto index_dtype = logical.index_dtype();
        const auto offset_dtype = logical.offset_dtype();
        require(layout == Layout::csr, "physical mapping screen expects CSR X");
        auto physical_values = store.array("/X/data");
        auto physical_indices = store.array("/X/indices");
        auto physical_indptr = store.array("/X/indptr");
        const auto expected_values = physical_values.shape();
        const auto expected_indices = physical_indices.shape();
        const auto expected_indptr = physical_indptr.shape();
        require(!expected_values.empty() && !expected_indices.empty() && !expected_indptr.empty(),
                "physical CSR buffers must be rank one");
        require(expected_values[0] == expected_indices[0],
                "physical CSR data/index extents differ");
        require(expected_values.size() == 1 && expected_indices.size() == 1 && expected_indptr.size() == 1,
                "physical CSR buffers must be rank one");
        const auto nnz = expected_values[0];
        const auto page = sysconf(_SC_PAGESIZE);
        const auto page_size = page > 0 ? static_cast<std::size_t>(page) : std::size_t{4096};
        auto data = map_child(store, "data", "/X/data", page_size);
        auto indices = map_child(store, "indices", "/X/indices", page_size);
        auto indptr = map_child(store, "indptr", "/X/indptr", page_size);
        std::cout << "{\"case\":\"map\",\"component\":" << json_string(input.component)
                  << ",\"logical_layout\":\"csr\",\"nnz\":" << nnz
                  << ",\"value_dtype\":" << json_string(dtype_name(value_dtype))
                  << ",\"index_dtype\":" << json_string(dtype_name(index_dtype))
                  << ",\"offset_dtype\":" << json_string(dtype_name(offset_dtype))
                  << ",\"offset_count\":" << expected_indptr[0]
                  << ",\"page_size\":" << page_size << ",\"arrays\":[";
        print_mapping_result(data);
        std::cout << ',';
        print_mapping_result(indices);
        std::cout << ',';
        print_mapping_result(indptr);
        std::cout << "]}\n";
        return;
    }

    auto data = open_h5ads(input.manifest);
    if (case_name == "inspect") {
        std::size_t n_obs = 0, n_vars = 0, n_components = 0;
        bool has_x = false;
        std::vector<std::size_t> x_shape;
        Layout x_layout = Layout::dense;
        DType x_dtype = DType::float64;
        std::string first_key;
        const auto measurement = measure([&] {
            n_obs = data.n_obs();
            n_vars = data.n_vars();
            n_components = data.n_components();
            has_x = data.has_x();
            const auto components = data.view().components();
            if (!components.empty()) first_key = components.front().key;
            if (has_x && n_components) {
                auto x = data.view().component(0).x();
                x_shape = x.shape();
                x_layout = x.layout();
                x_dtype = x.dtype();
            }
        });
        std::cout << "{\"case\":\"inspect\",\"manifest\":" << json_string(input.manifest)
                  << ",\"n_obs\":" << n_obs << ",\"n_vars\":" << n_vars
                  << ",\"n_components\":" << n_components
                  << ",\"has_x\":" << (has_x ? "true" : "false")
                  << ",\"first_component_key\":" << json_string(first_key)
                  << ",\"first_x_layout\":" << json_string(layout_name(x_layout))
                  << ",\"first_x_dtype\":" << json_string(dtype_name(x_dtype))
                  << ",\"first_x_shape\":";
        print_shape(x_shape);
        std::cout << ",\"measurement\":";
        print_measurement(measurement);
        std::cout << "}\n";
        return;
    }

    if (case_name == "view") {
        require(data.n_obs() <= static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) &&
                data.n_vars() <= static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()),
                "collection shape exceeds signed positional range");
        AnnDataSetView selected;
        const auto measurement = measure([&] {
            selected = data.slice(range{0, std::nullopt, 2},
                                  range{0, std::nullopt, 3});
        });
        std::cout << "{\"case\":\"view\",\"manifest\":" << json_string(input.manifest)
                  << ",\"selector\":\"positional_range\",\"row_step\":2,\"gene_step\":3"
                  << ",\"shape\":[" << selected.n_obs() << ',' << selected.n_vars() << ']'
                  << ",\"measurement\":";
        print_measurement(measurement);
        std::cout << "}\n";
        return;
    }

    if (case_name == "read") {
        const auto rows = spread_positions(data.n_obs(), 64);
        const auto genes = spread_positions(data.n_vars(), 64);
        auto selected = data.slice(indices(rows), indices(genes));
        run_sparse_read(selected, case_name, input.manifest, rows.size(), genes.size(),
            [](auto& view) { return view.requirements(); },
            [](auto& view, const SparseBuffer& destination) { view.read_x_into(destination); });
        return;
    }

    throw std::runtime_error("unknown collection case");
}

void run_monolithic_case(std::string_view case_name, const Inputs& input) {
    if (case_name == "mono-open") {
        BackedAnnData data;
        const auto measurement = measure([&] { data = open_h5ad(input.component); });
        std::cout << "{\"case\":\"mono-open\",\"component\":" << json_string(input.component)
                  << ",\"n_obs\":" << data.n_obs() << ",\"n_vars\":" << data.n_vars()
                  << ",\"has_x\":" << (data.has_x() ? "true" : "false")
                  << ",\"measurement\":";
        print_measurement(measurement);
        std::cout << "}\n";
        return;
    }
    if (case_name == "mono-read") {
        auto data = open_h5ad(input.component);
        const auto rows = spread_positions(data.n_obs(), 64);
        const auto genes = spread_positions(data.n_vars(), 64);
        auto selected = data.slice(indices(rows), indices(genes));
        auto prepared = selected.x().prepare_read();
        run_sparse_read(prepared, case_name, input.component, rows.size(), genes.size(),
            [](auto& read) { return read.requirements(); },
            [](auto& read, const SparseBuffer& destination) { read.read_into(destination); });
        return;
    }
    throw std::runtime_error("unknown monolithic case");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        print_usage(argv[0]);
        return 2;
    }
    try {
        const std::string_view case_name = argv[1];
        const auto input = resolve_inputs(argc, argv);
        if (is_monolithic_case(case_name)) run_monolithic_case(case_name, input);
        else run_collection_case(case_name, input);
    } catch (const std::exception& error) {
        std::cerr << "format-family-screen: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
