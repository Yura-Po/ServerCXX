
#include "chat_service.h"
#include "../db/db_pool.h"
#include "../utils/utils.h"
#include "../auth.h"

#include <unordered_map>
#include <fstream>
#include <sys/stat.h>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <ctime>

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

    json arr = json::array();

    //--------------------------------------------------
    // Private chats
    //--------------------------------------------------
    auto r_priv = txn.exec_params(
        R"(
        SELECT
            c.id AS chat_id,
            LOWER(u.email) AS email,
            u.username AS username,
            COALESCE(u.avatar_url, '') AS avatar_url
        FROM chats c
        JOIN chat_participants cp1 ON c.id = cp1.chat_id
        JOIN chat_participants cp2 ON c.id = cp2.chat_id
        JOIN users u ON cp2.user_id = u.id
        WHERE cp1.user_id = $1
        AND cp2.user_id != $1
        AND c.type='private'
        )",
        userId
    );

    for (auto row : r_priv)
    {
        std::string otherEmail = row["email"].c_str();
        std::string avatarUrl  = row["avatar_url"].c_str();

        json entry = {
            {"chat_id",   row["chat_id"].as<int>()},
            {"chat_type", "private"},
            {"email",     otherEmail},
            {"username",  row["username"].c_str()},
            {"online",    isUserOnline(otherEmail)}
        };

        if (!avatarUrl.empty())
            entry["avatar_url"] = avatarUrl;

        arr.push_back(entry);
    }

    //--------------------------------------------------
    // Group chats
    //--------------------------------------------------
    auto r_grp = txn.exec_params(
        R"(
        SELECT
            c.id AS chat_id,
            COALESCE(c.group_name, 'Група') AS group_name,
            cp.role AS my_role
        FROM chats c
        JOIN chat_participants cp ON c.id = cp.chat_id
        WHERE cp.user_id = $1
        AND c.type = 'group'
        )",
        userId
    );

    for (auto row : r_grp)
    {
        int         chatId    = row["chat_id"].as<int>();
        std::string groupName = row["group_name"].c_str();
        bool        isAdmin   = (std::string(row["my_role"].c_str()) == "admin");

        json entry = {
            {"chat_id",    chatId},
            {"chat_type",  "group"},
            {"email",      "__grp__" + std::to_string(chatId)},
            {"username",   groupName},
            {"group_name", groupName},
            {"online",     false},
            {"is_admin",   isAdmin}
        };
        arr.push_back(entry);
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
            m.id          AS message_id,
            m.content,
            m.type        AS msg_type,
            m.is_edited,
            m.is_deleted,

            LOWER(u.email) AS email,

            u.username    AS username,

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
            ) AS date,

            f.file_name,
            f.file_path,
            f.file_size,
            f.file_type   AS file_mime

        FROM messages m

        LEFT JOIN users u
            ON m.sender_id = u.id

        LEFT JOIN files f
            ON m.id = f.message_id

        WHERE m.chat_id=$1

        ORDER BY m.created_at ASC
        )",
        chatId
    );

    json arr = json::array();

    for(auto row : r)
    {
        json msgJson = {
            {"message_id", row["message_id"].as<int>()},
            {"from",       row["email"].c_str()},
            {"username",   row["username"].c_str()},
            {"text",       row["content"].c_str()},
            {"time",       row["time"].c_str()},
            {"date",       row["date"].c_str()},
            {"is_edited",  row["is_edited"].as<bool>()},
            {"is_deleted", row["is_deleted"].as<bool>()},
            {"msg_type",   row["msg_type"].c_str()}
        };

        if (!row["file_name"].is_null())
        {
            msgJson["file_name"] = row["file_name"].c_str();
            msgJson["file_path"] = row["file_path"].c_str();
            msgJson["file_size"] = row["file_size"].as<int>();
            msgJson["file_mime"] = row["file_mime"].c_str();
        }

        arr.push_back(msgJson);
    }

    return arr;
}

//==================================================
// PROFILE FUNCTIONS
//==================================================

json getUserProfile(const std::string& email)
{
    PooledConnection pc;

    pqxx::work txn(pc.get());

    auto r = txn.exec_params(
        "SELECT LOWER(email) AS email, username, "
        "COALESCE(avatar_url,'') AS avatar_url "
        "FROM users WHERE LOWER(email)=LOWER($1)",
        email
    );

    if(r.empty())
        return {
            {"type",       "profile_data"},
            {"email",      ""},
            {"username",   ""},
            {"avatar_url", ""}
        };

    return {
        {"type",       "profile_data"},
        {"email",      r[0]["email"].c_str()},
        {"username",   r[0]["username"].c_str()},
        {"avatar_url", r[0]["avatar_url"].c_str()}
    };
}

bool updateUserProfile(const std::string& currentEmail,
                       const std::string& newEmail,
                       const std::string& newUsername,
                       std::string& outError)
{
    if (newEmail.empty() || newUsername.empty())
    {
        outError = "EMPTY_FIELDS";
        return false;
    }

    // Check uniqueness only when email actually changes
    if (toLower(newEmail) != toLower(currentEmail))
    {
        PooledConnection pcChk;

        pqxx::work txnChk(pcChk.get());

        auto chk = txnChk.exec_params(
            "SELECT id FROM users WHERE LOWER(email)=LOWER($1)",
            newEmail
        );

        if (!chk.empty())
        {
            outError = "EMAIL_EXISTS";
            return false;
        }
    }

    PooledConnection pc;

    pqxx::work txn(pc.get());

    txn.exec_params(
        "UPDATE users SET email=LOWER($1), username=$2 "
        "WHERE LOWER(email)=LOWER($3)",
        newEmail,
        newUsername,
        currentEmail
    );

    txn.commit();

    return true;
}

std::string uploadUserAvatar(const std::string& email,
                             const std::string& b64Data,
                             const std::string& origName)
{
    mkdir("uploads",         0755);
    mkdir("uploads/avatars", 0755);

    std::string decoded    = base64Decode(b64Data);
    std::string storedName = generateUniqueFilename(origName);
    std::string filePath   = "uploads/avatars/" + storedName;

    {
        std::ofstream ofs(filePath, std::ios::binary);
        if (!ofs.good())
            return "";
        ofs.write(decoded.data(), (std::streamsize)decoded.size());
    }

    PooledConnection pc;

    pqxx::work txn(pc.get());

    txn.exec_params(
        "UPDATE users SET avatar_url=$1 WHERE LOWER(email)=LOWER($2)",
        filePath,
        email
    );

    txn.commit();

    return filePath;
}

void deleteUserAvatar(const std::string& email)
{
    // Remove file from disk
    {
        PooledConnection pcGet;

        pqxx::work txnGet(pcGet.get());

        auto r = txnGet.exec_params(
            "SELECT avatar_url FROM users WHERE LOWER(email)=LOWER($1)",
            email
        );

        if (!r.empty() && !r[0]["avatar_url"].is_null())
        {
            std::string path = r[0]["avatar_url"].c_str();
            if (!path.empty())
                ::remove(path.c_str());
        }
    }

    PooledConnection pc;

    pqxx::work txn(pc.get());

    txn.exec_params(
        "UPDATE users SET avatar_url=NULL WHERE LOWER(email)=LOWER($1)",
        email
    );

    txn.commit();
}

std::string getAvatarPath(const std::string& email)
{
    PooledConnection pc;

    pqxx::work txn(pc.get());

    auto r = txn.exec_params(
        "SELECT COALESCE(avatar_url,'') AS avatar_url "
        "FROM users WHERE LOWER(email)=LOWER($1)",
        email
    );

    if (r.empty())
        return "";

    return r[0]["avatar_url"].c_str();
}

//==================================================
// GROUP CHATS
//==================================================

int createGroupChat(const std::string& creatorEmail,
                    const std::string& groupName,
                    const std::vector<std::string>& memberEmails)
{
    int creatorId = getUserId(creatorEmail);
    if (creatorId == -1) return -1;

    PooledConnection pc;

    pqxx::work txn(pc.get());

    auto r = txn.exec_params(
        "INSERT INTO chats(type, group_name, created_by) "
        "VALUES('group', $1, $2) RETURNING id",
        groupName,
        creatorId
    );

    int chatId = r[0]["id"].as<int>();

    // Creator as admin
    txn.exec_params(
        "INSERT INTO chat_participants(chat_id, user_id, role) "
        "VALUES($1, $2, 'admin')",
        chatId,
        creatorId
    );

    // Add members
    for (const auto& email : memberEmails)
    {
        if (toLower(email) == creatorEmail) continue;

        auto rUser = txn.exec_params(
            "SELECT id FROM users WHERE LOWER(email)=LOWER($1)", email);

        if (rUser.empty()) continue;

        int memberId = rUser[0]["id"].as<int>();

        txn.exec_params(
            "INSERT INTO chat_participants(chat_id, user_id) "
            "VALUES($1, $2) ON CONFLICT DO NOTHING",
            chatId,
            memberId
        );
    }

    txn.commit();
    return chatId;
}

//==================================================
// PASSWORD RESET
//==================================================

std::string generateResetCode()
{
    std::ostringstream oss;
    oss << std::setw(6) << std::setfill('0') << (std::rand() % 1000000);
    return oss.str();
}

bool saveResetCode(const std::string& email, const std::string& code)
{
    PooledConnection pc;

    pqxx::work txn(pc.get());

    auto r = txn.exec_params(
        "UPDATE users "
        "SET reset_code=$1, "
        "    reset_code_expiry=NOW() + INTERVAL '10 minutes' "
        "WHERE LOWER(email)=LOWER($2) "
        "RETURNING id",
        code,
        email
    );

    if (r.empty()) return false;

    txn.commit();
    return true;
}

bool verifyAndResetPassword(const std::string& email,
                            const std::string& code,
                            const std::string& newPassword)
{
    // Verify code and expiry
    {
        PooledConnection pcChk;

        pqxx::work txnChk(pcChk.get());

        auto r = txnChk.exec_params(
            "SELECT id FROM users "
            "WHERE LOWER(email)=LOWER($1) "
            "  AND reset_code=$2 "
            "  AND reset_code_expiry > NOW()",
            email,
            code
        );

        if (r.empty()) return false;
    }

    std::string newHash = hashPassword(newPassword);
    if (newHash.empty()) return false;

    PooledConnection pc;

    pqxx::work txn(pc.get());

    txn.exec_params(
        "UPDATE users "
        "SET password_hash=$1, "
        "    reset_code=NULL, "
        "    reset_code_expiry=NULL "
        "WHERE LOWER(email)=LOWER($2)",
        newHash,
        email
    );

    txn.commit();
    return true;
}
