// 在多线程程序中调用fork函数
#include<pthread.h>
#include<unistd.h>
#include<stdio.h>
#include<stdlib.h>
#include<wait.h>

pthread_mutex_t mutex;

// 子线程运行的函数
// 它首先获得互斥锁mutex，然后暂停5s，再释放互斥锁
void* another(void* arg){
    printf("in child thread, lock the mutex\n");
    pthread_mutex_lock(&mutex);
    sleep(5);
    pthread_mutex_unlock(&mutex);
    printf("pthread is over\n");
}

void prepare(){ pthread_mutex_lock(&mutex); }
void infork(){ pthread_mutex_unlock(&mutex); }

int main(){
    pthread_mutex_init(&mutex, NULL);
    pthread_t id;
    pthread_create(&id, NULL, another, NULL);
    // 父进程中的主线程暂停1s，以确保在执行fork操作之前，子线程已经开始运行并获得了互斥变量mutex
    sleep(1);

    pthread_atfork(prepare, infork, infork);
    int pid = fork();
    if(pid < 0){
        pthread_join(id, NULL);
        pthread_mutex_destroy(&mutex);
        return 1;
    }
    else if(pid == 0){
        printf("I am in the child, want to get the lock\n");
        pthread_mutex_lock(&mutex);
        printf("I can not run to here, oop...\n");
        pthread_mutex_unlock(&mutex);
        exit(0);
    }
    else{
        wait(NULL);
    }
    pthread_join(id, NULL);
    pthread_mutex_destroy(&mutex);
    return 0;
}