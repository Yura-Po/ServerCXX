#include "smtp_sender.h"
#include "utils.h"          // base64Encode

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <iostream>

// ── Configure these ────────────────────────────────────────────────────────
static const char* SMTP_HOST   = "smtp.gmail.com";
static const int   SMTP_PORT   = 465;
static const char* SMTP_USER   = "popatenkoyura2020@gmail.com";   // ← your Gmail
static const char* SMTP_PASS   = "rkfv dmhy nugm lfdz";    // ← App Password
static const char* FROM_EMAIL  = "popatenkoyura2020@gmail.com";
static const char* FROM_NAME   = "Chat App";
// ──────────────────────────────────────────────────────────────────────────

struct SmtpConn
{
    int      fd  = -1;
    SSL_CTX *ctx = nullptr;
    SSL     *ssl = nullptr;

    ~SmtpConn()
    {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        if (ctx) { SSL_CTX_free(ctx); }
        if (fd >= 0) ::close(fd);
    }
};

// Read one CRLF-terminated line from the SSL stream.
static bool smtpReadLine(SmtpConn& c, std::string& line)
{
    line.clear();
    char ch;
    while (true)
    {
        int n = SSL_read(c.ssl, &ch, 1);
        if (n <= 0) return false;
        line += ch;
        if (line.size() >= 2 &&
            line[line.size()-2] == '\r' &&
            line[line.size()-1] == '\n')
            break;
    }
    return true;
}

// Read all continuation lines until the last one (code + space + text).
static bool smtpExpect(SmtpConn& c, int expected)
{
    std::string line;
    while (true)
    {
        if (!smtpReadLine(c, line)) return false;
        if (line.size() < 4) return false;

        int got = std::stoi(line.substr(0, 3));
        if (got != expected)
        {
            std::cerr << "SMTP unexpected: " << line;
            return false;
        }
        if (line[3] == ' ') break;   // last line of multi-line response
    }
    return true;
}

static bool smtpSend(SmtpConn& c, const std::string& msg)
{
    int n = SSL_write(c.ssl, msg.data(), (int)msg.size());
    return n > 0;
}

bool sendResetCodeEmail(const std::string& toEmail,
                        const std::string& code)
{
    SmtpConn c;

    //--------------------------------------------------
    // Resolve host
    //--------------------------------------------------
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string portStr = std::to_string(SMTP_PORT);
    if (getaddrinfo(SMTP_HOST, portStr.c_str(), &hints, &res) != 0)
    {
        std::cerr << "SMTP: getaddrinfo failed\n";
        return false;
    }

    c.fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    bool connected = (c.fd >= 0) &&
                     (::connect(c.fd, res->ai_addr, res->ai_addrlen) == 0);
    freeaddrinfo(res);

    if (!connected)
    {
        std::cerr << "SMTP: TCP connect failed\n";
        return false;
    }

    //--------------------------------------------------
    // SSL handshake (SMTPS — SSL from the first byte)
    //--------------------------------------------------
    c.ctx = SSL_CTX_new(TLS_client_method());
    if (!c.ctx) return false;

    c.ssl = SSL_new(c.ctx);
    SSL_set_fd(c.ssl, c.fd);

    if (SSL_connect(c.ssl) <= 0)
    {
        ERR_print_errors_fp(stderr);
        std::cerr << "SMTP: SSL handshake failed\n";
        return false;
    }

    //--------------------------------------------------
    // SMTP protocol
    //--------------------------------------------------
    if (!smtpExpect(c, 220)) return false;   // greeting

    if (!smtpSend(c, "EHLO localhost\r\n"))   return false;
    if (!smtpExpect(c, 250))                  return false;

    if (!smtpSend(c, "AUTH LOGIN\r\n"))        return false;
    if (!smtpExpect(c, 334))                   return false;

    if (!smtpSend(c, base64Encode(SMTP_USER) + "\r\n")) return false;
    if (!smtpExpect(c, 334))                             return false;

    if (!smtpSend(c, base64Encode(SMTP_PASS) + "\r\n")) return false;
    if (!smtpExpect(c, 235))                             return false;   // authenticated

    std::string mailfrom = "MAIL FROM:<" + std::string(FROM_EMAIL) + ">\r\n";
    if (!smtpSend(c, mailfrom))  return false;
    if (!smtpExpect(c, 250))     return false;

    std::string rcptto = "RCPT TO:<" + toEmail + ">\r\n";
    if (!smtpSend(c, rcptto))   return false;
    if (!smtpExpect(c, 250))    return false;

    if (!smtpSend(c, "DATA\r\n")) return false;
    if (!smtpExpect(c, 354))      return false;

    //--------------------------------------------------
    // Email headers + body
    //--------------------------------------------------
    std::ostringstream mail;
    mail << "From: " << FROM_NAME << " <" << FROM_EMAIL << ">\r\n"
         << "To: <" << toEmail << ">\r\n"
         << "Subject: Chat App - Password Reset Code\r\n"
         << "MIME-Version: 1.0\r\n"
         << "Content-Type: text/plain; charset=UTF-8\r\n"
         << "\r\n"
         << "Your password reset code is: " << code << "\r\n"
         << "This code is valid for 10 minutes.\r\n"
         << "If you did not request a password reset, ignore this email.\r\n"
         << "\r\n"
         << ".\r\n";

    if (!smtpSend(c, mail.str())) return false;
    if (!smtpExpect(c, 250))      return false;

    smtpSend(c, "QUIT\r\n");
    return true;
}
