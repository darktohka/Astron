#pragma once
#include "NetworkAcceptor.h"
#include <functional>

typedef std::function<void(const std::shared_ptr<uvw::tcp_handle>&, const uvw::socket_address& remote, const uvw::socket_address& local, const bool haproxy_mode)> TcpAcceptorCallback;

class TcpAcceptor : public NetworkAcceptor
{
  public:
    TcpAcceptor(TcpAcceptorCallback &callback, AcceptorErrorCallback &err_callback);
    virtual ~TcpAcceptor() {}

  private:
    TcpAcceptorCallback m_callback;

    virtual void start_accept();
    void handle_accept(const std::shared_ptr<uvw::tcp_handle>& socket);
    void handle_endpoints(const std::shared_ptr<uvw::tcp_handle>& socket, const uvw::socket_address& remote, const uvw::socket_address& local);
};
