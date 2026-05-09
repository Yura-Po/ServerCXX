#pragma once
#include <pqxx/pqxx>
#include <memory>

class PooledConnection
{
public:
    PooledConnection();
    ~PooledConnection();

    pqxx::connection& get();

private:
    std::unique_ptr<pqxx::connection> conn;
};