#include <chrono>
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import anndatapp;

namespace {
using namespace anndatapp;
using Clock = std::chrono::steady_clock;
using Begin = void (*)();
using End = void (*)(std::uint64_t*);
constexpr std::size_t rows = 4096, cols = 1024;
constexpr std::size_t block_rows = 128, block_cols = 256, rectangles = 16;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
float expected(std::size_t r, std::size_t c) {
    return static_cast<float>(static_cast<int>((17*r + 13*c + (r*c)%97)%1009) - 504) / 16.0f;
}
template<class T> T encode(float x) { return x; }
template<> Float16 encode(float x) { return Float16::from_float(x); }
float decode(float x) { return x; }
float decode(Float16 x) { return x.to_float(); }

template<class T> void verify(const T* array, std::size_t m, std::size_t n,
                              std::size_t r = 0, std::size_t c = 0) {
    for (std::size_t i = 0; i < m; ++i) for (std::size_t j = 0; j < n; ++j)
        check(decode(array[i*n+j]) == expected(r+i,c+j), "dense output disagrees with formula");
}
template<class T> void verify_matrix(const AnnData& array) {
    const auto x = array.x();
    check(x.layout() == Layout::dense && x.dtype() == dtype_of<T>() &&
          x.extent(0) == rows && x.extent(1) == cols &&
          x.bytes().element_size() == sizeof(T), "unexpected matrix geometry");
    verify(x.dense<T>().data(), rows, cols);
}
template<class T> Array matrix() {
    auto array = Array::empty<T>({rows,cols});
    auto* x = array.edit().template dense<T>().data();
    for (std::size_t r = 0; r < rows; ++r) for (std::size_t c = 0; c < cols; ++c)
        x[r*cols+c] = encode<T>(expected(r,c));
    return array;
}

struct Measurement {
    bool diagnostic;
    Begin begin_alloc{}, begin_io{};
    End end_alloc{}, end_io{};
    std::uint64_t alloc[2]{}, io[16]{};
    TransferStats transfer{};
    double setup = 0, runtime = 0;
    std::size_t output_bytes = 0;

    explicit Measurement(bool observe) : diagnostic(observe) {
        if (!observe) return;
        begin_alloc = reinterpret_cast<Begin>(dlsym(RTLD_DEFAULT,"v1_alloc_begin"));
        end_alloc = reinterpret_cast<End>(dlsym(RTLD_DEFAULT,"v1_alloc_end"));
        begin_io = reinterpret_cast<Begin>(dlsym(RTLD_DEFAULT,"v1_io_begin"));
        end_io = reinterpret_cast<End>(dlsym(RTLD_DEFAULT,"v1_io_end"));
        check(begin_alloc && end_alloc && begin_io && end_io, "missing diagnostic observers");
    }
    template<class F> void measure(F operation) {
        if (diagnostic) { begin_io(); begin_alloc(); }
        auto start = Clock::now();
        operation();
        runtime = elapsed(start);
        if (diagnostic) { end_alloc(alloc); end_io(io); }
    }
    void report() const {
        std::cout << std::setprecision(12) << "{\"setup_s\":" << setup
                  << ",\"runtime_s\":" << (diagnostic ? 0 : runtime)
                  << ",\"payload_bytes\":" << output_bytes;
        if (diagnostic) {
            std::cout << ",\"new_calls\":" << alloc[0] << ",\"requested_bytes\":" << alloc[1]
                      << ",\"libc_read_calls\":" << io[0] << ",\"libc_read_bytes\":" << io[1]
                      << ",\"libc_write_calls\":" << io[2] << ",\"libc_write_bytes\":" << io[3]
                      << ",\"hdf5_read_calls\":" << io[14] << ",\"hdf5_write_calls\":" << io[15]
                      << ",\"transfer_read_calls\":" << transfer.read_calls
                      << ",\"transfer_read_bytes\":" << transfer.read_bytes
                      << ",\"transfer_write_calls\":" << transfer.write_calls
                      << ",\"transfer_write_bytes\":" << transfer.write_bytes
                      << ",\"transfer_scratch_events\":" << transfer.scratch_allocations
                      << ",\"transfer_retained_scratch_bytes\":" << transfer.scratch_bytes;
        }
        std::cout << "}\n";
    }
};

template<class T> void run(std::string_view op, std::string_view format, const std::string& path,
                           unsigned passes, bool negative, Measurement& m) {
    const bool hdf5 = format == "h5-contiguous" || format == "h5-gzip";
    auto read = [&] { return hdf5 ? read_h5ad(path) : read_zarr(path); };
    const auto setup_start = Clock::now();
    m.output_bytes = rows*cols*sizeof(T)*passes;
    if (op == "copy") {
        const auto source = matrix<T>();
        auto dest = Array::empty<T>({rows,cols});
        const auto from = source.view();
        const auto to = dest.edit();
        m.setup = elapsed(setup_start);
        m.measure([&] { for (unsigned i = 0; i < passes; ++i) from.copy_into(to); });
        verify(dest.view().template dense<T>().data(), rows, cols);
    } else if (op == "eager-read") {
        AnnData observed;
        m.setup = elapsed(setup_start);
        m.measure([&] { for (unsigned i = 0; i < passes; ++i) observed = read(); });
        verify_matrix<T>(observed);
    } else if (op == "eager-write") {
        auto data = AnnData::empty(rows,cols);
        data.set_x(matrix<T>());
        m.setup = elapsed(setup_start);
        m.measure([&] {
            for (unsigned i = 0; i < passes; ++i) {
                if (hdf5) write_h5ad(path,data,{.compression = format=="h5-gzip" ? Compression::gzip : Compression::none,
                                               .gzip_level = 1});
                else write_zarr(path,data,{.format = format=="zarr2" ? ZarrFormat::v2 : ZarrFormat::v3});
            }
        });
    } else if (op == "prepared-read" || op == "prepared-write") {
        const bool writing = op == "prepared-write";
        const BackedOptions options{.mode = writing ? AccessMode::read_write : AccessMode::read_only};
        auto file = hdf5 ? open_h5ad(path,options) : open_zarr(path,options);
        auto context = file.x().prepare_transfer();
        constexpr std::size_t elements = block_rows*block_cols;
        const auto blocks = writing ? std::size_t{4} : rectangles;
        std::vector<T> storage(blocks*elements);
        if (writing) {
            const float sign = negative ? -1.0f : 1.0f;
            for (std::size_t k = 0; k < blocks; ++k)
                for (std::size_t i = 0; i < block_rows; ++i)
                    for (std::size_t j = 0; j < block_cols; ++j)
                        storage[k*elements+i*block_cols+j] =
                            encode<T>(sign*expected(4+k*251+i,9+(k%4)*245+j));
        }
        const std::vector<std::size_t> count{block_rows,block_cols};
        std::vector<std::size_t> start{0,0};
        auto buffer = DenseBuffer::from(storage.data(),count);
        const auto geometry = context.geometry();
        check(geometry.layout == Layout::dense && geometry.dtype == dtype_of<T>() &&
              dtype_size(geometry.dtype) == sizeof(T) &&
              geometry.shape == std::vector<std::size_t>{rows,cols}, "unexpected prepared geometry");
        auto transfer_pass = [&] {
            for (std::size_t k = 0; k < blocks; ++k) {
                start[0] = 4 + k*251;
                start[1] = 9 + (k%4)*245;
                buffer.data = storage.data()+k*elements;
                if (writing) context.write_from(start,count,buffer);
                else context.read_into(start,count,buffer);
            }
        };
        m.output_bytes = passes*blocks*elements*sizeof(T);
        m.setup = elapsed(setup_start);
        m.measure([&] { for (unsigned i = 0; i < passes; ++i) transfer_pass(); });
        if (m.diagnostic) m.transfer = context.stats();
        if (!writing) for (std::size_t k = 0; k < blocks; ++k)
            verify(storage.data()+k*elements, block_rows, block_cols, 4+k*251, 9+(k%4)*245);
    } else {
        throw std::runtime_error("unknown benchmark operation");
    }
}
} // namespace

int main(int argc, char** argv) try {
    check(argc == 8, "usage: fp16_benchmark OP FORMAT PATH 16|32 PASSES time|diag +|-");
    const std::string_view mode(argv[6]), precision(argv[4]);
    const auto n = std::stoul(argv[5]);
    check(n > 0 && n <= 100 && (mode == "time" || mode == "diag"), "invalid run options");
    Measurement measurement(mode=="diag");
    const bool negative = std::string_view(argv[7]) == "-";
    if (precision == "16") run<Float16>(argv[1],argv[2],argv[3],n,negative,measurement);
    else if (precision == "32") run<float>(argv[1],argv[2],argv[3],n,negative,measurement);
    else throw std::runtime_error("unknown precision");
    measurement.report();
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
