#include "webserver.h"

WebServer::WebServer()
{
    // 创建http_conn类的对象数组，每个对象代表一个客户端连接
    users = new http_conn[MAX_FD];

    // 获取当前工作目录路径，并拼接"/root"作为服务器根目录
    char server_path[200];
    getcwd(server_path, 200); // 获取当前工作目录的绝对路径
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1); // 分配内存存储根目录路径
    strcpy(m_root, server_path);                                     // 拷贝当前路径
    strcat(m_root, root);                                            // 拼接"/root"子目录

    // 创建定时器数据结构数组，用于管理客户端的连接超时
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    // 关闭epoll文件描述符
    close(m_epollfd);

    // 关闭监听的socket文件描述符
    close(m_listenfd);

    // 关闭用于进程通信的管道文件描述符
    close(m_pipefd[1]);
    close(m_pipefd[0]);

    // 释放http_conn对象数组
    delete[] users;

    // 释放定时器数组
    delete[] users_timer;

    // 释放线程池对象
    delete m_pool;
}

void WebServer::init(int port, string user,
                     string passWord,
                     string databaseName,
                     int log_write,
                     int opt_linger,
                     int trigmode,
                     int sql_num,
                     int thread_num,
                     int close_log,
                     int actor_model)
{
    // 初始化服务器配置信息
    m_port = port;                 // 服务器监听端口号
    m_user = user;                 // 数据库用户名
    m_passWord = passWord;         // 数据库密码
    m_databaseName = databaseName; // 数据库名
    m_sql_num = sql_num;           // 数据库连接池中连接数量
    m_thread_num = thread_num;     // 线程池中的线程数量
    m_log_write = log_write;       // 日志写入方式
    m_OPT_LINGER = opt_linger;     // 是否开启优雅关闭（SO_LINGER选项）
    m_TRIGMode = trigmode;         // 触发模式（LT/ET触发模式）
    m_close_log = close_log;       // 是否关闭日志
    m_actormodel = actor_model;    // 模型选择（Proactor/半同步半反应堆模型）
}

void WebServer::eventListen()
{
    // 网络编程基础步骤：创建监听套接字
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    // 断言判断socket是否创建成功，创建失败就退出
    assert(m_listenfd >= 0);

    // 设置优雅关闭连接（SO_LINGER选项）
    if (m_OPT_LINGER == 0)
    {
        // 关闭连接对应行为的参数
        struct linger tmp = {0, 1};
        // setsockopt 设置套接字选项的系统调用函数
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (m_OPT_LINGER == 1)
    {
        struct linger tmp = {1, 1};
        // setsockopt 设置套接字选项的系统调用函数
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    // 初始化服务器地址结构体
    struct sockaddr_in address;
    // 初始化置0
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;                // 使用 IPv4 协议
    address.sin_addr.s_addr = htonl(INADDR_ANY); // 绑定到所有网络接口
    address.sin_port = htons(m_port);            // 设置服务器监听端口

    // 设置地址复用，避免 "Address already in use" 错误
    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    // 绑定地址到监听套接字
    int ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(m_listenfd));
    assert(ret >= 0);
    // 开始监听
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    // 初始化定时器工具类（设置时间间隔）
    utils.init(TIMESLOT);

    // 创建 epoll 内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    // 将监听文件描述符加入 epoll 中 设置对应监听触发模式
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);

    // 静态成员变量设置为当前 epoll 文件描述符
    http_conn::m_epollfd = m_epollfd;

    // 创建管道，作为主线程与信号处理间的通信手段
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);

    // 设置管道的写端为非阻塞
    utils.setnonblocking(m_pipefd[1]);

    // 将管道的读端加入 epoll
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    // 注册信号处理函数，忽略 SIGPIPE 信号
    utils.addsig(SIGPIPE, SIG_IGN);

    // 注册 SIGALRM 和 SIGTERM 信号，并绑定工具类的信号处理函数
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    // 启动定时器
    alarm(TIMESLOT);

    // 设置工具类的全局静态变量（管道和 epoll 文件描述符）
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

void WebServer::eventLoop()
{
    // 设置超时信号和停止信号
    bool timeout = false;
    bool stop_server = false;

    // 循环 epollwait阻塞等待事件发生
    while (!stop_server)
    {
        // 记录发生事件的数量
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        // epoll 出现错误
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        // 遍历events 处理发生的事件
        for (int i = 0; i < number; ++i)
        {
            // 取出事件的fd
            int sockfd = events[i].data.fd;

            // if判断发生的是什么类事件
            if (sockfd == m_listenfd) // 有新客户端连接
            {
                bool flag = dealclientdata();
                if (!flag)
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) // 客户端断开或错误
            {
                // 关闭对应客户端的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) // 收到发送至主线程的信号 并处理
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (!flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            else if (events[i].events & EPOLLIN) // 客户端读事件
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT) // 客户端写事件
            {
                dealwithwrite(sockfd);
            }
        }
        if (timeout) // 如果收到超时信号，处理定时器
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}