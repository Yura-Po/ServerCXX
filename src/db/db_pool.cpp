#include "db_pool.h"
#include <queue>
#include <mutex>
#include <condition_variable>

class ConnectionPool
{
private:
    std::queue<std::unique_ptr<pqxx::connection>> pool;
    std::mutex mtx;
    std::condition_variable cv;

public:
    ConnectionPool(int size)
    {
        for(int i = 0; i < size; i++)
        {
            pool.push(std::make_unique<pqxx::connection>(
                "postgresql://chat_user:13312222@127.0.0.1:5432/chat_db"
            ));
        }
    }

    std::unique_ptr<pqxx::connection> acquire()
    {
        std::unique_lock<std::mutex> lock(mtx);
        while(pool.empty()) cv.wait(lock);

        auto conn = std::move(pool.front());
        pool.pop();
        return conn;
    }

    void release(std::unique_ptr<pqxx::connection> conn)
    {
        std::lock_guard<std::mutex> lock(mtx);
        pool.push(std::move(conn));
        cv.notify_one();
    }
};

static ConnectionPool dbPool(10);

PooledConnection::PooledConnection()
{
    conn = dbPool.acquire();
}

PooledConnection::~PooledConnection()
{
    dbPool.release(std::move(conn));
}

pqxx::connection& PooledConnection::get()
{
    return *conn;
}