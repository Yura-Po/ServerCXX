
#include <iostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <signal.h>

#include <unordered_map>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <nlohmann/json.hpp>

#include "models/client.h"
#include "handlers/message_handler.h"

#define PORT 5000
#define MAX_EVENTS 100

using json = nlohmann::json;

std::unordered_map<int, Client> clients;
std::unordered_map<std::string, int> onlineUsers;
std::unordered_map<int, std::string> socketToEmail;

SSL_CTX* ssl_ctx;
int epollFd;
void closeClient(int fd);

//--------------------------------------------------
int setNonBlocking(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

//--------------------------------------------------
void sendJson(Client &client,
              const json& j)
{
    //--------------------------------
    // INVALID SSL
    //--------------------------------
    if(client.ssl == nullptr)
        return;

    if(clients.find(client.fd) == clients.end())
        return;

    std::string out =
        j.dump() + "\n";

    //--------------------------------
    // LOG (truncate large payloads)
    //--------------------------------
    if(out.size() <= 300)
        std::cout << "SEND: " << out << std::endl;
    else
        std::cout << "SEND: ["
                  << out.size()
                  << " bytes]\n";

    //--------------------------------
    // Switch to blocking for reliable
    // large writes (e.g. file data)
    //--------------------------------
    int oldFlags =
        fcntl(client.fd, F_GETFL, 0);

    fcntl(client.fd,
          F_SETFL,
          oldFlags & ~O_NONBLOCK);

    //--------------------------------
    // Write loop (handles partial writes)
    //--------------------------------
    const char* ptr = out.c_str();
    int remaining   = (int)out.size();

    while(remaining > 0)
    {
        int res =
            SSL_write(
                client.ssl,
                ptr,
                remaining);

        if(res <= 0)
        {
            int err =
                SSL_get_error(
                    client.ssl,
                    res);

            std::cout
                << "SSL WRITE ERROR: "
                << err
                << std::endl;

            break;
        }

        ptr       += res;
        remaining -= res;
    }

    //--------------------------------
    // Restore non-blocking mode
    //--------------------------------
    fcntl(client.fd,
          F_SETFL,
          oldFlags);
}


//--------------------------------------------------
void broadcastUserStatus(const std::string& email,
                         bool online)
{
    json j = {
        {"type", "user_status"},
        {"email", email},
        {"online", online}
    };

    std::vector<int> deadClients;

    for(auto &[clientFd, client] : clients)
    {
        //--------------------------------
        // INVALID CLIENT
        //--------------------------------
        if(client.fd <= 0 ||
           client.ssl == nullptr ||
           !client.handshakeDone)
        {
            continue;
        }

        //--------------------------------
        // DON'T SEND TO LEAVING USER
        //--------------------------------
        if(!online &&
           socketToEmail.count(clientFd) &&
           socketToEmail[clientFd] == email)
        {
            continue;
        }

        //--------------------------------
        // SEND
        //--------------------------------
        std::string out = j.dump() + "\n";

int res = SSL_write(
    client.ssl,
    out.c_str(),
    out.size()
);

        if(res <= 0)
        {
            int err =
                SSL_get_error(
                    client.ssl,
                    res);

            if(err != SSL_ERROR_WANT_READ &&
               err != SSL_ERROR_WANT_WRITE)
            {
                deadClients.push_back(clientFd);
            }
        }
    }

    //--------------------------------
    // REMOVE DEAD CLIENTS
    //--------------------------------
    for(int fd : deadClients)
    {
        closeClient(fd);
    }
}

//--------------------------------------------------
void closeClient(int fd)
{
    std::cout
        << "CLIENT CLOSED: "
        << fd
        << std::endl;

    //--------------------------------
    // EXISTS?
    //--------------------------------
    if(!clients.count(fd))
    return;

    //--------------------------------
    // SAVE EMAIL
    //--------------------------------
    std::string email;

    if(socketToEmail.count(fd))
    {
        email =
            socketToEmail[fd];
    }

    //--------------------------------
    // SAVE SSL
    //--------------------------------
    SSL* ssl =
        clients[fd].ssl;

    //--------------------------------
    // REMOVE CLIENT FIRST
    //--------------------------------
    clients.erase(fd);

    //--------------------------------
    // REMOVE ONLINE
    //--------------------------------
    if(!email.empty())
    {
        onlineUsers.erase(email);
        socketToEmail.erase(fd);
    }

    //--------------------------------
    // REMOVE FROM EPOLL
    //--------------------------------
   epoll_ctl(
    epollFd,
    EPOLL_CTL_DEL,
    fd,
    nullptr
);

    //--------------------------------
    // BROADCAST
    //--------------------------------
    if(!email.empty())
    {
        broadcastUserStatus(
            email,
            false);
    }

    //--------------------------------
    // SSL SHUTDOWN
    //--------------------------------
    if(ssl)
    {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }

    //--------------------------------
    // CLOSE SOCKET
    //--------------------------------
    
    close(fd);
    fd = -1;
}

//--------------------------------------------------
int main()
{
    signal(SIGPIPE, SIG_IGN);
    SSL_library_init();
    SSL_load_error_strings();

    ssl_ctx =
        SSL_CTX_new(TLS_server_method());

    if(!ssl_ctx)
    {
        std::cout << "SSL CTX ERROR\n";
        return 1;
    }

    SSL_CTX_use_certificate_file(
        ssl_ctx,
        "cert.pem",
        SSL_FILETYPE_PEM
    );

    SSL_CTX_use_PrivateKey_file(
        ssl_ctx,
        "key.pem",
        SSL_FILETYPE_PEM
    );

    int serverSocket =
        socket(AF_INET,
               SOCK_STREAM,
               0);

    sockaddr_in addr{};

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(serverSocket,
         (sockaddr*)&addr,
         sizeof(addr));

    listen(serverSocket, SOMAXCONN);

    setNonBlocking(serverSocket);

    epollFd = epoll_create1(0);

    epoll_event ev{}, events[MAX_EVENTS];

    ev.events = EPOLLIN;
    ev.data.fd = serverSocket;

    epoll_ctl(epollFd,
              EPOLL_CTL_ADD,
              serverSocket,
              &ev);

    std::cout << "TLS Server started\n";

    while(true)
    {
        int count =
            epoll_wait(epollFd,
                       events,
                       MAX_EVENTS,
                       -1);

        for(int i = 0; i < count; i++)
        {
            int fd =
                events[i].data.fd;

            //--------------------------------
            // NEW CLIENT
            //--------------------------------
            if(fd == serverSocket)
            {
                int clientFd =
                    accept(serverSocket,
                           nullptr,
                           nullptr);

                if(clientFd < 0)
                {
                    std::cout
                        << "ACCEPT ERROR\n";

                    continue;
                }

                std::cout
                    << "NEW CLIENT: "
                    << clientFd
                    << std::endl;

                // 10-second send timeout so SSL_write never blocks forever
                struct timeval tv{};
                tv.tv_sec = 10;
                setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO,
                           &tv, sizeof(tv));

                setNonBlocking(clientFd);

                SSL* ssl =
                    SSL_new(ssl_ctx);

                SSL_set_fd(ssl, clientFd);

                clients[clientFd] =
                {
                    clientFd,
                    ssl
                };

                epoll_event cev{};

                cev.events =
                    EPOLLIN | EPOLLOUT;

                cev.data.fd =
                    clientFd;

                epoll_ctl(epollFd,
                          EPOLL_CTL_ADD,
                          clientFd,
                          &cev);
            }
            else
            {
                if(!clients.count(fd))
                    continue;

               auto it = clients.find(fd);

if(it == clients.end())
    continue;

Client* client = &it->second;

                //--------------------------------
                // HANDSHAKE
                //--------------------------------
                if(!client->handshakeDone)
                {
                    int res =
                        SSL_accept(client->ssl);

                    if(res == 1)
                    {
                        client->handshakeDone = true;

                        std::cout
                            << "TLS OK (fd="
                            << fd
                            << ")\n";
                    }
                    else
                    {
                        int err =
                            SSL_get_error(
                                client->ssl,
                                res);

                        if(err == SSL_ERROR_WANT_READ ||
                           err == SSL_ERROR_WANT_WRITE)
                            continue;

                        std::cout
                            << "SSL ACCEPT ERROR: "
                            << err
                            << std::endl;

                        closeClient(fd);
                        continue;
                    }
                }

                //--------------------------------
                // READ
                //--------------------------------
                char buffer[1024];

                int bytes =
                    SSL_read(client->ssl,
                             buffer,
                             sizeof(buffer));

                if(bytes <= 0)
                {
                    int err =
                        SSL_get_error(
                            client->ssl,
                            bytes);

                    if(err == SSL_ERROR_WANT_READ ||
                       err == SSL_ERROR_WANT_WRITE)
                        continue;

                    std::cout
                        << "SSL READ ERROR: "
                        << err
                        << std::endl;

                    closeClient(fd);
                    continue;
                }

                client->buffer.append(
                    buffer,
                    bytes);

                //--------------------------------
                // PARSE
                //--------------------------------
                size_t pos;

                while((pos =
                       client->buffer.find('\n'))
                      != std::string::npos)
                {
                    std::string msg =
                        client->buffer.substr(0, pos);

                    client->buffer.erase(
                        0,
                        pos + 1);

                    std::cout
                        << "RAW: "
                        << msg
                        << std::endl;

                    try
                    {
                        json j =
                            json::parse(msg);

                        handleMessage(
                            *client,
                            j,
                            fd);
                    }
                    catch(...)
                    {
                        std::cout
                            << "JSON PARSE ERROR\n";
                    }
                }
            }
        }
    }
}