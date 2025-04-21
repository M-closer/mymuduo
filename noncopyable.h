#pragma once

/*
noncopyable 被继承后，派生类对象可以正常的构造和析构，但是无法进行拷贝
构造和赋值操作
*/

class noncopyable
{
public:
    noncopyable(const noncopyable&) = delete;
    noncopyable& operator=(const noncopyable&) = delete;
protected:
    noncopyable() = default;
    ~noncopyable() = default;
};