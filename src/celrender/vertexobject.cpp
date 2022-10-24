// vertexobject.cpp
//
// Copyright (C) 2019-present, the Celestia Development Team
//
// VBO/VAO wrappper class. Currently GL2/GL2+VAO only.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include <cassert>
#include "vertexobject.h"

namespace
{
[[ nodiscard ]] inline bool isVAOSupported()
{
#ifndef GL_ES
    return celestia::gl::ARB_vertex_array_object;
#else
    return celestia::gl::OES_vertex_array_object;
#endif
}
}

namespace celestia::render
{
VertexObject::VertexObject(GLenum bufferType) :
    m_bufferType(bufferType)
{
}

VertexObject::VertexObject(GLenum bufferType, GLsizeiptr bufferSize, GLenum streamType) :
    m_bufferSize(bufferSize),
    m_bufferType(bufferType),
    m_streamType(streamType)
{
}

VertexObject::~VertexObject()
{
    if (m_vaoId != 0 && isVAOSupported())
        glDeleteVertexArrays(1, &m_vaoId);

    if (m_vboId != 0)
        glDeleteBuffers(1, &m_vboId);
}

void VertexObject::bind() noexcept
{
    if ((m_state & State::Initialize) != 0)
    {
        if (isVAOSupported())
        {
            glGenVertexArrays(1, &m_vaoId);
            glBindVertexArray(m_vaoId);
        }
        glGenBuffers(1, &m_vboId);
        glBindBuffer(m_bufferType, m_vboId);
    }
    else
    {
        if (isVAOSupported())
        {
            glBindVertexArray(m_vaoId);
            if ((m_state & State::Update) != 0)
                glBindBuffer(m_bufferType, m_vboId);
        }
        else
        {
            glBindBuffer(m_bufferType, m_vboId);
            enableAttribArrays();
        }
    }
}

void VertexObject::bindWritable() noexcept
{
    m_state |= State::Update;
    bind();
}

void VertexObject::unbind() noexcept
{
    if (isVAOSupported())
    {
        if ((m_state & (State::Initialize | State::Update)) != 0)
            glBindBuffer(m_bufferType, 0);
        glBindVertexArray(0);
    }
    else
    {
        disableAttribArrays();
        glBindBuffer(m_bufferType, 0);
    }
    m_state = State::NormalState;
}

void VertexObject::allocate(const void* data) const noexcept
{
    glBufferData(m_bufferType, m_bufferSize, data, m_streamType);
}

void VertexObject::allocate(GLsizeiptr bufferSize, const void* data) noexcept
{
    m_bufferSize = bufferSize;
    allocate(data);
}

void VertexObject::allocate(GLenum bufferType, GLsizeiptr bufferSize, const void* data, GLenum streamType) noexcept
{
    m_bufferType = bufferType;
    m_bufferSize = bufferSize;
    m_streamType = streamType;
    allocate(data);
}

void VertexObject::setBufferData(const void* data, GLintptr offset, GLsizeiptr size) const noexcept
{
    glBufferSubData(m_bufferType, offset, size == 0 ? m_bufferSize : size, data);
}

void VertexObject::draw(GLenum primitive, GLsizei count, GLint first) const noexcept
{
    if ((m_state & State::Initialize) != 0)
        enableAttribArrays();

    glDrawArrays(primitive, first, count);
}

struct VertexObject::PtrParams
{
    PtrParams(GLint location, GLsizeiptr offset, GLsizei stride, GLint count, GLenum type, bool normalized) :
        location(location), offset(offset), stride(stride), count(count), type(type), normalized(normalized)
    {}
    GLint      location;
    GLsizeiptr offset;
    GLsizei    stride;
    GLint      count;
    GLenum     type;
    bool       normalized;
};

void VertexObject::enableAttribArrays() const noexcept
{
    for (const auto& p : m_attribParams)
    {
        glEnableVertexAttribArray(p.location);
        glVertexAttribPointer(p.location, p.count, p.type, p.normalized, p.stride, (GLvoid*) p.offset);
    }
}

void VertexObject::disableAttribArrays() const noexcept
{
    for (const auto& p : m_attribParams)
        glDisableVertexAttribArray(p.location);
}

void VertexObject::setVertexAttribArray(GLint location, GLint count, GLenum type, bool normalized, GLsizei stride, GLsizeiptr offset)
{
    assert(location >= 0);
    m_attribParams.emplace_back(location, offset, stride, count, type, normalized);
}
} // namespace
