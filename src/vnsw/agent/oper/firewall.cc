/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#include <algorithm>
#include <boost/uuid/uuid_io.hpp>
#include <base/parse_object.h>
#include <ifmap/ifmap_link.h>
#include <ifmap/ifmap_table.h>
#include <vnc_cfg_types.h>

#include <cmn/agent_cmn.h>
#include <cfg/cfg_init.h>
#include <oper/firewall.h>

#include <oper/interface_common.h>
#include <oper/config_manager.h>

using namespace autogen;
using namespace std;

TagTable *TagTable::tag_table_;

bool TagEntry::IsLess(const DBEntry &rhs) const {
    const TagEntry &a = static_cast<const TagEntry &>(rhs);
    return (tag_uuid_ < a.tag_uuid_);
}

string TagEntry::ToString() const {
    std::stringstream uuidstring;
    uuidstring << tag_uuid_;
    return uuidstring.str();
}

DBEntryBase::KeyPtr TagEntry::GetDBRequestKey() const {
    TagKey *key = new TagKey(tag_uuid_);
    return DBEntryBase::KeyPtr(key);
}

void TagEntry::SetKey(const DBRequestKey *key) { 
    const TagKey *k = static_cast<const TagKey *>(key);
    tag_uuid_ = k->tag_uuid_;
}

std::auto_ptr<DBEntry> TagTable::AllocEntry(const DBRequestKey *k) const {
    const TagKey *key = static_cast<const TagKey *>(k);
    TagEntry *tag = new TagEntry(key->tag_uuid_);
    return std::auto_ptr<DBEntry>(static_cast<DBEntry *>(tag));
}

DBEntry *TagTable::OperDBAdd(const DBRequest *req) {
    TagKey *key = static_cast<TagKey *>(req->key.get());
    TagEntry *tag = new TagEntry(key->tag_uuid_);
    ChangeHandler(tag, req);
    return tag;
}

bool TagTable::OperDBOnChange(DBEntry *entry, const DBRequest *req) {
    bool ret = ChangeHandler(entry, req);
    //TagEntry *tag = static_cast<TagEntry *>(entry);
    return ret;
}

bool TagTable::ChangeHandler(DBEntry *entry, const DBRequest *req) {
    bool ret = false;
    TagEntry *tag = static_cast<TagEntry *>(entry);
    TagData *data = static_cast<TagData *>(req->data.get());
    
    if (tag->tag_id_ != data->tag_id_) {
        tag->tag_id_ = data->tag_id_;
        ret = true;
    }

    return ret;
}

bool TagTable::OperDBDelete(DBEntry *entry, const DBRequest *req) {
    //TagEntry *tag = static_cast<TagEntry *>(entry);
    return true;
}

DBTableBase *TagTable::CreateTable(DB *db, const std::string &name) {
    tag_table_ = new TagTable(db, name);
    tag_table_->Init();
    return tag_table_;
};

bool TagTable::IFNodeToUuid(IFMapNode *node, boost::uuids::uuid &u) {
    Tag *cfg = static_cast<Tag *>(node->GetObject());
    assert(cfg);
    autogen::IdPermsType id_perms = cfg->id_perms();
    CfgUuidSet(id_perms.uuid.uuid_mslong, id_perms.uuid.uuid_lslong, u);
    return true;
}

bool TagTable::IFNodeToReq(IFMapNode *node, DBRequest &req,
        const boost::uuids::uuid &u) {
    Tag *cfg = static_cast<Tag *>(node->GetObject());
    assert(cfg);

    assert(!u.is_nil());

    if ((req.oper == DBRequest::DB_ENTRY_DELETE) || node->IsDeleted()) {
        req.oper = DBRequest::DB_ENTRY_DELETE;
        req.key.reset(new TagKey(u));
        //agent()->tag_table()->Enqueue(&req);
        return false;
    }

    agent()->config_manager()->AddTagNode(node);
    return false;
}

bool TagTable::ProcessConfig(IFMapNode *node, DBRequest &req,
        const boost::uuids::uuid &u) {

    if (node->IsDeleted())
        return false;

    Tag *cfg = static_cast<Tag *>(node->GetObject());
    assert(cfg);

    TagKey *key = new TagKey(u);
    TagData *data  = NULL;

    req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
    uint32_t tag_id = cfg->id();
    if (tag_id == TagTable::kInvalidTagId) {
        OPER_TRACE(Sg, "Ignore tag id 0", UuidToString(u));
        return false;
    }

    IFMapAgentTable *table = static_cast<IFMapAgentTable *>(node->table());
    for (DBGraphVertex::adjacency_iterator iter =
         node->begin(table->GetGraph()); 
         iter != node->end(table->GetGraph()); ++iter) {
        IFMapNode *adj_node = static_cast<IFMapNode *>(iter.operator->());
        if (agent()->config_manager()->SkipNode(adj_node)) {
            continue;
        }

    }
    data = new TagData(agent(), node, tag_id);
    req.key.reset(key);
    req.data.reset(data);
    agent()->tag_table()->Enqueue(&req);

    return false;
}

bool TagEntry::DBEntrySandesh(Sandesh *sresp, std::string &name)  const {
    return false;
}

