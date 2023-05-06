#include "pubsub.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

std::atomic<int> messages_received{0};
std::atomic<int> messages_published{0};

void messageHandler(const std::string& channel, const std::string& message) {
    messages_received.fetch_add(1, std::memory_order_relaxed);
}

int main() {
    const int num_threads = 4;
    const int num_messages = 100000;
    const std::string channel_prefix = "channel_";

    RedisPubSub redis_pub_sub("localhost", 6379, messageHandler);

    // Subscribe to channels
    for (int i = 0; i < num_threads; ++i) {
        redis_pub_sub.subscribe(channel_prefix + std::to_string(i));
    }

    // Create publisher threads
    std::vector<std::thread> publisher_threads;
    for (int i = 0; i < num_threads; ++i) {
        publisher_threads.emplace_back([&, i]() {
            for (int j = 0; j < num_messages; ++j) {
                redis_pub_sub.publish(channel_prefix + std::to_string(i),
                                      std::to_string(j));
                messages_published.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Join publisher threads
    for (auto& t : publisher_threads) {
        t.join();
    }

    // Give some time for messages to be received
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Unsubscribe from channels
    for (int i = 0; i < num_threads; ++i) {
        redis_pub_sub.unsubscribe(channel_prefix + std::to_string(i));
    }

    redis_pub_sub.stop();

    std::cout << "Published messages: " << messages_published.load() << std::endl;
    std::cout << "Received messages: " << messages_received.load() << std::endl;

    if (messages_published.load() == messages_received.load()) {
        std::cout << "No messages lost. Test passed." << std::endl;
    } else {
        std::cout << "Some messages were lost. Test failed." << std::endl;
    }

    return 0;
}

/************************************
#include "pubsub.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

std::atomic<int> messages_received{0};
std::atomic<int> messages_published{0};

void messageHandler(const std::string& channel, const std::string& message) {
    messages_received.fetch_add(1, std::memory_order_relaxed);
}

int main() {
    const int num_channels = 9998;
    const int num_messages = 10;
    const std::string channel_prefix = "channel_";

    RedisPubSub redis_pub_sub("localhost", 6379, messageHandler);

    // Subscribe to channels
    for (int i = 0; i < num_channels; ++i) {
        redis_pub_sub.subscribe(channel_prefix + std::to_string(i));
    }

    // Create publisher threads
    std::vector<std::thread> publisher_threads;
    for (int i = 0; i < num_channels; ++i) {
        publisher_threads.emplace_back([&, i]() {
            for (int j = 0; j < num_messages; ++j) {
                redis_pub_sub.publish(channel_prefix + std::to_string(i),
                                      std::to_string(j));
                messages_published.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Join publisher threads
    for (auto& t : publisher_threads) {
        t.join();
    }

    // Give some time for messages to be received
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // Unsubscribe from channels
    for (int i = 0; i < num_channels; ++i) {
        redis_pub_sub.unsubscribe(channel_prefix + std::to_string(i));
    }

    redis_pub_sub.stop();

    std::cout << "Published messages: " << messages_published.load() << std::endl;
    std::cout << "Received messages: " << messages_received.load() << std::endl;

    if (messages_published.load() == messages_received.load()) {
        std::cout << "No messages lost. Test passed." << std::endl;
    } else {
        std::cout << "Some messages were lost. Test failed." << std::endl;
    }

    return 0;
}
************************************/

/************************************
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
************************************/