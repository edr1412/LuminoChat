#include "codec.h"
#include "dispatcher.h"
#include "chat.pb.h"

#include <muduo/base/Logging.h>
#include <muduo/base/Mutex.h>
#include <muduo/net/EventLoopThread.h>
#include <muduo/net/TcpClient.h>

#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <sstream>
#include <unistd.h>

using namespace muduo;
using namespace muduo::net;

using LoginRequestPtr = std::shared_ptr<chat::LoginRequest>;
using RegisterRequestPtr = std::shared_ptr<chat::RegisterRequest>;
using TextMessagePtr = std::shared_ptr<chat::TextMessage>;
using LoginResponsePtr = std::shared_ptr<chat::LoginResponse>;
using RegisterResponsePtr = std::shared_ptr<chat::RegisterResponse>;
using SearchResponsePtr = std::shared_ptr<chat::SearchResponse>;

class ChatClient
{
public:
  ChatClient(EventLoop *loop, const InetAddress &serverAddr)
      : loop_(loop),
        client_(loop, serverAddr, "ChatClient"),
        dispatcher_(std::bind(&ChatClient::onUnknownMessageType, this, _1, _2, _3)),
        codec_(std::bind(&ProtobufDispatcher::onProtobufMessage, &dispatcher_, _1, _2, _3)),
        username_("guest")
  {
    dispatcher_.registerMessageCallback<chat::LoginResponse>(
        std::bind(&ChatClient::onLoginResponse, this, _1, _2, _3));
    dispatcher_.registerMessageCallback<chat::RegisterResponse>(
        std::bind(&ChatClient::onRegisterResponse, this, _1, _2, _3));
    dispatcher_.registerMessageCallback<chat::TextMessage>(
        std::bind(&ChatClient::onTextMessage, this, _1, _2, _3));
    dispatcher_.registerMessageCallback<chat::SearchResponse>(
      std::bind(&ChatClient::onSearchResponse, this, _1, _2, _3));
    client_.setConnectionCallback(
        std::bind(&ChatClient::onConnection, this, _1));
    client_.setMessageCallback(
        std::bind(&ProtobufCodec::onMessage, &codec_, _1, _2, _3));
    client_.enableRetry();
  }

  void connect()
  {
    client_.connect();
  }


  void disconnect()
  {
    client_.disconnect();
  }

  void send(const std::string &line)
  {
    MutexLockGuard lock(mutex_);
    if (connection_)
    {
      processCommand(line);
    }
  }

private:
  void onConnection(const TcpConnectionPtr &conn)
  {
    LOG_INFO << conn->localAddress().toIpPort() << " -> "
             << conn->peerAddress().toIpPort() << " is "
             << (conn->connected() ? "UP" : "DOWN");

    MutexLockGuard lock(mutex_);
    if (conn->connected())
    {
      connection_ = conn;
    }
    else
    {
      connection_.reset();
    }
  }

  void onLoginResponse(const TcpConnectionPtr &conn,
                       const LoginResponsePtr &message,
                       Timestamp)
  {
    LOG_INFO << "onLoginResponse: " << message->GetTypeName();

    if (message->success())
    {
      LOG_INFO << "Login succeeded";
    }
    else
    {
      LOG_ERROR << "Login failed: " << message->error_message();
    }
  }

  void onRegisterResponse(const TcpConnectionPtr &conn,
                          const RegisterResponsePtr &message,
                          Timestamp)
  {
    LOG_INFO << "onRegisterResponse: " << message->GetTypeName();

    if (message->success())
    {
      LOG_INFO << "Register succeeded";
    }
    else
    {
      LOG_ERROR << "Register failed: " << message->error_message();
    }
  }

  void onSearchResponse(const TcpConnectionPtr &conn,
                      const SearchResponsePtr &message,
                      Timestamp)
{
  LOG_INFO << "onSearchResponse: " << message->GetTypeName();
  printf(">>> Search Result:\n");
  for (const auto &username : message->usernames())
  {
    printf(">>> - %s\n", username.c_str());
  }
}

  void onTextMessage(const TcpConnectionPtr &conn,
                     const TextMessagePtr &message,
                     Timestamp)
  {
    LOG_INFO << "onTextMessage: " << message->GetTypeName();
    //LOG_INFO << "From: " << message->sender() << ", Message: " << message->content();
    printf("<<< [%s] %s\n", message->sender().c_str(), message->content().c_str());
  }

  void onUnknownMessageType(const TcpConnectionPtr &conn,
                            const MessagePtr &message,
                            Timestamp)
  {
    LOG_INFO << "onUnknownMessageType: " << message->GetTypeName();
    conn->shutdown();
  }

  void processCommand(const std::string &line)
  {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd == "register")
    {
      std::string username, password;
      iss >> username >> password;
      chat::RegisterRequest request;
      request.set_username(username);
      request.set_password(password);
      codec_.send(connection_, request);
    }
    else if (cmd == "login")
    {
      std::string username, password;
      iss >> username >> password;
      chat::LoginRequest request;
      request.set_username(username);
      request.set_password(password);
      codec_.send(connection_, request);
      username_ = username;
    }
    else if (cmd == "send")
    {
      std::string message;
      std::getline(iss, message);
      chat::TextMessage textMessage;
      textMessage.set_sender(username_);
      textMessage.set_content(message);
      codec_.send(connection_, textMessage);
    }
    else if (cmd == "search")
    {
      std::string keyword;
      iss >> keyword;
      chat::SearchRequest request;
      request.set_keyword(keyword);
      codec_.send(connection_, request);
    }
    else
    {
      LOG_ERROR << "Unknown command: " << cmd;
    }
  }

  EventLoop *loop_;
  TcpClient client_;
  ProtobufDispatcher dispatcher_;
  ProtobufCodec codec_;
  MutexLock mutex_;
  TcpConnectionPtr connection_ GUARDED_BY(mutex_);
  std::string username_;
};

int main(int argc, char *argv[])
{
  LOG_INFO << "pid = " << getpid();
  if (argc > 2)
  {
    EventLoopThread loopThread;
    uint16_t port = static_cast<uint16_t>(atoi(argv[2]));
    InetAddress serverAddr(argv[1], port);

    ChatClient client(loopThread.startLoop(), serverAddr);
    client.connect();

    std::string line;
    while (std::getline(std::cin, line))
    {
      client.send(line);
    }

    client.disconnect();
    CurrentThread::sleepUsec(1000*1000);  // wait for disconnect, see ace/logging/client.cc
  }
  else
  {
    printf("Usage: %s server_ip port\n", argv[0]);
  }
}