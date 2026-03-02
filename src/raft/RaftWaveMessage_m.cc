//
// Generated file, do not edit! Created by nedtool 5.6 from raft/RaftWaveMessage.msg.
//

// Disable warnings about unused variables, empty switch stmts, etc:
#ifdef _MSC_VER
#  pragma warning(disable:4101)
#  pragma warning(disable:4065)
#endif

#if defined(__clang__)
#  pragma clang diagnostic ignored "-Wshadow"
#  pragma clang diagnostic ignored "-Wconversion"
#  pragma clang diagnostic ignored "-Wunused-parameter"
#  pragma clang diagnostic ignored "-Wc++98-compat"
#  pragma clang diagnostic ignored "-Wunreachable-code-break"
#  pragma clang diagnostic ignored "-Wold-style-cast"
#elif defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wshadow"
#  pragma GCC diagnostic ignored "-Wconversion"
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#  pragma GCC diagnostic ignored "-Wold-style-cast"
#  pragma GCC diagnostic ignored "-Wsuggest-attribute=noreturn"
#  pragma GCC diagnostic ignored "-Wfloat-conversion"
#endif

#include <iostream>
#include <sstream>
#include <memory>
#include "RaftWaveMessage_m.h"

namespace omnetpp {

// Template pack/unpack rules. They are declared *after* a1l type-specific pack functions for multiple reasons.
// They are in the omnetpp namespace, to allow them to be found by argument-dependent lookup via the cCommBuffer argument

// Packing/unpacking an std::vector
template<typename T, typename A>
void doParsimPacking(omnetpp::cCommBuffer *buffer, const std::vector<T,A>& v)
{
    int n = v.size();
    doParsimPacking(buffer, n);
    for (int i = 0; i < n; i++)
        doParsimPacking(buffer, v[i]);
}

template<typename T, typename A>
void doParsimUnpacking(omnetpp::cCommBuffer *buffer, std::vector<T,A>& v)
{
    int n;
    doParsimUnpacking(buffer, n);
    v.resize(n);
    for (int i = 0; i < n; i++)
        doParsimUnpacking(buffer, v[i]);
}

// Packing/unpacking an std::list
template<typename T, typename A>
void doParsimPacking(omnetpp::cCommBuffer *buffer, const std::list<T,A>& l)
{
    doParsimPacking(buffer, (int)l.size());
    for (typename std::list<T,A>::const_iterator it = l.begin(); it != l.end(); ++it)
        doParsimPacking(buffer, (T&)*it);
}

template<typename T, typename A>
void doParsimUnpacking(omnetpp::cCommBuffer *buffer, std::list<T,A>& l)
{
    int n;
    doParsimUnpacking(buffer, n);
    for (int i = 0; i < n; i++) {
        l.push_back(T());
        doParsimUnpacking(buffer, l.back());
    }
}

// Packing/unpacking an std::set
template<typename T, typename Tr, typename A>
void doParsimPacking(omnetpp::cCommBuffer *buffer, const std::set<T,Tr,A>& s)
{
    doParsimPacking(buffer, (int)s.size());
    for (typename std::set<T,Tr,A>::const_iterator it = s.begin(); it != s.end(); ++it)
        doParsimPacking(buffer, *it);
}

template<typename T, typename Tr, typename A>
void doParsimUnpacking(omnetpp::cCommBuffer *buffer, std::set<T,Tr,A>& s)
{
    int n;
    doParsimUnpacking(buffer, n);
    for (int i = 0; i < n; i++) {
        T x;
        doParsimUnpacking(buffer, x);
        s.insert(x);
    }
}

// Packing/unpacking an std::map
template<typename K, typename V, typename Tr, typename A>
void doParsimPacking(omnetpp::cCommBuffer *buffer, const std::map<K,V,Tr,A>& m)
{
    doParsimPacking(buffer, (int)m.size());
    for (typename std::map<K,V,Tr,A>::const_iterator it = m.begin(); it != m.end(); ++it) {
        doParsimPacking(buffer, it->first);
        doParsimPacking(buffer, it->second);
    }
}

template<typename K, typename V, typename Tr, typename A>
void doParsimUnpacking(omnetpp::cCommBuffer *buffer, std::map<K,V,Tr,A>& m)
{
    int n;
    doParsimUnpacking(buffer, n);
    for (int i = 0; i < n; i++) {
        K k; V v;
        doParsimUnpacking(buffer, k);
        doParsimUnpacking(buffer, v);
        m[k] = v;
    }
}

// Default pack/unpack function for arrays
template<typename T>
void doParsimArrayPacking(omnetpp::cCommBuffer *b, const T *t, int n)
{
    for (int i = 0; i < n; i++)
        doParsimPacking(b, t[i]);
}

template<typename T>
void doParsimArrayUnpacking(omnetpp::cCommBuffer *b, T *t, int n)
{
    for (int i = 0; i < n; i++)
        doParsimUnpacking(b, t[i]);
}

// Default rule to prevent compiler from choosing base class' doParsimPacking() function
template<typename T>
void doParsimPacking(omnetpp::cCommBuffer *, const T& t)
{
    throw omnetpp::cRuntimeError("Parsim error: No doParsimPacking() function for type %s", omnetpp::opp_typename(typeid(t)));
}

template<typename T>
void doParsimUnpacking(omnetpp::cCommBuffer *, T& t)
{
    throw omnetpp::cRuntimeError("Parsim error: No doParsimUnpacking() function for type %s", omnetpp::opp_typename(typeid(t)));
}

}  // namespace omnetpp

namespace {
template <class T> inline
typename std::enable_if<std::is_polymorphic<T>::value && std::is_base_of<omnetpp::cObject,T>::value, void *>::type
toVoidPtr(T* t)
{
    return (void *)(static_cast<const omnetpp::cObject *>(t));
}

template <class T> inline
typename std::enable_if<std::is_polymorphic<T>::value && !std::is_base_of<omnetpp::cObject,T>::value, void *>::type
toVoidPtr(T* t)
{
    return (void *)dynamic_cast<const void *>(t);
}

template <class T> inline
typename std::enable_if<!std::is_polymorphic<T>::value, void *>::type
toVoidPtr(T* t)
{
    return (void *)static_cast<const void *>(t);
}

}

namespace benchmark {

// forward
template<typename T, typename A>
std::ostream& operator<<(std::ostream& out, const std::vector<T,A>& vec);

// Template rule to generate operator<< for shared_ptr<T>
template<typename T>
inline std::ostream& operator<<(std::ostream& out,const std::shared_ptr<T>& t) { return out << t.get(); }

// Template rule which fires if a struct or class doesn't have operator<<
template<typename T>
inline std::ostream& operator<<(std::ostream& out,const T&) {return out;}

// operator<< for std::vector<T>
template<typename T, typename A>
inline std::ostream& operator<<(std::ostream& out, const std::vector<T,A>& vec)
{
    out.put('{');
    for(typename std::vector<T,A>::const_iterator it = vec.begin(); it != vec.end(); ++it)
    {
        if (it != vec.begin()) {
            out.put(','); out.put(' ');
        }
        out << *it;
    }
    out.put('}');

    char buf[32];
    sprintf(buf, " (size=%u)", (unsigned int)vec.size());
    out.write(buf, strlen(buf));
    return out;
}

EXECUTE_ON_STARTUP(
    omnetpp::cEnum *e = omnetpp::cEnum::find("benchmark::RaftMsgType");
    if (!e) omnetpp::enums.getInstance()->add(e = new omnetpp::cEnum("benchmark::RaftMsgType"));
    e->insert(DISCOVERY_BEACON, "DISCOVERY_BEACON");
    e->insert(CLUSTER_FORM, "CLUSTER_FORM");
    e->insert(CLUSTER_EXISTS, "CLUSTER_EXISTS");
    e->insert(CLUSTER_INVITATION, "CLUSTER_INVITATION");
    e->insert(RAFT_REQUEST_VOTE, "RAFT_REQUEST_VOTE");
    e->insert(RAFT_REQUEST_VOTE_RESPONSE, "RAFT_REQUEST_VOTE_RESPONSE");
    e->insert(RAFT_APPEND_ENTRIES, "RAFT_APPEND_ENTRIES");
    e->insert(RAFT_APPEND_ENTRIES_RESPONSE, "RAFT_APPEND_ENTRIES_RESPONSE");
    e->insert(COORD_STATUS_REQUEST, "COORD_STATUS_REQUEST");
    e->insert(COORD_STATUS_RESPONSE, "COORD_STATUS_RESPONSE");
    e->insert(COORD_VEHICLE_PASSED, "COORD_VEHICLE_PASSED");
    e->insert(COORD_VEHICLE_LEFT, "COORD_VEHICLE_LEFT");
    e->insert(COORD_VEHICLE_LEFT_REBROADCAST, "COORD_VEHICLE_LEFT_REBROADCAST");
    e->insert(LATE_JOIN_ORDER, "LATE_JOIN_ORDER");
)

Register_Class(RaftWaveMessage)

RaftWaveMessage::RaftWaveMessage(const char *name, short kind) : ::veins::BaseFrame1609_4(name, kind)
{
}

RaftWaveMessage::RaftWaveMessage(const RaftWaveMessage& other) : ::veins::BaseFrame1609_4(other)
{
    copy(other);
}

RaftWaveMessage::~RaftWaveMessage()
{
    delete [] this->payload;
}

RaftWaveMessage& RaftWaveMessage::operator=(const RaftWaveMessage& other)
{
    if (this == &other) return *this;
    ::veins::BaseFrame1609_4::operator=(other);
    copy(other);
    return *this;
}

void RaftWaveMessage::copy(const RaftWaveMessage& other)
{
    this->msgType = other.msgType;
    this->senderId = other.senderId;
    this->targetId = other.targetId;
    this->payloadLen = other.payloadLen;
    delete [] this->payload;
    this->payload = (other.payload_arraysize==0) ? nullptr : new char[other.payload_arraysize];
    payload_arraysize = other.payload_arraysize;
    for (size_t i = 0; i < payload_arraysize; i++) {
        this->payload[i] = other.payload[i];
    }
}

void RaftWaveMessage::parsimPack(omnetpp::cCommBuffer *b) const
{
    ::veins::BaseFrame1609_4::parsimPack(b);
    doParsimPacking(b,this->msgType);
    doParsimPacking(b,this->senderId);
    doParsimPacking(b,this->targetId);
    doParsimPacking(b,this->payloadLen);
    b->pack(payload_arraysize);
    doParsimArrayPacking(b,this->payload,payload_arraysize);
}

void RaftWaveMessage::parsimUnpack(omnetpp::cCommBuffer *b)
{
    ::veins::BaseFrame1609_4::parsimUnpack(b);
    doParsimUnpacking(b,this->msgType);
    doParsimUnpacking(b,this->senderId);
    doParsimUnpacking(b,this->targetId);
    doParsimUnpacking(b,this->payloadLen);
    delete [] this->payload;
    b->unpack(payload_arraysize);
    if (payload_arraysize == 0) {
        this->payload = nullptr;
    } else {
        this->payload = new char[payload_arraysize];
        doParsimArrayUnpacking(b,this->payload,payload_arraysize);
    }
}

int RaftWaveMessage::getMsgType() const
{
    return this->msgType;
}

void RaftWaveMessage::setMsgType(int msgType)
{
    this->msgType = msgType;
}

int RaftWaveMessage::getSenderId() const
{
    return this->senderId;
}

void RaftWaveMessage::setSenderId(int senderId)
{
    this->senderId = senderId;
}

int RaftWaveMessage::getTargetId() const
{
    return this->targetId;
}

void RaftWaveMessage::setTargetId(int targetId)
{
    this->targetId = targetId;
}

unsigned int RaftWaveMessage::getPayloadLen() const
{
    return this->payloadLen;
}

void RaftWaveMessage::setPayloadLen(unsigned int payloadLen)
{
    this->payloadLen = payloadLen;
}

size_t RaftWaveMessage::getPayloadArraySize() const
{
    return payload_arraysize;
}

char RaftWaveMessage::getPayload(size_t k) const
{
    if (k >= payload_arraysize) throw omnetpp::cRuntimeError("Array of size payload_arraysize indexed by %lu", (unsigned long)k);
    return this->payload[k];
}

void RaftWaveMessage::setPayloadArraySize(size_t newSize)
{
    char *payload2 = (newSize==0) ? nullptr : new char[newSize];
    size_t minSize = payload_arraysize < newSize ? payload_arraysize : newSize;
    for (size_t i = 0; i < minSize; i++)
        payload2[i] = this->payload[i];
    for (size_t i = minSize; i < newSize; i++)
        payload2[i] = 0;
    delete [] this->payload;
    this->payload = payload2;
    payload_arraysize = newSize;
}

void RaftWaveMessage::setPayload(size_t k, char payload)
{
    if (k >= payload_arraysize) throw omnetpp::cRuntimeError("Array of size  indexed by %lu", (unsigned long)k);
    this->payload[k] = payload;
}

void RaftWaveMessage::insertPayload(size_t k, char payload)
{
    if (k > payload_arraysize) throw omnetpp::cRuntimeError("Array of size  indexed by %lu", (unsigned long)k);
    size_t newSize = payload_arraysize + 1;
    char *payload2 = new char[newSize];
    size_t i;
    for (i = 0; i < k; i++)
        payload2[i] = this->payload[i];
    payload2[k] = payload;
    for (i = k + 1; i < newSize; i++)
        payload2[i] = this->payload[i-1];
    delete [] this->payload;
    this->payload = payload2;
    payload_arraysize = newSize;
}

void RaftWaveMessage::insertPayload(char payload)
{
    insertPayload(payload_arraysize, payload);
}

void RaftWaveMessage::erasePayload(size_t k)
{
    if (k >= payload_arraysize) throw omnetpp::cRuntimeError("Array of size  indexed by %lu", (unsigned long)k);
    size_t newSize = payload_arraysize - 1;
    char *payload2 = (newSize == 0) ? nullptr : new char[newSize];
    size_t i;
    for (i = 0; i < k; i++)
        payload2[i] = this->payload[i];
    for (i = k; i < newSize; i++)
        payload2[i] = this->payload[i+1];
    delete [] this->payload;
    this->payload = payload2;
    payload_arraysize = newSize;
}

class RaftWaveMessageDescriptor : public omnetpp::cClassDescriptor
{
  private:
    mutable const char **propertynames;
    enum FieldConstants {
        FIELD_msgType,
        FIELD_senderId,
        FIELD_targetId,
        FIELD_payloadLen,
        FIELD_payload,
    };
  public:
    RaftWaveMessageDescriptor();
    virtual ~RaftWaveMessageDescriptor();

    virtual bool doesSupport(omnetpp::cObject *obj) const override;
    virtual const char **getPropertyNames() const override;
    virtual const char *getProperty(const char *propertyname) const override;
    virtual int getFieldCount() const override;
    virtual const char *getFieldName(int field) const override;
    virtual int findField(const char *fieldName) const override;
    virtual unsigned int getFieldTypeFlags(int field) const override;
    virtual const char *getFieldTypeString(int field) const override;
    virtual const char **getFieldPropertyNames(int field) const override;
    virtual const char *getFieldProperty(int field, const char *propertyname) const override;
    virtual int getFieldArraySize(void *object, int field) const override;

    virtual const char *getFieldDynamicTypeString(void *object, int field, int i) const override;
    virtual std::string getFieldValueAsString(void *object, int field, int i) const override;
    virtual bool setFieldValueAsString(void *object, int field, int i, const char *value) const override;

    virtual const char *getFieldStructName(int field) const override;
    virtual void *getFieldStructValuePointer(void *object, int field, int i) const override;
};

Register_ClassDescriptor(RaftWaveMessageDescriptor)

RaftWaveMessageDescriptor::RaftWaveMessageDescriptor() : omnetpp::cClassDescriptor(omnetpp::opp_typename(typeid(benchmark::RaftWaveMessage)), "veins::BaseFrame1609_4")
{
    propertynames = nullptr;
}

RaftWaveMessageDescriptor::~RaftWaveMessageDescriptor()
{
    delete[] propertynames;
}

bool RaftWaveMessageDescriptor::doesSupport(omnetpp::cObject *obj) const
{
    return dynamic_cast<RaftWaveMessage *>(obj)!=nullptr;
}

const char **RaftWaveMessageDescriptor::getPropertyNames() const
{
    if (!propertynames) {
        static const char *names[] = {  nullptr };
        omnetpp::cClassDescriptor *basedesc = getBaseClassDescriptor();
        const char **basenames = basedesc ? basedesc->getPropertyNames() : nullptr;
        propertynames = mergeLists(basenames, names);
    }
    return propertynames;
}

const char *RaftWaveMessageDescriptor::getProperty(const char *propertyname) const
{
    omnetpp::cClassDescriptor *basedesc = getBaseClassDescriptor();
    return basedesc ? basedesc->getProperty(propertyname) : nullptr;
}

int RaftWaveMessageDescriptor::getFieldCount() const
{
    omnetpp::cClassDescriptor *basedesc = getBaseClassDescriptor();
    return basedesc ? 5+basedesc->getFieldCount() : 5;
}

unsigned int RaftWaveMessageDescriptor::getFieldTypeFlags(int field) const
{
    omnetpp::cClassDescriptor *basedesc = getBaseClassDescriptor();
    if (basedesc) {
        if (field < basedesc->getFieldCount())
            return basedesc->getFieldTypeFlags(field);
        field -= basedesc->getFieldCount();
    }
    static unsigned int fieldTypeFlags[] = {
        FD_ISEDITABLE,    // FIELD_msgType
        FD_ISEDITABLE,    // FIELD_senderId
        FD_ISEDITABLE,    // FIELD_targetId
        FD_ISEDITABLE,    // FIELD_payloadLen
        FD_ISARRAY | FD_ISEDITABLE,    // FIELD_payload
    };
    return (field >= 0 && field < 5) ? fieldTypeFlags[field] : 0;
}

const char *RaftWaveMessageDescriptor::getFieldName(int field) const
{
    omnetpp::cClassDescriptor *basedesc = getBaseClassDescriptor();
    if (basedesc) {
        if (field < basedesc->getFieldCount())
            return basedesc->getFieldName(field);
        field -= basedesc->getFieldCount();
    }
    static const char *fieldNames[] = {
        "msgType",
        "senderId",
        "targetId",
        "payloadLen",
        "payload",
    };
    return (field >= 0 && field < 5) ? fieldNames[field] : nullptr;
}

int RaftWaveMessageDescriptor::findField(const char *fieldName) const
{
    omnetpp::cClassDescriptor *basedesc = getBaseClassDescriptor();
    int base = basedesc ? basedesc->getFieldCount() : 0;
    if (fieldName[0] == 'm' && strcmp(fieldName, "msgType") == 0) return base+0;
    if (fieldName[0] == 's' && strcmp(fieldName, "senderId") == 0) return base+1;
    if (fieldName[0] == 't' && strcmp(fieldName, "targetId") == 0) return base+2;
    if (fieldName[0] == 'p' && strcmp(fieldName, "payloadLen") == 0) return base+3;
    if (fieldName[0] == 'p' && strcmp(fieldName, "payload") == 0) return base+4;
    return basedesc ? basedesc->findField(fieldName) : -1;
}

const char *RaftWaveMessageDescriptor::getFieldTypeString(int field) const
{
    omnetpp::cClassDescriptor *basedesc = getBaseClassDescriptor();
    if (basedesc) {
        if (field < basedesc->getFieldCount())
            return basedesc->getFieldTypeString(field);
        field -= basedesc->getFieldCount();
    }
    static const char *fieldTypeStrings[] = {
        "int",    // FIELD_msgType
        "int",    // FIELD_senderId
        "int",    // FIELD_targetId
        "unsigned int",    // FIELD_payloadLen
        "char",    // FIELD_payload
    };
    return (field >= 0 && field < 5) ? fieldTypeStrings[field] : nullptr;
}

const char **RaftWaveMessageDescriptor::getFieldPropertyNames(int field) const
{
    omnetpp::cClassDescriptor *basedesc = getBaseClassDescriptor();
    if (basedesc) {
        if (field < basedesc->getFieldCount())
            return basedesc->getFieldPropertyNames(field);
        field -= basedesc->getFieldCount();
    }
    switch (field) {
        case FIELD_msgType: {
            static const char *names[] = { "enum", "enum",  nullptr };
            return names;
        }
        default: return nullptr;
    }
}

const char *RaftWaveMessageDescriptor::getFieldProperty(int field, const char *propertyname) const
{
    omnetpp::cClassDescriptor *basedesc = getBaseClassDescriptor();
    if (basedesc) {
        if (field < basedesc->getFieldCount())
            return basedesc->getFieldProperty(field, propertyname);
        field -= basedesc->getFieldCount();
    }
    switch (field) {
        case FIELD_msgType:
            if (!strcmp(propertyname, "enum")) return "RaftMsgType";
            if (!strcmp(propertyname, "enum")) return "benchmark::RaftMsgType";
            return nullptr;
        default: return nullptr;
    }
}

int RaftWaveMessageDescriptor::getFieldArraySize(void *object, int field) const
{
    omnetpp::cClassDescriptor *basedesc = getBaseClassDescriptor();
    if (basedesc) {
        if (field < basedesc->getFieldCount())
            return basedesc->getFieldArraySize(object, field);
        field -= basedesc->getFieldCount();
    }
    RaftWaveMessage *pp = (RaftWaveMessage *)object; (void)pp;
    switch (field) {
        case FIELD_payload: return pp->getPayloadArraySize();
        default: return 0;
    }
}

const char *RaftWaveMessageDescriptor::getFieldDynamicTypeString(void *object, int field, int i) const
{
    omnetpp::cClassDescriptor *basedesc = getBaseClassDescriptor();
    if (basedesc) {
        if (field < basedesc->getFieldCount())
            return basedesc->getFieldDynamicTypeString(object,field,i);
        field -= basedesc->getFieldCount();
    }
    RaftWaveMessage *pp = (RaftWaveMessage *)object; (void)pp;
    switch (field) {
        default: return nullptr;
    }
}

std::string RaftWaveMessageDescriptor::getFieldValueAsString(void *object, int field, int i) const
{
    omnetpp::cClassDescriptor *basedesc = getBaseClassDescriptor();
    if (basedesc) {
        if (field < basedesc->getFieldCount())
            return basedesc->getFieldValueAsString(object,field,i);
        field -= basedesc->getFieldCount();
    }
    RaftWaveMessage *pp = (RaftWaveMessage *)object; (void)pp;
    switch (field) {
        case FIELD_msgType: return enum2string(pp->getMsgType(), "benchmark::RaftMsgType");
        case FIELD_senderId: return long2string(pp->getSenderId());
        case FIELD_targetId: return long2string(pp->getTargetId());
        case FIELD_payloadLen: return ulong2string(pp->getPayloadLen());
        case FIELD_payload: return long2string(pp->getPayload(i));
        default: return "";
    }
}

bool RaftWaveMessageDescriptor::setFieldValueAsString(void *object, int field, int i, const char *value) const
{
    omnetpp::cClassDescriptor *basedesc = getBaseClassDescriptor();
    if (basedesc) {
        if (field < basedesc->getFieldCount())
            return basedesc->setFieldValueAsString(object,field,i,value);
        field -= basedesc->getFieldCount();
    }
    RaftWaveMessage *pp = (RaftWaveMessage *)object; (void)pp;
    switch (field) {
        case FIELD_msgType: pp->setMsgType((benchmark::RaftMsgType)string2enum(value, "benchmark::RaftMsgType")); return true;
        case FIELD_senderId: pp->setSenderId(string2long(value)); return true;
        case FIELD_targetId: pp->setTargetId(string2long(value)); return true;
        case FIELD_payloadLen: pp->setPayloadLen(string2ulong(value)); return true;
        case FIELD_payload: pp->setPayload(i,string2long(value)); return true;
        default: return false;
    }
}

const char *RaftWaveMessageDescriptor::getFieldStructName(int field) const
{
    omnetpp::cClassDescriptor *basedesc = getBaseClassDescriptor();
    if (basedesc) {
        if (field < basedesc->getFieldCount())
            return basedesc->getFieldStructName(field);
        field -= basedesc->getFieldCount();
    }
    switch (field) {
        default: return nullptr;
    };
}

void *RaftWaveMessageDescriptor::getFieldStructValuePointer(void *object, int field, int i) const
{
    omnetpp::cClassDescriptor *basedesc = getBaseClassDescriptor();
    if (basedesc) {
        if (field < basedesc->getFieldCount())
            return basedesc->getFieldStructValuePointer(object, field, i);
        field -= basedesc->getFieldCount();
    }
    RaftWaveMessage *pp = (RaftWaveMessage *)object; (void)pp;
    switch (field) {
        default: return nullptr;
    }
}

} // namespace benchmark

