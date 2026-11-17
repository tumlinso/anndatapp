// Reuse the qualified probe and its independent exact-output oracles.
#define main qualified_probe_main
#include "../collection-fix/cpp_probe.cpp"
#undef main

int main(int argc, char** argv) {
    try {
        require(argc >= 3 && argc <= 5, "usage: cpp_probe CASE FIXTURE [--diagnostic] [--check-axis-names]");
        bool diagnostic = false, check_names = false;
        for (int i = 3; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--diagnostic" && !diagnostic) diagnostic = true;
            else if (arg == "--check-axis-names" && !check_names) check_names = true;
            else throw std::runtime_error("unknown or duplicate option");
        }
        const std::string_view name(argv[1]);
        require(name == "dataset-small" || name == "dataset-10k", "unknown case");
#if HAS_AXIS_NAME_OPTION
        DatasetOptions options;
        options.check_axis_names = check_names;
#else
        require(!check_names, "baseline always checks axis names");
#endif
        Observer observer(diagnostic);
        observer.start();
        const auto start = Clock::now();
#if HAS_AXIS_NAME_OPTION
        auto owner = open_h5ads(argv[2], options);
#else
        auto owner = open_h5ads(argv[2]);
#endif
        const auto opened = Clock::now();
        run_dataset(name, argv[2], observer, name == "dataset-10k", start, opened, std::move(owner));
    } catch (const std::exception& e) {
        std::cerr << "optional-axis-validation: " << e.what() << '\n';
        return 1;
    }
}
