// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <memory>
#include <string>

#include <obsmxl/backend.hpp>

namespace obsmxl
{
/**
 * In-process stand-in for an MXL domain. Used by the unit tests and by
 * macOS development builds where the Linux-only shared memory transport is
 * not available. All flow state is kept in memory; reads and writes follow
 * the MXL ring buffer semantics (absolute indices, strict write ordering,
 * invalid grains for gaps, blocking reads with timeouts).
 */
class FakeDomain
{
public:
    explicit FakeDomain(std::string path);
    ~FakeDomain();

    [[nodiscard]] std::string const& path() const noexcept;
    [[nodiscard]] std::size_t flow_count() const;

    /** Deletes every flow; mimics a process restart on the writer side. */
    void clear();

    /** Domain history used for new flows (MXL default is 200ms). */
    void set_history_duration_ms(int milliseconds);
    [[nodiscard]] int history_duration_ms() const;

private:
    friend class FakeBackendImpl;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

std::shared_ptr<FakeDomain> make_fake_domain(std::string path = "/dev/shm/mxl");

std::unique_ptr<Backend> create_fake_backend(std::shared_ptr<FakeDomain> domain);
std::unique_ptr<Backend> create_fake_backend(std::string path = "/dev/shm/mxl");
} // namespace obsmxl
