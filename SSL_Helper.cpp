#include "SSL_Helper.h"

SSL_Helper::SSL_Helper(const TcpConnectionPtr& conn)
{
    m_EncryptedSendData.resize(1024 * 10);
    m_DecryptedRecvData.resize(1024 * 10);
    m_CurrRecived = 0;
    m_BytesSizeRecieved = 0;
    m_TotalRecived = 0;
    m_Handshaked = false;

    m_conn = conn;
}

SSL_Helper::~SSL_Helper()
{
    printf("~SSL_Helper()\n");
    if (m_Ssl)
    {
        SSL_free(m_Ssl);
    }
    if (m_SslCtx)
    {
        SSL_CTX_free(m_SslCtx);
    }
}

void SSL_Helper::set_type(SSL_TYPE type)
{
    m_type = type; 
}

void SSL_Helper::set_connected_callback(std::function<void()> fun)
{
    m_SSL_connected_callback = fun;
}

void SSL_Helper::set_receive_callback(std::function<int(SSL_Helper*, unsigned char*, size_t)> fun)
{
    m_SSL_receive_callback = fun;
}

void SSL_Helper::onConnection(const TcpConnectionPtr& conn)
{
    if (conn->connected())
    {
        if(m_type == SERVER)
            do_ssl_accept();
        else 
            do_ssl_connect();
    }
    else
    {
        LOG_WARN << "connect closed";
    }
}

void SSL_Helper::onMessage(const TcpConnectionPtr& conn, Buffer* buf, muduo::Timestamp time)
{
    printf("receive data, size:%ld \n", buf->readableBytes());

    auto datalen = buf->readableBytes();
    SSLProcessingRecv(buf->peek(), datalen);
    buf->retrieveAll();
}

void SSL_Helper::SSLReceiveData()
{
    printf("m_CurrRecived:%lld ", m_CurrRecived);
    printf("m_TotalRecived:%lld\n ", m_TotalRecived);

    if (m_SSL_receive_callback)
        m_SSL_receive_callback(this, m_DecryptedRecvData.data(), m_DecryptedRecvData.size());
    
    m_DecryptedRecvData.clear();
}

void SSL_Helper::SSLConnected()
{
    if (m_SSL_connected_callback)
        m_SSL_connected_callback();
}

void SSL_Helper::SSLSendData(char* data, size_t size)
{
    int	ret = SSL_write(m_Ssl, data, size);
    int	ssl_error = SSL_get_error(m_Ssl, ret);

    if (IsSSLError(ssl_error))
        close_session();

    SSLProcessingSend();
}

int SSL_Helper::do_ssl_accept()
{
    init_ssl();
    CreateServerSSLContext();
    SSL_set_accept_state(m_Ssl);
    SSLProcessingAccept();
    return 1;
}

int SSL_Helper::do_ssl_connect()
{
    init_ssl();
    CreateClientSSLContext();
    SSL_set_connect_state(m_Ssl);
    SSLProcessingConnect();
    return 1;
}

void SSL_Helper::SSLProcessingAccept()
{
    int ret;
    int ssl_error;

    int dwBytesSizeRecieved = 0;

    do
    {
        ret = SSL_read(m_Ssl, m_DecryptedRecvData.data(), m_DecryptedRecvData.size());
        ssl_error = SSL_get_error(m_Ssl, ret);

        if (IsSSLError(ssl_error))
            close_session();

        if (ret > 0)
            dwBytesSizeRecieved += ret;
    } while (ret > 0);


    if (SSL_is_init_finished(m_Ssl))
    {
        m_Handshaked = true;
        SSLReceiveData();//receive data from ssl sockets
    }

    SSLProcessingSend();
}

void SSL_Helper::SSLProcessingConnect()
{
    int ret;
    int ssl_error;

    int bytesSizeRecieved = 0;
    do
    {
        ret = SSL_read(m_Ssl, m_DecryptedRecvData.data(), m_DecryptedRecvData.size());
        ssl_error = SSL_get_error(m_Ssl, ret);

        if (IsSSLError(ssl_error))
            close_session();

        if (ret > 0)
            bytesSizeRecieved += ret;

    } while (ret > 0);


    if (SSL_is_init_finished(m_Ssl))
    {
        m_Handshaked = true;
        SSLReceiveData();//receive data from ssl sockets
    }


    SSLProcessingSend();
}

void SSL_Helper::SSLProcessingSend()
{
    int ret;
    int ssl_error;

    while (BIO_pending(m_Bio[SEND]))
    {
        ret = BIO_read(m_Bio[SEND], m_EncryptedSendData.data(), m_EncryptedSendData.size());

        if (ret > 0)
        {
            m_conn->send(reinterpret_cast<char*>(m_EncryptedSendData.data()), ret);
        }
        else
        {
            ssl_error = SSL_get_error(m_Ssl, ret);

            if (IsSSLError(ssl_error))
                close_session();
        }
    }
}

void SSL_Helper::SSLProcessingRecv(const char*  RecvBuffer, size_t BytesSizeRecieved)
{
    int ret;
    int ssl_error;

    m_BytesSizeRecieved += BytesSizeRecieved;
    if (m_BytesSizeRecieved > 0)
    {
        ret = BIO_write(m_Bio[RECV], RecvBuffer, BytesSizeRecieved);

        if (ret > 0)
        {
            int intRet = ret;
            if (intRet > m_BytesSizeRecieved)
                close_session();

            m_BytesSizeRecieved -= intRet;
        }
        else
        {
            ssl_error = SSL_get_error(m_Ssl, ret);
            if (IsSSLError(ssl_error))
                close_session();
        }
    }


    do
    {
        assert(m_DecryptedRecvData.size() - m_CurrRecived > 0);
        ret = SSL_read(m_Ssl, m_DecryptedRecvData.data() + m_CurrRecived, m_DecryptedRecvData.size() - m_CurrRecived);

        if (ret > 0)
        {
            m_CurrRecived += ret;
            m_TotalRecived += ret;

            if (m_Handshaked)
            {
                SSLReceiveData();
            }
        }
        else
        {
            ssl_error = SSL_get_error(m_Ssl, ret);

            if (IsSSLError(ssl_error))
                close_session();
        }
    } while (ret > 0);

    if (!m_Handshaked)
    {
        if (SSL_is_init_finished(m_Ssl))
        {
            m_Handshaked = true;
            SSLConnected();
        }
    }


    SSLProcessingSend();
}

void SSL_Helper::SSLProcessingRecvAndGetDecryptedData(const char*  RecvBuffer, size_t BytesSizeRecieved, std::vector<unsigned char>& decryptedData)
{
    int ret;
    int ssl_error;

    m_BytesSizeRecieved += BytesSizeRecieved;
    if (m_BytesSizeRecieved > 0)
    {
        ret = BIO_write(m_Bio[RECV], RecvBuffer, BytesSizeRecieved);

        if (ret > 0)
        {
            int intRet = ret;
            if (intRet > m_BytesSizeRecieved)
                close_session();

            m_BytesSizeRecieved -= intRet;
        }
        else
        {
            ssl_error = SSL_get_error(m_Ssl, ret);
            if (IsSSLError(ssl_error))
                close_session();
        }
    }


    do
    {
        assert(m_DecryptedRecvData.size() - m_CurrRecived > 0);
        ret = SSL_read(m_Ssl, m_DecryptedRecvData.data() + m_CurrRecived, m_DecryptedRecvData.size() - m_CurrRecived);

        if (ret > 0)
        {
            m_CurrRecived += ret;
            m_TotalRecived += ret;

            if (m_Handshaked)
            {
                decryptedData.insert(decryptedData.end(), m_DecryptedRecvData.begin(), m_DecryptedRecvData.end());
                SSLReceiveData();
            }
        }
        else
        {
            ssl_error = SSL_get_error(m_Ssl, ret);

            if (IsSSLError(ssl_error))
                close_session();
        }
    } while (ret > 0);

    if (!m_Handshaked)
    {
        if (SSL_is_init_finished(m_Ssl))
        {
            m_Handshaked = true;
            SSLConnected();
        }
    }


}


void SSL_Helper::init_ssl()
{
    SSL_load_error_strings();
    SSL_library_init();

    OpenSSL_add_all_algorithms();
    #if OPENSSL_VERSION_NUMBER < 0x30000000L
    ERR_load_BIO_strings();
    #endif
}

void SSL_Helper::CreateClientSSLContext()
{
    m_SslCtx = SSL_CTX_new(SSLv23_method());
//	SSL_CTX_set_verify(m_SslCtx, SSL_VERIFY_NONE, nullptr);

    m_Ssl = SSL_new(m_SslCtx);

    m_Bio[SEND] = BIO_new(BIO_s_mem());
    m_Bio[RECV] = BIO_new(BIO_s_mem());
    SSL_set_bio(m_Ssl, m_Bio[RECV], m_Bio[SEND]);
}

void SSL_Helper::CreateServerSSLContext()
{
    m_SslCtx = SSL_CTX_new(SSLv23_server_method());
    SSL_CTX_set_verify(m_SslCtx, SSL_VERIFY_NONE, nullptr);


    SetSSLCertificate();
    m_Ssl = SSL_new(m_SslCtx);

    m_Bio[SEND] = BIO_new(BIO_s_mem());
    m_Bio[RECV] = BIO_new(BIO_s_mem());
    SSL_set_bio(m_Ssl, m_Bio[RECV], m_Bio[SEND]);
}

void SSL_Helper::SetSSLCertificate()
{
    int length = strlen(ca_cert_key_pem);
    BIO *bio_cert = BIO_new_mem_buf((void*)ca_cert_key_pem, length);
    X509 *cert = PEM_read_bio_X509(bio_cert, nullptr, nullptr, nullptr);
    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio_cert, 0, 0, 0);


    int ret = SSL_CTX_use_certificate(m_SslCtx, cert);

    if (ret != 1)
        close_session();

    ret = SSL_CTX_use_PrivateKey(m_SslCtx, pkey);

    if (ret != 1)
        close_session();

    X509_free(cert);
    EVP_PKEY_free(pkey);
    BIO_free(bio_cert);
}

bool SSL_Helper::IsSSLError(int ssl_error)
{
    switch (ssl_error)
    {
    case SSL_ERROR_NONE:
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
    case SSL_ERROR_WANT_CONNECT:
    case SSL_ERROR_WANT_ACCEPT:
        return false;

    default: return true;
    }
}

void SSL_Helper::close_session()
{
    m_conn->forceClose();
    printf("close_session()\n");
}
