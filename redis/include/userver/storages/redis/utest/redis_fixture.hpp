#pragma once

/// @file userver/storages/redis/utest/redis_fixture.hpp
/// @brief @copybrief storages::redis::utest::RedisTest

#include <userver/utest/utest.hpp>

#include <userver/storages/redis/utest/redis_local.hpp>

USERVER_NAMESPACE_BEGIN

namespace storages::redis::utest {

/// @brief Valkey or Redis fixture
///
/// Provides access to redis clients by means
/// `storages::redis::utest::RedisLocal` class
///
/// see example:
/// @snippet samples/redis_service/unittests/redis_test.cpp Unit test
// NOLINTNEXTLINE(fuchsia-multiple-inheritance)
class RedisTest : public ::testing::Test, public RedisLocal {
public:
    RedisTest() { FlushDb(); }
};

}  // namespace storages::redis::utest

USERVER_NAMESPACE_END
