#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

import anndatapp.dataset;
import anndatapp.spatialdata;
import anndatapp.zarr;

using namespace anndatapp;
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
long long ns(Time a, Time b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}

struct Observation {
    std::uint64_t io[16]{}, alloc[2]{};
};
void add(Observation& to, const Observation& from) {
    for (int i = 0; i < 16; ++i) to.io[i] += from.io[i];
    for (int i = 0; i < 2; ++i) to.alloc[i] += from.alloc[i];
}
struct Observer {
    using Begin = void (*)();
    using End = void (*)(std::uint64_t*);
    Begin io_begin = nullptr, alloc_begin = nullptr;
    End io_end = nullptr, alloc_end = nullptr;
    bool enabled = false;

    explicit Observer(bool diagnostic) : enabled(diagnostic) {
        if (!enabled) return;
        io_begin = reinterpret_cast<Begin>(dlsym(RTLD_DEFAULT, "v1_io_begin"));
        io_end = reinterpret_cast<End>(dlsym(RTLD_DEFAULT, "v1_io_end"));
        alloc_begin = reinterpret_cast<Begin>(dlsym(RTLD_DEFAULT, "v1_alloc_begin"));
        alloc_end = reinterpret_cast<End>(dlsym(RTLD_DEFAULT, "v1_alloc_end"));
        require(io_begin && io_end && alloc_begin && alloc_end,
                "--diagnostic requires v1 I/O and allocation interposers");
    }
    void start() const {
        if (enabled) { io_begin(); alloc_begin(); }
    }
    Observation stop_phase() const {
        Observation out{};
        if (enabled) { alloc_end(out.alloc); io_end(out.io); }
        return out;
    }
};

void print_observation(const Observation& x) {
    std::cout << "{\"v1_io_slots\":[";
    for (int i = 0; i < 16; ++i) {
        if (i) std::cout << ',';
        if (i < 4) std::cout << x.io[i];
        else std::cout << "null";
    }
    std::cout << "],\"cpp_new_calls\":" << x.alloc[0]
              << ",\"cpp_new_bytes\":" << x.alloc[1] << '}';
}
void print_diagnostic(const Observation& total, const Observation& open,
                      const Observation& selection, const Observation& preparation,
                      const Observation& allocation, const Observation& transfer,
                      bool has_preparation) {
    std::cout << ",\"diagnostic\":{\"scope\":\"open_through_transfer\""
              << ",\"intercepted_read_calls\":" << total.io[0]
              << ",\"intercepted_read_bytes\":" << total.io[1]
              << ",\"intercepted_write_calls\":" << total.io[2]
              << ",\"intercepted_write_bytes\":" << total.io[3]
              << ",\"h5dread_calls\":null,\"h5dwrite_calls\":null"
              << ",\"cpp_new_calls\":" << total.alloc[0]
              << ",\"cpp_new_bytes\":" << total.alloc[1]
              << ",\"phases\":{\"open\":";
    print_observation(open);
    std::cout << ",\"selection\":";
    print_observation(selection);
    if (has_preparation) {
        std::cout << ",\"preparation_requirements\":";
        print_observation(preparation);
    }
    std::cout << ",\"buffer_allocation\":";
    print_observation(allocation);
    std::cout << ",\"transfer\":";
    print_observation(transfer);
    std::cout << "}}";
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

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
void hash_byte(std::uint64_t& hash, std::uint8_t byte) {
    hash = (hash ^ byte) * kFnvPrime;
}
template<class T> void hash_little_endian(std::uint64_t& hash, T value) {
    using U = std::make_unsigned_t<T>;
    const auto bits = static_cast<U>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i)
        hash_byte(hash, static_cast<std::uint8_t>(bits >> (8 * i)));
}
std::string hex64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}
std::string fnv_raster(const std::vector<std::uint16_t>& values) {
    auto hash = kFnvOffset;
    for (const auto value : values) hash_little_endian(hash, value);
    return hex64(hash);
}

struct RasterCase {
    std::size_t source_width, y0, y_step, x0, x_step;
    const char* selector;
};
RasterCase raster_case(std::string_view name) {
    if (name == "raster-decimated") return {256, 16, 2, 32, 2, "channels=all;rows=16:80:2;cols=32:96:2"};
    if (name == "raster-aligned") return {256, 16, 1, 32, 1, "channels=all;rows=16:48:1;cols=32:64:1"};
    if (name == "raster-multichunk") return {512, 96, 4, 96, 4, "channels=all;rows=96:224:4;cols=96:224:4"};
    throw std::runtime_error("unknown raster case");
}
BackedArray select_raster(BackedArray source, const RasterCase& c) {
    require(source.dtype() == DType::uint16, "expected uint16 raster source");
    require(source.shape() == std::vector<std::size_t>{1, c.source_width, c.source_width},
            "unexpected raster source shape");
    return source.select({all,
        range{static_cast<std::ptrdiff_t>(c.y0), static_cast<std::ptrdiff_t>(c.y0 + 32 * c.y_step), static_cast<std::ptrdiff_t>(c.y_step)},
        range{static_cast<std::ptrdiff_t>(c.x0), static_cast<std::ptrdiff_t>(c.x0 + 32 * c.x_step), static_cast<std::ptrdiff_t>(c.x_step)}});
}
void verify_raster(const std::vector<std::uint16_t>& values, const RasterCase& c) {
    require(values.size() == 1024, "incorrect raster output size");
    for (std::size_t y = 0; y < 32; ++y) {
        for (std::size_t x = 0; x < 32; ++x) {
            const auto source_y = c.y0 + c.y_step * y;
            const auto source_x = c.x0 + c.x_step * x;
            const auto expected = static_cast<std::uint16_t>((c.source_width * source_y + source_x) % 65536);
            require(values[y * 32 + x] == expected, "incorrect raster value");
        }
    }
}
void run_raster(std::string_view name, const char* fixture, bool raw,
                Observer& observer, Time start, Time opened, BackedArray source,
                const Observation& open_observation) {
    const auto c = raster_case(raw ? name.substr(0, name.size() - 4) : name);
    auto selected = select_raster(std::move(source), c);
    require(selected.shape() == std::vector<std::size_t>{1, 32, 32}, "incorrect selected raster shape");
    const auto selected_at = Clock::now();
    const auto selection_observation = observer.stop_phase();
    observer.start();
    std::vector<std::uint16_t> values(1024);
    auto destination = DenseBuffer::from(values.data(), {1, 32, 32});
    const auto allocated = Clock::now();
    const auto allocation_observation = observer.stop_phase();
    observer.start();
    selected.read_into(destination);
    const auto finished = Clock::now();
    const auto transfer_observation = observer.stop_phase();
    verify_raster(values, c);
    Observation total = open_observation;
    add(total, selection_observation);
    add(total, allocation_observation);
    add(total, transfer_observation);
    std::cout << "{\"case\":\"" << name << "\",\"api\":\""
              << (raw ? "raw_store_array" : "spatialdata_raster")
              << "\",\"fixture\":\"" << fixture << "\",\"level\":0"
              << ",\"source_shape\":[1," << c.source_width << ',' << c.source_width << ']'
              << ",\"shape\":[1,32,32],\"selector\":\"" << c.selector << "\""
              << ",\"dtype\":\"uint16\",\"dtype_width_bytes\":2"
              << ",\"golden\":\"pass\",\"output_fnv1a64\":\"" << fnv_raster(values) << '\"'
              << ",\"open_ns\":" << ns(start, opened)
              << ",\"selection_ns\":" << ns(opened, selected_at)
              << ",\"allocation_ns\":" << ns(selected_at, allocated)
              << ",\"transfer_ns\":" << ns(allocated, finished)
              << ",\"end_to_end_ns\":" << ns(start, finished);
    if (observer.enabled) {
        print_diagnostic(total, open_observation, selection_observation, {},
                         allocation_observation, transfer_observation, false);
    }
    std::cout << "}\n";
}

std::size_t words(std::size_t count, DType type) {
    return (count * dtype_size(type) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
}
struct Buffers {
    std::vector<std::uint64_t> values, indices, offsets;
    SparseBuffer dst;
    explicit Buffers(const ReadRequirements& req)
        : values(words(req.nnz, req.dtype)),
          indices(words(req.nnz, req.index_dtype)),
          offsets(words(req.shape.at(0) + 1, req.offset_dtype)),
          dst{values.data(), indices.data(), offsets.data(), req.dtype,
              req.index_dtype, req.offset_dtype, req.nnz, req.nnz,
              req.shape.at(0) + 1} {}
};
template<class T> T cell(const std::vector<std::uint64_t>& buffer, std::size_t i) {
    T value;
    std::memcpy(&value, reinterpret_cast<const std::byte*>(buffer.data()) + i * sizeof(T), sizeof(T));
    return value;
}
std::uint64_t integer(const std::vector<std::uint64_t>& buffer, DType type, std::size_t i) {
    switch (type) {
    case DType::int32: {
        const auto value = cell<std::int32_t>(buffer, i);
        require(value >= 0, "negative sparse index or offset");
        return static_cast<std::uint64_t>(value);
    }
    case DType::uint32: return cell<std::uint32_t>(buffer, i);
    case DType::int64: {
        const auto value = cell<std::int64_t>(buffer, i);
        require(value >= 0, "negative sparse index or offset");
        return static_cast<std::uint64_t>(value);
    }
    case DType::uint64: return cell<std::uint64_t>(buffer, i);
    default: throw std::runtime_error("unexpected sparse integer dtype");
    }
}
void verify_dataset_requirements(const ReadRequirements& req, bool tenk) {
    require(req.layout == Layout::csr && req.dtype == DType::float32,
            "expected float32 CSR output");
    require(req.index_dtype == DType::int32 &&
            req.offset_dtype == (tenk ? DType::int64 : DType::int32),
            "unexpected native sparse index or offset dtype");
    require(req.shape == std::vector<std::size_t>{64, 64}, "incorrect dataset output shape");
}
void verify_small(const Buffers& b, const ReadRequirements& req) {
    verify_dataset_requirements(req, false);
    require(req.nnz == 512, "incorrect small dataset nnz");
    std::size_t next = 0;
    for (std::size_t i = 0; i < 64; ++i) {
        require(integer(b.offsets, req.offset_dtype, i) == next, "incorrect small dataset row offset");
        for (std::size_t k = 0; k < 8; ++k) {
            require(integer(b.indices, req.index_dtype, next) == 8 * k,
                    "incorrect small dataset column index");
            require(cell<float>(b.values, next) == static_cast<float>(1 + (4 * i + 3 * k) % 29),
                    "incorrect small dataset value");
            ++next;
        }
    }
    require(next == req.nnz && integer(b.offsets, req.offset_dtype, 64) == next,
            "incorrect small dataset final offset");
}
std::vector<std::ptrdiff_t> spread_positions(std::size_t extent) {
    std::vector<std::ptrdiff_t> out;
    constexpr std::size_t count = 64;
    for (std::size_t i = 0; i < count; ++i) {
        const auto distance = extent - 1;
        const auto denominator = count - 1;
        out.push_back(static_cast<std::ptrdiff_t>((distance / denominator) * i +
            ((distance % denominator) * i) / denominator));
    }
    return out;
}
void verify_tenk(const Buffers& b, const ReadRequirements& req,
                 const std::vector<std::ptrdiff_t>& rows,
                 const std::vector<std::ptrdiff_t>& cols) {
    verify_dataset_requirements(req, true);
    std::size_t next = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        require(integer(b.offsets, req.offset_dtype, i) == next,
                "incorrect 10k dataset row offset");
        for (std::size_t k = 0; k < 100; ++k) {
            const auto source_column = 199 * static_cast<std::size_t>(k) +
                                       static_cast<std::size_t>(rows[i]) % 97;
            const auto found = std::lower_bound(cols.begin(), cols.end(),
                static_cast<std::ptrdiff_t>(source_column));
            if (found == cols.end() || *found != static_cast<std::ptrdiff_t>(source_column)) continue;
            require(next < req.nnz, "missing 10k dataset sparse entry");
            const auto output_column = static_cast<std::size_t>(found - cols.begin());
            require(integer(b.indices, req.index_dtype, next) == output_column,
                    "incorrect 10k dataset column index");
            require(cell<float>(b.values, next) ==
                    static_cast<float>(1 + (static_cast<std::size_t>(rows[i]) + k) % 31),
                    "incorrect 10k dataset value");
            ++next;
        }
    }
    require(next == req.nnz && integer(b.offsets, req.offset_dtype, rows.size()) == next,
            "incorrect 10k dataset nnz or final offset");
}
void hash_csr_value(std::uint64_t& hash, float value) {
    hash_little_endian(hash, std::bit_cast<std::uint32_t>(value));
}
std::string fnv_csr(const Buffers& b, const ReadRequirements& req) {
    auto hash = kFnvOffset;
    for (std::size_t i = 0; i < req.nnz; ++i) hash_csr_value(hash, cell<float>(b.values, i));
    for (std::size_t i = 0; i < req.nnz; ++i)
        hash_little_endian(hash, static_cast<std::uint32_t>(integer(b.indices, req.index_dtype, i)));
    for (std::size_t i = 0; i <= req.shape.at(0); ++i)
        hash_little_endian(hash, integer(b.offsets, req.offset_dtype, i));
    return hex64(hash);
}
void print_shape(std::ostream& out, const std::vector<std::size_t>& shape) {
    out << '[';
    for (std::size_t i = 0; i < shape.size(); ++i) out << (i ? "," : "") << shape[i];
    out << ']';
}
void run_dataset(std::string_view name, const char* fixture, Observer& observer,
                 bool tenk, Time start, Time opened, AnnDataSet owner) {
    Observation open_observation = observer.stop_phase();
    observer.start();
    const auto source_shape = std::vector<std::size_t>{owner.n_obs(), owner.n_vars()};
    auto rows = tenk ? spread_positions(10000) : [&] {
        std::vector<std::ptrdiff_t> p;
        for (std::ptrdiff_t i = 0; i < 256; i += 4) p.push_back(i);
        return p;
    }();
    auto cols = tenk ? spread_positions(20000) : rows;
    auto view = owner.slice(indices(rows), indices(cols));
    const auto selected = Clock::now();
    const auto selection_observation = observer.stop_phase();
    observer.start();
    auto prepared = view.prepare_x();
    const auto req = prepared.requirements();
    const auto preparation_done = Clock::now();
    const auto preparation_observation = observer.stop_phase();
    observer.start();
    Buffers buffers(req);
    const auto allocated = Clock::now();
    const auto allocation_observation = observer.stop_phase();
    observer.start();
    prepared.read_into(buffers.dst);
    const auto finished = Clock::now();
    const auto transfer_observation = observer.stop_phase();
    if (tenk) verify_tenk(buffers, req, rows, cols);
    else verify_small(buffers, req);
    Observation total = open_observation;
    add(total, selection_observation);
    add(total, preparation_observation);
    add(total, allocation_observation);
    add(total, transfer_observation);
    std::cout << "{\"case\":\"" << name << "\",\"api\":\"open_h5ads_prepare_x\""
              << ",\"fixture\":\"" << fixture << "\",\"source_shape\":";
    print_shape(std::cout, source_shape);
    std::cout << ",\"shape\":[64,64],\"selector\":\""
              << (tenk ? "64_spread_across_obs=10000_var=20000" : "obs=0:256:4;var=0:256:4")
              << "\",\"nnz\":" << req.nnz
              << ",\"value_dtype\":\"" << dtype_name(req.dtype) << "\",\"value_width_bytes\":" << dtype_size(req.dtype)
              << ",\"actual_index_dtype\":\"" << dtype_name(req.index_dtype) << "\",\"index_width_bytes\":" << dtype_size(req.index_dtype)
              << ",\"actual_offset_dtype\":\"" << dtype_name(req.offset_dtype) << "\",\"offset_width_bytes\":" << dtype_size(req.offset_dtype)
              << ",\"golden\":\"pass\",\"output_fnv1a64\":\"" << fnv_csr(buffers, req) << '\"'
              << ",\"open_ns\":" << ns(start, opened)
              << ",\"selection_ns\":" << ns(opened, selected)
              << ",\"preparation_requirements_ns\":" << ns(selected, preparation_done)
              << ",\"buffer_allocation_ns\":" << ns(preparation_done, allocated)
              << ",\"transfer_ns\":" << ns(allocated, finished)
              << ",\"end_to_end_ns\":" << ns(start, finished);
    if (observer.enabled) {
        print_diagnostic(total, open_observation, selection_observation,
                         preparation_observation, allocation_observation,
                         transfer_observation, true);
    }
    std::cout << "}\n";
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 3 || (argc == 4 && std::string_view(argv[3]) == "--diagnostic"),
                "usage: release11-cpp CASE FIXTURE_PATH [--diagnostic]");
        const std::string_view name(argv[1]);
        const bool diagnostic = argc == 4;
        Observer observer(diagnostic);
        if (name.starts_with("raster-")) {
            const bool raw = name.ends_with("-raw");
            const auto base = raw ? name.substr(0, name.size() - 4) : name;
            (void)raster_case(base);
            observer.start();
            const auto start = Clock::now();
            if (raw) {
                auto store = open_zarr_store(argv[2]);
                const auto opened = Clock::now();
                const auto open_observation = observer.stop_phase();
                observer.start();
                auto source = store.array("/images/image/s0");
                run_raster(name, argv[2], true, observer, start, opened,
                           std::move(source), open_observation);
            } else {
                auto data = open_spatialdata(argv[2]);
                const auto opened = Clock::now();
                const auto open_observation = observer.stop_phase();
                observer.start();
                auto source = data.raster("image");
                run_raster(name, argv[2], false, observer, start, opened,
                           std::move(source), open_observation);
            }
        } else if (name == "dataset-small" || name == "dataset-10k") {
            observer.start();
            const auto start = Clock::now();
            auto owner = open_h5ads(argv[2]);
            const auto opened = Clock::now();
            run_dataset(name, argv[2], observer, name == "dataset-10k", start, opened, std::move(owner));
        } else {
            throw std::runtime_error("unknown case");
        }
    } catch (const std::exception& e) {
        std::cerr << "release11-cpp: " << e.what() << '\n';
        return 1;
    }
}
