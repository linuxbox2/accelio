
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

set(xio_client_srcs
  xio_client.c
  )

set(xio_server_srcs
  xio_server.c
  )

add_executable(xio_server
  ${common_srcs}
  ${xio_server_srcs}
  )

target_link_libraries(xio_server
  "${PROJECT_BINARY_DIR}/src/usr/liblibxio.a"
  ibverbs rdmacm pthread rt
  )

add_executable(xio_client
  ${common_srcs}
  ${xio_client_srcs}
  )

target_link_libraries(xio_client
  "${PROJECT_BINARY_DIR}/src/usr/liblibxio.a"
  ibverbs rdmacm pthread rt
  )
