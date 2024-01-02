//
// C++ Implementation: starname
//
// Description:
//
//
// Author: Toti <root@totibox>, (C) 2005
//
// Copyright: See COPYING file that comes with this distribution
//
//

#include "starname.h"

#include <array>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <istream>
#include <optional>
#include <string>
#include <system_error>

#include <fmt/format.h>

#include <celcompat/charconv.h>
#include <celutil/greek.h>
#include "astroobj.h"
#include "constellation.h"


namespace
{

constexpr std::string_view::size_type MAX_CANONICAL_LENGTH = 256;
constexpr unsigned int FIRST_NUMBERED_VARIABLE = 335;

// Try parsing the first word of a name as a Flamsteed number or variable star
// designation. Single-letter variable star designations are handled by the
// Bayer parser due to indistinguishability with case-insensitive lookup.
bool isFlamsteedOrVariable(std::string_view prefix)
{
    using celestia::compat::from_chars;
    switch (prefix.size())
    {
        case 0:
            return false;
        case 1:
            // Match single-digit Flamsteed number
            return prefix[0] >= '1' && prefix[0] <= '9';
        case 2:
            {
                auto p0 = static_cast<unsigned char>(prefix[0]);
                auto p1 = static_cast<unsigned char>(prefix[1]);
                return
                    // Two-digit Flamsteed number
                    (std::isdigit(p0) && p0 != '0' && std::isdigit(p1)) ||
                    (std::isalpha(p0) && std::isalpha(p1) &&
                     std::tolower(p0) != 'j' && std::tolower(p1) != 'j' &&
                     p1 >= p0);
            }
        default:
            {
                // check for either Flamsteed or V### format variable star designations
                std::size_t startNumber = std::tolower(static_cast<unsigned char>(prefix[0])) == 'v'
                    ? 1
                    : 0;
                auto endPtr = prefix.data() + prefix.size();
                unsigned int value;
                auto [ptr, ec] = from_chars(prefix.data() + startNumber, endPtr, value);
                return ec == std::errc{} && ptr == endPtr &&
                       (startNumber == 0 || value >= FIRST_NUMBERED_VARIABLE);
            }
    }
}


struct BayerLetter
{
    std::string_view letter{ };
    unsigned int number{ 0 };
};


// Attempts to parse the first word of a star name as a Greek or Latin-letter
// Bayer designation, with optional numeric suffix
BayerLetter parseBayerLetter(std::string_view prefix)
{
    using celestia::compat::from_chars;

    BayerLetter result;
    if (auto numberPos = prefix.find_first_of("0123456789"); numberPos == std::string_view::npos)
        result.letter = prefix;
    else if (auto [ptr, ec] = from_chars(prefix.data() + numberPos, prefix.data() + prefix.size(), result.number);
             ec == std::errc{} && ptr == prefix.data() + prefix.size())
        result.letter = prefix.substr(0, numberPos);
    else
        return {};

    if (result.letter.empty())
        return {};

    if (auto greek = GetCanonicalGreekAbbreviation(result.letter); !greek.empty())
        result.letter = greek;
    else if (result.letter.size() != 1 || !std::isalpha(static_cast<unsigned char>(result.letter[0])))
        return {};

    return result;
}

} // end unnamed namespace


std::uint32_t
StarNameDatabase::findCatalogNumberByName(std::string_view name, bool i18n) const
{
    if (auto catalogNumber = getCatalogNumberByName(name, i18n);
        catalogNumber != AstroCatalog::InvalidIndex)
        return catalogNumber;

    if (auto pos = name.find(' '); pos != 0 && pos != std::string::npos && pos < name.size() - 1)
    {
        std::string_view prefix = name.substr(0, pos);
        std::string_view remainder = name.substr(pos + 1);

        if (auto catalogNumber = findFlamsteedOrVariable(prefix, remainder, i18n);
            catalogNumber != AstroCatalog::InvalidIndex)
            return catalogNumber;

        if (auto catalogNumber = findBayer(prefix, remainder, i18n);
            catalogNumber != AstroCatalog::InvalidIndex)
            return catalogNumber;
    }

    return findWithComponentSuffix(name, i18n);
}


std::uint32_t
StarNameDatabase::findFlamsteedOrVariable(std::string_view prefix,
                                          std::string_view remainder,
                                          bool i18n) const
{
    if (!isFlamsteedOrVariable(prefix))
        return AstroCatalog::InvalidIndex;

    auto [constellationAbbrev, suffix] = ParseConstellation(remainder);
    if (constellationAbbrev.empty() || (!suffix.empty() && suffix.front() != ' '))
        return AstroCatalog::InvalidIndex;

    std::array<char, MAX_CANONICAL_LENGTH> canonical;
    if (auto [it, size] = fmt::format_to_n(canonical.data(), canonical.size(), "{} {}{}",
                                           prefix, constellationAbbrev, suffix);
        size <= canonical.size())
    {
        auto catalogNumber = getCatalogNumberByName({canonical.data(), size}, i18n);
        if (catalogNumber != AstroCatalog::InvalidIndex)
            return catalogNumber;
    }

    if (!suffix.empty())
        return AstroCatalog::InvalidIndex;

    // try appending " A"
    if (auto [it, size] = fmt::format_to_n(canonical.data(), canonical.size(), "{} {} A",
                                           prefix, constellationAbbrev);
        size <= canonical.size())
        return getCatalogNumberByName({canonical.data(), size}, i18n);

    return AstroCatalog::InvalidIndex;
}


std::uint32_t
StarNameDatabase::findBayer(std::string_view prefix,
                            std::string_view remainder,
                            bool i18n) const
{
    auto bayerLetter = parseBayerLetter(prefix);
    if (bayerLetter.letter.empty())
        return AstroCatalog::InvalidIndex;

    auto [constellationAbbrev, suffix] = ParseConstellation(remainder);
    if (constellationAbbrev.empty() || (!suffix.empty() && suffix.front() != ' '))
        return AstroCatalog::InvalidIndex;

    return bayerLetter.number == 0
        ? findBayerNoNumber(bayerLetter.letter, constellationAbbrev, suffix, i18n)
        : findBayerWithNumber(bayerLetter.letter,
                              bayerLetter.number,
                              constellationAbbrev,
                              suffix,
                              i18n);
}


std::uint32_t
StarNameDatabase::findBayerNoNumber(std::string_view letter,
                                    std::string_view constellationAbbrev,
                                    std::string_view suffix,
                                    bool i18n) const
{
    std::array<char, MAX_CANONICAL_LENGTH> canonical;
    if (auto [it, size] = fmt::format_to_n(canonical.data(), canonical.size(), "{} {}{}",
                                           letter, constellationAbbrev, suffix);
        size <= canonical.size())
    {
        auto catalogNumber = getCatalogNumberByName({canonical.data(), size}, i18n);
        if (catalogNumber != AstroCatalog::InvalidIndex)
            return catalogNumber;
    }

    // Try appending "1" to the letter, e.g. ALF CVn --> ALF1 CVn
    if (auto [it, size] = fmt::format_to_n(canonical.data(), canonical.size(), "{}1 {}{}",
                                           letter, constellationAbbrev, suffix);
        size <= canonical.size())
    {
        auto catalogNumber = getCatalogNumberByName({canonical.data(), size}, i18n);
        if (catalogNumber != AstroCatalog::InvalidIndex)
            return catalogNumber;
    }

    if (!suffix.empty())
        return AstroCatalog::InvalidIndex;

    // No component suffix, so try appending " A"
    if (auto [it, size] = fmt::format_to_n(canonical.data(), canonical.size(), "{} {} A",
                                           letter, constellationAbbrev);
        size <= canonical.size())
    {
        auto catalogNumber = getCatalogNumberByName({canonical.data(), size}, i18n);
        if (catalogNumber != AstroCatalog::InvalidIndex)
            return catalogNumber;
    }

    if (auto [it, size] = fmt::format_to_n(canonical.data(), canonical.size(), "{}1 {} A",
                                           letter, constellationAbbrev);
        size <= canonical.size())
        return getCatalogNumberByName({canonical.data(), size}, i18n);

    return AstroCatalog::InvalidIndex;
}


std::uint32_t
StarNameDatabase::findBayerWithNumber(std::string_view letter,
                                      unsigned int number,
                                      std::string_view constellationAbbrev,
                                      std::string_view suffix,
                                      bool i18n) const
{
    std::array<char, MAX_CANONICAL_LENGTH> canonical;
    if (auto [it, size] = fmt::format_to_n(canonical.data(), canonical.size(), "{}{} {}{}",
                                           letter, number, constellationAbbrev, suffix);
        size <= canonical.size())
    {
        auto catalogNumber = getCatalogNumberByName({canonical.data(), size}, i18n);
        if (catalogNumber != AstroCatalog::InvalidIndex)
            return catalogNumber;
    }

    if (!suffix.empty())
        return AstroCatalog::InvalidIndex;

    // No component suffix, so try appending "A"
    if (auto [it, size] = fmt::format_to_n(canonical.data(), canonical.size(), "{}{} {} A",
                                           letter, number, constellationAbbrev);
        size <= canonical.size())
        return getCatalogNumberByName({canonical.data(), size}, i18n);

    return AstroCatalog::InvalidIndex;
}


std::uint32_t
StarNameDatabase::findWithComponentSuffix(std::string_view name, bool i18n) const
{
    std::array<char, MAX_CANONICAL_LENGTH> canonical;
    if (auto [it, size] = fmt::format_to_n(canonical.data(), canonical.size(), "{} A", name);
        size <= canonical.size())
        return getCatalogNumberByName({canonical.data(), size}, i18n);

    return AstroCatalog::InvalidIndex;
}


std::unique_ptr<StarNameDatabase>
StarNameDatabase::readNames(std::istream& in)
{
    using celestia::compat::from_chars;

    constexpr std::size_t maxLength = 1024;
    auto db = std::make_unique<StarNameDatabase>();
    std::string buffer(maxLength, '\0');
    while (!in.eof())
    {
        in.getline(buffer.data(), maxLength);
        // Delimiter is extracted and contributes to gcount() but is not stored
        std::size_t lineLength;

        if (in.good())
            lineLength = static_cast<std::size_t>(in.gcount() - 1);
        else if (in.eof())
            lineLength = static_cast<std::size_t>(in.gcount());
        else
            return nullptr;

        auto line = static_cast<std::string_view>(buffer).substr(0, lineLength);

        if (line.empty() || line.front() == '#')
            continue;

        auto pos = line.find(':');
        if (pos == std::string_view::npos)
            return nullptr;

        auto catalogNumber = AstroCatalog::InvalidIndex;
        if (auto [ptr, ec] = from_chars(line.data(), line.data() + pos, catalogNumber);
            ec != std::errc{} || ptr != line.data() + pos)
        {
            return nullptr;
        }

        // Iterate through the string for names delimited
        // by ':', and insert them into the star database. Note that
        // db->add() will skip empty names.
        line = line.substr(pos + 1);
        while (!line.empty())
        {
            std::string_view name;
            pos = line.find(':');
            if (pos == std::string_view::npos)
            {
                name = line;
                line = {};
            }
            else
            {
                name = line.substr(0, pos);
                line = line.substr(pos + 1);
            }

            db->add(catalogNumber, name);
        }
    }

    return db;
}
