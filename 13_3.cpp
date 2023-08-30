// 使用IPC_PRIVATE信号量
#include<sys/sem.h>
#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<sys/wait.h>

union semun{
    int val; // 用于SETVAL命令
    struct semid_ds* buf; // 用于IPC_STAT和IPC_SET命令
    unsigned short int* array; // 用于GETALL和SETALL命令
    struct seminfo* __buf; // 用于IPC_INFO命令
};

// op为-1时执行P操作，表示对信号量值进行减操作，即期望获得信号量
// op为1时执行V操作
void pv(int sem_id, int op){
    struct sembuf sem_b;
    sem_b.sem_num = 0; // sem_num成员是信号量编号，0表示信号量集中的第一个信号量
    sem_b.sem_op = op; // sem_op指定操作类型，可选值为：正整数、0和负整数
    sem_b.sem_flg = SEM_UNDO; // sem_op的行为受到sem_flg的影响
    semop(sem_id, &sem_b, 1);
}

int main(int argc, char* argv[]){
    int sem_id = semget(IPC_PRIVATE, 1, 0666);
    
    union semun sem_un;
    sem_un.val = 1;
    // 第二参数sem_num=0，表示被操作信号量在信号量集中的编号为0
    // 第三参数SETVAL使用第四参数sem_un作为实参
    semctl(sem_id, 0, SETVAL, sem_un); 

    pid_t id = fork();
    if(id < 0){ return 1; }
    else if(id == 0){
        printf("child try to get binary sem\n");
        // 在父子进程间共享IPC_PRIVATE信号量的关键就在于二者都可以操作该信号量的标识符sem_id
        pv(sem_id, -1);
        printf("child get the sem and would release it after 5 seconds\n");
        sleep(5);
        pv(sem_id, 1);
        exit(0);
    }
    else{
        printf("parent try to get binary sem\n");
        pv(sem_id, -1);
        printf("parent get the sem and would release it after 5 seconds\n");
        sleep(5);
        pv(sem_id, 1);
    }
    waitpid(id, NULL, 0);
    semctl(sem_id, 0, IPC_RMID, sem_un); // 删除信号量
    return 0;
}