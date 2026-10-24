// Tests for the operators of avis::flags (include/avis/enum_flags.h)

#include "avis/enum_flags.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace avis::tests::flags_test
{
    enum class colour : std::uint8_t
    {
        red = 1,
        green = 2,
        blue = 4,
        alpha = 8
    };
} // namespace avis::tests::flags_test

ALLOW_FLAGS_FOR_ENUM(avis::tests::flags_test::colour)

namespace avis::tests::flags_test
{
    using colours = avis::flags<colour>;

    // Combining from inside a namespace: the operators must be found here too, not only at global scope
    colours warm() noexcept
    {
        return colour::red | colour::green | colour::alpha;
    }

    bool has_blue(const colours value) noexcept
    {
        return static_cast<bool>(value & colour::blue);
    }
} // namespace avis::tests::flags_test

using avis::tests::flags_test::colour;
using avis::tests::flags_test::colours;

TEST_CASE("flags combine with |, & and ^", "[enum_flags]")
{
    SECTION("enum | enum and chains")
    {
        const colours two = colour::red | colour::blue;
        CHECK(two.underlying_value() == 5);

        const colours three = colour::red | colour::green | colour::blue;
        CHECK(three.underlying_value() == 7);
    }

    SECTION("flags with enum, enum with flags, flags with flags")
    {
        const colours red = colour::red;
        CHECK((red | colour::green).underlying_value() == 3);
        CHECK((colour::green | red).underlying_value() == 3);
        CHECK((red | colours{ colour::blue }).underlying_value() == 5);
    }

    SECTION("& and ^")
    {
        const colours value = colour::red | colour::green;
        CHECK((value & colour::green).underlying_value() == 2);
        CHECK((value & colour::blue).empty());
        CHECK((colour::red & colour::green).empty());
        CHECK((value ^ colour::green).underlying_value() == 1);
        CHECK((colour::red ^ colour::red).empty());
    }

    SECTION("compound assignment")
    {
        colours value = colour::red;
        value |= colour::blue;
        CHECK(value.underlying_value() == 5);
        value &= colour::blue;
        CHECK(value.underlying_value() == 4);
        value ^= colour::alpha;
        CHECK(value.underlying_value() == 12);
    }

    SECTION("from inside a namespace")
    {
        CHECK(avis::tests::flags_test::warm().underlying_value() == 11);
        CHECK(avis::tests::flags_test::has_blue(colour::blue | colour::red));
        CHECK_FALSE(avis::tests::flags_test::has_blue(avis::tests::flags_test::warm()));
    }

    SECTION("constant expressions")
    {
        constexpr colours value = colour::red | colour::blue;
        STATIC_CHECK(value.underlying_value() == 5);
        STATIC_CHECK(
            (value & colour::blue) == colours(colour::blue)); // colours{ ... } would pick the non-constexpr list
    }
}

TEST_CASE("flags compare, iterate and swap", "[enum_flags]")
{
    const colours value = colour::green | colour::alpha;

    CHECK(value == (colour::alpha | colour::green));
    CHECK(value != colours{ colour::green });
    CHECK(value == colours{ colour::green, colour::alpha });

    std::vector<colour> members;
    for (const colour member : value)
    {
        members.push_back(member);
    }
    CHECK(members == std::vector<colour>{ colour::green, colour::alpha });
    CHECK(value.size() == 2);
    CHECK(value.count(colour::alpha) == 1);
    CHECK(value.count(colour::red) == 0);
    CHECK(value.find(colour::red) == value.end());
    CHECK(value.find(colour::green) != value.end());

    colours first = colour::red;
    colours second = colour::blue;
    using std::swap;
    swap(first, second); // finds the hidden friend
    CHECK(first == colours{ colour::blue });
    CHECK(second == colours{ colour::red });
}
