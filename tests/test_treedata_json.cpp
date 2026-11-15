#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <stdexcept>

#include "../src/treedata_json.hpp"

using anndatapp::detail::dump_tree_json;
using anndatapp::detail::parse_tree_json;
using anndatapp::detail::scan_tree_json_members;
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

    assert(scan_tree_json_members("\xef\xbb\xbf{}").empty());
    const std::string source = R"json( {"first": {"ignored":[true,null,"text"]}, "\u0061": [NaN,Infinity,-Infinity], "last": 1, "a": {"updated":2}}  )json";
    const auto members = scan_tree_json_members(source);
    assert(members.size() == 3);
    assert(members[0].name == "first" && members[1].name == "a" && members[2].name == "last");
    assert(source.substr(members[0].begin, members[0].end - members[0].begin) == R"({"ignored":[true,null,"text"]})");
    assert(source.substr(members[1].begin, members[1].end - members[1].begin) == R"({"updated":2})");
    assert(parse_tree_json(std::string_view(source).substr(members[1].begin,
                           members[1].end - members[1].begin))["updated"] == 2);
    const std::string special = R"({"tree":[NaN,Infinity,-Infinity]})";
    const auto span = scan_tree_json_members(special).front();
    const auto selected = parse_tree_json(std::string_view(special).substr(span.begin, span.end - span.begin));
    assert(std::isnan(selected[0].get<double>()));
    assert(std::isinf(selected[1].get<double>()) && selected[1].get<double>() > 0);
    assert(std::isinf(selected[2].get<double>()) && selected[2].get<double>() < 0);
    assert(scan_tree_json_members(R"({"\ud83d\ude00":"\u0000"})")[0].name == "\xf0\x9f\x98\x80");
    for (const auto* bad : {"[]", "{}x", "{\"x\":[1,]}", "{\"x\":{\"y\":}}",
                            "{\"x\":01}", "{\"x\":NaNfoo}", "{\"x\":\"\\ud800\"}",
                            "{\"x\":\"\\q\"}", "{\"x\":1,}", "{\"x\":1e9999}"}) {
        bool failed = false;
        try { (void)scan_tree_json_members(bad); }
        catch (const Json::exception&) { failed = true; }
        assert(failed);
    }
    for (const auto* bad : {"{\"ignored\":18446744073709551616}",
                            "{\"ignored\":[-9223372036854775809]}"}) {
        bool failed = false;
        try { (void)scan_tree_json_members(bad); }
        catch (const std::out_of_range&) { failed = true; }
        assert(failed);
    }

}
