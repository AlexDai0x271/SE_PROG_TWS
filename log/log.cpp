#include "log.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

// 初始化静态成员
LogType Log::m_log_type = SYNC_LOG;  // 默认同步日志

Log::Log() : m_log_queue(nullptr), m_fp(nullptr), m_buf(nullptr), 
             m_is_async(false), m_close_log(0), m_count(0) {}

Log::~Log()
{
    if (m_fp != nullptr) {
        fclose(m_fp);
    }
    if (m_buf != nullptr) {
        delete[] m_buf;
    }
    if (m_log_queue != nullptr) {
        delete m_log_queue;
    }
}

bool Log::init(const char *file_name, int close_log, int log_buf_size, 
              int split_lines, int max_queue_size)
{
    // 根据配置选择初始化方式
    if (m_log_type == ASYNC_LOG) {
        return async_init(file_name, close_log, log_buf_size, split_lines, max_queue_size);
    } else {
        return sync_init(file_name, close_log, log_buf_size, split_lines, max_queue_size);
    }
}

bool Log::async_init(const char *file_name, int close_log, int log_buf_size, 
                    int split_lines, int max_queue_size)
{
    if (max_queue_size >= 1) {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        pthread_create(&tid, NULL, async_flush_thread, NULL);
    }
    
    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    if (p == NULL) {
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, 
                my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    } else {
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, 
                my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;
    m_fp = fopen(log_full_name, "a");
    return m_fp != nullptr;
}

bool Log::sync_init(const char *file_name, int close_log, int log_buf_size, 
                   int split_lines, int max_queue_size)
{
    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    if (p == NULL) {
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, 
                my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    } else {
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, 
                my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;
    m_fp = fopen(log_full_name, "a");
    return m_fp != nullptr;
}

void Log::write_log(int level, const char *format, ...)
{
    va_list valst;
    va_start(valst, format);
    
    if (m_log_type == ASYNC_LOG) {
        async_write_log(level, format, valst);
    } else {
        sync_write_log(level, format, valst);
    }
    
    va_end(valst);
}

void Log::async_write_log(int level, const char *format, va_list valst)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    
    switch (level) {
    case 0: strcpy(s, "[debug]:"); break;
    case 1: strcpy(s, "[info]:"); break;
    case 2: strcpy(s, "[warn]:"); break;
    case 3: strcpy(s, "[erro]:"); break;
    default: strcpy(s, "[info]:"); break;
    }

    m_mutex.lock();
    m_count++;

    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) {
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};
       
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
       
        if (m_today != my_tm.tm_mday) {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        } else {
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }
    m_mutex.unlock();

    string log_str;
    m_mutex.lock();

    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if (m_is_async && !m_log_queue->full()) {
        m_log_queue->push(log_str);
    } else {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }
}

void Log::sync_write_log(int level, const char *format, va_list valst)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    
    switch (level) {
    case 0: strcpy(s, "[debug]:"); break;
    case 1: strcpy(s, "[info]:"); break;
    case 2: strcpy(s, "[warn]:"); break;
    case 3: strcpy(s, "[erro]:"); break;
    default: strcpy(s, "[info]:"); break;
    }

    m_mutex.lock();
    m_count++;

    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) {
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};
       
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
       
        if (m_today != my_tm.tm_mday) {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        } else {
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }
    m_mutex.unlock();

    string log_str;
    m_mutex.lock();

    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    m_mutex.lock();
    fputs(log_str.c_str(), m_fp);
    m_mutex.unlock();
}

void Log::flush()
{
    if (m_log_type == ASYNC_LOG) {
        async_flush();
    } else {
        sync_flush();
    }
}

void Log::async_flush()
{
    m_mutex.lock();
    fflush(m_fp);
    m_mutex.unlock();
}

void Log::sync_flush()
{
    m_mutex.lock();
    fflush(m_fp);
    m_mutex.unlock();
}

void *Log::async_flush_thread(void *args)
{
    Log::get_instance()->async_log();
    return nullptr;
}