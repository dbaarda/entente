/*=
 * Copyright (c) 2017 Donovan Baarda <abo@minkirri.apana.org.au>
 *
 * Licensed under the MIT License. See LICENSE file for details.
 */

#include "nss2ldap.h"
#include "utils.h"

/* PartialAttribute methods. */
static PartialAttribute_t *PartialAttribute_new(const char *type);
static LDAPString_t *PartialAttribute_add(PartialAttribute_t *attr, const char *value);
static LDAPString_t *PartialAttribute_addf(PartialAttribute_t *attr, char *format, ...);

/* SearchResultEntry methods. */
static PartialAttribute_t *SearchResultEntry_add(SearchResultEntry_t *res, const char *type);
static const PartialAttribute_t *SearchResultEntry_get(const SearchResultEntry_t *res, const char *type);
static void SearchResultEntry_passwd(SearchResultEntry_t *res, const char *basedn, passwd_t *pw);
static void SearchResultEntry_group(SearchResultEntry_t *res, const char *basedn, group_t *gr);
static int SearchResultEntry_getpwnam(SearchResultEntry_t *res, const char *basedn, const char *name);

/* AttributeValueAssertion methods */
static bool AttributeValueAssertion_equal(const AttributeValueAssertion_t *equal, const SearchResultEntry_t *res);

/* Filter methods. */
static bool Filter_matches(const Filter_t *filter, const SearchResultEntry_t *res);
static bool Filter_ok(const Filter_t *filter);

/* Initialize an ldap_reponse. */
void ldap_response_init(ldap_response *res, int size)
{
    assert(res);
    assert(size > 0);

    res->count = 0;
    res->next = 0;
    res->size = size;
    res->msgs = XNEW0(LDAPMessage_t *, size);
}

/* Destroy an ldap_response. */
void ldap_response_done(ldap_response *res)
{
    assert(res);

    for (int i = 0; i < res->count; i++)
        ldapmessage_free(res->msgs[i]);
    free(res->msgs);
}

/* Add an LDAPMessage to an ldap_response. */
LDAPMessage_t *ldap_response_add(ldap_response *res)
{
    assert(res);

    /* Double the allocated size if full. */
    if (res->count == res->size) {
        res->size *= 2;
        res->msgs = XRENEW(res->msgs, LDAPMessage_t *, res->size);
    }
    return res->msgs[res->count++] = XNEW0(LDAPMessage_t, 1);
}

/* Get the next LDAPMessage_t to send. */
LDAPMessage_t *ldap_response_get(ldap_response *res)
{
    assert(res);

    if (res->next < res->count)
        return res->msgs[res->next];
    return NULL;
}

/* Increment the next LDAPMessage_t to send. */
void ldap_response_inc(ldap_response *res)
{
    assert(res);

    res->next++;
}

/* Get the ldap_response for a SearchRequest message. */
void ldap_response_search(ldap_response *res, const char *basedn, const int msgid, const SearchRequest_t *req)
{
    assert(req);
    assert(basedn);
    assert(res);
    const int bad_dn = strcmp((const char *)req->baseObject.buf, basedn)
        && strcmp((const char *)req->baseObject.buf, "");
    const int bad_filter = !Filter_ok(&req->filter);
    int limit = req->sizeLimit;

    /* Adjust limit to RESPONSE_MAX if it is zero or too large. */
    limit = (limit && (limit < RESPONSE_MAX)) ? limit : RESPONSE_MAX;
    LDAPMessage_t *msg = ldap_response_add(res);
    /* Add all the matching entries. */
    if (!bad_dn && !bad_filter) {
        passwd_t *pw;
        while ((pw = getpwent()) && (res->count <= limit)) {
            msg->messageID = msgid;
            msg->protocolOp.present = LDAPMessage__protocolOp_PR_searchResEntry;
            SearchResultEntry_t *entry = &msg->protocolOp.choice.searchResEntry;
            SearchResultEntry_passwd(entry, basedn, pw);
            if (Filter_matches(&req->filter, entry)) {
                /* The entry matches, keep it and add another. */
                msg = ldap_response_add(res);
            } else {
                /* Empty and wipe the entry message for the next one. */
                ldapmessage_empty(msg);
                memset(msg, 0, sizeof(*msg));
            }
        }
        endpwent();
        group_t *gr;
        while ((gr = getgrent()) && (res->count <= limit)) {
            msg->messageID = msgid;
            msg->protocolOp.present = LDAPMessage__protocolOp_PR_searchResEntry;
            SearchResultEntry_t *entry = &msg->protocolOp.choice.searchResEntry;
            SearchResultEntry_group(entry, basedn, gr);
            if (Filter_matches(&req->filter, entry)) {
                /* The entry matches, keep it and add another. */
                msg = ldap_response_add(res);
            } else {
                /* Empty and wipe the entry message for the next one. */
                ldapmessage_empty(msg);
                memset(msg, 0, sizeof(*msg));
            }
        }
        endgrent();
    }
    /* Otherwise construct a SearchResultDone. */
    msg->messageID = msgid;
    msg->protocolOp.present = LDAPMessage__protocolOp_PR_searchResDone;
    SearchResultDone_t *done = &msg->protocolOp.choice.searchResDone;
    if (bad_dn) {
        done->resultCode = LDAPResult__resultCode_other;
        LDAPString_set(&done->diagnosticMessage, "baseobject is invalid");
    } else if (bad_filter) {
        done->resultCode = LDAPResult__resultCode_other;
        LDAPString_set(&done->diagnosticMessage, "filter not supported");
    } else {
        done->resultCode = LDAPResult__resultCode_success;
        LDAPString_set(&done->matchedDN, basedn);
    }
}

/* Get the cn from the first field of a gecos entry. */
char *gecos2cn(const char *gecos, char *cn)
{
    assert(gecos);
    assert(cn);
    size_t len = strcspn(gecos, ",");

    memcpy(cn, gecos, len);
    cn[len] = '\0';
    return cn;
}

/* Return a full "uid=<name>,ou=people,..." ldap dn from a name and basedn. */
char *name2dn(const char *basedn, const char *name, char *dn)
{
    assert(basedn);
    assert(name);
    assert(dn);
    snprintf(dn, STRING_MAX, "uid=%s,ou=people,%s", name, basedn);
    return dn;
}

/* Return a full "uid=<name>,ou=groups,..." ldap dn from a name and basedn. */
char *group2dn(const char *basedn, const char *group, char *dn)
{
    assert(basedn);
    assert(group);
    assert(dn);
    snprintf(dn, STRING_MAX, "cn=%s,ou=groups,%s", group, basedn);
    return dn;
}

/* Return the name from a full "uid=<name>,ou=people,..." ldap dn. */
char *dn2name(const char *basedn, const char *dn, char *name)
{
    assert(basedn);
    assert(dn);
    assert(name);
    /* uid=$name$,ou=people,$basedn$ */
    const char *pos = dn + 4;
    const char *end = strchr(dn, ',');
    size_t len = end - pos;

    if (!end || strncmp(dn, "uid=", 4) || strncmp(end, ",ou=people,", 11) || strcmp(end + 11, basedn))
        return NULL;
    memcpy(name, pos, len);
    name[len] = '\0';
    return name;
}

/* Allocate a PartialAttribute and set it's type. */
static PartialAttribute_t *PartialAttribute_new(const char *type)
{
    assert(type);
    PartialAttribute_t *a = XNEW0(PartialAttribute_t, 1);

    LDAPString_set(&a->type, type);
    return a;
}

/* Add a string value to a PartialAttribute. */
static LDAPString_t *PartialAttribute_add(PartialAttribute_t *attr, const char *value)
{
    assert(attr);
    assert(value);
    LDAPString_t *s = LDAPString_new(value);
    assert(s);

    asn_set_add(&attr->vals, s);
    return s;
}

/* Add a formated value to a PartialAttribute. */
static LDAPString_t *PartialAttribute_addf(PartialAttribute_t *attr, char *format, ...)
{
    assert(attr);
    assert(format);
    char v[STRING_MAX];
    va_list args;

    va_start(args, format);
    vsnprintf(v, sizeof(v), format, args);
    return PartialAttribute_add(attr, v);
}

/* Add a PartialAttribute to a SearchResultEntry. */
static PartialAttribute_t *SearchResultEntry_add(SearchResultEntry_t *res, const char *type)
{
    assert(res);
    assert(type);
    PartialAttribute_t *a = PartialAttribute_new(type);
    assert(a);

    asn_sequence_add(&res->attributes, a);
    return a;
}

/* Get a PartialAttribute from a SearchResultEntry. */
static const PartialAttribute_t *SearchResultEntry_get(const SearchResultEntry_t *res, const char *type)
{
    assert(res);
    assert(type);

    for (int i = 0; i < res->attributes.list.count; i++) {
        const PartialAttribute_t *attr = res->attributes.list.array[i];
        if (!strcmp((const char *)attr->type.buf, type))
            return attr;
    }
    return NULL;
}

/* Set a SearchResultEntry from an nss passwd entry. */
static void SearchResultEntry_passwd(SearchResultEntry_t *res, const char *basedn, passwd_t *pw)
{
    assert(res);
    assert(basedn);
    assert(pw);
    PartialAttribute_t *attribute;
    char buf[STRING_MAX];

    LDAPString_set(&res->objectName, name2dn(basedn, pw->pw_name, buf));
    attribute = SearchResultEntry_add(res, "objectClass");
    PartialAttribute_add(attribute, "top");
    PartialAttribute_add(attribute, "account");
    PartialAttribute_add(attribute, "posixAccount");
    attribute = SearchResultEntry_add(res, "uid");
    PartialAttribute_add(attribute, pw->pw_name);
    attribute = SearchResultEntry_add(res, "cn");
    PartialAttribute_add(attribute, gecos2cn(pw->pw_gecos, buf));
    attribute = SearchResultEntry_add(res, "userPassword");
    PartialAttribute_addf(attribute, "{crypt}%s", pw->pw_passwd);
    attribute = SearchResultEntry_add(res, "uidNumber");
    PartialAttribute_addf(attribute, "%i", pw->pw_uid);
    attribute = SearchResultEntry_add(res, "gidNumber");
    PartialAttribute_addf(attribute, "%i", pw->pw_gid);
    attribute = SearchResultEntry_add(res, "gecos");
    PartialAttribute_add(attribute, pw->pw_gecos);
    attribute = SearchResultEntry_add(res, "homeDirectory");
    PartialAttribute_add(attribute, pw->pw_dir);
    attribute = SearchResultEntry_add(res, "loginShell");
    PartialAttribute_add(attribute, pw->pw_shell);
}

/* Set a SearchResultEntry from an nss group entry. */
static void SearchResultEntry_group(SearchResultEntry_t *res, const char *basedn, group_t *gr)
{
    assert(res);
    assert(basedn);
    assert(gr);
    PartialAttribute_t *attribute;
    char buf[STRING_MAX];

    LDAPString_set(&res->objectName, group2dn(basedn, gr->gr_name, buf));
    attribute = SearchResultEntry_add(res, "objectClass");
    PartialAttribute_add(attribute, "top");
    PartialAttribute_add(attribute, "posixGroup");
    attribute = SearchResultEntry_add(res, "cn");
    PartialAttribute_add(attribute, gr->gr_name);
    attribute = SearchResultEntry_add(res, "userPassword");
    PartialAttribute_addf(attribute, "{crypt}%s", gr->gr_passwd);
    attribute = SearchResultEntry_add(res, "gidNumber");
    PartialAttribute_addf(attribute, "%i", gr->gr_gid);
    attribute = SearchResultEntry_add(res, "memberUid");
    for (char **m = gr->gr_mem; *m; m++)
        PartialAttribute_add(attribute, *m);
}

/* Set a SearchResultEntry from an nss user's name. */
static int SearchResultEntry_getpwnam(SearchResultEntry_t *res, const char *basedn, const char *name)
{
    assert(res);
    assert(basedn);
    assert(name);
    passwd_t *pw = getpwnam(name);

    if (!pw)
        return -1;
    SearchResultEntry_passwd(res, basedn, pw);
    return 0;
}

/* Check if an AttributeValueAssertion is equal to a SearchResultEntry */
static bool AttributeValueAssertion_equal(const AttributeValueAssertion_t *equal, const SearchResultEntry_t *res)
{
    assert(equal);
    assert(res);
    const char *name = (const char *)equal->attributeDesc.buf;
    const char *value = (const char *)equal->assertionValue.buf;
    const PartialAttribute_t *attr = SearchResultEntry_get(res, name);

    if (attr)
        for (int i = 0; i < attr->vals.list.count; i++)
            if (!strcmp((const char *)attr->vals.list.array[i]->buf, value))
                return true;
    return false;
}

/* Check if a Filter is fully supported. */
static bool Filter_ok(const Filter_t *filter)
{
    assert(filter);

    switch (filter->present) {
    case Filter_PR_and:
        for (int i = 0; i < filter->choice.And.list.count; i++)
            if (!Filter_ok(filter->choice.And.list.array[i]))
                return false;
        return true;
    case Filter_PR_or:
        for (int i = 0; i < filter->choice.Or.list.count; i++)
            if (!Filter_ok(filter->choice.Or.list.array[i]))
                return false;
        return true;
    case Filter_PR_not:
        return Filter_ok(filter->choice.Not);
    case Filter_PR_equalityMatch:
    case Filter_PR_present:
        return true;
    case Filter_PR_substrings:
    case Filter_PR_greaterOrEqual:
    case Filter_PR_lessOrEqual:
    case Filter_PR_approxMatch:
    case Filter_PR_extensibleMatch:
    default:
        return false;
    }
}

/* Check if a Filter matches a SearchResultEntry. */
static bool Filter_matches(const Filter_t *filter, const SearchResultEntry_t *res)
{
    assert(filter);
    assert(res);
    assert(Filter_ok(filter));

    switch (filter->present) {
    case Filter_PR_and:
        for (int i = 0; i < filter->choice.And.list.count; i++)
            if (!Filter_matches(filter->choice.And.list.array[i], res))
                return false;
        return true;
    case Filter_PR_or:
        for (int i = 0; i < filter->choice.Or.list.count; i++)
            if (Filter_matches(filter->choice.Or.list.array[i], res))
                return true;
        return false;
    case Filter_PR_not:
        return !Filter_matches(filter->choice.Not, res);
    case Filter_PR_equalityMatch:
        return AttributeValueAssertion_equal(&filter->choice.equalityMatch, res);
    case Filter_PR_present:
        return SearchResultEntry_get(res, (const char *)filter->choice.present.buf) != NULL;
    case Filter_PR_substrings:
    case Filter_PR_greaterOrEqual:
    case Filter_PR_lessOrEqual:
    case Filter_PR_approxMatch:
    case Filter_PR_extensibleMatch:
    default:
        return false;
    }
}
