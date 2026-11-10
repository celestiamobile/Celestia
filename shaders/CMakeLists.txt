file(GLOB SHADERS_SOURCES *.glsl)

install(
  FILES ${SHADERS_SOURCES}
        "${PROJECT_SOURCE_DIR}/thirdparty/fsr1/ffx_fsr1.h"
        "${PROJECT_SOURCE_DIR}/thirdparty/fsr1/LICENSE.txt"
  DESTINATION "${DATADIR}/shaders"
  COMPONENT core
)
