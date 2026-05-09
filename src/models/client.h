#pragma once
#include <openssl/ssl.h>
#include <string>

struct Client
{
    int fd;
    SSL* ssl;
    bool handshakeDone = false;
    std::string buffer;
};