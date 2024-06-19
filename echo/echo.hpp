#include "../source/TcpServer.hpp"
#include "../source/Server.hpp"

class EchoServer
{
private:
    TcpServer _server;

private:
    void OnConnected(const PtrConnection &conn)
    {
        DBG_LOG("NEW CONNECTION:%p", conn.get());
    }
    void OnClosed(const PtrConnection &conn)
    {
        DBG_LOG("CLOSE CONNECTION:%p", conn.get());
    }
    void OnMessage(const PtrConnection &conn, Buffer *buf)
    {
        conn->Send(buf->ReadPosition(), buf->ReadAbleSize());
        buf->MoveReadOffset(buf->ReadAbleSize());
        conn->Shutdown();
    }

public:
    EchoServer(int port) : _server(port)
    {
        _server.SetThreadCount(2);
        _server.EnableInactiveRelease(10);
        _server.SetClosedCallback(std::bind(&EchoServer::OnClosed, this, std::placeholders::_1));
        _server.SetConnectedCallback(std::bind(&EchoServer::OnConnected, this, std::placeholders::_1));
        _server.SetMessageCallback(std::bind(&EchoServer::OnMessage, this, std::placeholders::_1, std::placeholders::_2));
    }
    void Start() { _server.Start(); }
};