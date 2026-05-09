#pragma once

#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

bool userExists(const std::string& email);

int getUserId(const std::string& email);

int getPrivateChat(int user1, int user2);

int createPrivateChat(int user1, int user2);

json loadUserChats(const std::string& email);

json loadMessages(int chatId);

bool isUserOnline(const std::string& email);