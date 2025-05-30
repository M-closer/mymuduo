#include"TcpServer.h"
#include"Logger.h"
#include"TcpConnection.h"

#include <strings.h>
#include <functional>

EventLoop* CheckLoopNotNull(EventLoop *loop)
{
    if(loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d mainLoop is null! \n",__FILE__,__FUNCTION__,__LINE__);
    }
    return loop;
}

TcpServer::TcpServer(EventLoop* loop,
    const InetAddress& listenAddr,
    const std::string& nameArg,
    Option option)
    :loop_(CheckLoopNotNull(loop))
    ,ipPort_(listenAddr.toIpPort())
    ,name_(nameArg)
    ,acceptor_(new Acceptor(loop,listenAddr,option == kReusePort))
    ,threadpool_(new EventLoopThreadPool(loop,name_))
    ,connectionCallback_()
    ,messageCallback_()
    ,nextConnId_(1)
    ,start_(0)
{
    //当有新用户连接时，会执行TcpServer::newConnection回调
    acceptor_->setNewConnectionCallback(std::bind(&TcpServer::newConnection,this,
            std::placeholders::_1,std::placeholders::_2));
}


TcpServer::~TcpServer()
{
    for(auto& item : connections_)
    {
        TcpConnectionPtr conn(item.second); //这个局部的shared_ptr职能指针对象，出右括号，可以自动释放new出来的TcpConnection对象资源
        item.second.reset();

        //销毁连接
        conn->getLoop()->runInLoop(
            std::bind(&TcpConnection::connectDestroyed,conn)
        );
    }
}

// 有一个新的客户端连接，acceptor会执行这个回调操作
void TcpServer::newConnection(int sockfd,const InetAddress& peerAddr)
{
    //轮询算法，选择一个subLoop，来管理channel
    EventLoop* ioLoop = threadpool_->getNextLoop();
    char buf[1024] = {0};
    snprintf(buf,sizeof(buf),"-%s#%d",ipPort_.c_str(),nextConnId_);
    ++nextConnId_;
    std::string connName = name_+buf;

    LOG_INFO("TcpServer::newConnection [%s] - new connection [%s] from %s \n",
        name_.c_str(),connName.c_str(),peerAddr.toIpPort().c_str());

    //通过sockfd获取其绑定的本机的ip地址和端口信息
    struct sockaddr_in local;
    bzero(&local,sizeof(local));
    socklen_t addrlen = sizeof(local);
    if(::getsockname(sockfd,(sockaddr*)&local,&addrlen) < 0)
    {
        LOG_ERROR("sockets::getLocalAddr");
    }
    InetAddress localAddr(local);

    //根据连接成功的sockfd，创建TcpConnection连接对象
    TcpConnectionPtr conn(new TcpConnection(ioLoop,connName,sockfd,localAddr,peerAddr));
    connections_[connName] = conn;
    //下面的回调都是用户设置给TcpServer=>TcpConnection=>Channel=>Poller=>notify channel调用回调
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    //设置了如何关闭连接的回调 conn->shutdown
    conn->setCloseCallback(
        std::bind(&TcpServer::removeConnection,this,std::placeholders::_1)
    );
    //直接调用TcpConnection::connectEstablished
    ioLoop->runInLoop(std::bind(&TcpConnection::connectEstablished,conn));
    
}

//设置底层subloop的个数
void TcpServer::setThreadNum(int numThreads)
{
    threadpool_->setThreadNum(numThreads); 
}

//开启服务器监听 之后调用loop方法
void TcpServer::start()   // 防止一个TcpServer对象被start多次
{
    if(start_++ == 0)
    {
        threadpool_->start(threadInitCallback_); // 启动底层loop线程池
        loop_->runInLoop(std::bind(&Acceptor::listen,acceptor_.get()));
    }
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn)
{
    loop_->runInLoop(std::bind(&TcpServer::removeConnectionInLoop,this,conn));
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{
    LOG_INFO("TcpServer::removeConnectionInLoop [%s] - connetion %s \n",name_.c_str(),conn->name().c_str());
    size_t n = connections_.erase(conn->name());
    EventLoop* ioLoop = conn->getLoop();
    ioLoop->queueInLoop(
        std::bind(&TcpConnection::connectDestroyed,conn)
    );
}