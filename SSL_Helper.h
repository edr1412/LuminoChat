#pragma once

#include <muduo/base/Logging.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/net/TcpServer.h>

#include <muduo/base/Timestamp.h>
#include <functional>
#include <vector>
#include <memory>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include "cert.h"

using namespace muduo::net;

enum OVERLAPPED_TYPE
{
	RECV = 0,
	SEND = 1
};

enum SSL_TYPE
{
	CLIENT = 0,
	SERVER = 1
};

class SSL_Helper 
{
private:
	TcpConnectionPtr m_conn;

	SSL_CTX *m_SslCtx;
	SSL *m_Ssl;
	BIO *m_Bio[2]; 

	bool m_Handshaked;

	std::vector<unsigned char> m_EncryptedSendData;
	std::vector<unsigned char> m_DecryptedRecvData;
	int m_SendSize;

	unsigned long long  m_BytesSizeRecieved;
	unsigned long long  m_TotalRecived;
	unsigned long long  m_CurrRecived;

	std::function<void()> m_SSL_connected_callback;
	std::function<int(SSL_Helper*, unsigned char*, size_t)> m_SSL_receive_callback;
	std::function<void()> m_SSL_closed_callback;
	SSL_TYPE m_type;

	void close_session();

public:	
	SSL_Helper(const TcpConnectionPtr& conn);
	~SSL_Helper();
	void set_type(SSL_TYPE type);
	void set_connected_callback(std::function<void()> fun);
	void set_receive_callback(std::function<int(SSL_Helper*, unsigned char*, size_t)> fun);
	void onConnection(const TcpConnectionPtr& conn);
	void onMessage(const TcpConnectionPtr& conn, Buffer* buf, muduo::Timestamp time);
	void SSLReceiveData();
	void SSLConnected();
	void SSLSendData(char* data, size_t size);
	int do_ssl_accept();
	int do_ssl_connect();
	void SSLProcessingAccept();
	void SSLProcessingConnect();
	void SSLProcessingSend();
	void SSLProcessingRecv(const char*  RecvBuffer, size_t BytesSizeRecieved);
	void SSLProcessingRecvAndGetDecryptedData(const char*  RecvBuffer, size_t BytesSizeRecieved, std::vector<unsigned char>& decryptedData);
	void init_ssl();
	void CreateClientSSLContext();
	void CreateServerSSLContext();
	void SetSSLCertificate();
	bool IsSSLError(int ssl_error);
};

typedef std::shared_ptr<SSL_Helper> SSL_HelperPtr;
