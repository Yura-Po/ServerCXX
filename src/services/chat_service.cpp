#include "chat_service.h"
#include "../db/db_pool.h"
#include "../utils/utils.h"

#include <unordered_map>

extern std::unordered_map<std::string, int> onlineUsers;

bool isUserOnline(const std::string& email)
{
    return onlineUsers.count(toLower(email));
}

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

int getUserId(const std::string& email)
{
    PooledConnection pc;

    pqxx::work txn(pc.get());

    auto r = txn.exec_params(
        "SELECT id FROM users WHERE LOWER(email)=LOWER($1)",
        email
    );

    if(r.empty())
        return -1;

    return r[0]["id"].as<int>();
}

int getPrivateChat(int user1, int user2)
{
    PooledConnection pc;

    pqxx::work txn(pc.get());

    auto r = txn.exec_params(
        R"(
        SELECT c.id
        FROM chats c
        JOIN chat_participants cp
            ON c.id = cp.chat_id
        WHERE c.type='private'
        AND cp.user_id IN ($1, $2)
        GROUP BY c.id
        HAVING COUNT(DISTINCT cp.user_id) = 2
        )",
        user1,
        user2
    );

    if(r.empty())
        return -1;

    return r[0]["id"].as<int>();
}

int createPrivateChat(int user1, int user2)
{
    PooledConnection pc;

    pqxx::work txn(pc.get());

    auto r = txn.exec_params(
        "INSERT INTO chats(type) VALUES('private') RETURNING id"
    );

    int chatId =
        r[0]["id"].as<int>();

    txn.exec_params(
        "INSERT INTO chat_participants(chat_id,user_id) VALUES($1,$2)",
        chatId,
        user1
    );

    txn.exec_params(
        "INSERT INTO chat_participants(chat_id,user_id) VALUES($1,$2)",
        chatId,
        user2
    );

    txn.commit();

    return chatId;
}

json loadUserChats(const std::string& email)
{
    int userId = getUserId(email);

    PooledConnection pc;

    pqxx::work txn(pc.get());

    auto r = txn.exec_params(
        R"(
        SELECT
            c.id AS chat_id,
            LOWER(u.email) AS email,
            u.username AS username

        FROM chats c

        JOIN chat_participants cp1
            ON c.id = cp1.chat_id

        JOIN chat_participants cp2
            ON c.id = cp2.chat_id

        JOIN users u
            ON cp2.user_id = u.id

        WHERE cp1.user_id = $1
        AND cp2.user_id != $1
        AND c.type='private'
        )",
        userId
    );

    json arr = json::array();

    for(auto row : r)
    {
        std::string otherEmail =
            row["email"].c_str();

        arr.push_back({
            {"chat_id", row["chat_id"].as<int>()},
            {"email", otherEmail},
            {"username", row["username"].c_str()},
            {"online", isUserOnline(otherEmail)}
        });
    }

    return arr;
}

json loadMessages(int chatId)
{
    PooledConnection pc;

    pqxx::work txn(pc.get());

    auto r = txn.exec_params(
        R"(
        SELECT
            m.content,

            LOWER(u.email) AS email,

            u.username AS username,

            TO_CHAR(
                m.created_at
                AT TIME ZONE 'UTC'
                AT TIME ZONE 'Europe/Kyiv',
                'HH24:MI'
            ) AS time,

            TO_CHAR(
                m.created_at
                AT TIME ZONE 'UTC'
                AT TIME ZONE 'Europe/Kyiv',
                'DD.MM.YYYY'
            ) AS date

        FROM messages m

        LEFT JOIN users u
            ON m.sender_id = u.id

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
            {"username", row["username"].c_str()},
            {"text", row["content"].c_str()},
            {"time", row["time"].c_str()},
            {"date", row["date"].c_str()}
        });
    }

    return arr;
}