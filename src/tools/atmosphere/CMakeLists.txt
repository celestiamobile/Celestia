foreach(tool scattersim scattertable)
  add_executable(${tool} "${tool}.cpp")
  install(
    TARGETS ${tool}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT tools
  )
endforeach()

find_package(Threads REQUIRED)

add_library(brunetonprecompute_core STATIC
  brunetonbaker.cpp
  brunetonbaker.h
  brunetoninput.cpp
  brunetoninput.h
  brunetonprecompute.cpp
  brunetonprecompute.h
  brunetonvec.h)
target_link_libraries(brunetonprecompute_core PUBLIC Threads::Threads)
target_include_directories(brunetonprecompute_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
  target_compile_options(brunetonprecompute_core PRIVATE /EHsc)
else()
  target_compile_options(brunetonprecompute_core PRIVATE -fexceptions)
endif()

add_executable(brunetonprecompute brunetonprecompute_main.cpp)
target_link_libraries(brunetonprecompute PRIVATE brunetonprecompute_core celestia)
install(
  TARGETS brunetonprecompute
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT tools
)
