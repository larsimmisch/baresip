project(vqueue)

set(SRCS aufile_play.c aufile_src.c)
set(CPP_SRCS vqueue.cpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(STATIC)
  add_library(${PROJECT_NAME} OBJECT ${SRCS} ${CPP_SRCS})
else()
  add_library(${PROJECT_NAME} MODULE ${SRCS} ${CPP_SRCS})
endif()
