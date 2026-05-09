#pragma once
#include <nlohmann/json.hpp>
#include "../models/client.h"

void handleMessage(Client& client,
                   const nlohmann::json& j,
                   int fd);
void broadcastUserStatus(const std::string& email,bool online);