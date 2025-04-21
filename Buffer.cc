#include "Buffer.h"

#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>

//从fd上读取数据 poller工作在LT模式
//Buffer缓冲区是有大小的，但是从fd上读数据时，不知道tcp数据的最终大小
ssize_t Buffer::readFd(int fd,int* savedErrno)
{
    char extrabuf[65536] = {0}; //栈上内存空间
    struct iovec vec[2];    
    const size_t writable = writableBytes();  //这是Buffer底层缓冲区剩余的可写空间大小
    vec[0].iov_base = begin()+writerIndex_;
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);    
    //判断是在一个缓冲区写完了，还是两个缓冲区都有写
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    const ssize_t n = ::readv(fd,vec,iovcnt);

    if(n <= 0)
    {
        *savedErrno = errno;
    }
    else if (n <= writable)         //Buffer的可写缓冲区已经够存储读出来的数据了
    {
        writerIndex_ += n;
    }
    else  //n>writable标明，extrabuf也写入了数据
    {
        writerIndex_ = buffer_.size();
        append(extrabuf,n-writable);
    }

    return n;
}

ssize_t Buffer::writeFd(int fd,int *savedErrno)
{
    ssize_t n = ::write(fd,peek(),readableBytes());

    if(n < 0)
    {
        *savedErrno = errno;
    }
    return n;
}
