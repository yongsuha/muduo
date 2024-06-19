#pragma once 
#include <typeinfo>
#include "EventLoop.hpp"
#include "Server.hpp"
#include "Buffer.hpp"

class Any
{
private:
    class holder
    {
    public:
        virtual ~holder() {}
        virtual const std::type_info &type() = 0;
        virtual holder *clone() = 0;
    };
    template <class T>
    class placeholder : public holder
    {
    public:
        placeholder(const T &val) : _val(val) {}
        // 获取子类对象保存的数据类型
        virtual const std::type_info &type() { return typeid(T); }
        // 针对当前的对象自身，克隆出一个新的子类对象
        virtual holder *clone() { return new placeholder(_val); }

    public:
        T _val;
    };
    holder *_content;

public:
    Any() : _content(NULL) {}
    template <class T>
    Any(const T &val) : _content(new placeholder<T>(val)) {}
    Any(const Any &other) : _content(other._content ? other._content->clone() : NULL) {}
    ~Any() { delete _content; }

    Any &swap(Any &other)
    {
        std::swap(_content, other._content);
        return *this;
    }

    // 返回子类对象保存的数据的指针
    template <class T>
    T *get()
    {
        // 想要获取的数据类型，必须和保存的数据类型一致
        assert(typeid(T) == _content->type());
        return &((placeholder<T> *)_content)->_val;
    }
    // 赋值运算符的重载函数
    template <class T>
    Any &operator=(const T &val)
    {
        // 为val构造一个临时的通用容器，然后与当前容器自身进行指针交换，临时对象释放的时候，原先保存的数据也就被释放
        Any(val).swap(*this);
        return *this;
    }
    Any &operator=(const Any &other)
    {
        Any(other).swap(*this);
        return *this;
    }
};

class Connection;
// DISCONECTED -- 连接关闭状态；   CONNECTING -- 连接建立成功-待处理状态
// CONNECTED -- 连接建立完成，各种设置已完成，可以通信的状态；  DISCONNECTING -- 待关闭状态
typedef enum
{
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    DISCONNECTING
} ConnStatu;
using PtrConnection = std::shared_ptr<Connection>;

// 该模块是对Socket Channel Buffer的整体封装,对通信连接进行管理
// 其中的各种事件的回调处理函数需要组件使用者传入

class Connection : public std::enable_shared_from_this<Connection>
{
private:
    uint64_t _conn_id; // 连接的唯一ID，便于连接的管理和查找
    // uint64_t _timer_id;   //定时器ID，必须是唯一的，这块为了简化操作使用conn_id作为定时器ID
    int _sockfd;                   // 连接关联的文件描述符
    bool _enable_inactive_release; // 连接是否启动非活跃销毁的判断标志，默认为false
    EventLoop *_loop;              // 连接所关联的一个EventLoop
    ConnStatu _statu;              // 连接状态
    Socket _socket;                // 套接字操作管理
    Channel _channel;              // 连接的事件管理
    Buffer _in_buffer;             // 输入缓冲区---存放从socket中读取到的数据
    Buffer _out_buffer;            // 输出缓冲区---存放要发送给对端的数据
    Any _context;                  // 请求的接收处理上下文

    /*这四个回调函数，是让服务器模块来设置的（其实服务器模块的处理回调也是组件使用者设置的）*/
    /*换句话说，这几个回调都是组件使用者使用的*/
    using ConnectedCallback = std::function<void(const PtrConnection &)>;
    using MessageCallback = std::function<void(const PtrConnection &, Buffer *)>;
    using ClosedCallback = std::function<void(const PtrConnection &)>;
    using AnyEventCallback = std::function<void(const PtrConnection &)>;
    ConnectedCallback _connected_callback;
    MessageCallback _message_callback;
    ClosedCallback _closed_callback;
    AnyEventCallback _event_callback;
    /*组件内的连接关闭回调--组件内设置的，因为服务器组件内会把所有的连接管理起来，一旦某个连接要关闭*/
    /*就应该从管理的地方移除掉自己的信息*/
    ClosedCallback _server_closed_callback;

private:
    /*五个channel的事件回调函数*/
    // 描述符可读事件触发后调用的函数，接收socket数据放到接收缓冲区中，然后调用_message_callback
    void HandleRead()
    {
        // 1. 接收socket的数据，放到缓冲区
        char buf[65536];
        ssize_t ret = _socket.NonBlockRecv(buf, 65535);
        if (ret < 0)
        {
            // 出错了,不能直接关闭连接
            return ShutdownInLoop();
        }
        // 这里的等于0表示的是没有读取到数据，而并不是连接断开了，连接断开返回的是-1
        // 将数据放入输入缓冲区,写入之后顺便将写偏移向后移动
        _in_buffer.WriteAndPush(buf, ret);
        // 2. 调用message_callback进行业务处理
        if (_in_buffer.ReadAbleSize() > 0)
        {
            // shared_from_this--从当前对象自身获取自身的shared_ptr管理对象
            return _message_callback(shared_from_this(), &_in_buffer);
        }
    }
    // 描述符可写事件触发后调用的函数，将发送缓冲区中的数据进行发送
    void HandleWrite()
    {
        //_out_buffer中保存的数据就是要发送的数据
        ssize_t ret = _socket.NonBlockSend(_out_buffer.ReadPosition(), _out_buffer.ReadAbleSize());
        if (ret < 0)
        {
            // 发送错误就该关闭连接了，
            if (_in_buffer.ReadAbleSize() > 0)
            {
                _message_callback(shared_from_this(), &_in_buffer);
            }
            return Release(); // 这时候就是实际的关闭释放操作了。
        }
        _out_buffer.MoveReadOffset(ret); // 千万不要忘了，将读偏移向后移动
        if (_out_buffer.ReadAbleSize() == 0)
        {
            _channel.DisableWrite(); // 没有数据待发送了，关闭写事件监控
            // 如果当前是连接待关闭状态，则有数据，发送完数据释放连接，没有数据则直接释放
            if (_statu == DISCONNECTING)
            {
                return Release();
            }
        }
        return;
    }
    // 描述符触发挂断事件
    void HandleClose()
    {
        /*一旦连接挂断了，套接字就什么都干不了了，因此有数据待处理就处理一下，完毕关闭连接*/
        if (_in_buffer.ReadAbleSize() > 0)
        {
            _message_callback(shared_from_this(), &_in_buffer);
        }
        return Release();
    }
    // 描述符触发出错事件
    void HandleError()
    {
        return HandleClose();
    }
    // 描述符触发任意事件: 1. 刷新连接的活跃度--延迟定时销毁任务；  2. 调用组件使用者的任意事件回调
    void HandleEvent()
    {
        if (_enable_inactive_release == true)
        {
            _loop->TimerRefresh(_conn_id);
        }
        if (_event_callback)
        {
            _event_callback(shared_from_this());
        }
    }
    // 连接获取之后，所处的状态下要进行各种设置（启动读监控,调用回调函数）
    void EstablishedInLoop()
    {
        // 1. 修改连接状态；  2. 启动读事件监控；  3. 调用回调函数
        assert(_statu == CONNECTING); // 当前的状态必须一定是上层的半连接状态
        _statu = CONNECTED;           // 当前函数执行完毕，则连接进入已完成连接状态
        // 一旦启动读事件监控就有可能会立即触发读事件，如果这时候启动了非活跃连接销毁
        _channel.EnableRead();
        if (_connected_callback)
            _connected_callback(shared_from_this());
    }
    // 这个接口才是实际的释放接口
    void ReleaseInLoop()
    {
        // 1. 修改连接状态，将其置为DISCONNECTED
        _statu = DISCONNECTED;
        // 2. 移除连接的事件监控
        _channel.Remove();
        // 3. 关闭描述符
        _socket.Close();
        // 4. 如果当前定时器队列中还有定时销毁任务，则取消任务
        if (_loop->HasTimer(_conn_id))
            CancelInactiveReleaseInLoop();
        // 5. 调用关闭回调函数，避免先移除服务器管理的连接信息导致Connection被释放，再去处理会出错，因此先调用用户的回调函数
        if (_closed_callback)
            _closed_callback(shared_from_this());
        // 移除服务器内部管理的连接信息
        if (_server_closed_callback)
            _server_closed_callback(shared_from_this());
    }
    // 这个接口并不是实际的发送接口，而只是把数据放到了发送缓冲区，启动了可写事件监控
    void SendInLoop(Buffer &buf)
    {
        if (_statu == DISCONNECTED)
            return;
        _out_buffer.WriteBufferAndPush(buf);
        if (_channel.WriteAble() == false)
        {
            _channel.EnableWrite();
        }
    }
    // 这个关闭操作并非实际的连接释放操作，需要判断还有没有数据待处理，待发送
    void ShutdownInLoop()
    {
        _statu = DISCONNECTING; // 设置连接为半关闭状态
        if (_in_buffer.ReadAbleSize() > 0)
        {
            if (_message_callback)
                _message_callback(shared_from_this(), &_in_buffer);
        }
        // 要么就是写入数据的时候出错关闭，要么就是没有待发送数据，直接关闭
        if (_out_buffer.ReadAbleSize() > 0)
        {
            if (_channel.WriteAble() == false)
            {
                _channel.EnableWrite();
            }
        }
        if (_out_buffer.ReadAbleSize() == 0)
        {
            Release();
        }
    }
    // 启动非活跃连接超时释放规则
    void EnableInactiveReleaseInLoop(int sec)
    {
        // 1. 将判断标志 _enable_inactive_release 置为true
        _enable_inactive_release = true;
        // 2. 如果当前定时销毁任务已经存在，那就刷新延迟一下即可
        if (_loop->HasTimer(_conn_id))
        {
            return _loop->TimerRefresh(_conn_id);
        }
        // 3. 如果不存在定时销毁任务，则新增
        _loop->TimerAdd(_conn_id, sec, std::bind(&Connection::Release, this));
    }
    void CancelInactiveReleaseInLoop()
    {
        _enable_inactive_release = false;
        if (_loop->HasTimer(_conn_id))
        {
            _loop->TimerCancel(_conn_id);
        }
    }
    void UpgradeInLoop(const Any &context,
                       const ConnectedCallback &conn,
                       const MessageCallback &msg,
                       const ClosedCallback &closed,
                       const AnyEventCallback &event)
    {
        _context = context;
        _connected_callback = conn;
        _message_callback = msg;
        _closed_callback = closed;
        _event_callback = event;
    }

public:
    Connection(EventLoop *loop, uint64_t conn_id, int sockfd) : _conn_id(conn_id), _sockfd(sockfd),
                                                                _enable_inactive_release(false), _loop(loop), _statu(CONNECTING), _socket(_sockfd),
                                                                _channel(loop, _sockfd)
    {
        _channel.SetCloseCallback(std::bind(&Connection::HandleClose, this));
        _channel.SetEventCallback(std::bind(&Connection::HandleEvent, this));
        _channel.SetReadCallback(std::bind(&Connection::HandleRead, this));
        _channel.SetWriteCallback(std::bind(&Connection::HandleWrite, this));
        _channel.SetErrorCallback(std::bind(&Connection::HandleError, this));
    }
    ~Connection() { DBG_LOG("RELEASE CONNECTION:%p", this); }
    // 获取管理的文件描述符
    int Fd() { return _sockfd; }
    // 获取连接ID
    int Id() { return _conn_id; }
    // 是否处于CONNECTED状态
    bool Connected() { return (_statu == CONNECTED); }
    // 设置上下文--连接建立完成时进行调用
    void SetContext(const Any &context) { _context = context; }
    // 获取上下文，返回的是指针
    Any *GetContext() { return &_context; }
    void SetConnectedCallback(const ConnectedCallback &cb) { _connected_callback = cb; }
    void SetMessageCallback(const MessageCallback &cb) { _message_callback = cb; }
    void SetClosedCallback(const ClosedCallback &cb) { _closed_callback = cb; }
    void SetAnyEventCallback(const AnyEventCallback &cb) { _event_callback = cb; }
    void SetSrvClosedCallback(const ClosedCallback &cb) { _server_closed_callback = cb; }
    // 连接建立就绪后，进行channel回调设置，启动读监控，调用_connected_callback
    void Established()
    {
        _loop->RunInLoop(std::bind(&Connection::EstablishedInLoop, this));
    }
    // 发送数据，将数据放到发送缓冲区，启动写事件监控
    void Send(const char *data, size_t len)
    {
        // 外界传入的data，可能是个临时的空间，我们现在只是把发送操作压入了任务池，有可能并没有被立即执行
        // 因此有可能执行的时候，data指向的空间有可能已经被释放了。
        Buffer buf;
        buf.WriteAndPush(data, len);
        _loop->RunInLoop(std::bind(&Connection::SendInLoop, this, std::move(buf)));
    }
    // 提供给组件使用者的关闭接口--并不实际关闭，需要判断有没有数据待处理
    void Shutdown()
    {
        _loop->RunInLoop(std::bind(&Connection::ShutdownInLoop, this));
    }
    void Release()
    {
        _loop->QueueInLoop(std::bind(&Connection::ReleaseInLoop, this));
    }
    // 启动非活跃销毁，并定义多长时间无通信就是非活跃，添加定时任务
    void EnableInactiveRelease(int sec)
    {
        _loop->RunInLoop(std::bind(&Connection::EnableInactiveReleaseInLoop, this, sec));
    }
    // 取消非活跃销毁
    void CancelInactiveRelease()
    {
        _loop->RunInLoop(std::bind(&Connection::CancelInactiveReleaseInLoop, this));
    }
    // 切换协议---重置上下文以及阶段性回调处理函数 -- 而是这个接口必须在EventLoop线程中立即执行
    // 防备新的事件触发后，处理的时候，切换任务还没有被执行--会导致数据使用原协议处理了。
    void Upgrade(const Any &context, const ConnectedCallback &conn, const MessageCallback &msg,
                 const ClosedCallback &closed, const AnyEventCallback &event)
    {
        _loop->AssertInLoop();
        _loop->RunInLoop(std::bind(&Connection::UpgradeInLoop, this, context, conn, msg, closed, event));
    }
};