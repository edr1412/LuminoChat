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
using GroupRequestPtr = std::shared_ptr<chat::GroupRequest>;
using TextMessagePtr = std::shared_ptr<chat::TextMessage>;
using TextMessageResponsePtr = std::shared_ptr<chat::TextMessageResponse>;
using LoginResponsePtr = std::shared_ptr<chat::LoginResponse>;
using LogoutResponsePtr = std::shared_ptr<chat::LogoutResponse>;
using RegisterResponsePtr = std::shared_ptr<chat::RegisterResponse>;
using SearchResponsePtr = std::shared_ptr<chat::SearchResponse>;
using GroupResponsePtr = std::shared_ptr<chat::GroupResponse>;

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
    dispatcher_.registerMessageCallback<chat::LogoutResponse>(
        std::bind(&ChatClient::onLogoutResponse, this, _1, _2, _3));
    dispatcher_.registerMessageCallback<chat::RegisterResponse>(
        std::bind(&ChatClient::onRegisterResponse, this, _1, _2, _3));
    dispatcher_.registerMessageCallback<chat::TextMessage>(
        std::bind(&ChatClient::onTextMessage, this, _1, _2, _3));
    dispatcher_.registerMessageCallback<chat::TextMessageResponse>(
      std::bind(&ChatClient::onTextMessageResponse, this, _1, _2, _3));
    dispatcher_.registerMessageCallback<chat::SearchResponse>(
      std::bind(&ChatClient::onSearchResponse, this, _1, _2, _3));
    dispatcher_.registerMessageCallback<chat::GroupResponse>(
      std::bind(&ChatClient::onGroupResponse, this, _1, _2, _3));
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
    MutexLockGuard lock(connection_mutex_);
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

    MutexLockGuard lock(connection_mutex_);
    if (conn->connected())
    {
      connection_ = conn;
    }
    else
    {
      connection_.reset();
    }
  }

  void onTextMessageResponse(const TcpConnectionPtr &conn,
                            const TextMessageResponsePtr &message,
                            Timestamp)
  {
      LOG_INFO << "onTextMessageResponse: " << message->GetTypeName();

      if (message->success())
      {
          LOG_INFO << "Message sent successfully";
      }
      else
      {
          LOG_ERROR << "Failed to send message: " << message->error_message();
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
      MutexLockGuard lock(username_mutex_);
      username_ = message->username();
    }
    else
    {
      LOG_ERROR << "Login failed: " << message->error_message();
    }
  }

  void onLogoutResponse(const TcpConnectionPtr &conn,
                        const LogoutResponsePtr &message,
                        Timestamp)
  {
    LOG_INFO << "onLogoutResponse: " << message->GetTypeName();

    if (message->success())
    {
      LOG_INFO << "Logout succeeded";
      // MutexLockGuard lock(username_mutex_);
      // username_ = "guest";
    }
    else
    {
      LOG_ERROR << "Logout failed: " << message->error_message();
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

  void onGroupResponse(const TcpConnectionPtr &conn,
                      const GroupResponsePtr &message,
                      Timestamp)
  {
      LOG_INFO << "onGroupResponse: " << message->GetTypeName();

      if (message->success())
      {
          std::string operation;
          switch (message->operation()) {
              case chat::GroupOperation::CREATE:
                  operation = "Create group";
                  break;
              case chat::GroupOperation::JOIN:
                  operation = "Join group";
                  break;
              case chat::GroupOperation::LEAVE:
                  operation = "Leave group";
                  break;
              case chat::GroupOperation::QUERY:
                  operation = "Query groups";
                  break;
              default:
                  operation = "Unknown command";
                  break;
          }
          LOG_INFO << operation << " succeeded";
      }
      else
      {
          LOG_ERROR << "Group operation failed: " << message->error_message();
      }
      {
        MutexLockGuard lock(username_mutex_);
        printf(">>> User %s's Groups:\n", username_.c_str());
      }
      if (message->joined_groups_size() > 0)
      {
          for (const auto &group : message->joined_groups())
          {
              printf(">>> - %s\n", group.c_str());
          }
      }
      else
      {
          printf(">>> (No groups)\n");
      }
  }

  void onTextMessage(const TcpConnectionPtr &conn,
                     const TextMessagePtr &message,
                     Timestamp)
  {
    LOG_INFO << "onTextMessage: " << message->GetTypeName();
    //LOG_INFO << "From: " << message->sender() << ", Message: " << message->content();
    std::string group;
    if (message->target_type() == chat::TargetType::USER){
        group = "private";
    } 
    else if (message->target_type() == chat::TargetType::GROUP)
    {
        group = message->target();
    }
    printf("<<< %s [%s] %s\n", group.c_str(), message->sender().c_str(), message->content().c_str());
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
    MutexLockGuard lock(username_mutex_);

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
      // 如果再次登录自己，则退出
      if (username == username_) {
          return;
      }
      // 如果已经登录，要登录其他用户，则先登出
      if (username_ != "guest") {
          chat::LogoutRequest request;
          request.set_username(username_);
          codec_.send(connection_, request);
          username_ = "guest";
      }
      chat::LoginRequest request;
      request.set_username(username);
      request.set_password(password);
      codec_.send(connection_, request);
    }
    else if (cmd == "send")
    {
      std::string target_type, target, content;
      iss >> target_type >> target >> content;
      chat::TextMessage textMessage;
      textMessage.set_sender(username_);
      textMessage.set_content(content);
      if (target_type == "user")
      {
        textMessage.set_target_type(chat::TargetType::USER);
      }
      else if (target_type == "group")
      {
        textMessage.set_target_type(chat::TargetType::GROUP);
      }
      else
      {
        LOG_ERROR << "Unknown target type: " << target_type;
        LOG_INFO << "Usage: send <user|group> <target> <content>";
        return;
      }
      textMessage.set_target(target);
      codec_.send(connection_, textMessage);
    }
    else if (cmd == "search")
    {
      std::string keyword;
      iss >> keyword;
      chat::SearchRequest request;
      request.set_keyword(keyword);
      request.set_online_only(false);
      codec_.send(connection_, request);
    }
    else if (cmd == "search-online")
    {
      std::string keyword;
      iss >> keyword;
      chat::SearchRequest request;
      request.set_keyword(keyword);
      request.set_online_only(true);
      codec_.send(connection_, request);
    }
    else if (cmd == "group")
    {
      // 如果还没有登录，则不能操作群组
      if (username_ == "guest") {
        LOG_ERROR << "Please login first";
        return;
      }
      std::string operation, group_name;
      iss >> operation >> group_name;
      chat::GroupRequest request;
      request.set_group_name(group_name);
      request.set_username(username_);

      if (operation == "create")
      {
        request.set_operation(chat::GroupOperation::CREATE);
      }
      else if (operation == "join")
      {
        request.set_operation(chat::GroupOperation::JOIN);
      }
      else if (operation == "leave")
      {
        request.set_operation(chat::GroupOperation::LEAVE);
      }
      else if (operation == "query")
      {
        request.set_operation(chat::GroupOperation::QUERY);
      }
      else
      {
        LOG_ERROR << "Unknown group operation: " << operation;
        LOG_INFO << "Usage: group <create|join|leave|query> <groupname>";
        return;
      }
      codec_.send(connection_, request);
    }
    else if (cmd == "logout")
    {
      if (username_ != "guest") {
          chat::LogoutRequest request;
          request.set_username(username_);
          codec_.send(connection_, request);
          username_ = "guest";
      } else {
          LOG_ERROR << "You are not logged in!";
      }
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
  MutexLock connection_mutex_;
  TcpConnectionPtr connection_ GUARDED_BY(connection_mutex_);
  MutexLock username_mutex_;
  std::string username_ GUARDED_BY(username_mutex_);
};

int main(int argc, char *argv[])
{
  LOG_INFO << "pid = " << getpid();
  if (argc > 2)
  {
    EventLoopThread loopThread;
    uint16_t port = static_cast<uint16_t>(atoi(argv[2]));
    InetAddress serverAddr(argv[1], port);

    ChatClient client(loopThread.getLoop(), serverAddr);
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