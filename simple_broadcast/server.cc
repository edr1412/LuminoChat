#include "codec.h"
#include "dispatcher.h"
#include "chat.pb.h"

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpServer.h>
#include <muduo/base/ThreadLocalSingleton.h>

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <memory>
#include <unistd.h>

using namespace muduo;
using namespace muduo::net;

using LoginRequestPtr = std::shared_ptr<chat::LoginRequest>;
using RegisterRequestPtr = std::shared_ptr<chat::RegisterRequest>;
using SearchRequestPtr = std::shared_ptr<chat::SearchRequest>;
using TextMessagePtr = std::shared_ptr<chat::TextMessage>;
using ConnectionList = std::unordered_set<TcpConnectionPtr>;
using LocalConnections = ThreadLocalSingleton<ConnectionList>;
using UserMap = std::unordered_map<std::string, std::string>;
using UserMapPtr = std::shared_ptr<UserMap>;



class ChatServer
{
public:
    ChatServer(EventLoop *loop, const InetAddress &listenAddr)
        : server_(loop, listenAddr, "ChatServer"),
          dispatcher_(std::bind(&ChatServer::onUnknownMessageType, this, _1, _2, _3)),
          codec_(std::bind(&ProtobufDispatcher::onProtobufMessage, &dispatcher_, _1, _2, _3)),
          users_ptr_(new UserMap)
    {
        dispatcher_.registerMessageCallback<chat::LoginRequest>(
            std::bind(&ChatServer::onLoginRequest, this, _1, _2, _3));
        dispatcher_.registerMessageCallback<chat::RegisterRequest>(
            std::bind(&ChatServer::onRegisterRequest, this, _1, _2, _3));
        dispatcher_.registerMessageCallback<chat::TextMessage>(
            std::bind(&ChatServer::onTextMessage, this, _1, _2, _3));
        dispatcher_.registerMessageCallback<chat::SearchRequest>(
            std::bind(&ChatServer::onSearchRequest, this, _1, _2, _3));
        server_.setConnectionCallback(
            std::bind(&ChatServer::onConnection, this, _1));
        server_.setMessageCallback(
            std::bind(&ProtobufCodec::onMessage, &codec_, _1, _2, _3));
    }

    void start()
    {
        server_.setThreadInitCallback(std::bind(&ChatServer::threadInit, this, _1));
        server_.start();
    }

    void setThreadNum(int numThreads)
    {
        server_.setThreadNum(numThreads);
    }


private:
    void threadInit(EventLoop* loop)
    {
        // LocalConnections 存储当前线程的所有连接
        assert(LocalConnections::pointer() == NULL);
        LocalConnections::instance();
        assert(LocalConnections::pointer() != NULL);
        // 只有loops需要加锁保护
        MutexLockGuard lock(loops_mutex_);
        loops_.insert(loop);
    }

    void onConnection(const TcpConnectionPtr &conn)
    {
        LOG_INFO << "ChatServer - " << conn->peerAddress().toIpPort() << " -> "
                 << conn->localAddress().toIpPort() << " is "
                 << (conn->connected() ? "UP" : "DOWN");

        // 只需将连接加入到当前线程的 LocalConnections 中，无需加锁
        if (conn->connected())
        {
        LocalConnections::instance().insert(conn);
        }
        else
        {
        LocalConnections::instance().erase(conn);
        }
    }

    void onTextMessage(const TcpConnectionPtr &conn,
                       const TextMessagePtr &message,
                       Timestamp)
    {
        LOG_INFO << "onTextMessage: " << message->GetTypeName();

        EventLoop::Functor f = std::bind(&ChatServer::distributeTextMessage, this, message);
        LOG_DEBUG;

        // 只有loops需要加锁保护
        MutexLockGuard lock(loops_mutex_);

        for (const auto &loop : loops_)
        {
            loop->queueInLoop(f);
        }
        LOG_DEBUG;

    }

    void distributeTextMessage(const TextMessagePtr &message)
    {
        // 在自己的 loop thread 中执行，无需加锁
        LOG_DEBUG << "begin";
        for (const auto &connection :LocalConnections::instance())
        {
            codec_.send(connection, *message);
        }
        LOG_DEBUG << "end";
    }

    void onLoginRequest(const TcpConnectionPtr &conn,
                        const LoginRequestPtr &message,
                        Timestamp)
    {
        LOG_INFO << "onLoginRequest: " << message->GetTypeName();
        chat::LoginResponse response;
        bool loginSuccess = false;
        std::string storedPassword;

        // 对于read端，在读之前把引用计数加1，读完之后减1，这样保证在读的期间其引用计数大于1
        {
            UserMapPtr users_ptr = getUsersPtr();
            // users_ptr 一旦拿到，就不再需要锁了
            // 取数据的时候只有 getUsersPtr() 内部有锁，多线程并发读的性能很好

            auto it = users_ptr->find(message->username());
            if (it != users_ptr->end())
            {
                storedPassword = it->second;
            }
        }


        if (storedPassword == message->password())
        {
            loginSuccess = true;
        }

        if (loginSuccess)
        {
            response.set_success(true);
            response.set_error_message("");
        }
        else
        {
            response.set_success(false);
            response.set_error_message("Invalid username or password.");
        }

        codec_.send(conn, response);
    }

    void onRegisterRequest(const TcpConnectionPtr &conn,
                           const RegisterRequestPtr &message,
                           Timestamp)
    {
        LOG_INFO << "onRegisterRequest: " << message->GetTypeName();
        
        chat::RegisterResponse response;
        std::pair<std::unordered_map<std::string, std::string>::iterator, bool> result;

        // write段修改对象，若引用计数为1，则可以直接修改，无需拷贝；若引用计数大于1，则启动copy-on-other-reading
        // 必须全程持锁
        {
            MutexLockGuard lock(users_mutex_);
            // 如果引用计数大于1，说明这时候其他线程正在读，那么不能在原来的数据上修改，得把users_ptr_替换为新副本，然后再修改。这可能让那些线程读到旧数据，但比起网络延迟可以忽略不计。
            if (!users_ptr_.unique())
            {
                users_ptr_.reset(new UserMap(*users_ptr_));
            }
            // 如果引用计数为1，说明没有用户在读，那么就能安全地修改共享对象，节约一次Map拷贝。实测99%的情况下，可以节约一次拷贝。
            assert(users_ptr_.unique());
            result = users_ptr_->emplace(message->username(), message->password());
        }


        if (result.second)
        {
            response.set_success(true);
            response.set_error_message("");
        }
        else
        {
            response.set_success(false);
            response.set_error_message("Username already exists.");
        }

        codec_.send(conn, response);
    }

    void onSearchRequest(const TcpConnectionPtr &conn,
                     const SearchRequestPtr &message,
                     Timestamp)
    {
        LOG_INFO << "onSearchRequest: " << message->GetTypeName();
        chat::SearchResponse response;

        // 对于read端，在读之前把引用计数加1，读完之后减1，这样保证在读的期间其引用计数大于1
        {
            UserMapPtr users_ptr = getUsersPtr();
            // users_ptr 一旦拿到，就不再需要锁了
            // 取数据的时候只有 getUsersPtr() 内部有锁，多线程并发读的性能很好
            for (const auto &user : *users_ptr)
            {
                if (message->keyword().empty() || user.first.find(message->keyword()) != std::string::npos)
                {
                    response.add_usernames(user.first);
                }
            }
        }

        codec_.send(conn, response);
    }

    void onUnknownMessageType(const TcpConnectionPtr &conn,
                              const MessagePtr &message,
                              Timestamp)
    {
        LOG_INFO << "onUnknownMessageType: " << message->GetTypeName();
        conn->shutdown();
    }

    UserMapPtr getUsersPtr() 
    {
        MutexLockGuard lock(users_mutex_);
        return users_ptr_;
    }

    TcpServer server_;
    ProtobufDispatcher dispatcher_;
    ProtobufCodec codec_;
    MutexLock loops_mutex_;
    MutexLock users_mutex_;
    //std::unordered_set<TcpConnectionPtr> connections_  GUARDED_BY(connections_mutex_);
    std::unordered_set<EventLoop*> loops_ GUARDED_BY(loops_mutex_);
    UserMapPtr users_ptr_  GUARDED_BY(users_mutex_); // Key: username, Value: password
};

int main(int argc, char *argv[])
{
    LOG_INFO << "pid = " << getpid();
    if (argc > 1)
    {
        uint16_t port = static_cast<uint16_t>(atoi(argv[1]));
        InetAddress listenAddr(port);
        EventLoop loop;
        ChatServer server(&loop, listenAddr);
        if (argc > 2)
        {
        server.setThreadNum(atoi(argv[2]));
        }
        server.start();
        loop.loop();
    }
    else
    {
        printf("Usage: %s listen_port  [thread_num]\n", argv[0]);
    }
}
