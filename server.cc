#include "codec.h"
#include "dispatcher.h"
#include "chat.pb.h"
#include "pubsub.h"

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpServer.h>
#include <muduo/base/ThreadLocalSingleton.h>
#include <muduo/base/ThreadPool.h>

#include <functional>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <string>
#include <memory>
#include <unistd.h>
#include <hiredis/hiredis.h>

using namespace muduo;
using namespace muduo::net;

using LoginRequestPtr = std::shared_ptr<chat::LoginRequest>;
using LogoutRequestPtr = std::shared_ptr<chat::LogoutRequest>;
using RegisterRequestPtr = std::shared_ptr<chat::RegisterRequest>;
using SearchRequestPtr = std::shared_ptr<chat::SearchRequest>;
using TextMessagePtr = std::shared_ptr<chat::TextMessage>;
using GroupRequestPtr = std::shared_ptr<chat::GroupRequest>;

using OnlineUserMap = std::unordered_map<std::string, std::unordered_set<EventLoop*>>; // 一个用户可能在多个EventLoop中在线
using ConnectionMap = std::unordered_map<std::string,
    std::unordered_set<TcpConnectionPtr>>;  // 即使在同一个EventLoop中，
                                            //一个用户也可能有多个连接
using LocalConnections = ThreadLocalSingleton<ConnectionMap>;

thread_local redisContext *redis_ctx_ = nullptr;

class ChatServer
{
public:
    ChatServer(EventLoop *loop, const InetAddress &listenAddr)
        : server_(loop, listenAddr, "ChatServer"),
          dispatcher_(std::bind(&ChatServer::onUnknownMessageType, this, _1, _2, _3)),
          codec_(std::bind(&ProtobufDispatcher::onProtobufMessage, &dispatcher_, _1, _2, _3)),
          redis_pubsub_("localhost", 6379, std::bind(&ChatServer::onRedisPubSubMessage, this, _1, _2))
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

    ~ChatServer()
    {
        for (auto loop : loops_)
        {
            loop->queueInLoop(std::bind(&ChatServer::cleanupRedisContext, this));
        }
        
    }

    void start()
    {
        server_.setThreadInitCallback(std::bind(&ChatServer::threadInit, this, _1));
        threadPool_.setThreadInitCallback(std::bind(&ChatServer::initRedisContext, this));
        threadPool_.start(2); // 线程池分配2个线程
        server_.start();
    }

    void setThreadNum(int numThreads)
    {
        server_.setThreadNum(numThreads);
    }


private:
    void initRedisContext()
    {
        if (!redis_ctx_)
        {
            redis_ctx_ = redisConnect("127.0.0.1", 6379);
            if (redis_ctx_->err)
            {
                LOG_ERROR << "Redis connection error: " << redis_ctx_->errstr;
                redisFree(redis_ctx_);
                redis_ctx_ = nullptr;
                return;
            }
        }
    }
    void cleanupRedisContext()
    {
        if (redis_ctx_)
        {
            redisFree(redis_ctx_);
            redis_ctx_ = nullptr;
        }
    }
    void threadInit(EventLoop* loop)
    {
        initRedisContext();
        MutexLockGuard lock(loops_mutex_);
        loops_.insert(loop);
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
        if (online_users_.find(username) == online_users_.end()) {
            // 用户首次登录在此服务器
            redis_pubsub_.subscribe("user." + username);
        }
        online_users_[username].insert(conn->getLoop());
    }
    void removeUserFromOnlineUsers(const TcpConnectionPtr &conn, const std::string &username) {
        MutexLockGuard lock(online_users_mutex_);
        auto& loop_set = online_users_[username];
        loop_set.erase(conn->getLoop());
        if (loop_set.empty()) {
            // 用户彻底不在此服务器程序上在线了
            online_users_.erase(username);
            redis_pubsub_.unsubscribe("user." + username);
        }
    }

    void onRedisPubSubMessage(const std::string& channel, const std::string& msg) 
    {
        LOG_INFO << "onRedisPubSubMessage: " << channel;
        assert(channel.size() > 5 && channel.substr(0, 5) == "user.");
        // 从redis中读取消息，转发给在线用户
        TextMessagePtr message = std::make_shared<chat::TextMessage>();
        if (message->ParseFromString(msg))
        {
            std::string user = channel.substr(5); // message->target()可能是群名，所以使用channel来决定送往哪个用户
            MutexLockGuard lock(online_users_mutex_);
            auto it = online_users_.find(user);
            if (it != online_users_.end())
            {
                for (auto loop : it->second)
                {
                    loop->queueInLoop(std::bind(&ChatServer::sendTextMessage, this, user, message));
                }
            }
            else
            {
                LOG_ERROR << "Target user " << user << " not found or is offline.";
            }
        }
    }

    void onTextMessage(const TcpConnectionPtr &conn,
                    const TextMessagePtr &message,
                    Timestamp)
    {
        LOG_INFO << "onTextMessage: " << message->GetTypeName();
        bool is_sent = false;
        std::string error_msg;

        if (message->target_type() == chat::TargetType::USER)
        {
            // 私聊
            std::string user = message->target();
            // 不验证了，直接publish
            redis_pubsub_.publish("user." + user, message->SerializeAsString());
            is_sent = true;
        }
        else if (message->target_type() == chat::TargetType::GROUP)
        {
            // 群聊
            // 将 processGroupMessage 函数作为任务提交给线程池
            threadPool_.run(std::bind(&ChatServer::processGroupMessage, this, conn, message->target(), message));
            return;
        }
        else
        {
            error_msg = "Invalid target type";
        }

        // 创建并发送 TextMessageResponse
        chat::TextMessageResponse response;
        response.set_success(is_sent);
        response.set_error_message(error_msg);
        codec_.send(conn, response);
    }

    void processGroupMessage(const TcpConnectionPtr &conn, const std::string &group_id, const TextMessagePtr &message)
    {
        bool is_sent = false;
        std::string error_msg;

        redisReply *reply = (redisReply *)redisCommand(redis_ctx_, "SMEMBERS group:%s:users", group_id.c_str()); // FIXME: SMEMBERS 复杂度是 O(N)，可以考虑用 SSCAN
        if (reply->type == REDIS_REPLY_ARRAY)
        {
            if (reply->elements > 0)
            {
                for (size_t i = 0; i < reply->elements; i++)
                {
                    std::string user = reply->element[i]->str;
                    // 不验证了，直接publish
                    redis_pubsub_.publish("user." + user, message->SerializeAsString());
                }
                is_sent = true;
            }
            else
            {
                error_msg = "Target group not found";
            }
        }
        else
        {
            error_msg = "Error executing SMEMBERS command";
        }
        freeReplyObject(reply);

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
        std::string username = message->username();
        std::string storedPassword;

        // 获取存储在 Redis 中的密码
        redisReply *reply = (redisReply *)redisCommand(redis_ctx_, "HGET users %s", username.c_str());
        if (reply->type != REDIS_REPLY_NIL)
        {
            storedPassword = reply->str;
        }

        freeReplyObject(reply);

        if (!storedPassword.empty() && storedPassword == message->password())
        {
            response.set_username(username);
            response.set_success(true);
            response.set_error_message("");
            LocalConnections::instance()[username].insert(conn);
            addUserToOnlineUsers(conn, username);
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
        chat::LogoutResponse response;
        response.set_success(true);
        response.set_error_message("");
        codec_.send(conn, response);
    }


    void onRegisterRequest(const TcpConnectionPtr &conn,
                        const RegisterRequestPtr &message,
                        Timestamp)
    {
        LOG_INFO << "onRegisterRequest: " << message->GetTypeName();

        chat::RegisterResponse response;
        //username不能是guest
        if (message->username() == "guest")
        {
            response.set_success(false);
            response.set_error_message("Invalid username.");
            codec_.send(conn, response);
            return;
        }
        // 使用 HSETNX 命令在 Redis 中添加用户信息
        redisReply *reply = (redisReply *)redisCommand(redis_ctx_, "HSETNX users %s %s", message->username().c_str(), message->password().c_str());
        bool userAdded = reply->integer == 1;
        freeReplyObject(reply);

        if (userAdded)
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
        if (message->online_only()) 
        {
            MutexLockGuard lock(online_users_mutex_);
            for (const auto &user : online_users_)
            {
                if (message->keyword().empty() || user.first.find(message->keyword()) != std::string::npos)
                {
                    response.add_usernames(user.first);
                }
            }
        }
        else
        {
            // 获取 Redis 中的所有用户名
            redisReply *reply = (redisReply *)redisCommand(redis_ctx_, "HKEYS users");
            if (reply->type == REDIS_REPLY_ARRAY)
            {
                for (size_t i = 0; i < reply->elements; i++)
                {
                    std::string username = reply->element[i]->str;
                    // 检查关键词是否为空或用户名包含关键词
                    if (message->keyword().empty() || username.find(message->keyword()) != std::string::npos)
                    {
                        response.add_usernames(username);
                    }
                }
            }
            freeReplyObject(reply);
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
        redisReply *existsReply = (redisReply *)redisCommand(redis_ctx_, "EXISTS group:%s:users", group_name.c_str());
        bool groupExists = existsReply->integer == 1;
        freeReplyObject(existsReply);

        if (message->operation() == chat::GroupOperation::CREATE)
        {
            if (!groupExists)
            {
                redisReply *reply1 = (redisReply *)redisCommand(redis_ctx_, "SADD group:%s:users %s", group_name.c_str(), username.c_str());
                redisReply *reply2 = (redisReply *)redisCommand(redis_ctx_, "SADD user:%s:groups %s", username.c_str(), group_name.c_str());
                operationSuccess = reply1->integer == 1 && reply2->integer == 1;
                freeReplyObject(reply1);
                freeReplyObject(reply2);
            }

            response.set_success(operationSuccess);
            if (!operationSuccess)
            {
                response.set_error_message("Group already exists.");
            }
        }
        else if (message->operation() == chat::GroupOperation::JOIN)
        {
            if (groupExists)
            {
                redisReply *reply1 = (redisReply *)redisCommand(redis_ctx_, "SADD group:%s:users %s", group_name.c_str(), username.c_str());
                redisReply *reply2 = (redisReply *)redisCommand(redis_ctx_, "SADD user:%s:groups %s", username.c_str(), group_name.c_str());
                operationSuccess = reply1->integer == 1 && reply2->integer == 1;
                freeReplyObject(reply1);
                freeReplyObject(reply2);
            }

            response.set_success(operationSuccess);
            if (!operationSuccess)
            {
                if (!groupExists)
                {
                    response.set_error_message("Group does not exist.");
                }
                else
                {
                    response.set_error_message("User already in the group.");
                }
            }
        }
        else if (message->operation() == chat::GroupOperation::LEAVE)
        {
            if (groupExists)
            {
                redisReply *reply1 = (redisReply *)redisCommand(redis_ctx_, "SREM group:%s:users %s", group_name.c_str(), username.c_str());
                redisReply *reply2 = (redisReply *)redisCommand(redis_ctx_, "SREM user:%s:groups %s", username.c_str(), group_name.c_str());
                operationSuccess = reply1->integer == 1 && reply2->integer == 1;
                freeReplyObject(reply1);
                freeReplyObject(reply2);

                // 如果群组没有成员了，删除群组
                redisReply *cardReply1 = (redisReply *)redisCommand(redis_ctx_, "SCARD group:%s:users", group_name.c_str());
                if (cardReply1->integer == 0)
                {
                    redisReply *delReply = (redisReply *)redisCommand(redis_ctx_, "DEL group:%s:users", group_name.c_str());
                    freeReplyObject(delReply);
                }
                freeReplyObject(cardReply1);

                // 如果用户没有加入任何群组，删除对应集合
                redisReply *cardReply2 = (redisReply *)redisCommand(redis_ctx_, "SCARD user:%s:groups", username.c_str());
                if (cardReply2->integer == 0)
                {
                    redisReply *delReply = (redisReply *)redisCommand(redis_ctx_, "DEL user:%s:groups", username.c_str());
                    freeReplyObject(delReply);
                }
                freeReplyObject(cardReply2);
            }

            response.set_success(operationSuccess);
            if (!operationSuccess)
            {
                if (!groupExists)
                {
                    response.set_error_message("Group does not exist.");
                }
                else
                {
                    response.set_error_message("User not in the group.");
                }
            }
        }
        else if (message->operation() == chat::GroupOperation::QUERY)
        {
            operationSuccess = true;
            response.set_success(operationSuccess);
        }
        else
        {
            LOG_ERROR << "Unknown group operation.";
            response.set_success(false);
            response.set_error_message("Unknown group operation.");
        }
        // 获取用户所加入的所有群组
        redisReply *userGroupsReply = (redisReply *)redisCommand(redis_ctx_, "SMEMBERS user:%s:groups", username.c_str());
        if (userGroupsReply->type == REDIS_REPLY_ARRAY)
        {
            for (size_t i = 0; i < userGroupsReply->elements; i++)
            {
                response.add_joined_groups(userGroupsReply->element[i]->str);
            }
        }
        freeReplyObject(userGroupsReply);

        codec_.send(conn, response);
    }

    void onUnknownMessageType(const TcpConnectionPtr &conn,
                              const MessagePtr &message,
                              Timestamp)
    {
        LOG_INFO << "onUnknownMessageType: " << message->GetTypeName();
        conn->shutdown();
    }


    TcpServer server_;
    ThreadPool threadPool_;
    ProtobufDispatcher dispatcher_;
    ProtobufCodec codec_;
    RedisPubSub redis_pubsub_;
    
    MutexLock online_users_mutex_;
    MutexLock loops_mutex_;

    OnlineUserMap online_users_ GUARDED_BY(online_users_mutex_);
    std::set<EventLoop*> loops_ GUARDED_BY(loops_mutex_);
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
