#include "pubsub.h"
#include <iostream>
RedisPubSub::RedisPubSub(const std::string &host, int port, const MessageCallback &callback)
    : host_(host),
      port_(port),
      callback_(callback),
      stop_(false)
{
    redis_context_ = createRedisContext();

    if (redis_context_ == nullptr || redis_context_->err)
    {
        std::cerr << "Redis Pubsub Connection error: " << redis_context_->errstr << std::endl;
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
    redisContext *ctx = redisConnectWithTimeout(host_.c_str(), port_, timeout);
    // redisSetTimeout(ctx, timeout);
    return ctx;
}

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
        publish("__UNLOCK_CHANNEL__", "unlock");
        printf("-");
    }

    redisAppendCommand(redis_context_, "SUBSCRIBE %s", channel.c_str());

    int done = 0;
    while (!done)
    {
        if (redisBufferWrite(redis_context_, &done) != REDIS_OK)
        {
            printf("Error subscribing to channel %s\n", channel.c_str());
            std::abort();
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
        printf("-");
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

bool RedisPubSub::publish(const std::string &channel, const std::string &message)
{
    thread_local redisContext *local_context = createRedisContext();
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
