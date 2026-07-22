// multiplescatteringlut.h
//
// Copyright (C) 2026, Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <memory>

struct Atmosphere;

namespace celestia::render
{

class MultipleScatteringLut
{
public:
    MultipleScatteringLut();
    ~MultipleScatteringLut();
    MultipleScatteringLut(const MultipleScatteringLut&) = delete;
    MultipleScatteringLut(MultipleScatteringLut&&) = delete;
    MultipleScatteringLut& operator=(const MultipleScatteringLut&) = delete;
    MultipleScatteringLut& operator=(MultipleScatteringLut&&) = delete;

    bool bind(const Atmosphere& atmosphere,
              float planetRadius,
              float atmosphereHeight,
              unsigned int textureUnit);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace celestia::render
