#include "db/includes/mini_kv_store.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv)
{
    try
    {
        const auto directory = std::filesystem::absolute(argc > 1 ? argv[1] : "demo-data");
        std::filesystem::create_directories(directory);
        std::filesystem::current_path(directory);
        {
            mini_kv_store store;
            store.put("demo:hello", "world");
            if (store.get("demo:hello") != "world") throw std::runtime_error("put/get failed");
            store.put("demo:deleted", "temporary");
            store.erase("demo:deleted");
            if (!store.get("demo:deleted").empty()) throw std::runtime_error("erase failed");
            store.close();
        }
        {
            mini_kv_store reopened;
            if (reopened.get("demo:hello") != "world" || !reopened.get("demo:deleted").empty())
                throw std::runtime_error("reopen failed");
            reopened.close();
        }
        std::cout << "put/get/erase/reopen passed in " << directory.string() << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
