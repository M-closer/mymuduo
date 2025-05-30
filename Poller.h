#pragma once

#include "noncopyable.h"
#include "Timestamp.h"

#include<unordered_map>
#include<vector>
#include<map>

class Channel;
class EventLoop;

//muduo库中多路事件分发器的核心IO复用模块
class Poller : noncopyable
{
public:
    using ChannelList = std::vector<Channel*>;
    
    Poller(EventLoop *loop);
    virtual ~Poller() = default; 

    //给所有IO复用保留统一的接口
    virtual Timestamp poll(int timeoutMs,ChannelList* activeChannels) = 0;
    virtual void updateChannel(Channel* channel) = 0;
    virtual void removeChannel(Channel* Channel) = 0;

    //判断参数channel 是否在当前poller中
    bool hasChannel(Channel* channel) const;

    //EventLoop可以通过该接口获取默认的IO复用的具体实现(poll or epoll or select)
    static Poller* newDefaultPoller(EventLoop* loop); 
protected:
    //map的key就是sockfd   value: sockfd所属的通道类型
    using ChannelMap = std::unordered_map<int,Channel*>;
    // typedef std::map<int,Channel*> ChannelMap;
    ChannelMap channels_;

private:
    EventLoop* ownerLoop_; //定义Poller所属的事件循环EventLoop 
};