
add_definitions(
  -DPIC
  -D_REENTRANT
)
 
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC -Wno-error=padded -Wno-error=unused-but-set-variable -Wall -Werror -Wpadded")

include_directories(
  "${PROJECT_SOURCE_DIR}/include"
  "../common"
  "."
)

set (libxio_srcs
  xio_init.c
  get_clock.c
  xio_rdma_mempool.c
  xio_rdma_verbs.c
  xio_rdma_management.c
  xio_rdma_datapath.c
  xio_ev_loop.c
  xio_log.c
  xio_mem.c
  xio_task.c
  xio_context.c
  linux/hexdump.c
  ../common/xio_options.c
  ../common/xio_error.c
  ../common/xio_utils.c
  ../common/xio_server.c
  ../common/xio_session.c
  ../common/xio_sessions_store.c
  ../common/xio_conn.c
  ../common/xio_conns_store.c
  ../common/xio_transport.c
  ../common/xio_connection.c
)

add_library(libxio STATIC
  ${libxio_srcs}
)

install(TARGETS libxio DESTINATION lib)
