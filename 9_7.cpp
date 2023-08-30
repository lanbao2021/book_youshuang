// 聊天室服务器程序
#define _GNU_SOURCE 1
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
#include<poll.h>

#define USER_LIMIT 5 // 最大用户数量
#define BUFFER_SIZE 64 // 读缓冲区的大小
#define FD_LIMIT 65535 // 文件描述符数量的限制

// 客户数据：客户端socket地址、待写到客户端的数据、从客户端读入的数据
struct client_data{
    sockaddr_in address;
    char* write_buf;
    char buf[BUFFER_SIZE];
};

int setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
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
    assert(ret != -1);

    ret = listen(listenfd, 5);
    assert(ret != -1);

    // 创建users数组，分配FD_LIMIT个client_data对象
    // 可以预期，每个可能的socket连接都可以获得一个这样的对象
    // 并且socket的值可以直接用来索引（作为数组下标） socket连接对应的client_data对象
    // 这是将socket和客户数据相关联的简单而高效的方式
    client_data* users = new client_data[FD_LIMIT];
    // 尽管我们分配了足够多的client对象，但为了提高poll性能，仍然有必要限制用户数量
    pollfd fds[USER_LIMIT + 1]; // 之所以 +1 是因为还要监听listenfd对应的文件描述符

    int user_counter = 0; // 该变量用来记录当前的用户连接数量

    // 初始化一下fds
    for(int i=1; i<=USER_LIMIT; ++i){
        fds[i].fd = -1;
        fds[i].events = 0;
    }
    fds[0].fd = listenfd;
    fds[0].events = POLLIN | POLLERR; // 监听listenfd上可读数据、错误数据，可读数据其实就是新的用户连接来了
    fds[0].revents = 0;

    while(1){
        ret = poll(fds, user_counter + 1, -1); // 注意这里的 user_counter + 1 会动态变化
        if(ret < 0){
            printf("poll failure\n");
            break;
        }

        // 我们顺序记录新的用户连接到fds中，所以遍历的时候也是顺序遍历即可
        for(int i=0; i<user_counter+1; ++i){
            // 情况一：监听到了listenfd上的可读数据，即来新用户了
            if((fds[i].fd == listenfd) && (fds[i].revents & POLLIN)){
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);

                if(connfd < 0){
                    printf("errno is : %d\n", errno);
                    continue;
                }

                // 如果请求太多，则关闭新到的连接
                if(user_counter >= USER_LIMIT){
                    const char* info = "too many users\n";
                    printf("%s", info);
                    send(connfd, info, strlen(info), 0);
                    close(connfd);
                    continue;
                }

                // 对于新的连接，同时修改fds和users数组
                // 前文提到users[connfd]对应于新连接文件描述符connfd的客户数据
                user_counter++;
                users[connfd].address = client_address;
                setnonblocking(connfd);
                fds[user_counter].fd = connfd;
                fds[user_counter].events = POLLIN | POLLRDHUP | POLLERR;
                fds[user_counter].revents = 0;
                printf("comes a new user, now have %d users\n", user_counter);
            }
            
            // 情况二：监听到任意文件描述符的错误信息
            else if(fds[i].revents & POLLERR){
                printf("get an error from %d\n", fds[i].fd);
                char errors[100];
                memset(errors, '\0', 100);
                socklen_t length = sizeof(errors);
                if(getsockopt(fds[i].fd, SOL_SOCKET, SO_ERROR, &errors, &length) < 0){
                    printf("get socket option failed\n");
                }
                continue;
            }
            
            // 情况三：监听到对方关闭连接
            else if(fds[i].revents & POLLRDHUP){
                // 如果客户端关闭连接，则服务器也关闭对应的连接，并将用户总数减1
                // 0 1(off) 2 3 -> 0 3 2(i=0)
                users[fds[i].fd] = users[fds[user_counter].fd]; // 当前client离线后把末尾的client对应的client_data数据移到当前位置
                close(fds[i].fd); // 关闭client
                fds[i] = fds[user_counter]; // 当前client离线后把末尾的client对应的fds数据移到当前位置
                i--; // i退回上一位
                user_counter--; // 用户数量减 1
                printf("a client left\n");
            }
            
            // 情况四：监听到对方发送数据
            else if(fds[i].revents & POLLIN){
                int connfd = fds[i].fd;
                memset(users[connfd].buf, '\0', BUFFER_SIZE);
                ret = recv(connfd, users[connfd].buf, BUFFER_SIZE-1, 0);
                printf("get %d bytes of client data %s from %d\n", ret, users[connfd].buf, connfd);
                if(ret < 0){
                    // 如果读操作出错，则关闭连接
                    if(errno != EAGAIN){
                        close(connfd);
                        users[fds[i].fd] = users[fds[user_counter].fd];
                        fds[i] = fds[user_counter];
                        i--;
                        user_counter--;
                    }
                }
                else if(ret == 0){ }
                else{
                    // 接收到客户端数据，通知其他socket连接准备写数据
                    for(int j=1; j<=user_counter; ++j){
                        if(fds[j].fd == connfd){ continue; }
                        fds[j].events |= ~POLLIN; // 不监听对用户连接的可读时机了
                        fds[j].events |= POLLOUT; // 监听服务器向用户连接可写的时机，并初始化write_buf指针
                        users[fds[j].fd].write_buf = users[connfd].buf;
                    }
                }
            }
            
            // 情况四：监听到我方写数据
            else if(fds[i].revents & POLLOUT){
                int connfd = fds[i].fd;
                if(!users[connfd].write_buf){ continue; }
                ret = send(connfd, users[connfd].write_buf, strlen(users[connfd].write_buf), 0);
                users[connfd].write_buf = NULL;
                // 写完数据后需要重新注册fds[i]上的可读事件
                fds[i].events |= ~POLLOUT; // 不监听对用户连接的可写时机了
                fds[i].events |= POLLIN;
            }
        }
    }
    delete [] users;
    close(listenfd);
    return 0;
}