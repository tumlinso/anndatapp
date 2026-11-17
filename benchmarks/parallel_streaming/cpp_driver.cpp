#include <algorithm>
#include <array>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

import anndatapp;

namespace {
using namespace anndatapp;
using Clock = std::chrono::steady_clock;
using I32 = std::int32_t;
constexpr std::size_t rows = 4096, cols = 1024;
constexpr std::size_t zr = 256, zc = 512, hr = 64, hnnz = 32;
using TransferCounts = std::array<std::size_t, 4>;

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct Reader {
    const bool zarr;
    const std::size_t first, stride;
    BackedAnnData store;
    TransferContext context;
    std::vector<float> dense;
    std::vector<I32> values, indices, offsets;
    const std::vector<std::size_t> count;
    std::vector<std::size_t> start{0, 0};
    DenseBuffer db;
    SparseBuffer sb;

    Reader(bool z, const std::string& path, std::size_t worker, std::size_t workers)
        : zarr(z), first(worker), stride(workers),
          store(z ? open_zarr(path) : open_h5ad(path)),
          context(store.x().prepare_transfer()), dense(z ? zr*zc : 0),
          values(z ? 0 : hr*hnnz), indices(values.size()), offsets(z ? 0 : hr+1),
          count(z ? std::vector<std::size_t>{zr, zc} : std::vector<std::size_t>{hr, cols}),
          db(DenseBuffer::from(dense.data(), count)),
          sb{values.data(), indices.data(), offsets.data(), DType::int32, DType::int32,
             DType::int32, values.size(), indices.size(), offsets.size()} {
        const auto geometry = context.geometry();
        const std::vector<std::size_t> shape{rows, cols};
        check(geometry.shape == shape, "unexpected fixture shape");
        if (z) {
            check(geometry.layout == Layout::dense && geometry.dtype == DType::float32 &&
                  geometry.data.organization == StorageOrganization::sharded &&
                  geometry.data.write_unit == count, "Zarr fixture is not the expected float32 shard grid");
        } else {
            check(geometry.layout == Layout::csr && geometry.dtype == DType::int32,
                  "H5AD fixture is not the expected int32 CSR");
        }
    }

    void verify() const {
        if (zarr) {
            for (std::size_t i = 0; i < zr; ++i) for (std::size_t j = 0; j < zc; ++j) {
                const auto r = start[0] + i, c = start[1] + j;
                const auto x = float((17*r + 13*c + r*c%97)%1009)/1009.0f;
                check(dense[i*zc+j] == x, "dense value differs from fixture formula");
            }
        } else {
            std::array<I32, hnnz> expected{};
            for (std::size_t i = 0; i < hr; ++i) {
                const auto r = start[0] + i;
                check(offsets[i] == I32(i*hnnz), "unexpected block offset");
                for (std::size_t j = 0; j < hnnz; ++j) expected[j] = I32((37*r + 127*j)%cols);
                std::sort(expected.begin(), expected.end());
                for (std::size_t j = 0; j < hnnz; ++j) {
                    const auto x = I32(1 + (11*r + 7*expected[j])%11);
                    check(indices[i*hnnz+j] == expected[j] && values[i*hnnz+j] == x,
                          "sparse index or value differs from fixture formula");
                }
            }
            check(offsets[hr] == I32(hr*hnnz), "unexpected final block offset");
        }
    }

    void pass(bool verify_output) {
        const auto blocks = zarr ? rows/zr*(cols/zc) : rows/hr;
        for (std::size_t k = first; k < blocks; k += stride) {
            if (zarr) {
                start[0] = k/(cols/zc)*zr; start[1] = k%(cols/zc)*zc;
                context.read_into(start, count, db);
            } else {
                start[0] = k*hr;
                context.read_into(start, count, sb);
            }
            if (verify_output) verify();
        }
    }

    TransferCounts counts() const {
        auto s = context.stats();
        return {s.read_calls, s.read_bytes, s.scratch_allocations, s.scratch_bytes};
    }
};

struct ThreadReaders {
    std::barrier<> begin, end;
    std::vector<std::unique_ptr<Reader>> readers;
    std::vector<std::thread> threads;
    std::array<std::exception_ptr, 4> errors{};
    char command = 0;

    ThreadReaders(const std::string& path, std::size_t n) : begin(n+1), end(n+1) {
        for (std::size_t i = 0; i < n; ++i) readers.push_back(std::make_unique<Reader>(true, path, i, n));
        threads.reserve(n);
        try {
            for (std::size_t i = 0; i < n; ++i) threads.emplace_back([this, i] {
                for (;;) {
                    begin.arrive_and_wait();
                    if (command == 'q') break;
                    try { readers[i]->pass(command == 'v'); }
                    catch (...) { errors[i] = std::current_exception(); }
                    end.arrive_and_wait();
                }
            });
        } catch (...) {
            for (auto i = threads.size(); i < n; ++i) begin.arrive_and_drop();
            stop();
            throw;
        }
    }
    ~ThreadReaders() { stop(); }
    void stop() {
        command = 'q'; begin.arrive_and_wait();
        for (auto& thread : threads) thread.join();
    }
    void pass(char action) {
        command = action; begin.arrive_and_wait(); end.arrive_and_wait();
        for (const auto& error : errors) if (error) std::rethrow_exception(error);
    }
    TransferCounts counts() const {
        TransferCounts sum{};
        for (const auto& reader : readers) {
            auto c = reader->counts();
            for (std::size_t i = 0; i < sum.size(); ++i) sum[i] += c[i];
        }
        return sum;
    }
};

char receive(int fd) {
    char byte = 0;
    ssize_t count;
    do { count = ::read(fd, &byte, 1); } while (count < 0 && errno == EINTR);
    check(count >= 0, "worker pipe read failed");
    return count == 1 ? byte : 'q';
}

void send(int fd, char byte) {
    ssize_t count;
    do { count = ::write(fd, &byte, 1); } while (count < 0 && errno == EINTR);
    check(count == 1, "worker pipe write failed");
}

void counts_io(int fd, TransferCounts& counts, bool writing) {
    ssize_t n;
    do {
        n = writing ? ::write(fd, counts.data(), sizeof(counts)) : ::read(fd, counts.data(), sizeof(counts));
    } while (n < 0 && errno == EINTR);
    check(n == sizeof(counts), "worker counter handshake failed");
}

struct ProcessReaders {
    struct Child { pid_t pid; int in, out; };
    std::vector<Child> children;

    ProcessReaders(const std::string& path, std::size_t n) {
        children.reserve(n);
        try {
            for (std::size_t w = 0; w < n; ++w) {
                int request[2], reply[2];
                check(::pipe(request) == 0, "cannot create worker request pipe");
                if (::pipe(reply) != 0) {
                    ::close(request[0]); ::close(request[1]);
                    throw std::runtime_error("cannot create worker reply pipe");
                }
                const auto pid = ::fork();
                if (pid == 0) {
                    ::close(request[1]); ::close(reply[0]);
                    for (const auto& worker : children) { ::close(worker.in); ::close(worker.out); }
                    try {
                        Reader worker(false, path, w, n);
                        send(reply[1], 'o');
                        while (const auto command = receive(request[0])) {
                            if (command == 'q') break;
                            if (command == 'c') {
                                auto counts = worker.counts();
                                counts_io(reply[1], counts, true);
                                continue;
                            }
                            worker.pass(command == 'v');
                            send(reply[1], 'o');
                        }
                    } catch (const std::exception& error) {
                        std::cerr << "HDF5 reader worker: " << error.what() << '\n';
                        ::_exit(1);
                    }
                    ::_exit(0);
                }
                ::close(request[0]); ::close(reply[1]);
                if (pid < 0) {
                    ::close(request[1]); ::close(reply[0]);
                    throw std::runtime_error("cannot fork independent HDF5 reader");
                }
                children.push_back({pid, request[1], reply[0]});
            }
            wait_all();
        } catch (...) {
            stop();
            throw;
        }
    }
    ~ProcessReaders() { stop(); }
    void stop() {
        for (const auto& worker : children) { ::close(worker.in); ::close(worker.out); }
        for (const auto& worker : children)
            while (::waitpid(worker.pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
    void wait_all() {
        for (const auto& worker : children) check(receive(worker.out) == 'o', "independent HDF5 reader failed");
    }
    void pass(char action) {
        for (const auto& worker : children) send(worker.in, action);
        wait_all();
    }
    TransferCounts counts() const {
        TransferCounts sum{};
        for (const auto& worker : children) send(worker.in, 'c');
        for (const auto& worker : children) {
            TransferCounts c{};
            counts_io(worker.out, c, false);
            for (std::size_t i = 0; i < sum.size(); ++i) sum[i] += c[i];
        }
        return sum;
    }
};

auto nanos(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-t).count();
}

template<class Readers>
void run(bool zarr, const std::string& path, std::size_t workers) {
    const auto t = Clock::now();
    Readers readers(path, workers);
    const auto setup = nanos(t);
    const auto tv = Clock::now();
    readers.pass('v');
    const auto verified = nanos(tv);
    const auto counts = readers.counts();
    const auto blocks = zarr ? rows/zr*(cols/zc) : rows/hr;
    const auto bytes = zarr ? rows*cols*sizeof(float) : blocks*(2*hr*hnnz + hr + 1)*sizeof(I32);
    std::cout << "{\"event\":\"ready\",\"backend\":\"" << (zarr ? "zarr" : "h5ad")
              << "\",\"workers\":" << workers << ",\"blocks\":" << blocks
              << ",\"bytes_per_pass\":" << bytes << ",\"setup_ns\":" << setup
              << ",\"verification_ns\":" << verified
              << ",\"worker_model\":\"" << (zarr ? "threads" : "forked processes") << "\""
              << ",\"buffer_bytes_per_worker\":" << bytes/blocks
              << ",\"verification_backend_reads\":" << counts[0]
              << ",\"verification_backend_bytes\":" << counts[1]
              << ",\"verification_cpp_scratch_allocations\":" << counts[2]
              << ",\"verification_retained_scratch_bytes\":" << counts[3] << "}\n" << std::flush;
    std::string command;
    while (std::getline(std::cin, command) && command != "quit") {
        check(command == "read", "unexpected driver command");
        const auto start = Clock::now();
        readers.pass('r');
        const auto elapsed = nanos(start);
        std::cout << "{\"event\":\"sample\",\"elapsed_ns\":" << elapsed << "}\n" << std::flush;
    }
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 4, "usage: parallel_streaming <zarr|h5ad> <path> <1|4>");
        const std::string backend = argv[1], workers = argv[3];
        check(backend == "zarr" || backend == "h5ad", "invalid backend");
        check(workers == "1" || workers == "4", "expected one or four workers");
        const auto n = std::stoul(workers);
        if (backend == "zarr") run<ThreadReaders>(true, argv[2], n);
        else {
            std::signal(SIGPIPE, SIG_IGN);
            run<ProcessReaders>(false, argv[2], n);
        }
    } catch (const std::exception& error) {
        std::cerr << "parallel streaming: " << error.what() << '\n';
        return 1;
    }
}
