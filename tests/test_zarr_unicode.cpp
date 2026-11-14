#include "../src/zarr_engine.hpp"
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <cstdlib>
#include <zlib.h>

using namespace anndatapp_zarr;

static void require(bool condition) { if (!condition) throw std::runtime_error("Unicode decoder regression"); }

static Bytes units(std::initializer_list<std::uint32_t> values, bool little) {
    Bytes bytes;
    for(auto value:values) for(unsigned i=0;i<4;++i)
        bytes.push_back(std::byte((value >> (8*(little?i:3-i))) & 0xff));
    return bytes;
}
static void check(bool little) {
    ArrayMetadata m; m.format=2; m.type=Type::string; m.unicode_width=4;
    m.item_size=16; m.endian=little?'<':'>'; m.codecs=Json::array();
    auto bytes=units({'A',0,0x1f600,0, 0x3bb,0,0,0},little);
    auto result=decode_payload({m,m.codecs,65536},{2},bytes);
    require(result.strings.size()==2);
    require(result.strings[0]==std::string("A\0\xf0\x9f\x98\x80",6));
    require(result.strings[1]=="\xce\xbb");
    for(auto invalid:{std::uint32_t(0xd800),std::uint32_t(0x110000)}) {
        bool threw=false;
        try { (void)decode_payload({m,m.codecs,65536},{1},units({invalid,0,0,0},little)); }
        catch(const Error& e) { threw=e.failure==Failure::encoding; }
        require(threw);
    }
    bool threw=false;
    try { (void)decode_payload({m,m.codecs,65536},{1},units({'A'},little)); }
    catch(const Error& e) { threw=e.failure==Failure::encoding; }
    require(threw);
}
static void check_store(bool little) {
    char directory[]="/tmp/anndatapp-unicode-XXXXXX";
    const auto* created=::mkdtemp(directory); require(created);
    auto root=std::filesystem::path(created);
    std::filesystem::create_directory(root/"values");
    std::ofstream(root/".zgroup") << R"({"zarr_format":2})";
    std::ofstream meta(root/"values"/".zarray");
    meta << "{\"zarr_format\":2,\"shape\":[2],\"chunks\":[2],\"dtype\":\""
         << (little?"<U4":">U4")
         << "\",\"compressor\":" << (little ? "{\"id\":\"zlib\",\"level\":1}" : "null")
         << ",\"fill_value\":\"\",\"filters\":null,\"order\":\"C\"}";
    meta.close();
    auto bytes=units({'A',0,0x1f600,0,0x3bb,0,0,0},little);
    if(little) {
        uLongf length=compressBound(bytes.size()); Bytes compressed(length);
        const auto status=compress2(reinterpret_cast<Bytef*>(compressed.data()),&length,
            reinterpret_cast<const Bytef*>(bytes.data()),bytes.size(),1);
        require(status==Z_OK); compressed.resize(length); bytes=std::move(compressed);
    }
    std::ofstream out(root/"values"/"0",std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())); out.close();
    Store store(root.string(),false,65536);
    auto values=store.read_strings("values",{0},{2});
    require(values[0]==std::string("A\0\xf0\x9f\x98\x80",6));
    require(values[1]=="\xce\xbb");
    std::filesystem::remove_all(root);
}
int main(int argc,char** argv) {
    check(true); check(false); check_store(true); check_store(false);
    if(argc>1) {
        Store store(argv[1],false,65536);
        auto node=store.inspect("uns/project");
        require(node.exists && !node.group && node.array.unicode_width==7);
        auto result=store.read_strings("uns/project",{},{});
        require(result.size()==1 && result[0]=="fixture");
        std::cout << "fixture project=" << result[0] << '\n';
    }
    std::cout << "unicode decoder checks passed\n";
}
