cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBAPE_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_ape.cpp
)

SET(LIBAPE_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libmac.a
)

SET(LIBAPE_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libape
        "${LIBAPE_SRC}"
        "${LIBAPE_LIB}"
        []
        ""
        "${LIBAPE_INC}"
        ""
        "1")
