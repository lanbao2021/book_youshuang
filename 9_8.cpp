// 同时处理TCP请求和UDP请求的回射服务器
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<assert.h>
#include<stdio.h>
#include<unistd.h>
#include<errno.h>
#include<string.h>
#include<fcntl.h>
#include<stdlib.h>
#include<sys/epoll.h>
#include<pthread.h>

#define MAX_EVENT_NUMBER 1024
#define TCP_BUFFER_SIZE 512
#define UDP_BUFFER_SIZE 1024

int setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd){
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET; // 对fd启用ET模式
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
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
    address.sin_port = htons(port); // 这里的port是TCP的端口号，跟后面的UDP一样

    // 创建TCP socket，并将其绑定到端口号上
    int listenfd = socket(PF_INET, SOCK_STREAM, 0); // SOCK_STREAM表示TCP socket
    assert(listenfd >= 0);

    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret != -1);

    ret = listen(listenfd, 5);
    assert(ret != -1);

    // 创建UDP socket，并将其绑定到端口port上
    // 这个address感觉没有必要再初始化一次吧，跟前面的内容一样
    // 不知道还有没有深层次的原因？
    bzero(&address, sizeof(address)); 
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port); // 这里的port是UDP的端口号，跟前面的TCP一样
    int udpfd = socket(PF_INET, SOCK_DGRAM, 0); // SOCK_DGRAM表示UDP socket
    assert(udpfd >= 0);

    ret = bind(udpfd, (struct sockaddr*)&address, sizeof(address)); // 绑定端口
    assert(ret != -1);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5); // 创建epoll对象，参数5被忽略
    assert(epollfd != -1);

    // 注册TCP sock和UDP socket上的可读事件
    addfd(epollfd, listenfd);
    addfd(epollfd, udpfd);


    while (1)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if(number < 0){
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
            else if(sockfd == udpfd){
                char buf[UDP_BUFFER_SIZE];
                memset(buf, '\0', UDP_BUFFER_SIZE);
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);

                ret = recvfrom(udpfd, buf, UDP_BUFFER_SIZE-1, 0, (struct sockaddr*)&client_address, &client_addrlength); // 读取数据，UDP连接需要提供地址信息
                if(ret > 0){
                    sendto(udpfd, buf, UDP_BUFFER_SIZE-1, 0, (struct sockaddr*)&client_address, client_addrlength); // 发送数据，UDP连接需要提供地址信息
                }
            }
            else if(events[i].events & EPOLLIN){ // TCP连接来数据了
                char buf[TCP_BUFFER_SIZE];
                // 为啥上面的UDP读数据只要recvfrom一次就够了？
                while(1){
                    memset(buf, '\0', TCP_BUFFER_SIZE);
                    ret = recv(sockfd, buf, TCP_BUFFER_SIZE-1, 0);
                    if(ret < 0){
                        if((errno == EAGAIN) || (errno == EWOULDBLOCK)){ break; } // 下次再读
                        close(sockfd);
                        break;
                    }
                    else if(ret == 0){
                        close(sockfd);
                    }
                    else{
                        send(sockfd, buf, ret, 0);
                    }
                }
            }
            else{
                printf("something else happened\n");
            }
        }
    }
    close(listenfd);
    return 0;
}