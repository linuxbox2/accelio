
add_definitions(
  -D_REENTRANT
)
 
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error=padded -Wno-error=unused-but-set-variable -Wall -Werror -Wpadded")

include_directories(
  "${PROJECT_SOURCE_DIR}/include"
  "."
  )

set(libraio_srcs
  msg_pool.c
  raio_utils.c
  raio_api.c
  )

add_library(libraio STATIC
  ${libraio_srcs}
)

set(raio_client_srcs
  raio_client.c
  get_clock.c
  )

set(raio_server_srcs
  raio_server.c
  raio_handlers.c
  raio_bs.c
  raio_bs_null.c
  raio_bs_aio.c
  )

add_executable(raio_server
  ${raio_server_srcs}
  )

target_link_libraries(raio_server
  "${PROJECT_BINARY_DIR}/src/usr/liblibxio.a"
  raio aio ibverbs rdmacm pthread rt
  )

add_executable(raio_client
  ${raio_client_srcs}
  )

target_link_libraries(raio_client
  "${PROJECT_BINARY_DIR}/src/usr/liblibxio.a"
  raio aio ibverbs rdmacm pthread rt
  )
