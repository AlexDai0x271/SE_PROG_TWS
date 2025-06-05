#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

enum LogType {
    SYNC_LOG,   // 同步日志
    ASYNC_LOG   // 异步日志
};

class async_Log
{
public:
	virtual ~async_Log() = default;
	
protected:
	async_Log() = default;
	
private:
	virtual bool async_init(const char *file_name, int close_log, int log_buf_size, 
                   int split_lines, int max_queue_size) = 0;
    virtual void async_write_log(int level, const char *format, va_list valst) = 0;
    virtual void async_flush() = 0;
};

class sync_Log
{
public:
	virtual ~sync_Log() = default;
	
protected:
	sync_Log() = default;
	
private:
	virtual bool sync_init(const char *file_name, int close_log, int log_buf_size, 
                   int split_lines, int max_queue_size) = 0;
    virtual void sync_write_log(int level, const char *format, va_list valst) = 0;
    virtual void sync_flush() = 0;
};

class Log :public async_Log, public sync_Log
{
public:
    // 配置日志类型（必须在第一次使用前调用）
    static void set_log_type(LogType type) { m_log_type = type; }
    
    // 保持原有的get_instance接口
    static Log *get_instance()
    {
        static Log instance;
        return &instance;
    }

    // 原有接口保持不变
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, 
              int split_lines = 5000000, int max_queue_size = 0);
    void write_log(int level, const char *format, ...);
    void flush(void);

    // 用于宏定义访问成员变量
    int get_close_log() const { return m_close_log; }
    static void *async_flush_thread(void *args);

private:
    Log();  // 私有构造函数
    virtual ~Log();
    
    // 实际实现
    bool async_init(const char *file_name, int close_log, int log_buf_size, 
                   int split_lines, int max_queue_size) override;
    bool sync_init(const char *file_name, int close_log, int log_buf_size, 
                  int split_lines, int max_queue_size) override;
    void async_write_log(int level, const char *format, va_list valst) override;
    void sync_write_log(int level, const char *format, va_list valst) override;
    void async_flush() override;
    void sync_flush() override;
    
    void *async_log()
    {
        string single_log;
        //从阻塞队列中取出一个日志string，写入文件
        while (m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }

    static LogType m_log_type;  // 日志类型
    
    // 共用成员变量
    char dir_name[128];        // 路径名
    char log_name[128];        // log文件名
    int m_split_lines;         // 日志最大行数
    int m_log_buf_size;        // 日志缓冲区大小
    long long m_count;         // 日志行数记录
    int m_today;               // 按天分类记录当前日期
    FILE *m_fp;                // 打开log的文件指针
    char *m_buf;
    locker m_mutex;
    int m_close_log;           // 关闭日志标志
    
    // 异步日志特有成员
    block_queue<string> *m_log_queue; // 阻塞队列
    bool m_is_async;                 // 是否异步标志位

    // 禁止拷贝
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;
};

// 保持原有宏定义，但改为使用get_close_log()
#define LOG_DEBUG(format, ...) if(0 == Log::get_instance()->get_close_log()) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...) if(0 == Log::get_instance()->get_close_log()) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...) if(0 == Log::get_instance()->get_close_log()) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == Log::get_instance()->get_close_log()) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}

#endif