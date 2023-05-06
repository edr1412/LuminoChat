#include "pubsub.h"
#include <iostream>
void messageHandler(const std::string &channel, const std::string &message)
{
    printf("[%s] %s\n", channel.c_str(), message.c_str());
}

int main()
{
    RedisPubSub redis_pub_sub("localhost", 6379, messageHandler);

    redis_pub_sub.subscribe("channel_1");
    redis_pub_sub.subscribe("channel_2");
    redis_pub_sub.publish("channel_1", "111");
    redis_pub_sub.publish("channel_2", "222");

    std::thread publisher([&redis_pub_sub]()
                            {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        redis_pub_sub.publish("channel_1", "Hello, World!");
        redis_pub_sub.publish("channel_2", "Hello, World!"); });
    redis_pub_sub.publish("channel_1", "222");
    publisher.join();
    redis_pub_sub.publish("channel_1", "333");
    redis_pub_sub.publish("channel_2", "333");

    redis_pub_sub.unsubscribe("channel_2");
    redis_pub_sub.unsubscribe("channel_1");

    redis_pub_sub.stop();

    return 0;
}
