// 聊天室客户端程序
#define _GNU_SOURCE 1
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<assert.h>
#include<stdio.h>
#include<unistd.h>
#include<string.h>
#include<stdlib.h>
#include<poll.h>
#include<fcntl.h>

#define BUFFER_SIZE 64

int main(int argc, char* argv[]){
    if(argc <= 2){
        printf("usage: %s IP PORT\n", basename(argv[0]));
        return 1;
    }

    const char* ip = argv[1];
    int port = atoi(argv[2]);

    struct sockaddr_in server_address;
    bzero(&server_address, sizeof(server_address));
    server_address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &server_address.sin_addr);
    server_address.sin_port = htons(port);

    int sockfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(sockfd >= 0);
    if(connect(sockfd, (struct sockaddr*)&server_address, sizeof(server_address)) < 0){
        printf("connection failed\n");
        close(sockfd);
        return 1;
    }

    pollfd fds[2];
    // 注册文件描述符0（标准输入）和文件描述符sockfd上的可读事件
    fds[0].fd = 0; // 0代表标准输入
    fds[0].events = POLLIN;  // POLLIN代表监听文件描述符上是否有可读数据
    fds[0].revents = 0;
    fds[1].fd = sockfd; 
    fds[1].events = POLLIN | POLLRDHUP; // 监听文件描述符上是否有可读数据(POLLIN) + 对方关闭连接的请求(POLLRDHUP)
    fds[1].revents = 0;

    char read_buf[BUFFER_SIZE];
    int pipefd[2];
    int ret = pipe(pipefd); // 注册管道pipefd，其中pipefd[0]用于读数据，pipefd[1]用于写数据
    assert(ret != -1);

    while(1){
        ret = poll(fds, 2, -1); // poll监听文件描述符集合fds，2表示描述符数量，-1表示阻塞监听
        if(ret < 0){
            printf("poll failure\n");
            break;
        }
        if(fds[1].revents & POLLRDHUP){ 
            // 监听到消息后内核会修改对应文件描述的rvents参数
            // 该判断语句用于监听服务器端是否关闭连接
            printf("server close the connection\n");
            break;
        }
        else if(fds[1].revents & POLLIN){
            // 该判断语句用于监听与服务器连接的socket文件描述符是否接收到来自服务器的传送的数据
            memset(read_buf, '\0', BUFFER_SIZE); // 先初始化接收缓冲区
            recv(fds[1].fd, read_buf, BUFFER_SIZE - 1, 0); // 接收数据
            printf("%s\n", read_buf);
        }
        if(fds[0].revents & POLLIN){
            // 使用splice将用户输入的数据直接写到sockfd上，零拷贝
            ret = splice(0, NULL, pipefd[1], NULL, 32768, SPLICE_F_MORE | SPLICE_F_MOVE);
            ret = splice(pipefd[0], NULL, sockfd, NULL, 32768, SPLICE_F_MORE | SPLICE_F_MOVE);
        }
    }

    close(sockfd);
    return 0;
}