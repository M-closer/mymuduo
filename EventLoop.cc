#include "EventLoop.h"
#include "Logger.h"
#include "Poller.h"
#include "Channel.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <error.h>
#include <memory>

//防止一个线程创建多个EventLoop thread_local,全局变量，但是每个线程创建时都有一个副本
__thread EventLoop* t_loopInThisThread = nullptr;

//定义默认的Poller IO复用接口的超时时间
const int kPollTimeMs = 10000;

//创建wakeupfd，用来notify唤醒subReactor，处理新来的channel
int createEventfd()
{
    int envfd = ::eventfd(0,EFD_NONBLOCK | EFD_CLOEXEC);
    if(envfd < 0)
    {
        LOG_FATAL("eventfd error:%d \n",errno);
    }
    return envfd;
}

EventLoop::EventLoop()
    :looping_(false)
    ,quit_(false)
    ,callingPendingFunctors_(false)
    ,threadId_(CurrentThread::tid())
    ,poller_(Poller::newDefaultPoller(this))
    ,wakeupFd_(createEventfd())
    ,weakupChannel_(new Channel(this,wakeupFd_))
{
    LOG_DEBUG("EventLoop created %p in thread %d \n",this,threadId_);
    if(t_loopInThisThread)
    {
        LOG_FATAL("Another EventLoop %p exists in this thread %d \n",t_loopInThisThread,threadId_); 
    }
    else
    {
        t_loopInThisThread = this;
    }

    // 设置wakeupfd的事件类型以及发生事件后的回调操作
    weakupChannel_->setReadCallback(std::bind(&EventLoop::handleRead,this));
    // 每一个eventloop都将监听wakeupchannel的EPOLLI读事件了
    weakupChannel_->enableReading();
}

EventLoop::~EventLoop()
{
    weakupChannel_->disableAll();
    weakupChannel_->remove();
    ::close(wakeupFd_);
    t_loopInThisThread = nullptr;
}

void EventLoop::handleRead()
{
    uint64_t one = 1;
    ssize_t n = read(wakeupFd_,&one,sizeof(one));
    if(n != sizeof one)
    {
        LOG_ERROR("EventLoop::handleRead() reads %lu bytes instead of 8",n);
    }
}

//开启事件循环
void EventLoop::loop()
{
    looping_ = true;
    quit_ = false;

    LOG_INFO("Eventloop %p start looping \n",this);

    while(!quit_)
    {
        activeChannels_.clear();
        //监听两类fd 一种是client的fd,lfd 一种是wakefd，mainLoop和subloop之间的fd
        pollReturnTime_ = poller_->poll(kPollTimeMs,&activeChannels_);
        for(Channel *   channel : activeChannels_)
        {
            // Poller监听哪些channel发生事件了，然后上报给EventLoop，通知channel处理相应的事件
            channel->handleEvent(pollReturnTime_);
        }
        //执行当前EventLoop事件循环需要处理的回调操作
        /*
         事先注册一个回调cb （需要subloop来执行）
        */
        doPendingFunctors();
    }
    LOG_INFO("EventLoop %p stop looping. \n",this);
    looping_ = false;
}

//退出事件循环  1.loop在自己的线程中调用quit  2.在非当前loop的线程中，调用loop的quit
/*
                    MainLoop
            
    subLoop1        subLoop2       subLoop3
*/
void EventLoop::quit()
{
    quit_ = true;

    if(!isInLoopThread())
    {
        wakeup(); //如果是在其他线程中，调用quit 在一个subloop(woker)中，调用了mainLoop(Io)的quit，唤醒主线程，主线程quit_为true，退出线程
    }
}

// 在当前loop中执行cb
void EventLoop::runInLoop(Functor cb)   //在当前的loop线程中，执行cb
{
    if(isInLoopThread())
    {
        cb();
    }
    else // 在非当前loop线程中执行cb，就需要唤醒loop所在线程，执行cb
    {
        queueInLoop(cb);
    }
}

// 把cb放入队列中，唤醒loop所在的线程，执行cb
void EventLoop::queueInLoop(Functor cb)
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(cb);
    }
    // 唤醒相应的相应的，需要执行上面回调操作的loop的线程
    // 或者 当前正在进行回调，又有了新的回调，唤醒loop所在线程，继续执行
    if(!isInLoopThread() || callingPendingFunctors_)  //
    {
        wakeup();   //唤醒loop所在线程  
    }
}
 
// 唤醒loop所在的线程  向wakeupfd_写一个数据 wakeupChannel就发生读事件，当前loop线程就会被唤醒
// 此处写啥读啥都没有问题，只是为了唤醒线程     
void EventLoop::wakeup()
{
    uint64_t one = 1;
    ssize_t n = write(wakeupFd_,&one,sizeof one);
    if(n != sizeof one)
    {
        LOG_ERROR("EventLoop::wakeup() writes %lu bytes instead of 8 \n",n);
    }
}

// EventLoop的方法 -> Poller的方法
void EventLoop::updateChannel(Channel* channel)
{
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel)
{
    poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel* channel)
{
    return poller_->hasChannel(channel);
}

//执行回调
void EventLoop::doPendingFunctors()
{
    std::vector<Functor> functors; //置换pendingFunctors的内容，避免内容过多，造成服务器时延
    callingPendingFunctors_ = true; 

    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }

    for(const Functor& functor : functors)
    {
        functor(); //执行当前loop需要执行的回调操作
    }
    callingPendingFunctors_ = false; 
} 