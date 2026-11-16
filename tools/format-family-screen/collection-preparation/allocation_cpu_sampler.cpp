#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <csignal>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

import anndatapp.dataset;

using namespace anndatapp;
namespace {
constexpr std::size_t max_iterations=64;
constexpr std::chrono::seconds fifo_timeout{10};

void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }

std::vector<std::ptrdiff_t> positions() {
    std::vector<std::ptrdiff_t> p;
    for(int i=0;i<256;i+=4) p.push_back(i);
    return p;
}

void require_fifo(int fd,const char* name) {
    struct stat s{};
    if(fstat(fd,&s)!=0 || !S_ISFIFO(s.st_mode)) throw std::runtime_error(name);
}

[[noreturn]] void bad_ack(const char* message,const char* bytes,std::size_t size) {
    static constexpr char digits[]="0123456789abcdef";
    std::cerr << "ack-frame length=" << size << " hex=";
    for(std::size_t i=0;i<size;++i) {
        const auto b=static_cast<unsigned char>(bytes[i]);
        std::cerr.put(digits[b>>4]);
        std::cerr.put(digits[b&15]);
    }
    std::cerr << '\n';
    throw std::runtime_error(message);
}

int open_ack(const char* path) {
    int fd=open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    if(fd<0) throw std::runtime_error("cannot open acknowledgement FIFO");
    require_fifo(fd,"acknowledgement path is not a FIFO");
    return fd;
}

int open_control(const char* path) {
    const auto deadline=std::chrono::steady_clock::now()+fifo_timeout;
    while(std::chrono::steady_clock::now()<deadline) {
        int fd=open(path,O_WRONLY|O_NONBLOCK|O_CLOEXEC);
        if(fd>=0) {
            require_fifo(fd,"control path is not a FIFO");
            return fd;
        }
        if(errno!=ENXIO && errno!=EINTR) throw std::runtime_error("cannot open control FIFO");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("timed out opening control FIFO");
}

void wait_ack(int fd) {
    const auto deadline=std::chrono::steady_clock::now()+fifo_timeout;
    std::array<char,64> line{};
    std::size_t used=0;
    while(std::chrono::steady_clock::now()<deadline) {
        pollfd pfd{fd,POLLIN,0};
        const auto remain=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
        const int ready=poll(&pfd,1,static_cast<int>(std::min<std::int64_t>(remain,250)));
        if(ready<0) { if(errno==EINTR) continue; throw std::runtime_error("poll on acknowledgement FIFO failed"); }
        if(ready==0) continue;
        if(pfd.revents&POLLIN) {
            const auto n=read(fd,line.data()+used,line.size()-used);
            if(n<0) { if(errno==EINTR || errno==EAGAIN) continue; throw std::runtime_error("read from acknowledgement FIFO failed"); }
            if(n==0) continue;
            used+=static_cast<std::size_t>(n);
            const auto end=std::find(line.begin(),line.begin()+static_cast<std::ptrdiff_t>(used),'\n');
            if(end!=line.begin()+static_cast<std::ptrdiff_t>(used)) {
                const auto count=static_cast<std::size_t>(end-line.begin());
                if(!(count==3 && std::memcmp(line.data(),"ack",3)==0 && (count+1==used || (count+2==used && line[count+1]==0))))
                    bad_ack("unexpected perf acknowledgement",line.data(),used);
                return;
            }
            if(used==line.size()) bad_ack("oversized perf acknowledgement",line.data(),used);
        } else if(pfd.revents&POLLERR) {
            throw std::runtime_error("acknowledgement FIFO reported an error");
        } else if(pfd.revents&POLLHUP) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    throw std::runtime_error("timed out waiting for perf acknowledgement");
}

void command(int control_fd,int ack_fd,const char* text,std::size_t size) {
    while(true) {
        const auto n=write(control_fd,text,size);
        if(n==static_cast<ssize_t>(size)) break;
        if(n<0 && errno==EINTR) continue;
        throw std::runtime_error("write to perf control FIFO failed");
    }
    wait_ack(ack_fd);
}

std::size_t parse_iterations(int argc,char** argv) {
    if(argc==4) return max_iterations;
    std::size_t n=0;
    const std::string_view s(argv[4]);
    const auto [end,error]=std::from_chars(s.data(),s.data()+s.size(),n);
    require(error==std::errc{} && end==s.data()+s.size() && n>0 && n<=max_iterations,
            "iterations must be between 1 and 64");
    return n;
}

void verify_requirements(const ReadRequirements& r) {
    require(r.layout==Layout::csr && r.dtype==DType::float32,"expected float32 CSR");
    require(r.shape==std::vector<std::size_t>{64,64} && r.nnz==512,"incorrect shape or nnz");
}

std::size_t words(std::size_t n,std::size_t width) { return (n*width+7)/8; }
struct Buffers {
    std::vector<std::uint64_t> values,indices,offsets;
    SparseBuffer dst;
    explicit Buffers(const ReadRequirements& r)
        : values(words(r.nnz,dtype_size(r.dtype))),
          indices(words(r.nnz,dtype_size(r.index_dtype))),
          offsets(words(r.shape.at(0)+1,dtype_size(r.offset_dtype))),
          dst{values.data(),indices.data(),offsets.data(),r.dtype,r.index_dtype,r.offset_dtype,
              r.nnz,r.nnz,r.shape.at(0)+1} {}
};

template<class T> T cell(const std::vector<std::uint64_t>& b,std::size_t i) {
    T x{};
    std::memcpy(&x,reinterpret_cast<const std::byte*>(b.data())+i*sizeof(T),sizeof(T));
    return x;
}

std::uint64_t integer(const std::vector<std::uint64_t>& b,DType type,std::size_t i) {
    switch(type) {
    case DType::int32: { const auto n=cell<std::int32_t>(b,i); require(n>=0,"negative sparse index"); return n; }
    case DType::uint32: return cell<std::uint32_t>(b,i);
    case DType::int64: { const auto n=cell<std::int64_t>(b,i); require(n>=0,"negative sparse index"); return n; }
    case DType::uint64: return cell<std::uint64_t>(b,i);
    default: throw std::runtime_error("unexpected sparse integer dtype");
    }
}

void verify(const Buffers& b,const ReadRequirements& r) {
    verify_requirements(r);
    for(std::size_t i=0;i<64;++i) {
        require(integer(b.offsets,r.offset_dtype,i)==8*i,"incorrect row offset");
        for(std::size_t k=0;k<8;++k) {
            const auto j=8*i+k;
            require(integer(b.indices,r.index_dtype,j)==8*k,"incorrect column index");
            require(cell<float>(b.values,j)==static_cast<float>(1+(4*i+3*k)%29),"incorrect value");
        }
    }
    require(integer(b.offsets,r.offset_dtype,64)==512,"incorrect final offset");
}
}

int main(int argc,char** argv) {
    try {
        require(argc==4 || argc==5,
                "usage: collection-allocation-cpu-sampler dataset.h5ads control.fifo ack.fifo [iterations<=64]");
        std::signal(SIGPIPE,SIG_IGN);
        const auto iterations=parse_iterations(argc,argv);
        auto data=open_h5ads(argv[1]);
        auto p=positions();
        auto view=data.slice(indices(p),indices(p));
        const int ack=open_ack(argv[3]);
        const int ctl=open_control(argv[2]);
        std::size_t requirements_passed=0;
        for(std::size_t i=0;i<iterations;++i) {
            command(ctl,ack,"enable\n",7);
            {
                auto prepared=view.prepare_x();
                const auto req=prepared.requirements();
                command(ctl,ack,"disable\n",8);
                verify_requirements(req);
                ++requirements_passed;
            }
        }
        auto prepared=view.prepare_x();
        const auto req=prepared.requirements();
        verify_requirements(req);
        Buffers b(req);
        prepared.read_into(b.dst);
        verify(b,req);
        std::cout << "{\"iterations\":" << iterations
                  << ",\"requirements_passed\":" << requirements_passed
                  << ",\"golden_pass\":true,\"shape\":[64,64],\"nnz\":512}\n";
        close(ctl);
        close(ack);
        return 0;
    } catch(const std::exception& e) {
        std::cerr << "collection-allocation-cpu-sampler: " << e.what() << '\n';
        return 1;
    }
}
