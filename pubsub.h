#ifndef PUBSUB_H
#define PUBSUB_H

#include <hiredis/hiredis.h>
#include <string>
#include <mutex>
#include <thread>
#include <functional>
#include <atomic>
#include <cstdlib>
#include <map>

class RedisPubSub {
public:
    using MessageCallback = std::function<void(const std::string &, const std::string &)>;

    RedisPubSub(const std::string &host, int port, const MessageCallback &callback);
    ~RedisPubSub();

    bool subscribe(const std::string &channel);
    bool unsubscribe(const std::string &channel);
    bool publish(const std::string &channel, const std::string &message);
    void stop();

private:
    void messageListener();
    redisContext* createRedisContext();
    redisContext* getLocalContext();

    std::string host_;
    int port_;
    redisContext *redis_context_;
    std::timed_mutex mtx_;
    std::mutex connect_mtx_; 
    std::thread listener_thread_;
    MessageCallback callback_;
    std::atomic<bool> stop_;
    static thread_local std::map<const RedisPubSub*, redisContext*> context_map_;
};

#endif // PUBSUB_H
