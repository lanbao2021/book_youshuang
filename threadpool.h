/* 半同步/半反应堆线程池 */
/* 使用一个工作队列完全解除了主线程和工作线程之间的耦合关系 */
/* 主线程往工作队列中插入任务，工作线程通过竞争来取得任务并执行它 */
/* 不过我们必须保证所有客户请求是状态无关的，因为同一个连接上的不同请求可能会由不同线程处理 */
#ifndef THREADPOLL_H
#define THREADPOLL_H

#include<list>
#include<cstdio>
#include<exception>
#include<pthread.h>

/* 14章介绍的线程同步机制的包装类 */
#include "locker.h" 

/* 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类 */
template<typename T>
class threadpool{
    public:
        /* thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求数量 */
        threadpool(int thread_number = 8, int max_requests = 10000);
        ~threadpool();
        bool append(T* request); // 往请求队列加任务

    private:
        /* 工作线程运行的函数，它不断从工作队列中取出任务并执行它 */
        static void* worker(void* arg);
        void run();

    private:
        int m_thread_number; // 线程池中线程数
        int m_max_requests; // 请求队列中允许的最大请求数
        pthread_t* m_threads; // 描述线程池的数组，其大小为m_thread_number
        std::list<T*> m_workqueue; // 请求队列
        locker m_queuelocker; // 保护请求队列的互斥锁
        sem m_queuestat; // 是否有任务需要处理
        bool m_stop; // 是否结束线程
};


template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests):\
m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL){
    if((thread_number <= 0) || (max_requests <= 0)){ throw std::exception(); }

    // 为线程池数组分配空间
    m_threads = new pthread_t(m_thread_number);
    if(m_threads){ throw std::exception(); }

    // 创建thread_number个线程，并将它们都设置为脱离线程
    for(int i=0; i<thread_number; ++i){
        printf("create the %dth thread\n", i);
        /* C++中使用pthread_create函数时第3个参数要是static函数
        但是，static函数不能调用类中的动态成员函数、成员，所以可以给它传递一个this指针 */
        if(pthread_create(m_threads + i, NULL, worker, this) != 0){
            // pthread_create成功时返回0，其余情况抛出异常
            delete [] m_threads;
            throw std::exception();
        }
        if(pthread_detach(m_threads[i])){
            // 脱离线程在退出时会自行释放其所占用的系统资源
            delete [] m_threads;
            throw std::exception();
        }
    }
}


template<typename T>
threadpool<T>::~threadpool(){
    delete [] m_threads;
    m_stop = true;
}


template<typename T>
bool threadpool<T>::append(T* request){
    // 操作工作队列时一定要加锁，因为它被所有线程共享
    m_queuelocker.lock();
    if(m_workqueue.size() > m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

/* static可以只在声明里写，实现部分可以不加static关键字了 */
template<typename T>
void* threadpool<T>::worker(void* arg){
    threadpool* pool = (threadpool*)arg;
    pool->run();
    return pool;
}

template<typename T>
void threadpool<T>::run(){
    while(!m_stop){
        m_queuestat.wait();
        m_queuelocker.lock();
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request){ continue; }
        request->process();
    }
}

#endif