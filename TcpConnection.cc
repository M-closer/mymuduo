#include "TcpConnection.h"
#include "Logger.h"
#include "Socket.h"
#include "Channel.h"
#include "EventLoop.h"

#include <functional>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <strings.h>
#include <netinet/tcp.h>

static EventLoop* CheckLoopNotNull(EventLoop *loop)
{
    if(loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d TcpConnection loop is null! \n",__FILE__,__FUNCTION__,__LINE__);
    }
    return loop;
}

TcpConnection::TcpConnection(EventLoop* loop,
    const std::string& nameArg,
    int sockfd,
    const InetAddress& localAddr,
    const InetAddress& peerAddr)
    :loop_(CheckLoopNotNull(loop))
    ,name_(nameArg)
    ,state_(kConnecting)
    ,reading_(true)
    ,socket_(new Socket(sockfd))
    ,channel_(new Channel(loop,sockfd))
    ,localAddr_(localAddr)
    ,peerAddr_(peerAddr)
    ,highWaterMark_(64*1024*1024)  //64M
{
    //下面给channel设置相应的回调函数，poller给channel通知感兴趣的事件发生了，channel会回调相应的操作函数
    channel_->setReadCallback(std::bind(&TcpConnection::handleRead,this,std::placeholders::_1));
    channel_->setWriteCallback(std::bind(&TcpConnection::handleWrite,this));
    channel_->setCloseCallback(std::bind(&TcpConnection::handleClose,this));
    channel_->setErrorCallback(std::bind(&TcpConnection::handleError,this));    

    LOG_INFO("TcpConnection::ctor[%s] at fd=%d \n",name_.c_str(),sockfd);
    socket_->setKeepAlive(true);
}

TcpConnection::~TcpConnection()
{
    LOG_INFO("TcpConnection::dtor[%s] at fd=%d state=%d\n",
        name_.c_str(),channel_->fd(),(int)state_);
}

void TcpConnection::handleRead(Timestamp receiveTime)
{
    int saveErrno = 0;
    ssize_t n = inputBuffer_.readFd(channel_->fd(),&saveErrno);
    if(n>0)
    {
        //已建立连接的用户，有可读事件发生了，调用用户传入的回调操作 onMessage
        messageCallback_(shared_from_this(),&inputBuffer_,receiveTime);
    }
    else if(n == 0)
    {
        handleClose();
    }
    else
    {
        errno = saveErrno;
        LOG_ERROR("TcpConnection::handleRead \n");
        handleError();
    }
}
void TcpConnection::handleWrite()
{
    if(channel_->isWriting())
    {
        int savedError = 0;
        ssize_t n = outputBuffer_.writeFd(channel_->fd(),&savedError);

        if(n>0)
        {
            outputBuffer_.retrieve(n);
            if(outputBuffer_.readableBytes() == 0) //读完了
            {
                channel_->disableWriting();
                //唤醒loop_对应的thread线程，执行回调
                if(writeCompleteCallback_)
                {
                    loop_->queueInLoop(std::bind(writeCompleteCallback_,shared_from_this()));
                }
                //因为在写过程中，可能发生关闭连接，但是必须把写操作完成后才能关闭连接，此处就是判断是否关闭连接
                if(state_ == kDisconnecting)
                {
                    shutdownInLoop();
                }

            }
        }
        else
        {
            LOG_ERROR("TcpConnection::handleWrite \n");
        }
    } //可写
    else
    {
        LOG_ERROR("TcpConnection::handleWrite Connection fd = %d is down, no more writing \n",channel_->fd());
    }

}

//poller => channel::closeCallback => TcpConnection::handleClose 
void TcpConnection::handleClose()
{
    LOG_INFO("TcpConnection::handleClose fd = %d, state = %d \n",channel_->fd(),(int)state_);
    setState(kDisconnected);
    channel_->disableAll();

    TcpConnectionPtr connPtr(shared_from_this());
    connectionCallback_(connPtr); //执行连接关闭的回调
    closeCallback_(connPtr); //关闭连接的回调 执行的是TcpServer::removeConnection回调方法
}
void TcpConnection::handleError()
{
    int optval;
    socklen_t optlen = static_cast<socklen_t>(sizeof optval);
    int err = 0;
    if(::getsockopt(channel_->fd(),SOL_SOCKET,SO_ERROR,&optval,&optlen) < 0)
    {
        err = errno;
    }
    else
    {
        err = optval;
    }
    LOG_ERROR("TcpConnection::handleError name:%s - SO_ERROR:%d \n",name_.c_str(),err);
}

void TcpConnection::send(const std::string& buf)
{
    if(state_ == kConnected)
    {
        if(loop_->isInLoopThread()) //刚好在此线程中
        {
            sendInLoop(buf.c_str(),buf.size());
        }
        else
        {
            loop_->runInLoop(std::bind(
                &TcpConnection::sendInLoop,
                this,
                buf.c_str(),
                buf.size()
            ));
        }
    }
}

//发送数据   应用写的快，而内核发送数据慢，需要把待发送的数据写入缓冲区，而且设置了水位回调
void TcpConnection::sendInLoop(const void* message,size_t len)
{
    ssize_t nwrote = 0;
    size_t remaining = len;  //还未发送完的数据
    bool faultError = false;

    //之前调用过该connection的shutdown，不能再进行发送了
    if(state_ == kDisconnected)
    {
        LOG_ERROR("TcpConnection::sendInLoop disconnectd,give up writing \n");
        return;
    }

    //表示channel_第一次开始写数据，并且缓冲区没有待发送的数据
    if(!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
    {
        nwrote = ::write(channel_->fd(),message,len);
        if(nwrote >= 0)
        {
            remaining = len - nwrote;
            if(remaining == 0 && writeCompleteCallback_)
            {
                //既然在这里数据全部发送完成，就不用再给channel设置epollout事件了
                loop_->queueInLoop(std::bind(writeCompleteCallback_,shared_from_this()));
            }
        }
        else
        {
            nwrote = 0;
            if(errno != EWOULDBLOCK)
            {
                LOG_ERROR("TcpConnection::sendInLoop \n");
                if(errno == EPIPE || errno == ECONNRESET) // SIGPIPE  RESET
                {
                    faultError = true;
                }
            }
        }
    }

    //说明当前这一次write，并没有把数据全部发送出去，剩余的数据需要保存到缓冲区中，
    //给channel注册EPOLLOUT事件，poller发现tcp的发送缓冲区有空间，会通知相应的sock-channel，调用handleWrite回调方法
    //也就是调用TcpConnection::handleWrite方法，把发送缓冲区中的数据全部发送完成
    if(!faultError && remaining > 0)  
    {   
        //目前发送缓冲区剩余的待发送数据的长度
        ssize_t oldLen = outputBuffer_.readableBytes();
        if(oldLen + remaining >= highWaterMark_ 
        && oldLen < highWaterMark_
        && highWaterMarkCallback_)
        {
            loop_->queueInLoop(std::bind(highWaterMarkCallback_,shared_from_this(),oldLen+remaining));
        }
        outputBuffer_.append(static_cast<const char*>(message)+nwrote,remaining);
        if(!channel_->isWriting())
        {
            channel_->enableWriting();  //这里一定要注册channel的写事件，否则poller不会给channel通知epollout
        }
    }
}

//建立连接
void TcpConnection::connectEstablished()
{
    setState(kConnected);
    channel_->tie(shared_from_this());
    channel_->enableReading();  //向Poller注册channel的读事件epollin

    //新连接建立，执行回调
    connectionCallback_(shared_from_this());
}


//销毁连接
void TcpConnection::connectDestroyed()
{
    if(state_ == kConnected)
    {
        setState(kDisconnected);
        channel_->disableAll(); //把channel的所有感兴趣的事件，从poller中del掉

        connectionCallback_(shared_from_this());
    }
    channel_->remove(); //把channel从poller中删除
}

// 关闭连接
void TcpConnection::shutdown()
{
    if(state_ == kConnected)
    {
        setState(kDisconnecting);
        loop_->runInLoop(
            std::bind(&TcpConnection::shutdownInLoop,this)
        );
    }
}

void TcpConnection::shutdownInLoop()
{
    if(!channel_->isWriting()) //说明当前outputbuffer中的数据已经全部发送完成
    {
        socket_->shutdownWrite();
    }
}