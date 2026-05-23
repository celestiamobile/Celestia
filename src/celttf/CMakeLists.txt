set(CELTTF_SOURCES
  fontshaper.h
  truetypefont.cpp
  truetypefont.h
)

if(APPLE)
  list(APPEND CELTTF_SOURCES fontshaper_apple.mm)
else()
  list(APPEND CELTTF_SOURCES fontshaper_embedded.cpp)
endif()

add_library(celttf OBJECT ${CELTTF_SOURCES})

if(APPLE)
  target_link_libraries(celttf PUBLIC
    "-framework CoreFoundation"
    "-framework CoreGraphics"
    "-framework CoreText"
  )
endif()
