#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

int main()
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5000);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int sListen = socket(AF_INET, SOCK_STREAM, 0);
    if (sListen < 0) {
        std::cout << "Socket error\n";
        return 1;
    }

    if (bind(sListen, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cout << "Bind error\n";
        return 1;
    }

    listen(sListen, SOMAXCONN);

    std::cout << "Waiting for connection...\n";

    sockaddr_in client{};
    socklen_t clientSize = sizeof(client);

    int newConnection = accept(sListen, (sockaddr*)&client, &clientSize);

    if (newConnection < 0) {
        std::cout << "Accept error\n";
    }
    else {
        std::cout << "Client connected!\n";

        char buffer[2048]{};

        int bytesReceived = recv(newConnection, buffer, sizeof(buffer), 0);

        if (bytesReceived > 0)
        {
            std::string data(buffer, bytesReceived);

            std::cout << "Raw data:\n" << data << std::endl;

            try
            {
               
                json j = json::parse(data);

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
                    send(newConnection, responseStr.c_str(), responseStr.size(), 0);
                }
            }
            catch (const std::exception& e)
            {
                std::cout << "JSON parse error: " << e.what() << std::endl;
            }
        }
    }

    close(newConnection);
    close(sListen);

    return 0;
}