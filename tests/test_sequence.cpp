#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string_view>
#include <type_traits>

import anndatapp;

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template<class F>
void expect_error(anndatapp::ErrorCode code, std::string_view path,
                  F&& operation, const char* message) {
    try {
        operation();
        expect(false, message);
    } catch (const anndatapp::Error& e) {
        expect(e.code() == code && e.path() == path, message);
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << message << " (got different exception: "
                  << e.what() << ")\n";
        ++failures;
    } catch (...) {
        expect(false, message);
    }
}

void heterogeneous_sequence() {
    using anndatapp::Element;
    using anndatapp::ElementKind;

    static_assert(!std::is_copy_constructible_v<Element>);
    static_assert(std::is_move_constructible_v<Element>);
    static_assert(static_cast<int>(ElementKind::record_array) == 11);
    static_assert(static_cast<int>(ElementKind::sequence) == 12);

    std::array<std::int32_t, 2> borrowed{41, 42};
    auto child = Element::sequence();
    child.append(Element(anndatapp::Array::borrow_dense(borrowed.data(), {2}, {1})));
    child.append(Element::scalar<std::uint16_t>(512));

    auto mapping = Element::mapping();
    mapping.set("explicit-null", Element{});
    mapping.set("nested", std::move(child));
    auto table = anndatapp::Table::with_index({"r0", "r1"}, "row");
    table.set("label", anndatapp::Column::strings({"first", "second"}));

    auto sequence = Element::sequence();
    sequence.append(Element{});
    sequence.append(Element("text"));
    sequence.append(Element(anndatapp::Column::strings({"x", "y"})));
    sequence.append(Element(std::move(table)));
    sequence.append(std::move(mapping));

    auto view = sequence.view();
    expect(view.kind() == ElementKind::sequence && view.size() == 5,
           "sequence view reports kind and current length");
    expect(view.at(0).kind() == ElementKind::null &&
           view.at(1).string() == "text" &&
           view.at(2).column().string(1) == "y" &&
           view.at(3).table().column("label").string(0) == "first" &&
           view.at(4).at("nested").at(1).scalar<std::uint16_t>() == 512,
           "sequence indexes heterogeneous and recursively nested values");
    expect(view.at(4).contains("explicit-null") &&
           view.at(4).at("explicit-null").kind() == ElementKind::null &&
           !view.at(4).contains("absent"),
           "mapping membership continues to distinguish absent keys from explicit null values");
    sequence.validate();

    auto moved = std::move(sequence);
    expect(moved.view().size() == 5 && moved.at(4).at("nested").size() == 2,
           "move-only sequence owner preserves nested values");

    const auto original_array = moved.at(4).at("nested").at(0).array();
    auto copied = moved.copy();
    const auto copied_array = copied.at(4).at("nested").at(0).array();
    expect(original_array.dense<std::int32_t>().data() == borrowed.data() &&
           copied_array.dense<std::int32_t>().data() != borrowed.data() &&
           copied_array.dense<std::int32_t>().data()[1] == 42,
           "deep sequence copy owns independent recursively nested array storage");
    borrowed[1] = 99;
    expect(original_array.dense<std::int32_t>().data()[1] == 99 &&
           copied_array.dense<std::int32_t>().data()[1] == 42,
           "deep sequence copy does not alias borrowed source data");
    copied.validate();
}

void retained_views_and_bounds() {
    using anndatapp::Element;
    using anndatapp::ElementView;

    ElementView retained;
    ElementView retained_child;
    {
        auto owner = Element::sequence();
        owner.append(Element("retained"));
        owner.append(Element::sequence());
        retained = owner.view();
        retained_child = owner.at(0);
    }
    expect(retained.size() == 2 && retained.at(0).string() == "retained" &&
           retained_child.string() == "retained",
           "sequence and indexed views retain their values after owner destruction");

    expect_error(anndatapp::ErrorCode::out_of_bounds, "element/sequence", [&] {
        (void)retained.at(retained.size());
    }, "sequence view rejects an index equal to its length");
    expect_error(anndatapp::ErrorCode::out_of_bounds, "element/sequence", [&] {
        auto owner = Element::sequence();
        (void)owner.at(0);
    }, "sequence owner rejects an index into an empty sequence");
    expect_error(anndatapp::ErrorCode::invalid_argument, "element", [&] {
        (void)retained_child.size();
    }, "size is restricted to sequence views");
    expect_error(anndatapp::ErrorCode::invalid_argument, "element", [&] {
        Element text("not a sequence");
        text.append(Element{});
    }, "append is restricted to sequence owners");
}

void positional_selection_resolution() {
    using anndatapp::Selection;

    const auto reverse = Selection(anndatapp::range{std::nullopt, std::nullopt, -1})
                             .resolve(4, "axis");
    expect(reverse.extent(4) == 4 && reverse.index(0) == 3 && reverse.index(3) == 0,
           "length-only resolution reuses reverse range stride mappings");

    const auto selected = Selection(anndatapp::indices{-1, 1}).resolve(4, "axis");
    expect(selected.extent(4) == 2 && selected.index(0) == 3 && selected.index(1) == 1,
           "length-only resolution normalizes negative positional indexes");

    const auto masked = Selection(anndatapp::mask{0, 1, 0, 1}).resolve(4, "axis");
    expect(masked.extent(4) == 2 && masked.index(0) == 1 && masked.index(1) == 3,
           "length-only resolution turns masks into positional mappings");
    expect(Selection(anndatapp::all).resolve(4, "axis").is_all(),
           "length-only resolution preserves the all-axis selector");

    expect_error(anndatapp::ErrorCode::out_of_bounds, "axis", [&] {
        (void)Selection(4).resolve(4, "axis");
    }, "length-only resolution bounds-checks positional indexes");
    expect_error(anndatapp::ErrorCode::shape_mismatch, "axis", [&] {
        (void)Selection(anndatapp::mask{1, 0}).resolve(4, "axis");
    }, "length-only resolution checks mask length");
    expect_error(anndatapp::ErrorCode::invalid_argument, "axis", [&] {
        (void)Selection("cell").resolve(4, "axis");
    }, "length-only resolution rejects a label without axis names");
    expect_error(anndatapp::ErrorCode::invalid_argument, "axis", [&] {
        (void)Selection(anndatapp::labels{"cell"}).resolve(4, "axis");
    }, "length-only resolution rejects label lists without axis names");
}

void mapping_traversal() {
    auto mapping = anndatapp::Element::mapping();
    mapping.set("explicit-null", anndatapp::Element{});
    mapping.set("value", anndatapp::Element("text"));
    const auto view = mapping.view();
    const auto first = view.mapping_key(0);
    const auto first_again = view.mapping_key(0);

    expect(view.mapping_size() == 2 && first == "explicit-null" &&
           view.mapping_key(1) == "value" && first.data() == first_again.data(),
           "mapping traversal preserves insertion order and borrows key bytes");
    expect(view.contains("explicit-null") &&
           view.at("explicit-null").kind() == anndatapp::ElementKind::null &&
           !view.contains("missing"),
           "mapping traversal preserves explicit null and missing-key distinction");

    expect_error(anndatapp::ErrorCode::out_of_bounds, "element/mapping", [&] {
        (void)view.mapping_key(view.mapping_size());
    }, "mapping traversal rejects key indexes past the end");
    auto sequence = anndatapp::Element::sequence();
    const auto sequence_view = sequence.view();
    expect_error(anndatapp::ErrorCode::invalid_argument, "element", [&] {
        (void)sequence_view.mapping_size();
    }, "mapping size is restricted to mapping views");
    expect_error(anndatapp::ErrorCode::invalid_argument, "element", [&] {
        (void)sequence_view.mapping_key(0);
    }, "mapping key access is restricted to mapping views");
}

} // namespace

int main() {
    heterogeneous_sequence();
    retained_views_and_bounds();
    positional_selection_resolution();
    mapping_traversal();
    if (failures) std::cerr << failures << " sequence test(s) failed\n";
    return failures ? 1 : 0;
}
