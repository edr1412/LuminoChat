#include "codec.h"
#include "dispatcher.h"
#include "chat.pb.h"

#include <muduo/base/Logging.h>
#include <muduo/base/Mutex.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThread.h>
#include <muduo/net/EventLoopThreadPool.h>
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
        username_("guest"),
        recvMsgCnt_(0),
        recvMsgBytes_(0),
        sendMsgCnt_(0),
        sendMsgBytes_(0)
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
  int RecvMsgCnt() const {
      return recvMsgCnt_;
  }
  long long RecvMsgBytes() const {
      return recvMsgBytes_;
  }
  int SendMsgCnt() const {
      return sendMsgCnt_;
  }
  long long SendMsgBytes() const {
      return sendMsgBytes_;
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
    //LOG_INFO << conn->localAddress().toIpPort() << " -> "
            //  << conn->peerAddress().toIpPort() << " is "
            //  << (conn->connected() ? "UP" : "DOWN");

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
    //LOG_INFO << "onTextMessage: " << message->GetTypeName();
    //LOG_INFO << "From: " << message->sender() << ", Message: " << message->content();
    //printf("<<< [%s] %s\n", message->sender().c_str(), message->content().c_str());
    recvMsgCnt_ ++;
    recvMsgBytes_ += message->content().size();
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
      sendMsgCnt_++;
      sendMsgBytes_ += message.size();
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
  int recvMsgCnt_; //收到的消息条数
  long long recvMsgBytes_; //收到的消息字符数
  int sendMsgCnt_;
  int sendMsgBytes_;
};


#include <random>
int g_usrNum;
int g_threadNum;
int g_msgLen;
std::string g_msgContent;
class ChatMultiClient{
public:
    ChatMultiClient(EventLoop* loop, InetAddress serverAddr, int userNum)
        : baseloop_(loop),
          serverAddr_(serverAddr),
          userNum_(userNum),
          threadPool_(new EventLoopThreadPool(loop, "multiclientpool")),
          gen_(rd_()),
          dis_(0.5, 1.0)
    {
       
    }
    ~ChatMultiClient(){

    }

    void start(){
         // 创建并启动线程池
        threadPool_->setThreadNum(g_threadNum); 
        threadPool_->start();
        for(int i = 0; i < userNum_; i++) {
            EventLoop* ioLoop  = threadPool_->getNextLoop();
            chatclients_.push_back(std::make_shared<ChatClient>(ioLoop, serverAddr_)); 
            chatclients_.back()->connect();
            TimerId timerId = ioLoop->runEvery(
               dis_(gen_), //随机的时间
                std::bind(&ChatClient::send, chatclients_.back(), "send "+g_msgContent)
            );
            ioLoop->runAfter(
                10, // 10s后结束发送
                std::bind(&EventLoop::cancel, ioLoop, timerId)
            );
        }
    }

    void stop(){ //需确保所有客户端的发送任务都已完成
        int idx = 0;
        int totalSendCnt = 0, totalRecvCnt = 0;
        long long totalSendBytes = 0, totalRecvBytes = 0;
        for(auto& client : chatclients_){
            printf("客户端%d: 发送%d条(%lld Bytes), 接收%d条(%lld Bytes)\n",
                    idx++, client->SendMsgCnt(), client->SendMsgBytes(),
                    client->RecvMsgCnt(), client->RecvMsgBytes());
            totalSendCnt += client->SendMsgCnt();
            totalSendBytes += client->SendMsgBytes();
            totalRecvCnt += client->RecvMsgCnt();
            totalRecvBytes += client->RecvMsgBytes();

            client->disconnect();
            

        }
        
        printf("\n\n====================================\n\n");
        printf("             多用户客户端测试报告           \n");
        printf(" 总计发送%d条,共%lld字节\n", totalSendCnt, totalSendBytes);
        printf(" 总计接收%d条,共%lld字节\n",totalRecvCnt, totalRecvBytes);
        printf("\n\n====================================\n\n");


    }

    void setThreadNum(int threadNum){
        assert(0 <=threadNum);
        threadPool_->setThreadNum(threadNum);
    }


private:
    EventLoop* baseloop_;
    InetAddress serverAddr_;
    int userNum_;
    std::unique_ptr<EventLoopThreadPool>  threadPool_;
//    std::vector<ChatClient> chatclients_; //所有的客户端
    std::vector<std::shared_ptr<ChatClient>> chatclients_;

    //随机数
    std::random_device rd_;
    std::mt19937 gen_;
    std::uniform_real_distribution<> dis_;
};




int main(int argc, char* argv[]){
    if(argc < 4){
        printf("Usage: <%s> <Server IP> <Port> <userNum> <threadNum = 1> <msgLen = 128>", argv[0]);
        exit(-1);
    }
    g_usrNum = atoi(argv[3]);
    g_threadNum = (argc >= 5) ? atoi(argv[4]) : 1;
    g_msgLen = (argc >= 6) ? atoi(argv[5]) : 128;
    g_msgContent = std::string(g_msgLen, 'S');

    EventLoop loop;
    InetAddress serverAddr(argv[1], atoi(argv[2]));

    ChatMultiClient multiClient(&loop, serverAddr, g_usrNum);
    multiClient.start();

    using namespace std::chrono_literals;
    CurrentThread::sleepUsec(11000*1000); // 11s后统计并断开连接
    multiClient.stop();
    CurrentThread::sleepUsec(2000*1000); // wait for disconnect, then safe to destruct LogClient (esp. TcpClient). Otherwise mutex_ is used after dtor.
    return 0;
}

