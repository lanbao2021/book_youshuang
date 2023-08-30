// 非阻塞connect
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<stdlib.h>
#include<assert.h>
#include<stdio.h>
#include<time.h>
#include<errno.h>
#include<fcntl.h>
#include<sys/ioctl.h>
#include<unistd.h>
#include<string.h>

#define BUFFER_SIZE 1023

int setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 超时连接函数，参数分别是服务器IP地址、端口号和超时时间（毫秒）
// 函数成功时返回已经处于连接状态的socket，失败则返回-1
int unblock_connect(const char* ip, int port, int time){
    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    int sockfd = socket(PF_INET, SOCK_STREAM, 0);
    int fdopt = setnonblocking(sockfd);
    ret = connect(sockfd, (struct sockaddr*)&address, sizeof(address));
    if(ret == 0){
        // 如果连接成功，则恢复sockfd的属性，并立即返回它
        printf("connect with server immediately\n");
        fcntl(sockfd, F_SETFL, fdopt);
        return sockfd;
    }
    else if(errno != EINPROGRESS){
        // 如果连接没有立即建立，那么只有当errno是EINPROGRESS时才表示连接还在进行
        printf("unblock connect not support\n");
        return -1;
    }
    
    // fd_set readfds;
    fd_set writefds;
    struct timeval timeout;

    FD_ZERO(&writefds);
    FD_SET(sockfd, &writefds);

    timeout.tv_sec = time;
    timeout.tv_usec = 0;

    ret = select(sockfd + 1, NULL, &writefds, NULL, &timeout);
    if(ret <= 0){
        // select出错、超时，立即返回
        printf("connection time out\n");
        close(sockfd);
        return -1;
    }
    if(!FD_ISSET(sockfd, &writefds)){
        printf("no events on sockfd found\n");
        close(sockfd);
        return -1;
    }

    int error = 0;
    socklen_t length = sizeof(error);
    // 调用getsockopt获取并清除sockfd上的错误
    if(getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &length) < 0){
        printf("get socket option failed \n");
        close(sockfd);
        return -1;
    }

    // 错误号不为0表示连接出错
    if(error != 0){
        printf("connection failed after select with the error: %d\n", error);
        close(sockfd);
        return -1;
    }

    // 连接成功
    printf("connection ready after select with the socket: %d\n", sockfd);
    fcntl(sockfd, F_SETFL, fdopt);
    return sockfd;
}