#ifdef DEBUG
#include <celutil/logger.h>
#endif
#include <celutil/gettext.h>
#include <celutil/greek.h>
#include <celutil/utf8.h>
#include "name.h"

std::uint32_t NameDatabase::getNameCount() const
{
    return nameIndex.size();
}

void
NameDatabase::add(const AstroCatalog::IndexNumber catalogNumber, std::string_view name)
{
    if (name.empty())
        return;

#ifdef DEBUG
    if (AstroCatalog::IndexNumber tmp = getCatalogNumberByName(name, false); tmp != AstroCatalog::InvalidIndex)
        celestia::util::GetLogger()->debug("Duplicated name '{}' on object with catalog numbers: {} and {}\n", name, tmp, catalogNumber);
#endif
    // Add the new name
    //nameIndex.insert(NameIndex::value_type(name, catalogNumber));
    std::string fname = ReplaceGreekLetterAbbr(name);

    nameIndex[fname] = catalogNumber;
    std::string lname = D_(fname.c_str());
    if (lname != fname)
        localizedNameIndex[lname] = catalogNumber;
    numberIndex.insert(NumberIndex::value_type(catalogNumber, fname));
}

void NameDatabase::erase(const AstroCatalog::IndexNumber catalogNumber)
{
    numberIndex.erase(catalogNumber);
}

AstroCatalog::IndexNumber NameDatabase::getCatalogNumberByName(std::string_view name, bool i18n) const
{
    auto iter = nameIndex.find(name);
    if (iter != nameIndex.end())
        return iter->second;

    if (i18n)
    {
        iter = localizedNameIndex.find(name);
        if (iter != localizedNameIndex.end())
            return iter->second;
    }

    auto replacedGreek = ReplaceGreekLetterAbbr(name);
    if (replacedGreek != name)
        return getCatalogNumberByName(replacedGreek, i18n);

    return AstroCatalog::InvalidIndex;
}

// Return the first name matching the catalog number or end()
// if there are no matching names.  The first name *should* be the
// proper name of the OBJ, if one exists. This requires the
// OBJ name database file to have the proper names listed before
// other designations.  Also, the STL implementation must
// preserve this order when inserting the names into the multimap
// (not certain whether or not this behavior is in the STL spec.
// but it works on the implementations I've tried so far.)
std::string NameDatabase::getNameByCatalogNumber(const AstroCatalog::IndexNumber catalogNumber) const
{
    if (catalogNumber == AstroCatalog::InvalidIndex)
        return "";

    NumberIndex::const_iterator iter = numberIndex.lower_bound(catalogNumber);

    if (iter != numberIndex.end() && iter->first == catalogNumber)
        return iter->second;

    return "";
}


// Return the first name matching the catalog number or end()
// if there are no matching names.  The first name *should* be the
// proper name of the OBJ, if one exists. This requires the
// OBJ name database file to have the proper names listed before
// other designations.  Also, the STL implementation must
// preserve this order when inserting the names into the multimap
// (not certain whether or not this behavior is in the STL spec.
// but it works on the implementations I've tried so far.)
NameDatabase::NumberIndex::const_iterator NameDatabase::getFirstNameIter(const AstroCatalog::IndexNumber catalogNumber) const
{
    NumberIndex::const_iterator iter = numberIndex.lower_bound(catalogNumber);

    if (iter == numberIndex.end() || iter->first != catalogNumber)
        return getFinalNameIter();
    else
        return iter;
}

NameDatabase::NumberIndex::const_iterator NameDatabase::getFinalNameIter() const
{
    return numberIndex.end();
}

void NameDatabase::getCompletion(std::vector<std::string>& completion, std::string_view name) const
{
    std::string name2 = ReplaceGreekLetter(name);
    for (const auto &[n, _] : nameIndex)
    {
        if (UTF8StartsWith(n, name2, true))
            completion.push_back(n);
    }

    for (const auto &[n, _] : localizedNameIndex)
    {
        if (UTF8StartsWith(n, name2, true))
            completion.push_back(n);
    }
}
