// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <unistd.h>

#include <obsmxl/domain-options.hpp>
#include <obsmxl/json.hpp>

using namespace obsmxl;

namespace
{
class TempDomain
{
public:
    TempDomain()
    {
        path_ = std::filesystem::temp_directory_path() /
                ("obs-mxl-domain-" + std::to_string(::getpid()) + "-" + std::to_string(counter_++));
    }

    ~TempDomain()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    std::string str() const { return path_.string(); }

private:
    std::filesystem::path path_;
    static inline int counter_{0};
};
} // namespace

TEST_CASE("Domain history is written under the MXL option key")
{
    TempDomain domain;

    std::string error;
    CHECK(read_domain_history(domain.str(), 200) == 200);
    REQUIRE(write_domain_history(domain.str(), 400, &error) == Status::Ok);
    CHECK(error.empty());
    CHECK(read_domain_history(domain.str(), 200) == 400);

    std::ifstream stream{std::filesystem::path{domain.str()} / "options.json"};
    REQUIRE(stream.good());
    std::stringstream buffer;
    buffer << stream.rdbuf();

    auto const document = JsonValue::parse(buffer.str());
    REQUIRE(document.has_value());
    auto const* value = document->find(domain_history_option);
    REQUIRE(value != nullptr);
    CHECK(value->as_number() == 400'000'000.0);
}

TEST_CASE("Domain history validation rejects invalid values")
{
    TempDomain domain;
    std::string error;

    CHECK(write_domain_history(domain.str(), 10, &error) == Status::InvalidArgument);
    CHECK(write_domain_history("", 200, &error) == Status::InvalidArgument);
    CHECK(write_domain_history(domain.str(), 6000, &error) == Status::InvalidArgument);
}
