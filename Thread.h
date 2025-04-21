#pragma once

#include "noncopyable.h"

#include <functional>
#include <string>
#include <thread>
#include <memory>
#include <unistd.h>
#include <mutex>
#include <atomic>

class Thread : noncopyable
{
public:
    using ThreadFunc = std::function<void()>;

    explicit Thread(ThreadFunc,const std::string& name = std::string());

    ~Thread();

    void start();
    void join();

    bool started() { return started_; }
    pid_t tid() const { return tid_; }
    const std::string& name() const { return name_; }

    static int numCreated() { return numCreated_; }
private:
    void setDefaultName();

    bool started_;
    bool joined_;
    std::shared_ptr<std::thread> thread_;//thread创建直接启动线程，所以需要使用智能指针封装，可以自己决定是否启动
    pid_t tid_;
    ThreadFunc func_; //存储线程函数
    std::string name_; 
    static std::atomic_int numCreated_;

}; 