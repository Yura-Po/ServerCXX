#pragma once
#include <string>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cstdlib>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

inline std::string toLower(const std::string& s)
{
    std::string res = s;
    std::transform(res.begin(), res.end(), res.begin(), ::tolower);
    return res;
}

inline std::string base64Encode(const std::string& input)
{
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, input.data(), (int)input.size());
    BIO_flush(b64);
    BUF_MEM *bptr = nullptr;
    BIO_get_mem_ptr(mem, &bptr);
    std::string result(bptr->data, bptr->length);
    BIO_free_all(b64);
    return result;
}

inline std::string base64Decode(const std::string& encoded)
{
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new_mem_buf(encoded.data(), (int)encoded.size());
    BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    std::string result(encoded.size(), '\0');
    int len = BIO_read(b64, &result[0], (int)result.size());
    BIO_free_all(b64);
    result.resize(len > 0 ? len : 0);
    return result;
}

inline std::string generateUniqueFilename(const std::string& originalName)
{
    size_t dotPos = originalName.rfind('.');
    std::string ext = (dotPos != std::string::npos)
        ? originalName.substr(dotPos)
        : "";

    for (char& c : ext)
        c = std::tolower((unsigned char)c);

    std::time_t now = std::time(nullptr);
    unsigned int r  = (unsigned int)std::rand();

    std::ostringstream oss;
    oss << now << "_"
        << std::hex << std::setw(8) << std::setfill('0') << r
        << ext;

    return oss.str();
}
