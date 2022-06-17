#include "TcpAcceptor.h"

TcpAcceptor::TcpAcceptor(TcpAcceptorCallback &callback, AcceptorErrorCallback& err_callback) :
    NetworkAcceptor(err_callback),
    m_callback(callback)
{
}

void TcpAcceptor::start_accept()
{
    m_acceptor->on<uvw::listen_event>([this](const uvw::listen_event &, uvw::tcp_handle &srv) {
        std::shared_ptr<uvw::tcp_handle> client = srv.parent().resource<uvw::tcp_handle>();
        srv.accept(*client);
        handle_accept(client);
    });

    m_acceptor->on<uvw::error_event>([this](const uvw::error_event &evt, uvw::tcp_handle &) {
        // Inform the error callback:
        this->m_err_callback(evt);
    });
}

void TcpAcceptor::handle_accept(const std::shared_ptr<uvw::tcp_handle>& socket)
{
    if(!m_started) {
        // We were turned off sometime before this operation completed; ignore.
        socket->close();
        return;
    }

    uvw::socket_address remote = socket->peer();
    uvw::socket_address local = socket->sock();
    handle_endpoints(socket, remote, local);
}

void TcpAcceptor::handle_endpoints(const std::shared_ptr<uvw::tcp_handle>& socket, const uvw::socket_address& remote, const uvw::socket_address& local)
{
    // Inform the callback:
    m_callback(socket, remote, local, m_haproxy_mode);
}
