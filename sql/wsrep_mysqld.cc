/* Copyright (c) 2008, 2025, Codership Oy <http://www.codership.com>
   Copyright (c) 2020, 2025, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.x1

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include "sql_plugin.h"                         /* wsrep_plugins_pre_init() */
#include "my_global.h"
#include "wsrep_server_state.h"
#include "wsrep_status.h"

#include "mariadb.h"
#include <mysqld.h>
#include <transaction.h>
#include <sql_class.h>
#include <sql_parse.h>
#include <sql_base.h> /* find_temporary_table() */
#include <sql_statistics.h> /* is_stat_table() */
#include "slave.h"
#include "rpl_mi.h"
#include "sql_repl.h"
#include "rpl_filter.h"
#include "sql_callback.h"
#include "sp_head.h"
#include "sql_show.h"
#include "sp.h"
#include "handler.h"
#include "wsrep_priv.h"
#include "wsrep_thd.h"
#include "wsrep_sst.h"
#include "wsrep_utils.h"
#include "wsrep_var.h"
#include "wsrep_binlog.h"
#include "wsrep_applier.h"
#include "wsrep_schema.h"
#include "wsrep_xid.h"
#include "wsrep_trans_observer.h"
#include "mysql/service_wsrep.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include "log_event.h"
#include "sql_connect.h"
#include "thread_cache.h"
#include "debug_sync.h"

#include <sstream>

/* wsrep-lib */

my_bool wsrep_emulate_bin_log= FALSE; // activating parts of binlog interface
my_bool wsrep_preordered_opt= FALSE;

/* Streaming Replication */
const char *wsrep_fragment_units[]= { "bytes", "rows", "statements", NullS };
const char *wsrep_SR_store_types[]= { "none", "table", NullS };

/*
 * Begin configuration options
 */

extern my_bool plugins_are_initialized;

/* System variables. */
const char *wsrep_provider;
const char *wsrep_provider_options;
const char *wsrep_cluster_address;
const char *wsrep_cluster_name;
const char *wsrep_node_name;
const char *wsrep_node_address;
const char *wsrep_node_incoming_address;
const char *wsrep_start_position;
const char *wsrep_data_home_dir;
const char *wsrep_dbug_option;
const char *wsrep_notify_cmd;
const char *wsrep_status_file;
const char *wsrep_allowlist;

ulong   wsrep_debug;                            // Debug level logging
my_bool wsrep_convert_LOCK_to_trx;              // Convert locking sessions to trx
my_bool wsrep_auto_increment_control;           // Control auto increment variables
my_bool wsrep_drupal_282555_workaround;         // Retry autoinc insert after dupkey
my_bool wsrep_certify_nonPK;                    // Certify, even when no primary key
ulong   wsrep_certification_rules      = WSREP_CERTIFICATION_RULES_STRICT;
my_bool wsrep_recovery;                         // Recovery
my_bool wsrep_log_conflicts;
my_bool wsrep_load_data_splitting= 0;           // Commit load data every 10K intervals
my_bool wsrep_slave_UK_checks;                  // Slave thread does UK checks
my_bool wsrep_slave_FK_checks;                  // Slave thread does FK checks
my_bool wsrep_restart_slave;                    // Should mysql slave thread be
                                                // restarted, when node joins back?
my_bool wsrep_desync;                           // De(re)synchronize the node from the
                                                // cluster
ulonglong wsrep_mode;
bool wsrep_service_started;                     // If Galera was initialized
long wsrep_slave_threads;                       // No. of slave appliers threads
ulong wsrep_retry_autocommit;                   // Retry aborted autocommit trx
ulong wsrep_max_ws_size;                        // Max allowed ws (RBR buffer) size
ulong wsrep_max_ws_rows;                        // Max number of rows in ws
ulong wsrep_forced_binlog_format= BINLOG_FORMAT_UNSPEC;
ulong wsrep_mysql_replication_bundle;

bool              wsrep_gtid_mode;              // Enable WSREP native GTID support
Wsrep_gtid_server wsrep_gtid_server;
uint  wsrep_gtid_domain_id=0;                   // Domain id on above structure

/* Other configuration variables and their default values. */
my_bool wsrep_incremental_data_collection= 0;   // Incremental data collection
my_bool wsrep_restart_slave_activated= 0;       // Node has dropped, and slave
                                                // restart will be needed
bool wsrep_new_cluster= false;                  // Bootstrap the cluster?
int wsrep_slave_count_change= 0;                // No. of appliers to stop/start
int wsrep_to_isolation= 0;                      // No. of active TO isolation threads
long wsrep_max_protocol_version= 4;             // Maximum protocol version to use
long int  wsrep_protocol_version= wsrep_max_protocol_version;
ulong wsrep_trx_fragment_unit= WSREP_FRAG_BYTES;
                                                // unit for fragment size
ulong wsrep_SR_store_type= WSREP_SR_STORE_TABLE;
uint  wsrep_ignore_apply_errors= 0;

std::atomic <bool> wsrep_thread_create_failed;

/*
 * End configuration options
 */

/*
 * Cached variables
 */

// Whether the Galera write-set replication provider is set
// wsrep_provider && strcmp(wsrep_provider, WSREP_NONE)
bool WSREP_PROVIDER_EXISTS_;

// Whether the Galera write-set replication is enabled
// global_system_variables.wsrep_on && WSREP_PROVIDER_EXISTS_
bool WSREP_ON_;

/*
 * Other wsrep global variables.
 */

mysql_mutex_t LOCK_wsrep_ready;
mysql_cond_t  COND_wsrep_ready;
mysql_mutex_t LOCK_wsrep_sst;
mysql_cond_t  COND_wsrep_sst;
mysql_mutex_t LOCK_wsrep_sst_init;
mysql_cond_t  COND_wsrep_sst_init;
mysql_mutex_t LOCK_wsrep_replaying;
mysql_cond_t  COND_wsrep_replaying;
mysql_mutex_t LOCK_wsrep_slave_threads;
mysql_cond_t  COND_wsrep_slave_threads;
mysql_mutex_t LOCK_wsrep_gtid_wait_upto;
mysql_mutex_t LOCK_wsrep_cluster_config;
mysql_mutex_t LOCK_wsrep_desync;
mysql_mutex_t LOCK_wsrep_config_state;
mysql_mutex_t LOCK_wsrep_group_commit;
mysql_mutex_t LOCK_wsrep_SR_pool;
mysql_mutex_t LOCK_wsrep_SR_store;
mysql_mutex_t LOCK_wsrep_joiner_monitor;
mysql_mutex_t LOCK_wsrep_donor_monitor;
mysql_cond_t  COND_wsrep_joiner_monitor;
mysql_cond_t  COND_wsrep_donor_monitor;

int wsrep_replaying= 0;
ulong  wsrep_running_threads = 0; // # of currently running wsrep
				  // # threads
ulong  wsrep_running_applier_threads = 0; // # of running applier threads
ulong  wsrep_running_rollbacker_threads = 0; // # of running
					     // # rollbacker threads
ulong  my_bind_addr;

#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key
  key_LOCK_wsrep_replaying, key_LOCK_wsrep_ready, key_LOCK_wsrep_sst,
  key_LOCK_wsrep_sst_thread, key_LOCK_wsrep_sst_init,
  key_LOCK_wsrep_slave_threads, key_LOCK_wsrep_gtid_wait_upto,
  key_LOCK_wsrep_desync,
  key_LOCK_wsrep_config_state, key_LOCK_wsrep_cluster_config,
  key_LOCK_wsrep_group_commit,
  key_LOCK_wsrep_SR_pool,
  key_LOCK_wsrep_SR_store,
  key_LOCK_wsrep_thd_queue,
  key_LOCK_wsrep_joiner_monitor,
  key_LOCK_wsrep_donor_monitor;

PSI_cond_key key_COND_wsrep_thd,
  key_COND_wsrep_replaying, key_COND_wsrep_ready, key_COND_wsrep_sst,
  key_COND_wsrep_sst_init, key_COND_wsrep_sst_thread,
  key_COND_wsrep_thd_queue, key_COND_wsrep_slave_threads, key_COND_wsrep_gtid_wait_upto,
  key_COND_wsrep_joiner_monitor, key_COND_wsrep_donor_monitor;

PSI_file_key key_file_wsrep_gra_log;

static PSI_mutex_info wsrep_mutexes[]=
{
  { &key_LOCK_wsrep_ready, "LOCK_wsrep_ready", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_sst, "LOCK_wsrep_sst", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_sst_thread, "wsrep_sst_thread", 0},
  { &key_LOCK_wsrep_sst_init, "LOCK_wsrep_sst_init", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_sst, "LOCK_wsrep_sst", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_replaying, "LOCK_wsrep_replaying", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_slave_threads, "LOCK_wsrep_slave_threads", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_gtid_wait_upto, "LOCK_wsrep_gtid_wait_upto", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_cluster_config, "LOCK_wsrep_cluster_config", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_desync, "LOCK_wsrep_desync", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_config_state, "LOCK_wsrep_config_state", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_group_commit, "LOCK_wsrep_group_commit", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_SR_pool, "LOCK_wsrep_SR_pool", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_SR_store, "LOCK_wsrep_SR_store", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_joiner_monitor, "LOCK_wsrep_joiner_monitor", PSI_FLAG_GLOBAL},
  { &key_LOCK_wsrep_donor_monitor, "LOCK_wsrep_donor_monitor", PSI_FLAG_GLOBAL}
};

static PSI_cond_info wsrep_conds[]=
{
  { &key_COND_wsrep_ready, "COND_wsrep_ready", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_sst, "COND_wsrep_sst", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_sst_init, "COND_wsrep_sst_init", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_sst_thread, "wsrep_sst_thread", 0},
  { &key_COND_wsrep_thd, "THD::COND_wsrep_thd", 0},
  { &key_COND_wsrep_replaying, "COND_wsrep_replaying", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_slave_threads, "COND_wsrep_wsrep_slave_threads", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_gtid_wait_upto, "COND_wsrep_gtid_wait_upto", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_joiner_monitor, "COND_wsrep_joiner_monitor", PSI_FLAG_GLOBAL},
  { &key_COND_wsrep_donor_monitor, "COND_wsrep_donor_monitor", PSI_FLAG_GLOBAL}
};

static PSI_file_info wsrep_files[]=
{
  { &key_file_wsrep_gra_log, "wsrep_gra_log", 0}
};

PSI_thread_key key_wsrep_sst_joiner, key_wsrep_sst_donor,
  key_wsrep_rollbacker, key_wsrep_applier,
  key_wsrep_sst_joiner_monitor, key_wsrep_sst_donor_monitor;

static PSI_thread_info wsrep_threads[]=
{
 {&key_wsrep_sst_joiner, "wsrep_sst_joiner_thread", PSI_FLAG_GLOBAL},
 {&key_wsrep_sst_donor, "wsrep_sst_donor_thread", PSI_FLAG_GLOBAL},
 {&key_wsrep_rollbacker, "wsrep_rollbacker_thread", PSI_FLAG_GLOBAL},
 {&key_wsrep_applier, "wsrep_applier_thread", PSI_FLAG_GLOBAL},
 {&key_wsrep_sst_joiner_monitor, "wsrep_sst_joiner_monitor", PSI_FLAG_GLOBAL},
 {&key_wsrep_sst_donor_monitor, "wsrep_sst_donor_monitor", PSI_FLAG_GLOBAL}
};

#endif /* HAVE_PSI_INTERFACE */

my_bool wsrep_inited= 0; // initialized ?

static wsrep_uuid_t node_uuid= WSREP_UUID_UNDEFINED;
static char         cluster_uuid_str[40]= { 0, };

static char provider_name[256]= { 0, };
static char provider_version[256]= { 0, };
static char provider_vendor[256]= { 0, };

/*
 * Wsrep status variables. LOCK_status must be locked When modifying
 * these variables,
 */
std::atomic<bool> wsrep_ready(false);
my_bool     wsrep_connected         = FALSE;
const char* wsrep_cluster_state_uuid= cluster_uuid_str;
long long   wsrep_cluster_conf_id   = WSREP_SEQNO_UNDEFINED;
const char* wsrep_cluster_status    = "Disconnected";
long        wsrep_cluster_size      = 0;
long        wsrep_local_index       = -1;
long long   wsrep_local_bf_aborts   = 0;
const char* wsrep_provider_name     = provider_name;
const char* wsrep_provider_version  = provider_version;
const char* wsrep_provider_vendor   = provider_vendor;
char* wsrep_provider_capabilities   = NULL;
char* wsrep_cluster_capabilities    = NULL;
/* End wsrep status variables */

wsp::Config_state *wsrep_config_state;

void WSREP_LOG(void (*fun)(const char* fmt, ...), const char* fmt, ...)
{
  /* Allocate short buffer from stack. If the vsnprintf() return value
     indicates that the message was truncated, a new buffer will be allocated
     dynamically and the message will be reprinted. */
  char msg[128] = {'\0'};
  va_list arglist;
  va_start(arglist, fmt);
  int n= vsnprintf(msg, sizeof(msg), fmt, arglist);
  va_end(arglist);
  if (n < 0)
  {
    sql_print_warning("WSREP: Printing message failed");
  }
  else if (n < (int)sizeof(msg))
  {
    fun("WSREP: %s", msg);
  }
  else
  {
    size_t dynbuf_size= std::max(n, 4096);
    char* dynbuf= (char*) my_malloc(PSI_NOT_INSTRUMENTED, dynbuf_size, MYF(0));
    if (dynbuf)
    {
      va_start(arglist, fmt);
      (void)vsnprintf(&dynbuf[0], dynbuf_size - 1, fmt, arglist);
      va_end(arglist);
      dynbuf[dynbuf_size - 1] = '\0';
      fun("WSREP: %s", &dynbuf[0]);
      my_free(dynbuf);
    }
    else
    {
      /* Memory allocation for vector failed, print truncated message. */
      fun("WSREP: %s", msg);
    }
  }
}


wsrep_uuid_t               local_uuid       = WSREP_UUID_UNDEFINED;
wsrep_seqno_t              local_seqno      = WSREP_SEQNO_UNDEFINED;

/*
 */
Wsrep_schema *wsrep_schema= 0;

static void wsrep_log_cb(wsrep::log::level level,
                         const char*, const char *msg)
{
  /*
    Silence all wsrep related logging from lib and provider if
    wsrep is not enabled.
  */
  if (!WSREP_ON) return;

  switch (level) {
  case wsrep::log::info:
    WSREP_INFO("%s", msg);
    break;
  case wsrep::log::warning:
    WSREP_WARN("%s", msg);
    break;
  case wsrep::log::error:
    WSREP_ERROR("%s", msg);
    break;
  case wsrep::log::debug:
    WSREP_DEBUG("%s", msg);
    break;
  case wsrep::log::unknown:
    WSREP_UNKNOWN("%s", msg);
    break;
  }
}

void wsrep_init_gtid()
{
  wsrep_server_gtid_t stored_gtid= wsrep_get_SE_checkpoint<wsrep_server_gtid_t>();
  // Domain id may have changed, use the one
  // received during state transfer.
  stored_gtid.domain_id= wsrep_gtid_server.domain_id;
  if (stored_gtid.server_id == 0)
  {
    rpl_gtid wsrep_last_gtid;
    if (mysql_bin_log.is_open() &&
        mysql_bin_log.lookup_domain_in_binlog_state(stored_gtid.domain_id,
                                                    &wsrep_last_gtid))
    {
      stored_gtid.server_id= wsrep_last_gtid.server_id;
      stored_gtid.seqno= wsrep_last_gtid.seq_no;
    }
    else
    {
      stored_gtid.server_id= global_system_variables.server_id;
      stored_gtid.seqno= 0;
    }
  }
  wsrep_gtid_server.gtid(stored_gtid);
}

bool wsrep_get_binlog_gtid_seqno(wsrep_server_gtid_t& gtid)
{
  rpl_gtid binlog_gtid;
  int ret= 0;
  if (mysql_bin_log.is_open() &&
      mysql_bin_log.find_in_binlog_state(gtid.domain_id,
                                         gtid.server_id,
                                         &binlog_gtid))
  {
    gtid.domain_id= binlog_gtid.domain_id;
    gtid.server_id= binlog_gtid.server_id;
    gtid.seqno=     binlog_gtid.seq_no;
    ret= 1;
  }
  return ret;
}

bool wsrep_check_gtid_seqno(const uint32& domain, const uint32& server,
                            uint64& seqno)
{
  if (domain == wsrep_gtid_server.domain_id && 
      server == wsrep_gtid_server.server_id)
  {
    if (wsrep_gtid_server.seqno_committed() < seqno) return 1;
    return 0;
  }
  return 0;
}

void wsrep_init_sidno(const wsrep::id& uuid)
{
  /*
    Protocol versions starting from 4 use group gtid as it is.
    For lesser protocol versions generate new Sid map entry from inverted
    uuid.
  */
  rpl_gtid sid;
  if (wsrep_protocol_version >= 4)
  {
    memcpy((void*)&sid, (const uchar*)uuid.data(),16);
  }
  else
  {
    wsrep_uuid_t ltid_uuid;
    for (size_t i= 0; i < sizeof(ltid_uuid.data); ++i)
    {
      ltid_uuid.data[i]= ~((const uchar*)uuid.data())[i];
    }
    memcpy((void*)&sid, (const uchar*)ltid_uuid.data,16);
  }
#ifdef GTID_SUPPORT
  global_sid_lock->wrlock();
  wsrep_sidno= global_sid_map->add_sid(sid);
  WSREP_INFO("Initialized wsrep sidno %d", wsrep_sidno);
  global_sid_lock->unlock();
#endif
}

void wsrep_init_schema()
{
  DBUG_ASSERT(!wsrep_schema);

  WSREP_INFO("wsrep_init_schema_and_SR %p", wsrep_schema);
  if (!wsrep_schema)
  {
    wsrep_schema= new Wsrep_schema();
    if (wsrep_schema->init())
    {
      WSREP_ERROR("Failed to init wsrep schema");
      unireg_abort(1);
    }
    // If we are bootstraping new cluster we should 
    // clear allowlist table and populate it from variable
    if (wsrep_new_cluster) 
    {
      wsrep_schema->clear_allowlist();
      std::vector<std::string> ip_allowlist;
      if (wsrep_split_allowlist(ip_allowlist))
      {
        wsrep_schema->store_allowlist(ip_allowlist);
      }
    }
  }
}

void wsrep_deinit_schema()
{
  delete wsrep_schema;
  wsrep_schema= 0;
}

void wsrep_recover_sr_from_storage(THD *orig_thd)
{
  switch (wsrep_SR_store_type)
  {
  case WSREP_SR_STORE_TABLE:
    if (!wsrep_schema)
    {
      WSREP_ERROR("Wsrep schema not initialized when trying to recover "
                  "streaming transactions: wsrep_on %d", WSREP_ON);
      trans_commit(orig_thd);
    }
    if (wsrep_schema->recover_sr_transactions(orig_thd))
    {
      WSREP_ERROR("Failed to recover SR transactions from schema: wsrep_on : %d", WSREP_ON);
      trans_commit(orig_thd);
    }
    break;
  default:
    /* */
    WSREP_ERROR("Unsupported wsrep SR store type: %lu wsrep_on: %d",
                wsrep_SR_store_type, WSREP_ON);
    trans_commit(orig_thd);
    break;
  }
}

/** Export the WSREP provider's capabilities as a human readable string.
 * The result is saved in a dynamically allocated string of the form:
 * :cap1:cap2:cap3:
 */
static void wsrep_capabilities_export(wsrep_cap_t const cap, char** str)
{
  static const char* names[] =
  {
    /* Keep in sync with wsrep/wsrep_api.h WSREP_CAP_* macros. */
    "MULTI_MASTER",
    "CERTIFICATION",
    "PARALLEL_APPLYING",
    "TRX_REPLAY",
    "ISOLATION",
    "PAUSE",
    "CAUSAL_READS",
    "CAUSAL_TRX",
    "INCREMENTAL_WRITESET",
    "SESSION_LOCKS",
    "DISTRIBUTED_LOCKS",
    "CONSISTENCY_CHECK",
    "UNORDERED",
    "ANNOTATION",
    "PREORDERED",
    "STREAMING",
    "SNAPSHOT",
    "NBO",
  };

  std::string s;
  for (size_t i= 0; i < sizeof(names) / sizeof(names[0]); ++i)
  {
    if (cap & (1ULL << i))
    {
      if (s.empty())
      {
        s= ":";
      }
      s += names[i];
      s += ":";
    }
  }

  /* A read from the string pointed to by *str may be started at any time,
   * so it must never point to free(3)d memory or non '\0' terminated string. */

  char* const previous= *str;

  *str= strdup(s.c_str());

  if (previous != NULL)
  {
    free(previous);
  }
}

/* Verifies that SE position is consistent with the group position
 * and initializes other variables */
void wsrep_verify_SE_checkpoint(const wsrep_uuid_t& uuid,
                                wsrep_seqno_t const seqno)
{
}

/*
  Wsrep is considered ready if
  1) Provider is not loaded (native mode)
  2) Server has reached synced state
  3) Server is in joiner mode and mysqldump SST method has been
     specified
  See Wsrep_server_service::log_state_change() for further details.
 */
my_bool wsrep_ready_get (void)
{
  return wsrep_ready;
}

int wsrep_show_ready(THD *thd, SHOW_VAR *var, void *buff,
                     system_status_var *, enum_var_type)
{
  var->type= SHOW_MY_BOOL;
  var->value= buff;
  *((my_bool *)buff)= wsrep_ready_get();
  return 0;
}

void wsrep_update_cluster_state_uuid(const char* uuid)
{
  strncpy(cluster_uuid_str, uuid, sizeof(cluster_uuid_str) - 1);
}

static void wsrep_init_position()
{
}

/****************************************************************************
                         Helpers for wsrep_init()
 ****************************************************************************/
static std::string wsrep_server_name()
{
  std::string ret(wsrep_node_name ? wsrep_node_name : "");
  return ret;
}

static std::string wsrep_server_id()
{
  /* using empty server_id, which enables view change handler to
     set final server_id later on
  */
  std::string ret("");
  return ret;
}

static std::string wsrep_server_node_address()
{

  std::string ret;
  if (!wsrep_data_home_dir || strlen(wsrep_data_home_dir) == 0)
    wsrep_data_home_dir= mysql_real_data_home;

  /* Initialize node address */
  if (!wsrep_node_address || !strcmp(wsrep_node_address, ""))
  {
    char node_addr[512]= {0, };
    const size_t node_addr_max= sizeof(node_addr) - 1;
    size_t guess_ip_ret= wsrep_guess_ip(node_addr, node_addr_max);
    if (!(guess_ip_ret > 0 && guess_ip_ret < node_addr_max))
    {
      WSREP_WARN("Failed to guess base node address. Set it explicitly via "
                 "wsrep_node_address.");
    }
    else
    {
      ret= node_addr;
    }
  }
  else
  {
    ret= wsrep_node_address;
  }
  return ret;
}

static std::string wsrep_server_incoming_address()
{
  std::string ret;
  const std::string node_addr(wsrep_server_node_address());
  char inc_addr[512]= { 0, };
  size_t const inc_addr_max= sizeof (inc_addr);

  /*
    In case wsrep_node_incoming_address is either not set or set to AUTO,
    we need to use mysqld's my_bind_addr_str:mysqld_port, lastly fallback
    to wsrep_node_address' value if mysqld's bind-address is not set either.
  */
  if ((!wsrep_node_incoming_address ||
       !strcmp (wsrep_node_incoming_address, WSREP_NODE_INCOMING_AUTO)))
  {
    bool is_ipv6= false;
    unsigned int my_bind_ip= INADDR_ANY; // default if not set

    if (my_bind_addr_str && strlen(my_bind_addr_str) &&
        strcmp(my_bind_addr_str, "*") != 0)
    {
      my_bind_ip= wsrep_check_ip(my_bind_addr_str, &is_ipv6);
    }

    if (INADDR_ANY != my_bind_ip)
    {
      /*
        If its a not a valid address, leave inc_addr as empty string. mysqld
        is not listening for client connections on network interfaces.
      */
      if (INADDR_NONE != my_bind_ip && INADDR_LOOPBACK != my_bind_ip)
      {
        const char *fmt= (is_ipv6) ? "[%s]:%u" : "%s:%u";
        snprintf(inc_addr, inc_addr_max, fmt, my_bind_addr_str, mysqld_port);
      }
    }
    else /* mysqld binds to 0.0.0.0, try taking IP from wsrep_node_address. */
    {
      if (node_addr.size())
      {
        size_t const ip_len_mdb= wsrep_host_len(node_addr.c_str(), node_addr.size());
        if (ip_len_mdb + 7 /* :55555\0 */ < inc_addr_max)
        {
          memcpy (inc_addr, node_addr.c_str(), ip_len_mdb);
          snprintf(inc_addr + ip_len_mdb, inc_addr_max - ip_len_mdb, ":%u",
                   (int)mysqld_port);
        }
        else
        {
          WSREP_WARN("Guessing address for incoming client connections: "
                     "address too long.");
          inc_addr[0]= '\0';
        }
      }

      if (!strlen(inc_addr))
      {
        WSREP_WARN("Guessing address for incoming client connections failed. "
                   "Try setting wsrep_node_incoming_address explicitly.");
        WSREP_INFO("Node addr: %s", node_addr.c_str());
      }
    }
  }
  else
  {
    wsp::Address addr(wsrep_node_incoming_address);

    if (!addr.is_valid())
    {
      WSREP_WARN("Could not parse wsrep_node_incoming_address : %s",
                 wsrep_node_incoming_address);
      goto done;
    }

    /*
      In case port is not specified in wsrep_node_incoming_address, we use
      mysqld_port.
      Note that we might get here before we execute set_ports().
    */
    int local_port= (addr.get_port() > 0) ? addr.get_port() : (int) mysqld_port;
    if (!local_port)
      local_port= MYSQL_PORT;
    const char *fmt= (addr.is_ipv6()) ? "[%s]:%u" : "%s:%u";

    snprintf(inc_addr, inc_addr_max, fmt, addr.get_address(), local_port);
  }

 done:
  if (!strlen(inc_addr))
    ret= wsrep_node_incoming_address;
  else
    ret= inc_addr;
  WSREP_DEBUG("wsrep_incoming_address = %s", ret.c_str());
  return ret;
}

static std::string wsrep_server_working_dir()
{
  std::string ret;
  if (!wsrep_data_home_dir || strlen(wsrep_data_home_dir) == 0)
  {
    ret= mysql_real_data_home;
  }
  else
  {
    ret= wsrep_data_home_dir;
  }
  return ret;
}

static wsrep::gtid wsrep_server_initial_position()
{
  wsrep::gtid ret;
  WSREP_DEBUG("Server initial position: %s", wsrep_start_position);
  std::istringstream is(wsrep_start_position);
  is >> ret;
  return ret;
}

/*
  Intitialize provider specific status variables
 */
static void wsrep_init_provider_status_variables()
{
  wsrep_inited= 1;
  const wsrep::provider& provider=
    Wsrep_server_state::instance().provider();
  strncpy(provider_name,
          provider.name().c_str(),    sizeof(provider_name) - 1);
  strncpy(provider_version,
          provider.version().c_str(), sizeof(provider_version) - 1);
  strncpy(provider_vendor,
          provider.vendor().c_str(),  sizeof(provider_vendor) - 1);
}

int wsrep_init_server()
{
  wsrep::log::logger_fn(wsrep_log_cb);
  try
  {
    Wsrep_status::init_once(wsrep_status_file);

    std::string server_name;
    std::string server_id;
    std::string node_address;
    std::string incoming_address;
    std::string working_dir;
    wsrep::gtid initial_position;

    server_name= wsrep_server_name();
    server_id= wsrep_server_id();
    node_address= wsrep_server_node_address();
    incoming_address= wsrep_server_incoming_address();
    working_dir= wsrep_server_working_dir();
    initial_position= wsrep_server_initial_position();

    Wsrep_server_state::init_once(server_name,
                                  incoming_address,
                                  node_address,
                                  working_dir,
                                  initial_position,
                                  wsrep_max_protocol_version);
    Wsrep_server_state::instance().debug_log_level(wsrep_debug);
  }
  catch (const wsrep::runtime_error& e)
  {
    WSREP_ERROR("Failed to init wsrep server %s", e.what());
    return 1;
  }
  catch (const std::exception& e)
  {
    WSREP_ERROR("Failed to init wsrep server %s", e.what());
  }
  return 0;
}

void wsrep_init_globals()
{
  wsrep_init_sidno(Wsrep_server_state::instance().connected_gtid().id());
  /* Recover last written wsrep gtid */
  wsrep_init_gtid();
  if (wsrep_new_cluster)
  {
    /* Start with provided domain_id & server_id found in configuration */
    wsrep_server_gtid_t new_gtid;
    new_gtid.domain_id= wsrep_gtid_domain_id;
    new_gtid.server_id= global_system_variables.server_id;
    /* Use seqno which was recovered in wsrep_init_gtid() */
    new_gtid.seqno= wsrep_gtid_server.seqno();
    /* Try to search for domain_id and server_id combination in binlog if found continue from last seqno */
    wsrep_get_binlog_gtid_seqno(new_gtid);
    wsrep_gtid_server.gtid(new_gtid);
  }
  else
  {
    if (wsrep_gtid_mode && wsrep_gtid_server.server_id != global_system_variables.server_id)
      WSREP_INFO("Ignoring server id %ld for non bootstrap node, using %ld.",
                 global_system_variables.server_id, wsrep_gtid_server.server_id);
  }
  wsrep_init_schema();
  {
    /* apparently this thread has already called my_thread_init(),
     * so we skip it, hence 'false' for initialization. */
    wsp::thd thd(false, true);
    wsrep_sst_cleanup_user(thd.ptr);
  }
  if (WSREP_ON)
  {
    Wsrep_server_state::instance().initialized();
  }
}

void wsrep_deinit_server()
{
  wsrep_deinit_schema();
  Wsrep_server_state::destroy();
  Wsrep_status::destroy();
  wsrep_free_status_vars();
}

int wsrep_init()
{
  assert(wsrep_provider);

  wsrep_init_position();

  if (!*wsrep_provider ||
      !strcasecmp(wsrep_provider, WSREP_NONE))
  {
    // enable normal operation in case no provider is specified
    global_system_variables.wsrep_on= 0;
    int err= Wsrep_server_state::init_provider(
        wsrep_provider, wsrep_provider_options ? wsrep_provider_options : "");
    if (err)
    {
      DBUG_PRINT("wsrep",("wsrep::init() failed: %d", err));
      WSREP_ERROR("wsrep::init() failed: %d, must shutdown", err);
    }
    else
      wsrep_init_provider_status_variables();
    return err;
  }

  if (wsrep_gtid_mode && opt_bin_log && !opt_log_slave_updates)
  {
    WSREP_ERROR("Option --log-slave-updates is required if "
                "binlog is enabled, GTID mode is on and wsrep provider "
                "is specified");
    return 1;
  }

  if (!wsrep_data_home_dir || strlen(wsrep_data_home_dir) == 0)
    wsrep_data_home_dir= mysql_real_data_home;

  Wsrep_server_state::init_provider_services();
  if (Wsrep_server_state::instance().load_provider(
      wsrep_provider,
      wsrep_provider_options,
      Wsrep_server_state::instance().provider_services()))
  {
    WSREP_ERROR("Failed to load provider");
    Wsrep_server_state::deinit_provider_services();
    return 1;
  }

  if (!wsrep_provider_is_SR_capable() &&
      global_system_variables.wsrep_trx_fragment_size > 0)
  {
    WSREP_ERROR("The WSREP provider (%s) does not support streaming "
                "replication but wsrep_trx_fragment_size is set to a "
                "value other than 0 (%llu). Cannot continue. Either set "
                "wsrep_trx_fragment_size to 0 or use wsrep_provider that "
                "supports streaming replication.",
                wsrep_provider, global_system_variables.wsrep_trx_fragment_size);
    Wsrep_server_state::instance().deinit_provider();
    Wsrep_server_state::deinit_provider_services();
    return 1;
  }

  /* Now WSREP is fully initialized */
  global_system_variables.wsrep_on= 1;
  WSREP_ON_= wsrep_provider && *wsrep_provider && strcasecmp(wsrep_provider, WSREP_NONE);
  wsrep_service_started= 1;

  wsrep_init_provider_status_variables();
  wsrep_capabilities_export(Wsrep_server_state::instance().provider().capabilities(),
                            &wsrep_provider_capabilities);

  WSREP_DEBUG("SR storage init for: %s",
              (wsrep_SR_store_type == WSREP_SR_STORE_TABLE) ? "table" : "void");
  return 0;
}

/* Initialize wsrep thread LOCKs and CONDs */
void wsrep_thr_init()
{
  DBUG_ENTER("wsrep_thr_init");
  wsrep_config_state= new wsp::Config_state;
#ifdef HAVE_PSI_INTERFACE
  mysql_mutex_register("sql", wsrep_mutexes, array_elements(wsrep_mutexes));
  mysql_cond_register("sql", wsrep_conds, array_elements(wsrep_conds));
  mysql_file_register("sql", wsrep_files, array_elements(wsrep_files));
  mysql_thread_register("sql", wsrep_threads, array_elements(wsrep_threads));
#endif

  mysql_mutex_init(key_LOCK_wsrep_ready, &LOCK_wsrep_ready, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wsrep_ready, &COND_wsrep_ready, NULL);
  mysql_mutex_init(key_LOCK_wsrep_sst, &LOCK_wsrep_sst, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wsrep_sst, &COND_wsrep_sst, NULL);
  mysql_mutex_init(key_LOCK_wsrep_sst_init, &LOCK_wsrep_sst_init, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wsrep_sst_init, &COND_wsrep_sst_init, NULL);
  mysql_mutex_init(key_LOCK_wsrep_replaying, &LOCK_wsrep_replaying, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wsrep_replaying, &COND_wsrep_replaying, NULL);
  mysql_mutex_init(key_LOCK_wsrep_slave_threads, &LOCK_wsrep_slave_threads, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wsrep_slave_threads, &COND_wsrep_slave_threads, NULL);
  mysql_mutex_init(key_LOCK_wsrep_gtid_wait_upto, &LOCK_wsrep_gtid_wait_upto, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wsrep_cluster_config, &LOCK_wsrep_cluster_config, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wsrep_desync, &LOCK_wsrep_desync, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wsrep_config_state, &LOCK_wsrep_config_state, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wsrep_group_commit, &LOCK_wsrep_group_commit, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wsrep_SR_pool,
                   &LOCK_wsrep_SR_pool, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wsrep_SR_store,
                   &LOCK_wsrep_SR_store, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wsrep_joiner_monitor,
                   &LOCK_wsrep_joiner_monitor, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_LOCK_wsrep_donor_monitor,
                   &LOCK_wsrep_donor_monitor, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_wsrep_joiner_monitor, &COND_wsrep_joiner_monitor, NULL);
  mysql_cond_init(key_COND_wsrep_donor_monitor, &COND_wsrep_donor_monitor, NULL);

  DBUG_VOID_RETURN;
}

void wsrep_init_startup (bool sst_first)
{
  if (wsrep_init()) unireg_abort(1);

  /*
    Pre-initialize global_system_variables.table_plugin with a dummy engine
    (placeholder) required during the initialization of wsrep threads (THDs).
    (see: plugin_thdvar_init())
    Note: This only needs to be done for rsync & mariabackup based SST methods.
    In case of mysqldump SST method, the wsrep threads are created after the
    server plugins & global system variables are initialized.
  */
  if (wsrep_before_SE())
    wsrep_plugins_pre_init();

  /* Skip replication start if dummy wsrep provider is loaded */
  if (!wsrep_provider || !*wsrep_provider ||
      !strcasecmp(wsrep_provider, WSREP_NONE)) return;

  /* Skip replication start if no cluster address */
  if (!wsrep_cluster_address_exists()) return;

  /*
    Read value of wsrep_new_cluster before wsrep_start_replication(),
    the value is reset to FALSE inside wsrep_start_replication.
  */
  if (!wsrep_start_replication(wsrep_cluster_address)) unireg_abort(1);

  wsrep_create_rollbacker();
  wsrep_create_appliers(1);

  if (Wsrep_server_state::init_options())
  {
    WSREP_WARN("Failed to initialize provider options");
  }

  Wsrep_server_state& server_state= Wsrep_server_state::instance();
  /*
    If the SST happens before server initialization, wait until the server
    state reaches initializing. This indicates that
    either SST was not necessary or SST has been delivered.

    With mysqldump SST (!sst_first) wait until the server reaches
    joiner state and proceed to accepting connections.
  */
  int err= 0;
  if (sst_first)
  {
    err= server_state.wait_until_state(Wsrep_server_state::s_initializing);
  }
  else
  {
    err= server_state.wait_until_state(Wsrep_server_state::s_joiner);
  }
  if (err)
  {
    WSREP_ERROR("Wsrep startup was interrupted");
    unireg_abort(1);
  }
}


void wsrep_deinit(bool free_options)
{
  DBUG_ASSERT(wsrep_inited == 1);
  WSREP_DEBUG("wsrep_deinit");

  Wsrep_server_state::deinit_provider();
  Wsrep_server_state::deinit_provider_services();

  provider_name[0]=    '\0';
  provider_version[0]= '\0';
  provider_vendor[0]=  '\0';

  wsrep_inited= 0;

  if (wsrep_provider_capabilities != NULL)
  {
    char* p= wsrep_provider_capabilities;
    wsrep_provider_capabilities= NULL;
    free(p);
  }
}

/* Destroy wsrep thread LOCKs and CONDs */
void wsrep_thr_deinit()
{
  if (!wsrep_config_state)
    return;                                     // Never initialized
  WSREP_DEBUG("wsrep_thr_deinit");
  mysql_mutex_destroy(&LOCK_wsrep_ready);
  mysql_cond_destroy(&COND_wsrep_ready);
  mysql_mutex_destroy(&LOCK_wsrep_sst);
  mysql_cond_destroy(&COND_wsrep_sst);
  mysql_mutex_destroy(&LOCK_wsrep_sst_init);
  mysql_cond_destroy(&COND_wsrep_sst_init);
  mysql_mutex_destroy(&LOCK_wsrep_replaying);
  mysql_cond_destroy(&COND_wsrep_replaying);
  mysql_mutex_destroy(&LOCK_wsrep_gtid_wait_upto);
  mysql_mutex_destroy(&LOCK_wsrep_slave_threads);
  mysql_cond_destroy(&COND_wsrep_slave_threads);
  mysql_mutex_destroy(&LOCK_wsrep_cluster_config);
  mysql_mutex_destroy(&LOCK_wsrep_desync);
  mysql_mutex_destroy(&LOCK_wsrep_config_state);
  mysql_mutex_destroy(&LOCK_wsrep_group_commit);
  mysql_mutex_destroy(&LOCK_wsrep_SR_pool);
  mysql_mutex_destroy(&LOCK_wsrep_SR_store);
  mysql_mutex_destroy(&LOCK_wsrep_joiner_monitor);
  mysql_mutex_destroy(&LOCK_wsrep_donor_monitor);
  mysql_cond_destroy(&COND_wsrep_joiner_monitor);
  mysql_cond_destroy(&COND_wsrep_donor_monitor);

  delete wsrep_config_state;
  wsrep_config_state= 0;                        // Safety

  if (wsrep_cluster_capabilities != NULL)
  {
    char* p= wsrep_cluster_capabilities;
    wsrep_cluster_capabilities= NULL;
    free(p);
  }
}

void wsrep_recover()
{
  char uuid_str[40];

  if (wsrep_uuid_compare(&local_uuid, &WSREP_UUID_UNDEFINED) == 0 &&
      local_seqno == -2)
  {
    wsrep_uuid_print(&local_uuid, uuid_str, sizeof(uuid_str));
    WSREP_INFO("Position %s:%lld given at startup, skipping position recovery",
               uuid_str, (long long)local_seqno);
    return;
  }
  wsrep::gtid gtid= wsrep_get_SE_checkpoint<wsrep::gtid>();
  std::ostringstream oss;
  oss << gtid;
  if (wsrep_gtid_mode)
  {
    wsrep_server_gtid_t server_gtid=  wsrep_get_SE_checkpoint<wsrep_server_gtid_t>();
    WSREP_INFO("Recovered position: %s,%d-%d-%llu", oss.str().c_str(), server_gtid.domain_id,
                server_gtid.server_id, server_gtid.seqno);
  }
  else
  {
    WSREP_INFO("Recovered position: %s", oss.str().c_str());
  }
}

static void wsrep_stop_replication_common(THD *thd)
{
  if (Wsrep_server_state::instance().state() !=
      Wsrep_server_state::s_disconnected)
  {
    WSREP_DEBUG("Disconnect provider");
    Wsrep_server_state::instance().disconnect();
    if (Wsrep_server_state::instance().wait_until_state(
            Wsrep_server_state::s_disconnected))
    {
      WSREP_WARN("Wsrep interrupted while waiting for disconnected state");
    }
  }

  /* my connection, should not terminate with
     wsrep_close_client_connections(), make transaction to rollback */
  if (thd && !thd->wsrep_applier)
    trans_rollback(thd);
  wsrep_close_client_connections(TRUE, thd);

  /* wait until appliers have stopped */
  wsrep_wait_appliers_close(thd);

  node_uuid= WSREP_UUID_UNDEFINED;
}

void wsrep_stop_replication(THD *thd)
{
  WSREP_INFO("Stop replication by %llu", (thd) ? thd->thread_id : 0);
  wsrep_stop_replication_common(thd);
}

void wsrep_shutdown_replication()
{
  WSREP_INFO("Shutdown replication");
  wsrep_stop_replication_common(nullptr);
  /* Undocking the thread specific data. */
  set_current_thd(nullptr);
}

bool wsrep_start_replication(const char *wsrep_cluster_address)
{
  int rcode;
  WSREP_DEBUG("wsrep_start_replication");

  /*
    if provider is trivial, don't even try to connect,
    but resume local node operation
  */
  if (!WSREP_PROVIDER_EXISTS)
  {
    // enable normal operation in case no provider is specified
    return true;
  }

  DBUG_ASSERT(wsrep_cluster_address[0]);

  // --wsrep-new-cluster flag is not used, checking wsrep_cluster_address
  // it should match gcomm:// only to be considered as bootstrap node.
  // This logic is used in galera.
  if (!wsrep_new_cluster && 
      (strlen(wsrep_cluster_address) == 8) &&
      !strncmp(wsrep_cluster_address, "gcomm://", 8)) 
  {
    wsrep_new_cluster= true;
  }

  bool const bootstrap(TRUE == wsrep_new_cluster);

  WSREP_INFO("Start replication");

  if ((rcode= Wsrep_server_state::instance().connect(
        wsrep_cluster_name,
        wsrep_cluster_address,
        wsrep_sst_donor,
        bootstrap)))
  {
    DBUG_PRINT("wsrep",("wsrep_ptr->connect(%s) failed: %d",
                        wsrep_cluster_address, rcode));
    WSREP_ERROR("wsrep::connect(%s) failed: %d",
                wsrep_cluster_address, rcode);
    return false;
  }
  else
  {
    try
    {
      std::string opts= Wsrep_server_state::instance().provider().options();
      wsrep_provider_options_init(opts.c_str());
    }
    catch (const wsrep::runtime_error&)
    {
      WSREP_WARN("Failed to get wsrep options");
    }
  }

  return true;
}

bool wsrep_check_mode (enum_wsrep_mode mask)
{
  return wsrep_mode & mask;
}

//seconds after which the limit warnings suppression will be activated
#define WSREP_WARNING_ACTIVATION_TIMEOUT 5*60
//number of limit warnings after which the suppression will be activated
#define WSREP_WARNING_ACTIVATION_THRESHOLD 10

enum wsrep_warning_type {
  WSREP_DISABLED = 0,
  WSREP_REQUIRE_PRIMARY_KEY= 1,
  WSREP_REQUIRE_INNODB= 2,
  WSREP_EXPERIMENTAL= 3,
  WSREP_REQUIRE_MAX=4,
};

static ulonglong wsrep_warning_start_time=0;
static bool wsrep_warning_active[WSREP_REQUIRE_MAX+1];
static ulonglong wsrep_warning_count[WSREP_REQUIRE_MAX+1];
static ulonglong wsrep_total_warnings_count=0;

/**
  Auxiliary function to reset the limit of wsrep warnings.
  This is done without mutex protection, but this should be good
  enough as it doesn't matter if we loose a couple of suppressed
  messages or if this is called multiple times.
*/

static void wsrep_reset_warnings(ulonglong now)
{
  uint i;

  wsrep_warning_start_time= now;
  wsrep_total_warnings_count= 0;

  for (i= 0 ; i < WSREP_REQUIRE_MAX ; i++)
  {
    wsrep_warning_active[i]= false;
    wsrep_warning_count[i]= 0;
  }
}

static const char* wsrep_warning_name(const enum wsrep_warning_type type)
{
  switch(type)
  {
  case WSREP_REQUIRE_PRIMARY_KEY:
    return "WSREP_REQUIRE_PRIMARY_KEY"; break;
  case WSREP_REQUIRE_INNODB:
    return "WSREP_REQUIRE_INNODB"; break;
  case WSREP_EXPERIMENTAL:
    return "WSREP_EXPERIMENTAL"; break;

  default: assert(0); return " "; break; // for compiler
  }
}
/**
  Auxiliary function to check if the warning statements should be
  thrown or suppressed.

  Logic is:
  - If we get more than WSREP_WARNING_ACTIVATION_THRESHOLD errors
    of one type, that type of errors will be suppressed for
    WSREP_WARNING_ACTIVATION_TIMEOUT.
  - When the time limit has been reached, all suppressions are reset.

  This means that if one gets many different types of errors, some of them
  may be reset less than WSREP_WARNING_ACTIVATION_TIMEOUT. However at
  least one error is disabled for this time.

  SYNOPSIS:
  @params
   warning_type - The type of warning.

  RETURN:
    0   0k to log
    1   Message suppressed
*/

static bool wsrep_protect_against_warning_flood(
              enum wsrep_warning_type warning_type)
{
  ulonglong count;
  ulonglong now= my_interval_timer()/1000000000ULL;

  count= ++wsrep_warning_count[warning_type];
  wsrep_total_warnings_count++;

  /*
    INITIALIZING:
    If this is the first time this function is called with log warning
    enabled, the monitoring the warnings should start.
  */
  if (wsrep_warning_start_time == 0)
  {
    wsrep_reset_warnings(now);
    return false;
  }

  /*
    The following is true if we got too many errors or if the error was
    already suppressed
  */
  if (count >= WSREP_WARNING_ACTIVATION_THRESHOLD)
  {
    ulonglong diff_time= (now - wsrep_warning_start_time);

    if (!wsrep_warning_active[warning_type])
    {
      /*
        ACTIVATION:
        We got WSREP_WARNING_ACTIVATION_THRESHOLD warnings in
        less than WSREP_WARNING_ACTIVATION_TIMEOUT we activate the
        suppression.
      */
      if (diff_time <= WSREP_WARNING_ACTIVATION_TIMEOUT)
      {
        wsrep_warning_active[warning_type]= true;
        WSREP_INFO("Suppressing warnings of type '%s' for up to %d seconds because of flooding",
                   wsrep_warning_name(warning_type),
                   WSREP_WARNING_ACTIVATION_TIMEOUT);
      }
      else
      {
        /*
          There is no flooding till now, therefore we restart the monitoring
        */
        wsrep_reset_warnings(now);
      }
    }
    else
    {
      /* This type of warnings was suppressed */
      if (diff_time > WSREP_WARNING_ACTIVATION_TIMEOUT)
      {
        ulonglong save_count= wsrep_total_warnings_count;
        /* Print a suppression note and remove the suppression */
        wsrep_reset_warnings(now);
        WSREP_INFO("Suppressed %lu unsafe warnings during "
                              "the last %d seconds",
                              save_count, (int) diff_time);
      }
    }
  }

  return wsrep_warning_active[warning_type];
}

/**
  Auxiliary function to push warning to client and to the error log
*/
static void wsrep_push_warning(THD *thd,
                               enum wsrep_warning_type type,
                               const handlerton *hton,
                               const TABLE_LIST *tables)
{
  switch(type)
  {
  case WSREP_REQUIRE_PRIMARY_KEY:
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_OPTION_PREVENTS_STATEMENT,
                        "WSREP: wsrep_mode = REQUIRED_PRIMARY_KEY enabled. "
                        "Table '%s'.'%s' should have PRIMARY KEY defined.",
                        tables->db.str, tables->table_name.str);
    if (global_system_variables.log_warnings > 1 &&
	!wsrep_protect_against_warning_flood(type))
      WSREP_WARN("wsrep_mode = REQUIRED_PRIMARY_KEY enabled. "
                 "Table '%s'.'%s' should have PRIMARY KEY defined",
                 tables->db.str, tables->table_name.str);
    break;
  case WSREP_REQUIRE_INNODB:
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_OPTION_PREVENTS_STATEMENT,
                        "WSREP: wsrep_mode = STRICT_REPLICATION enabled. "
                        "Storage engine %s for table '%s'.'%s' is "
                        "not supported in Galera",
                        ha_resolve_storage_engine_name(hton),
                        tables->db.str, tables->table_name.str);
    if (global_system_variables.log_warnings > 1 &&
	!wsrep_protect_against_warning_flood(type))
      WSREP_WARN("wsrep_mode = STRICT_REPLICATION enabled. "
                 "Storage engine %s for table '%s'.'%s' is "
                 "not supported in Galera",
                 ha_resolve_storage_engine_name(hton),
                 tables->db.str, tables->table_name.str);
    break;
  case WSREP_EXPERIMENTAL:
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_ERROR_DURING_COMMIT,
                        "WSREP: Replication of non-transactional engines is experimental. "
                        "Storage engine %s for table '%s'.'%s' is "
                        "not supported in Galera",
                        ha_resolve_storage_engine_name(hton),
                        tables->db.str, tables->table_name.str);
    if (global_system_variables.log_warnings > 1 &&
	!wsrep_protect_against_warning_flood(type))
      WSREP_WARN("Replication of non-transactional engines is experimental. "
                 "Storage engine %s for table '%s'.'%s' is "
                 "not supported in Galera",
                 ha_resolve_storage_engine_name(hton),
                 tables->db.str, tables->table_name.str);
    break;
  default: assert(0); break;
  }
}

bool wsrep_check_mode_after_open_table (THD *thd,
	const handlerton *hton,
	TABLE_LIST *tables)
{
  enum_sql_command sql_command= thd->lex->sql_command;
  bool is_dml_stmt= (thd->get_command() != COM_STMT_PREPARE &&
                     (sql_command_flags[sql_command] & CF_WSREP_BASIC_DML));

  if (!is_dml_stmt)
    return true;

  const legacy_db_type db_type= hton->db_type;
  bool replicate= ((db_type == DB_TYPE_MYISAM && wsrep_check_mode(WSREP_MODE_REPLICATE_MYISAM)) ||
                   (db_type == DB_TYPE_ARIA && wsrep_check_mode(WSREP_MODE_REPLICATE_ARIA)));
  TABLE *tbl= tables->table;

  DBUG_ASSERT(tbl);
  if (replicate)
  {
    /*
      It is not recommended to replicate MyISAM as it lacks rollback
      feature but if user demands then actions are replicated using TOI.
      Following code will kick-start the TOI but this has to be done
      only once per statement.

      Note: kick-start will take care of creating isolation key for
      all tables involved in the list (provided all of them are MYISAM
      or Aria tables).
    */
    if (tbl->s->table_category != TABLE_CATEGORY_STATISTICS)
    {
      if (tbl->s->primary_key == MAX_KEY &&
          wsrep_check_mode(WSREP_MODE_REQUIRED_PRIMARY_KEY))
      {
        /* Other replicated table doesn't have explicit primary-key defined. */
        wsrep_push_warning(thd, WSREP_REQUIRE_PRIMARY_KEY, hton, tables);
      }

      // Check are we inside a transaction
      bool not_used;
      uint rw_ha_count= ha_check_and_coalesce_trx_read_only(thd, thd->transaction->all.ha_list, true, &not_used);
      bool changes= wsrep_has_changes(thd);

      // Roll back current stmt if exists
      wsrep_before_rollback(thd, true);
      wsrep_after_rollback(thd, true);
      wsrep_after_statement(thd);

      // If there is updates, they would be lost above rollback
      if (rw_ha_count > 0 && changes)
      {
	my_message(ER_ERROR_DURING_COMMIT, "Transactional commit not supported "
                     "by involved engine(s)", MYF(0));
        wsrep_push_warning(thd, WSREP_EXPERIMENTAL, hton, tables);
	return false;
      }

      WSREP_TO_ISOLATION_BEGIN(NULL, NULL, (tables));
    }
  } else if (db_type != DB_TYPE_UNKNOWN &&
             db_type != DB_TYPE_PERFORMANCE_SCHEMA)
  {
    bool is_system_db= (tbl &&
                        (!strcmp(tbl->s->db.str, "mysql") ||
                         (tbl->s->table_category ==
                          TABLE_CATEGORY_INFORMATION &&
                          !strcmp(tbl->s->db.str, "information_schema"))));

    if (!is_system_db &&
	!is_temporary_table(tables))
    {
      if (db_type != DB_TYPE_INNODB &&
	  wsrep_check_mode(WSREP_MODE_STRICT_REPLICATION))
      {
        /* Table is not an InnoDB table and strict replication is requested*/
        wsrep_push_warning(thd, WSREP_REQUIRE_INNODB, hton, tables);
      }

      if (tbl->s->primary_key == MAX_KEY &&
          db_type == DB_TYPE_INNODB &&
          wsrep_check_mode(WSREP_MODE_REQUIRED_PRIMARY_KEY))
      {
        /* InnoDB table doesn't have explicit primary-key defined. */
        wsrep_push_warning(thd, WSREP_REQUIRE_PRIMARY_KEY, hton, tables);
      }

      if (db_type != DB_TYPE_INNODB &&
	  thd->variables.sql_log_bin == 1 &&
	  wsrep_check_mode(WSREP_MODE_DISALLOW_LOCAL_GTID))
      {
        /* Table is not an InnoDB table and local GTIDs are disallowed */
        my_error(ER_GALERA_REPLICATION_NOT_SUPPORTED, MYF(0));
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_OPTION_PREVENTS_STATEMENT,
                            "You can't execute statements that would generate local "
                            "GTIDs when wsrep_mode = DISALLOW_LOCAL_GTID is set. "
                            "Try disabling binary logging with SET sql_log_bin=0 "
                            "to execute this statement.");
        goto wsrep_error_label;
      }
    }
  }

  return true;

wsrep_error_label:
  return false;
}

bool wsrep_check_mode_before_cmd_execute (THD *thd)
{
  bool ret= true;
  if (wsrep_check_mode(WSREP_MODE_BINLOG_ROW_FORMAT_ONLY) &&
      !thd->is_current_stmt_binlog_format_row() && is_update_query(thd->lex->sql_command))
  {
    my_error(ER_GALERA_REPLICATION_NOT_SUPPORTED, MYF(0));
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_OPTION_PREVENTS_STATEMENT,
                        "WSREP: wsrep_mode = BINLOG_ROW_FORMAT_ONLY enabled. Only ROW binlog format is supported.");
    ret= false;
  }
  if (wsrep_check_mode(WSREP_MODE_REQUIRED_PRIMARY_KEY) &&
      thd->lex->sql_command == SQLCOM_CREATE_TABLE)
  {
    Key *key;
    List_iterator<Key> key_iterator(thd->lex->alter_info.key_list);
    bool primary_key_found= false;
    while ((key= key_iterator++))
    {
      if (key->type == Key::PRIMARY)
      {
        primary_key_found= true;
        break;
      }
    }
    if (!primary_key_found)
    {
      my_error(ER_GALERA_REPLICATION_NOT_SUPPORTED, MYF(0));
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_OPTION_PREVENTS_STATEMENT,
                          "WSREP: wsrep_mode = REQUIRED_PRIMARY_KEY enabled. Table should have PRIMARY KEY defined.");
      ret= false;
    }
  }
  return ret;
}

bool wsrep_must_sync_wait (THD* thd, uint mask)
{
  bool ret= 0;
  if (thd->variables.wsrep_on)
  {
    mysql_mutex_lock(&thd->LOCK_thd_data);
    ret= (thd->variables.wsrep_sync_wait & mask) &&
      thd->wsrep_client_thread &&
      WSREP_ON &&
      !(thd->variables.wsrep_dirty_reads &&
        !is_update_query(thd->lex->sql_command)) &&
      !thd->in_active_multi_stmt_transaction() &&
      thd->wsrep_trx().state() !=
      wsrep::transaction::s_replaying &&
      thd->wsrep_cs().sync_wait_gtid().is_undefined();
    mysql_mutex_unlock(&thd->LOCK_thd_data);
  }
  return ret;
}

bool wsrep_sync_wait (THD* thd, uint mask)
{
  if (wsrep_must_sync_wait(thd, mask))
  {
    WSREP_DEBUG("wsrep_sync_wait: thd->variables.wsrep_sync_wait= %u, "
                "mask= %u, thd->variables.wsrep_on= %d",
                thd->variables.wsrep_sync_wait, mask,
                thd->variables.wsrep_on);
    /*
      This allows autocommit SELECTs and a first SELECT after SET AUTOCOMMIT=0
      TODO: modify to check if thd has locked any rows.
    */
    if (thd->wsrep_cs().sync_wait(-1))
    {
      wsrep_override_error(thd, thd->wsrep_cs().current_error(),
                           thd->wsrep_cs().current_error_status());
      return true;
    }
  }

  return false;
}

enum wsrep::provider::status
wsrep_sync_wait_upto (THD*          thd,
                      wsrep_gtid_t* upto,
                      int           timeout)
{
  DBUG_ASSERT(upto);
  enum wsrep::provider::status ret;
  if (upto)
  {
    wsrep::gtid upto_gtid(wsrep::id(upto->uuid.data, sizeof(upto->uuid.data)),
                          wsrep::seqno(upto->seqno));
    ret= Wsrep_server_state::instance().wait_for_gtid(upto_gtid, timeout);
  }
  else
  {
    ret= Wsrep_server_state::instance().causal_read(timeout).second;
  }
  WSREP_DEBUG("wsrep_sync_wait_upto: %d", ret);
  return ret;
}

bool wsrep_is_show_query(enum enum_sql_command command)
{
  DBUG_ASSERT(command >= 0 && command <= SQLCOM_END);
  return (sql_command_flags[command] & CF_STATUS_COMMAND) != 0;
}

static bool wsrep_is_diagnostic_query(enum enum_sql_command command)
{
  assert(command >= 0 && command <= SQLCOM_END);
  return (sql_command_flags[command] & CF_DIAGNOSTIC_STMT) != 0;
}

static enum enum_wsrep_sync_wait
wsrep_sync_wait_mask_for_command(enum enum_sql_command command)
{
  switch (command)
  {
  case SQLCOM_SELECT:
  case SQLCOM_CHECKSUM:
    return WSREP_SYNC_WAIT_BEFORE_READ;
  case SQLCOM_DELETE:
  case SQLCOM_DELETE_MULTI:
  case SQLCOM_UPDATE:
  case SQLCOM_UPDATE_MULTI:
    return WSREP_SYNC_WAIT_BEFORE_UPDATE_DELETE;
  case SQLCOM_REPLACE:
  case SQLCOM_INSERT:
  case SQLCOM_REPLACE_SELECT:
  case SQLCOM_INSERT_SELECT:
    return WSREP_SYNC_WAIT_BEFORE_INSERT_REPLACE;
  default:
    if (wsrep_is_diagnostic_query(command))
    {
      return WSREP_SYNC_WAIT_NONE;
    }
    if (wsrep_is_show_query(command))
    {
      switch (command)
      {
      case SQLCOM_SHOW_PROFILE:
      case SQLCOM_SHOW_PROFILES:
      case SQLCOM_SHOW_SLAVE_HOSTS:
      case SQLCOM_SHOW_RELAYLOG_EVENTS:
      case SQLCOM_SHOW_SLAVE_STAT:
      case SQLCOM_SHOW_BINLOG_STAT:
      case SQLCOM_SHOW_ENGINE_STATUS:
      case SQLCOM_SHOW_ENGINE_MUTEX:
      case SQLCOM_SHOW_ENGINE_LOGS:
      case SQLCOM_SHOW_PROCESSLIST:
      case SQLCOM_SHOW_PRIVILEGES:
        return WSREP_SYNC_WAIT_NONE;
      default:
        return WSREP_SYNC_WAIT_BEFORE_SHOW;
      }
    }
  }
  return WSREP_SYNC_WAIT_NONE;
}

bool wsrep_sync_wait(THD* thd, enum enum_sql_command command)
{
  bool res = false;
  if (WSREP_CLIENT(thd) && thd->variables.wsrep_sync_wait)
    res = wsrep_sync_wait(thd, wsrep_sync_wait_mask_for_command(command));
  return res;
}

typedef struct wsrep_key_arr
{
    wsrep_key_t* keys;
    size_t       keys_len;
} wsrep_key_arr_t;

static void wsrep_keys_free(wsrep_key_arr_t* key_arr)
{
    for (size_t i= 0; i < key_arr->keys_len; ++i)
    {
        my_free((void*)key_arr->keys[i].key_parts);
    }
    my_free(key_arr->keys);
    key_arr->keys= 0;
    key_arr->keys_len= 0;
}

class Unknown_storage_engine_handler : public Internal_error_handler
{
public:
  Unknown_storage_engine_handler()
    : m_handled_errors(0), m_unhandled_errors(0)
  {}

  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char* sqlstate,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition ** cond_hdl) override
  {
    *cond_hdl= NULL;
    if (sql_errno == ER_UNKNOWN_STORAGE_ENGINE)
    {
      m_handled_errors++;
    }
    else if (*level == Sql_condition::WARN_LEVEL_ERROR)
    {
      m_unhandled_errors++;
    }
    return FALSE;
  }

  bool safely_trapped_errors()
  {
    return ((m_handled_errors > 0) && (m_unhandled_errors == 0));
  }

private:
  int m_handled_errors;
  int m_unhandled_errors;
};

/*!
 * @param thd    thread
 * @param tables list of tables
 * @param keys   prepared keys

 * @return 0 if parent table append was successful, non-zero otherwise.
*/
bool wsrep_append_fk_parent_table(THD *thd, TABLE_LIST *tables,
                                  wsrep::key_array *keys)
{
  assert(wsrep_thd_is_local(thd));

  bool fail= false;
  Open_table_context ot_ctx(thd, MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL);

  for (TABLE_LIST *table= tables; table; table= table->next_local)
  {
    if (!table->table)
    {
      TABLE_LIST *save_next_global= table->next_global;
      TABLE_LIST::enum_open_strategy save_open_strategy= table->open_strategy;
      table->open_strategy= TABLE_LIST::OPEN_IF_EXISTS;

      Unknown_storage_engine_handler no_storage_engine;
      thd->push_internal_handler(&no_storage_engine);

      if (open_table(thd, table, &ot_ctx))
      {
        if (no_storage_engine.safely_trapped_errors())
        {
          Diagnostics_area *da= thd->get_stmt_da();
          da->reset_diagnostics_area();
          da->clear_warning_info(thd->query_id);
        }
        else
        {
          fail= true;
        }
      }

      thd->pop_internal_handler();

      table->next_global= save_next_global;
      table->open_strategy= save_open_strategy;

      if (fail)
      {
        WSREP_DEBUG("Unable to open table for FK checks for %s",
                    wsrep_thd_query(thd));
        goto exit;
      }
    }

    if (table->table && !is_temporary_table(table))
    {
      FOREIGN_KEY_INFO *f_key_info;
      List<FOREIGN_KEY_INFO> f_key_list;

      table->table->file->get_foreign_key_list(thd, &f_key_list);
      List_iterator_fast<FOREIGN_KEY_INFO> it(f_key_list);
      while ((f_key_info= it++))
      {
        WSREP_DEBUG("appended fkey %s", f_key_info->referenced_table->str);
        keys->push_back(wsrep_prepare_key_for_toi(
            f_key_info->referenced_db->str, f_key_info->referenced_table->str,
            wsrep::key::shared));
      }
    }
  }

exit:
  DEBUG_SYNC(thd, "wsrep_append_fk_toi_keys_before_close_tables");

  /* close the table and release MDL locks */
  close_thread_tables(thd);
  thd->mdl_context.rollback_to_savepoint(ot_ctx.start_of_statement_svp());
  for (TABLE_LIST *table= tables; table; table= table->next_global)
  {
    table->table= NULL;
    table->mdl_request.ticket= NULL;
  }

  /*
    Reopen temporary tables if necessary.
    DROP TABLE pre-opens temporary tables, but the corresponding
    command does not have the CF_PREOPEN_TMP_TABLES flag set.
  */
  const bool preopen_tmp_tables=
      thd->lex->sql_command == SQLCOM_DROP_TABLE ||
      (sql_command_flags[thd->lex->sql_command] & CF_PREOPEN_TMP_TABLES);

  if (preopen_tmp_tables && thd->open_temporary_tables(tables))
  {
    WSREP_INFO("Unable to reopen temporary tables after FK checks");
    fail= true;
  }

  /*
    MDEV-32938: Check if DDL operation has been killed before.

    It may be that during collecting foreign keys this operation gets
    BF-aborted by another already-running TOI operation because it got MDL
    locks on the same table for checking foreign keys. After
    `close_thread_tables()` has been called it's safe to assume that no-one can
    BF-abort this operation as it's not holding any MDL locks any more.
  */
  if (!fail)
  {
    mysql_mutex_lock(&thd->LOCK_thd_kill);
    if (thd->killed)
    {
      fail= true;
    }
    mysql_mutex_unlock(&thd->LOCK_thd_kill);
  }

  return fail;
}

bool wsrep_reload_ssl()
{
  try
  {
    std::string opts= Wsrep_server_state::instance().provider().options();
    if (opts.find("socket.ssl_reload") == std::string::npos)
    {
      WSREP_DEBUG("Option `socket.ssl_reload` not found in parameters.");
      return false;
    }
    const std::string reload_ssl_param("socket.ssl_reload=1");
    enum wsrep::provider::status ret= Wsrep_server_state::instance().provider().options(reload_ssl_param);
    if (ret)
    {
      WSREP_ERROR("Set options returned %d", ret);
      return true;
    }
    return false;
  }
  catch (...)
  {
    WSREP_ERROR("Failed to get provider options");
    return true;
  }
}

bool wsrep_split_allowlist(std::vector<std::string>& allowlist)
{
  if (!wsrep_allowlist || 0 == strlen(wsrep_allowlist))
  {
    return false;
  }
  std::istringstream ss{wsrep_allowlist};
  std::string token;
  while (std::getline(ss, token, ',')) 
  {
    if (!token.empty()) 
    {
      struct sockaddr_in sa_4;
      struct sockaddr_in6 sa_6;
      if ((inet_pton(AF_INET, token.c_str(), &(sa_4.sin_addr)) != 0) ||
          (inet_pton(AF_INET6, token.c_str(), &(sa_6.sin6_addr)) != 0))
      {
        allowlist.push_back(token);
      }
      else
      {
        WSREP_WARN("Invalid IP address %s provided in `wsrep_allowlist` variable", token.c_str());
      }
    }
  }
  return allowlist.size();
}

/*!
 * @param db      Database string
 * @param table   Table string
 * @param key     Array of wsrep_key_t
 * @param key_len In: number of elements in key array, Out: number of
 *                elements populated
 *
 * @return true if preparation was successful, otherwise false.
 */

static bool wsrep_prepare_key_for_isolation(const char* db,
                                           const char* table,
                                           wsrep_buf_t* key,
                                           size_t* key_len)
{
  if (*key_len < 2) return false;

  switch (wsrep_protocol_version)
  {
  case 0:
    *key_len= 0;
    break;
  case 1:
  case 2:
  case 3:
  case 4:
  {
    *key_len= 0;
    if (db)
    {
      key[*key_len].ptr= db;
      key[*key_len].len= strlen(db);
      ++(*key_len);
      if (table)
      {
        key[*key_len].ptr= table;
        key[*key_len].len= strlen(table);
        ++(*key_len);
      }
    }
    break;
  }
  default:
    assert(0);
    WSREP_ERROR("Unsupported protocol version: %ld", wsrep_protocol_version);
    unireg_abort(1);
    return false;
  }

    return true;
}

static bool wsrep_prepare_key_for_isolation(const char* db,
                                            const char* table,
                                            wsrep_key_arr_t* ka)
{
  wsrep_key_t* tmp;
  tmp= (wsrep_key_t*)my_realloc(PSI_INSTRUMENT_ME, ka->keys,
                                (ka->keys_len + 1) * sizeof(wsrep_key_t),
                                MYF(MY_ALLOW_ZERO_PTR));
  if (!tmp)
  {
    WSREP_ERROR("Can't allocate memory for key_array");
    return false;
  }
  ka->keys= tmp;
  if (!(ka->keys[ka->keys_len].key_parts= (wsrep_buf_t*)
        my_malloc(PSI_INSTRUMENT_ME, sizeof(wsrep_buf_t)*2, MYF(0))))
  {
    WSREP_ERROR("Can't allocate memory for key_parts");
    return false;
  }
  ka->keys[ka->keys_len].key_parts_num= 2;
  ++ka->keys_len;
  if (!wsrep_prepare_key_for_isolation(db, table,
                                       (wsrep_buf_t*)ka->keys[ka->keys_len - 1].key_parts,
                                       &ka->keys[ka->keys_len - 1].key_parts_num))
  {
    WSREP_ERROR("Preparing keys for isolation failed");
    return false;
  }

  return true;
}

static bool wsrep_prepare_keys_for_alter_add_fk(const char* child_table_db,
                                                const Alter_info* alter_info,
                                                wsrep_key_arr_t* ka)
{
  Key *key;
  List_iterator<Key> key_iterator(const_cast<Alter_info*>(alter_info)->key_list);
  while ((key= key_iterator++))
  {
    if (key->type == Key::FOREIGN_KEY)
    {
      Foreign_key *fk_key= (Foreign_key *)key;
      const char *db_name= fk_key->ref_db.str;
      const char *table_name= fk_key->ref_table.str;
      if (!db_name)
      {
        db_name= child_table_db;
      }
      if (!wsrep_prepare_key_for_isolation(db_name, table_name, ka))
      {
        return false;
      }
    }
  }
  return true;
}

static bool wsrep_prepare_keys_for_isolation(THD*              thd,
                                             const char*       db,
                                             const char*       table,
                                             const TABLE_LIST* table_list,
                                             Alter_info*       alter_info,
                                             wsrep_key_arr_t*  ka)
{
  ka->keys= 0;
  ka->keys_len= 0;

  if (db || table)
  {
    if (!wsrep_prepare_key_for_isolation(db, table, ka))
      goto err;
  }

  for (const TABLE_LIST* table= table_list; table; table= table->next_global)
  {
    if (!wsrep_prepare_key_for_isolation(table->db.str, table->table_name.str, ka))
      goto err;
  }

  if (alter_info)
  {
    if (!wsrep_prepare_keys_for_alter_add_fk(table_list->db.str, alter_info, ka))
      goto err;
  }
  return false;

err:
    wsrep_keys_free(ka);
    return true;
}

/*
 * Prepare key list from db/table and table_list and append it to Wsrep
 * with the given key type.
 *
 * Return zero in case of success, 1 in case of failure.
 */
int wsrep_append_table_keys(THD* thd,
                            TABLE_LIST* first_table,
                            TABLE_LIST* table_list,
                            Wsrep_service_key_type key_type)
{
   wsrep_key_arr_t key_arr= {0, 0};
   const char* db_name= first_table ? first_table->db.str : NULL;
   const char* table_name= first_table ? first_table->table_name.str : NULL;
   int rcode= wsrep_prepare_keys_for_isolation(thd, db_name, table_name,
                                               table_list, NULL, &key_arr);

   if (!rcode && key_arr.keys_len)
   {
     rcode= wsrep_thd_append_key(thd, key_arr.keys,
                                 key_arr.keys_len, key_type);
   }

   wsrep_keys_free(&key_arr);
   return rcode;
}

extern "C" int wsrep_thd_append_table_key(MYSQL_THD thd,
                                    const char* db,
                                    const char* table,
                                    enum Wsrep_service_key_type key_type)
{
  wsrep_key_arr_t key_arr = {0, 0};
  int ret = wsrep_prepare_keys_for_isolation(thd, db, table, NULL, NULL, &key_arr);
  ret = ret || wsrep_thd_append_key(thd, key_arr.keys,
                                    (int)key_arr.keys_len, key_type);
  wsrep_keys_free(&key_arr);
  return ret;
}

bool wsrep_prepare_key(const uchar* cache_key, size_t cache_key_len,
                       const uchar* row_id, size_t row_id_len,
                       wsrep_buf_t* key, size_t* key_len)
{
    if (*key_len < 3) return false;

    *key_len= 0;
    switch (wsrep_protocol_version)
    {
    case 0:
    {
        key[0].ptr= cache_key;
        key[0].len= cache_key_len;

        *key_len= 1;
        break;
    }
    case 1:
    case 2:
    case 3:
    case 4:
    {
        key[0].ptr= cache_key;
        key[0].len= strlen( (char*)cache_key );

        key[1].ptr= cache_key + strlen( (char*)cache_key ) + 1;
        key[1].len= strlen( (char*)(key[1].ptr) );

        *key_len= 2;
        break;
    }
    default:
        return false;
    }

    key[*key_len].ptr= row_id;
    key[*key_len].len= row_id_len;
    ++(*key_len);

    return true;
}

bool wsrep_prepare_key_for_innodb(THD* thd,
                                  const uchar* cache_key,
                                  size_t cache_key_len,
                                  const uchar* row_id,
                                  size_t row_id_len,
                                  wsrep_buf_t* key,
                                  size_t* key_len)
{

  return wsrep_prepare_key(cache_key, cache_key_len, row_id, row_id_len, key, key_len);
}

wsrep::key wsrep_prepare_key_for_toi(const char* db, const char* table,
                                     enum wsrep::key::type type)
{
  wsrep::key ret(type);
  DBUG_ASSERT(db);
  ret.append_key_part(db, strlen(db));
  if (table) ret.append_key_part(table, strlen(table));
  return ret;
}

wsrep::key_array
wsrep_prepare_keys_for_alter_add_fk(const char* child_table_db,
                                    const Alter_info* alter_info)

{
  wsrep::key_array ret;
  Key *key;
  List_iterator<Key> key_iterator(const_cast<Alter_info*>(alter_info)->key_list);
  while ((key= key_iterator++))
  {
    if (key->type == Key::FOREIGN_KEY)
    {
      Foreign_key *fk_key= (Foreign_key *)key;
      const char *db_name= fk_key->ref_db.str;
      const char *table_name= fk_key->ref_table.str;
      if (!db_name)
      {
        db_name= child_table_db;
      }
      ret.push_back(wsrep_prepare_key_for_toi(db_name, table_name,
                                              wsrep::key::exclusive));
    }
  }
  return ret;
}

wsrep::key_array wsrep_prepare_keys_for_toi(const char *db,
                                            const char *table,
                                            const TABLE_LIST *table_list,
                                            const Alter_info *alter_info,
                                            const wsrep::key_array *fk_tables)
{
  wsrep::key_array ret;
  if (db || table)
  {
    ret.push_back(wsrep_prepare_key_for_toi(db, table, wsrep::key::exclusive));
  }
  for (const TABLE_LIST* table= table_list; table; table= table->next_global)
  {
    ret.push_back(wsrep_prepare_key_for_toi(table->db.str, table->table_name.str,
                                            wsrep::key::exclusive));
  }
  if (alter_info)
  {
    wsrep::key_array fk(wsrep_prepare_keys_for_alter_add_fk(table_list->db.str, alter_info));
    if (!fk.empty())
    {
      ret.insert(ret.end(), fk.begin(), fk.end());
    }
  }
  if (fk_tables && !fk_tables->empty())
  {
    ret.insert(ret.end(), fk_tables->begin(), fk_tables->end());
  }
  return ret;
}

/*
 * Construct Query_log_Event from thd query and serialize it
 * into buffer.
 *
 * Return 0 in case of success, 1 in case of error.
 */
int wsrep_to_buf_helper(
    THD* thd, const char *query, uint query_len, uchar** buf, size_t* buf_len)
{
  IO_CACHE tmp_io_cache;
  enum_binlog_checksum_alg current_binlog_check_alg=
    (enum_binlog_checksum_alg) binlog_checksum_options;
  Log_event_writer writer(&tmp_io_cache, NULL, current_binlog_check_alg, NULL);
  if (open_cached_file(&tmp_io_cache, mysql_tmpdir, TEMP_PREFIX,
                       65536, MYF(MY_WME)))
    return 1;
  int ret(0);

  Format_description_log_event *tmp_fd=
    new Format_description_log_event(4, NULL, current_binlog_check_alg);
  writer.write(tmp_fd);
  delete tmp_fd;

#ifdef GTID_SUPPORT
  if (thd->variables.gtid_next.type == GTID_GROUP)
  {
      Gtid_log_event gtid_ev(thd, FALSE, &thd->variables.gtid_next);
      if (!gtid_ev.is_valid()) ret= 0;
      if (!ret && writer.write(&gtid_ev)) ret= 1;
  }
#endif /* GTID_SUPPORT */
  /*
   * Check if this is applier thread, slave_thread or
   * we have set manually WSREP GTID seqno. Add GTID event.
   */
  if (thd->slave_thread || wsrep_thd_is_applying(thd) || 
      thd->variables.wsrep_gtid_seq_no)
  {
    uint64 seqno= thd->variables.gtid_seq_no;
    uint32 domain_id= thd->variables.gtid_domain_id;
    uint32 server_id= thd->variables.server_id;
    if (!thd->variables.gtid_seq_no && thd->variables.wsrep_gtid_seq_no)
    {
      seqno= thd->variables.wsrep_gtid_seq_no;
      domain_id= wsrep_gtid_server.domain_id;
      server_id= wsrep_gtid_server.server_id;
    }
    /*
    * Ignore if both thd->variables.gtid_seq_no and
    * thd->variables.wsrep_gtid_seq_no are not set.
    */
    if (seqno)
    {
      Gtid_log_event gtid_event(thd, seqno, domain_id, true,
                                LOG_EVENT_SUPPRESS_USE_F, true, 0);
      gtid_event.server_id= server_id;
      if (!gtid_event.is_valid()) ret= 0;
      ret= writer.write(&gtid_event);
    }
  }
  /*
    It's local DDL so in case of possible gtid seqno (SET gtid_seq_no=X)
    manipulation, seqno value will be ignored.
   */
  else
  {
    thd->variables.gtid_seq_no= 0;
  }

  /* if there is prepare query, add event for it */
  if (!ret && thd->wsrep_TOI_pre_query)
  {
    Query_log_event ev(thd, thd->wsrep_TOI_pre_query,
		       thd->wsrep_TOI_pre_query_len,
		       FALSE, FALSE, FALSE, 0);
    if (writer.write(&ev)) ret= 1;
  }

  /* continue to append the actual query */
  Query_log_event ev(thd, query, query_len, FALSE, FALSE, FALSE, 0);
  /* WSREP GTID mode, we need to change server_id */
  if (wsrep_gtid_mode && !thd->variables.gtid_seq_no)
    ev.server_id= wsrep_gtid_server.server_id;
  if (!ret && writer.write(&ev)) ret= 1;
  if (!ret && wsrep_write_cache_buf(&tmp_io_cache, buf, buf_len)) ret= 1;
  close_cached_file(&tmp_io_cache);
  return ret;
}

static int
wsrep_alter_query_string(THD *thd, String *buf)
{
  /* Append the "ALTER" part of the query */
  if (buf->append(STRING_WITH_LEN("ALTER ")))
    return 1;
  /* Append definer */
  append_definer(thd, buf, &(thd->lex->definer->user), &(thd->lex->definer->host));
  /* Append the left part of thd->query after event name part */
  if (buf->append(thd->lex->stmt_definition_begin,
                  thd->lex->stmt_definition_end -
                  thd->lex->stmt_definition_begin))
    return 1;

  return 0;
}

static int wsrep_alter_event_query(THD *thd, uchar** buf, size_t* buf_len)
{
  String log_query;

  if (wsrep_alter_query_string(thd, &log_query))
  {
    WSREP_WARN("events alter string failed: schema: %s, query: %s",
               thd->get_db(), thd->query());
    return 1;
  }
  return wsrep_to_buf_helper(thd, log_query.ptr(), log_query.length(), buf, buf_len);
}

#include "sql_show.h"
static int
create_view_query(THD *thd, uchar** buf, size_t* buf_len)
{
    LEX *lex= thd->lex;
    SELECT_LEX *select_lex= lex->first_select_lex();
    TABLE_LIST *first_table= select_lex->table_list.first;
    TABLE_LIST *views= first_table;
    LEX_USER *definer;
    String buff;
    const LEX_CSTRING command[3]=
      {{ STRING_WITH_LEN("CREATE ") },
       { STRING_WITH_LEN("ALTER ") },
       { STRING_WITH_LEN("CREATE OR REPLACE ") }};

    buff.append(&command[thd->lex->create_view->mode]);

    if (lex->definer)
      definer= get_current_user(thd, lex->definer);
    else
    {
      /*
        DEFINER-clause is missing; we have to create default definer in
        persistent arena to be PS/SP friendly.
        If this is an ALTER VIEW then the current user should be set as
        the definer.
      */
      definer= create_default_definer(thd, false);
    }

    if (definer)
    {
      views->definer.user= definer->user;
      views->definer.host= definer->host;
    } else {
      WSREP_ERROR("Failed to get DEFINER for VIEW.");
      return 1;
    }

    views->algorithm   = lex->create_view->algorithm;
    views->view_suid   = lex->create_view->suid;
    views->with_check  = lex->create_view->check;

    view_store_options(thd, views, &buff);
    buff.append(STRING_WITH_LEN("VIEW "));
    /* Test if user supplied a db (ie: we did not use thd->db) */
    if (views->db.str && views->db.str[0] &&
        (thd->db.str == NULL || cmp(&views->db, &thd->db)))
    {
      append_identifier(thd, &buff, &views->db);
      buff.append('.');
    }
    append_identifier(thd, &buff, &views->table_name);
    if (lex->view_list.elements)
    {
      List_iterator_fast<LEX_CSTRING> names(lex->view_list);
      LEX_CSTRING *name;
      int i;

      buff.append('(');
      for (i= 0; (name= names++); i++)
      {
        append_identifier(thd, &buff, name);
        buff.append(", ", 2);
      }
      if (i)
        buff.length(buff.length()-2);
      buff.append(')');
    }
    buff.append(STRING_WITH_LEN(" AS "));
    buff.append(thd->lex->create_view->select.str,
                thd->lex->create_view->select.length);
    return wsrep_to_buf_helper(thd, buff.ptr(), buff.length(), buf, buf_len);
}

/*
  Rewrite DROP TABLE for TOI. Temporary tables are eliminated from
  the query as they are visible only to client connection.

  TODO: See comments for sql_base.cc:drop_temporary_table() and refine
  the function to deal with transactional locked tables.
 */
static int wsrep_drop_table_query(THD* thd, uchar** buf, size_t* buf_len)
{

  LEX* lex= thd->lex;
  SELECT_LEX* select_lex= lex->first_select_lex();
  TABLE_LIST* first_table= select_lex->table_list.first;
  String buff;

  DBUG_ASSERT(!lex->create_info.tmp_table());

  bool found_temp_table= false;
  for (TABLE_LIST* table= first_table; table; table= table->next_global)
  {
    if (thd->find_temporary_table(table->db, table->table_name))
    {
      found_temp_table= true;
      break;
    }
  }

  if (found_temp_table)
  {
    buff.append(STRING_WITH_LEN("DROP TABLE "));
    if (lex->check_exists)
      buff.append(STRING_WITH_LEN("IF EXISTS "));

    for (TABLE_LIST* table= first_table; table; table= table->next_global)
    {
      if (!thd->find_temporary_table(table->db, table->table_name))
      {
        append_identifier(thd, &buff, table->db.str, table->db.length);
        buff.append('.');
        append_identifier(thd, &buff,
                          table->table_name.str, table->table_name.length);
        buff.append(',');
      }
    }

    /* Chop the last comma */
    buff.chop();
    buff.append(STRING_WITH_LEN(" /* generated by wsrep */"));

    WSREP_DEBUG("Rewrote '%s' as '%s'", thd->query(), buff.ptr());

    return wsrep_to_buf_helper(thd, buff.ptr(), buff.length(), buf, buf_len);
  }
  else
  {
    return wsrep_to_buf_helper(thd, thd->query(), thd->query_length(),
                               buf, buf_len);
  }
}


/* Forward declarations. */
int wsrep_create_trigger_query(THD *thd, uchar** buf, size_t* buf_len);

/*! Should DDL be replicated by Galera
 *
 * @param thd            thread handle
 * @param hton           real storage engine handlerton
 *
 * @retval true if we should replicate DDL, false if not */

bool wsrep_should_replicate_ddl(THD* thd, const handlerton *hton)
{
  if (!wsrep_check_mode(WSREP_MODE_STRICT_REPLICATION))
    return true;

  DBUG_ASSERT(hton != nullptr);

  switch (hton->db_type)
  {
    case DB_TYPE_UNKNOWN:
      /* Special pseudo-handlertons (such as 10.6+ JSON tables). */
      return true;
      break;
    case DB_TYPE_INNODB:
      return true;
      break;
    case DB_TYPE_MYISAM:
      if (wsrep_check_mode(WSREP_MODE_REPLICATE_MYISAM))
        return true;
      break;
    case DB_TYPE_ARIA:
      if (wsrep_check_mode(WSREP_MODE_REPLICATE_ARIA))
        return true;
      break;
    case DB_TYPE_PARTITION_DB:
      /* In most cases this means we could not find out
         table->file->partition_ht() */
      return true;
      break;
    default:
      break;
  }

  WSREP_DEBUG("wsrep OSU failed for %s", wsrep_thd_query(thd));

  /* wsrep_mode = STRICT_REPLICATION, treat as error */
  my_error(ER_GALERA_REPLICATION_NOT_SUPPORTED, MYF(0));
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_ILLEGAL_HA,
                      "WSREP: wsrep_mode = STRICT_REPLICATION enabled. "
                      "Storage engine %s not supported.",
                      ha_resolve_storage_engine_name(hton));
  return false;
}

bool wsrep_should_replicate_ddl_iterate(THD* thd, const TABLE_LIST* table_list)
{
  for (const TABLE_LIST* it= table_list; it; it= it->next_global)
  {
    const TABLE* table= it->table;
    if (table && !it->table_function)
    {
      /* If this is partitioned table we need to find out
         implementing storage engine handlerton.
      */
      const handlerton *ht= table->file->partition_ht();
      if (!ht) ht= table->s->db_type();
      if (!wsrep_should_replicate_ddl(thd, ht))
        return false;
    }
  }
  return true;
}

/*
  Decide if statement should run in TOI.

  Look if table or table_list contain temporary tables. If the
  statement affects only temporary tables,   statement should not run
  in TOI. If the table list contains mix of regular and temporary tables
  (DROP TABLE, OPTIMIZE, ANALYZE), statement should be run in TOI but
  should be rewritten at later time for replication to contain only
  non-temporary tables.
 */
bool wsrep_can_run_in_toi(THD *thd, const char *db, const char *table,
                                 const TABLE_LIST *table_list,
                                 const HA_CREATE_INFO* create_info)
{
  DBUG_ASSERT(!table || db);
  DBUG_ASSERT(table_list || db);

  LEX* lex= thd->lex;
  SELECT_LEX* select_lex= lex->first_select_lex();
  const TABLE_LIST* first_table= select_lex->table_list.first;

  switch (lex->sql_command)
  {
  case SQLCOM_CREATE_TABLE:
    if (thd->lex->create_info.options & HA_LEX_CREATE_TMP_TABLE)
    {
      return false;
    }
    if (!wsrep_should_replicate_ddl(thd, create_info->db_type))
    {
      return false;
    }
    /*
      If mariadb master has replicated a CTAS, we should not replicate the create table
      part separately as TOI, but to replicate both create table and following inserts
      as one write set.
      However, if CTAS creates empty table, we should replicate the create table alone
      as TOI. We have to do relay log event lookup to see if row events follow the
      create table event.
    */
    if (thd->slave_thread &&
	!(thd->rgi_slave->gtid_ev_flags2 & Gtid_log_event::FL_STANDALONE))
    {
      /* this is CTAS, either empty or populated table */
      ulonglong event_size = 0;
      enum Log_event_type ev_type= wsrep_peak_event(thd->rgi_slave, &event_size);
      switch (ev_type)
      {
      case QUERY_EVENT:
      case XID_EVENT:
        /* CTAS with empty table, we replicate create table as TOI */
        break;

      case TABLE_MAP_EVENT:
        WSREP_DEBUG("replicating CTAS of empty table as TOI");
        // fall through
      case WRITE_ROWS_EVENT:
        /* CTAS with populated table, we replicate later at commit time */
        WSREP_DEBUG("skipping create table of CTAS replication");
        return false;

      default:
        WSREP_WARN("unexpected async replication event: %d", ev_type);
      }
      return true;
    }
    /* no next async replication event */
    return true;
    break;
  case SQLCOM_CREATE_VIEW:

    DBUG_ASSERT(!table_list);
    DBUG_ASSERT(first_table); /* First table is view name */
    /*
      If any of the remaining tables refer to temporary table error
      is returned to client, so TOI can be skipped
    */
    for (const TABLE_LIST* it= first_table->next_global; it; it= it->next_global)
    {
      if (thd->find_temporary_table(it))
      {
        return false;
      }
    }
    return true;
    break;
  case SQLCOM_CREATE_TRIGGER:

    DBUG_ASSERT(first_table);

    if (thd->find_temporary_table(first_table))
    {
      return false;
    }
    return true;
    break;
  case SQLCOM_DROP_TRIGGER:
    DBUG_ASSERT(table_list);
    if (thd->find_temporary_table(table_list))
    {
      return false;
    }
    return true;
    break;
  case SQLCOM_ALTER_TABLE:
    if (create_info)
    {
      const handlerton *hton= create_info->db_type;
      if (!hton)
        hton= ha_default_handlerton(thd);
      if (!wsrep_should_replicate_ddl(thd, hton))
        return false;
    }
    /* fallthrough */
  default:
    if (table && !thd->find_temporary_table(Lex_ident_db(Lex_cstring_strlen(db)),
                                            Lex_cstring_strlen(table)))
    {
      return true;
    }

    if (table_list)
    {
      for (const TABLE_LIST* table= first_table; table; table= table->next_global)
      {
        if (!thd->find_temporary_table(table->db, table->table_name))
        {
          return true;
        }
      }
    }

    return !(table || table_list);
    break;
  case SQLCOM_CREATE_SEQUENCE:
    /* No TOI for temporary sequences as they are
       not replicated */
    if (thd->lex->tmp_table())
    {
      return false;
    }
    return true;

  }
}

static int wsrep_create_sp(THD *thd, uchar** buf, size_t* buf_len)
{
  String log_query;
  sp_head *sp= thd->lex->sphead;
  sql_mode_t saved_mode= thd->variables.sql_mode;
  String retstr(64);
  LEX_CSTRING returns= empty_clex_str;
  retstr.set_charset(system_charset_info);

  log_query.set_charset(system_charset_info);

  if (sp->m_handler->type() == SP_TYPE_FUNCTION)
  {
    sp->sp_returns_type(thd, retstr);
    retstr.get_value(&returns);
  }
  if (sp->m_handler->
      show_create_sp(thd, &log_query,
                     sp->m_explicit_name ? sp->m_db : null_clex_str,
                     sp->m_name, sp->m_params, returns,
                     sp->m_body, sp->chistics(),
                     thd->lex->definer[0],
                     thd->lex->create_info,
                     saved_mode))
  {
    WSREP_WARN("SP create string failed: schema: %s, query: %s",
               thd->get_db(), thd->query());
    return 1;
  }

  return wsrep_to_buf_helper(thd, log_query.ptr(), log_query.length(), buf, buf_len);
}

static int wsrep_TOI_event_buf(THD* thd, uchar** buf, size_t* buf_len)
{
  int err;
  switch (thd->lex->sql_command)
  {
  case SQLCOM_CREATE_VIEW:
    err= create_view_query(thd, buf, buf_len);
    break;
  case SQLCOM_CREATE_PROCEDURE:
  case SQLCOM_CREATE_SPFUNCTION:
    err= wsrep_create_sp(thd, buf, buf_len);
    break;
  case SQLCOM_CREATE_TRIGGER:
    err= wsrep_create_trigger_query(thd, buf, buf_len);
    break;
  case SQLCOM_CREATE_EVENT:
    err= wsrep_create_event_query(thd, buf, buf_len);
    break;
  case SQLCOM_ALTER_EVENT:
    err= wsrep_alter_event_query(thd, buf, buf_len);
    break;
  case SQLCOM_DROP_TABLE:
    err= wsrep_drop_table_query(thd, buf, buf_len);
    break;
  case SQLCOM_CREATE_ROLE:
    if (sp_process_definer(thd))
    {
      WSREP_WARN("Failed to set CREATE ROLE definer for TOI.");
    }
    /* fallthrough */
  default:
    err= wsrep_to_buf_helper(thd, thd->query(), thd->query_length(), buf,
                             buf_len);
    break;
  }

  return err;
}

static void wsrep_TOI_begin_failed(THD* thd, const wsrep_buf_t* /* const err */)
{
  if (wsrep_thd_trx_seqno(thd) > 0)
  {
    /* GTID was granted and TO acquired - need to log event and release TO */
    if (wsrep_emulate_bin_log) wsrep_thd_binlog_trx_reset(thd);
    if (wsrep_write_dummy_event(thd, "TOI begin failed")) { goto fail; }
    wsrep::client_state& cs(thd->wsrep_cs());
    std::string const err(wsrep::to_c_string(cs.current_error()));
    wsrep::mutable_buffer err_buf;
    err_buf.push_back(err);
    int const ret= cs.leave_toi_local(err_buf);
    if (ret)
    {
      WSREP_ERROR("Leaving critical section for failed TOI failed: thd: %lld, "
                  "schema: %s, SQL: %s, rcode: %d wsrep_error: %s",
                  (long long)thd->real_id, thd->db.str,
                  thd->query(), ret, err.c_str());
      goto fail;
    }
  }
  return;
fail:
  WSREP_ERROR("Failed to release TOI resources. Need to abort.");
  unireg_abort(1);
}

/*
  returns:
   0: statement was replicated as TOI
   1: TOI replication was skipped
  -1: TOI replication failed
 */
static int wsrep_TOI_begin(THD *thd, const char *db, const char *table,
                           const TABLE_LIST *table_list,
                           const Alter_info *alter_info,
                           const wsrep::key_array *fk_tables,
                           const HA_CREATE_INFO *create_info)
{
  DBUG_ASSERT(wsrep_OSU_method_get(thd) == WSREP_OSU_TOI);

  WSREP_DEBUG("TOI Begin: %s", wsrep_thd_query(thd));
  DEBUG_SYNC(thd, "wsrep_before_toi_begin");

  if (wsrep_can_run_in_toi(thd, db, table, table_list, create_info) == false)
  {
    WSREP_DEBUG("No TOI for %s", wsrep_thd_query(thd));
    return 1;
  }

  uchar* buf= 0;
  size_t buf_len(0);
  int buf_err;
  int rc;

  buf_err= wsrep_TOI_event_buf(thd, &buf, &buf_len);

  if (buf_err) {
    WSREP_ERROR("Failed to create TOI event buf: %d", buf_err);
    my_message(ER_UNKNOWN_ERROR,
               "WSREP replication failed to prepare TOI event buffer. "
               "Check your query.",
               MYF(0));
    return -1;
  }

  struct wsrep_buf buff= { buf, buf_len };

  wsrep::key_array key_array=
    wsrep_prepare_keys_for_toi(db, table, table_list, alter_info, fk_tables);

  if (thd->has_read_only_protection())
  {
    /* non replicated DDL, affecting temporary tables only */
    WSREP_DEBUG("TO isolation skipped, sql: %s."
                "Only temporary tables affected.",
                wsrep_thd_query(thd));
    if (buf) my_free(buf);
    return -1;
  }

  thd_proc_info(thd, "acquiring total order isolation");
  WSREP_DEBUG("wsrep_TOI_begin for %s", wsrep_thd_query(thd));
  THD_STAGE_INFO(thd, stage_waiting_isolation);

  DEBUG_SYNC(thd, "wsrep_before_toi_begin");

  wsrep::client_state& cs(thd->wsrep_cs());

  int ret= cs.enter_toi_local(key_array,
                              wsrep::const_buffer(buff.ptr, buff.len));

  if (ret)
  {
    DBUG_ASSERT(cs.current_error());
    WSREP_DEBUG("to_execute_start() failed for %llu: %s, seqno: %lld",
                thd->thread_id, wsrep_thd_query(thd),
                (long long)wsrep_thd_trx_seqno(thd));

    /* jump to error handler in mysql_execute_command() */
    switch (cs.current_error())
    {
    case wsrep::e_size_exceeded_error:
      WSREP_WARN("TO isolation failed for: %d, schema: %s, sql: %s. "
                 "Maximum size exceeded.",
                 ret,
                 (thd->db.str ? thd->db.str : "(null)"),
                 wsrep_thd_query(thd));
      my_error(ER_UNKNOWN_ERROR, MYF(0), "Maximum writeset size exceeded");
      break;
    case wsrep::e_deadlock_error:
      WSREP_WARN("TO isolation failed for: %d, schema: %s, sql: %s. "
                 "Deadlock error.",
                 ret,
                 (thd->db.str ? thd->db.str : "(null)"),
                 wsrep_thd_query(thd));
      my_error(ER_LOCK_DEADLOCK, MYF(0));
      break;
    case wsrep::e_timeout_error:
      WSREP_WARN("TO isolation failed for: %d, schema: %s, sql: %s. "
                 "Operation timed out.",
                 ret,
                 (thd->db.str ? thd->db.str : "(null)"),
                 wsrep_thd_query(thd));
      my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
      break;
    default:
      WSREP_WARN("TO isolation failed for: %d, schema: %s, sql: %s. "
                 "Check your wsrep connection state and retry the query.",
                 ret,
                 (thd->db.str ? thd->db.str : "(null)"),
                 wsrep_thd_query(thd));

      if (!thd->is_error())
      {
        push_warning_printf(thd, Sql_state_errno_level::WARN_LEVEL_ERROR,
                            ER_LOCK_DEADLOCK,
                            "WSREP replication failed. Check "
                            "your wsrep connection state and retry the query.");
        my_error(ER_LOCK_DEADLOCK, MYF(0));
      }
    }
    rc= -1;
  }
  else {
    if (!thd->variables.gtid_seq_no)
    {
    uint64 seqno= 0;
      if (thd->variables.wsrep_gtid_seq_no &&
          thd->variables.wsrep_gtid_seq_no > wsrep_gtid_server.seqno())
      {
        seqno= thd->variables.wsrep_gtid_seq_no;
        wsrep_gtid_server.seqno(thd->variables.wsrep_gtid_seq_no);
      }
      else
      {
        seqno= wsrep_gtid_server.seqno_inc();
      }
      thd->variables.wsrep_gtid_seq_no= 0;
      thd->wsrep_current_gtid_seqno= seqno;
      if (mysql_bin_log.is_open() && wsrep_gtid_mode)
      {
        thd->variables.gtid_seq_no= seqno;
        thd->variables.gtid_domain_id= wsrep_gtid_server.domain_id;
        thd->variables.server_id= wsrep_gtid_server.server_id;
      }
    }
    ++wsrep_to_isolation;
    rc= 0;
  }

  if (buf) my_free(buf);

  if (rc) wsrep_TOI_begin_failed(thd, NULL);

  return rc;
}

static void wsrep_TOI_end(THD *thd) {
  wsrep_to_isolation--;
  wsrep::client_state& client_state(thd->wsrep_cs());
  DBUG_ASSERT(wsrep_thd_is_local_toi(thd));
  WSREP_DEBUG("TO END: %lld: %s", client_state.toi_meta().seqno().get(),
              wsrep_thd_query(thd));

  wsrep_gtid_server.signal_waiters(thd->wsrep_current_gtid_seqno, false);

  if (wsrep_thd_is_local_toi(thd))
  {
    wsrep::mutable_buffer err;

    thd->wsrep_last_written_gtid_seqno= thd->wsrep_current_gtid_seqno;
    wsrep_set_SE_checkpoint(client_state.toi_meta().gtid(), wsrep_gtid_server.gtid());

    if (thd->is_error() && !wsrep_must_ignore_error(thd))
    {
      /* use only error code, for the message can be inconsistent
       * between the nodes due to differing lc_message settings
       * in client session and server applier thread */
      wsrep_store_error(thd, err, false);
    }

    int const ret= client_state.leave_toi_local(err);

    if (!ret)
    {
      WSREP_DEBUG("TO END: %lld", client_state.toi_meta().seqno().get());
    }
    else
    {
      WSREP_WARN("TO isolation end failed for: %d, schema: %s, sql: %s",
                 ret, (thd->db.str ? thd->db.str : "(null)"), wsrep_thd_query(thd));
    }
  }
}

static int wsrep_RSU_begin(THD *thd, const char *db_, const char *table_)
{
  WSREP_DEBUG("RSU BEGIN: %lld, : %s", wsrep_thd_trx_seqno(thd),
              wsrep_thd_query(thd));

  /* For CREATE TEMPORARY SEQUENCE we do not start RSU because
     object is local only and actually CREATE TABLE + INSERT
  */
  if (thd->lex->sql_command == SQLCOM_CREATE_SEQUENCE &&
      thd->lex->tmp_table())
    return 1;

  if (thd->variables.wsrep_OSU_method == WSREP_OSU_RSU &&
      thd->variables.sql_log_bin == 1 &&
      wsrep_check_mode(WSREP_MODE_DISALLOW_LOCAL_GTID))
  {
    /* wsrep_mode = WSREP_MODE_DISALLOW_LOCAL_GTID, treat as error */
    my_error(ER_GALERA_REPLICATION_NOT_SUPPORTED, MYF(0));
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_OPTION_PREVENTS_STATEMENT,
                        "You can't execute statements that would generate local "
                        "GTIDs when wsrep_mode = DISALLOW_LOCAL_GTID is set. "
                        "Try disabling binary logging with SET sql_log_bin=0 "
                        "to execute this statement.");

    return -1;
  }

  if (thd->wsrep_cs().begin_rsu(5000))
  {
    WSREP_WARN("RSU begin failed");
  }
  else
  {
    thd->variables.wsrep_on= 0;
  }
  return 0;
}

static void wsrep_RSU_end(THD *thd)
{
  WSREP_DEBUG("RSU END: %lld : %s", wsrep_thd_trx_seqno(thd),
              wsrep_thd_query(thd));
  if (thd->wsrep_cs().end_rsu())
  {
    WSREP_WARN("Failed to end RSU, server may need to be restarted");
  }
  thd->variables.wsrep_on= 1;
}

static inline bool is_replaying_connection(THD *thd)
{
  bool ret;

  mysql_mutex_lock(&thd->LOCK_thd_data);
  ret=  (thd->wsrep_trx().state() == wsrep::transaction::s_replaying) ? true : false;
  mysql_mutex_unlock(&thd->LOCK_thd_data);

  return ret;
}

int wsrep_to_isolation_begin(THD *thd, const char *db_, const char *table_,
                             const TABLE_LIST* table_list,
                             const Alter_info *alter_info,
                             const wsrep::key_array *fk_tables,
                             const HA_CREATE_INFO *create_info)
{
  DEBUG_SYNC(thd, "wsrep_kill_thd_before_enter_toi");
  mysql_mutex_lock(&thd->LOCK_thd_kill);
  const killed_state killed = thd->killed;
  mysql_mutex_unlock(&thd->LOCK_thd_kill);
  if (killed)
  {
    /* The thread may have been killed as a result of memory pressure. */
    return -1;
  }

  /*
    No isolation for applier or replaying threads.
   */
  if (!wsrep_thd_is_local(thd))
  {
    if (wsrep_OSU_method_get(thd) == WSREP_OSU_TOI)
      WSREP_DEBUG("%s TOI Begin: %s",
                  is_replaying_connection(thd) ? "Replay" : "Apply",
                  wsrep_thd_query(thd));

    return 0;
  }

  if (thd->wsrep_parallel_slave_wait_for_prior_commit())
  {
    WSREP_WARN("TOI: wait_for_prior_commit() returned error.");
    return -1;
  }

  int ret= 0;

  mysql_mutex_lock(&thd->LOCK_thd_data);

  if (thd->wsrep_trx().state() == wsrep::transaction::s_must_abort)
  {
    WSREP_INFO("thread: %lld  schema: %s  query: %s has been aborted due to multi-master conflict",
               (longlong) thd->thread_id, thd->get_db(), thd->query());
    mysql_mutex_unlock(&thd->LOCK_thd_data);
    return WSREP_TRX_FAIL;
  }
  mysql_mutex_unlock(&thd->LOCK_thd_data);

  DBUG_ASSERT(wsrep_thd_is_local(thd));
  DBUG_ASSERT(thd->wsrep_trx().ws_meta().seqno().is_undefined());

  if (Wsrep_server_state::instance().desynced_on_pause())
  {
    my_message(ER_UNKNOWN_COM_ERROR,
               "Aborting TOI: Replication paused on node for FTWRL/BACKUP STAGE.", MYF(0));
    WSREP_DEBUG("Aborting TOI: Replication paused on node for FTWRL/BACKUP STAGE.: %s %llu",
                wsrep_thd_query(thd), thd->thread_id);
    return -1;
  }

  /* If we are inside LOCK TABLE we release it and give warning. */
  if (thd->variables.option_bits & OPTION_TABLE_LOCK &&
      thd->lex->sql_command == SQLCOM_CREATE_SEQUENCE)
  {
    thd->locked_tables_list.unlock_locked_tables(thd);
    thd->variables.option_bits&= ~(OPTION_TABLE_LOCK);
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
			HA_ERR_UNSUPPORTED,
			"Galera cluster does not support LOCK TABLE on "
			"SEQUENCES. Lock is released.");
  }
  if (wsrep_debug && thd->mdl_context.has_locks())
  {
    WSREP_DEBUG("thread holds MDL locks at TO begin: %s %llu",
                wsrep_thd_query(thd), thd->thread_id);
  }

  /*
    It makes sense to set auto_increment_* to defaults in TOI operations.
    Must be done before wsrep_TOI_begin() since Query_log_event encapsulating
    TOI statement and auto inc variables for wsrep replication is constructed
    there. Variables are reset back in THD::reset_for_next_command() before
    processing of next command.
   */
  if (wsrep_auto_increment_control)
  {
    thd->variables.auto_increment_offset= 1;
    thd->variables.auto_increment_increment= 1;
  }

  if (thd->variables.wsrep_on && wsrep_thd_is_local(thd))
  {
    switch (wsrep_OSU_method_get(thd)) {
    case WSREP_OSU_TOI:
      ret= wsrep_TOI_begin(thd, db_, table_, table_list, alter_info, fk_tables,
                           create_info);
      break;
    case WSREP_OSU_RSU:
      ret= wsrep_RSU_begin(thd, db_, table_);
      break;
    default:
      WSREP_ERROR("Unsupported OSU method: %lu",
                  wsrep_OSU_method_get(thd));
      ret= -1;
      break;
    }

    switch (ret) {
    case 0: /* wsrep_TOI_begin should set toi mode */
      if (thd->variables.wsrep_OSU_method == WSREP_OSU_TOI)
      {
        /*
          TOI operations ignore the provided lock_wait_timeout once replicated,
          and restore it after operation is done.
         */
        thd->variables.saved_lock_wait_timeout= thd->variables.lock_wait_timeout;
        thd->variables.lock_wait_timeout= LONG_TIMEOUT;
      }
      break;
    case 1:
        /* TOI replication skipped, treat as success */
        ret= 0;
      break;
    case -1:
      /* TOI replication failed, treat as error */
      break;
    }
  }

  return ret;
}

void wsrep_to_isolation_end(THD *thd)
{
  if (wsrep_thd_is_local_toi(thd))
  {
    thd->variables.lock_wait_timeout= thd->variables.saved_lock_wait_timeout;
    DBUG_ASSERT(wsrep_OSU_method_get(thd) == WSREP_OSU_TOI);
    wsrep_TOI_end(thd);
  }
  else if (wsrep_thd_is_in_rsu(thd))
  {
    DBUG_ASSERT(wsrep_OSU_method_get(thd) == WSREP_OSU_RSU);
    wsrep_RSU_end(thd);
  }
  else
  {
   /* Applier or replaying threads just output TO END */
   if (wsrep_debug)
   {
     wsrep::client_state& client_state(thd->wsrep_cs());
     WSREP_DEBUG("%s TO END: %lld: %s",
                 is_replaying_connection(thd) ? "Replay" : "Apply",
                 client_state.toi_meta().seqno().get(),
                 wsrep_thd_query(thd));
   }
   return;
  }

  if (wsrep_emulate_bin_log) wsrep_thd_binlog_trx_reset(thd);
}

#define WSREP_MDL_LOG(severity, msg, schema, schema_len, req, gra)             \
    WSREP_##severity(                                                          \
      "%s\n"                                                                   \
      "schema:  %.*s\n"                                                        \
      "request: (%llu \tseqno %lld \twsrep (%s, %s, %s) cmd %d %d \t%s)\n"     \
      "granted: (%llu \tseqno %lld \twsrep (%s, %s, %s) cmd %d %d \t%s)",      \
      msg, schema_len, schema,                                                 \
      req->thread_id, (long long)wsrep_thd_trx_seqno(req),                     \
      wsrep_thd_client_mode_str(req), wsrep_thd_client_state_str(req), wsrep_thd_transaction_state_str(req), \
      req->get_command(), req->lex->sql_command, req->query(),                 \
      gra->thread_id, (long long)wsrep_thd_trx_seqno(gra),                     \
      wsrep_thd_client_mode_str(gra), wsrep_thd_client_state_str(gra), wsrep_thd_transaction_state_str(gra), \
      gra->get_command(), gra->lex->sql_command, gra->query());

/**
  Check if request for the metadata lock should be granted to the requester.

  @param  requestor_ctx        The MDL context of the requestor
  @param  ticket               MDL ticket for the requested lock
  @param  key                  The key of the object (data) being protected

*/
void wsrep_handle_mdl_conflict(MDL_context *requestor_ctx,
                               const MDL_ticket *ticket,
                               const MDL_key *key)
{
  THD *request_thd= requestor_ctx->get_thd();

  /* Fallback to the non-wsrep behaviour */
  if (!WSREP(request_thd)) return;

  mysql_mutex_lock(&request_thd->LOCK_thd_data);

  if (wsrep_thd_is_toi(request_thd) ||
      wsrep_thd_is_applying(request_thd))
  {
    THD *granted_thd= ticket->get_ctx()->get_thd();

    const char* schema= key->db_name();
    int schema_len= key->db_name_length();

    WSREP_DEBUG("wsrep_handle_mdl_conflict request TOI/APPLY for %s",
                wsrep_thd_query(request_thd));
    THD_STAGE_INFO(request_thd, stage_waiting_isolation);
    mysql_mutex_unlock(&request_thd->LOCK_thd_data);
    WSREP_MDL_LOG(DEBUG, "MDL conflict ", schema, schema_len,
                  request_thd, granted_thd);
    ticket->wsrep_report(wsrep_debug);

    DEBUG_SYNC(request_thd, "before_wsrep_thd_abort");
    DBUG_EXECUTE_IF("sync.before_wsrep_thd_abort", {
      const char act[]= "now "
                        "SIGNAL sync.before_wsrep_thd_abort_reached "
                        "WAIT_FOR signal.before_wsrep_thd_abort";
      DBUG_ASSERT(!debug_sync_set_action(request_thd, STRING_WITH_LEN(act)));
    };);

    /* Here we will call wsrep_abort_transaction so we should hold
    THD::LOCK_thd_data to protect victim from concurrent usage
    and THD::LOCK_thd_kill to protect from disconnect or delete.
    */
    mysql_mutex_lock(&granted_thd->LOCK_thd_kill);
    mysql_mutex_lock(&granted_thd->LOCK_thd_data);

    if (granted_thd->wsrep_aborter != 0)
    {
      DBUG_ASSERT(granted_thd->wsrep_aborter == request_thd->thread_id);
      WSREP_DEBUG("BF thread waiting for a victim to release locks");
    }
    else if (wsrep_thd_is_toi(granted_thd) ||
             wsrep_thd_is_applying(granted_thd))
    {
      if (wsrep_thd_is_aborting(granted_thd))
      {
        WSREP_DEBUG("BF thread waiting for SR in aborting state for %s",
                    wsrep_thd_query(request_thd));
        THD_STAGE_INFO(request_thd, stage_waiting_isolation);
        ticket->wsrep_report(wsrep_debug);
      }
      else if (wsrep_thd_is_SR(granted_thd) && !wsrep_thd_is_SR(request_thd))
      {
        WSREP_MDL_LOG(INFO, "MDL conflict, DDL vs SR",
                      schema, schema_len, request_thd, granted_thd);
        WSREP_DEBUG("wsrep_handle_mdl_conflict DDL vs SR for %s",
                    wsrep_thd_query(request_thd));
        THD_STAGE_INFO(request_thd, stage_waiting_isolation);
        wsrep_abort_thd(request_thd, granted_thd, 1);
      }
      else
      {
        WSREP_MDL_LOG(INFO, "MDL BF-BF conflict", schema, schema_len,
                      request_thd, granted_thd);
        ticket->wsrep_report(true);
        mysql_mutex_unlock(&granted_thd->LOCK_thd_data);
        mysql_mutex_unlock(&granted_thd->LOCK_thd_kill);
        unireg_abort(1);
      }
    }
    else if (granted_thd->lex->sql_command == SQLCOM_FLUSH ||
            /* System transactions with explicit locks are BACKUP. */
            (granted_thd->system_thread != NON_SYSTEM_THREAD &&
             granted_thd->mdl_context.has_explicit_locks()))
    {
      WSREP_DEBUG("BF thread waiting for %s",
                  granted_thd->lex->sql_command == SQLCOM_FLUSH ? "FLUSH" : "BACKUP");
      ticket->wsrep_report(wsrep_debug);

      if (granted_thd->current_backup_stage != BACKUP_FINISHED &&
	  wsrep_check_mode(WSREP_MODE_BF_MARIABACKUP))
      {
	wsrep_abort_thd(request_thd, granted_thd, 1);
      }
    }
    else if (granted_thd->lex->sql_command == SQLCOM_LOCK_TABLES)
    {
      WSREP_DEBUG("BF thread waiting for LOCK TABLES");
      ticket->wsrep_report(wsrep_debug);
    }
    else if (request_thd->lex->sql_command == SQLCOM_DROP_TABLE)
    {
      WSREP_DEBUG("DROP caused BF abort, conf %s for %s",
                  wsrep_thd_transaction_state_str(granted_thd),
                  wsrep_thd_query(request_thd));
      THD_STAGE_INFO(request_thd, stage_waiting_isolation);
      ticket->wsrep_report(wsrep_debug);
      wsrep_abort_thd(request_thd, granted_thd, 1);
    }
    else
    {
      WSREP_MDL_LOG(DEBUG, "MDL conflict-> BF abort", schema, schema_len,
                    request_thd, granted_thd);
      WSREP_DEBUG("wsrep_handle_mdl_conflict -> BF abort for %s",
                  wsrep_thd_query(request_thd));
      THD_STAGE_INFO(request_thd, stage_waiting_isolation);
      ticket->wsrep_report(wsrep_debug);

      if (granted_thd->wsrep_trx().active())
      {
        wsrep_abort_thd(request_thd, granted_thd, 1);
      }
      else
      {
        /*
          Granted_thd is likely executing with wsrep_on=0. If the requesting
          thd is BF, BF abort and wait.
        */
        if (wsrep_thd_is_BF(request_thd, FALSE))
        {
          granted_thd->awake_no_mutex(KILL_QUERY_HARD);
          ha_abort_transaction(request_thd, granted_thd, TRUE);
        }
        else
        {
	  WSREP_MDL_LOG(INFO, "MDL unknown BF-BF conflict",
                        schema, schema_len,
                        request_thd, granted_thd);
	  ticket->wsrep_report(true);
          mysql_mutex_unlock(&granted_thd->LOCK_thd_data);
          mysql_mutex_unlock(&granted_thd->LOCK_thd_kill);
	  unireg_abort(1);
        }
      }
    }
    mysql_mutex_unlock(&granted_thd->LOCK_thd_data);
    mysql_mutex_unlock(&granted_thd->LOCK_thd_kill);
    DEBUG_SYNC(request_thd, "after_wsrep_thd_abort");
  }
  else
  {
    mysql_mutex_unlock(&request_thd->LOCK_thd_data);
  }
}

/**/
static bool abort_replicated(THD *thd)
{
  bool ret_code= false;
  mysql_mutex_lock(&thd->LOCK_thd_kill);
  mysql_mutex_lock(&thd->LOCK_thd_data);
  if (thd->wsrep_trx().state() == wsrep::transaction::s_committing)
  {
    WSREP_DEBUG("aborting replicated trx: %llu", (ulonglong)(thd->real_id));

    wsrep_abort_thd(thd, thd, TRUE);
    ret_code= true;
  }
  mysql_mutex_unlock(&thd->LOCK_thd_data);
  mysql_mutex_unlock(&thd->LOCK_thd_kill);
  return ret_code;
}

/**/
static inline bool is_client_connection(THD *thd)
{
  return (thd->wsrep_client_thread && thd->variables.wsrep_on);
}

static inline bool is_committing_connection(THD *thd)
{
  bool ret;

  mysql_mutex_lock(&thd->LOCK_thd_data);
  ret=  (thd->wsrep_trx().state() == wsrep::transaction::s_committing) ? true : false;
  mysql_mutex_unlock(&thd->LOCK_thd_data);

  return ret;
}

static my_bool have_client_connections(THD *thd, void*)
{
  if (is_client_connection(thd))
  {
    if (thd->killed == KILL_CONNECTION ||
        thd->killed == KILL_CONNECTION_HARD)
    {
      (void)abort_replicated(thd);
      return true;
    }
    if (thd->get_stmt_da()->is_eof())
    {
      return true;
    }
  }
  return false;
}

static void wsrep_close_thread(THD *thd)
{
  thd->set_killed(KILL_CONNECTION_HARD);
  MYSQL_CALLBACK(thread_scheduler, post_kill_notification, (thd));
  mysql_mutex_lock(&thd->LOCK_thd_kill);
  thd->abort_current_cond_wait(true);
  mysql_mutex_unlock(&thd->LOCK_thd_kill);
}

static my_bool have_committing_connections(THD *thd, void *)
{
  return is_client_connection(thd) && is_committing_connection(thd) ? 1 : 0;
}

int wsrep_wait_committing_connections_close(int wait_time)
{
  int sleep_time= 100;

  WSREP_DEBUG("wait for committing transaction to close: %d sleep: %d", wait_time, sleep_time);
  while (server_threads.iterate(have_committing_connections) && wait_time > 0)
  {
    WSREP_DEBUG("wait for committing transaction to close: %d", wait_time);
    my_sleep(sleep_time);
    wait_time -= sleep_time;
  }
  return server_threads.iterate(have_committing_connections);
}

static my_bool kill_all_threads(THD *thd, THD *caller_thd)
{
  DBUG_PRINT("quit", ("Informing thread %lld that it's time to die",
                      (longlong) thd->thread_id));
  /* We skip slave threads & scheduler on this first loop through. */
  if (is_client_connection(thd) && thd != caller_thd)
  {
    /* the connection executing SHUTDOWN, should do clean exit,
       not aborting here */
    if (thd->get_command() == COM_SHUTDOWN)
    {
      WSREP_DEBUG("leaving SHUTDOWN executing connection alive, thread: %lld",
                 (longlong) thd->thread_id);
      return 0;
    }
    /* replaying connection is killed by signal */
    if (is_replaying_connection(thd))
    {
      WSREP_DEBUG("closing connection is replaying %lld", (longlong) thd->thread_id);
      thd->set_killed(KILL_CONNECTION_HARD);
      return 0;
    }

    if (!abort_replicated(thd))
    {
      /* replicated transactions must be skipped */
      WSREP_DEBUG("closing connection %lld", (longlong) thd->thread_id);
      /* instead of wsrep_close_thread() we do now hard kill by THD::awake */
      thd->awake(KILL_CONNECTION_HARD);
      return 0;
    }
  }
  return 0;
}

static my_bool kill_remaining_threads(THD *thd, THD *caller_thd)
{
#ifndef __bsdi__				// Bug in BSDI kernel
  if (is_client_connection(thd) &&
      !abort_replicated(thd)    &&
      !is_replaying_connection(thd) &&
      !thd->get_stmt_da()->is_eof() &&
      thd_is_connection_alive(thd) &&
      thd != caller_thd)
  {

    WSREP_INFO("killing local connection: %lld", (longlong) thd->thread_id);
    close_connection(thd);
  }
#endif
  return 0;
}

void wsrep_close_client_connections(my_bool wait_to_end, THD* except_caller_thd)
{
  /* Clear thread cache */
  thread_cache.final_flush();

  /*
    First signal all threads that it's time to die
  */
  server_threads.iterate(kill_all_threads, except_caller_thd);

  /*
    Force remaining threads to die by closing the connection to the client
  */
  server_threads.iterate(kill_remaining_threads, except_caller_thd);

  DBUG_PRINT("quit", ("Waiting for threads to die (count=%u)",
                      THD_count::value()));
  WSREP_DEBUG("waiting for client connections to close: %u",
              THD_count::value());

  while (wait_to_end && server_threads.iterate(have_client_connections))
  {
    sleep(1);
    DBUG_PRINT("quit",("One thread died (count=%u)", THD_count::value()));
  }

  /* All client connection threads have now been aborted */
}


void wsrep_close_applier(THD *thd)
{
  WSREP_DEBUG("closing applier %lld", (longlong) thd->thread_id);
  wsrep_close_thread(thd);
}

static my_bool wsrep_close_threads_callback(THD *thd, THD *caller_thd)
{
  DBUG_PRINT("quit",("Informing thread %lld that it's time to die",
                     (longlong) thd->thread_id));
  /* We skip slave threads & scheduler on this first loop through. */
  if (thd->wsrep_applier && thd != caller_thd)
  {
    WSREP_DEBUG("closing wsrep thread %lld", (longlong) thd->thread_id);
    wsrep_close_thread(thd);
  }
  return 0;
}

void wsrep_close_threads(THD *thd)
{
  server_threads.iterate(wsrep_close_threads_callback, thd);
}

void wsrep_wait_appliers_close(THD *thd)
{
  /* Wait for wsrep appliers to gracefully exit */
  mysql_mutex_lock(&LOCK_wsrep_slave_threads);
  while (wsrep_running_threads > 2)
    /*
      2 is for rollbacker thread which needs to be killed explicitly.
      This gotta be fixed in a more elegant manner if we gonna have arbitrary
      number of non-applier wsrep threads.
    */
  {
    mysql_cond_wait(&COND_wsrep_slave_threads, &LOCK_wsrep_slave_threads);
  }
  mysql_mutex_unlock(&LOCK_wsrep_slave_threads);
  DBUG_PRINT("quit",("applier threads have died (count=%u)",
                     uint32_t(wsrep_running_threads)));

  /* Now kill remaining wsrep threads: rollbacker */
  wsrep_close_threads (thd);
  /* and wait for them to die */
  mysql_mutex_lock(&LOCK_wsrep_slave_threads);
  while (wsrep_running_threads > 0)
  {
    mysql_cond_wait(&COND_wsrep_slave_threads, &LOCK_wsrep_slave_threads);
  }
  mysql_mutex_unlock(&LOCK_wsrep_slave_threads);
  DBUG_PRINT("quit",("all wsrep system threads have died"));

  /* All wsrep applier threads have now been aborted. However, if this thread
     is also applier, we are still running...
  */
}
int wsrep_must_ignore_error(THD* thd)
{
  const int error= thd->get_stmt_da()->sql_errno();
  const uint flags= sql_command_flags[thd->lex->sql_command];

  DBUG_ASSERT(error);
  DBUG_ASSERT(wsrep_thd_is_toi(thd));

  if ((wsrep_ignore_apply_errors & WSREP_IGNORE_ERRORS_ON_DDL))
    goto ignore_error;

  if ((flags & CF_WSREP_MAY_IGNORE_ERRORS) &&
      (wsrep_ignore_apply_errors & WSREP_IGNORE_ERRORS_ON_RECONCILING_DDL))
  {
    switch (error)
    {
    case ER_DB_DROP_EXISTS:
    case ER_BAD_TABLE_ERROR:
    case ER_CANT_DROP_FIELD_OR_KEY:
      goto ignore_error;
    }
  }

  return 0;

ignore_error:
  WSREP_WARN("Ignoring error '%s' on query. "
             "Default database: '%s'. Query: '%s', Error_code: %d",
             thd->get_stmt_da()->message(),
             safe_str(thd->db.str),
             thd->query(),
             error);
  return 1;
}

int wsrep_ignored_error_code(Log_event* ev, int error)
{
  const THD* thd= ev->thd;

  DBUG_ASSERT(error);
  /* Note that binlog events can be executed on master also with
     BINLOG '....'; */
  DBUG_ASSERT(!wsrep_thd_is_local_toi(thd));

  if ((wsrep_ignore_apply_errors & WSREP_IGNORE_ERRORS_ON_RECONCILING_DML))
  {
    const int ev_type= ev->get_type_code();
    if ((ev_type == DELETE_ROWS_EVENT || ev_type == DELETE_ROWS_EVENT_V1)
        && error == ER_KEY_NOT_FOUND)
      goto ignore_error;
  }

  return 0;

ignore_error:
  WSREP_WARN("Ignoring error '%s' on %s event. Error_code: %d",
             thd->get_stmt_da()->message(),
             ev->get_type_str(),
             error);
  return 1;
}

bool wsrep_provider_is_SR_capable()
{
  return Wsrep_server_state::has_capability(wsrep::provider::capability::streaming);
}

int wsrep_thd_retry_counter(const THD *thd)
{
  return thd->wsrep_retry_counter;
}

extern  bool wsrep_thd_ignore_table(THD *thd)
{
  return thd->wsrep_ignore_table;
}

bool wsrep_create_like_table(THD* thd, TABLE_LIST* table,
                             TABLE_LIST* src_table,
                             HA_CREATE_INFO *create_info)
{
  if (create_info->tmp_table())
  {
    /* CREATE TEMPORARY TABLE LIKE must be skipped from replication */
    WSREP_DEBUG("CREATE TEMPORARY TABLE LIKE... skipped replication\n %s",
                thd->query());
  }
  else if (!(thd->find_temporary_table(src_table)))
  {
    /* this is straight CREATE TABLE LIKE... with no tmp tables */
    WSREP_TO_ISOLATION_BEGIN_CREATE(table->db.str, table->table_name.str, table, create_info);
  }
  else
  {
    /* Non-MERGE tables ignore this call. */
    if (src_table->table->file->extra(HA_EXTRA_ADD_CHILDREN_LIST))
      return (true);

    char buf[2048];
    String query(buf, sizeof(buf), system_charset_info);
    query.length(0);  // Have to zero it since constructor doesn't

    int result __attribute__((unused))=
      show_create_table(thd, src_table, &query, NULL, WITH_DB_NAME);
    WSREP_DEBUG("TMP TABLE: %s ret_code %d", query.ptr(), result);

    thd->wsrep_TOI_pre_query=     query.ptr();
    thd->wsrep_TOI_pre_query_len= query.length();

    WSREP_TO_ISOLATION_BEGIN_CREATE(table->db.str, table->table_name.str, table, create_info);

    thd->wsrep_TOI_pre_query=      NULL;
    thd->wsrep_TOI_pre_query_len= 0;

    /* Non-MERGE tables ignore this call. */
    src_table->table->file->extra(HA_EXTRA_DETACH_CHILDREN);
  }

  return(false);
#ifdef WITH_WSREP
wsrep_error_label:
  thd->wsrep_TOI_pre_query= NULL;
  return (true);
#endif
}

int wsrep_create_trigger_query(THD *thd, uchar** buf, size_t* buf_len)
{
  LEX *lex= thd->lex;
  String stmt_query;

  LEX_CSTRING definer_user;
  LEX_CSTRING definer_host;

  if (!lex->definer)
  {
    if (!thd->slave_thread)
    {
      if (!(lex->definer= create_default_definer(thd, false)))
        return 1;
    }
  }

  if (lex->definer)
  {
    /* SUID trigger. */
    LEX_USER *d= get_current_user(thd, lex->definer);

    if (!d)
      return 1;

    definer_user= d->user;
    definer_host= d->host;
  }
  else
  {
    /* non-SUID trigger. */

    definer_user.str= 0;
    definer_user.length= 0;

    definer_host.str= 0;
    definer_host.length= 0;
  }

  const LEX_CSTRING command[2]=
      {{ C_STRING_WITH_LEN("CREATE ") },
       { C_STRING_WITH_LEN("CREATE OR REPLACE ") }};

  if (thd->lex->create_info.or_replace())
    stmt_query.append(command[1]);
  else
    stmt_query.append(command[0]);

  append_definer(thd, &stmt_query, &definer_user, &definer_host);

  LEX_CSTRING stmt_definition;
  stmt_definition.str= (char*) thd->lex->stmt_definition_begin;
  stmt_definition.length= thd->lex->stmt_definition_end
    - thd->lex->stmt_definition_begin;
  trim_whitespace(thd->charset(), &stmt_definition);

  stmt_query.append(stmt_definition.str, stmt_definition.length);

  return wsrep_to_buf_helper(thd, stmt_query.c_ptr(), stmt_query.length(),
                             buf, buf_len);
}

void* start_wsrep_THD(void *arg)
{
  THD *thd= NULL;

  Wsrep_thd_args* thd_args= (Wsrep_thd_args*) arg;

  if (my_thread_init() || (!(thd= new THD(next_thread_id(), true))))
  {
    goto error;
  }

  statistic_increment(thread_created, &LOCK_status);

  thd->real_id=pthread_self(); // Keep purify happy

  my_net_init(&thd->net,(st_vio*) 0, thd, MYF(0));

  DBUG_PRINT("wsrep",(("creating thread %lld"), (long long)thd->thread_id));
  thd->prior_thr_create_utime= thd->start_utime= microsecond_interval_timer();

  server_threads.insert(thd);

  /* from bootstrap()... */
  thd->bootstrap=1;
  thd->max_client_packet_length= thd->net.max_packet;
  thd->security_ctx->master_access= ALL_KNOWN_ACL;

  /* from handle_one_connection... */
  pthread_detach_this_thread();

  mysql_thread_set_psi_id(thd->thread_id);
  thd->thr_create_utime=  microsecond_interval_timer();

  DBUG_EXECUTE_IF("wsrep_simulate_failed_connection_1", goto error; );
// </5.1.17>
  /*
    handle_one_connection() is normally the only way a thread would
    start and would always be on the very high end of the stack ,
    therefore, the thread stack always starts at the address of the
    first local variable of handle_one_connection, which is thd. We
    need to know the start of the stack so that we could check for
    stack overruns.
  */
  DBUG_PRINT("wsrep", ("handle_one_connection called by thread %lld",
                       (long long)thd->thread_id));
  /* now that we've called my_thread_init(), it is safe to call DBUG_* */

  wsrep_assign_from_threadvars(thd);
  wsrep_store_threadvars(thd);

  thd->system_thread= SYSTEM_THREAD_SLAVE_SQL;
  thd->security_ctx->skip_grants();

  /* handle_one_connection() again... */
  thd->mark_connection_idle();
  thd->init_for_queries();
  mysql_mutex_lock(&LOCK_wsrep_slave_threads);

  wsrep_running_threads++;

  switch (thd_args->thread_type()) {
    case WSREP_APPLIER_THREAD:
      wsrep_running_applier_threads++;
      break;
    case WSREP_ROLLBACKER_THREAD:
      wsrep_running_rollbacker_threads++;
      break;
    default:
      WSREP_ERROR("Incorrect wsrep thread type: %d", thd_args->thread_type());
      break;
  }

  mysql_cond_broadcast(&COND_wsrep_slave_threads);
  mysql_mutex_unlock(&LOCK_wsrep_slave_threads);

  WSREP_DEBUG("wsrep system thread %llu, %p starting",
              thd->thread_id, thd);
  thd_args->fun()(thd, static_cast<void *>(thd_args));

  WSREP_DEBUG("wsrep system thread: %llu, %p closing",
              thd->thread_id, thd);

  /* Wsrep may reset globals during thread context switches, store globals
     before cleanup. */
  wsrep_store_threadvars(thd);

  close_connection(thd, 0);

  mysql_mutex_lock(&LOCK_wsrep_slave_threads);
  DBUG_ASSERT(wsrep_running_threads > 0);
  wsrep_running_threads--;

  switch (thd_args->thread_type()) {
    case WSREP_APPLIER_THREAD:
      DBUG_ASSERT(wsrep_running_applier_threads > 0);
      wsrep_running_applier_threads--;
      break;
    case WSREP_ROLLBACKER_THREAD:
      DBUG_ASSERT(wsrep_running_rollbacker_threads > 0);
      wsrep_running_rollbacker_threads--;
      break;
    default:
      WSREP_ERROR("Incorrect wsrep thread type: %d", thd_args->thread_type());
      break;
  }

  delete thd_args;
  WSREP_DEBUG("wsrep running threads now: %lu", wsrep_running_threads);
  mysql_cond_broadcast(&COND_wsrep_slave_threads);
  mysql_mutex_unlock(&LOCK_wsrep_slave_threads);
  /*
    Note: We can't call THD destructor without crashing
    if plugins have not been initialized. However, in most of the
    cases this means that pre SE initialization SST failed and
    we are going to exit anyway.
  */
  if (plugins_are_initialized)
  {
    net_end(&thd->net);
    unlink_thd(thd);
  }
  else
  {
    /*
      TODO: lightweight cleanup to get rid of:
      'Error in my_thread_global_end(): 2 threads didn't exit'
      at server shutdown
    */
    server_threads.erase(thd);
  }

  delete thd;
  my_thread_end();
  return(NULL);

error:
  WSREP_ERROR("Failed to create/initialize system thread");

  if (thd)
  {
    close_connection(thd, ER_OUT_OF_RESOURCES);
    statistic_increment(aborted_connects, &LOCK_status);
    server_threads.erase(thd);
    delete thd;
    my_thread_end();
  }
  delete thd_args;
  // This will signal error to wsrep_slave_threads_update
  wsrep_thread_create_failed.store(true, std::memory_order_relaxed);

  /* Abort if its the first applier/rollbacker thread. */
  if (!mysqld_server_initialized)
    unireg_abort(1);
  else
    return NULL;
}

enum wsrep::streaming_context::fragment_unit wsrep_fragment_unit(ulong unit)
{
  switch (unit)
  {
  case WSREP_FRAG_BYTES: return wsrep::streaming_context::bytes;
  case WSREP_FRAG_ROWS: return wsrep::streaming_context::row;
  case WSREP_FRAG_STATEMENTS: return wsrep::streaming_context::statement;
  default:
    DBUG_ASSERT(0);
    return wsrep::streaming_context::bytes;
  }
}

bool THD::wsrep_parallel_slave_wait_for_prior_commit()
{
  if (rgi_slave && rgi_slave->is_parallel_exec)
  {
    wait_for_pending_deadlock_kill(this, rgi_slave);
    if (rgi_slave->killed_for_retry) {
      my_error(ER_LOCK_DEADLOCK, MYF(0));
      return true;
    }
    return wait_for_prior_commit();
  }
  return false;
}

void wsrep_parallel_slave_wakeup_subsequent_commits(void *thd_ptr)
{
  THD *thd = (THD*)thd_ptr;
  if (thd->rgi_slave && thd->rgi_slave->is_parallel_exec &&
      thd->wait_for_commit_ptr)
  {
    thd->wait_for_commit_ptr->wakeup_subsequent_commits(0);
  }
}

/***** callbacks for wsrep service ************/

my_bool get_wsrep_recovery()
{
  return wsrep_recovery;
}

bool wsrep_consistency_check(THD *thd)
{
  return thd->wsrep_consistency_check == CONSISTENCY_CHECK_RUNNING;
}

// Wait until wsrep has reached ready state
void wsrep_wait_ready(THD *thd)
{
  // First check not locking the mutex.
  if (wsrep_ready)
    return;

  mysql_mutex_lock(&LOCK_wsrep_ready);
  while(!wsrep_ready)
  {
    WSREP_INFO("Waiting to reach ready state");
    mysql_cond_wait(&COND_wsrep_ready, &LOCK_wsrep_ready);
  }
  WSREP_INFO("ready state reached");
  mysql_mutex_unlock(&LOCK_wsrep_ready);
}

void wsrep_ready_set(bool ready_value)
{
  WSREP_DEBUG("Setting wsrep_ready to %d", ready_value);
  mysql_mutex_lock(&LOCK_wsrep_ready);
  wsrep_ready= ready_value;
  // Signal if we have reached ready state
  if (ready_value)
    mysql_cond_signal(&COND_wsrep_ready);
  mysql_mutex_unlock(&LOCK_wsrep_ready);
}


/*
  Commit an empty transaction.

  If the transaction is real and the wsrep transaction is still active,
  the transaction did not generate any rows or keys and is committed
  as empty. Here the wsrep transaction is rolled back and after statement
  step is performed to leave the wsrep transaction in the state as it
  never existed.

*/

void wsrep_commit_empty(THD* thd, bool all)
{
  DBUG_ENTER("wsrep_commit_empty");

  if (wsrep_is_real(thd, all) &&
      wsrep_thd_is_local(thd) &&
      thd->wsrep_trx().active() &&
      !thd->internal_transaction() &&
      thd->wsrep_trx().state() != wsrep::transaction::s_committed)
  {
#ifndef DBUG_OFF
    const bool empty= !wsrep_has_changes(thd);
    const bool create= thd->lex->sql_command == SQLCOM_CREATE_TABLE &&
      !thd->is_current_stmt_binlog_format_row();
    const bool aborted= thd->wsrep_cs().transaction().state() == wsrep::transaction::s_aborted;
    const bool ddl_replay= ((sql_command_flags[thd->lex->sql_command] &
                  (CF_SCHEMA_CHANGE | CF_ADMIN_COMMAND)) &&
                 thd->wsrep_cs().transaction().state() == wsrep::transaction::s_must_replay);
    /* Here transaction is either
       (1) empty (i.e. no changes) or
       (2) it was CREATE TABLE with no row binlog format or
       (3) we have already aborted transaction e.g. because max writeset size
       has been reached or
       (4) it was DDL and got BF aborted and must replay.
    */
    if(!(empty || create || aborted || ddl_replay))
    {
      WSREP_DEBUG("wsrep_commit_empty: thread: %llu client_state: %s client_mode:"
                  " %s trans_state: %s error: %s empty: %d create: %d aborted:"
		  " %d ddl_replay: %d sql: %s",
                  thd_get_thread_id(thd),
                  wsrep::to_c_string(thd->wsrep_cs().state()),
                  wsrep::to_c_string(thd->wsrep_cs().mode()),
                  wsrep::to_c_string(thd->wsrep_cs().transaction().state()),
                  wsrep::to_c_string(thd->wsrep_cs().current_error()),
                  empty, create, aborted, ddl_replay,
                  wsrep_thd_query(thd));

      DBUG_ASSERT(empty ||     // 1
                  create ||    // 2
                  aborted ||   // 3
                  ddl_replay); // 4
    }
#endif /* DBUG_OFF */
    bool have_error= wsrep_current_error(thd);
    int ret= wsrep_before_rollback(thd, all) ||
      wsrep_after_rollback(thd, all) ||
      wsrep_after_statement(thd);
    /* The committing transaction was empty but it held some locks and
       got BF aborted. As there were no certified changes in the
       data, we ignore the deadlock error and rely on error reporting
       by storage engine/server. */
    if (!ret && !have_error && wsrep_current_error(thd))
    {
      DBUG_ASSERT(wsrep_current_error(thd) == wsrep::e_deadlock_error);
      thd->wsrep_cs().reset_error();
    }

    if (ret)
      WSREP_DEBUG("wsrep_commit_empty failed: %s",
                  wsrep::to_c_string(thd->wsrep_cs().current_error()));
  }
  DBUG_VOID_RETURN;
}

bool wsrep_table_list_has_non_temp_tables(THD *thd, TABLE_LIST *tables)
{
  for (TABLE_LIST *table= tables; table; table= table->next_global)
  {
    if (!is_temporary_table(table))
    {
      return true;
    }
  }
  return false;
}

bool wsrep_foreign_key_append(THD *thd, FOREIGN_KEY_INFO *fk)
{
  if (WSREP(thd) && !thd->wsrep_applier &&
      wsrep_is_active(thd) &&
      (sql_command_flags[thd->lex->sql_command] &
       (CF_UPDATES_DATA | CF_DELETES_DATA)))
  {
    wsrep::key key(wsrep::key::shared);
    key.append_key_part(fk->foreign_db->str, fk->foreign_db->length);
    key.append_key_part(fk->foreign_table->str, fk->foreign_table->length);

    if (thd->wsrep_cs().append_key(key))
    {
      WSREP_ERROR("Appending table key failed: %s",
                  wsrep_thd_query(thd));
      sql_print_information("Failed Foreign key referenced table found: "
                            "%s.%s",
                            fk->foreign_db->str,
                            fk->foreign_table->str);
      return true;
    }

    DBUG_EXECUTE_IF(
      "wsrep_print_foreign_keys_table",
      sql_print_information("Foreign key referenced table found: %s.%s",
                            fk->foreign_db->str,
                            fk->foreign_table->str);
    );
  }

  return false;
}
