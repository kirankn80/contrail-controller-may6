/*
 * Copyright (c) 2014 Juniper Networks, Inc. All rights reserved.
 */

#ifndef vnsw_agent_oper_dhcp_options_h_
#define vnsw_agent_oper_dhcp_options_h_

#include <vnc_cfg_types.h>

namespace autogen {
    struct DhcpOptionType;
    struct RouteType;
}

// DHCP options coming from config
class OperDhcpOptions {
public:
    typedef std::vector<autogen::DhcpOptionType> DhcpOptionsList;
    typedef std::vector<autogen::RouteType> HostOptionsList;

    struct HostRoute {
        Ip4Address prefix_;
        uint32_t plen_;
        Ip4Address gw_;

        HostRoute() : prefix_(), plen_(0), gw_() {}
        bool operator<(const HostRoute &rhs) const {
            if (prefix_ != rhs.prefix_)
                return prefix_ < rhs.prefix_;
            if (plen_ != rhs.plen_)
                return plen_ < rhs.plen_;
            return gw_ < rhs.gw_;
        }
        bool operator==(const HostRoute &rhs) const {
            return prefix_ == rhs.prefix_ && plen_ == rhs.plen_ &&
                   gw_ == rhs.gw_;
        }
        std::string ToString() const { 
            char len[32];
            snprintf(len, sizeof(len), "%u", plen_);
            return prefix_.to_string() + "/" + std::string(len) +
                   " -> " + gw_.to_string();
        }
    };

    OperDhcpOptions() {}
    OperDhcpOptions(const OperDhcpOptions &options) {
        dhcp_options_ = options.dhcp_options_;
        host_routes_ = options.host_routes_;
    }
    virtual ~OperDhcpOptions() {}

    const DhcpOptionsList &dhcp_options() const { return dhcp_options_; }
    const std::vector<HostRoute> &host_routes() const { return host_routes_; }
    void set_options(const DhcpOptionsList &options) { dhcp_options_ = options; }
    void set_host_routes(const HostOptionsList &host_routes) {
        host_routes_.clear();
        update_host_routes(host_routes);
    }
    void update_host_routes(const HostOptionsList &host_routes) {
        host_routes_.clear();
        for (unsigned int i = 0; i < host_routes.size(); ++i) {
            HostRoute host_route;
            boost::system::error_code ec = Ip4PrefixParse(host_routes[i].prefix,
                                                          &host_route.prefix_,
                                                          (int *)&host_route.plen_);
            if (ec || host_route.plen_ > 32) {
                continue;
            }
            host_route.gw_ = Ip4Address::from_string(host_routes[i].next_hop, ec);
            if (ec) {
                host_route.gw_ = Ip4Address();
            }
            host_routes_.push_back(host_route);
        }
    }

    bool are_dhcp_options_set() const {
        return dhcp_options_.size() > 0;
    }
    bool are_host_routes_set() const {
        return host_routes_.size() > 0;
    }

private:
    DhcpOptionsList dhcp_options_;
    std::vector<HostRoute> host_routes_;
};

#endif // vnsw_agent_oper_dhcp_options_h_
