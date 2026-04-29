#include "auth.h"
#include <argon2.h>
#include <algorithm>

std::string toLowerAuth(const std::string& s)
{
    std::string res = s;
    std::transform(res.begin(), res.end(), res.begin(), ::tolower);
    return res;
}

std::string hashPassword(const std::string& password)
{
    char hash[256];
    char salt[16] = "randomsalt12345";

    int result = argon2id_hash_encoded(
        2,
        1 << 16,
        1,
        password.c_str(),
        password.size(),
        salt,
        sizeof(salt),
        32,
        hash,
        sizeof(hash)
    );

    if(result == ARGON2_OK)
        return std::string(hash);

    return "";
}

bool verifyPassword(const std::string& password,
                    const std::string& hash)
{
    return argon2id_verify(
        hash.c_str(),
        password.c_str(),
        password.size()
    ) == ARGON2_OK;
}

json registerUser(pqxx::connection& conn,
                  const std::string& username,
                  const std::string& email,
                  const std::string& password)
{
    pqxx::work txn(conn);

    auto r = txn.exec_params(
        "SELECT id FROM users WHERE LOWER(email)=LOWER($1)",
        email
    );

    if(!r.empty())
    {
        return {
            {"type","register_result"},
            {"code","USER_EXISTS"}
        };
    }

    std::string hash = hashPassword(password);

    if(hash.empty())
    {
        return {
            {"type","register_result"},
            {"code","SERVER_ERROR"}
        };
    }

    txn.exec_params(
        "INSERT INTO users(username,email,password_hash) VALUES($1,$2,$3)",
        username,
        toLowerAuth(email),
        hash
    );

    txn.commit();

    return {
        {"type","register_result"},
        {"code","OK"}
    };
}

json loginUser(pqxx::connection& conn,
               const std::string& email,
               const std::string& password)
{
    pqxx::work txn(conn);

    auto r = txn.exec_params(
        "SELECT password_hash FROM users WHERE LOWER(email)=LOWER($1)",
        email
    );

    if(!r.empty() &&
       verifyPassword(password, r[0]["password_hash"].c_str()))
    {
        return {
            {"type","login_result"},
            {"status","ok"}
        };
    }

    return {
        {"type","login_result"},
        {"status","error"},
        {"message","Invalid email or password"}
    };
}