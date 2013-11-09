
add_definitions(
  -D_REENTRANT
)
 
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error=padded -Wno-error=unused-but-set-variable -Wall -Werror -Wpadded")

include_directories(
  "${PROJECT_SOURCE_DIR}/include"
  "."
  )

set(common_srcs
  ""
  )

set(xio_client_srcs
  xio_mt_client.c
  )

set(xio_server_srcs
  xio_mt_server.c
  )

add_executable(hello_world_server_mt
  ${common_srcs}
  ${xio_server_srcs}
  )

target_link_libraries(hello_world_server_mt
  "${PROJECT_BINARY_DIR}/src/usr/liblibxio.a"
  ibverbs rdmacm pthread rt
  )

add_executable(hello_world_client_mt
  ${common_srcs}
  ${xio_client_srcs}
  )

target_link_libraries(hello_world_client_mt
  "${PROJECT_BINARY_DIR}/src/usr/liblibxio.a"
  ibverbs rdmacm pthread rt
  )
