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
using LogoutRequestPtr = std::shared_ptr<chat::LogoutRequest>;
using RegisterRequestPtr = std::shared_ptr<chat::RegisterRequest>;
using SearchRequestPtr = std::shared_ptr<chat::SearchRequest>;
using TextMessagePtr = std::shared_ptr<chat::TextMessage>;
using GroupRequestPtr = std::shared_ptr<chat::GroupRequest>;
using ConnectionMap = std::unordered_map<std::string, std::unordered_set<TcpConnectionPtr>>;
using LocalConnections = ThreadLocalSingleton<ConnectionMap>;
using UserMap = std::unordered_map<std::string, std::string>;
using UserMapPtr = std::shared_ptr<UserMap>;
using OnlineUserMap = std::unordered_map<std::string, std::unordered_set<EventLoop*>>;
using GroupMap = std::unordered_map<std::string, std::unordered_set<std::string>>;


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
        dispatcher_.registerMessageCallback<chat::LogoutRequest>(
            std::bind(&ChatServer::onLogoutRequest, this, _1, _2, _3));
        dispatcher_.registerMessageCallback<chat::RegisterRequest>(
            std::bind(&ChatServer::onRegisterRequest, this, _1, _2, _3));
        dispatcher_.registerMessageCallback<chat::TextMessage>(
            std::bind(&ChatServer::onTextMessage, this, _1, _2, _3));
        dispatcher_.registerMessageCallback<chat::SearchRequest>(
            std::bind(&ChatServer::onSearchRequest, this, _1, _2, _3));
        dispatcher_.registerMessageCallback<chat::GroupRequest>(
            std::bind(&ChatServer::onGroupRequest, this, _1, _2, _3));
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
    }

    void onConnection(const TcpConnectionPtr &conn) {
        LOG_INFO << "ChatServer - " << conn->peerAddress().toIpPort() << " -> "
                    << conn->localAddress().toIpPort() << " is "
                    << (conn->connected() ? "UP" : "DOWN");
        if (conn->connected()) {
        } 
        else 
        {
            // 没有登录也可以安全调用 logout，因为会找不到conn就什么也不会做
            logout(conn);
        }
    }
    void addUserToOnlineUsers(const TcpConnectionPtr &conn, const std::string &username) {
        MutexLockGuard lock(online_users_mutex_);
        online_users_[username].insert(conn->getLoop());
    }
    void removeUserFromOnlineUsers(const TcpConnectionPtr &conn, const std::string &username) {
        MutexLockGuard lock(online_users_mutex_);
        auto loop_set = online_users_[username];
        loop_set.erase(conn->getLoop());
        if (loop_set.empty()) {
            online_users_.erase(username);
        }
    }

    void onTextMessage(const TcpConnectionPtr &conn,
                    const TextMessagePtr &message,
                    Timestamp)
    {
        LOG_INFO << "onTextMessage: " << message->GetTypeName();

        bool is_sent = false;
        std::string error_msg;

        if (message->target_type() == chat::TargetType::USER) {
            // 私聊
            MutexLockGuard lock(online_users_mutex_);
            auto it = online_users_.find(message->target());
            if (it != online_users_.end()) {
                for (auto loop : it->second) {
                    loop->queueInLoop(std::bind(&ChatServer::sendTextMessage, this, message->target(), message));
                }
                is_sent = true;
            } else {
                error_msg = "Target user not found or is offline.";
            }
        } 
        else if (message->target_type() == chat::TargetType::GROUP) 
        {
            // 群聊
            std::unordered_set<std::string> users;
            {
                MutexLockGuard lock(groups_mutex_);
                auto group_it = groups_.find(message->target());
                if (group_it != groups_.end()) {
                    users = group_it->second;
                }
            }
            if (!users.empty()) {
                for (const auto &user : users) {
                    MutexLockGuard lock(online_users_mutex_);
                    auto it = online_users_.find(user);
                    if (it != online_users_.end()) {
                        for (auto loop : it->second) {
                            loop->queueInLoop(std::bind(&ChatServer::sendTextMessage, this, user, message));
                        }
                    }
                }
                is_sent = true;
            } else {
                error_msg = "Target group not found";
            }
        } else {
            error_msg = "Invalid target type";
        }

        // 创建并发送 TextMessageResponse
        chat::TextMessageResponse response;
        response.set_success(is_sent);
        response.set_error_message(error_msg);
        codec_.send(conn, response);
    }


    void sendTextMessage(const std::string &target, const TextMessagePtr &message) {
        // 在自己的 loop thread 中执行，无需加锁
        LOG_DEBUG << "begin";
        auto it = LocalConnections::instance().find(target);
        if (it != LocalConnections::instance().end()) {
            for (const auto &connection : it->second) {
                codec_.send(connection, *message);
            }
        }
        LOG_DEBUG << "end";
    }

    // void distributeTextMessage(const TextMessagePtr &message)
    // {
    //     // 在自己的 loop thread 中执行，无需加锁
    //     LOG_DEBUG << "begin";
    //     for (const auto &connection :LocalConnections::instance())
    //     {
    //         codec_.send(connection, *message);
    //     }
    //     LOG_DEBUG << "end";
    // }

    void onLoginRequest(const TcpConnectionPtr &conn,
                        const LoginRequestPtr &message,
                        Timestamp)
    {
        LOG_INFO << "onLoginRequest: " << message->GetTypeName();
        chat::LoginResponse response;
        bool userExists = true;
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
            else
            {
                userExists = false;
            }
        }


        if (userExists && storedPassword == message->password())
        {
            response.set_username(message->username());
            response.set_success(true);
            response.set_error_message("");
            LocalConnections::instance()[message->username()].insert(conn);
            addUserToOnlineUsers(conn, message->username());
        }
        else
        {
            response.set_username("guest");
            response.set_success(false);
            response.set_error_message("Invalid username or password.");
        }

        codec_.send(conn, response);
    }

    void logout(const TcpConnectionPtr &conn)
    {
        // 无需加锁
        // 从LocalConnections::instance() 找到值的set中含有conn的键值对，然后删除set中的conn
        for (auto &item : LocalConnections::instance()) {
            if (item.second.find(conn) != item.second.end()) {
                std::string username = item.first;
                item.second.erase(conn);
                //如果当前用户没有连接了，就删除整个键值对，并从online_users_中删除
                if (LocalConnections::instance()[username].empty()) {
                    LocalConnections::instance().erase(username);
                    removeUserFromOnlineUsers(conn, username);
                }
                break;
            }
        }
    }

    void onLogoutRequest(const TcpConnectionPtr &conn,
                         const LogoutRequestPtr &message,
                         Timestamp)
    {
        LOG_INFO << "onLogoutRequest: " << message->GetTypeName();
        logout(conn);
    }


    void onRegisterRequest(const TcpConnectionPtr &conn,
                           const RegisterRequestPtr &message,
                           Timestamp)
    {
        LOG_INFO << "onRegisterRequest: " << message->GetTypeName();
        
        chat::RegisterResponse response;
        std::pair<std::unordered_map<std::string, std::string>::iterator, bool> result;

        //username不能是guest
        if (message->username() == "guest")
        {
            response.set_success(false);
            response.set_error_message("Invalid username.");
            codec_.send(conn, response);
            return;
        }

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

    void onGroupRequest(const TcpConnectionPtr &conn,
                        const GroupRequestPtr &message,
                        Timestamp) 
    {
        std::string group_name = message->group_name();
        std::string username = message->username();

        chat::GroupResponse response;
        response.set_operation(message->operation());
        bool operationSuccess = false;
        if (message->operation() == chat::GroupOperation::CREATE) {
            {
                MutexLockGuard lock(groups_mutex_);
                if (groups_.find(group_name) == groups_.end()) {
                    groups_[group_name] = std::unordered_set<std::string>();
                    groups_[group_name].insert(username);
                    operationSuccess = true;
                }
            }

            response.set_success(operationSuccess);
            if (!operationSuccess) {
                response.set_error_message("Group already exists.");
            }
        }
        else if (message->operation() == chat::GroupOperation::JOIN) {
            {
                MutexLockGuard lock(groups_mutex_);
                if (groups_.find(group_name) != groups_.end()) {
                    groups_[group_name].insert(username);
                    operationSuccess = true;
                }
            }

            response.set_success(operationSuccess);
            if (!operationSuccess) {
                response.set_error_message("Group does not exist.");
            }
        }
        else if (message->operation() == chat::GroupOperation::LEAVE) {
            {
                MutexLockGuard lock(groups_mutex_);
                if (groups_.find(group_name) != groups_.end()) {
                    groups_[group_name].erase(username);
                    if (groups_[group_name].size() == 0) {
                        groups_.erase(group_name);
                    }
                    operationSuccess = true;
                }
            }

            response.set_success(operationSuccess);
            if (!operationSuccess) {
                response.set_error_message("Group does not exist.");
            }
        }
        else {
            LOG_ERROR << "Unknown group operation.";
            response.set_success(false);
            response.set_error_message("Unknown group operation.");
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
    
    MutexLock users_mutex_;
    MutexLock online_users_mutex_;
    MutexLock groups_mutex_;

    UserMapPtr users_ptr_  GUARDED_BY(users_mutex_); // Key: username, Value: password
    OnlineUserMap online_users_ GUARDED_BY(online_users_mutex_);
    GroupMap groups_ GUARDED_BY(groups_mutex_);
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
