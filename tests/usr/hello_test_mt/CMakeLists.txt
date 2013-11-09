
add_definitions(
  -D_REENTRANT
)
 
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error=padded -Wno-error=unused-but-set-variable -Wall -Werror -Wpadded")

include_directories(
  "${PROJECT_SOURCE_DIR}/include"
  "."
  )

set(common_srcs
  xio_msg.c
  )

set(xio_mt_client_srcs
  xio_mt_client.c
  )

set(xio_mt_server_srcs
  xio_mt_server.c
  )

add_executable(xio_mt_server
  ${common_srcs}
  ${xio_mt_server_srcs}
  )

target_link_libraries(xio_mt_server
  "${PROJECT_BINARY_DIR}/src/usr/libxio.a"
  ibverbs rdmacm pthread rt
  )

add_executable(xio_mt_client
  ${common_srcs}
  ${xio_mt_client_srcs}
  )

target_link_libraries(xio_mt_client
  "${PROJECT_BINARY_DIR}/src/usr/libxio.a"
  ibverbs rdmacm pthread rt
  )
