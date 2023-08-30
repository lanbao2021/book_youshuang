// 统一事件源
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<assert.h>
#include<stdio.h>
#include<signal.h>
#include<unistd.h>
#include<errno.h>
#include<string.h>
#include<fcntl.h>
#include<stdlib.h>
#include<sys/epoll.h>
#include<pthread.h>

#define MAX_EVENT_NUMBER 1024
static int pipefd[2];

int setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd){
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 信号处理函数
void sig_handler(int sig){
    // 保留原来的errno，在函数的最后回复，以保证函数的可重入性
    int save_errno = errno;
    int msg = sig;

    // 将信号值写入管道以通知主循环
    send(pipefd[1], (char*)&msg, 1, 0); 
    errno = save_errno;
}

// 设置信号的处理函数
void addsig(int sig){
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = sig_handler; // 设置信号处理函数
    sa.sa_flags |= SA_RESTART; // 信号处理完会重新执行被中断的系统调用
    sigfillset(&sa.sa_mask); // 在信号集中设置所有信号，所以的信号都被加入掩码？
    assert(sigaction(sig, &sa, NULL) != -1); // sigaction为sig设置它的信号处理函数
}

int main(int argc, char* argv[]){
    if(argc <= 2){
        printf("usage: %s IP PORT\n", basename(argv[0]));
        return 1;
    }

    const char* ip = argv[1];
    int port = atoi(argv[2]);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    if(ret == -1){
        printf("errno is %d\n", errno);
        return 1;
    }
    ret = listen(listenfd, 5);
    assert(ret != -1);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    addfd(epollfd, listenfd);

    // 使用sockpair创建管道，注册pipefd[0]上的可读事件
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]); // 管道的写端为啥要设置非阻塞呢？是因为读端是非阻塞的原因吗
    addfd(epollfd, pipefd[0]);

    // 设置一些信号的处理函数
    addsig(SIGHUP); // 控制终端挂起，不是很懂啥意思
    addsig(SIGCHLD); // 子进程状态发生变化（退出、暂停）
    addsig(SIGTERM); // 终止进程，kill命令默认发送的信号就是SIGTERM
    addsig(SIGINT); // 键盘输入以中断进程（CTRL + C)
    bool stop_server = false;

    while(!stop_server){
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if((number < 0) && (errno != EINTR)){
            printf("epoll failure\n");
            break;
        }

        for(int i=0; i<number; ++i){
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd){
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);
                addfd(epollfd, connfd);
            }
            else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)){
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1){ continue; }
                else if(ret == 0){ continue; }
                else{
                    // 因为每个信号值占1字节，所以按字节来逐个接收信号
                    // 我们以SIGTERM为例，来说明如何安全地终止服务器主循环
                    for(int i=0; i<ret; ++i){
                        switch (signals[i])
                        {
                        case SIGCHLD: 
                        case SIGHUP: { continue; }
                        case SIGTERM:
                        case SIGINT:{ stop_server = true; }
                        }
                    }
                }
            }
            else{}
        }
    }
    printf("close fds\n");
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    return 0;
}