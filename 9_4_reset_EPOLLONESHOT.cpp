#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <pthread.h>

#define MAX_EVENT_NUMBER 1024 // 最大事件数量
#define BUFFER_SIZE 1024      // 缓冲区大小

struct fds
{
    int epollfd; // 监听epollfd，内核事件表
    int sockfd;  // 用户fd
};

/**
 * 将fd设置为非阻塞
 */
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);      // 先保留先前的设置
    int new_option = old_option | O_NONBLOCK; // 在旧的事件标志基础上加上新的非阻塞标志
    fcntl(fd, F_SETFL, new_option);           // 进行设置
    return old_option;                        // 返回旧的事件标志
}

/**
 * 将fd上的EPOLLIN和EPOLLET事件注册到epollfd指示的epoll内核事件表中
 * 参数oneshot指定是否注册fd上的EPOLLONESHOT事件
 */
void addfd(int epollfd, int fd, bool oneshot)
{
    epoll_event event;                // 设置一个epoll_event结构体临时变量
    event.data.fd = fd;               // epoll_data联合体里我们使用了fd
    event.events = EPOLLIN | EPOLLET; // 添加基本的监听事件：可读事件、ET模式
    if (oneshot)
    {
        event.events |= EPOLLONESHOT; // 添加EPOLLONESHOT事件
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

/**
 * 重置fd上的EPOLLONESHOT事件
 *
 * 注册了 EPOLLONESHOT 事件的 socket 一旦被某个线程处理完毕,该线程就应该立即重置这个socket 上的 EPOLLONESHOT 事件,
 * 以确保这个socket 下一次可读时,其 EPOLLIN 事件能被触发,进而让其他工作线程有机会继续处理这个 socket。
 */
void reset_oneshot(int epollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 工作线程
void *worker(void *arg)
{
    int sockfd = ((fds *)arg)->sockfd;
    int epollfd = ((fds *)arg)->epollfd;
    printf("start new thread to receive data on fd: %d\n", sockfd);
    char buf[BUFFER_SIZE];
    memset(buf, '\0', BUFFER_SIZE);

    // 循环读取sockfd上的数据，直到出现EAGAIN错误（表示读完）
    while (1)
    {
        int ret = recv(sockfd, buf, BUFFER_SIZE - 1, 0);
        if (ret == 0)
        {
            close(sockfd);
            printf("foreiner closed the connection\n");
            break;
        }
        else if (ret < 0)
        {
            if (errno == EAGAIN)
            {
                reset_oneshot(epollfd, sockfd);
                printf("read_later\n");
                break;
            }
        }
        else
        {
            printf("get content: %s\n", buf);
            sleep(5); // 休眠5秒，模拟数据处理过程
        }
    }
    printf("end thread receiving data on fd : %d\n", sockfd);
}

int main(int argc, char *argv[])
{
    if (argc <= 2)
    {
        printf("usage: %s IP PORT\n", basename(argv[0]));
        return 1;
    }
    const char *ip = argv[1]; // 用字符数组保存IP地址
    int port = atoi(argv[2]); // 用于将一个字符串（string）转换为整数（integer），atoi 的名称代表 "ASCII to Integer"

    int ret = 0;
    /**
     * sockaddr_in 是一个在网络编程中常用的数据结构，用于表示 Internet 地址（IPv4 地址）和端口号的信息

        struct sockaddr_in {
            short sin_family;      // 地址族，通常为 AF_INET
            unsigned short sin_port; // 端口号，以网络字节序表示
            struct in_addr sin_addr; // IPv4 地址
            char sin_zero[8];       // 未使用，填充 0
        };
    */
    struct sockaddr_in address;
    /**
     * bzero 是一个早期的用于将一块内存区域清零的函数，通常用于将一段内存区域初始化为零。
     * 它的功能与 memset 函数类似，但 bzero 已经被标记为废弃（deprecated），不再推荐使用。
     * 取而代之，应该使用 memset 函数或其他更现代的内存操作函数。
     */
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;              // IPv4地址族
    inet_pton(AF_INET, ip, &address.sin_addr); // 将用户提供的IP地址字符串转换为socketadd_in地址结构中使用的二进制形式。
    address.sin_port = htons(port);            // 将用户提供的端口转化为网络字节序（大端）。

    int listenfd = socket(PF_INET, SOCK_STREAM, 0); // AF_INET 和 PF_INET 通常可以互换使用，因为它们具有相同的值
    assert(listenfd >= 0);

    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret != -1);

    ret = listen(listenfd, 5); // 全连接队列？
    assert(ret != -1);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5); // 这个5没啥太大意义
    assert(epollfd != -1);

    /**
     * 注意，监听socket：listenfd上是不能注册EPOLLONESHOT事件的，否则应用程序只能处理一个客户连接
     * 因为后续的客户连接请求将不再触发listenfd上的EPOLLIN事件
     */
    addfd(epollfd, listenfd, false);

    while (1)
    {
        int ret = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (ret < 0)
        {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < ret; ++i)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                addfd(epollfd, connfd, true); // 对每个非监听文件描述符都注册EPOLLONESHOT事件
            }
            else if (events[i].events & EPOLLIN)
            {
                pthread_t thread;
                fds fds_for_new_worker;
                fds_for_new_worker.epollfd = epollfd;
                fds_for_new_worker.sockfd = sockfd;
                pthread_create(&thread, NULL, worker, (void *)&fds_for_new_worker); // 新启动一个工作线程为sockfd服务
            }
            else
            {
                printf("something else happened\n");
            }
        }
    }
    close(listenfd);
    return 0;
}