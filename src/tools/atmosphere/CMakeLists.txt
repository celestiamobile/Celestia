foreach(tool scattersim scattertable)
  add_executable(${tool} "${tool}.cpp")
  install(
    TARGETS ${tool}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT tools
  )
endforeach()

add_executable(brunetonbake
  brunetonbake.cpp
  bruneton/brunetonprecompute.cpp
  ${PROJECT_SOURCE_DIR}/src/celengine/brunetonatmospherefile.cpp
)
find_package(Threads REQUIRED)
target_include_directories(brunetonbake PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/bruneton
)
target_link_libraries(brunetonbake PRIVATE Threads::Threads)
install(
  TARGETS brunetonbake
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT tools
)
