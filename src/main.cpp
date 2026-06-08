#include <iostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <poll.h>
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
// sendJson — надсилає JSON клієнту.
//
// Стратегія запису: blocking з коротким SO_SNDTIMEO.
//
// Чому не pure non-blocking:
//   OpenSSL вимагає що якщо SSL_write повернув WANT_WRITE,
//   наступний виклик МУСИТЬ бути з тим самим ptr і тим самим size.
//   Якщо ми дропаємо і йдемо далі — SSL стан сокета ламається,
//   всі наступні SSL_write на цьому fd падають з err=1 (SSL_ERROR_SSL).
//   Саме це і траплялось: після дропу 4 МБ аватара клієнт відключався.
//
// Стратегія:
//   1. Payload > MAX_SEND_BYTES (700 KB) — відхиляємо одразу, не пишемо.
//      Аватари такого розміру не повинні потрапляти сюди після
//      обмеження в uploadUserAvatar (512 KB). Якщо старий великий
//      аватар все ще є в БД — клієнт отримає порожній рядок замість
//      даних і має показати заглушку.
//   2. Малий payload — перемикаємо в blocking з таймаутом 3 с,
//      пишемо повністю, відновлюємо non-blocking.
//      3 секунди достатньо для будь-якого JSON чат-повідомлення
//      навіть на повільному зʼєднанні, і не заморожує epoll loop
//      надовго при мертвому клієнті.
//--------------------------------------------------
static constexpr size_t MAX_SEND_BYTES = 700 * 1024; // 700 KB

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
    // GUARD: відхиляємо занадто великі payload.
    // Якщо аватар > 700 KB потрапив сюди (старий запис в БД) —
    // просто не надсилаємо. SSL стан сокета залишається чистим.
    // Нові аватари обмежені 512 KB в uploadUserAvatar.
    //--------------------------------
    if(out.size() > MAX_SEND_BYTES)
    {
        std::cout << "SEND REJECTED: payload too large ("
                  << out.size() << " bytes) fd=" << client.fd << "\n";
        return;
    }

    //--------------------------------
    // Перевіряємо що сокет готовий до запису.
    // poll() з таймаутом 200 мс — якщо сокет не готовий
    // (клієнт мертвий або TCP буфер повний) — не чекаємо,
    // просто виходимо. Epoll loop не блокується.
    //--------------------------------
    {
        struct pollfd pfd{};
        pfd.fd     = client.fd;
        pfd.events = POLLOUT;
        int ready  = poll(&pfd, 1, 200); // 200 мс максимум
        if(ready <= 0)
        {
            // Сокет не готовий або помилка — не пишемо,
            // epoll сам виявить мертве зʼєднання при наступному read
            std::cout << "SEND SKIPPED (not writable) fd=" << client.fd << "\n";
            return;
        }
    }

    //--------------------------------
    // Перемикаємо в blocking з коротким таймаутом.
    // Після poll() буфер гарантовано має місце,
    // тому SSL_write поверне результат майже миттєво.
    // Таймаут 1 с — лише страховка від race condition.
    //--------------------------------
    int oldFlags = fcntl(client.fd, F_GETFL, 0);
    fcntl(client.fd, F_SETFL, oldFlags & ~O_NONBLOCK);

    struct timeval tv{};
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    setsockopt(client.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    //--------------------------------
    // Write loop
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

            std::cout << "SSL WRITE ERROR fd=" << client.fd
                      << " err=" << err
                      << " remaining=" << remaining << std::endl;
            break;
        }

        ptr       += res;
        remaining -= res;
    }

    //--------------------------------
    // Відновлюємо non-blocking і скидаємо таймаут
    //--------------------------------
    fcntl(client.fd, F_SETFL, oldFlags);

    tv.tv_sec  = 0;
    tv.tv_usec = 0;
    setsockopt(client.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}


//--------------------------------------------------
void broadcastUserStatus(const std::string& email,
                         bool online)
{
    json j = {
        {"type",   "user_status"},
        {"email",  email},
        {"online", online}
    };

    //--------------------------------
    // SNAPSHOT fd-ів щоб не ітеруватись
    // по map під час SSL_write —
    // помилка запису не повинна
    // модифікувати clients під час циклу.
    // Мертві з'єднання epoll виявить сам
    // при наступному read-евенті.
    //--------------------------------
    std::vector<int> targets;
    targets.reserve(clients.size());

    for(auto& [clientFd, client] : clients)
    {
        if(client.fd <= 0 ||
           client.ssl == nullptr ||
           !client.handshakeDone)
        {
            continue;
        }

        //--------------------------------
        // НЕ НАДСИЛАЄМО ЮЗЕРУ ЩО ВИХОДИТЬ
        //--------------------------------
        if(!online &&
           socketToEmail.count(clientFd) &&
           socketToEmail[clientFd] == email)
        {
            continue;
        }

        targets.push_back(clientFd);
    }

    for(int targetFd : targets)
    {
        if(!clients.count(targetFd)) continue;
        Client& c = clients[targetFd];
        if(!c.ssl) continue;

        std::cout << "[broadcastUserStatus] sending to fd=" << targetFd << std::endl;
        sendJson(c, j);
        std::cout << "[broadcastUserStatus] sent to fd=" << targetFd
                  << " still_alive=" << (clients.count(targetFd) ? "yes" : "NO!") << std::endl;
    }
}

//--------------------------------------------------
void closeClient(int fd)
{
    std::cout << "[closeClient] fd=" << fd << std::endl;

    //--------------------------------
    // EXISTS?
    //--------------------------------
    if(!clients.count(fd))
    {
        std::cout << "[closeClient] fd=" << fd << " not in clients, skip\n";
        return;
    }

    //--------------------------------
    // SAVE EMAIL
    //--------------------------------
    std::string email;

    if(socketToEmail.count(fd))
        email = socketToEmail[fd];

    std::cout << "[closeClient] fd=" << fd
              << " email=" << (email.empty() ? "(none)" : email) << std::endl;

    //--------------------------------
    // SAVE SSL (до erase!)
    //--------------------------------
    SSL* ssl = clients[fd].ssl;

    //--------------------------------
    // 1. ВИДАЛЯЄМО З EPOLL
    //--------------------------------
    epoll_ctl(
        epollFd,
        EPOLL_CTL_DEL,
        fd,
        nullptr
    );

    //--------------------------------
    // 2. SSL FREE (без shutdown —
    // якщо з'єднання вже мертве,
    // SSL_shutdown генерує зайві помилки)
    //--------------------------------
    if(ssl)
    {
        SSL_free(ssl);
    }

    //--------------------------------
    // 3. ЗАКРИВАЄМО СОКЕТ
    //--------------------------------
    close(fd);

    //--------------------------------
    // 4. ВИДАЛЯЄМО З clients
    //--------------------------------
    clients.erase(fd);

    //--------------------------------
    // 5. ВИДАЛЯЄМО З onlineUsers/socketToEmail
    // Перевіряємо що onlineUsers[email]
    // все ще вказує на ЦЕЙ fd.
    // Якщо юзер вже перелогінився під новим fd —
    // не чіпаємо його запис.
    //--------------------------------
    if(!email.empty())
    {
        if(onlineUsers.count(email) &&
           onlineUsers[email] == fd)
        {
            onlineUsers.erase(email);
            std::cout << "[closeClient] removed " << email << " from onlineUsers\n";
        }
        else if(onlineUsers.count(email))
        {
            std::cout << "[closeClient] kept " << email
                      << " in onlineUsers (now fd=" << onlineUsers[email] << ")\n";
        }

        socketToEmail.erase(fd);
    }

    //--------------------------------
    // 6. BROADCAST "offline"
    // Викликається ПІСЛЯ того як клієнт
    // повністю прибраний зі всіх структур.
    //--------------------------------
    if(!email.empty())
    {
        std::cout << "[closeClient] broadcasting offline for " << email << std::endl;
        broadcastUserStatus(email, false);
        std::cout << "[closeClient] broadcast done for " << email << std::endl;
    }

    std::cout << "[closeClient] done fd=" << fd << std::endl;
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

    // Вимикаємо серверний session cache.
    // SSL_free() під час закриття одного клієнта модифікує спільний
    // SSL_CTX cache, що може інвалідувати SSL стан інших клієнтів.
    SSL_CTX_set_session_cache_mode(ssl_ctx, SSL_SESS_CACHE_OFF);

    // Вимикаємо session tickets (TLS 1.3 resumption)
    SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_TICKET);

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

                    // Детальний лог: err=1 SSL_ERROR_SSL (зіпсований стан протоколу)
                    //                err=5 SSL_ERROR_SYSCALL (мережева помилка / EOF)
                    //                err=6 SSL_ERROR_ZERO_RETURN (клієнт надіслав close_notify)
                    unsigned long sslErr = ERR_get_error();
                    char errBuf[256] = {};
                    ERR_error_string_n(sslErr, errBuf, sizeof(errBuf));
                    std::cout << "SSL READ ERROR fd=" << fd
                              << " err=" << err
                              << " detail=" << errBuf << std::endl;

                    closeClient(fd);
                    continue;
                }

                client->buffer.append(
                    buffer,
                    bytes);

                //--------------------------------
                // PARSE
                // УВАГА: після handleMessage client*
                // може стати dangling (якщо closeClient
                // викликався зсередині — kick старої сесії,
                // erase в map тощо).
                // Тому перед кожною ітерацією робимо
                // clients.find(fd) заново.
                //--------------------------------
                while(true)
                {
                    auto cur = clients.find(fd);
                    if(cur == clients.end())
                        break;

                    size_t pos = cur->second.buffer.find('\n');
                    if(pos == std::string::npos)
                        break;

                    std::string msg =
                        cur->second.buffer.substr(0, pos);

                    cur->second.buffer.erase(0, pos + 1);

                    std::cout
                        << "RAW: "
                        << msg
                        << std::endl;

                    try
                    {
                        json j = json::parse(msg);

                        // Після handleMessage cur може бути недійсним —
                        // наступна ітерація знову робить find(fd)
                        handleMessage(cur->second, j, fd);
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