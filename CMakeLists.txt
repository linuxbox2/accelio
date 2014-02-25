cmake_minimum_required(VERSION 2.8.7)

project(libxio C)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} 
  "${CMAKE_SOURCE_DIR}/cmake/modules/")

include_directories(  
  ${PROJECT_SOURCE_DIR}/src/usr/rdma
  ${PROJECT_BINARY_DIR}/include
  ${OFED_PREFIX}/include
)

link_directories(
  ${OFED_PREFIX}/lib
)

#Check Includes
include(CheckIncludeFile)

SET(CMAKE_REQUIRED_INCLUDES ${CMAKE_REQUIRED_INCLUDES}
  "${OFED_PREFIX}/include")

CHECK_INCLUDE_FILE("infiniband/verbs.h" HAVE_VERBS_H)
CHECK_INCLUDE_FILE("rdma/rdma_cma.h" HAVE_RDMA_CMA_H)

find_library(IBVERBS, ibverbs, "${OFED_PREFIX}/lib")

# require verbs headers
#if((NOT HAVE_VERBS_H) OR (NOT HAVE_RDMA_CMA_H))
#  message(FATAL_ERROR "Unable to find OFED header files")
#endif((NOT HAVE_VERBS_H) OR (NOT HAVE_RDMA_CMA_H))

option(DEBUG "enable debug build" OFF)

if(DEBUG)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g3 -ggdb -gdwarf-4 -Wall -Werror -O0 -D_GNU_SOURCE")
else()
  set(CMAKE_C_FLAGS "-g -ggdb -Wall -Werror -Wpadded -O3 -D_GNU_SOURCE")
endif(DEBUG)

add_subdirectory(src/usr)
add_subdirectory(tests/usr)
add_subdirectory(examples/usr)
add_subdirectory(include)
