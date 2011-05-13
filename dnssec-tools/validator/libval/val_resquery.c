/*
 * Portions Copyright (c) 1995, 1996, 1997 by Trusted Information Systems, Inc.
 *
 * Permission to use, copy modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND TRUSTED INFORMATION SYSTEMS
 * DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL
 * TRUSTED INFORMATION SYSTEMS BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING
 * FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THE SOFTWARE.
 */
/*
 * Copyright 2005-2011 SPARTA, Inc.  All rights reserved.
 * See the COPYING file distributed with this software for details.
 */
/*
 * DESCRIPTION
 * Contains resolver functionality in libval 
 */
#include "validator-config.h"
#include "validator-internal.h"

#include "val_resquery.h"
#include "val_support.h"
#include "val_cache.h"
#include "val_assertion.h"
#include "val_context.h"

#define MERGE_RR(old_rr, new_rr) do{ \
	if (old_rr == NULL) \
		old_rr = new_rr;\
	else {\
		struct rrset_rec    *tail;\
		tail = old_rr;\
		while (tail->rrs_next != NULL)\
			tail = tail->rrs_next;\
		tail->rrs_next = new_rr;\
	}\
} while (0)

static int _process_rcvd_response(val_context_t * context,
                                  struct queries_for_query *matched_qfq,
                                  struct domain_info **response,
                                  struct queries_for_query **queries,
                                  struct timeval *closest_event,
                                  const char *name_p,
                                  struct name_server *server,
                                  u_char *response_data, size_t response_length);

/*
 * create a name_server struct from the given address rdata
 */
static int
extract_glue_from_rdata(struct val_rr_rec *addr_rr, struct name_server *ns)
{
    struct sockaddr_in *sock_in;
#ifdef VAL_IPV6
    struct sockaddr_in6 *sock_in6;
#endif

    if (ns == NULL) 
        return VAL_BAD_ARGUMENT;

    while (addr_rr) {
        int             i;
        struct sockaddr_storage **new_addr = NULL;

        if ((addr_rr->rr_rdata_length != 4)
#ifdef VAL_IPV6
            && addr_rr->rr_rdata_length != sizeof(struct in6_addr)
#endif
            ) {
            val_log(NULL, LOG_DEBUG, "extract_glue_from_rdata(): Skipping address with bad len=%d.",
                    addr_rr->rr_rdata_length);
            addr_rr = addr_rr->rr_next;
            continue;
        }

        new_addr = create_nsaddr_array(ns->ns_number_of_addresses + 1);
        if (new_addr == NULL)
            return VAL_OUT_OF_MEMORY;

        for (i = 0; i < ns->ns_number_of_addresses; i++) {
            memcpy(new_addr[i], ns->ns_address[i],
                   sizeof(struct sockaddr_storage));
            FREE(ns->ns_address[i]);
        }
        if (ns->ns_address)
            FREE(ns->ns_address);
        ns->ns_address = new_addr;

        if (addr_rr->rr_rdata_length == sizeof(struct in_addr)) {
            sock_in = (struct sockaddr_in *)
                ns->ns_address[ns->ns_number_of_addresses];
            memset(sock_in, 0, sizeof(struct sockaddr_in));
            sock_in->sin_family = AF_INET;
            sock_in->sin_port = htons(DNS_PORT);
            memcpy(&(sock_in->sin_addr), addr_rr->rr_rdata, 
                    sizeof(struct in_addr));
        }
#ifdef VAL_IPV6
        else if (addr_rr->rr_rdata_length == sizeof(struct in6_addr)) {
            sock_in6 = (struct sockaddr_in6 *)
                ns->ns_address[ns->ns_number_of_addresses];
            memset(sock_in6, 0, sizeof(struct sockaddr_in6));
            sock_in6->sin6_family = AF_INET6;
            sock_in6->sin6_port = htons(DNS_PORT);
            memcpy(&(sock_in6->sin6_addr), addr_rr->rr_rdata,
                   sizeof(struct in6_addr));
        }
#endif
        ns->ns_number_of_addresses++;
        addr_rr = addr_rr->rr_next;

    }
    return VAL_NO_ERROR;
}

/*
 * merge the data received from a glue fetch operation into
 * the original query. Also check for glue fetch loops.
 */
static int
merge_glue_in_referral(val_context_t *context,
                       struct queries_for_query *qfq_pc,
                       struct glue_fetch_bucket *bucket,
                       struct queries_for_query **queries)
{
    int             retval;
    struct queries_for_query *glue_qfq = NULL;
    struct val_query_chain *glueptr = NULL;
    struct val_query_chain *pc;
    struct name_server *pending_ns;
    char name_p[NS_MAXDNAME];
    u_int32_t flags;

    /*
     * check if we have data to merge 
     */
    if ((queries == NULL) || (qfq_pc == NULL) || (bucket == NULL)) 
        return VAL_BAD_ARGUMENT; 

    pc = qfq_pc->qfq_query; /* Can never be NULL if qfq_pc is not NULL */
    
    /* Nothing to do */
    if (pc->qc_referral == NULL) 
        return VAL_NO_ERROR;
    
    if (pc->qc_referral->cur_pending_glue_ns) {
        pending_ns = pc->qc_referral->cur_pending_glue_ns;
        if (ns_name_ntop(pending_ns->ns_name_n, name_p,
                     sizeof(name_p)) < 0) {
            strncpy(name_p, "unknown/error", sizeof(name_p)-1); 
        }
        
        /*
         * Identify the query in the query chain 
         */
        flags = qfq_pc->qfq_flags | 
                VAL_QUERY_GLUE_REQUEST;

        if (VAL_NO_ERROR != (retval = 
                    add_to_qfq_chain(context,
                                     queries, pending_ns->ns_name_n, ns_t_a, 
                                     ns_c_in, flags, &glue_qfq))) 
            return retval;

        glueptr = glue_qfq->qfq_query;/* Can never be NULL if glue_qfq is not NULL */
        
        /*
         * Check if glue was obtained 
         */
        if (glueptr->qc_state >= Q_ANSWERED) { 

            struct val_digested_auth_chain *as;
            
            /* This could be a cname or dname alias; search for the A record */
            for (as=glueptr->qc_ans; as; as=as->val_ac_rrset.val_ac_rrset_next) {
                if (as->val_ac_rrset.ac_data && 
                    (as->val_ac_rrset.ac_data->rrs_type_h == ns_t_a ||
                     as->val_ac_rrset.ac_data->rrs_type_h == ns_t_aaaa))
                    break;
            }
       
            if (as &&
                glueptr->qc_state == Q_ANSWERED &&
                VAL_NO_ERROR == (retval =
                    extract_glue_from_rdata(as->val_ac_rrset.ac_data->rrs_data,
                                            pending_ns))) {
                
                /* continue referral using the fetched glue records */

                val_log(context, LOG_DEBUG,
                        "merge_glue_in_referral(): successfully fetched glue for %s", 
                        name_p);
            
                /* save learned zone information */
                if (VAL_NO_ERROR != (retval = 
                        stow_zone_info(&pc->qc_referral->learned_zones, pc))) {
                    return retval;
                }
                pc->qc_referral->learned_zones = NULL;

                if (pc->qc_respondent_server) {
                    free_name_server(&pc->qc_respondent_server);
                    pc->qc_respondent_server = NULL;
                }
                if (pc->qc_ns_list) {
                    free_name_servers(&pc->qc_ns_list);
                    pc->qc_ns_list = NULL;
                }

                pc->qc_ns_list = pending_ns;
                /* release older reference to pending_ns */
                pc->qc_referral->cur_pending_glue_ns = NULL;

                if (pc->qc_zonecut_n != NULL) {
                    FREE(pc->qc_zonecut_n);
                    pc->qc_zonecut_n = NULL;
                }
                /* update the zonecut to the current referral point */
                u_char *cur_ref_n = pc->qc_referral->saved_zonecut_n;
                if (cur_ref_n != NULL) {
                    size_t len = wire_name_length(cur_ref_n);
                    pc->qc_zonecut_n =
                            (u_char *) MALLOC(len * sizeof(u_char));
                    if (pc->qc_zonecut_n == NULL)
                        return VAL_OUT_OF_MEMORY;
                    memcpy(pc->qc_zonecut_n, cur_ref_n, len);
                }

                pc->qc_state = Q_INIT;
                return VAL_NO_ERROR;            
            } else {
                /* error condition */
                val_log(context, LOG_DEBUG, 
                        "merge_glue_in_referral(): Could not fetch glue for %s", 
                        name_p);
                glueptr->qc_state = Q_REFERRAL_ERROR;
            } 
        } else {

            /* check if we have a loop */
            int i;
            for (i=0; i<bucket->qfq_count; i++) {
                if (bucket->qfq[i] == glue_qfq) {
                    /* loop detected */
                    val_log(context, LOG_DEBUG, 
                            "merge_glue_in_referral(): Loop detected while fetching glue for %s",
                            name_p);
                    glueptr->qc_state = Q_REFERRAL_ERROR;
                    break; 
                } 
            }
            if (i == bucket->qfq_count) {
                /* add this glueptr to the bucket */
                if ((bucket->qfq_count+1) == MAX_GLUE_FETCH_DEPTH) {
                    /* too many dependencies */
                    val_log(context, LOG_DEBUG, 
                            "merge_glue_in_referral(): Too many glue requests in dependency chain for %s",
                            name_p);
                    glueptr->qc_state = Q_REFERRAL_ERROR;
                } else {
                    bucket->qfq[i] = glue_qfq;
                    bucket->qfq_count++;
                }
            }
        }

        if (glueptr->qc_state >= Q_ERROR_BASE) {
            /* free this name server */
            free_name_server(&pending_ns);
            pending_ns = NULL;
            pc->qc_referral->cur_pending_glue_ns = NULL;
            pc->qc_state = Q_MISSING_GLUE;
        }
    } 
    
    if (pc->qc_state >= Q_ERROR_BASE &&
        pc->qc_referral->pending_glue_ns != NULL) {

        /* there is more glue to fetch */
        u_int32_t flags;
        struct queries_for_query *added_q = NULL;

        pc->qc_referral->cur_pending_glue_ns = pc->qc_referral->pending_glue_ns;
        pending_ns = pc->qc_referral->cur_pending_glue_ns;

        pc->qc_referral->pending_glue_ns = pending_ns->ns_next;
        pending_ns->ns_next = NULL;
        /*
         * Create a query for glue for pending_ns 
         */
        flags = qfq_pc->qfq_flags | VAL_QUERY_GLUE_REQUEST;

        if (VAL_NO_ERROR != (retval = add_to_qfq_chain(context,
                                       queries, pending_ns->ns_name_n, ns_t_a,
                                       ns_c_in, flags, &added_q)))
           return retval;

        pc->qc_state = Q_WAIT_FOR_GLUE;

    } 

    return VAL_NO_ERROR;
}

/*
 * Check queries in list for missing glue
 * Merge any glue that is available into the relevant query
 * Set *data_missing if some query in the list still remains
 * unanswered
 */
int
fix_glue(val_context_t * context,
         struct queries_for_query **queries,
         int *data_missing)
{
    struct queries_for_query *next_q;
    struct glue_fetch_bucket *depn_bucket = NULL;
    int    retval;
    char   name_p[NS_MAXDNAME];
    int i;

    retval = VAL_NO_ERROR;
   
    if (context == NULL || queries == NULL || data_missing == NULL)
        return VAL_BAD_ARGUMENT;

    if (*data_missing == 0)
        return VAL_NO_ERROR;

    *data_missing = 0;
    for (next_q = *queries; next_q; next_q = next_q->qfq_next) {
        if (next_q->qfq_query->qc_state < Q_ANSWERED) {
            *data_missing = 1;
        }
        if (next_q->qfq_query->qc_state == Q_WAIT_FOR_GLUE ||
            next_q->qfq_query->qc_state >= Q_ERROR_BASE) {

            struct glue_fetch_bucket *b;

            if (-1 == ns_name_ntop(next_q->qfq_query->qc_name_n, name_p, sizeof(name_p)))
                snprintf(name_p, sizeof(name_p), "unknown/error");

            /* 
             * check for a glue fetch loop 
             */
            /* first, find  the dependency bucket */
            for (b = depn_bucket; b; b=b->next_bucket) {
               for (i=0; i<b->qfq_count; i++) {
                   if (b->qfq[i] == next_q)
                       break;
               }
               if (i < b->qfq_count)
                   break;
            }

            if (b == NULL) {
                /* create a new bucket if one does not exist */
                b = (struct glue_fetch_bucket *) MALLOC (sizeof (struct glue_fetch_bucket));
                if (b == NULL)
                    goto err;
                memset(b->qfq, 0, MAX_GLUE_FETCH_DEPTH);
                b->qfq_count = 1;
                b->qfq[0] = next_q;
                /* add to head of list */
                b->next_bucket = depn_bucket;
                depn_bucket = b;
            }
            /* 
             * next, check if the glue for this query is already in the bucket 
             * or check if the glue was returned 
             */
            if (VAL_NO_ERROR != (retval =
                        merge_glue_in_referral(context,
                                               next_q,
                                               b,
                                               queries))) {
                goto err;
            }
            if (next_q->qfq_query->qc_state >= Q_ERROR_BASE) {
                val_log(context, LOG_DEBUG,
                        "fix_glue(): Error fetching {%s %d %d} and no pending glue (state: %d)", name_p,
                        next_q->qfq_query->qc_class_h, next_q->qfq_query->qc_type_h,
                        next_q->qfq_query->qc_state);
            }
        }
    }

err:
    /* free up depn_bucket */
    while(depn_bucket) {
        struct glue_fetch_bucket *temp = depn_bucket;

        depn_bucket = depn_bucket->next_bucket;
        FREE(temp);
    }

    return retval;
}

/*
 * Identify the referral name servers from the rrsets 
 * returned in the response. The glue may be missing,
 * in which case we save the incomplete name server information
 * in "pending glue"
 */ 
int
res_zi_unverified_ns_list(struct name_server **ns_list,
                          u_char * zone_name,
                          struct rrset_rec *unchecked_zone_info,
                          struct name_server **pending_glue)
{
    struct rrset_rec *unchecked_set;
    struct val_rr_rec  *ns_rr;
    struct name_server *temp_ns;
    struct name_server *ns;
    struct name_server *pending_glue_last;
    struct name_server *trail_ns;
    struct name_server *outer_trailer;
    struct name_server *tail_ns;
    size_t          name_len;
    int             retval;

    if ((ns_list == NULL) || (pending_glue == NULL))
        return VAL_BAD_ARGUMENT;

    *ns_list = NULL;
    *pending_glue = NULL;

    /*
     * Look through the unchecked_zone stuff for NS records 
     */
    unchecked_set = unchecked_zone_info;
    while (unchecked_set != NULL) {
        if (unchecked_set->rrs_type_h == ns_t_ns &&
            (namecmp(zone_name, unchecked_set->rrs_name_n) == 0))
        {
                ns_rr = unchecked_set->rrs_data;
                while (ns_rr) {
                    /*
                     * Create the structure for the name server 
                     */
                    temp_ns = (struct name_server *)
                        MALLOC(sizeof(struct name_server));
                    if (temp_ns == NULL) {
                        /*
                         * Since we're in trouble, free up just in case 
                         */
                        free_name_servers(ns_list);
                        *ns_list = NULL;
                        return VAL_OUT_OF_MEMORY;
                    }

                    /*
                     * Make room for the name and insert the name 
                     */
                    name_len = wire_name_length(ns_rr->rr_rdata);
                    if (name_len > sizeof(temp_ns->ns_name_n)) {
                        free_name_servers(ns_list);
                        *ns_list = NULL;
                        return VAL_OUT_OF_MEMORY;
                    }
                    memcpy(temp_ns->ns_name_n, ns_rr->rr_rdata, name_len);

                    /*
                     * Initialize the rest of the fields 
                     */
                    temp_ns->ns_tsig = NULL;
                    temp_ns->ns_security_options = ZONE_USE_NOTHING;
                    temp_ns->ns_status = SR_ZI_STATUS_LEARNED;

                    temp_ns->ns_retrans = RES_TIMEOUT;
                    temp_ns->ns_retry = RES_RETRY;
                    temp_ns->ns_options = RES_DEFAULT;
                    /* 
                     * Depending on where we got the definition for RES_DEFAULT
                     * RES_RECURSE may or may not be set.
                     * Ensure that recursion is disabled by default 
                     */
                    if (temp_ns->ns_options & RES_RECURSE)
                        temp_ns->ns_options ^= RES_RECURSE;

                    temp_ns->ns_edns0_size = RES_EDNS0_DEFAULT;

                    temp_ns->ns_next = NULL;
                    temp_ns->ns_address = NULL;
                    temp_ns->ns_number_of_addresses = 0;
                    /*
                     * Add the name server record to the list 
                     */
                    if (*ns_list == NULL)
                        *ns_list = temp_ns;
                    else {
                        /*
                         * Preserving order in case of round robin 
                         */
                        tail_ns = *ns_list;
                        while (tail_ns->ns_next != NULL)
                            tail_ns = tail_ns->ns_next;
                        tail_ns->ns_next = temp_ns;
                    }
                    ns_rr = ns_rr->rr_next;
                }
        }
        unchecked_set = unchecked_set->rrs_next;
    }

    /*
     * Now, we need the addresses 
     */
    /*
     * This is ugly - loop through unchecked data for address records,
     * then through the name server records to find a match,
     * then through the (possibly multiple) addresses under the A set
     */

    unchecked_set = unchecked_zone_info;
    while (unchecked_set != NULL) {
        if (unchecked_set->rrs_type_h == ns_t_a ||
            unchecked_set->rrs_type_h == ns_t_aaaa) {
            /*
             * If the owner name matches the name in an *ns_list entry...
             */
            trail_ns = NULL;
            ns = *ns_list;
            while (ns) {
                if (namecmp
                    (unchecked_set->rrs_name_n,
                     ns->ns_name_n) == 0) {
                    /*
                     * Found that address set is for an NS 
                     */
                    if (VAL_NO_ERROR !=
                        (retval =
                         extract_glue_from_rdata(unchecked_set->
                                                 rrs_data, ns)))
                        return retval;
                    break;
                } else {
                    trail_ns = ns;
                    ns = ns->ns_next;
                }
            }
        }
        unchecked_set = unchecked_set->rrs_next;
    }

    ns = *ns_list;
    outer_trailer = NULL;
    *pending_glue = NULL;
    pending_glue_last = NULL;
    while (ns) {
        if (ns->ns_number_of_addresses == 0) {
            if (outer_trailer) {
                outer_trailer->ns_next = ns->ns_next;

                /*
                 * Add ns to the end of the pending_glue list 
                 */
                if (*pending_glue == NULL) {
                    *pending_glue = ns;
                    pending_glue_last = *pending_glue;
                } else {
                    pending_glue_last->ns_next = ns;
                    pending_glue_last = ns;
                }
                ns->ns_next = NULL;

                /*
                 * move to the next element 
                 */
                ns = outer_trailer->ns_next;
            } else {
                *ns_list = ns->ns_next;

                /*
                 * Add ns to the end of the pending_glue list 
                 */
                if (*pending_glue == NULL) {
                    *pending_glue = ns;
                    pending_glue_last = *pending_glue;
                } else {
                    pending_glue_last->ns_next = ns;
                    pending_glue_last = ns;
                }
                ns->ns_next = NULL;

                /*
                 * Move to the next element 
                 */
                ns = *ns_list;
            }
        } else {                /* There is at least one address */

            outer_trailer = ns;
            ns = ns->ns_next;
        }
    }


    return VAL_NO_ERROR;
}

/*
 * Identify the name servers where the query needs to be sent to 
 */
int
find_nslist_for_query(val_context_t * context,
                      struct queries_for_query *next_qfq,
                      struct queries_for_query **queries)
{
    /*
     * See if we can get an answer from a closer NS (from cache) 
     */
    struct name_server *ref_ns_list;
    int             ret_val;
    struct val_query_chain *next_q;
    u_char       *test_n;
    u_int16_t       tzonestatus;
    u_int32_t ttl_x = 0;
    struct name_server *ns;

    if (next_qfq == NULL)
        return VAL_BAD_ARGUMENT;
    
    next_q = next_qfq->qfq_query; /* Can never be NULL if next_qfq is not NULL */

    ref_ns_list = NULL;

    /* reuse existing name server information */
    if (next_q->qc_ns_list != NULL) {
        goto done;
    }

    if (next_q->qc_zonecut_n)
        FREE(next_q->qc_zonecut_n);
    next_q->qc_zonecut_n = NULL;

    if (context->nslist != NULL) {
        /* 
         * if we have a default name server in our resolv.conf file, send
         *  to that name server
         */
        clone_ns_list(&(next_q->qc_ns_list), context->nslist);
        goto done;
    } 

    ret_val = get_nslist_from_cache(context, next_qfq, queries, &ref_ns_list, &next_q->qc_zonecut_n);
    
    if (ret_val == VAL_NO_ERROR) {
        /* if any one is NULL, get rid of both */
        if (next_q->qc_zonecut_n == NULL) {
            free_name_servers(&ref_ns_list);
            ref_ns_list = NULL;
        } else if (ref_ns_list == NULL) {
            if (next_q->qc_zonecut_n)
                FREE(next_q->qc_zonecut_n);
            next_q->qc_zonecut_n = NULL;
        } else {
            next_q->qc_ns_list = ref_ns_list;
            goto done; 
        } 
    } 

    /*
     * work downward from root 
     */
    if (context->root_ns == NULL) {
        /*
         * No root hints; should not happen here 
         */
        val_log(context, LOG_WARNING, 
                "find_nslist_for_query(): Trying to answer query recursively, but no root hints file found.");
        return VAL_CONF_NOT_FOUND;
    }
    clone_ns_list(&next_q->qc_ns_list, context->root_ns);
    next_q->qc_zonecut_n = (u_char *) MALLOC(sizeof(u_char));
    if (next_q->qc_zonecut_n == NULL) {
        return VAL_OUT_OF_MEMORY;
    }
    *(next_q->qc_zonecut_n) = (u_char) '\0';

done:

    /*
     * Only set the CD and EDNS0 options if we feel the server 
     * is capable of handling DNSSEC
     */

    if (next_q->qc_ns_list) { 
        int edns0 = 0;
        /* if query is for some DNSSEC meta data then we always turn on EDNS0 */
        if (DNSSEC_METADATA_QTYPE(next_q->qc_type_h)) {
            edns0 = 1;
        } else if (!(next_q->qc_flags & VAL_QUERY_NO_EDNS0)) { 
            if (next_q->qc_zonecut_n != NULL)
                test_n = next_q->qc_zonecut_n;
            else
                test_n = next_q->qc_name_n;
                    
            /* check if the zone has a security expectation of validate */
            if (VAL_NO_ERROR != (ret_val = 
                    get_zse(context, test_n, next_qfq->qfq_flags, 
                            &tzonestatus, NULL, &ttl_x))) {
                    return ret_val;
            }
            SET_MIN_TTL(next_q->qc_ttl_x, ttl_x);

            if (tzonestatus == VAL_AC_WAIT_FOR_TRUST) {
                edns0 = 1;
            }
        }
        if (edns0) {
            val_log(context, LOG_DEBUG,
                    " find_nslist_for_query(): Setting D0 bit and using EDNS0");

            if (next_q->qc_flags & VAL_QUERY_NO_EDNS0) {
                next_q->qc_flags ^= VAL_QUERY_NO_EDNS0;
            }

            for (ns = next_q->qc_ns_list; ns; ns = ns->ns_next) {
                ns->ns_edns0_size = (context && context->g_opt)? 
                    context->g_opt->edns0_size : RES_EDNS0_DEFAULT;
                ns->ns_options |= RES_USE_DNSSEC;
            }
        }
    }
    return VAL_NO_ERROR;
}

/*
 * Identify next name servers in referral chain, 
 * issue queries for for missing glue
 */
int
bootstrap_referral(val_context_t *context,
                   u_char * referral_zone_n,
                   struct rrset_rec *learned_zones,
                   struct queries_for_query *matched_qfq,
                   struct queries_for_query **queries,
                   struct name_server **ref_ns_list)
{
    struct name_server *pending_glue;
    int             ret_val;
    struct queries_for_query *added_q = NULL;
    struct val_query_chain *matched_q;
    u_int32_t flags;

    if ((context == NULL) || (matched_qfq == NULL) ||
        (queries == NULL) || (ref_ns_list == NULL))
        return VAL_BAD_ARGUMENT;

    *ref_ns_list = NULL;
    matched_q = matched_qfq->qfq_query; /* Can never be NULL if matched_qfq is not NULL */

    /*
     *  If we received a referral for the root, use our 
     *  pre-parsed root.hints information 
     */
    if (!namecmp(referral_zone_n, (const u_char *)"\0")) {
        if (context->root_ns == NULL) {
            /*
             * No root hints; should not happen here 
             */
            val_log(context, LOG_WARNING, 
                    "bootstrap_referral(): referral to root, but no root hints file found.");
            matched_q->qc_state = Q_REFERRAL_ERROR;
            return VAL_NO_ERROR;
        }
        clone_ns_list(ref_ns_list, context->root_ns);
        matched_q->qc_state = Q_INIT;
        return VAL_NO_ERROR;
    }
    
    if ((ret_val =
         res_zi_unverified_ns_list(ref_ns_list, referral_zone_n,
                                   learned_zones, &pending_glue))
        != VAL_NO_ERROR) {
        return ret_val;
    }

    if (pending_glue != NULL) {
        /*
         * Don't fetch glue if we're already fetching glue 
         */
        if (matched_q->qc_state == Q_WAIT_FOR_GLUE && *ref_ns_list == NULL) {
            free_name_servers(&pending_glue);
            val_log(context, LOG_DEBUG, 
                    "bootstrap_referral(): Already fetching glue; not fetching again");
            matched_q->qc_state = Q_REFERRAL_ERROR;
            return VAL_NO_ERROR;
        }

        /*
         * Create a new referral if one does not exist 
         */
        if (matched_q->qc_referral == NULL) {
            ALLOCATE_REFERRAL_BLOCK(matched_q->qc_referral);
        } 
            
        matched_q->qc_referral->cur_pending_glue_ns = NULL;
        matched_q->qc_referral->pending_glue_ns = NULL;

        /* save a copy of the current zonecut in the delegation info */
        if(referral_zone_n) {
            size_t len = wire_name_length(referral_zone_n); 
            matched_q->qc_referral->saved_zonecut_n = 
                (u_char *) MALLOC (len * sizeof(u_char));
            if (matched_q->qc_referral->saved_zonecut_n == NULL) {
                free_name_servers(&pending_glue);
                free_name_servers(ref_ns_list);
                *ref_ns_list = NULL;
                return VAL_OUT_OF_MEMORY;
            }
            memcpy(matched_q->qc_referral->saved_zonecut_n,
                    referral_zone_n,
                    len);
        }

        if (*ref_ns_list != NULL) {
            matched_q->qc_referral->pending_glue_ns = pending_glue;
            matched_q->qc_state = Q_INIT;
        } else {
            matched_q->qc_referral->cur_pending_glue_ns = pending_glue;
            matched_q->qc_referral->pending_glue_ns = pending_glue->ns_next;
            pending_glue->ns_next = NULL;

            /*
             * Create a query for glue for pending_ns 
            */
            flags = matched_qfq->qfq_flags | VAL_QUERY_GLUE_REQUEST;

            if (VAL_NO_ERROR != (ret_val = add_to_qfq_chain(context,
                                       queries, pending_glue->ns_name_n, ns_t_a,
                                       ns_c_in, flags, &added_q)))
                return ret_val;

            matched_q->qc_state = Q_WAIT_FOR_GLUE;
        }
    } else if (*ref_ns_list != NULL) {
        matched_q->qc_state = Q_INIT;
    } 

    return VAL_NO_ERROR;
}

/*
 * Clean up delegation_info structure
 */
void
free_referral_members(struct delegation_info *del)
{
    if (del == NULL)
        return;

    if (del->queries != NULL) {
        deregister_queries(&del->queries);
        del->queries = NULL;
    }
    if (del->answers != NULL) {
        res_sq_free_rrset_recs(&del->answers);
        del->answers = NULL;
    }
    if (del->proofs != NULL) {
        res_sq_free_rrset_recs(&del->proofs);
        del->proofs = NULL;
    }
    if (del->qnames) {
        free_qname_chain(&del->qnames);
        del->qnames = NULL;
    }
    if (del->cur_pending_glue_ns) {
        free_name_servers(&del->cur_pending_glue_ns);
        del->cur_pending_glue_ns = NULL;
    }
    if (del->pending_glue_ns) {
        free_name_servers(&del->pending_glue_ns);
        del->pending_glue_ns = NULL;
    }
    if (del->saved_zonecut_n) {
        FREE(del->saved_zonecut_n);
        del->saved_zonecut_n = NULL;
    }
    if (del->learned_zones) {
        res_sq_free_rrset_recs(&del->learned_zones);
        del->learned_zones = NULL;
    }

}


/*
 * wrapper around the additional query logic for aliases and
 * referrals.
 */
static int
follow_referral_or_alias_link(val_context_t * context,
                              int alias_chain,
                              u_char * zone_n,
                              struct queries_for_query *matched_qfq,
                              struct rrset_rec **learned_zones,
                              struct qname_chain **qnames,
                              struct queries_for_query **queries,
                              struct rrset_rec **answers,
                              struct rrset_rec **proofs)
{
    int             ret_val;
    struct name_server *ref_ns_list;
    size_t             len;
    u_char       *referral_zone_n;
    struct queries_for_query *added_q = NULL;
    struct val_query_chain *matched_q;
    u_int16_t       tzonestatus;
    u_int32_t       ttl_x = 0;

    if ((matched_qfq == NULL)  || (qnames == NULL) ||
        (learned_zones == NULL) || (queries == NULL) || (answers == NULL) ||
        (proofs == NULL))
        return VAL_BAD_ARGUMENT;
    
    ref_ns_list = NULL;
    referral_zone_n = zone_n;
    matched_q = matched_qfq->qfq_query; /* Can never be NULL if matched_qfq is not NULL */

    if (matched_q->qc_respondent_server) {
        free_name_server(&matched_q->qc_respondent_server);
        matched_q->qc_respondent_server = NULL;
    }
    if (matched_q->qc_ns_list) {
        free_name_servers(&matched_q->qc_ns_list);
        matched_q->qc_ns_list = NULL;
    }

    if (matched_q->qc_referral == NULL) {
        ALLOCATE_REFERRAL_BLOCK(matched_q->qc_referral);
    } else if (alias_chain && matched_q->qc_referral->queries) {
        /* free up the old set of registered queries and start afresh */
        deregister_queries(&matched_q->qc_referral->queries);
        matched_q->qc_referral->queries = NULL;
    } 

    /*
     * Consume qnames 
     */
    if (matched_q->qc_referral->qnames == NULL)
        matched_q->qc_referral->qnames = *qnames;
    else if (*qnames) {
        struct qname_chain *t_q;
        for (t_q = *qnames; t_q->qnc_next; t_q = t_q->qnc_next);
        t_q->qnc_next = matched_q->qc_referral->qnames;
        matched_q->qc_referral->qnames = *qnames;
    }
    *qnames = NULL;

    /*
     * Consume answers
     */
    merge_rrset_recs(&matched_q->qc_referral->answers, *answers);
    *answers = NULL;

    matched_q->qc_state = Q_INIT;

    if (alias_chain) {
        /*
         * Consume proofs only if they pertain to cname/dname chains
         */
        merge_rrset_recs(&matched_q->qc_referral->proofs, *proofs);
        *proofs = NULL;

        /* don't believe any of the cname hints that were provided */
        res_sq_free_rrset_recs(learned_zones);
        *learned_zones = NULL;

#if 0
        /* set EDNS0 if we believe the name can be validated */
        if (matched_q->qc_flags & VAL_QUERY_NO_EDNS0) {
            if (VAL_NO_ERROR != (ret_val =
                get_zse(context, matched_q->qc_name_n, matched_qfq->qfq_flags, 
                        &tzonestatus, NULL, &ttl_x))) { 
                return ret_val;
            }
            SET_MIN_TTL(matched_q->qc_ttl_x, ttl_x);
            if (tzonestatus == VAL_AC_WAIT_FOR_TRUST) { 
                matched_q->qc_flags ^= VAL_QUERY_NO_EDNS0;
                matched_q->qc_state = Q_INIT;
            }
        }
#endif

        return VAL_NO_ERROR;
    } 

    res_sq_free_rrset_recs(proofs);
    *proofs = NULL;

    if (referral_zone_n) {
        char            debug_name1[NS_MAXDNAME];
        char            debug_name2[NS_MAXDNAME];
        memset(debug_name1, 0, 1024);
        memset(debug_name2, 0, 1024);
        if(ns_name_ntop(matched_q->qc_name_n, debug_name1,
            sizeof(debug_name1)) < 0) {
            strncpy(debug_name1, "unknown/error", sizeof(debug_name1)-1);
        } 
        if (ns_name_ntop(referral_zone_n, debug_name2, sizeof(debug_name2))
                    < 0) {
            strncpy(debug_name2, "unknown/error", sizeof(debug_name2)-1);
        }
            
        val_log(context, LOG_DEBUG, 
                "follow_referral_or_alias_link(): Processing referral to %s for query {%s %d %d})", 
                debug_name2, debug_name1, matched_q->qc_class_h, matched_q->qc_type_h);
    }

    /*
     * Register the request name and zone with our referral monitor
     */
    if (register_query(&matched_q->qc_referral->queries, matched_q->qc_name_n,
            matched_q->qc_type_h, referral_zone_n) == ITS_BEEN_DONE) {
        /*
         * If this request has already been made then Referral Error
         */
        val_log(context, LOG_DEBUG, "follow_referral_or_alias_link(): Referral loop encountered");
        matched_q->qc_state = Q_REFERRAL_ERROR;
        return VAL_NO_ERROR;
    }

    if (VAL_NO_ERROR != (ret_val = bootstrap_referral(context,
                                                    referral_zone_n,
                                                    *learned_zones,
                                                    matched_qfq,
                                                    queries,
                                                    &ref_ns_list))) 
        return ret_val;

    if (matched_q->qc_state == Q_WAIT_FOR_GLUE) {
        matched_q->qc_referral->learned_zones = *learned_zones;        
    } else if (VAL_NO_ERROR != (ret_val = stow_zone_info(learned_zones, matched_q))) {
        res_sq_free_rrset_recs(learned_zones);
        *learned_zones = NULL;
        return ret_val;
    }
    *learned_zones = NULL; /* consumed */

    if (ref_ns_list == NULL && (matched_q->qc_state != Q_WAIT_FOR_GLUE)) {
        /*
         * nowhere to look 
         */
        val_log(context, LOG_DEBUG, "follow_referral_or_alias_link(): Missing glue");
        matched_q->qc_state = Q_MISSING_GLUE;
        return VAL_NO_ERROR;
    }

    if (matched_q->qc_zonecut_n && 
        !(matched_q->qc_flags & VAL_QUERY_DONT_VALIDATE)) {

        u_char *tp = NULL;

        if (VAL_NO_ERROR != (ret_val =
                get_zse(context, matched_q->qc_zonecut_n, matched_qfq->qfq_flags, 
                        &tzonestatus, NULL, &ttl_x))) { 
            return ret_val;
        }
        SET_MIN_TTL(matched_q->qc_ttl_x, ttl_x);

        ttl_x = 0;
        if (VAL_NO_ERROR != (ret_val = 
                find_trust_point(context, matched_q->qc_zonecut_n, 
                                 &tp, &ttl_x))) {
            return ret_val;
        }
        SET_MIN_TTL(matched_q->qc_ttl_x, ttl_x);

        /* 
         * If the trust point for the zone we are entering into 
         * is different from the zone we're at
         * ensure that we reset the VAL_QUERY_NO_EDNS0 status
         */
        if (tp && (matched_q->qc_flags & VAL_QUERY_NO_EDNS0)) {
            u_char *tp_ref = NULL;
            if (VAL_NO_ERROR != (ret_val = 
                find_trust_point(context, referral_zone_n, 
                                 &tp_ref, &ttl_x))) {
                return ret_val;
            }
            SET_MIN_TTL(matched_q->qc_ttl_x, ttl_x);

            if (tp_ref && namecmp(tp, tp_ref)) { 
                val_log(context, LOG_DEBUG, 
                        "follow_referral_or_alias_link(): Re-enabling EDNS0 where previously disabled");
                matched_q->qc_flags ^= VAL_QUERY_NO_EDNS0;
            }
            if (tp_ref) {
                FREE(tp_ref);
            } 
        }

        if (tp && tzonestatus == VAL_AC_WAIT_FOR_TRUST) {
            
            /*
             * Fetch DNSSEC meta-data in parallel 
             */
            
            if(VAL_NO_ERROR != 
                    (ret_val = add_to_qfq_chain(context, queries, 
                                            matched_q->qc_zonecut_n, ns_t_dnskey,
                                            ns_c_in, matched_qfq->qfq_flags, &added_q)))
                return ret_val;

            /* fetch DS only if are about to enter a zone that is below the trust point */    
            if (referral_zone_n && namename(referral_zone_n, tp) != NULL) {
                
                if (VAL_NO_ERROR != 
                        (ret_val = add_to_qfq_chain(context, queries, 
                                                    referral_zone_n, ns_t_ds,
                                                    ns_c_in, matched_qfq->qfq_flags, 
                                                    &added_q)))
                    return ret_val;
            } 
        }

        if (tp) {
            FREE(tp);
        } 
    }
    /*
     * Store the current referral value in the query 
     */
    if (matched_q->qc_zonecut_n != NULL) {
        FREE(matched_q->qc_zonecut_n);
        matched_q->qc_zonecut_n = NULL;
    }
    if (referral_zone_n != NULL) {
        len = wire_name_length(referral_zone_n);
        matched_q->qc_zonecut_n =
                (u_char *) MALLOC(len * sizeof(u_char));
        if (matched_q->qc_zonecut_n == NULL)
            return VAL_OUT_OF_MEMORY;
        memcpy(matched_q->qc_zonecut_n, referral_zone_n, len);
    }
    matched_q->qc_ns_list = ref_ns_list;

    return VAL_NO_ERROR;
}


#define SAVE_RR_TO_LIST(respondent_server, listtype, name_n, type_h,    \
                        set_type_h, class_h, ttl_h, hptr, rdata,        \
                        rdata_len_h, from_section, authoritive, zonecut_n) \
    do {                                                                \
        struct rrset_rec *rr_set;                                       \
        int ret_val;                                                    \
        u_char *r;                                                      \
        if (authoritive)                                                \
            rr_set = find_rr_set (respondent_server, listtype, name_n,  \
                              type_h, set_type_h, class_h, ttl_h, hptr, \
                              rdata, from_section, authoritive,         \
                              zonecut_n);                               \
        else                                                            \
            rr_set = find_rr_set (respondent_server, listtype, name_n,  \
                              type_h, set_type_h, class_h, ttl_h, hptr, \
                              rdata, from_section, authoritive,         \
                              NULL);                                    \
        if (rr_set==NULL) {                                             \
            ret_val = VAL_OUT_OF_MEMORY;                                \
        }                                                               \
        else {                                                          \
            if (type_h != ns_t_rrsig) {                                 \
                /* Add this record to its chain of val_rr_rec's. */     \
                ret_val = add_to_set(rr_set,rdata_len_h,rdata);         \
            } else if (VAL_NO_ERROR ==                                  \
                    (ret_val = add_as_sig(rr_set,rdata_len_h,rdata))) { \
                /* Add this record to the sig of rrset_rec (above). */  \
                /* and override the zonecut using the rrsig info */     \
                if (rr_set->rrs_zonecut_n) {                            \
                    FREE(rr_set->rrs_zonecut_n);                        \
                    rr_set->rrs_zonecut_n = NULL;                       \
                }                                                       \
                if ((rdata_len_h > SIGNBY) &&                           \
                     NULL != (r = namename(name_n, &rdata[SIGNBY]))) {  \
                    rr_set->rrs_zonecut_n =                             \
                        (u_char *) MALLOC (sizeof(u_char) *             \
                                wire_name_length(r));                    \
                    memcpy(rr_set->rrs_zonecut_n, r, wire_name_length(r));\
                }                                                       \
            }                                                           \
        }                                                               \
        if (ret_val != VAL_NO_ERROR) {                                  \
            res_sq_free_rrset_recs(&learned_zones);                     \
            FREE(rdata);                                                \
            return ret_val;                                             \
        }                                                               \
    } while (0)

#define FIX_ZONECUT(the_rrset, zonecut_n, retval) do {                  \
        struct rrset_rec *cur_rrset;                                    \
        size_t len = wire_name_length(zonecut_n);                          \
        retval = VAL_NO_ERROR;                                          \
        for (cur_rrset = the_rrset; cur_rrset;                          \
                cur_rrset=cur_rrset->rrs_next){                         \
            if (cur_rrset->rrs_zonecut_n)                               \
                FREE(cur_rrset->rrs_zonecut_n);                         \
            cur_rrset->rrs_zonecut_n =                                  \
                    (u_char *) MALLOC(len * sizeof(u_char));        \
            if (cur_rrset->rrs_zonecut_n == NULL) {                     \
                retval = VAL_OUT_OF_MEMORY;                             \
                break;                                                  \
            } else {                                                    \
                memcpy(cur_rrset->rrs_zonecut_n, zonecut_n, len);       \
            }                                                           \
        }                                                               \
    } while (0)

/*
 * Check if CNAME or DNAME chain are invalid or contain a loop
 */
int
process_cname_dname_responses(u_char *name_n, 
                              u_int16_t type_h, 
                              u_char *rdata, 
                              struct val_query_chain *matched_q,
                              struct qname_chain **qnames,
                              int *referral_error)
{
    u_char        temp_name[NS_MAXCDNAME];
    u_char       *p;
    int ret_val;

    if (!name_n || !rdata || !matched_q || 
            !qnames || !(*qnames) )
       return VAL_BAD_ARGUMENT; 
    
    if (referral_error)
        *referral_error = 0;
    
    if (type_h == ns_t_cname &&
        matched_q->qc_type_h != ns_t_cname &&
        namecmp((*qnames)->qnc_name_n, name_n) == 0) {
        
        /*
         * add the target 
         */
        if ((ret_val = add_to_qname_chain(qnames, rdata)) != VAL_NO_ERROR)
            return ret_val; 
        if (!matched_q->qc_referral)
            ALLOCATE_REFERRAL_BLOCK(matched_q->qc_referral);
            
        if (register_query(&matched_q->qc_referral->queries,
                           rdata,
                           matched_q->qc_type_h,
                           matched_q->qc_zonecut_n) == ITS_BEEN_DONE) {
            /*
             * If this request has already been made then Referral Error
             */
            matched_q->qc_state = Q_REFERRAL_ERROR;
            if (referral_error)
                *referral_error = 1;
            return VAL_NO_ERROR;
        }
        matched_q->qc_state = Q_INIT;
    }

    u_char  *qname_n = (*qnames)->qnc_name_n;
    if (type_h == ns_t_dname &&
        matched_q->qc_type_h != ns_t_dname &&
        namecmp(qname_n, name_n) != 0 &&
        NULL != (p = namename(qname_n, name_n)) &&
        p > qname_n) {

        u_char *qname_n = (*qnames)->qnc_name_n;
        size_t len1 = p - qname_n;
        size_t len2 = wire_name_length(rdata);
        if (len1 + len2 > sizeof(temp_name)) {
            matched_q->qc_state = Q_REFERRAL_ERROR;
            if (referral_error)
                *referral_error = 1;
            return VAL_NO_ERROR;
        }
        if (len1 > 0) {
            /*
             * add the DNAME owner name 
             */
            if ((ret_val = add_to_qname_chain(qnames, name_n)) != VAL_NO_ERROR)
                return ret_val; 
            if (!matched_q->qc_referral)
                ALLOCATE_REFERRAL_BLOCK(matched_q->qc_referral);

            if (register_query(&matched_q->qc_referral->queries,
                               name_n,
                               matched_q->qc_type_h,
                               matched_q->qc_zonecut_n) == ITS_BEEN_DONE) {
                /*
                 * If this request has already been made then Referral Error
                 */
                matched_q->qc_state = Q_REFERRAL_ERROR;
                if (referral_error)
                    *referral_error = 1;
                return VAL_NO_ERROR;
            }
        }
        /*
         * add the target 
         */
        memcpy(temp_name, qname_n, len1);
        memcpy(&temp_name[len1], rdata, len2);
        if ((ret_val = add_to_qname_chain(qnames,
                                        temp_name)) != VAL_NO_ERROR) {
           return ret_val; 
        }

        matched_q->qc_state = Q_INIT;
    }

    if (*qnames) {

        if (namecmp(matched_q->qc_name_n, (*qnames)->qnc_name_n))
            /*
             * Keep the current query name as the last name in the chain 
             */
            memcpy(matched_q->qc_name_n, (*qnames)->qnc_name_n,
                   wire_name_length((*qnames)->qnc_name_n));
    }

    return VAL_NO_ERROR;
}

/*
 * Create error rrset_rec structure
 */
static int
prepare_empty_nonexistence(struct rrset_rec **answers,
                       struct name_server *respondent_server,
                       u_char * query_name_n,
                       u_int16_t query_type_h, u_int16_t query_class_h,
                       u_char *hptr)
{
    size_t          length = wire_name_length(query_name_n);

    if (answers == NULL || length == 0)
        return VAL_BAD_ARGUMENT;

    *answers = (struct rrset_rec *) MALLOC(sizeof(struct rrset_rec));
    if (*answers == NULL)
        return VAL_OUT_OF_MEMORY;

    (*answers)->rrs_zonecut_n = NULL;
    (*answers)->rrs_name_n = (u_char *) MALLOC(length * sizeof(u_char));

    if ((*answers)->rrs_name_n == NULL) {
        FREE(*answers);
        *answers = NULL;
        return VAL_OUT_OF_MEMORY;
    }

    if (hptr) {
        (*answers)->rrs_rcode = ((HEADER *)hptr)->rcode;
    } else {
        (*answers)->rrs_rcode = 0; 
    }
    memcpy((*answers)->rrs_name_n, query_name_n, length);
    (*answers)->rrs_type_h = query_type_h;
    (*answers)->rrs_class_h = query_class_h;
    (*answers)->rrs_ttl_h = 0;/* don't have any basis to set the TTL value */
    (*answers)->rrs_cred = SR_CRED_UNSET;
    (*answers)->rrs_section = VAL_FROM_UNSET;
    if ((respondent_server) &&
        (respondent_server->ns_number_of_addresses > 0)) {
        (*answers)->rrs_server =
            (struct sockaddr *) MALLOC(sizeof(struct sockaddr_storage));
        if ((*answers)->rrs_server == NULL) {
            FREE((*answers)->rrs_name_n);
            FREE(*answers);
            *answers = NULL;
            return VAL_OUT_OF_MEMORY;
        }
        memcpy((*answers)->rrs_server,
               respondent_server->ns_address[0],
               sizeof(struct sockaddr_storage));
    } else {
        (*answers)->rrs_server = NULL;
    }
    (*answers)->rrs_data = NULL;
    (*answers)->rrs_sig = NULL;
    (*answers)->rrs_next = NULL;

    return VAL_NO_ERROR;
}

static void consume_referral_data(struct delegation_info **qc_referral, 
                                  struct domain_info *di_response,
                                  struct qname_chain **qnames) {

    if (di_response == NULL || qc_referral == NULL || 
            *qc_referral == NULL || qnames == NULL)
        return;

    /*
     * Consume answers
     */
    merge_rrset_recs(&((*qc_referral)->answers), di_response->di_answers);
    di_response->di_answers = (*qc_referral)->answers;
    (*qc_referral)->answers = NULL;

    /*
     * Consume proofs
     */
    merge_rrset_recs(&((*qc_referral)->proofs), di_response->di_proofs);
    di_response->di_proofs = (*qc_referral)->proofs;
    (*qc_referral)->proofs = NULL;

    /*
     * Consume qnames
     */
    if (*qnames == NULL)
        *qnames = (*qc_referral)->qnames;
    else if ((*qc_referral)->qnames) {
        struct qname_chain *t_q;
        for (t_q = *qnames; t_q->qnc_next; t_q = t_q->qnc_next);
            t_q->qnc_next = (*qc_referral)->qnames;
    }
    (*qc_referral)->qnames = NULL;

    /*
     * Note that we don't free qc_referral here 
     */
    free_referral_members(*qc_referral);
    qc_referral = NULL;
}

/*
 * The main routine for processing response data
 *  
 * RRsets in the answer and authority sections are extracted as is any
 * referral information present in the additional section of the response. Any
 * DS or DNSKEY information is also cached for future use. 
 * The validator follows any referrals or aliases that are returned as part of
 * the response and issues queries to fetch any missing glue. Information gathered
 * as part of following referrals are maintained separately within the val_query_chain
 * structure. Once the referral operation completes, all information within
 * this entry are merged into the validator cache.
 * 
 * The validator keeps track of nameservers that it actually used while following
 * referrals.  These are re-used in future requests for data in the same zone.
 */
static int
digest_response(val_context_t * context,
                struct queries_for_query *matched_qfq,
                struct queries_for_query **queries,
                u_char * response_data,
                size_t response_length, 
                struct domain_info *di_response)
{
    u_int16_t       question, answer, authority, additional;
    u_int16_t       rrs_to_go;
    int             i;
    size_t          response_index;
    u_char          name_n[NS_MAXCDNAME];
    u_int16_t       type_h;
    u_int16_t       set_type_h;
    u_int16_t       class_h;
    u_int32_t       ttl_h;
    size_t          rdata_len_h;
    size_t          rdata_index;
    int             authoritive = 0;
    u_char         *rdata;
    u_char         *hptr;
    int             ret_val;
    int             nothing_other_than_alias;
    int             from_section;
    struct rrset_rec *learned_zones = NULL;
    struct rrset_rec *learned_answers = NULL;
    struct rrset_rec *learned_proofs = NULL;

    u_char *query_name_n;
    u_int16_t       query_type_h;
    u_int16_t       query_class_h;
    u_char         *rrs_zonecut_n = NULL;
    int             referral_seen = FALSE;
    u_char          referral_zone_n[NS_MAXCDNAME];
    int             proof_seen = 0;
    int             soa_seen = 0;
    HEADER         *header;
    u_char         *end;
    size_t         qnamelen, tot;
    size_t len;
    struct qname_chain **qnames;
    int             zonecut_was_modified = 0;
    struct val_query_chain *matched_q;
    u_int16_t tzonestatus;
    u_int32_t ttl_x;
    char query_name_p[NS_MAXDNAME];
    char rrs_zonecut_p[NS_MAXDNAME];
    char name_p[NS_MAXDNAME];

    if ((matched_qfq == NULL) || (queries == NULL) ||
        (di_response == NULL) || (response_data == NULL))
        return VAL_BAD_ARGUMENT;

    matched_q = matched_qfq->qfq_query; /* Can never be NULL if matched_qfq is not NULL */
    
    qnames = &(di_response->di_qnames);
    header = (HEADER *) response_data;
    end = response_data + response_length;

    query_name_n = matched_q->qc_name_n;
    query_type_h = matched_q->qc_type_h;
    query_class_h = matched_q->qc_class_h;
    *qnames = NULL;
    di_response->di_answers = NULL; 
    di_response->di_proofs = NULL;
    hptr = NULL;
    rdata = NULL;

    question = ntohs(header->qdcount);
    answer = ntohs(header->ancount);
    authority = ntohs(header->nscount);
    additional = ntohs(header->arcount);

    if (answer == 0)
        nothing_other_than_alias = 0;
    else
        nothing_other_than_alias = 1;

    /*
     * Add the query name to the chain of acceptable 
     * names in the response 
     */
    if ((ret_val =
         add_to_qname_chain(qnames, query_name_n)) != VAL_NO_ERROR)
        return ret_val;
    strcpy(query_name_p, "");
    if (query_name_n && 
            ns_name_ntop(query_name_n, query_name_p, sizeof(query_name_p)) == -1) {
       ret_val =  VAL_BAD_ARGUMENT;
       goto done;
    }

    /*
     * Extract zone cut from the query chain element if it exists 
     */
    rrs_zonecut_n = matched_q->qc_zonecut_n;
    strcpy(rrs_zonecut_p, "");
    if (rrs_zonecut_n && 
            ns_name_ntop(rrs_zonecut_n, rrs_zonecut_p, sizeof(rrs_zonecut_p)) == -1) {
       ret_val =  VAL_BAD_ARGUMENT;
       goto done;
    }

    val_log(context, LOG_DEBUG, 
            "digest_response(): Processing response for {%s %d %d} from zonecut: %s",
            query_name_p, query_class_h, query_type_h, rrs_zonecut_p); 

    /*
     *  Skip question section 
     */
    if (response_length <= sizeof(HEADER)) {
        response_index = 0;
    } else {
        qnamelen = wire_name_length(&response_data[sizeof(HEADER)]);
        tot = sizeof(HEADER) + qnamelen + sizeof(u_int32_t);
        if (tot <= response_length) {
            hptr = response_data;
            response_index = tot;
        } else
            response_index = 0;
    }

    rrs_to_go = answer + authority + additional;

    if (rrs_to_go == 0 || (rrs_to_go == additional)) {
        if (!matched_q->qc_respondent_server) {
            matched_q->qc_state = Q_RESPONSE_ERROR; 
            goto done;
        }
        /*
         * We got a response with no records 
         * or we got only additional section data (e.g. EDNS0 opt records)
         * Check if EDNS was not used when it should have
         */
        if (matched_q->qc_flags & VAL_QUERY_NO_EDNS0) { 
            if (VAL_NO_ERROR != (ret_val =
                get_zse(context, query_name_n, matched_qfq->qfq_flags, 
                        &tzonestatus, NULL, &ttl_x))) { 
                return ret_val;
            }
            SET_MIN_TTL(matched_q->qc_ttl_x, ttl_x);
            if (tzonestatus == VAL_AC_WAIT_FOR_TRUST) { 
                matched_q->qc_flags ^= VAL_QUERY_NO_EDNS0;
                matched_q->qc_state = Q_INIT;
                ret_val = VAL_NO_ERROR;
                goto done;
            }
        }

        /*
         * Else this is a non-existence result
         * Type is decided by the rcode, which we will check later
         */
        matched_q->qc_state = Q_ANSWERED;
        ret_val = prepare_empty_nonexistence(&di_response->di_proofs, 
                        matched_q->qc_respondent_server,
                        query_name_n, query_type_h, query_class_h, hptr);
        /*
         * copy any answers that may be present in the referral structure
         * E.g. if this response was returned after following a CNAME/DNAME
         */
        if (matched_q->qc_referral != NULL) {
            consume_referral_data(&matched_q->qc_referral, di_response, qnames);
        } 
        goto done; 
    }

    /*
     * Now start processing each RRSet in the response
     */
    for (i = 0; i < rrs_to_go; i++) {

        rdata = NULL;

        /*
         * Determine what part of the response I'm reading 
         */
        if (i < answer)
            from_section = VAL_FROM_ANSWER;
        else if (i < answer + authority)
            from_section = VAL_FROM_AUTHORITY;
        else
            from_section = VAL_FROM_ADDITIONAL;

        /*
         * Response_index points to the beginning of an RR 
         * Grab the uncompressed name, type, class, ttl, rdata_len 
         * If the type is a signature, get the type_covered 
         * Leave a pointer to the rdata 
         * Advance the response_index 
         */
        if ((ret_val =
             extract_from_rr(response_data, &response_index, end, name_n,
                             &type_h, &set_type_h, &class_h, &ttl_h,
                             &rdata_len_h, &rdata_index)) != VAL_NO_ERROR) {
            goto done;
        }

        /*
         * response[rdata_index] is the first byte of the RDATA of the
         * record.  The data may contain domain names in compressed format,
         * so they need to be expanded.  This is type-dependent...
         */
        if ((ret_val =
             decompress(&rdata, response_data, rdata_index, end, type_h,
                        &rdata_len_h)) != VAL_NO_ERROR) {
            goto done;
        }

        /*
         * we're only authoritative for the first name in the
         * CNAME chain
         */
        authoritive = (header->aa == 1)
            && qname_chain_first_name(*qnames, name_n);

        /*
         * If it is from the answer section, it may be an alias 
         * If it is from the authority section, it may be a proof or a referral 
         * If it is from the additional section it may contain some DNSSEC meta-data or it may be glue
         */
        if (from_section == VAL_FROM_ANSWER && (authoritive || (header->rd && header->ra))) {
            if (nothing_other_than_alias) {
                /*
                 * check if we received a non-alias or had explicitly asked for this alias 
                 */
                if ((set_type_h != ns_t_cname && set_type_h != ns_t_dname) ||
                    ((query_type_h == ns_t_cname) && (set_type_h == ns_t_cname)) ||
                    ((query_type_h == ns_t_dname) && (set_type_h == ns_t_dname)) ||
                    (query_type_h == ns_t_any) ||
                    (query_type_h == ns_t_rrsig)) {

                    nothing_other_than_alias = 0;
                } else if (ALIAS_MATCH_TYPE(query_type_h)){
                    int referral_error = 0;
                    /* process CNAMEs or DNAMEs if they exist */
                    if ((VAL_NO_ERROR != (ret_val = 
                        process_cname_dname_responses(name_n, type_h, rdata, 
                                                  matched_q, qnames, 
                                                  &referral_error))) || 
                            (referral_error)) {
                        if (referral_error) 
                            val_log(context, LOG_DEBUG, "digest_response(): CNAME/DNAME error or loop encountered");
                        goto done;
                    }
                } else {
                    nothing_other_than_alias = 0;
                }
            }
            SAVE_RR_TO_LIST(matched_q->qc_respondent_server, 
                            &learned_answers, name_n, type_h,
                            set_type_h, class_h, ttl_h, hptr, rdata,
                            rdata_len_h, from_section, authoritive,
                            rrs_zonecut_n);
        } else if (from_section == VAL_FROM_AUTHORITY) {
            if ((set_type_h == ns_t_nsec)
#ifdef LIBVAL_NSEC3
                    || (set_type_h == ns_t_nsec3)
#endif
                    || (set_type_h == ns_t_soa)) {

                proof_seen = 1;
                if (set_type_h == ns_t_soa) {
                    soa_seen = 1;
                }

                SAVE_RR_TO_LIST(matched_q->qc_respondent_server, 
                                &learned_proofs, name_n, type_h,
                                set_type_h, class_h, ttl_h, hptr, rdata,
                                rdata_len_h, from_section, authoritive,
                                rrs_zonecut_n);
            } else if (set_type_h == ns_t_ns) {
                /* 
                 * The zonecut information for name servers is 
                 * their respective owner name 
                 */
                SAVE_RR_TO_LIST(matched_q->qc_respondent_server, 
                                &learned_zones, name_n,
                                type_h, set_type_h, class_h, ttl_h, hptr,
                                rdata, rdata_len_h, from_section,
                                authoritive, name_n);
            }
        } else if (from_section == VAL_FROM_ADDITIONAL) {
            if (set_type_h == ns_t_dnskey || set_type_h == ns_t_ds) {
                SAVE_RR_TO_LIST(matched_q->qc_respondent_server, 
                                &learned_answers, name_n,
                                type_h, set_type_h, class_h, ttl_h, hptr,
                                rdata, rdata_len_h, from_section,
                                authoritive, rrs_zonecut_n);
            } else if (set_type_h == ns_t_a || set_type_h == ns_t_aaaa) {
                SAVE_RR_TO_LIST(matched_q->qc_respondent_server, 
                                &learned_zones, name_n,
                                type_h, set_type_h, class_h, ttl_h, hptr,
                                rdata, rdata_len_h, from_section,
                                authoritive, name_n);
            }
        }

        /*
         * If we're asking for a DS and have got an SOA with the same name
         * the name server likely does not understand DNSSEC
         */
        if ( query_type_h == ns_t_ds &&
             set_type_h == ns_t_soa &&
             !namecmp(name_n, query_name_n)) {
            val_log(context, LOG_DEBUG, "digest_response(): bad response for DS record. NS probably not DNSSEC-capable.");
            matched_q->qc_state = Q_WRONG_ANSWER;
            ret_val = VAL_NO_ERROR;
            goto done;
        }
        /*
         * If we've received a CNAME/DNAME response for a type that cannot be followed using cnames/dnames
         * we're not going to follow it
         */
        if ((set_type_h == ns_t_cname || set_type_h == ns_t_dname) &&
             !ALIAS_MATCH_TYPE(query_type_h)) {
            val_log(context, LOG_DEBUG, 
                    "digest_response(): Won't follow alias for type %d.", 
                    query_type_h);
            matched_q->qc_state = Q_WRONG_ANSWER;
            ret_val = VAL_NO_ERROR;
            goto done;
        }

        /*
         * if we have an SOA in the ans/auth section
         *  or if we have a DNSKEY in the ans section
         *  or if we have an NS in the ans/auth section with answer > 0 or a noerror error code
         * AND if owner name is more specific than current zonecut 
         * we must use the provided zone cut hint
         * we need this if the parent is also authoritative for the child or if
         * recursion is enabled on the parent zone.
         * Although we may end up "fixing" the zone cut for even out-of-zone
         * data (think of out-of-bailiwick glue), these records will not
         * be saved because of the anti-pollution rules.
         */
        int fix_zonecut = 0;
        if (header->aa == 1 &&
            i < (answer + authority) &&
            (set_type_h == ns_t_soa || 
             (set_type_h == ns_t_dnskey && i < answer) ||
             (set_type_h == ns_t_ns && 
              (answer > 0 || header->rcode != ns_r_noerror)))) {

            fix_zonecut = 1;
        }
           
        /* check if this is a lame delegation */
        if (fix_zonecut &&
            set_type_h == ns_t_soa && 
            rrs_zonecut_n && 
            /* new zonecut is not the same as the old */
            namecmp(rrs_zonecut_n, name_n) &&
            /* old zonecut is closer more specific than the new zonecut */
            (namename(rrs_zonecut_n, name_n) != NULL)) { 

            val_log(context, LOG_DEBUG, "digest_response(): {%s %d %d} appears to lead to a lame server",
                    query_name_p, query_class_h, query_type_h);
            matched_q->qc_state = Q_REFERRAL_ERROR;
            ret_val = VAL_NO_ERROR;
            goto done;
        }

        /* make sure that our new zonecut is closer */
        if (fix_zonecut &&
              (rrs_zonecut_n == NULL 
               || NULL != namename(name_n, rrs_zonecut_n))) {

            /* 
             *  special case for DS record: zonecut cannot be the same or larger 
             *  than the queried name
             */
            if (query_type_h == ns_t_ds &&
                NULL != namename (name_n, query_name_n)) {
                fix_zonecut = 0;
                val_log(context, LOG_DEBUG, "digest_response(): bad response for DS record. NS probably not DNSSEC-capable.");
                matched_q->qc_state = Q_WRONG_ANSWER;
                ret_val = VAL_NO_ERROR;
                goto done;
            } else if (nothing_other_than_alias) {
                /* 
                 * make sure that the NS is closer to the alias than the target; 
                 * if not don't fix the zonecut
                 */
                u_char *n1 = NULL;
                u_char *n2 = NULL;

                if (*qnames) {
                    struct qname_chain *q = *qnames;

                    /* Compare with target */
                    n1 = namename(name_n, q->qnc_name_n); 

                    while(q->qnc_next) {
                        q = q->qnc_next;
                    }
                    /* compare with alias */
                    n2 = namename(name_n, q->qnc_name_n);
                }

               if (!n1 || !n2 || namename(n1, n2))
                   fix_zonecut = 0;
            } 
            if (fix_zonecut && !zonecut_was_modified) {

                zonecut_was_modified = 1;

                /* don't do anything if our existing zonecut matches the new one  */
                if (!matched_q->qc_zonecut_n ||
                    namecmp(matched_q->qc_zonecut_n, name_n)) {

                    /* update the zonecut information */
                    if (matched_q->qc_zonecut_n) 
                        FREE(matched_q->qc_zonecut_n);
                    len = wire_name_length(name_n);
                    matched_q->qc_zonecut_n = 
                        (u_char *) MALLOC (len * sizeof(u_char));
                    if (matched_q->qc_zonecut_n == NULL)
                        goto done;    
                    memcpy (matched_q->qc_zonecut_n, name_n, len);
                    rrs_zonecut_n = matched_q->qc_zonecut_n;
    
                    if (rrs_zonecut_n && 
                            ns_name_ntop(rrs_zonecut_n, rrs_zonecut_p, sizeof(rrs_zonecut_p)) == -1) {
                        ret_val =  VAL_BAD_ARGUMENT;
                        goto done;
                    }

                    val_log(context, LOG_DEBUG, 
                        "digest_response(): Setting zonecut for {%s %d %d} query responses to %s",
                        query_name_p, query_class_h, query_type_h, rrs_zonecut_p);
                    /*
                     * go back to all the rrsets that we created 
                     * and fix the zonecut info 
                     */
                    FIX_ZONECUT(learned_answers, rrs_zonecut_n, ret_val);
                    if (ret_val != VAL_NO_ERROR)
                        goto done;
                    FIX_ZONECUT(learned_proofs, rrs_zonecut_n, ret_val);
                    if (ret_val != VAL_NO_ERROR)
                        goto done;
                    FIX_ZONECUT(learned_zones, rrs_zonecut_n, ret_val);
                    if (ret_val != VAL_NO_ERROR)
                        goto done;
                }
            } else if (fix_zonecut && namecmp(rrs_zonecut_n, name_n)) {
                /*
                 * Multiple Zonecuts
                 */
                if (ns_name_ntop(name_n, name_p, sizeof(name_p)) == -1) {
                    ret_val =  VAL_BAD_ARGUMENT;
                    goto done;
                }
                val_log(context, LOG_DEBUG, 
                        "digest_response(): Ambiguous zonecut for {%s %d %d} query responses -- %s versus %s", 
                        query_name_p, query_class_h, query_type_h, rrs_zonecut_p, name_p);
                matched_q->qc_state = Q_CONFLICTING_ANSWERS;
                ret_val = VAL_NO_ERROR;
                goto done;
            }
        } else if (set_type_h == ns_t_ns && from_section == VAL_FROM_AUTHORITY && answer == 0) {
            if (referral_seen == FALSE) {
                memcpy(referral_zone_n, name_n,
                       wire_name_length(name_n));
                referral_seen = TRUE;
            } else if (namecmp(referral_zone_n, name_n) != 0) {
                /*
                 * Multiple NS records; Malformed referral notice 
                 */
                val_log(context, LOG_DEBUG, "digest_response(): Ambiguous referral zonecut");
                matched_q->qc_state = Q_REFERRAL_ERROR;
                ret_val = VAL_NO_ERROR;
                goto done;
            }
        }

        FREE(rdata);
        rdata = NULL;

    } 
    
    /*
     * Identify proofs of non-existence 
     * XXX This needs to be rethought, reviewed and fixed
     */
    if (proof_seen) {
    
        if (nothing_other_than_alias) {
            /*
             * If we just have an alias in the answer, this is an 
             * NXDOMAIN or NODATA proof for the target
             */
                
        } else if (answer > 0) {
            /*
             * If we have an answer this is a supporting proof for
             * that answer - e.g. a wildcard proof.
             */
            
            
        } else if (referral_seen && header->rcode == ns_r_noerror) {
            /* 
             * If we see an NS record in the authority section, this
             * _could_ be a referral
             */
            if (soa_seen) {
                /* XXX if this is a no-data response, it is not a referral */
                referral_seen = FALSE;
            } else {
                /* XXX else this is a DS non-existence proof and it is a referral */
                /* Don't enable EDNS0 below this level */
                val_log(context, LOG_DEBUG, "digest_response(): Disabling further EDNS0 for {%s %d %d}",
                        query_name_p, query_class_h, query_type_h); 
                matched_q->qc_flags |= VAL_QUERY_NO_EDNS0;
            }

        } else {
            /* XXX non-existence of queried name or type */
        }
    } 

    if (referral_seen || nothing_other_than_alias) {
        struct rrset_rec *cloned_answers, *cloned_proofs;
        cloned_answers = copy_rrset_rec_list(learned_answers);
        cloned_proofs = copy_rrset_rec_list(learned_proofs);

        if (VAL_NO_ERROR != (ret_val =
            follow_referral_or_alias_link(context,
                                          nothing_other_than_alias,
                                          referral_zone_n, matched_qfq,
                                          &learned_zones, qnames,
                                          queries, &cloned_answers, 
                                          &cloned_proofs))) {
            res_sq_free_rrset_recs(&cloned_answers);
            res_sq_free_rrset_recs(&cloned_proofs);
            goto done;
        }
        cloned_answers = NULL; /* consumed */
        cloned_proofs = NULL; /* consumed */
        learned_zones = NULL; /* consumed */

    } else {

        /*
         * if we hadn't enabled EDNS0 but we got a response for a zone 
         * where DNSSEC is enabled, we should retry with EDNS0 enabled
         * (This can occur if a name server a name server is 
         * authoritative for the parent zone as well as the 
         * child zone, or if one of the name servers reached while 
         * following referrals is also recursive)
         */
        if (zonecut_was_modified && 
            (matched_q->qc_flags & VAL_QUERY_NO_EDNS0)) {
            if (VAL_NO_ERROR != (ret_val =
                get_zse(context, query_name_n, matched_qfq->qfq_flags, 
                        &tzonestatus, NULL, &ttl_x))) { 
                return ret_val;
            }
            SET_MIN_TTL(matched_q->qc_ttl_x, ttl_x);
            if (tzonestatus == VAL_AC_WAIT_FOR_TRUST) { 
                matched_q->qc_flags ^= VAL_QUERY_NO_EDNS0;
                matched_q->qc_state = Q_INIT;
                ret_val = VAL_NO_ERROR;
                goto done;
            }
        }
        
        di_response->di_answers = copy_rrset_rec_list(learned_answers);
        di_response->di_proofs = copy_rrset_rec_list(learned_proofs);
        /* the learned zone information may be incomplete, don't save it */
        res_sq_free_rrset_recs(&learned_zones);
        learned_zones = NULL; /* consumed */
        
        /*
         * Check if this is the response to a referral request 
         */
        if (matched_q->qc_referral != NULL) {
            consume_referral_data(&matched_q->qc_referral, di_response, qnames);
        } 

        matched_q->qc_state = Q_ANSWERED;
        ret_val = VAL_NO_ERROR;

        /*
         * if we were fetching glue here, save a copy as zone info 
         */
        if ((matched_qfq->qfq_flags & VAL_QUERY_GLUE_REQUEST) && (answer != 0) 
                && !proof_seen && !nothing_other_than_alias) {
            struct rrset_rec *gluedata = copy_rrset_rec(learned_answers);
            if (VAL_NO_ERROR != (ret_val = stow_zone_info(&gluedata, matched_q))) {
                res_sq_free_rrset_recs(&gluedata);
                goto done;
            }
        }
    }

    if (VAL_NO_ERROR != (ret_val = stow_answers(&learned_answers, matched_q))) {
        goto done;
    }

    if (VAL_NO_ERROR != (ret_val = stow_negative_answers(&learned_proofs, matched_q))) {
        goto done;
    }

    return ret_val;

  done:
    if (rdata)
        FREE(rdata);
    res_sq_free_rrset_recs(&learned_answers);
    res_sq_free_rrset_recs(&learned_proofs);
    res_sq_free_rrset_recs(&learned_zones);
    return ret_val;
}


/*
 * This is the interface between libval and libsres for sending queries
 */
int
val_resquery_send(val_context_t * context,
                  struct queries_for_query *matched_qfq)
{
    char            name_p[NS_MAXDNAME];
    char            name_buf[INET6_ADDRSTRLEN + 1];
    int             ret_val;
    struct name_server *tempns;
    struct val_query_chain *matched_q;

    /*
     * Get a (set of) answer(s) from the default NS's.
     * If nslist is NULL, read the cached zones and name servers
     * in context to create the nslist
     */
    struct name_server *nslist;
    if ((matched_qfq == NULL) || 
        (matched_qfq->qfq_query->qc_ns_list == NULL)
#ifndef VAL_NO_ASYNC
        || (matched_qfq->qfq_flags & VAL_QUERY_ASYNC)
#endif
        ) {
        return VAL_BAD_ARGUMENT;
    }
    matched_q = matched_qfq->qfq_query; /* Can never be NULL if matched_qfq is not NULL */
    nslist = matched_q->qc_ns_list;

    if (ns_name_ntop(matched_q->qc_name_n, name_p, sizeof(name_p)) == -1) {
        return VAL_BAD_ARGUMENT;
    }

    val_log(context, LOG_DEBUG, "val_resquery_send(): Sending query for {%s %d %d} to:", 
            name_p, matched_q->qc_class_h, matched_q->qc_type_h);
    for (tempns = nslist; tempns; tempns = tempns->ns_next) {
        val_log(context, LOG_DEBUG, "    %s",
                val_get_ns_string((struct sockaddr *)tempns->ns_address[0],
                                  name_buf, sizeof(name_buf)));
    }

    if ((ret_val =
         query_send(name_p, matched_q->qc_type_h, matched_q->qc_class_h,
                    nslist, &(matched_q->qc_trans_id))) == SR_UNSET)
        return VAL_NO_ERROR;

    /*
     * ret_val contains a resolver error 
     */
    matched_q->qc_state = Q_QUERY_ERROR;
    return VAL_NO_ERROR;
}

/*
 * This is the interface between libval and libsres for receiving responses 
 */
int
val_resquery_rcv(val_context_t * context,
                 struct queries_for_query *matched_qfq,
                 struct domain_info **response,
                 struct queries_for_query **queries,
                 fd_set *pending_desc,
                 struct timeval *closest_event)
{
    struct name_server *server = NULL;
    u_char       *response_data = NULL;
    size_t       response_length = 0;
    char         name_p[NS_MAXDNAME];
    struct val_query_chain *matched_q;

    int             ret_val;

    if ((matched_qfq == NULL) || (response == NULL) || (queries == NULL) ||
        (pending_desc == NULL)
#ifndef VAL_NO_ASYNC
        || (matched_qfq->qfq_flags & VAL_QUERY_ASYNC)
#endif
        )
        return VAL_BAD_ARGUMENT;

    matched_q = matched_qfq->qfq_query; /* Can never be NULL if matched_qfq is not NULL */
    *response = NULL;
    ret_val = response_recv(&(matched_q->qc_trans_id), pending_desc, closest_event,
                            &server, &response_data, &response_length);

    if (ret_val == SR_NO_ANSWER_YET)
        return VAL_NO_ERROR;

    /** convert name to printable string */
    if (ns_name_ntop(matched_q->qc_name_n, name_p, sizeof(name_p)) == -1) {
        matched_q->qc_state = Q_RESPONSE_ERROR;
        if (response_data)
            FREE(response_data);
        return VAL_NO_ERROR;
    }

    /** if there was no answer, try smaller edns0 size */
    if (ret_val == SR_NO_ANSWER) {
        val_res_nsfallback(matched_q, name_p, closest_event);
        if (response_data)
            FREE(response_data);
        return VAL_NO_ERROR;
    }

    ret_val = _process_rcvd_response(context, matched_qfq, response, queries,
                                     closest_event, name_p, server,
                                     response_data, response_length);

    return ret_val;
}

void
val_res_cancel(struct val_query_chain *matched_q)
{
#ifndef VAL_NO_ASYNC
    if (matched_q->qc_ea) {
        res_async_query_free(matched_q->qc_ea);
        matched_q->qc_ea = NULL;
    }
    else
#endif
        res_cancel(&(matched_q->qc_trans_id));
}

void
val_res_nsfallback(struct val_query_chain *matched_q, const char *name_p,
                   struct timeval *closest_event)
{
    int edns0, ret_val;

#ifndef VAL_NO_ASYNC
    if (matched_q->qc_ea)
        ret_val = res_nsfallback_ea(matched_q->qc_ea, closest_event,
                                    name_p, matched_q->qc_class_h, 
                                    matched_q->qc_type_h, &edns0);
    else
#endif
        ret_val = res_nsfallback(matched_q->qc_trans_id, closest_event,
                                 name_p, matched_q->qc_class_h, 
                                 matched_q->qc_type_h, &edns0);
    if (-1 == ret_val) {
        matched_q->qc_state = Q_RESPONSE_ERROR;
        val_res_cancel(matched_q);
    }
    else {
        if (edns0) {
            /* reset the flag if enabled */
            if(matched_q->qc_flags & VAL_QUERY_NO_EDNS0)
                matched_q->qc_flags ^= VAL_QUERY_NO_EDNS0;
        } else {
            /* set the flag if disabled */
            if (!(matched_q->qc_flags & VAL_QUERY_NO_EDNS0))
                matched_q->qc_flags |= VAL_QUERY_NO_EDNS0;
        }
    }
}

static int
_process_rcvd_response(val_context_t * context,
                       struct queries_for_query *matched_qfq,
                       struct domain_info **response,
                       struct queries_for_query **queries,
                       struct timeval *closest_event,
                       const char *name_p,
                       struct name_server *server,
                       u_char *response_data, size_t response_length)
{
    struct val_query_chain *matched_q = matched_qfq->qfq_query;
    int ret_val;

    matched_q->qc_respondent_server = server;

    *response = (struct domain_info *) MALLOC(sizeof(struct domain_info));
    if (*response == NULL) {
        if (response_data)
            FREE(response_data);
        return VAL_OUT_OF_MEMORY;
    }

    /* we have an answer, that we need to process */

    /*
     * Initialize the response structure 
     */
    (*response)->di_answers = NULL;
    (*response)->di_proofs = NULL;
    (*response)->di_qnames = NULL;
    (*response)->di_requested_type_h = matched_q->qc_type_h;
    (*response)->di_requested_class_h = matched_q->qc_class_h;

    if (((*response)->di_requested_name_h = STRDUP(name_p)) == NULL) {
        FREE(*response);
        *response = NULL;
        if (response_data)
            FREE(response_data);
        return VAL_OUT_OF_MEMORY;
    }

    if ((ret_val = digest_response(context, matched_qfq,
                                   queries, response_data, response_length,
                                   *response) != VAL_NO_ERROR)) {
        free_domain_info_ptrs(*response);
        FREE(*response);
        *response = NULL;
        FREE(response_data);
        return ret_val;
    }

    if (matched_q->qc_state == Q_RESPONSE_ERROR) {
        /* try a different NS if possible */
        free_domain_info_ptrs(*response);
        FREE(*response);
        *response = NULL;
        val_res_nsfallback(matched_q, name_p, closest_event);
        if (matched_q->qc_state != Q_RESPONSE_ERROR)
            matched_q->qc_state = Q_SENT;
    }
    else {
        /* we're good to go, cancel pending query transactions */
        val_res_cancel(matched_q);
        (*response)->di_res_error = SR_UNSET;
    }

    FREE(response_data);

    /*
     * What happens when an empty NXDOMAIN is returned? 
     * What happens when an empty NOERROR is returned? 
     */

    return VAL_NO_ERROR;
}

/*****************************************************************************
 *
 *
 * Asynchronous API
 *
 *
 ****************************************************************************/
#ifndef VAL_NO_ASYNC


/*
 * This is the interface between libval and libsres for sending queries
 * Get a (set of) answer(s) from the default NS's.
 */
int
val_resquery_async_send(val_context_t * context,
                        struct queries_for_query *matched_qfq)
{
    char            name_p[NS_MAXDNAME];
    char            name_buf[INET6_ADDRSTRLEN + 1];
    struct val_query_chain *matched_q;

    if ((matched_qfq == NULL) || (matched_qfq->qfq_query->qc_ns_list == NULL))
        return VAL_BAD_ARGUMENT;

    /** Can never be NULL if matched_qfq is not NULL */
    matched_q = matched_qfq->qfq_query;

    if (ns_name_ntop(matched_q->qc_name_n, name_p, sizeof(name_p)) == -1)
        return VAL_BAD_ARGUMENT;

    if (val_log_debug_level() >= LOG_DEBUG ) {
        struct name_server *tempns;
        struct name_server *nslist = matched_q->qc_ns_list;

        val_log(context, LOG_DEBUG,
                "val_resquery_async_send(): Sending query for {%s %d %d} to:", 
                name_p, matched_q->qc_class_h, matched_q->qc_type_h);
        for (tempns = nslist; tempns; tempns = tempns->ns_next) {
            val_log(context, LOG_DEBUG, "    %s",
                    val_get_ns_string((struct sockaddr *)tempns->ns_address[0],
                                      name_buf, sizeof(name_buf)));
        }
    }

    matched_q->qc_ea = res_async_query_send(name_p, matched_q->qc_type_h,
                                            matched_q->qc_class_h, 
                                            matched_q->qc_ns_list);
    if (!matched_q->qc_ea)
        matched_q->qc_state = Q_QUERY_ERROR;

    return VAL_NO_ERROR;
}

/*
 * This is the interface between libval and libsres for receiving responses 
 */
int
val_resquery_async_rcv(val_context_t * context,
                       struct queries_for_query *matched_qfq,
                       struct domain_info **response,
                       struct queries_for_query **queries,
                       fd_set *pending_desc,
                       struct timeval *closest_event)
{
    struct name_server *server = NULL;
    u_char       *response_data = NULL;
    size_t       response_length = 0;
    char         name_p[NS_MAXDNAME], stream;
    struct val_query_chain *matched_q;
    int             ret_val, handled;

    if ((matched_qfq == NULL) || (response == NULL) || (queries == NULL) ||
        (pending_desc == NULL))
        return VAL_BAD_ARGUMENT;

    matched_q = matched_qfq->qfq_query; /* ! NULL if matched_qfq ! NULL */
    *response = NULL;
    stream = res_async_ea_is_using_stream(matched_q->qc_ea);

    /** check for a response */
    ret_val = res_async_query_handle(matched_q->qc_ea, &handled, pending_desc);
    if (ret_val == SR_NO_ANSWER_YET)
        return VAL_NO_ERROR;

    /** get the response, if any */
    ret_val = res_io_get_a_response(matched_q->qc_ea, &response_data,
                                    &response_length, &server);

    /** convert name to printable string */
    if (ns_name_ntop(matched_q->qc_name_n, name_p, sizeof(name_p)) == -1) {
        matched_q->qc_state = Q_RESPONSE_ERROR;
        if (response_data)
            FREE(response_data);
        return VAL_NO_ERROR;
    }

    /** if there was no answer, try smaller edns0 size */
    if (ret_val == SR_NO_ANSWER) {
        if (stream || !res_async_ea_is_using_stream(matched_q->qc_ea))
            val_res_nsfallback(matched_q, name_p, closest_event);
        if (response_data)
            FREE(response_data);
        return VAL_NO_ERROR;
    }

    ret_val = _process_rcvd_response(context, matched_qfq, response, queries,
                                     closest_event, name_p, server,
                                     response_data, response_length);

    return ret_val;
 }
 
int
val_async_select_info(val_context_t *context, fd_set *activefds,
                      int *nfds, struct timeval *timeout)
{
    val_async_status *as;
    struct queries_for_query *qfq;

    if (NULL == context)
        return VAL_BAD_ARGUMENT;

    for (as = context->as_list; as; as = as->val_as_next)
        for (qfq = as->val_as_queries; qfq; qfq = qfq->qfq_next) {
            if (!qfq->qfq_query->qc_ea)
                continue;
            res_async_query_select_info(qfq->qfq_query->qc_ea, nfds, activefds,
                                        timeout);
        }

    return VAL_NO_ERROR;
}


#endif /* VAL_NO_ASYNC */
