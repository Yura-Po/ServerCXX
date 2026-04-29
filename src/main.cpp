#include <iostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <algorithm>

#include <pqxx/pqxx>
#include <argon2.h>
#include <nlohmann/json.hpp>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "auth.h"

using json = nlohmann::json;

#define PORT 5000
#define MAX_EVENTS 100

//--------------------------------------------------
std::string toLower(const std::string& s)
{
    std::string res = s;
    std::transform(res.begin(), res.end(), res.begin(), ::tolower);
    return res;
}

//--------------------------------------------------
class ConnectionPool
{
private:
    std::queue<std::unique_ptr<pqxx::connection>> pool;
    std::mutex mtx;
    std::condition_variable cv;

public:
    ConnectionPool(int size)
    {
        for(int i = 0; i < size; i++)
        {
            pool.push(std::make_unique<pqxx::connection>(
                "postgresql://chat_user:13312222@127.0.0.1:5432/chat_db"
            ));
        }
    }

    std::unique_ptr<pqxx::connection> acquire()
    {
        std::unique_lock<std::mutex> lock(mtx);
        while(pool.empty()) cv.wait(lock);

        auto conn = std::move(pool.front());
        pool.pop();
        return conn;
    }

    void release(std::unique_ptr<pqxx::connection> conn)
    {
        std::lock_guard<std::mutex> lock(mtx);
        pool.push(std::move(conn));
        cv.notify_one();
    }
};

ConnectionPool dbPool(10);

//--------------------------------------------------
class PooledConnection
{
private:
    std::unique_ptr<pqxx::connection> conn;

public:
    PooledConnection() { conn = dbPool.acquire(); }
    ~PooledConnection() { dbPool.release(std::move(conn)); }

    pqxx::connection& get() { return *conn; }
};

//--------------------------------------------------
struct Client
{
    int fd;
    SSL* ssl;
    bool handshakeDone = false;
    std::string buffer;
};

std::unordered_map<int, Client> clients;
std::unordered_map<std::string, int> onlineUsers;
std::unordered_map<int, std::string> socketToEmail;

SSL_CTX* ssl_ctx;

//--------------------------------------------------
int setNonBlocking(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

//--------------------------------------------------
bool userExists(const std::string& email)
{
    PooledConnection pc;
    pqxx::work txn(pc.get());

    auto r = txn.exec_params(
        "SELECT id FROM users WHERE LOWER(email)=LOWER($1)",
        email
    );

    return !r.empty();
}

//--------------------------------------------------
int getUserId(const std::string& email)
{
    PooledConnection pc;
    pqxx::work txn(pc.get());

    auto r = txn.exec_params(
        "SELECT id FROM users WHERE LOWER(email)=LOWER($1)",
        email
    );

    if(r.empty()) return -1;
    return r[0]["id"].as<int>();
}

//--------------------------------------------------
// ✅ FIXED QUERY
int getPrivateChat(int user1, int user2)
{
    PooledConnection pc;
    pqxx::work txn(pc.get());

    auto r = txn.exec_params(
        R"(
        SELECT c.id
        FROM chats c
        JOIN chat_participants cp ON c.id = cp.chat_id
        WHERE c.type='private'
        AND cp.user_id IN ($1, $2)
        GROUP BY c.id
        HAVING COUNT(DISTINCT cp.user_id) = 2
        )",
        user1, user2
    );

    if(r.empty()) return -1;
    return r[0]["id"].as<int>();
}

//--------------------------------------------------
int createPrivateChat(int user1, int user2)
{
    PooledConnection pc;
    pqxx::work txn(pc.get());

    auto r = txn.exec_params(
        "INSERT INTO chats (type) VALUES ('private') RETURNING id"
    );

    int chatId = r[0]["id"].as<int>();

    txn.exec_params(
        "INSERT INTO chat_participants (chat_id, user_id) VALUES ($1,$2)",
        chatId, user1
    );

    txn.exec_params(
        "INSERT INTO chat_participants (chat_id, user_id) VALUES ($1,$2)",
        chatId, user2
    );

    txn.commit();
    return chatId;
}

//--------------------------------------------------
json loadUserChats(const std::string& email)
{
    int userId = getUserId(email);

    PooledConnection pc;
    pqxx::work txn(pc.get());

    auto r = txn.exec_params(
        R"(
        SELECT c.id AS chat_id, LOWER(u.email) as email
        FROM chats c
        JOIN chat_participants cp1 ON c.id = cp1.chat_id
        JOIN chat_participants cp2 ON c.id = cp2.chat_id
        JOIN users u ON cp2.user_id = u.id
        WHERE cp1.user_id = $1
        AND cp2.user_id != $1
        AND c.type = 'private'
        )",
        userId
    );

    json arr = json::array();

    for(auto row : r)
    {
        arr.push_back({
            {"chat_id", row["chat_id"].as<int>()},
            {"email", row["email"].c_str()}
        });
    }

    return arr;
}

//--------------------------------------------------
json loadMessages(int chatId)
{
    PooledConnection pc;
    pqxx::work txn(pc.get());

    auto r = txn.exec_params(
        R"(
        SELECT m.content, LOWER(u.email) as email
        FROM messages m
        LEFT JOIN users u ON m.sender_id = u.id
        WHERE m.chat_id=$1
        ORDER BY m.created_at ASC
        )",
        chatId
    );

    json arr = json::array();

    for(auto row : r)
    {
        arr.push_back({
            {"from", row["email"].c_str()},
            {"text", row["content"].c_str()}
        });
    }

    return arr;
}

//--------------------------------------------------
void sendJson(Client &client, const json& j)
{
    std::string out = j.dump() + "\n";

    std::cout << "SEND TO CLIENT: " << out << std::endl;

    int res = SSL_write(client.ssl, out.c_str(), out.size());

if(res <= 0)
{
    int err = SSL_get_error(client.ssl, res);

    if(err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
    {
        return; // 🔥 не панікуємо
    }

    std::cout << "SSL WRITE ERROR: " << err << std::endl;
}
}

//--------------------------------------------------
void closeClient(int fd)
{
    if(clients.count(fd))
    {
        if(socketToEmail.count(fd))
        {
            onlineUsers.erase(socketToEmail[fd]);
            socketToEmail.erase(fd);
        }

        SSL_free(clients[fd].ssl);
        close(fd);
        clients.erase(fd);
    }
}

//--------------------------------------------------
int main()
{
    SSL_library_init();
    SSL_load_error_strings();

    ssl_ctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_use_certificate_file(ssl_ctx, "cert.pem", SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(ssl_ctx, "key.pem", SSL_FILETYPE_PEM);

    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(serverSocket, (sockaddr*)&addr, sizeof(addr));
    listen(serverSocket, SOMAXCONN);

    setNonBlocking(serverSocket);

    int epollFd = epoll_create1(0);

    epoll_event ev{}, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = serverSocket;

    epoll_ctl(epollFd, EPOLL_CTL_ADD, serverSocket, &ev);

    std::cout << "TLS Server started\n";

    while(true)
    {
        int count = epoll_wait(epollFd, events, MAX_EVENTS, -1);

        for(int i = 0; i < count; i++)
        {
            int fd = events[i].data.fd;

            if(fd == serverSocket)
            {
                int clientFd = accept(serverSocket, nullptr, nullptr);
                setNonBlocking(clientFd);

                SSL* ssl = SSL_new(ssl_ctx);
                SSL_set_fd(ssl, clientFd);

                clients[clientFd] = {clientFd, ssl};

                epoll_event cev{};
                cev.events = EPOLLIN | EPOLLOUT;
                cev.data.fd = clientFd;

                epoll_ctl(epollFd, EPOLL_CTL_ADD, clientFd, &cev);
            }
            else
            {
                Client &client = clients[fd];

                if(!client.handshakeDone)
{
    int res = SSL_accept(client.ssl);

    if(res == 1)
    {
        client.handshakeDone = true;
        std::cout << "TLS OK\n";
    }
    else
    {
        int err = SSL_get_error(client.ssl, res);

        if(err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        {
            continue;
        }

        std::cout << "SSL ACCEPT ERROR: " << err << std::endl;
        closeClient(fd);
        continue;
    }
}

                char buffer[1024];
                int bytes = SSL_read(client.ssl, buffer, sizeof(buffer));

if(bytes <= 0)
{
    int err = SSL_get_error(client.ssl, bytes);

    if(err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
    {
        continue; // 🔥 не закриваємо!
    }

    std::cout << "SSL READ ERROR: " << err << std::endl;

    closeClient(fd);
    continue;
}

                client.buffer.append(buffer, bytes);

                size_t pos;
                while((pos = client.buffer.find('\n')) != std::string::npos)
                {
                    std::cout << "RAW FROM CLIENT: "  << std::endl;
                    std::string msg = client.buffer.substr(0, pos);
                    client.buffer.erase(0, pos + 1);

                    json j = json::parse(msg);
                    std::string type = j["type"];
                    std::cout << "TYPE: " << type << std::endl;

                    //--------------------------------------------------
                    if(type == "login")
{
    std::string email = toLower(j["email"]);
    std::string password = j["password"];

    PooledConnection pc;

    json result = loginUser(pc.get(), email, password);

    if(result["status"] == "ok")
    {
        onlineUsers[email] = fd;
        socketToEmail[fd] = email;

        sendJson(client, result);
        sendJson(client, {
            {"type","chat_list"},
            {"chats", loadUserChats(email)}
        });
    }
    else
    {
        sendJson(client, result);
    }
}


else if(type == "register")
{
    std::string username = j["username"];
    std::string email = j["email"];
    std::string password = j["password"];

    PooledConnection pc;

    json result = registerUser(
        pc.get(),
        username,
        email,
        password
    );

    sendJson(client, result);
}
                    //--------------------------------------------------
                    else if(type == "add_user")
                    {
                        std::cout << "ADD_USER HANDLER ENTERED\n";
                        std::string email = toLower(j["email"]);

                        if(!socketToEmail.count(fd)) continue;

                        std::string myEmail = socketToEmail[fd];

                        // ✅ не можна додати себе
                        if(email == myEmail)
                        {
                            sendJson(client, {
                                {"type","add_user_result"},
                                {"status","error"},
                                {"message","Не можна додати себе"}
                            });
                            continue;
                        }

                        if(!userExists(email))
                        {
                            sendJson(client, {
                                {"type","add_user_result"},
                                {"status","error"},
                                {"message","User not found"}
                            });
                            continue;
                        }

                        int user1 = getUserId(myEmail);
                        int user2 = getUserId(email);

                        int chatId = getPrivateChat(user1, user2);
                        if(chatId == -1)
                            chatId = createPrivateChat(user1, user2);

                        sendJson(client, {
                            {"type","add_user_result"},
                            {"status","ok"},
                            {"email", email},
                            {"chat_id", chatId}
                        });

                        // ✅ повідомити другого користувача
                        if(onlineUsers.count(email))
                        {
                            int otherFd = onlineUsers[email];

                            sendJson(clients[otherFd], {
                                {"type","new_chat"},
                                {"email", myEmail},
                                {"chat_id", chatId}
                            });
                        }

                        // ✅ оновити список чатів
                        sendJson(client, {
                            {"type","chat_list"},
                            {"chats", loadUserChats(myEmail)}
                        });
                    }

                    //--------------------------------------------------
                    else if(type == "load_messages")
                    {
                        int chatId = j["chat_id"];

                        sendJson(client, {
                            {"type","chat_history"},
                            {"messages", loadMessages(chatId)}
                        });
                    }

                    //--------------------------------------------------
                    else if(type == "chat_message_v2")
                    {
                        int chatId = j["chat_id"];
                        std::string from = toLower(j["from"]);
                        std::string text = j["text"];

                        int senderId = getUserId(from);
                        if(senderId == -1) continue;

                        {
                            PooledConnection pc;
                            pqxx::work txn(pc.get());

                            txn.exec_params(
                                "INSERT INTO messages (chat_id, sender_id, content) VALUES ($1,$2,$3)",
                                chatId, senderId, text
                            );

                            txn.commit();
                        }

                        // ✅ тільки учасникам чату
                        PooledConnection pc;
                        pqxx::work txn(pc.get());

                        auto r = txn.exec_params(
                            "SELECT u.email FROM chat_participants cp "
                            "JOIN users u ON cp.user_id = u.id "
                            "WHERE cp.chat_id=$1",
                            chatId
                        );

                        for(auto row : r)
                        {
                            std::string email = toLower(row["email"].c_str());

                            if(onlineUsers.count(email))
                            {
                                int userFd = onlineUsers[email];

                                sendJson(clients[userFd], {
                                    {"type","chat_message"},
                                    {"from", from},
                                    {"text", text}
                                });
                            }
                        }
                    }
                }
            }
        }
    }
}