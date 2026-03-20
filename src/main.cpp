#include <iostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <cstring>
#include <unordered_map>
#include <errno.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#define PORT 5000
#define MAX_EVENTS 100


int setNonBlocking(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}


std::unordered_map<int, std::string> clientBuffers;

int main()
{
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0)
    {
        std::cout << "Socket error\n";
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        std::cout << "Bind error\n";
        return 1;
    }

    listen(serverSocket, SOMAXCONN);
    setNonBlocking(serverSocket);

    int epollFd = epoll_create1(0);

    epoll_event ev{}, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = serverSocket;

    epoll_ctl(epollFd, EPOLL_CTL_ADD, serverSocket, &ev);

    std::cout << "Epoll server started...\n";

    while (true)
    {
        int eventCount = epoll_wait(epollFd, events, MAX_EVENTS, -1);

        for (int i = 0; i < eventCount; i++)
        {
            int fd = events[i].data.fd;

            
            if (fd == serverSocket)
            {
                while (true)
                {
                    sockaddr_in client{};
                    socklen_t clientSize = sizeof(client);

                    int clientSocket = accept(serverSocket, (sockaddr*)&client, &clientSize);

                    if (clientSocket < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        else
                        {
                            std::cout << "Accept error\n";
                            break;
                        }
                    }

                    setNonBlocking(clientSocket);

                    epoll_event clientEv{};
                    clientEv.events = EPOLLIN;
                    clientEv.data.fd = clientSocket;

                    epoll_ctl(epollFd, EPOLL_CTL_ADD, clientSocket, &clientEv);

                    clientBuffers[clientSocket] = "";

                    std::cout << "New client connected\n";
                }
            }
            else
            {
                char buffer[1024];

                while (true)
                {
                    int bytes = recv(fd, buffer, sizeof(buffer), 0);

                    if (bytes > 0)
                    {
                        clientBuffers[fd].append(buffer, bytes);
                    }
                    else if (bytes == 0)
                    {
                        std::cout << "Client disconnected\n";
                        close(fd);
                        clientBuffers.erase(fd);
                        break;
                    }
                    else
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            
                            break;
                        }
                        else
                        {
                            std::cout << "Recv error\n";
                            close(fd);
                            clientBuffers.erase(fd);
                            break;
                        }
                    }
                }

                // 🔥 обробка повідомлень
                std::string &data = clientBuffers[fd];

                size_t pos;
                while ((pos = data.find('\n')) != std::string::npos)
                {
                    std::string message = data.substr(0, pos);
                    data.erase(0, pos + 1);

                    std::cout << "\nFull message:\n" << message << std::endl;

                    try
                    {
                        json j = json::parse(message);

                        std::string type = j.value("type", "");

                        if (type == "login")
                        {
                            std::string email = j.value("email", "");
                            std::string password = j.value("password", "");

                            std::cout << "\n===== LOGIN DATA =====\n";
                            std::cout << "Email: " << email << "\n";
                            std::cout << "Password: " << password << "\n";
                            std::cout << "======================\n";

                            json response;
                            response["type"] = "login_result";
                            response["status"] = "ok";

                            std::string responseStr = response.dump() + "\n";
                            send(fd, responseStr.c_str(), responseStr.size(), 0);
                        }
                    }
                    catch (const std::exception& e)
                    {
                        std::cout << "JSON parse error: " << e.what() << std::endl;
                    }
                }
            }
        }
    }

    close(serverSocket);
    return 0;
}