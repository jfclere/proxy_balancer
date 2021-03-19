/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mod_proxy.h"
#include "mod_watchdog.h"
#include "scoreboard.h"
#include "ap_mpm.h"
#include "apr_version.h"
#include "ap_hooks.h"

static APR_OPTIONAL_FN_TYPE(balancer_manage) *balancer_manage = NULL;

module AP_MODULE_DECLARE_DATA test_balancer_module;

#define LB_CLUSTER_WATHCHDOG_NAME ("_lb_cluster_")
static ap_watchdog_t *watchdog;
static apr_time_t last = 0;

/*
 * Builds the parameter for mod_balancer
 */
static apr_status_t mod_manager_manage_worker(request_rec *r) {
    apr_table_t *params;
    params = apr_table_make(r->pool, 10);
    /* balancer */
    apr_table_set(params, "b" , "mycluster");
    apr_table_set(params, "b_lbm", "byrequests");
    /*apr_table_set(params, "b_tmo", apr_psprintf(r->pool, "%d", bal->Timeout));
    apr_table_set(params, "b_max", apr_psprintf(r->pool, "%d", bal->Maxattempts));
    apr_table_set(params, "b_ss", apr_pstrcat(r->pool, bal->StickySessionCookie, "|", bal->StickySessionPath, NULL));
     */

    /* and new worker */
    apr_table_set(params, "b_wyes" , "1");
    apr_table_set(params, "b_nwrkr" ,  "http://localhost:8080");
    balancer_manage(r, params);
    apr_table_clear(params);

    /* now process the worker */
    apr_table_set(params, "b" , "mycluster");
    apr_table_set(params, "w" ,  "http://localhost:8080");
    apr_table_set(params, "w_wr", "jvmroutetomcat1");
    apr_table_set(params, "w_status_D", "0"); /* Not Dissabled */

    /* set the health check (requires mod_proxy_hcheck) */
    /* CPING for AJP and OPTIONS for HTTP/1.1 */

    /*    apr_table_set(params, "w_hm", "OPTIONS"); */
    apr_table_set(params, "w_hm", "CPING");

    /* Use 10 sec for the moment, the idea is to adjust it with the STATUS frequency */
    apr_table_set(params, "w_hi", "10000");
    return balancer_manage(r, params);
}


static apr_status_t mc_watchdog_callback(int state, void *data,
                                         apr_pool_t *pool)
{
    apr_status_t rv = APR_SUCCESS;
    server_rec *server = (server_rec *)data;
    apr_time_t now;
    switch (state) {
        case AP_WATCHDOG_STATE_STARTING:
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "lbmethod_cluster_watchdog_callback STARTING");
            break;

        case AP_WATCHDOG_STATE_RUNNING:
            now = apr_time_now();
            if (server) {
                if (last < (now - apr_time_from_sec(5))) {
                    apr_pool_t *rrp;
                    request_rec *rnew;
                    last = now;
                    apr_pool_create(&rrp, pool);
                    apr_pool_tag(rrp, "hc_request"); 
                    /* request here */
                    rnew = apr_pcalloc(rrp, sizeof(request_rec));
                    rnew->pool = rrp;
                    /* we need only those ones */
                    rnew->server = server;
                    rnew->connection = apr_pcalloc(rrp, sizeof(conn_rec));
                    rnew->connection->log_id = "-";
                    rnew->connection->conn_config = ap_create_conn_config(rrp);
                    rnew->log_id = "-";
                    rnew->useragent_addr = apr_pcalloc(rrp, sizeof(apr_sockaddr_t));
                    rnew->per_dir_config = server->lookup_defaults;
                    rnew->notes = apr_table_make(rnew->pool, 1);
                    rnew->method = "PING";
                    rnew->uri = "/";
                    rnew->headers_in = apr_table_make(rnew->pool, 1);

                    mod_manager_manage_worker(rnew);
                    apr_pool_destroy(rrp);
                }
            }
            break;

        case AP_WATCHDOG_STATE_STOPPING:
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, server, "lbmethod_cluster_watchdog_callback STOPPING");
            break;
    }
    return rv;
}

static int lbmethod_cluster_post_config(apr_pool_t *p, apr_pool_t *plog,
                                     apr_pool_t *ptemp, server_rec *s)
{
    APR_OPTIONAL_FN_TYPE(ap_watchdog_get_instance) *mc_watchdog_get_instance;
    APR_OPTIONAL_FN_TYPE(ap_watchdog_register_callback) *mc_watchdog_register_callback;

    if (ap_state_query(AP_SQ_MAIN_STATE) == AP_SQ_MS_CREATE_PRE_CONFIG) {
        return OK;
    }

    /* add our watchdog callback */
    mc_watchdog_get_instance = APR_RETRIEVE_OPTIONAL_FN(ap_watchdog_get_instance);
    mc_watchdog_register_callback = APR_RETRIEVE_OPTIONAL_FN(ap_watchdog_register_callback);
    if (!mc_watchdog_get_instance || !mc_watchdog_register_callback) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s,
                     "mod_watchdog is required");
        return !OK;
    }
    if (mc_watchdog_get_instance(&watchdog,
                                  LB_CLUSTER_WATHCHDOG_NAME,
                                  0, 1, p)) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s,
                     "Failed to create watchdog instance (%s)",
                     LB_CLUSTER_WATHCHDOG_NAME);
        return !OK;
    }
    while (s) {
        if (mc_watchdog_register_callback(watchdog,
                AP_WD_TM_SLICE,
                s,
                mc_watchdog_callback)) {
            ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s,
                         "Failed to register watchdog callback (%s)",
                         LB_CLUSTER_WATHCHDOG_NAME);
            return !OK;
        }
        s = s->next;
    }

    balancer_manage = APR_RETRIEVE_OPTIONAL_FN(balancer_manage);
    return OK;
}

static void register_hooks(apr_pool_t *p)
{
    ap_hook_post_config(lbmethod_cluster_post_config, NULL, NULL, APR_HOOK_MIDDLE);
}


AP_DECLARE_MODULE(test_balancer) = {
    STANDARD20_MODULE_STUFF,
    NULL,                       /* create per-directory config structure */
    NULL,                       /* merge per-directory config structures */
    NULL,        /* create per-server config structure */
    NULL,         /* merge per-server config structures */
    NULL,                       /* command apr_table_t */
    register_hooks              /* register hooks */
};
