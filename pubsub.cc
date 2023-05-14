#include "pubsub.h"
//#include <iostream>
RedisPubSub::RedisPubSub(const std::string &host, int port, const MessageCallback &callback)
    : host_(host),
      port_(port),
      callback_(callback),
      stop_(false)
{
    redis_context_ = createRedisContext();

    if (redis_context_ == nullptr || redis_context_->err)
    {
        //std::cerr << "Redis Pubsub Connection error: " << redis_context_->errstr << std::endl;
        std::abort();
    }

    // Subscribe to the special unlock channel
    redisAppendCommand(redis_context_, "SUBSCRIBE %s", "__UNLOCK_CHANNEL__");
    int done = 0;
    while (!done)
    {
        redisBufferWrite(redis_context_, &done);
    }

    listener_thread_ = std::thread(&RedisPubSub::messageListener, this);
}

redisContext *RedisPubSub::createRedisContext()
{
    struct timeval timeout = {1, 500000}; // 1.5 seconds
    std::lock_guard<std::mutex> lock(connect_mtx_); // Is redisConnectWithTimeout thread-safe? Not sure.
    redisContext *ctx = redisConnectWithTimeout(host_.c_str(), port_, timeout);
    // redisSetTimeout(ctx, timeout);
    return ctx;
}

// 在类的定义外部定义 context_map_
thread_local std::map<const RedisPubSub*, redisContext*> RedisPubSub::context_map_;

RedisPubSub::~RedisPubSub()
{
    listener_thread_.join();
    redisFree(redis_context_);
}

void RedisPubSub::stop()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop_ = true;
    unsubscribe("__UNLOCK_CHANNEL__");
}

bool RedisPubSub::subscribe(const std::string &channel)
{
    std::unique_lock<std::timed_mutex> lock(mtx_, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(200))) 
    {
        // 通过发布一个消息来夺取锁，否则redisGetReply会一直阻塞下去
        // 参考 https://github.com/redis/hiredis/discussions/1078
        // unsubscribe 同理
        publish("__UNLOCK_CHANNEL__", "unlock");
        //printf("-");
    }

    redisAppendCommand(redis_context_, "SUBSCRIBE %s", channel.c_str());

    int done = 0;
    while (!done)
    {
        if (redisBufferWrite(redis_context_, &done) != REDIS_OK)
        {
            //printf("Error subscribing to channel %s\n", channel.c_str());
            return false;
        }
    }
    return true;
}

bool RedisPubSub::unsubscribe(const std::string &channel)
{
    std::unique_lock<std::timed_mutex> lock(mtx_, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(200))) 
    {
        publish("__UNLOCK_CHANNEL__", "unlock");
        //printf("-");
    }

    redisAppendCommand(redis_context_, "UNSUBSCRIBE %s", channel.c_str());

    int done = 0;
    while (!done)
    {
        if (redisBufferWrite(redis_context_, &done) != REDIS_OK)
        {
            return false;
        }
    }
    return true;
}

redisContext* RedisPubSub::getLocalContext() 
{
    auto it = context_map_.find(this);
    if (it == context_map_.end()) {
        // 如果这个实例的 redisContext 还没有创建，那么创建一个
        redisContext* context = createRedisContext();
        context_map_[this] = context;
        return context;
    } else {
        // 否则，返回已经创建的 redisContext
        return it->second;
    }
}

bool RedisPubSub::publish(const std::string &channel, const std::string &message)
{
    redisContext *local_context = getLocalContext();

    redisAppendCommand(local_context, "PUBLISH %s %s", channel.c_str(), message.c_str());

    int done = 0;
    while (!done)
    {
        if (redisBufferWrite(local_context, &done) != REDIS_OK)
        {
            return false;
        }
    }

    return true;

    // 下面的写法可保证命令执行是否成功，但会阻塞等待redis的回复，很慢
    // thread_local redisContext *local_context = createRedisContext();
    // redisReply *reply = static_cast<redisReply*>(
    //     redisCommand(local_context, "PUBLISH %s %s", channel.c_str(), message.c_str())
    // );

    // if(reply == nullptr) {
    //     // 在此情况下，redisCommand 因为某种原因（例如网络问题）失败，并返回 nullptr
    //     return false;
    // }
    
    // bool success = (reply->type != REDIS_REPLY_ERROR);

    // freeReplyObject(reply);  // 记得释放 redisReply 对象
    
    // return success;
}

void RedisPubSub::messageListener()
{
    while (!stop_)
    {
        redisReply *reply = nullptr;

        {
            std::unique_lock<std::timed_mutex> lock(mtx_);
            if (redisGetReply(redis_context_, (void **)&reply) != REDIS_OK)
            {
                //std::cout << "[-] Error receiving message: " << redis_context_->errstr << std::endl;
                continue;
            }
        }

        if (reply == nullptr)
        {
            continue;
        }

        if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3)
        {
            std::string messageType(reply->element[0]->str);

            if (messageType == "message")
            {
                std::string channel(reply->element[1]->str);
                std::string message(reply->element[2]->str);

                if (channel == "__UNLOCK_CHANNEL__")
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                else
                {
                    callback_(channel, message);
                }
            }
        }

        freeReplyObject(reply);
    }
}
