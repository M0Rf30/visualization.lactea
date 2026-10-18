cmake_minimum_required(VERSION 3.10)
project(visualization.lactea)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR})

find_package(Kodi REQUIRED)
find_package(lactea REQUIRED)

if(NOT WIN32 AND (APP_RENDER_SYSTEM STREQUAL "gl" OR NOT APP_RENDER_SYSTEM))
  find_package(OpenGl REQUIRED)
  set(DEPLIBS ${OPENGL_LIBRARIES})
  set(INCLUDES ${OPENGL_INCLUDE_DIR})
  add_definitions(${OPENGL_DEFINITIONS})
else()
  find_package(OpenGLES REQUIRED)
  set(DEPLIBS ${OPENGLES_LIBRARIES})
  set(INCLUDES ${OPENGLES_INCLUDE_DIR})
  add_definitions(${OPENGLES_DEFINITIONS})
endif()

list(APPEND DEPLIBS ${LACTEA_LIBRARIES})

include_directories(${INCLUDES}
                    ${LACTEA_INCLUDE_DIRS}
                    ${KODI_INCLUDE_DIR}/..) # Hack way with "/..", need bigger Kodi cmake rework to match right include ways

set(LACTEA_SOURCES src/Main.cpp src/LacteaPlayer.cpp)
set(LACTEA_HEADERS src/Main.h src/LacteaPlayer.h)

# A shared liblactea_capi that is not a system library is installed next to the
# addon binary; $ORIGIN lets the addon find it there.
set(LACTEA_ADDITIONAL_BINARY ${LACTEA_BUNDLE_BINARY})
set(CMAKE_INSTALL_RPATH "\$ORIGIN")

build_addon(visualization.lactea LACTEA DEPLIBS)

include(CPack)

# Preset-rotation test against a real lactea engine (needs a GPU; skipped with
# exit code 77 otherwise). Standalone builds only:
#   cmake -DLACTEA_BUILD_TESTS=ON ... && make && ctest --output-on-failure
option(LACTEA_BUILD_TESTS "Build the preset rotation test" OFF)
if(LACTEA_BUILD_TESTS)
  enable_testing()
  add_executable(preset_cycle tests/preset_cycle.cpp src/LacteaPlayer.cpp)
  target_compile_features(preset_cycle PRIVATE cxx_std_17)
  target_include_directories(preset_cycle PRIVATE src ${LACTEA_INCLUDE_DIRS})
  target_link_libraries(preset_cycle PRIVATE ${LACTEA_LIBRARIES})
  add_test(NAME preset_cycle COMMAND preset_cycle)
  set_tests_properties(preset_cycle PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()
