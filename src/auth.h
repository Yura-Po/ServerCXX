#ifndef AUTH_H
#define AUTH_H

#include <string>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

using json = nlohmann::json;

std::string hashPassword(const std::string& password);

bool verifyPassword(const std::string& password,
                    const std::string& hash);

json registerUser(pqxx::connection& conn,
                  const std::string& username,
                  const std::string& email,
                  const std::string& password);

json loginUser(pqxx::connection& conn,
               const std::string& email,
               const std::string& password);

#endif