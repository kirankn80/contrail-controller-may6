/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "oper/interface_common.h"
#include "services/dns_proto.h"
#include "bind/bind_resolver.h"
#include "cmn/agent_cmn.h"
#include "ifmap/ifmap_link.h"
#include "ifmap/ifmap_table.h"

void DnsProto::Init() {
}

void DnsProto::Shutdown() {
    BindResolver::Shutdown();
}

void DnsProto::ConfigInit() {
    std::vector<std::string> dns_servers;
    for (int i = 0; i < MAX_XMPP_SERVERS; i++) {
        std::string server = agent()->GetDnsXmppServer(i);    
        if (server != "")
            dns_servers.push_back(server);
    }
    BindResolver::Init(*agent()->GetEventManager()->io_service(), dns_servers,
                       boost::bind(&DnsHandler::SendDnsIpc, _1));
}

DnsProto::DnsProto(Agent *agent, boost::asio::io_service &io) :
    Proto(agent, "Agent::Services", PktHandler::DNS, io),
    xid_(0), timeout_(kDnsTimeout), max_retries_(kDnsMaxRetries) {
    lid_ = agent->GetInterfaceTable()->Register(
                  boost::bind(&DnsProto::ItfUpdate, this, _2));
    Vnlid_ = agent->GetVnTable()->Register(
                  boost::bind(&DnsProto::VnUpdate, this, _2));
    agent->GetDomainConfigTable()->RegisterIpamCb(
                  boost::bind(&DnsProto::IpamUpdate, this, _1));
    agent->GetDomainConfigTable()->RegisterVdnsCb(
                  boost::bind(&DnsProto::VdnsUpdate, this, _1));
}

DnsProto::~DnsProto() {
    agent_->GetInterfaceTable()->Unregister(lid_);
    agent_->GetVnTable()->Unregister(Vnlid_);
}

ProtoHandler *DnsProto::AllocProtoHandler(boost::shared_ptr<PktInfo> info,
                                          boost::asio::io_service &io) {
    return new DnsHandler(agent(), info, io);
}

void DnsProto::ItfUpdate(DBEntryBase *entry) {
    Interface *itf = static_cast<Interface *>(entry);
    if (itf->type() != Interface::VM_INTERFACE)
        return;

    const VmInterface *vmitf = static_cast<VmInterface *>(entry);
    if (entry->IsDeleted()) {
        DnsHandler::SendDnsUpdateIpc(NULL, DnsAgentXmpp::Update, vmitf);
        DnsHandler::SendDnsUpdateIpc(NULL, DnsAgentXmpp::Update, vmitf, true);
        all_vms_.erase(vmitf);
    } else {
        uint32_t ttl = kDnsDefaultTtl;
        std::string vdns_name, domain;
        GetVdnsData(vmitf->vn(), vmitf->ip_addr(),
                    vdns_name, domain, ttl);
        VmDataMap::iterator it = all_vms_.find(vmitf);
        if (it == all_vms_.end()) {
            if (!UpdateDnsEntry(vmitf, vmitf->vn(), vdns_name,
                                vmitf->ip_addr(), false, false))
                vdns_name = "";
            IpVdnsMap vmdata;
            vmdata.insert(IpVdnsPair(vmitf->ip_addr().to_ulong(), vdns_name));
            all_vms_.insert(VmDataPair(vmitf, vmdata));
        } else {
            CheckForUpdate(it->second, vmitf, vmitf->vn(),
                           vmitf->ip_addr(), vdns_name, domain, ttl, false);
        }
    }
}

void DnsProto::VnUpdate(DBEntryBase *entry) {
    const VnEntry *vn = static_cast<const VnEntry *>(entry);
    if (vn->IsDeleted() || vn->GetVnIpam().size() == 0)
        return;
    for (VmDataMap::iterator it = all_vms_.begin(); it != all_vms_.end(); ++it) {
        if (it->first->vn() == vn) {
            uint32_t ttl = kDnsDefaultTtl;
            std::string vdns_name, domain;
            GetVdnsData(vn, it->first->ip_addr(), vdns_name, domain, ttl);
            CheckForUpdate(it->second, it->first, it->first->vn(),
                           it->first->ip_addr(), vdns_name, domain, ttl, false);
        }
        const VmInterface::FloatingIpSet &fip_list =
            it->first->floating_ip_list().list_;
        for (VmInterface::FloatingIpSet::iterator fip = fip_list.begin();
             fip != fip_list.end(); ++fip) {
            if (fip->vn_.get() == vn) {
                uint32_t ttl = kDnsDefaultTtl;
                std::string vdns_name, domain;
                GetVdnsData(vn, fip->floating_ip_, vdns_name, domain, ttl);
                CheckForUpdate(it->second, it->first, vn,
                               fip->floating_ip_, vdns_name, domain, ttl, true);
            }
        }
    }
}

void DnsProto::IpamUpdate(IFMapNode *node) {
    if (node->IsDeleted())
        return;
    ProcessUpdate(node->name(), node->IsDeleted(), true);
}

void DnsProto::VdnsUpdate(IFMapNode *node) {
    // Update any existing records prior to checking for new ones
    if (!node->IsDeleted()) {
        autogen::VirtualDns *virtual_dns =
                 static_cast <autogen::VirtualDns *> (node->GetObject());
        autogen::VirtualDnsType vdns_type = virtual_dns->data();
        std::string name = node->name();
        UpdateDnsEntry(NULL, name, name, vdns_type.domain_name,
                       (uint32_t)vdns_type.default_ttl_seconds, false);
    }
    ProcessUpdate(node->name(), node->IsDeleted(), false);
}

void DnsProto::ProcessUpdate(std::string name, bool is_deleted, bool is_ipam) {
    for (VmDataMap::iterator it = all_vms_.begin(); it != all_vms_.end(); ++it) {
        uint32_t ttl = kDnsDefaultTtl;
        std::string vdns_name, domain;
        GetVdnsData(it->first->vn(), it->first->ip_addr(),
                    vdns_name, domain, ttl);
        // in case of VDNS delete, clear the name
        if (!is_ipam && is_deleted && vdns_name == name)
            vdns_name.clear();
        CheckForUpdate(it->second, it->first, it->first->vn(),
                       it->first->ip_addr(), vdns_name, domain, ttl, false);

        const VmInterface::FloatingIpSet &fip_list =
            it->first->floating_ip_list().list_;
        for (VmInterface::FloatingIpSet::iterator fip = fip_list.begin();
             fip != fip_list.end(); ++fip) {
            VnEntry *vn = fip->vn_.get();
            ttl = kDnsDefaultTtl;
            GetVdnsData(vn, fip->floating_ip_, vdns_name, domain, ttl);
            CheckForUpdate(it->second, it->first, vn,
                           fip->floating_ip_, vdns_name, domain, ttl, true);
        }
    }
}

void DnsProto::CheckForUpdate(IpVdnsMap &ipvdns, const VmInterface *vmitf,
                              const VnEntry *vn, const Ip4Address &ip,
                              std::string &vdns_name, std::string &domain, 
                              uint32_t ttl, bool is_floating) {
    IpVdnsMap::iterator vmdata_it = ipvdns.find(ip.to_ulong());
    if (vmdata_it == ipvdns.end()) {
	if (UpdateDnsEntry(vmitf, vn, vdns_name, ip, is_floating, false))
            ipvdns.insert(IpVdnsPair(ip.to_ulong(), vdns_name));
        else
            ipvdns.insert(IpVdnsPair(ip.to_ulong(), ""));
	return;
    }
    if (vmdata_it->second.empty() && vmdata_it->second != vdns_name) {
	if (UpdateDnsEntry(vmitf, vn, vdns_name, ip, is_floating, false))
	    vmdata_it->second = vdns_name;
    } else if (vmdata_it->second != vdns_name) {
	if (UpdateDnsEntry(vmitf, vdns_name, vmdata_it->second, domain,
                           ttl, is_floating))
	    vmdata_it->second = vdns_name;
    }
}

// Send A & PTR record entries
void DnsProto::UpdateDnsEntry(const VmInterface *vmitf,
                              const std::string &name,
                              const Ip4Address &ip, uint32_t plen,
                              const std::string &vdns_name,
                              const autogen::VirtualDnsType &vdns_type,
                              bool is_floating, bool is_delete) {
    if (!name.size() || !vdns_type.dynamic_records_from_client) {
        DNS_BIND_TRACE(DnsBindTrace, "Not adding DNS entry; Name = " <<
                       name << "Dynamic records allowed = " <<
                       (vdns_type.dynamic_records_from_client ? "yes" : "no"));
        return;
    }

    DnsUpdateData *data = new DnsUpdateData();
    data->virtual_dns = vdns_name;
    data->zone = vdns_type.domain_name;

    // Add a DNS record
    DnsItem item;
    item.eclass = is_delete ? DNS_CLASS_NONE : DNS_CLASS_IN;
    item.type = DNS_A_RECORD;
    item.ttl = is_delete ? 0 : vdns_type.default_ttl_seconds;
    item.name = name;
    boost::system::error_code ec;
    item.data = ip.to_string(ec);
    data->items.push_back(item);

    DNS_BIND_TRACE(DnsBindTrace, "DNS update sent for : " << item.ToString() <<
                   " VDNS : " << data->virtual_dns << " Zone : " << data->zone);
    DnsHandler::SendDnsUpdateIpc(data, DnsAgentXmpp::Update, vmitf, is_floating);

    // Add a PTR record as well
    DnsUpdateData *ptr_data = new DnsUpdateData();
    ptr_data->virtual_dns = vdns_name;
    BindUtil::GetReverseZone(ip.to_ulong(), plen, ptr_data->zone);

    item.type = DNS_PTR_RECORD;
    item.data.swap(item.name);
    std::stringstream str;
    for (int i = 0; i < 4; i++) {
        str << ((ip.to_ulong() >> (i * 8)) & 0xFF);
        str << ".";
    }
    item.name = str.str() + "in-addr.arpa";
    ptr_data->items.push_back(item);

    DNS_BIND_TRACE(DnsBindTrace, "DNS update sent for : " << item.ToString() <<
                   " VDNS : " << ptr_data->virtual_dns <<
                   " Zone : " << ptr_data->zone);
    DnsHandler::SendDnsUpdateIpc(ptr_data, DnsAgentXmpp::Update, vmitf, is_floating);
}

// Update the floating ip entries
bool DnsProto::UpdateDnsEntry(const VmInterface *vmitf, const VnEntry *vn,
                              const Ip4Address &ip, bool is_deleted) {
    bool is_floating = true;
    uint32_t ttl = kDnsDefaultTtl;
    std::string vdns_name, domain;
    GetVdnsData(vn, ip, vdns_name, domain, ttl);
    VmDataMap::iterator it = all_vms_.find(vmitf);
    if (it == all_vms_.end()) {
        if (is_deleted)
            return true;
        if (!UpdateDnsEntry(vmitf, vn, vdns_name, ip, is_floating, false))
            vdns_name = "";
        IpVdnsMap vmdata;
        vmdata.insert(IpVdnsPair(ip.to_ulong(), vdns_name));
        all_vms_.insert(VmDataPair(vmitf, vmdata));
    } else {
        if (is_deleted) {
            UpdateDnsEntry(vmitf, vn, vdns_name, ip, is_floating, true);
            it->second.erase(ip.to_ulong());
        } else {
            CheckForUpdate(it->second, vmitf, vn, ip, vdns_name, domain,
                           ttl, is_floating);
        }
    }
    return true;
}

bool DnsProto::UpdateDnsEntry(const VmInterface *vmitf, const VnEntry *vn,
                              const std::string &vdns_name, const Ip4Address &ip,
                              bool is_floating, bool is_deleted) {
    if (!vdns_name.empty()) {
        uint32_t plen = 0;
        const std::vector<VnIpam> &ipam = vn->GetVnIpam();
        unsigned int i;
        for (i = 0; i < ipam.size(); ++i) {
            uint32_t mask = ipam[i].plen ? (0xFFFFFFFF << (32 - ipam[i].plen)) : 0;
            if ((ip.to_ulong() & mask) == (ipam[i].ip_prefix.to_ulong() & mask)) {
                plen = ipam[i].plen;
                break;
            }
        }
        if (i == ipam.size()) {
            DNS_BIND_TRACE(DnsBindTrace, "UpdateDnsEntry for VM <" <<
                           vmitf->vm_name() << "> not done for vn <" <<
                           vn->GetName() << "> IPAM doesnt have the subnet; " <<
                           ((is_deleted)? "delete" : "update"));
            return false;
        }

        autogen::VirtualDnsType vdns_type;
        if (agent_->GetDomainConfigTable()->GetVDns(vdns_name, &vdns_type)) {
            UpdateDnsEntry(vmitf, vmitf->vm_name(), ip, plen,
                           vdns_name, vdns_type, is_floating, is_deleted);
        } else {
            DNS_BIND_TRACE(DnsBindTrace, "UpdateDnsEntry for VM <" <<
                           vmitf->vm_name() << "> entry not " <<
                           ((is_deleted)? "deleted; " : "updated; ") <<
                           "VDNS info for " << vdns_name << " not present");
            return false;
        }
    }

    return true;
}

// Move DNS entries for a VM from one VDNS to another
bool DnsProto::UpdateDnsEntry(const VmInterface *vmitf,
                              std::string &new_vdns_name, std::string &old_vdns_name,
                              std::string &new_domain, uint32_t ttl, bool is_floating) {
    std::string name = (vmitf ? vmitf->vm_name() : "All Vms");
    DNS_BIND_TRACE(DnsBindTrace, "VDNS modify for VM <" << name <<
                   " old VDNS : " << old_vdns_name <<
                   " new VDNS : " << new_vdns_name <<
                   " ttl : " << ttl <<
                   " floating : " << (is_floating ? "yes" : "no"));
    DnsHandler::SendDnsUpdateIpc(vmitf, new_vdns_name,
                                 old_vdns_name, new_domain, ttl, is_floating);
    return true;
}

bool DnsProto::GetVdnsData(const VnEntry *vn, const Ip4Address &vm_addr, 
                           std::string &vdns_name, std::string &domain,
                           uint32_t &ttl) {
    if (!vn)
        return false;

    autogen::IpamType ipam_type;
    autogen::VirtualDnsType vdns_type;
    if (!vn->GetIpamVdnsData(vm_addr, &ipam_type, &vdns_type)) {
        DNS_BIND_TRACE(DnsBindTrace, "Unable to retrieve VDNS data; VN : " <<
                   vn->GetName() << " IP : " << vm_addr.to_string());
        return false;
    }

    vdns_name = ipam_type.ipam_dns_server.virtual_dns_server_name;
    domain = vdns_type.domain_name;
    ttl = (uint32_t)vdns_type.default_ttl_seconds;
    return true;
}

uint16_t DnsProto::GetTransId() {
    return (++xid_ == 0 ? ++xid_ : xid_);
}
