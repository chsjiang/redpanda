/*
 * Copyright 2020 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#pragma once

#include "cluster/partition.h"
#include "kafka/requests/request_context.h"
#include "kafka/requests/response.h"
#include "likely.h"
#include "model/metadata.h"
#include "model/timeout_clock.h"
#include "seastarx.h"

#include <seastar/core/future.hh>

namespace kafka {

struct fetch_response;

struct fetch_api final {
    using response_type = fetch_response;

    static constexpr const char* name = "fetch";
    static constexpr api_key key = api_key(1);
    static constexpr api_version min_supported = api_version(4);
    static constexpr api_version max_supported = api_version(10);

    static ss::future<response_ptr>
    process(request_context&&, ss::smp_service_group);
};

struct fetch_request final {
    using api_type = fetch_api;

    struct partition {
        model::partition_id id;
        int32_t current_leader_epoch; // >= v9
        model::offset fetch_offset;
        // inter-broker data
        model::offset log_start_offset; // >= v5
        int32_t partition_max_bytes;
    };

    struct topic {
        model::topic name;
        std::vector<partition> partitions;
    };

    struct forgotten_topic {
        model::topic name;
        std::vector<int32_t> partitions;
        friend std::ostream& operator<<(std::ostream&, const forgotten_topic&);
    };

    model::node_id replica_id;
    std::chrono::milliseconds max_wait_time;
    int32_t min_bytes;
    int32_t max_bytes;      // >= v3
    int8_t isolation_level; // >= v4
    int32_t session_id;     // >= v7
    int32_t session_epoch;  // >= v7
    std::vector<topic> topics;
    std::vector<forgotten_topic> forgotten_topics; // >= v7

    void encode(response_writer& writer, api_version version);
    void decode(request_context& ctx);

    /*
     * For max_wait_time > 0 the request may be debounced in order to collect
     * additional data for the response. Otherwise, no such delay is requested.
     */
    std::optional<std::chrono::milliseconds> debounce_delay() const {
        if (max_wait_time <= std::chrono::milliseconds::zero()) {
            return std::nullopt;
        } else {
            return max_wait_time;
        }
    }

    /**
     * return empty if request doesn't contain any topics or all topics are
     * empty
     */
    bool empty() const {
        return topics.empty()
               || std::all_of(
                 topics.cbegin(), topics.cend(), [](const topic& t) {
                     return t.partitions.empty();
                 });
    }

    /*
     * iterator over request partitions. this adapter iterator is used because
     * the partitions are decoded off the wire directly into a hierarhical
     * representation:
     *
     *       [
     *         topic0 -> [...]
     *         topic1 -> [topic1-part1, topic1-part2, ...]
     *         ...
     *         topicN -> [...]
     *       ]
     *
     * the iterator value is a reference to the current topic and partition.
     */
    class const_iterator {
    public:
        using const_topic_iterator = std::vector<topic>::const_iterator;
        using const_partition_iterator = std::vector<partition>::const_iterator;

        struct value_type {
            bool new_topic;
            const_topic_iterator topic;
            const_partition_iterator partition;
        };

        using difference_type = void;
        using pointer = const value_type*;
        using reference = const value_type&;
        using iterator_category = std::forward_iterator_tag;

        const_iterator(const_topic_iterator begin, const_topic_iterator end)
          : state_({.new_topic = true, .topic = begin})
          , t_end_(end) {
            if (likely(state_.topic != t_end_)) {
                state_.partition = state_.topic->partitions.cbegin();
                normalize();
            }
        }

        reference operator*() const noexcept { return state_; }

        pointer operator->() const noexcept { return &state_; }

        const_iterator& operator++() {
            state_.partition++;
            state_.new_topic = false;
            normalize();
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const const_iterator& o) const noexcept {
            if (state_.topic == o.state_.topic) {
                if (state_.topic == t_end_) {
                    return true;
                } else {
                    return state_.partition == o.state_.partition;
                }
            }
            return false;
        }

        bool operator!=(const const_iterator& o) const noexcept {
            return !(*this == o);
        }

    private:
        void normalize() {
            while (state_.partition == state_.topic->partitions.cend()) {
                state_.topic++;
                state_.new_topic = true;
                if (state_.topic != t_end_) {
                    state_.partition = state_.topic->partitions.cbegin();
                } else {
                    break;
                }
            }
        }

        value_type state_;
        const_topic_iterator t_end_;
    };

    const_iterator cbegin() const {
        return const_iterator(topics.cbegin(), topics.cend());
    }

    const_iterator cend() const {
        return const_iterator(topics.cend(), topics.cend());
    }
};

std::ostream& operator<<(std::ostream&, const fetch_request&);

struct fetch_response final {
    using api_type = fetch_api;

    struct aborted_transaction {
        int64_t producer_id;
        model::offset first_offset;
    };

    struct partition_response {
        model::partition_id id;
        error_code error;
        model::offset high_watermark;
        model::offset last_stable_offset;                      // >= v4
        model::offset log_start_offset;                        // >= v5
        std::vector<aborted_transaction> aborted_transactions; // >= v4
        std::optional<iobuf> record_set;

        bool has_error() const { return error != error_code::none; }
    };

    struct partition {
        model::topic name;
        std::vector<partition_response> responses;

        partition(model::topic name)
          : name(std::move(name)) {}
    };

    fetch_response()
      : throttle_time(0) {}

    std::chrono::milliseconds throttle_time = std::chrono::milliseconds(
      0);               // >= v1
    error_code error;   // >= v7
    int32_t session_id; // >= v7
    std::vector<partition> partitions;

    void encode(const request_context& ctx, response& resp);
    void decode(iobuf buf, api_version version);

    friend std::ostream& operator<<(std::ostream&, const fetch_response&);

    /*
     * iterator over response partitions. this adapter iterator is used because
     * the partitions are encoded into the vector of partition responses
     *
     *       [
     *         partition0 -> [...]
     *         partition1 -> [partition_resp1, partition_resp2, ...]
     *         ...
     *         partitionN -> [...]
     *       ]
     *
     * the iterator value is a reference to the current partition and
     * partition_response.
     */
    class iterator {
    public:
        using partition_iterator = std::vector<partition>::iterator;
        using partition_response_iterator
          = std::vector<partition_response>::iterator;

        struct value_type {
            partition_iterator partition;
            partition_response_iterator partition_response;
        };

        using difference_type = void;
        using pointer = value_type*;
        using reference = value_type&;
        using iterator_category = std::forward_iterator_tag;

        iterator(partition_iterator begin, partition_iterator end)
          : state_({.partition = begin})
          , t_end_(end) {
            if (likely(state_.partition != t_end_)) {
                state_.partition_response = state_.partition->responses.begin();
                normalize();
            }
        }

        reference operator*() noexcept { return state_; }

        pointer operator->() noexcept { return &state_; }

        iterator& operator++() {
            state_.partition_response++;
            normalize();
            return *this;
        }
        iterator operator++(int) {
            iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const iterator& o) const noexcept {
            if (state_.partition == o.state_.partition) {
                if (state_.partition == t_end_) {
                    return true;
                } else {
                    return state_.partition_response
                           == o.state_.partition_response;
                }
            }
            return false;
        }

        bool operator!=(const iterator& o) const noexcept {
            return !(*this == o);
        }

    private:
        void normalize() {
            while (state_.partition_response
                   == state_.partition->responses.end()) {
                state_.partition++;
                if (state_.partition != t_end_) {
                    state_.partition_response
                      = state_.partition->responses.begin();
                } else {
                    break;
                }
            }
        }

        value_type state_;
        partition_iterator t_end_;
    };

    iterator begin() { return iterator(partitions.begin(), partitions.end()); }

    iterator end() { return iterator(partitions.end(), partitions.end()); }
};

std::ostream& operator<<(std::ostream&, const fetch_response&);

/*
 * Fetch operation context
 */
struct op_context {
    class response_iterator {
    public:
        using difference_type = void;
        using pointer = fetch_response::iterator::pointer;
        using reference = fetch_response::iterator::reference;
        using iterator_category = std::forward_iterator_tag;

        response_iterator(fetch_response::iterator, op_context* ctx);

        reference operator*() noexcept { return *_it; }

        pointer operator->() noexcept { return &(*_it); }

        response_iterator& operator++();

        const response_iterator operator++(int);

        bool operator==(const response_iterator& o) const noexcept;

        bool operator!=(const response_iterator& o) const noexcept;

        void set(fetch_response::partition_response&&);

    private:
        fetch_response::iterator _it;
        op_context* _ctx;
    };

    void reset_context();

    // decode request and initialize budgets
    op_context(request_context&& ctx, ss::smp_service_group ssg);

    // reserve space for a new topic in the response
    void start_response_topic(const fetch_request::topic& topic);

    // reserve space for new partition in the response
    void start_response_partition(const fetch_request::partition&);

    // create placeholder for response topics and partitions
    void create_response_placeholders();

    bool should_stop_fetch() const {
        return !request.debounce_delay() || over_min_bytes() || request.empty()
               || response_error || deadline <= model::timeout_clock::now();
    }

    bool over_min_bytes() const {
        return static_cast<int32_t>(response_size) >= request.min_bytes;
    }

    ss::future<response_ptr> send_response() &&;

    response_iterator response_begin() {
        return response_iterator(response.begin(), this);
    }

    response_iterator response_end() {
        return response_iterator(response.end(), this);
    }

    request_context rctx;
    ss::smp_service_group ssg;
    fetch_request request;
    fetch_response response;

    // operation budgets
    size_t bytes_left;
    std::optional<model::timeout_clock::time_point> deadline;

    // size of response
    size_t response_size;
    // does the response contain an error
    bool response_error;

    bool initial_fetch = true;
};

class partition_wrapper {
public:
    partition_wrapper(
      ss::lw_shared_ptr<cluster::partition> partition,
      std::optional<storage::log> log = std::nullopt)
      : _partition(partition)
      , _log(log) {}

    ss::future<model::record_batch_reader>
    make_reader(storage::log_reader_config config) {
        return _log ? _log->make_reader(config)
                    : _partition->make_reader(config);
    }

    cluster::partition_probe& probe() { return _partition->probe(); }

    model::offset high_watermark() const {
        return _log ? _log->offsets().dirty_offset
                    : _partition->high_watermark();
    }

    model::offset last_stable_offset() const {
        return _log ? _log->offsets().dirty_offset
                    : _partition->last_stable_offset();
    }

private:
    ss::lw_shared_ptr<cluster::partition> _partition;
    std::optional<storage::log> _log;
};

struct fetch_config {
    model::offset start_offset;
    size_t max_bytes;
    model::timeout_clock::time_point timeout;
    bool strict_max_bytes{false};
};
/**
 * Simple type aggregating either reader and offsets or an error
 */
struct read_result {
    explicit read_result(error_code e)
      : error(e) {}

    read_result(
      model::record_batch_reader rdr, model::offset hw, model::offset lso)
      : reader(std::move(rdr))
      , high_watermark(hw)
      , last_stable_offset(lso)
      , error(error_code::none) {}

    read_result(model::offset hw, model::offset lso)
      : high_watermark(hw)
      , last_stable_offset(lso)
      , error(error_code::none) {}

    std::optional<model::record_batch_reader> reader;
    model::offset high_watermark;
    model::offset last_stable_offset;
    error_code error;
};

ss::future<fetch_response::partition_response>
read_from_ntp(op_context& octx, model::ntp ntp, fetch_config config);

} // namespace kafka
