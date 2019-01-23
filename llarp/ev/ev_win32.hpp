#ifndef EV_WIN32_HPP
#define EV_WIN32_HPP

#include <ev/ev.hpp>

#include <net/net.h>
#include <net/net.hpp>
#include <util/buffer.h>
#include <util/logger.hpp>

#include <windows.h>
#include <process.h>

#include <cstdio>

// io packet for TUN read/write
struct asio_evt_pkt
{
  OVERLAPPED pkt = {
      0, 0, 0, 0, nullptr};  // must be first, since this is part of the IO call
  bool write = false;        // true, or false if read pkt
  size_t sz;  // should match the queued data size, if not try again?
  void* buf;  // must remain valid until we get notification; this is _supposed_
              // to be zero-copy
};

struct win32_tun_io;
extern "C" DWORD FAR PASCAL
tun_ev_loop(void* unused);
// list of TUN listeners (useful for exits or other nodes with multiple TUNs)
std::list< win32_tun_io* > tun_listeners;

// a single event queue for the TUN interface
HANDLE tun_event_queue =
    INVALID_HANDLE_VALUE;  // we pass this to the event loop thread procedure
                           // upon setup

// we hand the kernel our thread handles to process completion events
HANDLE* kThreadPool;

void
begin_tun_loop(int nThreads)
{
  kThreadPool = new HANDLE[nThreads];
  for(int i = 0; i < nThreads; ++i)
  {
    kThreadPool[i] =
        CreateThread(nullptr, 0, &tun_ev_loop, nullptr, 0, nullptr);
  }
  llarp::LogInfo("created ", nThreads, " threads for TUN event queue");
}

// A different kind of event loop,
// more suited for the native Windows NT
// event model
struct win32_tun_io
{
  llarp_tun_io* t;
  device* tunif;
  byte_t readbuf[EV_READ_BUF_SZ] = {0};

  struct WriteBuffer
  {
    llarp_time_t timestamp = 0;
    size_t bufsz;
    byte_t buf[EV_WRITE_BUF_SZ];

    WriteBuffer() = default;

    WriteBuffer(const byte_t* ptr, size_t sz)
    {
      if(sz <= sizeof(buf))
      {
        bufsz = sz;
        memcpy(buf, ptr, bufsz);
      }
      else
        bufsz = 0;
    }

    struct GetTime
    {
      llarp_time_t
      operator()(const WriteBuffer& buf) const
      {
        return buf.timestamp;
      }
    };

    struct GetNow
    {
      void* loop;
      GetNow(void* l) : loop(l)
      {
      }

      llarp_time_t
      operator()() const
      {
        return llarp::time_now_ms();
      }
    };

    struct PutTime
    {
      void* loop;
      PutTime(void* l) : loop(l)
      {
      }
      void
      operator()(WriteBuffer& buf)
      {
        buf.timestamp = llarp::time_now_ms();
      }
    };

    struct Compare
    {
      bool
      operator()(const WriteBuffer& left, const WriteBuffer& right) const
      {
        return left.timestamp < right.timestamp;
      }
    };
  };

  using LossyWriteQueue_t =
      llarp::util::CoDelQueue< WriteBuffer, WriteBuffer::GetTime,
                               WriteBuffer::PutTime, WriteBuffer::Compare,
                               WriteBuffer::GetNow, llarp::util::NullMutex,
                               llarp::util::NullLock, 5, 100, 128 >;

  std::unique_ptr< LossyWriteQueue_t > m_LossyWriteQueue;

  win32_tun_io(llarp_tun_io* tio) : t(tio), tunif(tuntap_init())
  {
    // This is not your normal everyday event loop, this is _advanced_ event
    // handling :>
    m_LossyWriteQueue = std::make_unique< LossyWriteQueue_t >("win32_tun_queue",
                                                              nullptr, nullptr);
  };

  bool
  queue_write(const byte_t* buf, size_t sz)
  {
    if(m_LossyWriteQueue)
    {
      m_LossyWriteQueue->Emplace(buf, sz);
      flush_write();
      return true;
    }
    else
      return false;
  }

  bool
  setup()
  {
    if(tuntap_start(tunif, TUNTAP_MODE_TUNNEL, 0) == -1)
    {
      llarp::LogWarn("failed to start interface");
      return false;
    }
    if(tuntap_set_ip(tunif, t->ifaddr, t->ifaddr, t->netmask) == -1)
    {
      llarp::LogWarn("failed to set ip");
      return false;
    }
    if(tuntap_up(tunif) == -1)
    {
      char ebuf[1024];
      int err = GetLastError();
      FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err, LANG_NEUTRAL,
                    ebuf, 1024, nullptr);
      llarp::LogWarn("failed to put interface up: ", ebuf);
      return false;
    }

    if(tunif->tun_fd == INVALID_HANDLE_VALUE)
      return false;

    return true;
  }

  // first TUN device gets to set up the event port
  bool
  add_ev()
  {
    if(tun_event_queue == INVALID_HANDLE_VALUE)
    {
      SYSTEM_INFO sys_info;
      GetSystemInfo(&sys_info);
      unsigned long numCPU = sys_info.dwNumberOfProcessors;
      // let the system handle 2x the number of CPUs or hardware
      // threads
      tun_event_queue = CreateIoCompletionPort(tunif->tun_fd, nullptr,
                                               (ULONG_PTR)this, numCPU * 2);
      begin_tun_loop(numCPU * 2);
    }
    else
      CreateIoCompletionPort(tunif->tun_fd, tun_event_queue, (ULONG_PTR)this,
                             0);

    // we're already non-blocking
    // add to list
    tun_listeners.push_back(this);
    read(readbuf, 4096);
    return true;
  }

  // places data in event queue for kernel to process
  void
  do_write(void* data, size_t sz)
  {
    llarp::LogInfo("writing some data");
    asio_evt_pkt* pkt = new asio_evt_pkt;
    pkt->buf          = data;
    pkt->sz           = sz;
    pkt->write        = true;
    memset(&pkt->pkt, '\0', sizeof(pkt->pkt));
    WriteFile(tunif->tun_fd, data, sz, nullptr, &pkt->pkt);
  }

  // we call this one when we get a packet in the event port
  // which then kicks off another write
  void
  flush_write()
  {
    if(t->before_write)
      t->before_write(t);
    m_LossyWriteQueue->Process([&](WriteBuffer& buffer) {
      do_write(buffer.buf, buffer.bufsz);
      // we are NEVER going to block
      // because Windows NT implements true async io
    });
  }

  void
  read(byte_t* buf, size_t sz)
  {
    asio_evt_pkt* pkt = new asio_evt_pkt;
    pkt->buf          = buf;
    memset(&pkt->pkt, '\0', sizeof(OVERLAPPED));
    pkt->sz    = sz;
    pkt->write = false;
    ReadFile(tunif->tun_fd, buf, sz, nullptr, &pkt->pkt);
  }

  ~win32_tun_io()
  {
    CancelIo(tunif->tun_fd);
    if(tunif->tun_fd)
      tuntap_destroy(tunif);
  }
};

// and now the event loop itself
extern "C" DWORD FAR PASCAL
tun_ev_loop(void* unused)
{
  UNREFERENCED_PARAMETER(unused);

  DWORD size         = 0;
  OVERLAPPED* ovl    = nullptr;
  ULONG_PTR listener = 0;
  asio_evt_pkt* pkt  = nullptr;
  BOOL alert;

  while(true)
  {
    alert =
        GetQueuedCompletionStatus(tun_event_queue, &size, &listener, &ovl, 100);

    if(!alert)
      continue;  // let's go at it once more
    if(listener == (ULONG_PTR)~0)
      break;
    // if we're here, then we got something interesting :>
    pkt              = (asio_evt_pkt*)ovl;
    win32_tun_io* ev = reinterpret_cast< win32_tun_io* >(listener);
    if(!pkt->write)
    {
      // llarp::LogInfo("read tun ", size, " bytes, pass to handler");
      if(ev->t->recvpkt)
        ev->t->recvpkt(ev->t, llarp::InitBuffer(pkt->buf, size));
      ev->flush_write();
      ev->read(ev->readbuf, sizeof(ev->readbuf));
    }
    else
    {
      llarp::LogInfo("write ", size, " bytes to tunnel interface");
      // ok let's queue another read!
      ev->read(ev->readbuf, sizeof(ev->readbuf));
    }
    delete pkt;  // don't leak
  }
  llarp::LogInfo("exit TUN event loop thread from system managed thread pool");
  return 0;
}

void
exit_tun_loop()
{
  // if we get all-ones in the queue, thread exits, and we clean up
  PostQueuedCompletionStatus(tun_event_queue, 0, ~0, nullptr);

  // kill the kernel's thread pool
  int i = (&kThreadPool)[1] - kThreadPool;  // get the size of our thread pool
  llarp::LogInfo("closing ", i, " threads");
  WaitForMultipleObjects(i, kThreadPool, TRUE, INFINITE);
  for(int j = 0; j < i; ++j)
    CloseHandle(kThreadPool[j]);
  delete[] kThreadPool;

  // the IOCP refcount is decreased each time an associated fd
  // is closed
  // the fds are closed in their destructors
  // once we get to zero, we can safely close the event port
  auto itr = tun_listeners.begin();
  while(itr != tun_listeners.end())
  {
    delete(*itr);
    itr = tun_listeners.erase(itr);
  }
  CloseHandle(tun_event_queue);
}

namespace llarp
{
  int
  tcp_conn::read(byte_t* buf, size_t sz)
  {
    if(_shouldClose)
      return -1;

    ssize_t amount = uread(fd, (char*)buf, sz);

    if(amount > 0)
    {
      if(tcp.read)
        tcp.read(&tcp, llarp::InitBuffer(buf, amount));
    }
    else
    {
      // error
      _shouldClose = true;
      return -1;
    }
    return 0;
  }

  void
  tcp_conn::flush_write()
  {
    connected();
    ev_io::flush_write();
  }

  ssize_t
  tcp_conn::do_write(void* buf, size_t sz)
  {
    if(_shouldClose)
      return -1;
    return uwrite(fd, (char*)buf, sz);
  }

  void
  tcp_conn::connect()
  {
    socklen_t slen = sizeof(sockaddr_in);
    if(_addr.ss_family == AF_UNIX)
      slen = sizeof(sockaddr_un);
    else if(_addr.ss_family == AF_INET6)
      slen = sizeof(sockaddr_in6);
    int result = ::connect(fd, (const sockaddr*)&_addr, slen);
    if(result == 0)
    {
      llarp::LogDebug("connected immedidately");
      connected();
    }
    else if(WSAGetLastError() == WSAEINPROGRESS)
    {
      // in progress
      llarp::LogDebug("connect in progress");
      WSASetLastError(0);
      return;
    }
    else if(_conn->error)
    {
      // wtf?
      char ebuf[1024];
      int err = WSAGetLastError();
      FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err, LANG_NEUTRAL,
                    ebuf, 1024, nullptr);
      llarp::LogError("error connecting: ", ebuf);
      _conn->error(_conn);
    }
  }

  int
  tcp_serv::read(byte_t*, size_t)
  {
    int new_fd = ::accept(fd, nullptr, nullptr);
    if(new_fd == -1)
    {
      char ebuf[1024];
      int err = WSAGetLastError();
      FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err, LANG_NEUTRAL,
                    ebuf, 1024, nullptr);
      llarp::LogError("failed to accept on ", fd, ":", ebuf);
      return -1;
    }
    // build handler
    llarp::tcp_conn* connimpl = new tcp_conn(loop, new_fd);
    if(loop->add_ev(connimpl, true))
    {
      // call callback
      if(tcp->accepted)
        tcp->accepted(tcp, &connimpl->tcp);
      return 0;
    }
    // cleanup error
    delete connimpl;
    return -1;
  }

  struct udp_listener : public ev_io
  {
    llarp_udp_io* udp;

    udp_listener(int fd, llarp_udp_io* u) : ev_io(fd), udp(u){};

    ~udp_listener()
    {
    }

    bool
    tick()
    {
      if(udp->tick)
        udp->tick(udp);
      return true;
    }

    int
    read(byte_t* buf, size_t sz)
    {
      llarp_buffer_t b;
      b.base = buf;
      b.cur  = b.base;
      sockaddr_in6 src;
      socklen_t slen = sizeof(sockaddr_in6);
      sockaddr* addr = (sockaddr*)&src;
      ssize_t ret    = ::recvfrom(fd, (char*)b.base, sz, 0, addr, &slen);
      if(ret < 0)
        return -1;
      if(static_cast< size_t >(ret) > sz)
        return -1;
      b.sz = ret;
      udp->recvfrom(udp, addr, b);
      return 0;
    }

    int
    sendto(const sockaddr* to, const void* data, size_t sz)
    {
      socklen_t slen;
      switch(to->sa_family)
      {
        case AF_INET:
          slen = sizeof(struct sockaddr_in);
          break;
        case AF_INET6:
          slen = sizeof(struct sockaddr_in6);
          break;
        default:
          return -1;
      }
      ssize_t sent = ::sendto(fd, (char*)data, sz, 0, to, slen);
      if(sent == -1)
      {
        char ebuf[1024];
        int err = WSAGetLastError();
        FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err, LANG_NEUTRAL,
                      ebuf, 1024, nullptr);
        llarp::LogWarn(ebuf);
      }
      return sent;
    }
  };

};  // namespace llarp

struct llarp_win32_loop : public llarp_ev_loop
{
  upoll_t* upollfd;

  llarp_win32_loop() : upollfd(nullptr)
  {
  }

  bool
  tcp_connect(struct llarp_tcp_connecter* tcp, const sockaddr* remoteaddr)
  {
    // create socket
    int fd = usocket(remoteaddr->sa_family, SOCK_STREAM, 0);
    if(fd == -1)
      return false;
    llarp::tcp_conn* conn = new llarp::tcp_conn(this, fd, remoteaddr, tcp);
    add_ev(conn, true);
    conn->connect();
    return true;
  }

  llarp::ev_io*
  bind_tcp(llarp_tcp_acceptor* tcp, const sockaddr* bindaddr)
  {
    int fd = usocket(bindaddr->sa_family, SOCK_STREAM, 0);
    if(fd == -1)
      return nullptr;
    socklen_t sz = sizeof(sockaddr_in);
    if(bindaddr->sa_family == AF_INET6)
    {
      sz = sizeof(sockaddr_in6);
    }
    // keep. inexplicably, windows now has unix domain sockets
    // for now, use the ID numbers directly until this comes out of
    // beta
    else if(bindaddr->sa_family == AF_UNIX)
      sz = sizeof(sockaddr_un);

    if(::bind(fd, bindaddr, sz) == -1)
    {
      uclose(fd);
      return nullptr;
    }
    if(ulisten(fd, 5) == -1)
    {
      uclose(fd);
      return nullptr;
    }
    return new llarp::tcp_serv(this, fd, tcp);
  }

  virtual bool
  udp_listen(llarp_udp_io* l, const sockaddr* src)
  {
    auto ev = create_udp(l, src);
    if(ev)
      l->fd = ev->fd;
    return ev && add_ev(ev, false);
  }

  ~llarp_win32_loop()
  {
    if(upollfd)
      upoll_destroy(upollfd);
  }

  bool
  running() const
  {
    return (upollfd != nullptr);
  }

  bool
  init()
  {
    if(!upollfd)
      upollfd = upoll_create(1);
    return upollfd != nullptr;
  }

  // OK, the event loop, as it exists now, will _only_
  // work on sockets (and not very efficiently at that).
  int
  tick(int ms)
  {
    upoll_event_t events[1024];
    int result;
    result = upoll_wait(upollfd, events, 1024, ms);
    if(result > 0)
    {
      int idx = 0;
      while(idx < result)
      {
        llarp::ev_io* ev = static_cast< llarp::ev_io* >(events[idx].data.ptr);
        if(ev)
        {
          if(events[idx].events & UPOLLERR)
          {
            ev->error();
          }
          else
          {
            if(events[idx].events & UPOLLIN)
            {
              ev->read(readbuf, sizeof(readbuf));
            }
            if(events[idx].events & UPOLLOUT)
            {
              ev->flush_write();
            }
          }
        }
        ++idx;
      }
    }

    if(result != -1)
      tick_listeners();
    return result;
  }

  int
  run()
  {
    upoll_event_t events[1024];
    int result;
    do
    {
      result = upoll_wait(upollfd, events, 1024, EV_TICK_INTERVAL);
      if(result > 0)
      {
        int idx = 0;
        while(idx < result)
        {
          llarp::ev_io* ev = static_cast< llarp::ev_io* >(events[idx].data.ptr);
          if(ev)
          {
            if(events[idx].events & UPOLLERR)
            {
              ev->error();
            }
            else
            {
              if(events[idx].events & UPOLLIN)
              {
                ev->read(readbuf, sizeof(readbuf));
              }
              if(events[idx].events & UPOLLOUT)
              {
                ev->flush_write();
              }
            }
          }
          ++idx;
        }
      }
      if(result != -1)
        tick_listeners();
    } while(upollfd);
    return result;
  }

  int
  udp_bind(const sockaddr* addr)
  {
    socklen_t slen;
    switch(addr->sa_family)
    {
      case AF_INET:
        slen = sizeof(struct sockaddr_in);
        break;
      case AF_INET6:
        slen = sizeof(struct sockaddr_in6);
        break;
      default:
        return -1;
    }
    int fd = usocket(addr->sa_family, SOCK_DGRAM, 0);
    if(fd == -1)
    {
      perror("usocket()");
      return -1;
    }

    if(addr->sa_family == AF_INET6)
    {
      // enable dual stack explicitly
      int dual = 1;
      if(setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&dual, sizeof(dual))
         == -1)
      {
        // failed
        perror("setsockopt()");
        close(fd);
        return -1;
      }
    }
    llarp::Addr a(*addr);
    llarp::LogDebug("bind to ", a);
    if(bind(fd, addr, slen) == -1)
    {
      perror("bind()");
      close(fd);
      return -1;
    }

    return fd;
  }

  bool
  close_ev(llarp::ev_io* ev)
  {
    return upoll_ctl(upollfd, UPOLL_CTL_DEL, ev->fd, nullptr) != -1;
  }

  // no tunnels here
  llarp::ev_io*
  create_tun(llarp_tun_io* tun)
  {
    UNREFERENCED_PARAMETER(tun);
    return nullptr;
  }

  llarp::ev_io*
  create_udp(llarp_udp_io* l, const sockaddr* src)
  {
    int fd = udp_bind(src);
    if(fd == -1)
      return nullptr;
    llarp::ev_io* listener = new llarp::udp_listener(fd, l);
    l->impl                = listener;
    return listener;
  }

  bool
  add_ev(llarp::ev_io* e, bool write)
  {
    upoll_event_t ev;
    ev.data.ptr = e;
    ev.events   = UPOLLIN | UPOLLERR;
    if(write)
      ev.events |= UPOLLOUT;
    if(upoll_ctl(upollfd, UPOLL_CTL_ADD, e->fd, &ev) == -1)
    {
      delete e;
      return false;
    }
    handlers.emplace_back(e);
    return true;
  }

  bool
  udp_close(llarp_udp_io* l)
  {
    bool ret = false;
    llarp::udp_listener* listener =
        static_cast< llarp::udp_listener* >(l->impl);
    if(listener)
    {
      close_ev(listener);
      // remove handler
      auto itr = handlers.begin();
      while(itr != handlers.end())
      {
        if(itr->get() == listener)
          itr = handlers.erase(itr);
        else
          ++itr;
      }
      l->impl = nullptr;
      ret     = true;
    }
    return ret;
  }

  void
  stop()
  {
    // do nothing
  }
};

#endif