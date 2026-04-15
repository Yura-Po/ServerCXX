#include <iostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#define PORT 5000
#define MAX_EVENTS 100

//--------------------------------------------------
// Non-blocking socket
//--------------------------------------------------
int setNonBlocking(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

//--------------------------------------------------
// Global storage
//--------------------------------------------------

// socket -> buffered data
std::unordered_map<int, std::string> clientBuffers;

// email -> socket
std::unordered_map<std::string, int> onlineUsers;

// socket -> email
std::unordered_map<int, std::string> socketToEmail;

// email -> contacts
std::unordered_map<std::string,
    std::unordered_set<std::string>> contacts;

//--------------------------------------------------
// Safe send JSON
//--------------------------------------------------
void sendJson(int fd, const json& j)
{
    std::string out = j.dump() + "\n";

    send(fd,
         out.c_str(),
         out.size(),
         0);
}

//--------------------------------------------------
// Main
//--------------------------------------------------
int main()
{
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    if(serverSocket < 0)
    {
        std::cerr << "Socket creation failed\n";
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(serverSocket,
            (sockaddr*)&addr,
            sizeof(addr)) < 0)
    {
        std::cerr << "Bind failed\n";
        return 1;
    }

    if(listen(serverSocket, SOMAXCONN) < 0)
    {
        std::cerr << "Listen failed\n";
        return 1;
    }

    setNonBlocking(serverSocket);

    int epollFd = epoll_create1(0);

    epoll_event ev{}, events[MAX_EVENTS];

    ev.events = EPOLLIN;
    ev.data.fd = serverSocket;

    epoll_ctl(epollFd,
              EPOLL_CTL_ADD,
              serverSocket,
              &ev);

    std::cout << "Server started on port "
              << PORT << "...\n";

    //--------------------------------------------------
    // Event loop
    //--------------------------------------------------
    while(true)
    {
        int eventCount =
            epoll_wait(epollFd,
                       events,
                       MAX_EVENTS,
                       -1);

        for(int i = 0; i < eventCount; i++)
        {
            int fd = events[i].data.fd;

            //--------------------------------------------------
            // New client
            //--------------------------------------------------
            if(fd == serverSocket)
            {
                sockaddr_in client{};
                socklen_t size = sizeof(client);

                int clientSocket =
                    accept(serverSocket,
                           (sockaddr*)&client,
                           &size);

                if(clientSocket < 0)
                    continue;

                setNonBlocking(clientSocket);

                epoll_event clientEv{};
                clientEv.events = EPOLLIN;
                clientEv.data.fd = clientSocket;

                epoll_ctl(epollFd,
                          EPOLL_CTL_ADD,
                          clientSocket,
                          &clientEv);

                clientBuffers[clientSocket] = "";

                std::cout << "Client connected: "
                          << clientSocket
                          << "\n";
            }

            //--------------------------------------------------
            // Existing client data
            //--------------------------------------------------
            else
            {
                char buffer[1024];

                int bytes =
                    recv(fd,
                         buffer,
                         sizeof(buffer),
                         0);

                //--------------------------------------------------
                // Disconnect
                //--------------------------------------------------
                if(bytes <= 0)
                {
                    if(socketToEmail.count(fd))
                    {
                        std::string email =
                            socketToEmail[fd];

                        onlineUsers.erase(email);
                        socketToEmail.erase(fd);

                        std::cout
                            << email
                            << " disconnected\n";
                    }

                    close(fd);

                    clientBuffers.erase(fd);

                    continue;
                }

                //--------------------------------------------------
                // Append received data
                //--------------------------------------------------
                clientBuffers[fd].append(buffer, bytes);

                std::string &data =
                    clientBuffers[fd];

                size_t pos;

                //--------------------------------------------------
                // Process complete JSON messages
                //--------------------------------------------------
                while((pos = data.find('\n'))
                      != std::string::npos)
                {
                    std::string message =
                        data.substr(0, pos);

                    data.erase(0, pos + 1);

                    if(message.empty())
                        continue;

                    try
                    {
                        json j =
                            json::parse(message);

                        std::string type =
                            j["type"];

                        //--------------------------------------------------
                        // LOGIN
                        //--------------------------------------------------
                        if(type == "login")
                        {
                            std::string email =
                                j["email"];

                            onlineUsers[email] = fd;
                            socketToEmail[fd] = email;

                            json resp;
                            resp["type"] =
                                "login_result";
                            resp["status"] =
                                "ok";

                            sendJson(fd, resp);

                            std::cout
                                << email
                                << " logged in\n";
                        }

                        //--------------------------------------------------
                        // ADD USER
                        //--------------------------------------------------
                        else if(type == "add_user")
                        {
                            std::string requester =
                                socketToEmail[fd];

                            std::string targetEmail =
                                j["email"];

                            json resp;

                            if(onlineUsers.count(targetEmail))
                            {
                                contacts[requester]
                                    .insert(targetEmail);

                                contacts[targetEmail]
                                    .insert(requester);

                                //--------------------------------
                                // Notify requester
                                //--------------------------------
                                resp["type"] =
                                    "add_user_result";
                                resp["status"] =
                                    "ok";
                                resp["email"] =
                                    targetEmail;

                                sendJson(fd, resp);

                                //--------------------------------
                                // Notify target
                                //--------------------------------
                                json notify;
                                notify["type"] =
                                    "add_user_result";
                                notify["status"] =
                                    "ok";
                                notify["email"] =
                                    requester;

                                int targetFd =
                                    onlineUsers[targetEmail];

                                sendJson(targetFd,
                                         notify);

                                std::cout
                                    << requester
                                    << " added "
                                    << targetEmail
                                    << "\n";
                            }
                            else
                            {
                                resp["type"] =
                                    "add_user_result";
                                resp["status"] =
                                    "error";

                                sendJson(fd, resp);
                            }
                        }

                        //--------------------------------------------------
                        // CHAT MESSAGE
                        //--------------------------------------------------
                        else if(type == "chat_message")
                        {
                            std::string to =
                                j["to"];

                            if(onlineUsers.count(to))
                            {
                                int targetFd =
                                    onlineUsers[to];

                                sendJson(targetFd, j);

                                std::cout
                                    << j["from"]
                                    << " -> "
                                    << to
                                    << ": "
                                    << j["text"]
                                    << "\n";
                            }
                        }
                    }
                    catch(...)
                    {
                        std::cerr
                            << "Invalid JSON received\n";
                    }
                }
            }
        }
    }

    close(serverSocket);

    return 0;
}