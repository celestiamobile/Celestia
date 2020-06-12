#include "glsupport.h"
#include <algorithm>
#include <cstring>

namespace celestia::gl
{
static int
epoxy_gl_version(void);
static bool
epoxy_has_gl_extension(const char *ext);

#ifdef GL_ES
CELAPI bool OES_vertex_array_object        = false;
CELAPI bool OES_texture_border_clamp       = false;
CELAPI bool OES_geometry_shader            = false;
#else
CELAPI bool ARB_vertex_array_object        = false;
CELAPI bool ARB_framebuffer_object         = false;
#endif
CELAPI bool ARB_shader_texture_lod         = false;
CELAPI bool EXT_texture_compression_s3tc   = false;
CELAPI bool EXT_texture_filter_anisotropic = false;
CELAPI bool MESA_pack_invert               = false;
CELAPI GLint maxPointSize                  = 0;
CELAPI GLint maxTextureSize                = 0;
CELAPI GLfloat maxLineWidth                = 0.0f;
CELAPI GLint maxTextureAnisotropy          = 0;

namespace
{

bool EnableGeomShaders = true;

inline bool has_extension(const char *name) noexcept
{
    return epoxy_has_gl_extension(name);
}

bool check_extension(util::array_view<std::string> list, const char *name) noexcept
{
    return std::find(list.begin(), list.end(), std::string(name)) == list.end()
           && has_extension(name);
}

void enable_workarounds()
{
    bool isMesa = false;
    bool isAMD = false;
    bool isNavi = false;

    const char* s;
    s = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    // "4.6 (Compatibility Profile) Mesa 22.3.6"
    // "OpenGL ES 3.2 Mesa 22.3.6"
    if (s != nullptr)
        isMesa = std::strstr(s, "Mesa") != nullptr;

    s = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    // "AMD" for radeonsi
    // "Mesa/X.org" for llvmpipe
    // "Collabora Ltd" for zink
    if (s != nullptr)
        isAMD = std::strcmp(s, "AMD") == 0;

    s = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    // "AMD Radeon RX 6600 (navi23, LLVM 15.0.6, DRM 3.52, 6.4.0-0.deb12.2-amd64)"" for radeonsi
    // "llvmpipe (LLVM 15.0.6, 256 bits)""
    // "zink (llvmpipe (LLVM 15.0.6, 256 bits))""
    // "zink (AMD Radeon RX 6600 (RADV NAVI23))""
    if (s != nullptr)
        isNavi = std::strstr(s, "navi") != nullptr;

    // https://gitlab.freedesktop.org/mesa/mesa/-/issues/9971
    if (isMesa && isAMD && isNavi)
        EnableGeomShaders = false;
}

} // namespace

bool init(util::array_view<std::string> ignore) noexcept
{
#ifdef GL_ES
    OES_vertex_array_object        = check_extension(ignore, "GL_OES_vertex_array_object");
    OES_texture_border_clamp       = check_extension(ignore, "GL_OES_texture_border_clamp") || check_extension(ignore, "GL_EXT_texture_border_clamp");
    OES_geometry_shader            = check_extension(ignore, "GL_OES_geometry_shader") || check_extension(ignore, "GL_EXT_geometry_shader");
#else
    ARB_vertex_array_object        = check_extension(ignore, "GL_ARB_vertex_array_object");
    ARB_framebuffer_object         = check_extension(ignore, "GL_ARB_framebuffer_object") || check_extension(ignore, "GL_EXT_framebuffer_object");
#endif
    ARB_shader_texture_lod         = check_extension(ignore, "GL_ARB_shader_texture_lod");
    EXT_texture_compression_s3tc   = check_extension(ignore, "GL_EXT_texture_compression_s3tc");
    EXT_texture_filter_anisotropic = check_extension(ignore, "GL_EXT_texture_filter_anisotropic") || check_extension(ignore, "GL_ARB_texture_filter_anisotropic");
    MESA_pack_invert               = check_extension(ignore, "GL_MESA_pack_invert");

    GLint pointSizeRange[2];
    GLfloat lineWidthRange[2];
#ifdef GL_ES
    glGetIntegerv(GL_ALIASED_POINT_SIZE_RANGE, pointSizeRange);
    glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, lineWidthRange);
#else
    glGetIntegerv(GL_SMOOTH_POINT_SIZE_RANGE, pointSizeRange);
    glGetFloatv(GL_SMOOTH_LINE_WIDTH_RANGE, lineWidthRange);
#endif
    maxPointSize = pointSizeRange[1];
    maxLineWidth = lineWidthRange[1];

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);

    if (gl::EXT_texture_filter_anisotropic)
        glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxTextureAnisotropy);

    enable_workarounds();

    return true;
}

bool checkVersion(int v) noexcept
{
    static int version = 0;
    if (version == 0)
        version = epoxy_gl_version(); // this function always queries GL
    return version >= v;
}

bool hasGeomShader() noexcept
{
#ifdef GL_ES
    return EnableGeomShaders && checkVersion(celestia::gl::GLES_3_2);
#else
    return EnableGeomShaders && checkVersion(celestia::gl::GL_3_2);
#endif
}

void enableGeomShaders() noexcept
{
    EnableGeomShaders = true;
}

void disableGeomShaders() noexcept
{
    EnableGeomShaders = false;
}

// Taken from libepoxy
static int
epoxy_internal_gl_version(GLenum version_string, int error_version, int factor);
static bool
epoxy_internal_has_gl_extension(const char *ext, bool invalid_op_mode);

static int
epoxy_gl_version(void)
{
    return epoxy_internal_gl_version(GL_VERSION, 0, 10);
}

bool
epoxy_has_gl_extension(const char *ext)
{
    return epoxy_internal_has_gl_extension(ext, false);
}

static int
epoxy_internal_gl_version(GLenum version_string, int error_version, int factor)
{
    const char *version = (const char *)glGetString(version_string);
    GLint major, minor;
    int scanf_count;

    if (!version)
        return error_version;

    /* skip to version number */
    while (!isdigit(*version) && *version != '\0')
        version++;

    /* Interpret version number */
    scanf_count = sscanf(version, "%i.%i", &major, &minor);
    if (scanf_count != 2) {
        fprintf(stderr, "Unable to interpret GL_VERSION string: %s\n",
                version);
        abort();
    }

    return factor * major + minor;
}

static bool
epoxy_extension_in_string(const char *extension_list, const char *ext)
{
    const char *ptr = extension_list;
    int len;

    if (!ext)
        return false;

    len = strlen(ext);

    if (extension_list == NULL || *extension_list == '\0')
        return false;

    /* Make sure that don't just find an extension with our name as a prefix. */
    while (true) {
        ptr = strstr(ptr, ext);
        if (!ptr)
            return false;

        if (ptr[len] == ' ' || ptr[len] == 0)
            return true;
        ptr += len;
    }
}

static bool
epoxy_internal_has_gl_extension(const char *ext, bool invalid_op_mode)
{
    if (epoxy_gl_version() < 30) {
        const char *exts = (const char *)glGetString(GL_EXTENSIONS);
        if (!exts)
            return invalid_op_mode;
        return epoxy_extension_in_string(exts, ext);
    } else {
        int num_extensions;
        int i;

        glGetIntegerv(GL_NUM_EXTENSIONS, &num_extensions);
        if (num_extensions == 0)
            return invalid_op_mode;

        for (i = 0; i < num_extensions; i++) {
            const char *gl_ext = (const char *)glGetStringi(GL_EXTENSIONS, i);
            if (!gl_ext)
                return false;
            if (strcmp(ext, gl_ext) == 0)
                return true;
        }

        return false;
    }
}
} // end namespace celestia::gl
