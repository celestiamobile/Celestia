// stardb.cpp
//
// Copyright (C) 2001-2009, the Celestia Development Team
// Original version by Chris Laurel <claurel@gmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "stardb.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <istream>
#include <set>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <fmt/format.h>

#include <celcompat/bit.h>
#include <celcompat/charconv.h>
#include <celcompat/numbers.h>
#include <celutil/fsutils.h>
#include <celutil/gettext.h>
#include <celutil/intrusiveptr.h>
#include <celutil/logger.h>
#include <celutil/timer.h>
#include <celutil/tokenizer.h>
#include <celutil/stringutils.h>
#include "meshmanager.h"
#include "parser.h"
#include "value.h"

using namespace std::string_view_literals;
using celestia::util::GetLogger;
using celestia::util::IntrusivePtr;

namespace astro = celestia::astro;
namespace engine = celestia::engine;
namespace math = celestia::math;
namespace util = celestia::util;

// Enable the below to switch back to parsing coordinates as float to match
// legacy behaviour. This shouldn't be necessary since stars.dat stores
// Cartesian coordinates.
// #define PARSE_COORDS_FLOAT


struct StarDatabaseBuilder::CustomStarDetails
{
    bool hasCustomDetails{false};
    fs::path modelName;
    fs::path textureName;
    std::shared_ptr<const celestia::ephem::Orbit> orbit;
    std::shared_ptr<const celestia::ephem::RotationModel> rm;
    std::optional<Eigen::Vector3d> semiAxes{std::nullopt};
    std::optional<float> radius{std::nullopt};
    double temperature{0.0};
    std::optional<float> bolometricCorrection{std::nullopt};
    const std::string* infoURL{nullptr};
};


namespace
{

constexpr inline std::uint16_t StarDBVersion = 0x0100;
constexpr inline std::uint16_t CrossIndexVersion = 0x0100;

constexpr inline std::string_view HDCatalogPrefix        = "HD "sv;
constexpr inline std::string_view HIPPARCOSCatalogPrefix = "HIP "sv;
constexpr inline std::string_view TychoCatalogPrefix     = "TYC "sv;
constexpr inline std::string_view SAOCatalogPrefix       = "SAO "sv;
#if 0
constexpr inline std::string_view GlieseCatalogPrefix    = "Gliese "sv;
constexpr inline std::string_view RossCatalogPrefix      = "Ross "sv;
constexpr inline std::string_view LacailleCatalogPrefix  = "Lacaille "sv;
#endif

// The size of the root star octree node is also the maximum distance
// distance from the Sun at which any star may be located. The current
// setting of 1.0e7 light years is large enough to contain the entire
// local group of galaxies. A larger value should be OK, but the
// performance implications for octree traversal still need to be
// investigated.
constexpr inline float STAR_OCTREE_ROOT_SIZE   = 1000000000.0f;

constexpr inline float STAR_OCTREE_MAGNITUDE   = 6.0f;
//constexpr const float STAR_EXTRA_ROOM        = 0.01f; // Reserve 1% capacity for extra stars

constexpr inline std::string_view STARSDAT_MAGIC   = "CELSTARS"sv;
constexpr inline std::string_view CROSSINDEX_MAGIC = "CELINDEX"sv;

constexpr inline AstroCatalog::IndexNumber TYC3_MULTIPLIER = 1000000000u;
constexpr inline AstroCatalog::IndexNumber TYC2_MULTIPLIER = 10000u;
constexpr inline AstroCatalog::IndexNumber TYC123_MIN = 1u;
constexpr inline AstroCatalog::IndexNumber TYC1_MAX   = 9999u;  // actual upper limit is 9537 in TYC2
constexpr inline AstroCatalog::IndexNumber TYC2_MAX   = 99999u; // actual upper limit is 12121 in TYC2
constexpr inline AstroCatalog::IndexNumber TYC3_MAX   = 3u;     // from TYC2

// In the original Tycho catalog, TYC3 ranges from 1 to 3, so no there is
// no chance of overflow in the multiplication. TDSC (Fabricius et al. 2002)
// adds one entry with TYC3 = 4 (TYC 2907-1276-4) so permit TYC=4 when the
// TYC1 number is <= 2907
constexpr inline AstroCatalog::IndexNumber TDSC_TYC3_MAX            = 4u;
constexpr inline AstroCatalog::IndexNumber TDSC_TYC3_MAX_RANGE_TYC1 = 2907u;


template<typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
T
readIntLE(const char* src)
{
    using celestia::compat::byteswap;
    using celestia::compat::endian;

    T result;
    std::memcpy(&result, src, sizeof(T));
    if constexpr (endian::native == endian::little)
        return result;
    else
        return celestia::compat::byteswap(result);
}


float
readFloatLE(const char* src)
{
    using celestia::compat::byteswap;
    using celestia::compat::endian;
    float result;
    if constexpr (endian::native == endian::little)
    {
        std::memcpy(&result, src, sizeof(float));
    }
    else
    {
        static_assert(sizeof(std::uint32_t) == sizeof(float));
        std::uint32_t temp;
        std::memcpy(&temp, src, sizeof(float));
        temp = byteswap(temp);
        std::memcpy(&result, &temp, sizeof(float));
    }

    return result;
}


#pragma pack(push, 1)
// stars.dat header structure
struct StarsDatHeader
{
    StarsDatHeader() = delete;
    char magic[8];
    std::uint16_t version;
    std::uint32_t counter;
};

static_assert(std::is_standard_layout_v<StarsDatHeader>);


// stars.dat record structure
struct StarsDatRecord
{
    StarsDatRecord() = delete;
    AstroCatalog::IndexNumber catNo;
    float x;
    float y;
    float z;
    std::int16_t absMag;
    std::uint16_t spectralType;
};

static_assert(std::is_standard_layout_v<StarsDatRecord>);


// cross-index header structure
struct CrossIndexHeader
{
    CrossIndexHeader() = delete;
    char magic[8];
    std::uint16_t version;
};

static_assert(std::is_standard_layout_v<CrossIndexHeader>);


// cross-index record structure
struct CrossIndexRecord
{
    CrossIndexRecord() = delete;
    std::uint32_t catalogNumber;
    std::uint32_t celCatalogNumber;
};

static_assert(std::is_standard_layout_v<CrossIndexRecord>);

#pragma pack(pop)


bool
parseSimpleCatalogNumber(std::string_view name,
                         std::string_view prefix,
                         AstroCatalog::IndexNumber& catalogNumber)
{
    using celestia::compat::from_chars;
    if (compareIgnoringCase(name, prefix, prefix.size()) != 0)
        return false;

    // skip additional whitespace
    auto pos = name.find_first_not_of(" \t", prefix.size());
    if (pos == std::string_view::npos)
        return false;

    if (auto [ptr, ec] = from_chars(name.data() + pos, name.data() + name.size(), catalogNumber); ec == std::errc{})
    {
        // Do not match if suffix is present
        pos = name.find_first_not_of(" \t", ptr - name.data());
        return pos == std::string_view::npos;
    }

    return false;
}


bool
parseHIPPARCOSCatalogNumber(std::string_view name,
                            AstroCatalog::IndexNumber& catalogNumber)
{
    return parseSimpleCatalogNumber(name,
                                    HIPPARCOSCatalogPrefix,
                                    catalogNumber);
}


bool
parseHDCatalogNumber(std::string_view name,
                     AstroCatalog::IndexNumber& catalogNumber)
{
    return parseSimpleCatalogNumber(name,
                                    HDCatalogPrefix,
                                    catalogNumber);
}


bool
parseTychoCatalogNumber(std::string_view name,
                        AstroCatalog::IndexNumber& catalogNumber)
{
    using celestia::compat::from_chars;
    if (compareIgnoringCase(name, TychoCatalogPrefix, TychoCatalogPrefix.size()) != 0)
        return false;

    // skip additional whitespace
    auto pos = name.find_first_not_of(" \t", TychoCatalogPrefix.size());
    if (pos == std::string_view::npos)
        return false;

    const char* const end_ptr = name.data() + name.size();

    std::array<AstroCatalog::IndexNumber, 3> tycParts;
    auto result = from_chars(name.data() + pos, end_ptr, tycParts[0]);
    if (result.ec != std::errc{}
        || tycParts[0] < TYC123_MIN || tycParts[0] > TYC1_MAX
        || result.ptr == end_ptr
        || *result.ptr != '-')
    {
        return false;
    }

    result = from_chars(result.ptr + 1, end_ptr, tycParts[1]);
    if (result.ec != std::errc{}
        || tycParts[1] < TYC123_MIN || tycParts[1] > TYC2_MAX
        || result.ptr == end_ptr
        || *result.ptr != '-')
    {
        return false;
    }

    if (result = from_chars(result.ptr + 1, end_ptr, tycParts[2]);
        result.ec == std::errc{}
        && tycParts[2] >= TYC123_MIN
        && (tycParts[2] <= TYC3_MAX
            || (tycParts[2] == TDSC_TYC3_MAX && tycParts[0] <= TDSC_TYC3_MAX_RANGE_TYC1)))
    {
        // Do not match if suffix is present
        pos = name.find_first_not_of(" \t", result.ptr - name.data());
        if (pos != std::string_view::npos)
            return false;

        catalogNumber = tycParts[2] * TYC3_MULTIPLIER
                      + tycParts[1] * TYC2_MULTIPLIER
                      + tycParts[0];
        return true;
    }

    return false;
}


bool
parseCelestiaCatalogNumber(std::string_view name,
                           AstroCatalog::IndexNumber& catalogNumber)
{
    using celestia::compat::from_chars;
    if (name.size() == 0 || name[0] != '#')
        return false;

    if (auto [ptr, ec] = from_chars(name.data() + 1, name.data() + name.size(), catalogNumber);
        ec == std::errc{})
    {
        // Do not match if suffix is present
        auto pos = name.find_first_not_of(" \t", ptr - name.data());
        return pos == std::string_view::npos;
    }

    return false;
}


std::string
catalogNumberToString(AstroCatalog::IndexNumber catalogNumber)
{
    if (catalogNumber <= StarDatabase::MAX_HIPPARCOS_NUMBER)
    {
        return fmt::format("HIP {}", catalogNumber);
    }
    else
    {
        AstroCatalog::IndexNumber tyc3 = catalogNumber / TYC3_MULTIPLIER;
        catalogNumber -= tyc3 * TYC3_MULTIPLIER;
        AstroCatalog::IndexNumber tyc2 = catalogNumber / TYC2_MULTIPLIER;
        catalogNumber -= tyc2 * TYC2_MULTIPLIER;
        AstroCatalog::IndexNumber tyc1 = catalogNumber;
        return fmt::format("TYC {}-{}-{}", tyc1, tyc2, tyc3);
    }
}


void
stcError(const Tokenizer& tok, std::string_view msg)
{
    GetLogger()->error(_("Error in .stc file (line {}): {}\n"), tok.getLineNumber(), msg);
}


void
modifyStarDetails(Star* star,
                  IntrusivePtr<StarDetails>&& referenceDetails,
                  bool hasCustomDetails)
{
    StarDetails* existingDetails = star->getDetails();
    assert(existingDetails != nullptr);

    if (existingDetails->shared())
    {
        // If the star definition has extended information, clone the
        // star details so we can customize it without affecting other
        // stars of the same spectral type.
        if (hasCustomDetails)
            star->setDetails(referenceDetails == nullptr ? existingDetails->clone() : referenceDetails->clone());
        else if (referenceDetails != nullptr)
            star->setDetails(std::move(referenceDetails));
    }
    else if (referenceDetails != nullptr)
    {
        // If the spectral type was modified, copy the new data
        // to the custom details record.
        existingDetails->setSpectralType(referenceDetails->getSpectralType());
        existingDetails->setTemperature(referenceDetails->getTemperature());
        existingDetails->setBolometricCorrection(referenceDetails->getBolometricCorrection());
        if ((existingDetails->getKnowledge() & StarDetails::KnowTexture) == 0)
            existingDetails->setTexture(referenceDetails->getTexture());
        if ((existingDetails->getKnowledge() & StarDetails::KnowRotation) == 0)
            existingDetails->setRotationModel(referenceDetails->getRotationModel());
        existingDetails->setVisibility(referenceDetails->getVisibility());
    }
}


StarDatabaseBuilder::CustomStarDetails
parseCustomStarDetails(const Hash* starData,
                       const fs::path& path)
{
    StarDatabaseBuilder::CustomStarDetails customDetails;

    if (const std::string* mesh = starData->getString("Mesh"); mesh != nullptr)
    {
        if (auto meshPath = util::U8FileName(*mesh); meshPath.has_value())
            customDetails.modelName = std::move(*meshPath);
        else
            GetLogger()->error("Invalid filename in Mesh\n");
    }

    if (const std::string* texture = starData->getString("Texture"); texture != nullptr)
    {
        if (auto texturePath = util::U8FileName(*texture); texturePath.has_value())
            customDetails.textureName = std::move(*texturePath);
        else
            GetLogger()->error("Invalid filename in Texture\n");
    }

    customDetails.orbit = CreateOrbit(Selection(), starData, path, true);
    customDetails.rm = CreateRotationModel(starData, path, 1.0);
    customDetails.semiAxes = starData->getLengthVector<double>("SemiAxes");
    customDetails.radius = starData->getLength<float>("Radius");
    customDetails.temperature = starData->getNumber<double>("Temperature").value_or(0.0);
    customDetails.bolometricCorrection = starData->getNumber<float>("BoloCorrection");
    customDetails.infoURL = starData->getString("InfoURL");

    customDetails.hasCustomDetails = !customDetails.modelName.empty() ||
                                     !customDetails.textureName.empty() ||
                                     customDetails.orbit != nullptr ||
                                     customDetails.rm != nullptr ||
                                     customDetails.semiAxes.has_value() ||
                                     customDetails.radius.has_value() ||
                                     customDetails.temperature > 0.0 ||
                                     customDetails.bolometricCorrection.has_value() ||
                                     customDetails.infoURL != nullptr;

    return customDetails;
}

} // end unnamed namespace



StarDatabase::StarDatabase()
{
    crossIndexes.resize(static_cast<std::size_t>(StarCatalog::MaxCatalog));
}


StarDatabase::~StarDatabase()
{
    delete [] stars;
}


Star*
StarDatabase::find(AstroCatalog::IndexNumber catalogNumber) const
{
    auto star = std::lower_bound(catalogNumberIndex.cbegin(), catalogNumberIndex.cend(),
                                 catalogNumber,
                                 [](const Star* star, AstroCatalog::IndexNumber catNum) { return star->getIndex() < catNum; });

    if (star != catalogNumberIndex.cend() && (*star)->getIndex() == catalogNumber)
        return *star;
    else
        return nullptr;
}


AstroCatalog::IndexNumber
StarDatabase::findCatalogNumberByName(std::string_view name, bool i18n) const
{
    if (name.empty())
        return AstroCatalog::InvalidIndex;

    AstroCatalog::IndexNumber catalogNumber = AstroCatalog::InvalidIndex;

    if (namesDB != nullptr)
    {
        catalogNumber = namesDB->findCatalogNumberByName(name, i18n);
        if (catalogNumber != AstroCatalog::InvalidIndex)
            return catalogNumber;
    }

    if (parseCelestiaCatalogNumber(name, catalogNumber))
        return catalogNumber;
    if (parseHIPPARCOSCatalogNumber(name, catalogNumber))
        return catalogNumber;
    if (parseTychoCatalogNumber(name, catalogNumber))
        return catalogNumber;
    if (parseHDCatalogNumber(name, catalogNumber))
        return searchCrossIndexForCatalogNumber(StarCatalog::HenryDraper, catalogNumber);
    if (parseSimpleCatalogNumber(name, SAOCatalogPrefix, catalogNumber))
        return searchCrossIndexForCatalogNumber(StarCatalog::SAO, catalogNumber);

    return AstroCatalog::InvalidIndex;
}


Star*
StarDatabase::find(std::string_view name, bool i18n) const
{
    AstroCatalog::IndexNumber catalogNumber = findCatalogNumberByName(name, i18n);
    if (catalogNumber != AstroCatalog::InvalidIndex)
        return find(catalogNumber);
    else
        return nullptr;
}


AstroCatalog::IndexNumber
StarDatabase::crossIndex(StarCatalog catalog, AstroCatalog::IndexNumber celCatalogNumber) const
{
    auto catalogIndex = static_cast<std::size_t>(catalog);
    if (catalogIndex >= crossIndexes.size())
        return AstroCatalog::InvalidIndex;

    const CrossIndex& xindex = crossIndexes[catalogIndex];

    // A simple linear search.  We could store cross indices sorted by
    // both catalog numbers and trade memory for speed
    auto iter = std::find_if(xindex.begin(), xindex.end(),
                             [celCatalogNumber](const CrossIndexEntry& o) { return celCatalogNumber == o.celCatalogNumber; });
    return iter == xindex.end()
        ? AstroCatalog::InvalidIndex
        : iter->catalogNumber;
}


// Return the Celestia catalog number for the star with a specified number
// in a cross index.
AstroCatalog::IndexNumber
StarDatabase::searchCrossIndexForCatalogNumber(StarCatalog catalog, AstroCatalog::IndexNumber number) const
{
    auto catalogIndex = static_cast<std::size_t>(catalog);
    if (catalogIndex >= crossIndexes.size())
        return AstroCatalog::InvalidIndex;

    const CrossIndex& xindex = crossIndexes[catalogIndex];
    auto iter = std::lower_bound(xindex.begin(), xindex.end(), number,
                                 [](const CrossIndexEntry& ent, AstroCatalog::IndexNumber n) { return ent.catalogNumber < n; });
    return iter == xindex.end() || iter->catalogNumber != number
        ? AstroCatalog::InvalidIndex
        : iter->celCatalogNumber;
}


Star*
StarDatabase::searchCrossIndex(StarCatalog catalog, AstroCatalog::IndexNumber number) const
{
    AstroCatalog::IndexNumber celCatalogNumber = searchCrossIndexForCatalogNumber(catalog, number);
    if (celCatalogNumber != AstroCatalog::InvalidIndex)
        return find(celCatalogNumber);
    else
        return nullptr;
}


void
StarDatabase::getCompletion(std::vector<std::string>& completion, std::string_view name) const
{
    // only named stars are supported by completion.
    if (!name.empty() && namesDB != nullptr)
        namesDB->getCompletion(completion, name);
}


// Return the name for the star with specified catalog number.  The returned
// string will be:
//      the common name if it exists, otherwise
//      the Bayer or Flamsteed designation if it exists, otherwise
//      the HD catalog number if it exists, otherwise
//      the HIPPARCOS catalog number.
//
// CAREFUL:
// If the star name is not present in the names database, a new
// string is constructed to contain the catalog number--keep in
// mind that calling this method could possibly incur the overhead
// of a memory allocation (though no explcit deallocation is
// required as it's all wrapped in the string class.)
std::string
StarDatabase::getStarName(const Star& star, bool i18n) const
{
    AstroCatalog::IndexNumber catalogNumber = star.getIndex();

    if (namesDB != nullptr)
    {
        StarNameDatabase::NumberIndex::const_iterator iter = namesDB->getFirstNameIter(catalogNumber);
        if (iter != namesDB->getFinalNameIter() && iter->first == catalogNumber)
        {
            if (i18n)
            {
                const char * local = D_(iter->second.c_str());
                if (iter->second != local)
                    return local;
            }
            return iter->second;
        }
    }

    /*
      // Get the HD catalog name
      if (star.getIndex() != AstroCatalog::InvalidIndex)
      return fmt::format("HD {}", star.getIndex(Star::HDCatalog));
      else
    */
    return catalogNumberToString(catalogNumber);
}


std::string
StarDatabase::getStarNameList(const Star& star, const unsigned int maxNames) const
{
    std::string starNames;
    unsigned int catalogNumber = star.getIndex();
    std::set<std::string> nameSet;
    bool isNameSetEmpty = true;

    auto append = [&] (const std::string &str)
    {
        auto inserted = nameSet.insert(str);
        if (inserted.second)
        {
            if (isNameSetEmpty)
                isNameSetEmpty = false;
            else
                starNames += " / ";
            starNames += str;
        }
    };

    if (namesDB != nullptr)
    {
        StarNameDatabase::NumberIndex::const_iterator iter = namesDB->getFirstNameIter(catalogNumber);

        while (iter != namesDB->getFinalNameIter() && iter->first == catalogNumber && nameSet.size() < maxNames)
        {
            append(D_(iter->second.c_str()));
            ++iter;
        }
    }

    AstroCatalog::IndexNumber hip  = catalogNumber;
    if (hip != AstroCatalog::InvalidIndex && hip != 0 && nameSet.size() < maxNames)
    {
        if (hip <= Star::MaxTychoCatalogNumber)
        {
            if (hip >= 1000000)
            {
                AstroCatalog::IndexNumber h = hip;
                AstroCatalog::IndexNumber tyc3 = h / TYC3_MULTIPLIER;
                h -= tyc3 * TYC3_MULTIPLIER;
                AstroCatalog::IndexNumber tyc2 = h / TYC2_MULTIPLIER;
                h -= tyc2 * TYC2_MULTIPLIER;
                AstroCatalog::IndexNumber tyc1 = h;

                append(fmt::format("TYC {}-{}-{}", tyc1, tyc2, tyc3));
            }
            else
            {
                append(fmt::format("HIP {}", hip));
            }
        }
    }

    AstroCatalog::IndexNumber hd   = crossIndex(StarCatalog::HenryDraper, hip);
    if (nameSet.size() < maxNames && hd != AstroCatalog::InvalidIndex)
        append(fmt::format("HD {}", hd));

    AstroCatalog::IndexNumber sao   = crossIndex(StarCatalog::SAO, hip);
    if (nameSet.size() < maxNames && sao != AstroCatalog::InvalidIndex)
        append(fmt::format("SAO {}", sao));

    return starNames;
}


void
StarDatabase::findVisibleStars(StarHandler& starHandler,
                               const Eigen::Vector3f& position,
                               const Eigen::Quaternionf& orientation,
                               float fovY,
                               float aspectRatio,
                               float limitingMag) const
{
    // Compute the bounding planes of an infinite view frustum
    Eigen::Hyperplane<float, 3> frustumPlanes[5];
    Eigen::Vector3f planeNormals[5];
    Eigen::Matrix3f rot = orientation.toRotationMatrix();
    float h = (float) tan(fovY / 2);
    float w = h * aspectRatio;
    planeNormals[0] = Eigen::Vector3f(0.0f, 1.0f, -h);
    planeNormals[1] = Eigen::Vector3f(0.0f, -1.0f, -h);
    planeNormals[2] = Eigen::Vector3f(1.0f, 0.0f, -w);
    planeNormals[3] = Eigen::Vector3f(-1.0f, 0.0f, -w);
    planeNormals[4] = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    for (int i = 0; i < 5; i++)
    {
        planeNormals[i] = rot.transpose() * planeNormals[i].normalized();
        frustumPlanes[i] = Eigen::Hyperplane<float, 3>(planeNormals[i], position);
    }

    octreeRoot->processVisibleObjects(starHandler,
                                      position,
                                      frustumPlanes,
                                      limitingMag,
                                      STAR_OCTREE_ROOT_SIZE);
}


void
StarDatabase::findCloseStars(StarHandler& starHandler,
                             const Eigen::Vector3f& position,
                             float radius) const
{
    octreeRoot->processCloseObjects(starHandler,
                                    position,
                                    radius,
                                    STAR_OCTREE_ROOT_SIZE);
}


StarNameDatabase*
StarDatabase::getNameDatabase() const
{
    return namesDB.get();
}


bool
StarDatabaseBuilder::loadBinary(std::istream& in)
{
    Timer timer{};
    std::uint32_t nStarsInFile = 0;
    {
        std::array<char, sizeof(StarsDatHeader)> header;
        if (!in.read(header.data(), header.size()).good()) /* Flawfinder: ignore */
            return false;

        // Verify the magic string
        if (std::string_view(header.data() + offsetof(StarsDatHeader, magic), STARSDAT_MAGIC.size()) != STARSDAT_MAGIC)
            return false;

        // Verify the version

        if (auto version = readIntLE<std::uint16_t>(header.data() + offsetof(StarsDatHeader, version));
            version != StarDBVersion)
        {
            return false;
        }

        // Read the star count
        nStarsInFile = readIntLE<std::uint32_t>(header.data() + offsetof(StarsDatHeader, counter));
    }

    constexpr std::uint32_t BUFFER_RECORDS = UINT32_C(4096) / sizeof(StarsDatRecord);
    std::vector<char> buffer(sizeof(StarsDatRecord) * BUFFER_RECORDS);
    std::uint32_t nStarsRemaining = nStarsInFile;
    while (nStarsRemaining > 0)
    {
        std::uint32_t recordsToRead = std::min(BUFFER_RECORDS, nStarsRemaining);
        if (!in.read(buffer.data(), sizeof(StarsDatRecord) * recordsToRead).good()) /* Flawfinder: ignore */
            return false;

        const char* ptr = buffer.data();
        for (std::uint32_t i = 0; i < recordsToRead; ++i)
        {
            auto catNo = readIntLE<AstroCatalog::IndexNumber>(ptr + offsetof(StarsDatRecord, catNo));
            float x = readFloatLE(ptr + offsetof(StarsDatRecord, x));
            float y = readFloatLE(ptr + offsetof(StarsDatRecord, y));
            float z = readFloatLE(ptr + offsetof(StarsDatRecord, z));
            auto absMag = readIntLE<std::int16_t>(ptr + offsetof(StarsDatRecord, absMag));
            auto spectralType = readIntLE<std::uint16_t>(ptr + offsetof(StarsDatRecord, spectralType));

            Star star;
            star.setPosition(x, y, z);
            star.setAbsoluteMagnitude(static_cast<float>(absMag) / 256.0f);

            IntrusivePtr<StarDetails> details = nullptr;
            StellarClass sc;
            if (sc.unpackV1(spectralType))
                details = StarDetails::GetStarDetails(sc);

            if (details == nullptr)
            {
                GetLogger()->error(_("Bad spectral type in star database, star #{}\n"), starDB->nStars);
                return false;
            }

            star.setDetails(std::move(details));
            star.setIndex(catNo);
            unsortedStars.add(star);

            ptr += sizeof(StarsDatRecord);
            ++starDB->nStars;
        }

        nStarsRemaining -= recordsToRead;
    }

    if (in.bad())
        return false;

    auto loadTime = timer.getTime();

    GetLogger()->debug("StarDatabase::read: nStars = {}, time = {} ms\n", nStarsInFile, loadTime);
    GetLogger()->info(_("{} stars in binary database\n"), starDB->nStars);

    // Create the temporary list of stars sorted by catalog number; this
    // will be used to lookup stars during file loading. After loading is
    // complete, the stars are sorted into an octree and this list gets
    // replaced.
    if (auto binFileStarCount = unsortedStars.size(); binFileStarCount > 0)
    {
        binFileCatalogNumberIndex.resize(binFileStarCount);
        for (unsigned int i = 0; i < binFileStarCount; i++)
        {
            binFileCatalogNumberIndex[i] = &unsortedStars[i];
        }

        std::sort(binFileCatalogNumberIndex.begin(), binFileCatalogNumberIndex.end(),
                  [](const Star* star0, const Star* star1) { return star0->getIndex() < star1->getIndex(); });
    }

    return true;
}


/*! Load an STC file with star definitions. Each definition has the form:
 *
 *  [disposition] [object type] [catalog number] [name]
 *  {
 *      [properties]
 *  }
 *
 *  Disposition is either Add, Replace, or Modify; Add is the default.
 *  Object type is either Star or Barycenter, with Star the default
 *  It is an error to omit both the catalog number and the name.
 *
 *  The dispositions are slightly more complicated than suggested by
 *  their names. Every star must have an unique catalog number. But
 *  instead of generating an error, Adding a star with a catalog
 *  number that already exists will actually replace that star. Here
 *  are how all of the possibilities are handled:
 *
 *  <name> or <number> already exists:
 *  Add <name>        : new star
 *  Add <number>      : replace star
 *  Replace <name>    : replace star
 *  Replace <number>  : replace star
 *  Modify <name>     : modify star
 *  Modify <number>   : modify star
 *
 *  <name> or <number> doesn't exist:
 *  Add <name>        : new star
 *  Add <number>      : new star
 *  Replace <name>    : new star
 *  Replace <number>  : new star
 *  Modify <name>     : error
 *  Modify <number>   : error
 */
bool
StarDatabaseBuilder::load(std::istream& in, const fs::path& resourcePath)
{
    Tokenizer tokenizer(&in);
    Parser parser(&tokenizer);

#ifdef ENABLE_NLS
    std::string domain = resourcePath.string();
    const char *d = domain.c_str();
    bindtextdomain(d, d); // domain name is the same as resource path
#else
    std::string domain;
#endif

    while (tokenizer.nextToken() != Tokenizer::TokenEnd)
    {
        bool isStar = true;

        // Parse the disposition--either Add, Replace, or Modify. The disposition
        // may be omitted. The default value is Add.
        DataDisposition disposition = DataDisposition::Add;
        if (auto tokenValue = tokenizer.getNameValue(); tokenValue.has_value())
        {
            if (*tokenValue == "Modify")
            {
                disposition = DataDisposition::Modify;
                tokenizer.nextToken();
            }
            else if (*tokenValue == "Replace")
            {
                disposition = DataDisposition::Replace;
                tokenizer.nextToken();
            }
            else if (*tokenValue == "Add")
            {
                disposition = DataDisposition::Add;
                tokenizer.nextToken();
            }
        }

        // Parse the object type--either Star or Barycenter. The object type
        // may be omitted. The default is Star.
        if (auto tokenValue = tokenizer.getNameValue(); tokenValue.has_value())
        {
            if (*tokenValue == "Star")
            {
                isStar = true;
            }
            else if (*tokenValue == "Barycenter")
            {
                isStar = false;
            }
            else
            {
                stcError(tokenizer, "unrecognized object type");
                return false;
            }
            tokenizer.nextToken();
        }

        // Parse the catalog number; it may be omitted if a name is supplied.
        AstroCatalog::IndexNumber catalogNumber = AstroCatalog::InvalidIndex;
        if (auto tokenValue = tokenizer.getNumberValue(); tokenValue.has_value())
        {
            catalogNumber = static_cast<AstroCatalog::IndexNumber>(*tokenValue);
            tokenizer.nextToken();
        }

        std::string objName;
        std::string firstName;
        if (auto tokenValue = tokenizer.getStringValue(); tokenValue.has_value())
        {
            // A star name (or names) is present
            objName = *tokenValue;
            tokenizer.nextToken();
            if (!objName.empty())
            {
                std::string::size_type next = objName.find(':', 0);
                firstName = objName.substr(0, next);
            }
        }

        // now goes the star definition
        if (tokenizer.getTokenType() != Tokenizer::TokenBeginGroup)
        {
            GetLogger()->error("Unexpected token at line {}!\n", tokenizer.getLineNumber());
            return false;
        }

        Star* star = nullptr;

        switch (disposition)
        {
        case DataDisposition::Add:
            // Automatically generate a catalog number for the star if one isn't
            // supplied.
            if (catalogNumber == AstroCatalog::InvalidIndex)
            {
                if (!isStar && firstName.empty())
                {
                    GetLogger()->error("Bad barycenter: neither catalog number nor name set at line {}.\n", tokenizer.getLineNumber());
                    return false;
                }
                catalogNumber = nextAutoCatalogNumber--;
            }
            else
            {
                star = findWhileLoading(catalogNumber);
            }
            break;

        case DataDisposition::Replace:
            if (catalogNumber == AstroCatalog::InvalidIndex)
            {
                if (!firstName.empty())
                    catalogNumber = starDB->findCatalogNumberByName(firstName, false);
            }

            if (catalogNumber == AstroCatalog::InvalidIndex)
                catalogNumber = nextAutoCatalogNumber--;
            else
                star = findWhileLoading(catalogNumber);
            break;

        case DataDisposition::Modify:
            // If no catalog number was specified, try looking up the star by name
            if (catalogNumber == AstroCatalog::InvalidIndex && !firstName.empty())
                catalogNumber = starDB->findCatalogNumberByName(firstName, false);

            if (catalogNumber != AstroCatalog::InvalidIndex)
                star = findWhileLoading(catalogNumber);

            break;
        }

        bool isNewStar = star == nullptr;

        tokenizer.pushBack();

        const Value starDataValue = parser.readValue();
        const Hash* starData = starDataValue.getHash();
        if (starData == nullptr)
        {
            GetLogger()->error("Bad star definition at line {}.\n", tokenizer.getLineNumber());
            return false;
        }

        if (isNewStar)
            star = new Star();

        bool ok = false;
        if (isNewStar && disposition == DataDisposition::Modify)
        {
            GetLogger()->warn("Modify requested for nonexistent star.\n");
        }
        else
        {
            ok = createStar(star, disposition, catalogNumber, starData, resourcePath, !isStar);
            loadCategories(catalogNumber, starData, disposition, domain);
        }

        if (ok)
        {
            if (isNewStar)
            {
                unsortedStars.add(*star);
                ++starDB->nStars;
                delete star;

                // Add the new star to the temporary (load time) index.
                stcFileCatalogNumberIndex[catalogNumber] = &unsortedStars[unsortedStars.size() - 1];
            }

            if (starDB->namesDB != nullptr && !objName.empty())
            {
                // List of namesDB will replace any that already exist for
                // this star.
                starDB->namesDB->erase(catalogNumber);

                // Iterate through the string for names delimited
                // by ':', and insert them into the star database.
                // Note that db->add() will skip empty namesDB.
                std::string::size_type startPos = 0;
                while (startPos != std::string::npos)
                {
                    std::string::size_type next   = objName.find(':', startPos);
                    std::string::size_type length = std::string::npos;
                    if (next != std::string::npos)
                    {
                        length = next - startPos;
                        ++next;
                    }
                    std::string starName = objName.substr(startPos, length);
                    starDB->namesDB->add(catalogNumber, starName);
                    startPos = next;
                }
            }
        }
        else
        {
            if (isNewStar)
                delete star;
            GetLogger()->info("Bad star definition--will continue parsing file.\n");
        }
    }

    return true;
}


void
StarDatabaseBuilder::setNameDatabase(std::unique_ptr<StarNameDatabase>&& nameDB)
{
    starDB->namesDB = std::move(nameDB);
}


bool
StarDatabaseBuilder::loadCrossIndex(StarCatalog catalog, std::istream& in)
{
    Timer timer{};

    auto catalogIndex = static_cast<std::size_t>(catalog);
    if (catalogIndex >= starDB->crossIndexes.size())
        return false;

    // Verify that the cross index file has a correct header
    {
        std::array<char, sizeof(CrossIndexHeader)> header;
        if (!in.read(header.data(), header.size()).good()) /* Flawfinder: ignore */
            return false;

        // Verify the magic string
        if (std::string_view(header.data() + offsetof(CrossIndexHeader, magic), CROSSINDEX_MAGIC.size()) != CROSSINDEX_MAGIC)
        {
            GetLogger()->error(_("Bad header for cross index\n"));
            return false;
        }

        // Verify the version
        auto version = readIntLE<std::uint16_t>(header.data() + offsetof(CrossIndexHeader, version));
        if (version != CrossIndexVersion)
        {
            GetLogger()->error(_("Bad version for cross index\n"));
            return false;
        }
    }

    StarDatabase::CrossIndex& xindex = starDB->crossIndexes[catalogIndex];
    xindex = {};

    constexpr std::uint32_t BUFFER_RECORDS = UINT32_C(4096) / sizeof(CrossIndexRecord);
    std::vector<char> buffer(sizeof(CrossIndexRecord) * BUFFER_RECORDS);
    bool hasMoreRecords = true;
    while (hasMoreRecords)
    {
        std::size_t remainingRecords = BUFFER_RECORDS;
        in.read(buffer.data(), buffer.size());
        if (in.bad())
        {
            GetLogger()->error(_("Loading cross index failed\n"));
            xindex = {};
            return false;
        }
        if (in.eof())
        {
            auto bytesRead = static_cast<std::uint32_t>(in.gcount());
            remainingRecords = bytesRead / sizeof(CrossIndexRecord);
            // disallow partial records
            if (bytesRead % sizeof(CrossIndexRecord) != 0)
            {
                GetLogger()->error(_("Loading cross index failed - unexpected EOF\n"));
                xindex = {};
                return false;
            }

            hasMoreRecords = false;
        }

        xindex.reserve(xindex.size() + remainingRecords);

        const char* ptr = buffer.data();
        while (remainingRecords-- > 0)
        {
            StarDatabase::CrossIndexEntry& ent = xindex.emplace_back();
            ent.catalogNumber = readIntLE<AstroCatalog::IndexNumber>(ptr + offsetof(CrossIndexRecord, catalogNumber));
            ent.celCatalogNumber = readIntLE<AstroCatalog::IndexNumber>(ptr + offsetof(CrossIndexRecord, celCatalogNumber));
            ptr += sizeof(CrossIndexRecord);
        }
    }

    GetLogger()->debug("Loaded xindex in {} ms\n", timer.getTime());

    std::sort(xindex.begin(), xindex.end());
    return true;
}


std::unique_ptr<StarDatabase>
StarDatabaseBuilder::finish()
{
    GetLogger()->info(_("Total star count: {}\n"), starDB->nStars);

    buildOctree();
    buildIndexes();

    // Resolve all barycenters; this can't be done before star sorting. There's
    // still a bug here: final orbital radii aren't available until after
    // the barycenters have been resolved, and these are required when building
    // the octree.  This will only rarely cause a problem, but it still needs
    // to be addressed.
    for (const auto& b : barycenters)
    {
        Star* star = starDB->find(b.catNo);
        Star* barycenter = starDB->find(b.barycenterCatNo);
        assert(star != nullptr);
        assert(barycenter != nullptr);
        if (star != nullptr && barycenter != nullptr)
        {
            star->setOrbitBarycenter(barycenter);
            barycenter->addOrbitingStar(star);
        }
    }

    for (const auto& [catalogNumber, category] : categories)
    {
        Star* star = starDB->find(catalogNumber);
        UserCategory::addObject(star, category);
    }

    return std::move(starDB);
}


/*! Load star data from a property list into a star instance.
 */
bool
StarDatabaseBuilder::createStar(Star* star,
                                DataDisposition disposition,
                                AstroCatalog::IndexNumber catalogNumber,
                                const Hash* starData,
                                const fs::path& path,
                                bool isBarycenter)
{
    std::optional<Eigen::Vector3f> barycenterPosition = std::nullopt;
    if (!createOrUpdateStarDetails(star,
                                   disposition,
                                   catalogNumber,
                                   starData,
                                   path,
                                   isBarycenter,
                                   barycenterPosition))
        return false;

    if (disposition != DataDisposition::Modify)
        star->setIndex(catalogNumber);

    // Compute the position in rectangular coordinates.  If a star has an
    // orbit and barycenter, its position is the position of the barycenter.
    if (barycenterPosition.has_value())
        star->setPosition(*barycenterPosition);
    else if (auto rectangularPos = starData->getLengthVector<float>("Position", astro::KM_PER_LY<double>); rectangularPos.has_value())
    {
        // "Position" allows the position of the star to be specified in
        // coordinates matching those used in stars.dat, allowing an exact
        // translation of stars.dat entries to .stc.
        star->setPosition(*rectangularPos);
    }
    else
    {
        double ra = 0.0;
        double dec = 0.0;
        double distance = 0.0;

        if (disposition == DataDisposition::Modify)
        {
            Eigen::Vector3f pos = star->getPosition();

            // Convert from Celestia's coordinate system
            Eigen::Vector3f v(pos.x(), -pos.z(), pos.y());
            v = Eigen::Quaternionf(Eigen::AngleAxis<float>((float) astro::J2000Obliquity, Eigen::Vector3f::UnitX())) * v;

            distance = v.norm();
            if (distance > 0.0)
            {
                v.normalize();
                ra = math::radToDeg(std::atan2(v.y(), v.x())) / astro::DEG_PER_HRA;
                dec = math::radToDeg(std::asin(v.z()));
            }
        }

        bool modifyPosition = false;
        if (auto raValue = starData->getAngle<double>("RA", astro::DEG_PER_HRA, 1.0); raValue.has_value())
        {
            ra = *raValue;
            modifyPosition = true;
        }
        else if (disposition != DataDisposition::Modify)
        {
            GetLogger()->error(_("Invalid star: missing right ascension\n"));
            return false;
        }

        if (auto decValue = starData->getAngle<double>("Dec"); decValue.has_value())
        {
            dec = *decValue;
            modifyPosition = true;
        }
        else if (disposition != DataDisposition::Modify)
        {
            GetLogger()->error(_("Invalid star: missing declination.\n"));
            return false;
        }

        if (auto dist = starData->getLength<double>("Distance", astro::KM_PER_LY<double>); dist.has_value())
        {
            distance = *dist;
            modifyPosition = true;
        }
        else if (disposition != DataDisposition::Modify)
        {
            GetLogger()->error(_("Invalid star: missing distance.\n"));
            return false;
        }

        if (modifyPosition)
        {
#ifdef PARSE_COORDS_FLOAT
            // Truncate to floats to match behavior of reading from binary file.
            // (No longer applies since binary file stores Cartesians)
            // The conversion to rectangular coordinates is still performed at
            // double precision, however.
            float raf = ((float) ra);
            float decf = ((float) dec);
            float distancef = ((float) distance);
            Eigen::Vector3d pos = astro::equatorialToCelestialCart((double) raf, (double) decf, (double) distancef);
#else
            Eigen::Vector3d pos = astro::equatorialToCelestialCart(ra, dec, distance);
#endif
            star->setPosition(pos.cast<float>());
        }
    }

    if (isBarycenter)
    {
        star->setAbsoluteMagnitude(30.0f);
    }
    else
    {
        bool absoluteDefined = true;
        std::optional<float> magnitude = starData->getNumber<float>("AbsMag");
        if (!magnitude.has_value())
        {
            absoluteDefined = false;
            if (auto appMag = starData->getNumber<float>("AppMag"); appMag.has_value())
            {
                float distance = star->getPosition().norm();

                // We can't compute the intrinsic brightness of the star from
                // the apparent magnitude if the star is within a few AU of the
                // origin.
                if (distance < 1e-5f)
                {
                    GetLogger()->error(_("Invalid star: absolute (not apparent) magnitude must be specified for star near origin\n"));
                    return false;
                }
                magnitude = astro::appToAbsMag(*appMag, distance);
            }
            else if (disposition != DataDisposition::Modify)
            {
                GetLogger()->error(_("Invalid star: missing magnitude.\n"));
                return false;
            }
        }

        if (magnitude.has_value())
            star->setAbsoluteMagnitude(*magnitude);

        if (auto extinction = starData->getNumber<float>("Extinction"); extinction.has_value())
        {
            float distance = star->getPosition().norm();
            if (distance != 0.0f)
                star->setExtinction(*extinction / distance);
            else
                extinction = 0.0f;
            if (!absoluteDefined)
                star->setAbsoluteMagnitude(star->getAbsoluteMagnitude() - *extinction);
        }
    }

    return true;
}


bool
StarDatabaseBuilder::createOrUpdateStarDetails(Star* star,
                                               DataDisposition disposition,
                                               AstroCatalog::IndexNumber catalogNumber,
                                               const Hash* starData,
                                               const fs::path& path,
                                               const bool isBarycenter,
                                               std::optional<Eigen::Vector3f>& barycenterPosition)
{
    barycenterPosition = std::nullopt;
    IntrusivePtr<StarDetails> referenceDetails;

    // Get the magnitude and spectral type; if the star is actually
    // a barycenter placeholder, these fields are ignored.
    if (isBarycenter)
    {
        referenceDetails = StarDetails::GetBarycenterDetails();
    }
    else
    {
        const std::string* spectralType = starData->getString("SpectralType");
        if (spectralType != nullptr)
        {
            StellarClass sc = StellarClass::parse(*spectralType);
            referenceDetails = StarDetails::GetStarDetails(sc);
            if (referenceDetails == nullptr)
            {
                GetLogger()->error(_("Invalid star: bad spectral type.\n"));
                return false;
            }
        }
        else if (disposition != DataDisposition::Modify)
        {
            // Spectral type is required for new stars
            GetLogger()->error(_("Invalid star: missing spectral type.\n"));
            return false;
        }
    }

    CustomStarDetails customDetails = parseCustomStarDetails(starData, path);
    barycenterPosition = std::nullopt;

    if (disposition == DataDisposition::Modify)
        modifyStarDetails(star, std::move(referenceDetails), customDetails.hasCustomDetails);
    else
        star->setDetails(customDetails.hasCustomDetails ? referenceDetails->clone() : referenceDetails);

    return applyCustomStarDetails(star,
                                  catalogNumber,
                                  starData,
                                  path,
                                  customDetails,
                                  barycenterPosition);
}


bool
StarDatabaseBuilder::applyCustomStarDetails(const Star* star,
                                            AstroCatalog::IndexNumber catalogNumber,
                                            const Hash* starData,
                                            const fs::path& path,
                                            const CustomStarDetails& customDetails,
                                            std::optional<Eigen::Vector3f>& barycenterPosition)
{
    if (!customDetails.hasCustomDetails)
        return true;

    StarDetails* details = star->getDetails();
    assert(!details->shared());

    if (!customDetails.textureName.empty())
    {
        details->setTexture(MultiResTexture(customDetails.textureName, path));
        details->addKnowledge(StarDetails::KnowTexture);
    }

    if (!customDetails.modelName.empty())
    {
        using engine::GeometryInfo;
        using engine::GetGeometryManager;
        ResourceHandle geometryHandle = GetGeometryManager()->getHandle(GeometryInfo(customDetails.modelName,
                                                                                     path,
                                                                                     Eigen::Vector3f::Zero(),
                                                                                     1.0f,
                                                                                     true));
        details->setGeometry(geometryHandle);
    }

    if (customDetails.semiAxes.has_value())
        details->setEllipsoidSemiAxes(customDetails.semiAxes->cast<float>());

    if (customDetails.radius.has_value())
    {
        details->setRadius(*customDetails.radius);
        details->addKnowledge(StarDetails::KnowRadius);
    }

    if (customDetails.temperature > 0.0)
    {
        details->setTemperature(static_cast<float>(customDetails.temperature));

        if (!customDetails.bolometricCorrection.has_value())
        {
            // if we change the temperature, recalculate the bolometric
            // correction using formula from formula for main sequence
            // stars given in B. Cameron Reed (1998), "The Composite
            // Observational-Theoretical HR Diagram", Journal of the Royal
            // Astronomical Society of Canada, Vol 92. p36.

            double logT = std::log10(customDetails.temperature) - 4;
            double bc = -8.499 * std::pow(logT, 4) + 13.421 * std::pow(logT, 3)
                        - 8.131 * logT * logT - 3.901 * logT - 0.438;

            details->setBolometricCorrection(static_cast<float>(bc));
        }
    }

    if (customDetails.bolometricCorrection.has_value())
    {
        details->setBolometricCorrection(*customDetails.bolometricCorrection);
    }

    if (customDetails.infoURL != nullptr)
        details->setInfoURL(*customDetails.infoURL);

    if (!applyOrbit(catalogNumber, starData, details, customDetails, barycenterPosition))
        return false;

    if (customDetails.rm != nullptr)
        details->setRotationModel(customDetails.rm);

    return true;
}


bool
StarDatabaseBuilder::applyOrbit(AstroCatalog::IndexNumber catalogNumber,
                                const Hash* starData,
                                StarDetails* details,
                                const CustomStarDetails& customDetails,
                                std::optional<Eigen::Vector3f>& barycenterPosition)
{
    if (customDetails.orbit == nullptr)
        return true;

    details->setOrbit(customDetails.orbit);

    // See if a barycenter was specified as well
    AstroCatalog::IndexNumber barycenterCatNo = AstroCatalog::InvalidIndex;
    bool barycenterDefined = false;

    const std::string* barycenterName = starData->getString("OrbitBarycenter");
    if (barycenterName != nullptr)
    {
        barycenterCatNo   = starDB->findCatalogNumberByName(*barycenterName, false);
        barycenterDefined = true;
    }
    else if (auto barycenterNumber = starData->getNumber<AstroCatalog::IndexNumber>("OrbitBarycenter");
                barycenterNumber.has_value())
    {
        barycenterCatNo   = *barycenterNumber;
        barycenterDefined = true;
    }

    if (barycenterDefined)
    {
        if (barycenterCatNo != AstroCatalog::InvalidIndex)
        {
            // We can't actually resolve the barycenter catalog number
            // to a Star pointer until after all stars have been loaded
            // and spatially sorted.  Just store it in a list to be
            // resolved after sorting.
            BarycenterUsage bc;
            bc.catNo = catalogNumber;
            bc.barycenterCatNo = barycenterCatNo;
            barycenters.push_back(bc);

            // Even though we can't actually get the Star pointer for
            // the barycenter, we can get the star information.
            Star* barycenter = findWhileLoading(barycenterCatNo);
            if (barycenter != nullptr)
                barycenterPosition = barycenter->getPosition();
        }

        if (!barycenterPosition.has_value())
        {
            if (barycenterName == nullptr)
                GetLogger()->error(_("Barycenter {} does not exist.\n"), barycenterCatNo);
            else
                GetLogger()->error(_("Barycenter {} does not exist.\n"), *barycenterName);
            return false;
        }
    }

    return true;
}


void
StarDatabaseBuilder::loadCategories(AstroCatalog::IndexNumber catalogNumber,
                                    const Hash *hash,
                                    DataDisposition disposition,
                                    const std::string &domain)
{
    if (disposition == DataDisposition::Replace)
        categories.erase(catalogNumber);

    const Value* categoryValue = hash->getValue("Category");
    if (categoryValue == nullptr)
        return;

    if (const std::string* categoryName = categoryValue->getString(); categoryName != nullptr)
    {
        if (categoryName->empty())
            return;

        addCategory(catalogNumber, *categoryName, domain);
        return;
    }

    const ValueArray *arr = categoryValue->getArray();
    if (arr == nullptr)
        return;

    for (const auto& it : *arr)
    {
        const std::string* categoryName = it.getString();
        if (categoryName == nullptr || categoryName->empty())
            continue;

        addCategory(catalogNumber, *categoryName, domain);
    }
}


void
StarDatabaseBuilder::addCategory(AstroCatalog::IndexNumber catalogNumber,
                                 const std::string& name,
                                 const std::string& domain)
{
    auto category = UserCategory::findOrAdd(name, domain);
    if (category == UserCategoryId::Invalid)
        return;

    auto [start, end] = categories.equal_range(catalogNumber);
    if (start == end)
    {
        categories.emplace(catalogNumber, category);
        return;
    }

    if (std::any_of(start, end, [category](const auto& it) { return it.second == category; }))
        return;

    categories.emplace_hint(end, catalogNumber, category);
}


/*! While loading the star catalogs, this function must be called instead of
 *  find(). The final catalog number index for stars cannot be built until
 *  after all stars have been loaded. During catalog loading, there are two
 *  separate indexes: one for the binary catalog and another index for stars
 *  loaded from stc files. They binary catalog index is a sorted array, while
 *  the stc catalog index is an STL map. Since the binary file can be quite
 *  large, we want to avoid creating a map with as many nodes as there are
 *  stars. Stc files should collectively contain many fewer stars, and stars
 *  in an stc file may reference each other (barycenters). Thus, a dynamic
 *  structure like a map is both practical and essential.
 */
Star*
StarDatabaseBuilder::findWhileLoading(AstroCatalog::IndexNumber catalogNumber) const
{
    // First check for stars loaded from the binary database
    if (auto star = std::lower_bound(binFileCatalogNumberIndex.cbegin(), binFileCatalogNumberIndex.cend(),
                                     catalogNumber,
                                     [](const Star* star, AstroCatalog::IndexNumber catNum) { return star->getIndex() < catNum; });
        star != binFileCatalogNumberIndex.cend() && (*star)->getIndex() == catalogNumber)
        return *star;

    // Next check for stars loaded from an stc file
    auto iter = stcFileCatalogNumberIndex.find(catalogNumber);
    if (iter != stcFileCatalogNumberIndex.end())
        return iter->second;

    // Star not found
    return nullptr;
}


void StarDatabaseBuilder::buildOctree()
{
    // This should only be called once for the database
    // ASSERT(octreeRoot == nullptr);

    GetLogger()->debug("Sorting stars into octree . . .\n");
    float absMag = astro::appToAbsMag(STAR_OCTREE_MAGNITUDE,
                                      STAR_OCTREE_ROOT_SIZE * celestia::numbers::sqrt3_v<float>);
    DynamicStarOctree* root = new DynamicStarOctree(Eigen::Vector3f(1000.0f, 1000.0f, 1000.0f),
                                                    absMag);
    for (unsigned int i = 0; i < unsortedStars.size(); ++i)
    {
        root->insertObject(unsortedStars[i], STAR_OCTREE_ROOT_SIZE);
    }

    GetLogger()->debug("Spatially sorting stars for improved locality of reference . . .\n");
    Star* sortedStars    = new Star[starDB->nStars];
    Star* firstStar      = sortedStars;
    root->rebuildAndSort(starDB->octreeRoot, firstStar);

    // ASSERT((int) (firstStar - sortedStars) == nStars);
    GetLogger()->debug("{} stars total\nOctree has {} nodes and {} stars.\n",
                       firstStar - sortedStars,
                       1 + starDB->octreeRoot->countChildren(), starDB->octreeRoot->countObjects());
#ifdef PROFILE_OCTREE
    vector<OctreeLevelStatistics> stats;
    octreeRoot->computeStatistics(stats);
    int level = 0;
    for (const auto& stat : stats)
    {
        level++;
        clog << fmt::format(
                     "Level {}, {:.5f} ly, {} nodes, {} stars\n",
                     level,
                     STAR_OCTREE_ROOT_SIZE / pow(2.0, (double) level),
                     stat.nodeCount,
                     stat.objectCount;
    }
#endif

    // Clean up . . .
    //delete[] stars;
    unsortedStars.clear();
    delete root;

    starDB->stars = sortedStars;
}


void StarDatabaseBuilder::buildIndexes()
{
    // This should only be called once for the database
    // assert(catalogNumberIndexes[0] == nullptr);

    GetLogger()->info("Building catalog number indexes . . .\n");

    starDB->catalogNumberIndex.clear();
    starDB->catalogNumberIndex.reserve(starDB->nStars);
    for (std::uint32_t i = 0; i < starDB->nStars; ++i)
        starDB->catalogNumberIndex.push_back(&starDB->stars[i]);

    std::sort(starDB->catalogNumberIndex.begin(), starDB->catalogNumberIndex.end(),
              [](const Star* star0, const Star* star1) { return star0->getIndex() < star1->getIndex(); });
}
