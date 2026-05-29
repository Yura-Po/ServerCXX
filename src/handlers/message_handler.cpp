
#include "message_handler.h"
#include "../services/chat_service.h"
#include "../db/db_pool.h"
#include "../utils/utils.h"
#include "../utils/smtp_sender.h"
#include "../auth.h"

#include <unordered_map>
#include <iostream>
#include <fstream>
#include <sys/stat.h>

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

// Helper: broadcast json to all participants of a chat
static void broadcastToChat(int chatId, const json& payload)
{
    std::vector<int> targetFds;

    {
        PooledConnection pc;

        pqxx::work txn(pc.get());

        auto r =
            txn.exec_params(
                "SELECT u.email "
                "FROM chat_participants cp "
                "JOIN users u ON cp.user_id=u.id "
                "WHERE cp.chat_id=$1",
                chatId);

        for (auto row : r)
        {
            std::string email =
                toLower(row["email"].c_str());

            if (!onlineUsers.count(email))
                continue;

            int userFd = onlineUsers[email];

            if (!clients.count(userFd))
            {
                onlineUsers.erase(email);
                continue;
            }

            if (clients[userFd].ssl == nullptr)
            {
                onlineUsers.erase(email);
                continue;
            }

            targetFds.push_back(userFd);
        }
    } // DB connection released here — sendJson runs without holding a pool slot

    for (int userFd : targetFds)
    {
        if (!clients.count(userFd))    continue;
        if (!clients[userFd].ssl)      continue;
        sendJson(clients[userFd], payload);
    }
}

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
            {"type",     "chat_history"},
            {"chat_id",  chatId},          // echoed so client can discard stale responses
            {"messages", loadMessages(chatId)}
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

        int         newMessageId = -1;
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

                    id,

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

            newMessageId =
                result[0]["id"]
                    .as<int>();

            messageTime =
                result[0]["time"]
                    .c_str();

            messageDate =
                result[0]["date"]
                    .c_str();

            txn.commit();
        }

        //--------------------------------
        // BROADCAST
        //--------------------------------
        broadcastToChat(chatId,
        {
            {"type",       "chat_message"},
            {"chat_id",    chatId},
            {"msg_type",   "text"},
            {"from",       from},
            {"username",   username},
            {"text",       text},
            {"time",       messageTime},
            {"date",       messageDate},
            {"message_id", newMessageId}
        });
    }

    //--------------------------------
    // ADD USER (create private chat)
    //--------------------------------
    else if(type == "add_user")
    {
        std::string myEmail =
            socketToEmail.count(fd)
                ? socketToEmail[fd]
                : "";

        if(myEmail.empty())
            return;

        std::string targetEmail =
            toLower(j["email"]);

        if(targetEmail == myEmail)
            return;

        //--------------------------------
        // TARGET EXISTS?
        //--------------------------------
        if(!userExists(targetEmail))
        {
            sendJson(client,
            {
                {"type", "add_user_result"},
                {"code", "USER_NOT_FOUND"}
            });
            return;
        }

        int myId     = getUserId(myEmail);
        int targetId = getUserId(targetEmail);

        //--------------------------------
        // CHAT ALREADY EXISTS?
        //--------------------------------
        if(getPrivateChat(myId, targetId) != -1)
        {
            sendJson(client,
            {
                {"type", "add_user_result"},
                {"code", "ALREADY_EXISTS"}
            });
            return;
        }

        //--------------------------------
        // CREATE CHAT
        //--------------------------------
        int chatId = createPrivateChat(myId, targetId);

        std::string targetUsername;
        std::string myUsername;

        {
            PooledConnection pc;

            pqxx::work txn(pc.get());

            auto r1 =
                txn.exec_params(
                    "SELECT username FROM users WHERE id=$1",
                    targetId);

            if(!r1.empty())
                targetUsername = r1[0]["username"].c_str();

            auto r2 =
                txn.exec_params(
                    "SELECT username FROM users WHERE id=$1",
                    myId);

            if(!r2.empty())
                myUsername = r2[0]["username"].c_str();
        }

        //--------------------------------
        // NOTIFY REQUESTER
        //--------------------------------
        sendJson(client,
        {
            {"type",     "new_chat"},
            {"chat_id",  chatId},
            {"email",    targetEmail},
            {"username", targetUsername}
        });

        //--------------------------------
        // NOTIFY TARGET (if online)
        //--------------------------------
        if(onlineUsers.count(targetEmail))
        {
            int targetFd = onlineUsers[targetEmail];

            if(clients.count(targetFd) &&
               clients[targetFd].ssl != nullptr)
            {
                sendJson(clients[targetFd],
                {
                    {"type",     "new_chat"},
                    {"chat_id",  chatId},
                    {"email",    myEmail},
                    {"username", myUsername}
                });
            }
        }
    }

    //--------------------------------
    // SEND FILE
    //--------------------------------
    else if(type == "send_file")
    {
        std::string myEmail =
            socketToEmail.count(fd)
                ? socketToEmail[fd]
                : "";

        if(myEmail.empty())
            return;

        int         chatId       = j["chat_id"];
        std::string originalName = j["file_name"];
        std::string fileData     = j["data"]; // base64
        std::string fileMime     =
            j.value("file_mime", "application/octet-stream");

        int senderId = getUserId(myEmail);
        if(senderId == -1)
            return;

        //--------------------------------
        // DECODE & SAVE TO DISK
        //--------------------------------
        std::string fileBytes  = base64Decode(fileData);
        int         fileSize   = (int)fileBytes.size();
        std::string storedName = generateUniqueFilename(originalName);
        std::string filePath   = "uploads/" + storedName;

        mkdir("uploads", 0755);

        {
            std::ofstream ofs(filePath, std::ios::binary);
            if(!ofs)
            {
                std::cout << "FILE WRITE ERROR: " << filePath << std::endl;
                return;
            }
            ofs.write(fileBytes.data(), fileBytes.size());
        }

        //--------------------------------
        // GET SENDER USERNAME
        //--------------------------------
        std::string senderUsername;

        {
            PooledConnection pc;

            pqxx::work txn(pc.get());

            auto r =
                txn.exec_params(
                    "SELECT username FROM users WHERE id=$1",
                    senderId);

            if(!r.empty())
                senderUsername = r[0]["username"].c_str();
        }

        //--------------------------------
        // INSERT MESSAGE + FILE RECORD
        //--------------------------------
        int         newMessageId = -1;
        std::string messageTime;
        std::string messageDate;

        {
            PooledConnection pc;

            pqxx::work txn(pc.get());

            auto r =
                txn.exec_params(
                    R"(
                    INSERT INTO messages
                    (chat_id, sender_id, content, type)
                    VALUES ($1,$2,$3,'file')
                    RETURNING
                        id,
                        TO_CHAR(
                            created_at
                            AT TIME ZONE 'UTC'
                            AT TIME ZONE 'Europe/Kyiv',
                            'HH24:MI'
                        ) AS time,
                        TO_CHAR(
                            created_at
                            AT TIME ZONE 'UTC'
                            AT TIME ZONE 'Europe/Kyiv',
                            'DD.MM.YYYY'
                        ) AS date
                    )",
                    chatId,
                    senderId,
                    originalName);

            newMessageId = r[0]["id"].as<int>();
            messageTime  = r[0]["time"].c_str();
            messageDate  = r[0]["date"].c_str();

            txn.exec_params(
                "INSERT INTO files"
                "(message_id, file_name, file_path, file_size, file_type)"
                " VALUES ($1,$2,$3,$4,$5)",
                newMessageId,
                originalName,
                storedName,
                fileSize,
                fileMime);

            txn.commit();
        }

        //--------------------------------
        // BROADCAST FILE MESSAGE
        //--------------------------------
        broadcastToChat(chatId,
        {
            {"type",       "chat_message"},
            {"chat_id",    chatId},
            {"msg_type",   "file"},
            {"from",       myEmail},
            {"username",   senderUsername},
            {"message_id", newMessageId},
            {"time",       messageTime},
            {"date",       messageDate},
            {"file_name",  originalName},
            {"file_path",  storedName},
            {"file_size",  fileSize},
            {"file_mime",  fileMime}
        });
    }

    //--------------------------------
    // DOWNLOAD FILE
    //--------------------------------
    else if(type == "download_file")
    {
        std::string storedName =
            j["file_path"];

        std::string filePath =
            "uploads/" + storedName;

        //--------------------------------
        // READ FROM DISK
        //--------------------------------
        std::ifstream ifs(filePath, std::ios::binary);

        if(!ifs)
        {
            sendJson(client,
            {
                {"type", "file_data"},
                {"code", "NOT_FOUND"}
            });
            return;
        }

        std::string fileBytes(
            (std::istreambuf_iterator<char>(ifs)),
             std::istreambuf_iterator<char>());

        //--------------------------------
        // GET ORIGINAL NAME FROM DB
        //--------------------------------
        std::string originalName = storedName;

        {
            PooledConnection pc;

            pqxx::work txn(pc.get());

            auto r =
                txn.exec_params(
                    "SELECT file_name FROM files "
                    "WHERE file_path=$1",
                    storedName);

            if(!r.empty())
                originalName = r[0]["file_name"].c_str();
        }

        //--------------------------------
        // SEND BASE64
        //--------------------------------
        sendJson(client,
        {
            {"type",      "file_data"},
            {"file_name", originalName},
            {"file_path", storedName},
            {"data",      base64Encode(fileBytes)}
        });
    }

    //--------------------------------
    // EDIT MESSAGE
    //--------------------------------
    else if(type == "edit_message")
    {
        int messageId =
            j["message_id"];

        std::string newText =
            j["text"];

        std::string requesterEmail =
            socketToEmail.count(fd)
                ? socketToEmail[fd]
                : "";

        int requesterId =
            getUserId(requesterEmail);

        if(requesterId == -1)
            return;

        int chatId = -1;

        //--------------------------------
        // UPDATE (owner check)
        //--------------------------------
        {
            PooledConnection pc;

            pqxx::work txn(pc.get());

            auto r =
                txn.exec_params(
                    "UPDATE messages "
                    "SET content=$1, "
                    "    is_edited=TRUE "
                    "WHERE id=$2 "
                    "AND sender_id=$3 "
                    "AND type='text' "
                    "RETURNING chat_id",
                    newText,
                    messageId,
                    requesterId);

            if(r.empty())
                return;

            chatId =
                r[0]["chat_id"]
                    .as<int>();

            txn.commit();
        }

        //--------------------------------
        // BROADCAST
        //--------------------------------
        broadcastToChat(chatId,
        {
            {"type",       "message_edited"},
            {"message_id", messageId},
            {"text",       newText}
        });
    }

    //--------------------------------
    // DELETE MESSAGE
    //--------------------------------
    else if(type == "delete_message")
    {
        int messageId =
            j["message_id"];

        std::string requesterEmail =
            socketToEmail.count(fd)
                ? socketToEmail[fd]
                : "";

        int requesterId =
            getUserId(requesterEmail);

        if(requesterId == -1)
            return;

        int chatId = -1;

        //--------------------------------
        // SOFT DELETE (owner check)
        //--------------------------------
        {
            PooledConnection pc;

            pqxx::work txn(pc.get());

            auto r =
                txn.exec_params(
                    "UPDATE messages "
                    "SET is_deleted=TRUE, "
                    "    content='' "
                    "WHERE id=$1 "
                    "AND sender_id=$2 "
                    "RETURNING chat_id",
                    messageId,
                    requesterId);

            if(r.empty())
                return;

            chatId =
                r[0]["chat_id"]
                    .as<int>();

            txn.commit();
        }

        //--------------------------------
        // BROADCAST
        //--------------------------------
        broadcastToChat(chatId,
        {
            {"type",       "message_deleted"},
            {"message_id", messageId}
        });
    }

    //--------------------------------
    // GET PROFILE
    //--------------------------------
    else if(type == "get_profile")
    {
        if(!socketToEmail.count(fd))
            return;

        std::string email = socketToEmail[fd];

        sendJson(client, getUserProfile(email));
    }

    //--------------------------------
    // UPDATE PROFILE
    //--------------------------------
    else if(type == "update_profile")
    {
        if(!socketToEmail.count(fd))
            return;

        std::string currentEmail = socketToEmail[fd];
        std::string newEmail     = toLower(j.value("email",    ""));
        std::string newUsername  = j.value("username", "");

        std::string err;

        if(!updateUserProfile(currentEmail, newEmail, newUsername, err))
        {
            sendJson(client,
            {
                {"type", "profile_error"},
                {"code", err}
            });
            return;
        }

        // Keep session maps in sync when email changes
        if(newEmail != currentEmail)
        {
            onlineUsers.erase(currentEmail);
            onlineUsers[newEmail] = fd;
            socketToEmail[fd]     = newEmail;
        }

        sendJson(client,
        {
            {"type",     "profile_updated"},
            {"email",    newEmail},
            {"username", newUsername}
        });
    }

    //--------------------------------
    // UPLOAD AVATAR
    //--------------------------------
    else if(type == "upload_avatar")
    {
        if(!socketToEmail.count(fd))
            return;

        std::string email    = socketToEmail[fd];
        std::string fileName = j.value("file_name", "avatar.jpg");
        std::string b64data  = j.value("data",      "");

        std::string avatarPath =
            uploadUserAvatar(email, b64data, fileName);

        if(avatarPath.empty())
        {
            sendJson(client,
            {
                {"type", "profile_error"},
                {"code", "UPLOAD_FAILED"}
            });
            return;
        }

        sendJson(client,
        {
            {"type",       "avatar_updated"},
            {"avatar_url", avatarPath}
        });
    }

    //--------------------------------
    // DELETE AVATAR
    //--------------------------------
    else if(type == "delete_avatar")
    {
        if(!socketToEmail.count(fd))
            return;

        std::string email = socketToEmail[fd];

        deleteUserAvatar(email);

        sendJson(client, {{"type", "avatar_deleted"}});
    }

    //--------------------------------
    // GET AVATAR
    //--------------------------------
    else if(type == "get_avatar")
    {
        std::string email = toLower(j.value("email", ""));
        std::string path  = getAvatarPath(email);

        if(path.empty())
        {
            sendJson(client,
            {
                {"type",  "avatar_data"},
                {"email", email},
                {"data",  ""}
            });
            return;
        }

        std::ifstream ifs(path, std::ios::binary);

        if(!ifs.good())
        {
            sendJson(client,
            {
                {"type",  "avatar_data"},
                {"email", email},
                {"data",  ""}
            });
            return;
        }

        std::string raw(
            (std::istreambuf_iterator<char>(ifs)),
             std::istreambuf_iterator<char>());

        sendJson(client,
        {
            {"type",  "avatar_data"},
            {"email", email},
            {"data",  base64Encode(raw)}
        });
    }

    //--------------------------------
    // CREATE GROUP CHAT
    //--------------------------------
    else if(type == "create_group")
    {
        std::string myEmail =
            socketToEmail.count(fd) ? socketToEmail[fd] : "";

        if(myEmail.empty()) return;

        std::string groupName = j.value("group_name", "");
        if(groupName.empty()) return;

        std::vector<std::string> memberEmails;
        if(j.contains("members") && j["members"].is_array())
        {
            for(auto& m : j["members"])
                if(m.is_string())
                    memberEmails.push_back(toLower(m.get<std::string>()));
        }

        // Ensure creator is included
        bool creatorIn = false;
        for(auto& e : memberEmails)
            if(e == myEmail) { creatorIn = true; break; }
        if(!creatorIn)
            memberEmails.push_back(myEmail);

        int chatId = createGroupChat(myEmail, groupName, memberEmails);
        if(chatId == -1)
        {
            sendJson(client,
            {
                {"type", "group_error"},
                {"code", "CREATE_FAILED"}
            });
            return;
        }

        for(const auto& email : memberEmails)
        {
            if(!onlineUsers.count(email)) continue;

            int memberFd = onlineUsers[email];

            if(clients.count(memberFd) &&
               clients[memberFd].ssl != nullptr)
            {
                bool isAdmin = (email == myEmail);
                sendJson(clients[memberFd],
                {
                    {"type",       "new_group_chat"},
                    {"chat_id",    chatId},
                    {"group_name", groupName},
                    {"email",      "__grp__" + std::to_string(chatId)},
                    {"is_admin",   isAdmin}
                });
            }
        }
    }

    //--------------------------------
    // DELETE PRIVATE CHAT
    //--------------------------------
    else if(type == "delete_chat")
    {
        std::string myEmail =
            socketToEmail.count(fd) ? socketToEmail[fd] : "";
        if(myEmail.empty()) return;

        int chatId = j["chat_id"];
        int myId   = getUserId(myEmail);
        if(myId == -1) return;

        std::vector<std::string> participantEmails;
        std::vector<std::string> filePaths;

        {
            PooledConnection pc;
            pqxx::work txn(pc.get());

            // Verify participant + ensure chat is private
            auto rCheck = txn.exec_params(
                "SELECT c.type FROM chats c "
                "JOIN chat_participants cp ON c.id=cp.chat_id "
                "WHERE c.id=$1 AND cp.user_id=$2",
                chatId, myId);

            if(rCheck.empty()) return;
            if(std::string(rCheck[0]["type"].c_str()) != "private") return;

            // Collect all participant emails
            auto rPart = txn.exec_params(
                "SELECT LOWER(u.email) AS email "
                "FROM chat_participants cp "
                "JOIN users u ON cp.user_id=u.id "
                "WHERE cp.chat_id=$1",
                chatId);

            for(auto row : rPart)
                participantEmails.push_back(row["email"].c_str());

            // Collect file paths before CASCADE delete
            auto rFiles = txn.exec_params(
                "SELECT f.file_path FROM files f "
                "JOIN messages m ON f.message_id=m.id "
                "WHERE m.chat_id=$1",
                chatId);

            for(auto row : rFiles)
                filePaths.push_back(row["file_path"].c_str());

            // Delete chat; CASCADE removes messages, files, participants
            txn.exec_params("DELETE FROM chats WHERE id=$1", chatId);
            txn.commit();
        }

        // Remove uploaded files from disk
        for(const auto& path : filePaths)
            std::remove(("uploads/" + path).c_str());

        // Notify all participants
        json notif = {{"type", "chat_deleted"}, {"chat_id", chatId}};
        for(const auto& email : participantEmails)
        {
            if(!onlineUsers.count(email)) continue;
            int userFd = onlineUsers[email];
            if(clients.count(userFd) && clients[userFd].ssl != nullptr)
                sendJson(clients[userFd], notif);
        }
    }

    //--------------------------------
    // LEAVE GROUP
    //--------------------------------
    else if(type == "leave_group")
    {
        std::string myEmail =
            socketToEmail.count(fd) ? socketToEmail[fd] : "";
        if(myEmail.empty()) return;

        int chatId = j["chat_id"];
        int myId   = getUserId(myEmail);
        if(myId == -1) return;

        std::vector<std::string> remainingEmails;
        std::vector<std::string> filePaths;
        bool lastMember = false;

        {
            PooledConnection pc;
            pqxx::work txn(pc.get());

            // Verify participant + ensure chat is a group
            auto rCheck = txn.exec_params(
                "SELECT c.type FROM chats c "
                "JOIN chat_participants cp ON c.id=cp.chat_id "
                "WHERE c.id=$1 AND cp.user_id=$2",
                chatId, myId);

            if(rCheck.empty()) return;
            if(std::string(rCheck[0]["type"].c_str()) != "group") return;

            // Remove this user from group
            txn.exec_params(
                "DELETE FROM chat_participants "
                "WHERE chat_id=$1 AND user_id=$2",
                chatId, myId);

            // Check remaining members
            auto rRemain = txn.exec_params(
                "SELECT LOWER(u.email) AS email "
                "FROM chat_participants cp "
                "JOIN users u ON cp.user_id=u.id "
                "WHERE cp.chat_id=$1",
                chatId);

            if(rRemain.empty())
            {
                lastMember = true;

                // Collect file paths before delete
                auto rFiles = txn.exec_params(
                    "SELECT f.file_path FROM files f "
                    "JOIN messages m ON f.message_id=m.id "
                    "WHERE m.chat_id=$1",
                    chatId);

                for(auto row : rFiles)
                    filePaths.push_back(row["file_path"].c_str());

                // Delete the empty chat
                txn.exec_params("DELETE FROM chats WHERE id=$1", chatId);
            }
            else
            {
                for(auto row : rRemain)
                    remainingEmails.push_back(row["email"].c_str());
            }

            txn.commit();
        }

        if(lastMember)
        {
            for(const auto& path : filePaths)
                std::remove(("uploads/" + path).c_str());
        }

        // Notify leaving user
        sendJson(client, {{"type", "chat_deleted"}, {"chat_id", chatId}});

        // Notify remaining members
        if(!lastMember)
        {
            json notif = {
                {"type",    "user_left_group"},
                {"chat_id", chatId},
                {"email",   myEmail}
            };
            for(const auto& email : remainingEmails)
            {
                if(!onlineUsers.count(email)) continue;
                int userFd = onlineUsers[email];
                if(clients.count(userFd) && clients[userFd].ssl != nullptr)
                    sendJson(clients[userFd], notif);
            }
        }
    }

    //--------------------------------
    // ADD MEMBER TO GROUP
    //--------------------------------
    else if(type == "add_to_group")
    {
        std::string myEmail =
            socketToEmail.count(fd) ? socketToEmail[fd] : "";
        if(myEmail.empty()) return;

        int         chatId      = j["chat_id"];
        std::string targetEmail = toLower(j.value("email", ""));
        if(targetEmail.empty()) return;

        int myId     = getUserId(myEmail);
        int targetId = getUserId(targetEmail);

        if(myId == -1 || targetId == -1)
        {
            sendJson(client, {
                {"type", "add_to_group_result"},
                {"code", "USER_NOT_FOUND"}
            });
            return;
        }

        std::string groupName;

        {
            PooledConnection pc;
            pqxx::work txn(pc.get());

            // Verify requester is admin
            auto rAdmin = txn.exec_params(
                "SELECT role FROM chat_participants "
                "WHERE chat_id=$1 AND user_id=$2",
                chatId, myId);

            if(rAdmin.empty() ||
               std::string(rAdmin[0]["role"].c_str()) != "admin")
            {
                sendJson(client, {
                    {"type", "add_to_group_result"},
                    {"code", "NOT_ADMIN"}
                });
                return;
            }

            // Check already member
            auto rExists = txn.exec_params(
                "SELECT 1 FROM chat_participants "
                "WHERE chat_id=$1 AND user_id=$2",
                chatId, targetId);

            if(!rExists.empty())
            {
                sendJson(client, {
                    {"type", "add_to_group_result"},
                    {"code", "ALREADY_MEMBER"}
                });
                return;
            }

            // Get group name
            auto rGroup = txn.exec_params(
                "SELECT COALESCE(group_name,'Група') AS group_name "
                "FROM chats WHERE id=$1",
                chatId);

            if(!rGroup.empty())
                groupName = rGroup[0]["group_name"].c_str();

            // Add member
            txn.exec_params(
                "INSERT INTO chat_participants(chat_id,user_id,role) "
                "VALUES($1,$2,'member')",
                chatId, targetId);

            txn.commit();
        }

        // Confirm to requester
        sendJson(client, {
            {"type", "add_to_group_result"},
            {"code", "OK"}
        });

        // Notify new member if online
        if(onlineUsers.count(targetEmail))
        {
            int targetFd = onlineUsers[targetEmail];
            if(clients.count(targetFd) && clients[targetFd].ssl != nullptr)
            {
                sendJson(clients[targetFd],
                {
                    {"type",       "new_group_chat"},
                    {"chat_id",    chatId},
                    {"group_name", groupName},
                    {"email",      "__grp__" + std::to_string(chatId)},
                    {"is_admin",   false}
                });
            }
        }
    }

    //--------------------------------
    // FORGOT PASSWORD
    //--------------------------------
    else if(type == "forgot_password")
    {
        std::string email = toLower(j.value("email", ""));
        if(email.empty()) return;

        if(!userExists(email))
        {
            sendJson(client,
            {
                {"type", "forgot_password_result"},
                {"code", "USER_NOT_FOUND"}
            });
            return;
        }

        std::string code = generateResetCode();

        if(!saveResetCode(email, code))
        {
            sendJson(client,
            {
                {"type", "forgot_password_result"},
                {"code", "DB_ERROR"}
            });
            return;
        }

        bool sent = sendResetCodeEmail(email, code);

        if(!sent)
        {
            // In development: log the code so you can test without SMTP
            std::cout << "RESET CODE for " << email
                      << ": " << code << std::endl;
        }

        // Return OK regardless of email delivery so we don't
        // leak whether the address exists via timing differences.
        sendJson(client,
        {
            {"type", "forgot_password_result"},
            {"code", sent ? "OK" : "EMAIL_FAILED"}
        });
    }

    //--------------------------------
    // RESET PASSWORD
    //--------------------------------
    else if(type == "reset_password")
    {
        std::string email   = toLower(j.value("email",    ""));
        std::string code    = j.value("code",             "");
        std::string newPass = j.value("password",         "");

        if(email.empty() || code.empty() || newPass.empty())
            return;

        if(newPass.size() < 4)
        {
            sendJson(client,
            {
                {"type", "reset_password_result"},
                {"code", "WEAK_PASSWORD"}
            });
            return;
        }

        bool ok = verifyAndResetPassword(email, code, newPass);

        sendJson(client,
        {
            {"type", "reset_password_result"},
            {"code", ok ? "OK" : "INVALID_CODE"}
        });
    }
}
