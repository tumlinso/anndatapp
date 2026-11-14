extern "C" int spatial_parquet_test(const char*, const char*, int, const char* const*);
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3 && argc != 7) return 2;
    return spatial_parquet_test(argv[1], argc >= 3 ? argv[2] : "", argc == 7 ? 4 : 0,
                                argc == 7 ? argv + 3 : nullptr);
}
