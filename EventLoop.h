#pragma once

#include "noncopyable.h"
#include "Timestamp.h"  
#include "Channel.h"
#include "CurrentThread.h"
 
#include <functional>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>

// Reator, at most one per thread
class Poller;
// class Channel;

//时间循环类 主要包括了两大模块 Channel Poller(epoll的抽象)
class EventLoop : noncopyable
{
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    //开启事件循环
    void loop();  
    //退出事件循环  
    void quit();

    Timestamp pollReturnTime() const { return pollReturnTime_;}

    // 在当前loop中执行cb
    void runInLoop(Functor cb);
    // 把cb放入队列中，唤醒loop所在的线程，执行cb
    void queueInLoop(Functor cb);

    // 唤醒loop所在的线程
    void wakeup();

    // EventLoop的方法 -> Poller的方法
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);
    bool hasChannel(Channel* channel);

    // 判断EventLoop对象是否在自己的线程里面
    bool isInLoopThread() const { return threadId_ == CurrentThread::tid(); }
private:
    void handleRead(); //唤醒wakeup
    void doPendingFunctors();  //执行回调

    using ChannelList = std::vector<Channel*>;

    std::atomic_bool looping_; //原子操作，通过CAS实现的
    std::atomic_bool quit_; //标识退出loop循环 
    
    const pid_t threadId_; //记录当前loop所在的线程id
    Timestamp pollReturnTime_; //poller返回发生事件的channels的时间点
    std::unique_ptr<Poller> poller_;
    /*
        eventfd()，采用的是线程间的通讯机制 muduo
        socketpair，主loop和子loop都创建socketpair，双向通信，走的网络通信libevent
    */
    int wakeupFd_; //eventfd(),当mainLoop获取一个新用户的channel，通过轮询算法选择一个subLoop，通过该成员唤醒subloop处理channel
    std::unique_ptr<Channel> weakupChannel_;

    ChannelList activeChannels_;

    std::atomic_bool callingPendingFunctors_; //标识当前loop是否有需要执行的回调操作
    std::vector<Functor> pendingFunctors_; //存储loop所有需要执行的回调操作
    std::mutex mutex_;  //用来保护上面vector容器的线程安全操作
};