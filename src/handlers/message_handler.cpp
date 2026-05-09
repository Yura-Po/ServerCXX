#include "message_handler.h"
#include "../services/chat_service.h"
#include "../db/db_pool.h"
#include "../utils/utils.h"
#include "../auth.h"

#include <unordered_map>
#include <iostream>

extern std::unordered_map<std::string, int> onlineUsers;
extern std::unordered_map<int, std::string> socketToEmail;
extern std::unordered_map<int, Client> clients;

extern void sendJson(
    Client &client,
    const nlohmann::json& j);

extern void broadcastUserStatus(
    const std::string& email,
    bool online);

using json = nlohmann::json;

void handleMessage(Client& client,
                   const json& j,
                   int fd)
{
    std::string type = j["type"];

    std::cout << "TYPE: "
              << type
              << std::endl;

    //--------------------------------
    // LOGIN
    //--------------------------------
    if(type == "login")
    {
        std::string email =
            toLower(j["email"]);

        std::string password =
            j["password"];

        PooledConnection pc;

        json result =
            loginUser(
                pc.get(),
                email,
                password);

        if(result["status"] == "ok")
        {
            onlineUsers[email] = fd;
            socketToEmail[fd] = email;

            sendJson(client, result);

            //--------------------------------
            // SEND CHAT LIST
            //--------------------------------
            sendJson(client,
            {
                {"type","chat_list"},
                {"chats",
                 loadUserChats(email)}
            });

            //--------------------------------
            // BROADCAST ONLINE
            //--------------------------------
            broadcastUserStatus(
                email,
                true);
        }
        else
        {
            sendJson(client, result);
        }
    }

    //--------------------------------
    // REGISTER
    //--------------------------------
    else if(type == "register")
    {
        PooledConnection pc;

        json result =
            registerUser(
                pc.get(),
                j["username"],
                j["email"],
                j["password"]);

        sendJson(client, result);
    }

    //--------------------------------
    // LOAD MESSAGES
    //--------------------------------
    else if(type == "load_messages")
    {
        int chatId =
            j["chat_id"];

        sendJson(client,
        {
            {"type","chat_history"},
            {"messages",
             loadMessages(chatId)}
        });
    }

    //--------------------------------
    // CHAT MESSAGE
    //--------------------------------
    else if(type == "chat_message_v2")
    {
        int chatId =
            j["chat_id"];

        std::string from =
            toLower(j["from"]);

        std::string text =
            j["text"];

        int senderId =
            getUserId(from);

        if(senderId == -1)
            return;

        std::string username;

        {
            PooledConnection pc;

            pqxx::work txn(pc.get());

            auto r =
                txn.exec_params(
                    "SELECT username "
                    "FROM users "
                    "WHERE id=$1",
                    senderId);

            if(!r.empty())
            {
                username =
                    r[0]["username"]
                        .c_str();
            }
        }

        std::string messageTime;
        std::string messageDate;

        //--------------------------------
        // SAVE MESSAGE
        //--------------------------------
        {
            PooledConnection pc;

            pqxx::work txn(pc.get());

            auto result =
                txn.exec_params(
                    R"(
                    INSERT INTO messages
                    (chat_id,
                     sender_id,
                     content)

                    VALUES ($1,$2,$3)

                    RETURNING

                    TO_CHAR(
                        created_at
                        AT TIME ZONE 'UTC'
                        AT TIME ZONE 'Europe/Kyiv',
                        'HH24:MI'
                    ) as time,

                    TO_CHAR(
                        created_at
                        AT TIME ZONE 'UTC'
                        AT TIME ZONE 'Europe/Kyiv',
                        'DD.MM.YYYY'
                    ) as date
                    )",
                    chatId,
                    senderId,
                    text);

            messageTime =
                result[0]["time"]
                    .c_str();

            messageDate =
                result[0]["date"]
                    .c_str();

            txn.commit();
        }

        //--------------------------------
        // SEND TO USERS
        //--------------------------------
        PooledConnection pc;

        pqxx::work txn(pc.get());

        auto r =
            txn.exec_params(
                "SELECT u.email "
                "FROM chat_participants cp "
                "JOIN users u "
                "ON cp.user_id=u.id "
                "WHERE cp.chat_id=$1",
                chatId);

        for(auto row : r)
        {
            std::string email =
                toLower(
                    row["email"]
                        .c_str());

            if(onlineUsers.count(email))
{
    int userFd =
        onlineUsers[email];

    //--------------------------------
    // CLIENT EXISTS?
    //--------------------------------
    if(!clients.count(userFd))
    {
        onlineUsers.erase(email);
        continue;
    }

    //--------------------------------
    // VALID SSL?
    //--------------------------------
    if(clients[userFd].ssl == nullptr)
    {
        onlineUsers.erase(email);
        continue;
    }

                sendJson(
                    clients[userFd],
                {
                    {"type","chat_message"},
                    {"from", from},
                    {"username", username},
                    {"text", text},
                    {"time", messageTime},
                    {"date", messageDate}
                });
            }
        }
    }
}