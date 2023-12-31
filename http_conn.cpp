#include "http_conn.h"

const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

const char* doc_root = "/var/www/html";

int setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot){
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    
    // 是否要防止两个线程同时操作一个socket的局面
    if(one_shot){ event.events |= EPOLLONESHOT; }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev){
    epoll_event event;
    event.data.fd = fd;
    // 注意这里加上了EPOLLONESHOT，所以每次modfd都能刷新EPOLLONESHOT的作用
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close){
    if(real_close && (m_sockfd != -1)){
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;

    // 下面两行是为了避免TIME_WAIT状态，仅用于调试，实际使用时应去掉
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    addfd(m_epollfd, sockfd, true);
    m_user_count++;

    init();
}

void http_conn::init(){
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 从状态机，用于解析出某行的内容
http_conn::LINE_STATUS http_conn::parse_line(){
    // m_checked_index指向buffer（应用程序的读缓冲区）中当前正在分析的字节
    // m_read_index指向buffer中客户数据尾部的下一字节（即buffer中数据长度）
    char temp; // 存放当前分析字节的内容，临时变量
    for(; m_checked_idx < m_read_idx; ++m_checked_idx){
        // 我们一个字节一个字节的分析其中的内容
        temp = m_read_buf[m_checked_idx];

        // 如果当前字符是'\r'回车符，则说明可能读取到一个完整的行
        if(temp == '\r'){
            // 如果'\r'字符碰巧是目前buffer中的最后一个已经被读入的客户数据，那么这次分析没有读取到一个完整的行
            // 返回LINE_OPEN以表示还需要继续读取客户数据才能进一步分析
            if((m_checked_idx + 1) == m_read_idx){ return LINE_OPEN; }
            else if(m_read_buf[m_checked_idx + 1] == '\n'){
                // 如果下一个字符是'\n'，则说明我们成功读取到一个完整的行
                m_read_buf[m_checked_idx++] = '\0'; // '\r'替换成'\0'
                m_read_buf[m_checked_idx++] = '\0'; // '\n'替换成'\0'
                return LINE_OK; // 读取成功
            }
        }
        // 如果当前字符是'\n'换行符，则也说明可能读取到一个完整的行
        else if(temp == '\n'){
            if((m_checked_idx > 1) && (m_read_buf[m_checked_idx-1] == 'r')){
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            // 出现语法错误，直接返回错误
            return LINE_BAD;
        }
    }
    // 如果所有字节分析完毕也没遇到'\r'或者'\n'，则返回LINE_OPEN，表示还需要继续读取客户数据才能进一步分析
    return LINE_OPEN;
}

// 循环读取数据，直到无数据可读或者对方关闭连接
bool http_conn::read(){
    if(m_read_idx >= READ_BUFFER_SIZE){ return false; }
    int bytes_read = 0;
    while(true){
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if(bytes_read == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK){ break; }
            return false;
        }
        else if(bytes_read == 0){ return false; }
        m_read_idx += bytes_read;
    }
    return true;
}

// 解析HTTP请求行，获得请求方法、目标URL，以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text){
    m_url = strpbrk(text, " \t");
    if(!m_url){ return BAD_REQUEST; }
    *m_url++ = '\0';

    char* method = text;
    if(strcasecmp(method, "GET") == 0){ m_method = GET; }
    else{ return BAD_REQUEST; }

    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if(!m_version){ return BAD_REQUEST; }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if(strcasecmp(m_version, "HTTP/1.1") != 0){ return BAD_REQUEST; }
    if(strncasecmp(m_url, "http://", 7) == 0){
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if(!m_url || m_url[0] != '/'){ return BAD_REQUEST; }

    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text){
    // 遇到空行，表示头部字段解析完毕
    if(text[0] == '\0'){
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体
        // 状态机转移到CHECK_STATE_CONTENT状态
        if(m_content_length != 0){
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 头部字段解析完毕就可以去do_request操作然后将其返回给客户
        return GET_REQUEST;
    }
    // 处理Connection头部字段
    else if(strncasecmp(text, "Connection:", 11) == 0){
        text += 11;
        text += strspn(text, " \t");
        if(strcasecmp(text, "keep-alive") == 0){ m_linger = true; }
    }
    // 处理Content-Length头部字段
    else if(strncasecmp(text, "Content-Length:", 15) == 0){
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    // 处理头部字段
    else if(strncasecmp(text, "Host:", 5) == 0){
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else{
        printf("oop! unknow header %s\n", text);
    }

    return NO_REQUEST;
}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否完整读入
http_conn::HTTP_CODE http_conn::parse_content(char* text){
    if(m_read_idx >= (m_content_length + m_checked_idx)){
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，其分析参考8.6节
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    // m_check_state的默认值是CHECK_STATE_REQUESTLINE，见init()函数
    // 所以第一次进入循环取决于(line_status = parse_line()) == LINE_OK
    while(((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) \
       || ((line_status = parse_line()) == LINE_OK)){
        text = get_line(); // 因为parse_line()中把'\r'和'\n'替换成了'\0'，所以这里得到的text就是一行内容
        m_start_line = m_checked_idx; // 重置m_start_line的位置，下一次就是下一行的起点了
        printf("got 1 http line: %s\n", text);

        switch(m_check_state){
            // 请求消息分为3部分：
            // ①请求行（首行）
            // ②头部字段（占多行）
            // 空行分隔
            // ③消息体（占多行）

            case CHECK_STATE_REQUESTLINE:{
                // 第1次循环会先进入这里，解析请求行
                ret = parse_request_line(text);

                // 只要没出现解析异常就可以申请继续读取数据进一步分析了
                if(ret == BAD_REQUEST){ return BAD_REQUEST; }
                break;
            }
            case CHECK_STATE_HEADER:{
                ret = parse_headers(text);
                if(ret == BAD_REQUEST){ return BAD_REQUEST; }
                else if(ret == GET_REQUEST){ return do_request(); }
                break;
            }
            case CHECK_STATE_CONTENT:{
                ret = parse_content(text);
                if(ret == GET_REQUEST){ return do_request(); }
                line_status = LINE_OPEN;
                break;
            }
            default:{ return INTERNAL_ERROR; }
        }
    }
    // 只要没出现解析异常就可以申请继续读取数据进一步分析了
    return NO_REQUEST;
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性
// 如果目标文件存在、对所有用户可读，且不是目录
// 则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request(){
    strcpy(m_real_file, doc_root); // doc_root -> m_real_file
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    
    if(stat(m_real_file, &m_file_stat) < 0){ return NO_RESOURCE; }
    if(!(m_file_stat.st_mode & S_IROTH)){ return FORBIDDEN_REQUEST; }
    if(S_ISDIR(m_file_stat.st_mode)){ return BAD_REQUEST; }

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST; // 我们只能正确处理这一种情况
}

// 对内存映射区执行munmap操作
void http_conn::unmap(){
    if(m_file_address){
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 写HTTP响应
bool http_conn::write(){
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx;


    if(bytes_to_send == 0){
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init(); // 重开
        return true;
    }

    while(1){
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if(temp <= -1){
            // 如果TCP没有写缓存空间，则等待下一轮的EPOLLOUT事件
            // 虽然在此期间服务器无法立即接收到同一客户的下一请求，但这可以保证连接的完整性
            if(errno == EAGAIN){
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            // 其它情况就是出错了
            unmap();
            return false;
        }

        // 这块不太理解

        bytes_to_send -= temp;
        bytes_have_send += temp;

        // 我的理解：下面的代码是指只要temp != 1且bytes_to_send <= bytes_have_send那么数据就是传完了
        if(bytes_to_send <= bytes_have_send){
            unmap();
            if(m_linger){
                init();
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return true;
            }
            else{
                modfd(m_epollfd, m_sockfd, EPOLLIN); // 这句我觉得多余了
                return false;
            }
        }
    }
}

// 往写缓冲中写入待发送数据
bool http_conn::add_response(const char* format, ...){
    if(m_write_idx >= WRITE_BUFFER_SIZE){ return false; }

    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if(len >= (WRITE_BUFFER_SIZE - 1- m_write_idx)){ return false; }

    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char* title){
    add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len){
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len){
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger(){
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line(){
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char* content){
    return add_response("%s", content);
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret){
    switch(ret){
        case INTERNAL_ERROR:{
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form)){ return false; }
            break;
        }
        case BAD_REQUEST:{
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form)){ return false; }
            break;
        }
        case NO_RESOURCE:{
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form)){ return false; }
            break;
        }
        case FORBIDDEN_REQUEST:{
            add_status_line(403, error_403_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_403_form)){ return false; }
            break;
        }
        case FILE_REQUEST:{
            add_status_line(200, ok_200_title);
            if(m_file_stat.st_size != 0){
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                return true;
            }
            else{
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if(!add_content(ok_string)){ return false; }
            }
        }
        default:{ return false; }
    }
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process(){
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST){
        // 请求不完整，还要读取更多数据，所以需要监听对应socket接收的消息
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    bool write_ret = process_write(read_ret);
    if(!write_ret){
        close_conn();
    }

    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}

