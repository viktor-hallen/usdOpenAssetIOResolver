
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
# Optional URI scheme registration is filled in at configure time. The
# library file name is platform specific (e.g. lib*.so, lib*.dylib,
# *.dll), so it is filled in from the built target at generate time.
set(USDRESOLVER_URI_SCHEMES_JSON "")
if (OPENASSETIO_USDRESOLVER_URI_SCHEMES)
    set(_quoted_schemes)
    foreach (scheme IN LISTS OPENASSETIO_USDRESOLVER_URI_SCHEMES)
        # RFC 3986 scheme syntax, which USD also requires.
        if (NOT scheme MATCHES "^[A-Za-z][A-Za-z0-9+.-]*$")
            message(FATAL_ERROR "Invalid URI scheme in OPENASSETIO_USDRESOLVER_URI_SCHEMES: '${scheme}'")
        endif ()
        list(APPEND _quoted_schemes "\"${scheme}\"")
    endforeach ()
    list(JOIN _quoted_schemes ", " _joined_schemes)
    set(USDRESOLVER_URI_SCHEMES_JSON ",\n                        \"uriSchemes\": [${_joined_schemes}]")
endif ()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/plugInfo.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json.configured
    @ONLY
)
file(
    GENERATE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json
    INPUT ${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json.configured
)
install(
    FILES
    ${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json
    DESTINATION
    ./resources
)
