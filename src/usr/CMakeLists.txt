
add_definitions(
  -DPIC
  -D_REENTRANT
)
 
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC -Wno-error=padded -Wno-error=unused-but-set-variable -Wall -Werror -Wpadded")

include_directories(
  "${PROJECT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/src/common"
  "${PROJECT_SOURCE_DIR}/src/usr/xio"
  "."
)

set (libxio_srcs
  rdma/xio_rdma_datapath.c
  rdma/xio_rdma_management.c
  rdma/xio_rdma_mempool.c
  rdma/xio_rdma_utils.c
  rdma/xio_rdma_verbs.c
  xio/get_clock.c
  xio/xio_context.c
  xio/xio_ev_loop.c
  xio/xio_init.c
  xio/xio_log.c
  xio/xio_mem.c
  xio/xio_schedwork.c
  xio/xio_task.c
  xio/xio_tls.c
  xio/xio_usr_utils.c
  linux/hexdump.c
  ../common/xio_conn.c
  ../common/xio_connection.c
  ../common/xio_conns_store.c
  ../common/xio_error.c
  ../common/xio_observer.c
  ../common/xio_options.c
  ../common/xio_server.c
  ../common/xio_session.c
  ../common/xio_session_client.c
  ../common/xio_session_server.c
  ../common/xio_sessions_store.c
  ../common/xio_transport.c
  ../common/xio_utils.c
)

add_library(xio STATIC
  ${libxio_srcs}
)

install(TARGETS xio DESTINATION lib)
