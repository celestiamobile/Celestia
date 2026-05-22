file(GLOB FONTS_SOURCES "*.ttf" "*.ttc")

install(
  FILES ${FONTS_SOURCES}
  DESTINATION "${DATADIR}/fonts"
  COMPONENT core
)
