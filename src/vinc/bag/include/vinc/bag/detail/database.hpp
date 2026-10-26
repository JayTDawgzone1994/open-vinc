#pragma once
#include <vinc/bag/bag.hpp>
#include <span>
struct sqlite3;

namespace vinc::bag::detail {
struct Record {
    std::unique_ptr<BaseMessage> message;
    std::uint64_t retained_bytes = 0;
    std::uint64_t receive_ns = 0;
    Recorder::Encoder encode = nullptr;
};

class Database {
    sqlite3* db_ = nullptr;

public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Result<void> create(const std::filesystem::path&, const Options&);
    Result<void> begin();
    Result<void> topic(const Topic&);
    Result<void> append(const Record&, std::span<const std::byte>);
    Result<void> commit(const Counters&, bool completed);
    void rollback() noexcept;
    Result<void> close();
};
}
