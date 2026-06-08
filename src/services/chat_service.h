#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

bool userExists(const std::string& email);

int getUserId(const std::string& email);

int getPrivateChat(int user1, int user2);

int createPrivateChat(int user1, int user2);

json loadUserChats(const std::string& email);

json loadMessages(int chatId);

bool isUserOnline(const std::string& email);

// ---- Profile / avatar ----

json getUserProfile(const std::string& email);

bool updateUserProfile(const std::string& currentEmail,
                       const std::string& newEmail,
                       const std::string& newUsername,
                       std::string& outError);

std::string uploadUserAvatar(const std::string& email,
                             const std::string& b64Data,
                             const std::string& origName);

void deleteUserAvatar(const std::string& email);

std::string getAvatarPath(const std::string& email);

// ---- Group chats ----

int createGroupChat(const std::string& creatorEmail,
                    const std::string& groupName,
                    const std::vector<std::string>& memberEmails);

// ---- Password reset ----

std::string generateResetCode();

bool saveResetCode(const std::string& email,
                   const std::string& code);

bool verifyAndResetPassword(const std::string& email,
                            const std::string& code,
                            const std::string& newPassword);
