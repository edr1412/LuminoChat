#include "codec.h"
#include "dispatcher.h"
#include "chat.pb.h"

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpServer.h>

#include <functional>
#include <unordered_map>
#include <string>
#include <memory>

using namespace muduo;
using namespace muduo::net;

using LoginRequestPtr = std::shared_ptr<chat::LoginRequest>;
using RegisterRequestPtr = std::shared_ptr<chat::RegisterRequest>;
using TextMessagePtr = std::shared_ptr<chat::TextMessage>;
using LoginResponsePtr = std::shared_ptr<chat::LoginResponse>;
using RegisterResponsePtr = std::shared_ptr<chat::RegisterResponse>;

class ChatServer
{
public:
    ChatServer(EventLoop *loop, const InetAddress &listenAddr)
        : server_(loop, listenAddr, "ChatServer"),
          dispatcher_(std::bind(&ChatServer::onUnknownMessageType, this, _1, _2, _3)),
          codec_(std::bind(&ProtobufDispatcher::onProtobufMessage, &dispatcher_, _1, _2, _3))
    {
        dispatcher_.registerMessageCallback<chat::LoginRequest>(
            std::bind(&ChatServer::onLoginRequest, this, _1, _2, _3));
        dispatcher_.registerMessageCallback<chat::RegisterRequest>(
            std::bind(&ChatServer::onRegisterRequest, this, _1, _2, _3));
        dispatcher_.registerMessageCallback<chat::TextMessage>(
            std::bind(&ChatServer::onTextMessage, this, _1, _2, _3));
        server_.setConnectionCallback(
            std::bind(&ChatServer::onConnection, this, _1));
        server_.setMessageCallback(
            std::bind(&ProtobufCodec::onMessage, &codec_, _1, _2, _3));
    }

    void start()
    {
        server_.start();
    }

private:
    void onConnection(const TcpConnectionPtr &conn)
    {
        LOG_INFO << "ChatServer - " << conn->peerAddress().toIpPort() << " -> "
                 << conn->localAddress().toIpPort() << " is "
                 << (conn->connected() ? "UP" : "DOWN");

        if (conn->connected())
        {
            connections_.insert(conn);
        }
        else
        {
            connections_.erase(conn);
        }
    }

    void onLoginRequest(const TcpConnectionPtr &conn,
                        const LoginRequestPtr &message,
                        Timestamp)
    {
        LOG_INFO << "onLoginRequest: " << message->GetTypeName();
        chat::LoginResponse response;

        auto it = users_.find(message->username());
        if (it != users_.end() && it->second == message->password())
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

        auto result = users_.emplace(message->username(), message->password());
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

    void onTextMessage(const TcpConnectionPtr &conn,
                       const TextMessagePtr &message,
                       Timestamp)
    {
        LOG_INFO << "onTextMessage: " << message->GetTypeName();

        for (const auto &connection : connections_)
        {
            codec_.send(connection, *message);
        }
    }

    void onUnknownMessageType(const TcpConnectionPtr &conn,
                              const MessagePtr &message,
                              Timestamp)
    {
        LOG_INFO << "onUnknownMessageType: " << message->GetTypeName();
        conn->shutdown();
    }

    TcpServer server_;
    ProtobufDispatcher dispatcher_;
    ProtobufCodec codec_;
    std::unordered_map<std::string, std::string> users_; // Key: username, Value: password
    std::unordered_set<TcpConnectionPtr> connections_;
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
        server.start();
        loop.loop();
    }
    else
    {
        printf("Usage: %s listen_port\n", argv[0]);
    }
}
