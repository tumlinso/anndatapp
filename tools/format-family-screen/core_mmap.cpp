#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <openssl/evp.h>
#include <stdexcept>
import anndatapp;
using Clock = std::chrono::steady_clock;
static double elapsed(Clock::time_point t) { return std::chrono::duration<double>(Clock::now()-t).count(); }
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: core_mmap CONTIGUOUS_H5AD\n"; return 2; }
    try {
        auto start=Clock::now();
        auto store=anndatapp::open_h5ad(argv[1]);
        const double open=elapsed(start);
        start=Clock::now();
        auto array=store.x();
        auto mapping=array.map_readonly();
        const double map=elapsed(start);
        // Touch every byte; include the page faults, with verification outside this interval.
        start=Clock::now();
        volatile unsigned long long sum=0;
        auto bytes=reinterpret_cast<const unsigned char*>(mapping.data());
        for(std::size_t i=0;i<mapping.byte_size();++i) sum=sum+bytes[i];
        const double touch=elapsed(start);
        unsigned char digest[EVP_MAX_MD_SIZE]; unsigned size=0;
        auto* ctx=EVP_MD_CTX_new();
        if(!ctx || EVP_DigestInit_ex(ctx,EVP_sha256(),nullptr)!=1 || EVP_DigestUpdate(ctx,bytes,mapping.byte_size())!=1 || EVP_DigestFinal_ex(ctx,digest,&size)!=1) throw std::runtime_error("sha256 failed");
        EVP_MD_CTX_free(ctx);
        constexpr char hex[]="0123456789abcdef"; std::string hash(2*size,'0');
        for(unsigned i=0;i<size;++i) {hash[2*i]=hex[digest[i]>>4];hash[2*i+1]=hex[digest[i]&15];}
        std::cout<<std::setprecision(12)<<"{\"open_seconds\":"<<open<<",\"map_seconds\":"<<map<<",\"touch_seconds\":"<<touch<<",\"end_to_end_seconds\":"<<open+map+touch<<",\"bytes\":"<<mapping.byte_size()<<",\"sum\":"<<sum<<",\"sha256\":\""<<hash<<"\"}\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
