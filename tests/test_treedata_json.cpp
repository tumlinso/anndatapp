#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <stdexcept>

#include "../src/treedata_json.hpp"

using anndatapp::detail::dump_tree_json;
using anndatapp::detail::parse_tree_json;
using Json = nlohmann::ordered_json;

int main() {
    const auto value = parse_tree_json(
        R"json({"first":-1,"123":0.5,"nested":[NaN,{"positive":Infinity,"negative":-Infinity,"exponent":-2.5e+1}],"unsigned":18446744073709551615,"text":"NaN Infinity -Infinity 1e2","escaped":"quoted: \"NaN\" and slash: \\ Infinity","Infinity":0.25})json");

    assert(value.is_object());
    assert(value.begin().key() == "first");
    assert(value["first"].is_number_integer());
    assert(value["first"].get<int>() == -1);
    assert(value["123"].is_number_float());
    assert(value["123"].get<double>() == 0.5);
    assert(std::isnan(value["nested"][0].get<double>()));
    assert(std::isinf(value["nested"][1]["positive"].get<double>()));
    assert(value["nested"][1]["positive"].get<double>() > 0.0);
    assert(std::isinf(value["nested"][1]["negative"].get<double>()));
    assert(value["nested"][1]["negative"].get<double>() < 0.0);
    assert(value["nested"][1]["exponent"].is_number_float());
    assert(value["nested"][1]["exponent"].get<double>() == -25.0);
    assert(value["unsigned"].is_number_unsigned());
    assert(value["unsigned"].get<std::uint64_t>() == UINT64_MAX);
    assert(value["text"].is_string());
    assert(value["text"].get<std::string>() == "NaN Infinity -Infinity 1e2");
    assert(value["escaped"].get<std::string>() == "quoted: \"NaN\" and slash: \\ Infinity");
    assert(value["Infinity"].is_number_float());
    assert(value["Infinity"].get<double>() == 0.25);

    const auto encoded = dump_tree_json(value);
    assert(encoded.find("\"first\"") < encoded.find("\"nested\""));
    assert(encoded.find("\"nested\"") < encoded.find("\"unsigned\""));
    assert(encoded.find("NaN") != std::string::npos);
    assert(encoded.find("Infinity") != std::string::npos);
    assert(encoded.find("-Infinity") != std::string::npos);
    const auto decoded = parse_tree_json(encoded);
    assert(std::isnan(decoded["nested"][0].get<double>()));
    assert(std::isinf(decoded["nested"][1]["positive"].get<double>()));
    assert(std::isinf(decoded["nested"][1]["negative"].get<double>()));
    assert(decoded["unsigned"].get<std::uint64_t>() == UINT64_MAX);
    assert(decoded["escaped"].get<std::string>() == value["escaped"].get<std::string>());

    assert(dump_tree_json(Json::array()) == "[]");
    assert(dump_tree_json(Json::object()) == "{}");
    assert(parse_tree_json("[]").is_array() && parse_tree_json("[]").empty());
    assert(parse_tree_json("{}").is_object() && parse_tree_json("{}").empty());
    const auto nested_arrays = parse_tree_json("{\"nested\":[[NaN],[]]}");
    assert(std::isnan(nested_arrays["nested"][0][0].get<double>()));
    assert(nested_arrays["nested"][1].is_array() && nested_arrays["nested"][1].empty());
    const auto integer_edges = parse_tree_json(
        "[-9223372036854775808,9223372036854775807,18446744073709551615]");
    assert(integer_edges[0].is_number_integer());
    assert(integer_edges[0].get<std::int64_t>() == INT64_MIN);
    assert(integer_edges[1].is_number_integer());
    assert(integer_edges[1].get<std::int64_t>() == INT64_MAX);
    assert(integer_edges[2].is_number_unsigned());
    assert(integer_edges[2].get<std::uint64_t>() == UINT64_MAX);

    bool range_rejected = false;
    try { (void)parse_tree_json("18446744073709551616"); }
    catch (const std::out_of_range&) { range_rejected = true; }
    assert(range_rejected);
    range_rejected = false;
    try { (void)parse_tree_json("-9223372036854775809"); }
    catch (const std::out_of_range&) { range_rejected = true; }
    assert(range_rejected);

    const auto large_float_numbers = parse_tree_json(
        "[18446744073709551616.0,18446744073709551616e0]");
    const double two_to_64 = std::ldexp(1.0, 64);
    assert(large_float_numbers[0].is_number_float());
    assert(large_float_numbers[1].is_number_float());
    assert(large_float_numbers[0].get<double>() == two_to_64);
    assert(large_float_numbers[1].get<double>() == two_to_64);

    const auto string_integers = parse_tree_json(
        R"json({"too_big":"18446744073709551616","too_small":"-9223372036854775809"})json");
    assert(string_integers["too_big"].get<std::string>() == "18446744073709551616");
    assert(string_integers["too_small"].get<std::string>() == "-9223372036854775809");

    const auto root_array = parse_tree_json("[NaN,Infinity,-Infinity,1,-1e-2]");
    assert(std::isnan(root_array[0].get<double>()));
    assert(std::isinf(root_array[1].get<double>()));
    assert(std::isinf(root_array[2].get<double>()));
    assert(root_array[3].is_number_integer() && root_array[3].get<int>() == 1);
    assert(root_array[4].is_number_float() && root_array[4].get<double>() == -0.01);

    bool rejected = false;
    try { (void)parse_tree_json("{\"value\":NaNfoo}"); }
    catch (const Json::parse_error&) { rejected = true; }
    assert(rejected);

    rejected = false;
    try { (void)parse_tree_json("{\"same\":NaN,\"same\":Infinity,\"next\":-Infinity}"); }
    catch (const Json::parse_error&) { rejected = true; }
    assert(rejected);
}
