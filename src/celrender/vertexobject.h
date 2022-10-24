// vertexobject.h
//
// Copyright (C) 2019-present, the Celestia Development Team
//
// VBO/VAO wrappper class. Currently GL2/GL2+VAO only.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <celengine/glsupport.h>
#include <vector>

namespace celestia::render
{
/**
 * \class VertexObject vertexobject.h celrender/vertexobject.h
 *
 * @brief Provides abstraction over GL Vertex Buffer Object and Vertex Array Object.
 *
 * Worflow:
 *   - initial steps
 *       1. create vo
 *       2. vo.bind()
 *       3. vo.setVertexAttribArray()
 *       4. vo.allocate(data pointer)
 *   - on the next frames
 *       -# static buffers
 *            1. vo.bind()
 *            2. vo.draw()
 *       -# dynamic buffers
 *            1. vo.bindWritable()
 *            2. <em>(optionaly)</em> vo.allocate(nullptr)
 *            3. vo.setBufferData()
 *            4. vo.draw()
 */
class VertexObject
{
 public:
    VertexObject(const VertexObject&) = delete;
    VertexObject(VertexObject&&) = delete;
    VertexObject& operator=(const VertexObject&) = delete;
    VertexObject& operator=(VertexObject&&) = delete;

    VertexObject() = default;

    /**
     * @brief Construct a new VertexObject.
     *
     * @param bufferType Currently only GL_ARRAY_BUFFER is supported.
     */
    explicit VertexObject(GLenum bufferType);

    /**
     * @brief Construct a new VertexObject.
     *
     * @param bufferType Currently only GL_ARRAY_BUFFER is supported.
     * @param bufferSize Buffer size in bytes.
     * @param streamType Buffer update policy: GL_STATIC_DRAW, GL_DYNAMIC_DRAW, GL_STREAM_DRAW.
     */
    VertexObject(GLenum bufferType, GLsizeiptr bufferSize, GLenum streamType);

    ~VertexObject();

    /**
     * @brief Bind the buffer to use.
     *
     * When the buffer is not initialized (just created) then after this call you can provide vertex
     * data and configuration. After that only drawing is allowed.
     */
    void bind() noexcept;

    /**
     * @brief Bind the buffer to update and draw.
     *
     * If the buffer's update policy is GL_DYNAMIC_DRAW or GL_STREAM_DRAW then use this call to
     * update data and draw.
     */
    void bindWritable() noexcept;

    //! Unbind the buffer (stop usage)
    void unbind() noexcept;

    /**
     * @brief Draw the buffer data
     *
     * @param primitive OpenGL primitive (GL_LINES, GL_TRIANGLES and so on).
     * @param count Number of vertices to draw.
     * @param first First vertice to draw.
     */
    void draw(GLenum primitive, GLsizei count, GLint first = 0) const noexcept;

    /**
     * @brief Allocate GPU buffer and (optionally) copy data.
     *
     * Allocate GPU buffer which size is defined by `bufferSize` parameter of constructor. If `data`
     * is equal to <em>nullptr</em> then just allocate a new GPU side memory.
     * @param data Pointer to CPU side memory buffer with vertex data.
     */
    void allocate(const void* data = nullptr) const noexcept;

    /**
     * @brief Allocate GPU buffer and copy data.
     *
     * Allocate GPU buffer which size is defined by `bufferSize` parameter of the method.
     *
     * @param bufferSize Buffer size in bytes.
     * @param data Pointer to CPU side memory buffer with vertex data.
     */
    void allocate(GLsizeiptr bufferSize, const void* data = nullptr) noexcept;

    /**
     * @brief Allocate GPU buffer and copy data.
     *
     * @param bufferType Currently only GL_ARRAY_BUFFER is supported.
     * @param bufferSize Buffer size in bytes.
     * @param data Pointer to CPU side memory buffer with vertex data.
     * @param streamType Buffer update policy: GL_STATIC_DRAW, GL_DYNAMIC_DRAW, GL_STREAM_DRAW.
     */
    void allocate(GLenum bufferType, GLsizeiptr bufferSize, const void* data, GLenum streamType) noexcept;

    /**
     * @brief Copy vertex data from a CPU buffer to GPU buffer.
     *
     * @param data Pointer to CPU side memory buffer with vertex data.
     * @param offset Offset in GPU memory to copy data to.
     * @param size Total number of bytes to copy.
     */
    void setBufferData(const void* data, GLintptr offset = 0, GLsizeiptr size = 0) const noexcept;

    /**
     * @brief Define an array of generic vertex attribute data.
     *
     * Define an array of generic vertex attribute data. See documentation for glVertexAttribPointer
     * OpenGL method for more information.
     *
     * @param location Specifies the index of the generic vertex attribute to be modified.
     * @param count Specifies the number of components per generic vertex attribute. Must be 1, 2,
     * 3, or 4.
     * @param type Specifies the data type of each component in the array.
     * @param normalized Specifies whether fixed-point data values should be normalized (true) or
     * converted directly as fixed-point values (false) when they are accessed.
     * @param stride Specifies the byte offset between consecutive generic vertex attributes. If
     * stride is 0, the generic vertex attributes are understood to be tightly packed in the array.
     * @param offset Specifies a pointer to the first component of the first generic vertex
     * attribute in the array.
     */
    void setVertexAttribArray(GLint location, GLint count, GLenum type, bool normalized = false, GLsizei stride = 0, GLsizeiptr offset = 0);

    //! Return the buffer's initialization state.
    inline bool initialized() const noexcept
    {
        return (m_state & State::Initialize) == 0;
    }
    //! Return the buffer's current type.
    GLenum getBufferType() const noexcept              { return m_bufferType; }

    //! Update the buffer's current type.
    void setBufferType(GLenum bufferType) noexcept     { m_bufferType = bufferType; }

    //! Return the buffer's current size.
    GLsizeiptr getBufferSize() const noexcept          { return m_bufferSize; }

    //! Update the buffer's current size.
    void setBufferSize(GLsizeiptr bufferSize) noexcept { m_bufferSize = bufferSize; }

    //! Return the buffer's current update policy.
    GLenum getStreamType() const noexcept              { return m_streamType; }

    //! Update the buffer's current update policy.
    void setStreamType(GLenum streamType) noexcept     { m_streamType = streamType; }

 private:
    enum State : uint16_t
    {
        NormalState = 0x0000,
        Initialize  = 0x0001,
        Update      = 0x0002
    };

    struct PtrParams;

    void enableAttribArrays() const noexcept;
    void disableAttribArrays() const noexcept;

    std::vector<PtrParams> m_attribParams;

    GLuint     m_vboId{ 0 };
    GLuint     m_vaoId{ 0 };

    GLsizeiptr m_bufferSize{ 0 };
    GLenum     m_bufferType{ 0 };
    GLenum     m_streamType{ 0 };

    uint16_t   m_state{ State::Initialize };
};
} // namespace
