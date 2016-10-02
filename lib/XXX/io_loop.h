#ifndef XXX_IOLOOP_H
#define XXX_IOLOOP_H

#include "XXX/types.h"

#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <list>
#include <memory>
#include <set>
#include <future>

#include <uv.h>

namespace XXX {

class kernel;
struct logger;
class io_loop;
class io_handle;
struct io_request;



typedef std::function<void(int port, std::unique_ptr<io_handle>)> socket_accept_cb;
typedef std::function<void(io_handle*, int)> tcp_connect_cb;

// TODO: comment
void version_check_libuv(int uv_major, int uv_minor);

struct uv_handle_data
{
  enum { DATA_CHECK = 0x5555555555555555 };
  uint64_t check;

  union {
    io_handle * io_handle_ptr;
  };
  enum ptr_type {
    io_handle_tcp,
    io_handle_async
  } type;

  uv_handle_data(ptr_type t, void* ptr)
    : check( DATA_CHECK ),
      io_handle_ptr(nullptr),
      type(t)
  {
    switch (t)
    {
      case io_handle_tcp :
      case io_handle_async :
        io_handle_ptr = (io_handle*) ptr;
        break;
    }
  }


};

struct  tcp_server
{
  uv_tcp_t uvh;
  int port;
  io_loop * ioloop;
  socket_accept_cb cb;
};

/* IO Thread */
class io_loop
{
public:
  io_loop(kernel&);
  ~io_loop();

  void stop();

  void on_async();


  void add_passive_handle(tcp_server* server, io_handle* iohandle);

  void add_server(int port, std::promise<int> listener_err, socket_accept_cb);

  uv_tcp_t*  connect(std::string addr,
                     std::string port,
                     bool resolve_hostname,
                     std::function<void()> on_success,
                     std::function<void(std::exception_ptr)> on_failure);

  void cancel_connect(uv_tcp_t*);

  uv_loop_t* uv_loop() { return m_uv_loop; }

  logger & get_logger() const { return __logger; }
  kernel & get_kernel() const { return m_kernel; }

private:

  void run_loop();

  void create_tcp_server_socket(int port, socket_accept_cb cb,
                                std::unique_ptr< std::promise<int> > );

  void on_tcp_connect_cb(uv_connect_t* __req, int status);

  void push_request(std::unique_ptr<io_request>);

  kernel & m_kernel;
  struct logger & __logger;
  uv_loop_t*   m_uv_loop;
  std::unique_ptr<uv_async_t> m_async;

  std::vector< std::unique_ptr<io_request> > m_pending_requests;
  std::mutex                                 m_pending_requests_lock;


  enum PendingFlags
  {
    eNone  = 0x00,
    eFinal = 0x01
  };

  std::list< std::unique_ptr<tcp_server> > m_server_handles;
  std::thread  m_thread; // should be final member
};

} // namespace XXX


#endif
