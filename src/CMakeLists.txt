
set(PLUGIN_NAME usdOpenAssetIOResolver)

set(
    SRC
    resolver.cpp
)

add_library(${PLUGIN_NAME}
    SHARED
    ${SRC}
)

target_link_libraries(${PLUGIN_NAME}
    PRIVATE
    ar
    OpenAssetIO::openassetio-core
    OpenAssetIO::openassetio-python-bridge
    OpenAssetIO-MediaCreation::openassetio-mediacreation
)

#-----------------------------------------------------------------------
# Activate warnings as errors, pedantic, etc.
openassetio_usdresolver_set_default_target_properties(${PLUGIN_NAME})

# ----------------------------------------------------------------------
# Silence warnings about deprecation from USD itself
# At time of writing, from : from USD/include/pxr/base/tf/hashset.h:39
# MSVC has no equivalent flag and rejects it.
if (NOT MSVC)
    target_compile_options(${PLUGIN_NAME} PRIVATE -Wno-deprecated)
endif ()

#-----------------------------------------------------------------------
# Main install
install(
    TARGETS
    ${PLUGIN_NAME}
    DESTINATION
    .
)

#-----------------------------------------------------------------------
# Generate and install plugInfo.json
# The library file name is platform specific (e.g. lib*.so, lib*.dylib,
# *.dll), so it is filled in from the built target.
file(
    GENERATE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json
    INPUT ${CMAKE_CURRENT_SOURCE_DIR}/resources/plugInfo.json.in
)
install(
    FILES
    ${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json
    DESTINATION
    ./resources
)
