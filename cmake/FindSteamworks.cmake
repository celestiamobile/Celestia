# FindSteamworks
# --------------
# Locate the Steamworks SDK headers and redistributable runtime library.
#
# Input variables / cache entries:
#   STEAMWORKS_SDK_ROOT     Path to the SDK root (containing public/ and
#                           redistributable_bin/). May also be set via the
#                           STEAMWORKS_SDK_ROOT environment variable.
#
# Output:
#   Steamworks::Steamworks       IMPORTED target with include dir + linkable lib.
#   Steamworks_INCLUDE_DIR       Header directory.
#   Steamworks_LIBRARY           Import library (.lib on Windows, .so/.dylib elsewhere).
#   Steamworks_RUNTIME_LIBRARY   Runtime shared library (DLL on Windows). Only
#                                set on platforms where it is distinct from the
#                                import library.

if(NOT STEAMWORKS_SDK_ROOT AND DEFINED ENV{STEAMWORKS_SDK_ROOT})
    set(STEAMWORKS_SDK_ROOT "$ENV{STEAMWORKS_SDK_ROOT}")
endif()

find_path(Steamworks_INCLUDE_DIR
    NAMES steam/steam_api.h
    HINTS "${STEAMWORKS_SDK_ROOT}/public"
)

if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_steamworks_lib_dir "${STEAMWORKS_SDK_ROOT}/redistributable_bin/win64")
        set(_steamworks_lib_name "steam_api64")
        set(_steamworks_dll_name "steam_api64.dll")
    else()
        set(_steamworks_lib_dir "${STEAMWORKS_SDK_ROOT}/redistributable_bin")
        set(_steamworks_lib_name "steam_api")
        set(_steamworks_dll_name "steam_api.dll")
    endif()
elseif(APPLE)
    set(_steamworks_lib_dir "${STEAMWORKS_SDK_ROOT}/redistributable_bin/osx")
    set(_steamworks_lib_name "steam_api")
elseif(UNIX)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_steamworks_lib_dir "${STEAMWORKS_SDK_ROOT}/redistributable_bin/linux64")
    else()
        set(_steamworks_lib_dir "${STEAMWORKS_SDK_ROOT}/redistributable_bin/linux32")
    endif()
    set(_steamworks_lib_name "steam_api")
endif()

find_library(Steamworks_LIBRARY
    NAMES ${_steamworks_lib_name}
    HINTS "${_steamworks_lib_dir}"
)

if(WIN32 AND _steamworks_dll_name)
    find_file(Steamworks_RUNTIME_LIBRARY
        NAMES ${_steamworks_dll_name}
        HINTS "${_steamworks_lib_dir}"
    )
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Steamworks
    REQUIRED_VARS Steamworks_LIBRARY Steamworks_INCLUDE_DIR
)

if(Steamworks_FOUND AND NOT TARGET Steamworks::Steamworks)
    add_library(Steamworks::Steamworks UNKNOWN IMPORTED)
    set_target_properties(Steamworks::Steamworks PROPERTIES
        IMPORTED_LOCATION "${Steamworks_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Steamworks_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(Steamworks_INCLUDE_DIR Steamworks_LIBRARY Steamworks_RUNTIME_LIBRARY)
