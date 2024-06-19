#pragma once
#include <signal.h>
#include "Acceptor.hpp"

class TcpServer
{
private:
    uint64_t _next_id; // 这是一个自动增长的连接ID，
    int _port;
    int _timeout;                                       // 这是非活跃连接的统计时间---多长时间无通信就是非活跃连接
    bool _enable_inactive_release;                      // 是否启动了非活跃连接超时销毁的判断标志
    EventLoop _baseloop;                                // 这是主线程的EventLoop对象，负责监听事件的处理
    Acceptor _acceptor;                                 // 这是监听套接字的管理对象
    LoopThreadPool _pool;                               // 这是从属EventLoop线程池
    std::unordered_map<uint64_t, PtrConnection> _conns; // 保存管理所有连接对应的shared_ptr对象

    using ConnectedCallback = std::function<void(const PtrConnection &)>;
    using MessageCallback = std::function<void(const PtrConnection &, Buffer *)>;
    using ClosedCallback = std::function<void(const PtrConnection &)>;
    using AnyEventCallback = std::function<void(const PtrConnection &)>;
    using Functor = std::function<void()>;
    ConnectedCallback _connected_callback;
    MessageCallback _message_callback;
    ClosedCallback _closed_callback;
    AnyEventCallback _event_callback;

private:
    void RunAfterInLoop(const Functor &task, int delay)
    {
        _next_id++;
        _baseloop.TimerAdd(_next_id, delay, task);
    }
    // 为新连接构造一个Connection进行管理
    void NewConnection(int fd)
    {
        _next_id++;
        PtrConnection conn(new Connection(_pool.NextLoop(), _next_id, fd));
        conn->SetMessageCallback(_message_callback);
        conn->SetClosedCallback(_closed_callback);
        conn->SetConnectedCallback(_connected_callback);
        conn->SetAnyEventCallback(_event_callback);
        conn->SetSrvClosedCallback(std::bind(&TcpServer::RemoveConnection, this, std::placeholders::_1));
        if (_enable_inactive_release)
            conn->EnableInactiveRelease(_timeout); // 启动非活跃超时销毁
        conn->Established();                       // 就绪初始化
        _conns.insert(std::make_pair(_next_id, conn));
    }
    void RemoveConnectionInLoop(const PtrConnection &conn)
    {
        int id = conn->Id();
        auto it = _conns.find(id);
        if (it != _conns.end())
        {
            _conns.erase(it);
        }
    }
    // 从管理Connection的_conns中移除连接信息
    void RemoveConnection(const PtrConnection &conn)
    {
        _baseloop.RunInLoop(std::bind(&TcpServer::RemoveConnectionInLoop, this, conn));
    }

public:
    TcpServer(int port) : _port(port),
                          _next_id(0),
                          _enable_inactive_release(false),
                          _acceptor(&_baseloop, port),
                          _pool(&_baseloop)
    {
        _acceptor.SetAcceptCallback(std::bind(&TcpServer::NewConnection, this, std::placeholders::_1));
        _acceptor.Listen(); // 将监听套接字挂到baseloop上
    }
    void SetThreadCount(int count) { return _pool.SetThreadCount(count); }
    void SetConnectedCallback(const ConnectedCallback &cb) { _connected_callback = cb; }
    void SetMessageCallback(const MessageCallback &cb) { _message_callback = cb; }
    void SetClosedCallback(const ClosedCallback &cb) { _closed_callback = cb; }
    void SetAnyEventCallback(const AnyEventCallback &cb) { _event_callback = cb; }
    void EnableInactiveRelease(int timeout)
    {
        _timeout = timeout;
        _enable_inactive_release = true;
    }
    // 用于添加一个定时任务
    void RunAfter(const Functor &task, int delay)
    {
        _baseloop.RunInLoop(std::bind(&TcpServer::RunAfterInLoop, this, task, delay));
    }
    void Start()
    {
        _pool.Create();
        _baseloop.Start();
    }
};

void Channel::Remove() { return _loop->RemoveEvent(this); }
void Channel::Update() { return _loop->UpdateEvent(this); }
void TimerWheel::TimerAdd(uint64_t id, uint32_t delay, const TaskFunc &cb)
{
    _loop->RunInLoop(std::bind(&TimerWheel::TimerAddInLoop, this, id, delay, cb));
}
// 刷新/延迟定时任务
void TimerWheel::TimerRefresh(uint64_t id)
{
    _loop->RunInLoop(std::bind(&TimerWheel::TimerRefreshInLoop, this, id));
}
void TimerWheel::TimerCancel(uint64_t id)
{
    _loop->RunInLoop(std::bind(&TimerWheel::TimerCancelInLoop, this, id));
}

//当连接断开还进行send，服务器就会出现SIGPIPE异常，导致程序退出，需要屏蔽
class NetWorker
{
public: 
    NetWorker()
    {
        DBG_LOG("SIGPIPE INIT");
        signal(SIGPIPE, SIG_IGN);
    }
};
static NetWorker nw;//静态变量，将在main函数执行前生成