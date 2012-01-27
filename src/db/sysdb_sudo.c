/*
    Authors:
        Jakub Hrozek <jhrozek@redhat.com>

    Copyright (C) 2011 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _XOPEN_SOURCE

#include <talloc.h>
#include <time.h>

#include "db/sysdb.h"
#include "db/sysdb_private.h"
#include "db/sysdb_sudo.h"

#define NULL_CHECK(val, rval, label) do { \
    if (!val) {                           \
        rval = ENOMEM;                    \
        goto label;                       \
    }                                     \
} while(0)

/* ====================  Utility functions ==================== */

static errno_t sysdb_sudo_check_time(struct sysdb_attrs *rule,
                                     time_t now,
                                     bool *result)
{
    TALLOC_CTX *tmp_ctx = NULL;
    const char **values = NULL;
    char *tret = NULL;
    time_t converted;
    struct tm tm;
    errno_t ret;
    int i;

    tmp_ctx = talloc_new(NULL);
    NULL_CHECK(tmp_ctx, ret, done);

    /*
     * From man sudoers.ldap:
     *
     * A timestamp is in the form yyyymmddHHMMZ.
     * If multiple sudoNotBefore entries are present, the *earliest* is used.
     * If multiple sudoNotAfter entries are present, the *last one* is used.
     */

    /* check for sudoNotBefore */
    ret = sysdb_attrs_get_string_array(rule, SYSDB_SUDO_CACHE_AT_NOTBEFORE,
                                       tmp_ctx, &values);
    if (ret != EOK) {
        goto done;
    }
    if (values != NULL && values[0] != NULL) {
        tret = strptime(values[0], SYSDB_SUDO_TIME_FORMAT, &tm);
        if (tret == NULL || *tret != '\0') {
            DEBUG(SSSDBG_FUNC_DATA, ("Invalid time format!\n"));
            ret = EINVAL;
            goto done;
        }
        converted = mktime(&tm);

        if (now < converted) {
            *result = false;
            goto done;
        }
    }

    /* check for sudoNotAfter */
    ret = sysdb_attrs_get_string_array(rule, SYSDB_SUDO_CACHE_AT_NOTAFTER,
                                       tmp_ctx, &values);
    if (ret != EOK) {
        goto done;
    }
    if (values != NULL && values[0] != NULL) {
        /* find last value */
        for (i = 0; values[i] != NULL; i++) {
            // do nothing
        }

        tret = strptime(values[i - 1], SYSDB_SUDO_TIME_FORMAT, &tm);
        if (tret == NULL || *tret != '\0') {
            DEBUG(SSSDBG_FUNC_DATA, ("Invalid time format!\n"));
            ret = EINVAL;
            goto done;
        }
        converted = mktime(&tm);

        if (now > converted) {
            *result = false;
            goto done;
        }
    }

    *result = true;
    ret = EOK;

done:
    if (ret == ENOENT) {
        *result = true;
        ret = EOK;
    }

    talloc_free(tmp_ctx);
    return ret;
}

errno_t sysdb_sudo_filter_rules_by_time(TALLOC_CTX *mem_ctx,
                                        size_t in_num_rules,
                                        struct sysdb_attrs **in_rules,
                                        time_t now,
                                        size_t *_num_rules,
                                        struct sysdb_attrs ***_rules)
{
    size_t num_rules = 0;
    struct sysdb_attrs **rules = NULL;
    TALLOC_CTX *tmp_ctx = NULL;
    bool allowed = false;
    errno_t ret;
    int i;

    tmp_ctx = talloc_new(NULL);
    NULL_CHECK(tmp_ctx, ret, done);

    if (now == 0) {
        now = time(NULL);
    }

    for (i = 0; i < in_num_rules; i++) {
        ret = sysdb_sudo_check_time(in_rules[i], now, &allowed);
        if (ret == EOK && allowed) {
            num_rules++;
            rules = talloc_realloc(tmp_ctx, rules, struct sysdb_attrs *,
                                   num_rules);
            NULL_CHECK(rules, ret, done);

            rules[num_rules - 1] = in_rules[i];
        }
    }

    *_num_rules = num_rules;
    *_rules = talloc_steal(mem_ctx, rules);

    ret = EOK;

done:
    talloc_free(tmp_ctx);
    return ret;
}

errno_t
sysdb_get_sudo_filter(TALLOC_CTX *mem_ctx, const char *username,
                      uid_t uid, char **groupnames, unsigned int flags,
                      char **_filter)
{
    TALLOC_CTX *tmp_ctx = NULL;
    char *filter = NULL;
    char *specific_filter = NULL;
    errno_t ret;
    int i;

    tmp_ctx = talloc_new(NULL);
    NULL_CHECK(tmp_ctx, ret, done);

    /* build specific filter */

    specific_filter = talloc_zero(tmp_ctx, char); /* assign to tmp_ctx */
    NULL_CHECK(specific_filter, ret, done);

    if (flags & SYSDB_SUDO_FILTER_INCLUDE_ALL) {
        specific_filter = talloc_asprintf_append(specific_filter, "(%s=ALL)",
                                                 SYSDB_SUDO_CACHE_AT_USER);
        NULL_CHECK(specific_filter, ret, done);
    }

    if (flags & SYSDB_SUDO_FILTER_INCLUDE_DFL) {
        specific_filter = talloc_asprintf_append(specific_filter, "(%s=defaults)",
                                                 SYSDB_NAME);
        NULL_CHECK(specific_filter, ret, done);
    }

    if ((flags & SYSDB_SUDO_FILTER_USERNAME) && (username != NULL)) {
        specific_filter = talloc_asprintf_append(specific_filter, "(%s=%s)",
                                                 SYSDB_SUDO_CACHE_AT_USER,
                                                 username);
        NULL_CHECK(specific_filter, ret, done);
    }

    if ((flags & SYSDB_SUDO_FILTER_UID) && (uid != 0)) {
        specific_filter = talloc_asprintf_append(specific_filter, "(%s=#%llu)",
                                                 SYSDB_SUDO_CACHE_AT_USER,
                                                 (unsigned long long) uid);
        NULL_CHECK(specific_filter, ret, done);
    }

    if ((flags & SYSDB_SUDO_FILTER_GROUPS) && (groupnames != NULL)) {
        for (i=0; groupnames[i] != NULL; i++) {
            specific_filter = talloc_asprintf_append(specific_filter, "(%s=%%%s)",
                                                     SYSDB_SUDO_CACHE_AT_USER,
                                                     groupnames[i]);
            NULL_CHECK(specific_filter, ret, done);
        }
    }

    if (flags & SYSDB_SUDO_FILTER_NGRS) {
        specific_filter = talloc_asprintf_append(specific_filter, "(%s=+*)",
                                                 SYSDB_SUDO_CACHE_AT_USER);
        NULL_CHECK(specific_filter, ret, done);
    }

    /* build global filter */

    filter = talloc_asprintf(tmp_ctx, "(&(%s=%s)",
                             SYSDB_OBJECTCLASS, SYSDB_SUDO_CACHE_AT_OC);
    NULL_CHECK(filter, ret, done);

    if (specific_filter[0] != '\0') {
        filter = talloc_asprintf_append(filter, "(|%s)", specific_filter);
        NULL_CHECK(filter, ret, done);
    }

    filter = talloc_strdup_append(filter, ")");
    NULL_CHECK(filter, ret, done);

    ret = EOK;
    *_filter = talloc_steal(mem_ctx, filter);

done:
    talloc_free(tmp_ctx);
    return ret;
}

errno_t
sysdb_get_sudo_user_info(TALLOC_CTX *mem_ctx, const char *username,
                         struct sysdb_ctx *sysdb, uid_t *_uid,
                         char ***groupnames)
{
    TALLOC_CTX *tmp_ctx;
    errno_t ret;
    const char *attrs[3];
    struct ldb_message *msg;
    char **sysdb_groupnames = NULL;
    struct ldb_message_element *groups;
    uid_t uid;
    int i;

    tmp_ctx = talloc_new(NULL);
    NULL_CHECK(tmp_ctx, ret, done);

    attrs[0] = SYSDB_MEMBEROF;
    attrs[1] = SYSDB_UIDNUM;
    attrs[2] = NULL;
    ret = sysdb_search_user_by_name(tmp_ctx, sysdb, username,
                                    attrs, &msg);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("Error looking up user %s\n", username));
        goto done;
    }

    uid = ldb_msg_find_attr_as_uint64(msg, SYSDB_UIDNUM, 0);
    if (!uid) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("A user with no UID?\n"));
        ret = EIO;
        goto done;
    }

    groups = ldb_msg_find_element(msg, SYSDB_MEMBEROF);
    if (!groups || groups->num_values == 0) {
        /* No groups for this user in sysdb currently */
        sysdb_groupnames = NULL;
    } else {
        sysdb_groupnames = talloc_array(tmp_ctx, char *, groups->num_values+1);
        NULL_CHECK(sysdb_groupnames, ret, done);

        /* Get a list of the groups by groupname only */
        for (i = 0; i < groups->num_values; i++) {
            ret = sysdb_group_dn_name(sysdb,
                                      sysdb_groupnames,
                                      (const char *)groups->values[i].data,
                                      &sysdb_groupnames[i]);
            if (ret != EOK) {
                ret = ENOMEM;
                goto done;
            }
        }
        sysdb_groupnames[groups->num_values] = NULL;
    }

    ret = EOK;
    *_uid = uid;
    *groupnames = talloc_steal(mem_ctx, sysdb_groupnames);
done:
    talloc_free(tmp_ctx);
    return EOK;
}

static errno_t
sysdb_sudo_purge_subdir(struct sysdb_ctx *sysdb,
                        struct sss_domain_info *domain,
                        const char *subdir)
{
    struct ldb_dn *base_dn = NULL;
    TALLOC_CTX *tmp_ctx = NULL;
    errno_t ret;

    tmp_ctx = talloc_new(NULL);
    NULL_CHECK(tmp_ctx, ret, done);

    base_dn = sysdb_custom_subtree_dn(sysdb, tmp_ctx, domain->name, subdir);
    NULL_CHECK(base_dn, ret, done);

    ret = sysdb_delete_recursive(sysdb, base_dn, true);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, ("sysdb_delete_recursive failed.\n"));
        goto done;
    }

    ret = EOK;
done:
    talloc_free(tmp_ctx);
    return EOK;
}

errno_t
sysdb_save_sudorule(struct sysdb_ctx *sysdb_ctx,
                   const char *rule_name,
                   struct sysdb_attrs *attrs)
{
    errno_t ret;

    DEBUG(SSSDBG_TRACE_FUNC, ("Adding sudo rule %s\n", rule_name));

    ret = sysdb_attrs_add_string(attrs, SYSDB_OBJECTCLASS,
                                 SYSDB_SUDO_CACHE_AT_OC);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, ("Could not set rule object class [%d]: %s\n",
              ret, strerror(ret)));
        return ret;
    }

    ret = sysdb_attrs_add_string(attrs, SYSDB_NAME, rule_name);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, ("Could not set name attribute [%d]: %s\n",
              ret, strerror(ret)));
        return ret;
    }

    ret = sysdb_store_custom(sysdb_ctx, rule_name, SUDORULE_SUBDIR, attrs);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, ("sysdb_store_custom failed [%d]: %s\n",
              ret, strerror(ret)));
        return ret;
    }

    return EOK;
}

errno_t
sysdb_purge_sudorule_subtree(struct sysdb_ctx *sysdb,
                             struct sss_domain_info *domain,
                             const char *filter)
{
    TALLOC_CTX *tmp_ctx;
    size_t count;
    struct ldb_message **msgs;
    const char *name;
    int i;
    errno_t ret;
    const char *attrs[] = { SYSDB_OBJECTCLASS,
                            SYSDB_NAME,
                            SYSDB_SUDO_CACHE_AT_OC,
                            SYSDB_SUDO_CACHE_AT_CN,
                            NULL };

    /* just purge all if there's no filter */
    if (!filter) {
        return sysdb_sudo_purge_subdir(sysdb, domain, SUDORULE_SUBDIR);
    }

    tmp_ctx = talloc_new(NULL);
    NULL_CHECK(tmp_ctx, ret, done);

    /* match entries based on the filter and remove them one by one */
    ret = sysdb_search_custom(tmp_ctx, sysdb, filter,
                              SUDORULE_SUBDIR, attrs,
                              &count, &msgs);
    if (ret != EOK && ret != ENOENT) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("Error looking up SUDO rules"));
        goto done;
    } if (ret == ENOENT) {
        DEBUG(SSSDBG_TRACE_FUNC, ("No rules matched\n"));
        ret = EOK;
        goto done;
    }

    for (i = 0; i < count; i++) {
        name = ldb_msg_find_attr_as_string(msgs[i], SYSDB_NAME, NULL);
        if (name == NULL) {
            DEBUG(SSSDBG_OP_FAILURE, ("A rule without a name?\n"));
            /* skip this one but still delete other entries */
            continue;
        }

        ret = sysdb_delete_custom(sysdb, name, SUDORULE_SUBDIR);
        if (ret != EOK) {
            DEBUG(SSSDBG_OP_FAILURE, ("Could not delete rule %s\n", name));
            goto done;
        }
    }

    ret = EOK;
done:
    talloc_free(tmp_ctx);
    return ret;
}

errno_t sysdb_sudo_set_refreshed(struct sysdb_ctx *sysdb,
                                 bool refreshed)
{
    errno_t ret;
    struct ldb_dn *dn;
    TALLOC_CTX *tmp_ctx;


    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) {
        ret = ENOMEM;
        goto done;
    }

    dn = ldb_dn_new_fmt(tmp_ctx, sysdb->ldb, SYSDB_TMPL_CUSTOM_SUBTREE,
                        SUDORULE_SUBDIR, sysdb->domain->name);
    if (!dn) {
        ret = ENOMEM;
        goto done;
    }

    ret = sysdb_set_bool(sysdb, dn, SUDORULE_SUBDIR,
                         SYSDB_SUDO_AT_REFRESHED, refreshed);

done:
    talloc_free(tmp_ctx);
    return ret;
}

errno_t sysdb_sudo_get_refreshed(struct sysdb_ctx *sysdb,
                                 bool *refreshed)
{
    errno_t ret;
    struct ldb_dn *dn;
    TALLOC_CTX *tmp_ctx;


    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) {
        ret = ENOMEM;
        goto done;
    }

    dn = ldb_dn_new_fmt(tmp_ctx, sysdb->ldb, SYSDB_TMPL_CUSTOM_SUBTREE,
                        SUDORULE_SUBDIR, sysdb->domain->name);
    if (!dn) {
        ret = ENOMEM;
        goto done;
    }

    ret = sysdb_get_bool(sysdb, dn, SYSDB_SUDO_AT_REFRESHED, refreshed);

done:
    talloc_free(tmp_ctx);
    return ret;
}
