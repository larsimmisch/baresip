project(debug_cmd)

set(CMAKE_BUILD_TYPE Debug)

set(SRCS debug_cmd.c)

if(STATIC)
    add_library(${PROJECT_NAME} OBJECT ${SRCS})
else()
    add_library(${PROJECT_NAME} MODULE ${SRCS})
endif()

