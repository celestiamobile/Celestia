// unicode.h
//
// Copyright (C) 2023-present, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <string>
#include <icu.h>

namespace celestia::util
{

enum class ConversionOption : unsigned int
{
    None            = 0x00,
    ArabicShaping   = 0x01,
    BidiReordering  = 0x02,
};

bool UTF8StringToUnicodeString(std::string_view input, std::vector<UChar> &output);
bool UnicodeStringToWString(const std::vector<UChar> &input, std::wstring &output, ConversionOption options = ConversionOption::None);

}
