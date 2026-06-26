if(NOT ENABLE_QT6)
  message(STATUS "Qt6 frontend is disabled.")
  return()
endif()

include(../qt/CelestiaQtCommon.cmake)

# Instruct CMake to run moc automatically when needed
set(CMAKE_AUTOMOC ON)
# Create code from a list of Qt designer ui files
set(CMAKE_AUTOUIC ON)

find_package(Qt6 COMPONENTS Core Widgets OpenGLWidgets CONFIG REQUIRED)
qt6_add_resources(CELQT6_RC_SRC "../qt/icons.qrc")

GetQtSources(CELQT6_SOURCES)

add_executable(celestia-qt6 WIN32 ${CELQT6_SOURCES} ${CELQT6_RC_SRC})
add_dependencies(celestia-qt6 celestia)
target_link_libraries(celestia-qt6 PUBLIC Qt6::Widgets Qt6::OpenGLWidgets celestia)

if(ENABLE_STEAM)
  find_package(Steamworks REQUIRED)
  target_link_libraries(celestia-qt6 PRIVATE Steamworks::Steamworks)
  target_compile_definitions(celestia-qt6 PRIVATE USE_STEAM)
  if(WIN32 AND Steamworks_RUNTIME_LIBRARY)
    add_custom_command(TARGET celestia-qt6 POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${Steamworks_RUNTIME_LIBRARY}"
        "$<TARGET_FILE_DIR:celestia-qt6>"
    )
    install(FILES "${Steamworks_RUNTIME_LIBRARY}"
      DESTINATION ${CMAKE_INSTALL_BINDIR}
      COMPONENT qt6gui
    )
  endif()
endif()

if(ENABLE_SENTRY)
  find_package(sentry CONFIG REQUIRED)
  target_link_libraries(celestia-qt6 PRIVATE sentry::sentry)
  target_compile_definitions(celestia-qt6 PRIVATE
    USE_SENTRY
    CELESTIA_SENTRY_DSN="${SENTRY_DSN}"
    CELESTIA_SENTRY_RELEASE="celestia@${PROJECT_VERSION}+${SENTRY_BUILD_NUMBER}"
  )

  # The crashpad backend runs out-of-process, so crashpad_handler(.exe) must be
  # shipped next to celestia-qt6 and located at runtime (see sentryintegration).
  # Hints cover both the from-source install (<root>/lib/cmake/sentry) and the
  # vcpkg layout (<root>/share/sentry, handler under tools/sentry-native).
  find_program(SENTRY_CRASHPAD_HANDLER
    NAMES crashpad_handler
    HINTS
      "${sentry_DIR}/../../../bin"
      "${sentry_DIR}/../../bin"
      "${sentry_DIR}/../../tools/sentry-native"
    NO_DEFAULT_PATH
  )
  if(SENTRY_CRASHPAD_HANDLER)
    add_custom_command(TARGET celestia-qt6 POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SENTRY_CRASHPAD_HANDLER}"
        "$<TARGET_FILE_DIR:celestia-qt6>"
    )
    install(PROGRAMS "${SENTRY_CRASHPAD_HANDLER}"
      DESTINATION ${CMAKE_INSTALL_BINDIR}
      COMPONENT qt6gui
    )
  endif()

  # Ship the sentry runtime library when built as a shared object.
  get_target_property(_sentry_type sentry::sentry TYPE)
  if(_sentry_type STREQUAL "SHARED_LIBRARY")
    add_custom_command(TARGET celestia-qt6 POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:sentry::sentry>"
        "$<TARGET_FILE_DIR:celestia-qt6>"
    )
    install(FILES "$<TARGET_FILE:sentry::sentry>"
      DESTINATION ${CMAKE_INSTALL_BINDIR}
      COMPONENT qt6gui
    )
  endif()
endif()

set_target_properties(celestia-qt6 PROPERTIES CXX_VISIBILITY_PRESET hidden)

if(APPLE)
  set_property(TARGET celestia-qt6 APPEND_STRING PROPERTY LINK_FLAGS " -framework CoreFoundation")
  set_property(TARGET celestia-qt6 APPEND_STRING PROPERTY LINK_FLAGS " -framework CoreServices")
endif()

if(WIN32)
  install(
    TARGETS celestia-qt6
    RUNTIME_DEPENDENCIES
      PRE_EXCLUDE_REGEXES "^api-ms" "^ext-ms-"
      POST_EXCLUDE_REGEXES ".*system32/.*\\.dll$"
      DIRECTORIES $<TARGET_FILE_DIR:celestia> $<TARGET_FILE_DIR:Qt6::Core>
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT qt6gui
  )
else()
  install(
    TARGETS celestia-qt6
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT qt6gui
  )
endif()

if(ENABLE_LTO)
  set_target_properties(celestia-qt6 PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)
endif()

if(WIN32)
  qt6_generate_deploy_app_script(TARGET celestia-qt6
    OUTPUT_SCRIPT deploy-celestia-qt6
    NO_UNSUPPORTED_PLATFORM_ERROR
    NO_COMPILER_RUNTIME
    NO_TRANSLATIONS
  )
  install(
    SCRIPT ${deploy-celestia-qt6}
    COMPONENT qt6gui
  )
endif()

add_subdirectory(data)
