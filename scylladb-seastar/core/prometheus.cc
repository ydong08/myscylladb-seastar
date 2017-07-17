/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2016 ScyllaDB
 */

#include "prometheus.hh"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include "proto/metrics2.pb.h"
#include <sstream>

#include "scollectd_api.hh"
#include "scollectd-impl.hh"
#include "metrics_api.hh"
#include "http/function_handlers.hh"
#include <boost/algorithm/string/replace.hpp>
#include <boost/range/algorithm_ext/erase.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/combine.hpp>

namespace seastar {

namespace prometheus {
namespace pm = io::prometheus::client;

namespace mi = metrics::impl;

/**
 * Taken from an answer in stackoverflow:
 * http://stackoverflow.com/questions/2340730/are-there-c-equivalents-for-the-protocol-buffers-delimited-i-o-functions-in-ja
 */
static bool write_delimited_to(const google::protobuf::MessageLite& message,
        google::protobuf::io::ZeroCopyOutputStream* rawOutput) {
    google::protobuf::io::CodedOutputStream output(rawOutput);

    const int size = message.ByteSize();
    output.WriteVarint32(size);

    uint8_t* buffer = output.GetDirectBufferForNBytesAndAdvance(size);
    if (buffer != nullptr) {
        message.SerializeWithCachedSizesToArray(buffer);
    } else {
        message.SerializeWithCachedSizes(&output);
        if (output.HadError()) {
            return false;
        }
    }

    return true;
}

static pm::Metric* add_label(pm::Metric* mt, const metrics::impl::metric_id & id, const config& ctx) {
    mt->mutable_label()->Reserve(id.labels().size() + 1);
    auto label = mt->add_label();
    label->set_name("instance");
    label->set_value(ctx.hostname);
    for (auto &&i : id.labels()) {
        label = mt->add_label();
        label->set_name(i.first);
        label->set_value(i.second);
    }
    return mt;
}

static void fill_metric(pm::MetricFamily& mf, const metrics::impl::metric_value& c,
        const metrics::impl::metric_id & id, const config& ctx) {
    switch (c.type()) {
    case scollectd::data_type::DERIVE:
        add_label(mf.add_metric(), id, ctx)->mutable_counter()->set_value(c.i());
        mf.set_type(pm::MetricType::COUNTER);
        break;
    case scollectd::data_type::GAUGE:
        add_label(mf.add_metric(), id, ctx)->mutable_gauge()->set_value(c.d());
        mf.set_type(pm::MetricType::GAUGE);
        break;
    case scollectd::data_type::HISTOGRAM:
    {
        auto h = c.get_histogram();
        auto mh = add_label(mf.add_metric(), id,ctx)->mutable_histogram();
        mh->set_sample_count(h.sample_count);
        mh->set_sample_sum(h.sample_sum);
        for (auto b : h.buckets) {
            auto bc = mh->add_bucket();
            bc->set_cumulative_count(b.count);
            bc->set_upper_bound(b.upper_bound);
        }
        mf.set_type(pm::MetricType::HISTOGRAM);
        break;
    }
    default:
        add_label(mf.add_metric(), id, ctx)->mutable_counter()->set_value(c.ui());
        mf.set_type(pm::MetricType::COUNTER);
        break;
    }
}

using metrics_families_per_shard = std::vector<foreign_ptr<mi::values_reference>>;

static future<> get_map_value(metrics_families_per_shard& vec) {
    vec.resize(smp::count);
    return parallel_for_each(boost::irange(0u, smp::count), [&vec] (auto cpu) {
        return smp::submit_to(cpu, [] {
            return mi::get_values();
        }).then([&vec, cpu] (auto res) {
            vec[cpu] = std::move(res);
        });
    });
}

static std::string to_str(seastar::metrics::impl::data_type dt) {
    switch (dt) {
    case seastar::metrics::impl::data_type::GAUGE:
        return "gauge";
    case seastar::metrics::impl::data_type::COUNTER:
        return "counter";
    case seastar::metrics::impl::data_type::HISTOGRAM:
        return "histogram";
    case seastar::metrics::impl::data_type::DERIVE:
        // Prometheus server does not respect derive parameters
        // So we report them as counter
        return "counter";
    default:
        break;
    }
    return "untyped";
}

static std::string to_str(const seastar::metrics::impl::metric_value& v) {
    switch (v.type()) {
    case seastar::metrics::impl::data_type::GAUGE:
        return std::to_string(v.d());
    case seastar::metrics::impl::data_type::COUNTER:
        return std::to_string(v.i());
    case seastar::metrics::impl::data_type::DERIVE:
        return std::to_string(v.ui());
    default:
        break;
    }
    return ""; // we should never get here but it makes the compiler happy
}

static void add_name(std::ostream& s, const sstring& name, const std::map<sstring, sstring>& labels, const config& ctx) {
    s << name << "{instance=\"" << ctx.hostname << '"';
    if (!labels.empty()) {
        for (auto l : labels) {
            s << "," << l.first  << "=\"" << l.second << '"';
        }
    }
    s << "} ";

}

std::map<sstring, std::tuple<uint32_t, const mi::metric_family_info*>> get_metric_per_family(const metrics_families_per_shard& families) {
    std::map<sstring, std::tuple<uint32_t, const mi::metric_family_info*>> res;
    for (auto&& shard : families) {
        for (auto&& m : *(shard->metadata.get())) {
            std::get<uint32_t>(res[m.mf.name]) += m.metrics.size();
            std::get<const mi::metric_family_info*>(res[m.mf.name]) = &m.mf;
        }
    }
    return res;
}

/*!
 * \brief iterator for metric family
 *
 * In prometheus, a single shard collecct all the data from the other
 * shards and report it.
 *
 * Each shard returns a value_copy struct that has a vector of vector values (a vector per metric family)
 * and a vector of metadata (and insdie it a vector of metric metadata)
 *
 * The metrics are sorted by the metric family name.
 *
 * In prometheus, all the metrics that belongs to the same metric family are reported together.
 *
 * For efficiency the results from the metrics layer are kept in a vector.
 *
 * So we have a vector of shards of a vector of metric families of a vector of values.
 *
 * To produce the result, we use the metric_family_iterator that is created by metric_family_range.
 *
 * When iterating over the metrics we use two helper structure.
 *
 * 1. A map between metric family name and the total number of values (combine on all shards) and
 *    pointer to the metric family metadata.
 * 2. A vector of positions to the current metric family for each shard.
 *
 * The metric_family_range returns a metric_family_iterator that goes over all the families.
 *
 * The iterator returns a metric_family object, that can report the metric_family name, the size (how many
 * metrics in total belongs to the metric family) and a a foreach_metric method.
 *
 * The foreach_metric method can be used to perform an action on each of the metric that belongs to
 * that metric family
 *
 * Iterating over the metrics is done:
 * - go over each of the shard and each of the entry in the position vector:
 *   - if the current family (the metric family that we get from the shard and position) has the current name:
 *     - iterate over each of the metrics belong to that metric family:
 *
 * for example, if m is a metric_family_range
 *
 * for (auto&& i : m) {
 *   std::cout << i.name() << std::endl;
 *   i.foreach_metric([](const mi::metric_value& value, const mi::metric_info& value_info) {
 *     std::cout << value_info.id.labels().size() <<std::cout;
 *   });
 * }
 *
 * Will print all the metric family names followed by the number of labels each metric has.
 */
class metric_family_iterator;


/*!
 * \brief holds the information related to a metric_family
 *
 * The metric family is the object that return by the metric_family iterator
 *
 */
class metric_family {
    const metrics_families_per_shard& _families;
    std::vector<size_t> _positions;
    std::map<sstring, std::tuple<uint32_t, const mi::metric_family_info*>>::const_iterator _current_family;
public:
    metric_family() = delete;
    metric_family(const metrics_families_per_shard& families,
            const std::map<sstring, std::tuple<uint32_t, const mi::metric_family_info*>>::const_iterator& current_family,
            unsigned shards)
        : _families(families), _positions(shards, 0), _current_family(current_family) {
    }
    const sstring name() const {
        return _current_family->first;
    }

    const uint32_t size() const {
        return std::get<uint32_t>(_current_family->second);
    }

    const mi::metric_family_info& metadata() const {
        return *(std::get<const mi::metric_family_info*>(_current_family->second));
    }

    void foreach_metric(std::function<void(const mi::metric_value&, const mi::metric_info&)> f) {
        // iterating over the shard vector and the position vector
        for (auto&& i : boost::combine(_positions, _families)) {
            auto& pos_in_metric_per_shared = boost::get<0>(i);
            auto& metric_family = boost::get<1>(i);
            if (pos_in_metric_per_shared >= metric_family->metadata->size()) {
                // no more metric family in this shard
                continue;
            }
            auto& metadata = metric_family->metadata->at(pos_in_metric_per_shared);
            // the the name is different, that means that on this shard, the metric family
            // does not exist, because everything is sorted by metric family name, this is fine.
            if (metadata.mf.name == name()) {
                const mi::value_vector& values = metric_family->values[pos_in_metric_per_shared];
                const mi::metric_metadata_vector& metrics_metadata = metadata.metrics;
                for (auto&& vm : boost::combine(values, metrics_metadata)) {
                    auto& value = boost::get<0>(vm);
                    auto& metric_metadata = boost::get<1>(vm);
                    f(value, metric_metadata);
                }
                pos_in_metric_per_shared++;
            }
        }
    }

    friend class metric_family_iterator;
};

class metric_family_iterator {
    metric_family _family;
public:
    metric_family_iterator() = delete;
    metric_family_iterator(const metrics_families_per_shard& families,
            const std::map<sstring, std::tuple<uint32_t, const mi::metric_family_info*>>::const_iterator& current_family,
            unsigned shards)
        : _family(families, current_family, shards) {
    }

    metric_family_iterator& operator++() {
        _family._current_family++;
        return *this;
    }

    bool operator!=(const metric_family_iterator& o) const {
        return _family._current_family != o._family._current_family;
    }

    metric_family& operator*() {
        return _family;
    }

    metric_family* operator->() {
        return &_family;
    }
};

class metric_family_range {
    const metrics_families_per_shard& _families;
    std::map<sstring, std::tuple<uint32_t, const mi::metric_family_info*>> _metric_families_info;
public:
    metric_family_range(const metrics_families_per_shard& families) : _families(families),
    _metric_families_info(get_metric_per_family(families)) {

    }
    metric_family_iterator begin () const {
        return metric_family_iterator(_families, _metric_families_info.begin(), smp::count);
    }


    metric_family_iterator end() const {
        return metric_family_iterator(_families, _metric_families_info.end(), 0);
    }
};

std::string get_text_representation(const metrics_families_per_shard& families, const config& ctx) {
    std::stringstream s;
    bool found;
    metric_family_range m(families);
    for (auto&& metric_family : m) {
        auto name = ctx.prefix + "_" + metric_family.name();
        found = false;
        metric_family.foreach_metric([&s, &ctx, &found, &name, &metric_family](auto value, auto value_info) {
            if (!found) {
                if (metric_family.metadata().d.str() != "") {
                    s << "# HELP " << name << " " <<  metric_family.metadata().d.str() << "\n";
                }
                s << "# TYPE " << name << " " << to_str(metric_family.metadata().type) << "\n";
                found = true;
            }
            if (value.type() == mi::data_type::HISTOGRAM) {
                auto&& h = value.get_histogram();
                std::map<sstring, sstring> labels = value_info.id.labels();
                auto& le = labels["le"];
                uint64_t count = 0;
                auto bucket = name + "_bucket";
                for (auto  i : h.buckets) {
                     le = std::to_string(i.upper_bound);
                    count += i.count;
                    add_name(s, bucket, labels, ctx);
                    s << count;
                    s << "\n";
                }
                labels["le"] = "+Inf";
                add_name(s, bucket, labels, ctx);
                s << h.sample_count;
                s << "\n";

                add_name(s, name + "_sum", {}, ctx);
                s << h.sample_sum;
                s << "\n";
                add_name(s, name + "_count", {}, ctx);
                s << h.sample_count;
                s << "\n";

            } else {
                add_name(s, name, value_info.id.labels(), ctx);
                s << to_str(value);
                s << "\n";
            }
        });
    }
    return s.str();
}


std::string get_protobuf_representation(const metrics_families_per_shard& families, const config& ctx) {
    std::string s;
    google::protobuf::io::StringOutputStream os(&s);
    metric_family_range m(families);
    for (auto&& metric_family : m) {
        auto& name = metric_family.name();
        pm::MetricFamily mtf;

        mtf.set_name(ctx.prefix + "_" + name);
        mtf.mutable_metric()->Reserve(metric_family.size());
        metric_family.foreach_metric([&mtf, &ctx](auto value, auto value_info) {
            fill_metric(mtf, value, value_info.id, ctx);
        });
        if (!write_delimited_to(mtf, &os)) {
            seastar_logger.warn("Failed to write protobuf metrics");
        }
    }
    return s;
}

bool is_accept_text(const std::string& accept) {
    std::vector<std::string> strs;
    boost::split(strs, accept, boost::is_any_of(","));
    for (auto i : strs) {
        boost::trim(i);
        if (boost::starts_with(i, "application/vnd.google.protobuf;")) {
            return false;
        }
    }
    return true;
}

class metrics_handler : public handler_base  {
    sstring _prefix;
    config _ctx;

public:
    metrics_handler(config ctx) : _ctx(ctx) {}

    future<std::unique_ptr<httpd::reply>> handle(const sstring& path,
        std::unique_ptr<httpd::request> req, std::unique_ptr<httpd::reply> rep) override {
        return do_with(metrics_families_per_shard(), [rep = std::move(rep), this, req=std::move(req)] (auto& families) mutable {
            return get_map_value(families).then([&families, rep = std::move(rep), this, req=std::move(req)] () mutable {
                auto text = is_accept_text(req->get_header("Accept"));
                std::string s = (text) ? get_text_representation(families, _ctx) :
                        get_protobuf_representation(families, _ctx);
                rep->_content = std::move(s);
                rep->set_content_type((text) ? "txt" : "proto");
                return make_ready_future<std::unique_ptr<reply>>(std::move(rep));
            });
        });
    }
};


future<> start(httpd::http_server_control& http_server, config ctx) {
    if (ctx.hostname == "") {
        ctx.hostname = metrics::impl::get_local_impl()->get_config().hostname;
    }

    return http_server.set_routes([ctx](httpd::routes& r) {
        r.put(GET, "/metrics", new metrics_handler(ctx));
    });
}

}
}
