/* Copyright (c) 2007, 2012, Oracle and/or its affiliates.
   Copyright (c) 2020, 2022, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */


#include "mariadb.h"
#include "sql_class.h"
#include "debug_sync.h"
#include "sql_array.h"
#include "rpl_rli.h"
#include <lf.h>
#include "aligned.h"
#include "unireg.h"
#include <mysql/plugin.h>
#include <mysql/service_thd_wait.h>
#include <mysql/psi/mysql_stage.h>
#include <tpool.h>
#include <pfs_metadata_provider.h>
#include <mysql/psi/mysql_mdl.h>
#include <algorithm>
#include <array>
#ifdef WITH_WSREP
#include "wsrep_mysqld.h"
#endif

static PSI_memory_key key_memory_MDL_context_acquire_locks;

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_MDL_lock_fast_lane_mutex;
static PSI_mutex_key key_MDL_wait_LOCK_wait_status;

static PSI_mutex_info all_mdl_mutexes[]=
{
  { &key_MDL_lock_fast_lane_mutex, "MDL_lock::Fast_road::Lane::m_mutex", 0 },
  { &key_MDL_wait_LOCK_wait_status, "MDL_wait::LOCK_wait_status", 0}
};

static PSI_rwlock_key key_MDL_lock_rwlock;
static PSI_rwlock_key key_MDL_context_LOCK_waiting_for;

static PSI_rwlock_info all_mdl_rwlocks[]=
{
  { &key_MDL_lock_rwlock, "MDL_lock::rwlock", 0},
  { &key_MDL_context_LOCK_waiting_for, "MDL_context::LOCK_waiting_for", 0}
};

static PSI_cond_key key_MDL_wait_COND_wait_status;

static PSI_cond_info all_mdl_conds[]=
{
  { &key_MDL_wait_COND_wait_status, "MDL_context::COND_wait_status", 0}
};

static PSI_memory_info all_mdl_memory[]=
{
  { &key_memory_MDL_context_acquire_locks, "MDL_context::acquire_locks", 0}
};

/**
  Initialise all the performance schema instrumentation points
  used by the MDL subsystem.
*/
static void init_mdl_psi_keys(void)
{
  int count;

  count= array_elements(all_mdl_mutexes);
  mysql_mutex_register("sql", all_mdl_mutexes, count);

  count= array_elements(all_mdl_rwlocks);
  mysql_rwlock_register("sql", all_mdl_rwlocks, count);

  count= array_elements(all_mdl_conds);
  mysql_cond_register("sql", all_mdl_conds, count);

  count= array_elements(all_mdl_memory);
  mysql_memory_register("sql", all_mdl_memory, count);

  MDL_key::init_psi_keys();
}
#endif /* HAVE_PSI_INTERFACE */


/**
  Thread state names to be used in case when we have to wait on resource
  belonging to certain namespace.
*/

PSI_stage_info MDL_key::m_namespace_to_wait_state_name[NAMESPACE_END]=
{
  {0, "Waiting for backup lock", 0},
  {0, "Waiting for schema metadata lock", 0},
  {0, "Waiting for table metadata lock", 0},
  {0, "Waiting for stored function metadata lock", 0},
  {0, "Waiting for stored procedure metadata lock", 0},
  {0, "Waiting for stored package body metadata lock", 0},
  {0, "Waiting for trigger metadata lock", 0},
  {0, "Waiting for event metadata lock", 0},
  {0, "User lock", 0} /* Be compatible with old status. */
};


static const LEX_STRING lock_types[]=
{
  { C_STRING_WITH_LEN("MDL_INTENTION_EXCLUSIVE") },
  { C_STRING_WITH_LEN("MDL_SHARED") },
  { C_STRING_WITH_LEN("MDL_SHARED_HIGH_PRIO") },
  { C_STRING_WITH_LEN("MDL_SHARED_READ") },
  { C_STRING_WITH_LEN("MDL_SHARED_WRITE") },
  { C_STRING_WITH_LEN("MDL_SHARED_UPGRADABLE") },
  { C_STRING_WITH_LEN("MDL_SHARED_READ_ONLY") },
  { C_STRING_WITH_LEN("MDL_SHARED_NO_WRITE") },
  { C_STRING_WITH_LEN("MDL_SHARED_NO_READ_WRITE") },
  { C_STRING_WITH_LEN("MDL_EXCLUSIVE") },
};


static const LEX_STRING backup_lock_types[]=
{
  { C_STRING_WITH_LEN("MDL_BACKUP_START") },
  { C_STRING_WITH_LEN("MDL_BACKUP_FLUSH") },
  { C_STRING_WITH_LEN("MDL_BACKUP_WAIT_FLUSH") },
  { C_STRING_WITH_LEN("MDL_BACKUP_WAIT_DDL") },
  { C_STRING_WITH_LEN("MDL_BACKUP_WAIT_COMMIT") },
  { C_STRING_WITH_LEN("MDL_BACKUP_FTWRL1") },
  { C_STRING_WITH_LEN("MDL_BACKUP_FTWRL2") },
  { C_STRING_WITH_LEN("MDL_BACKUP_DML") },
  { C_STRING_WITH_LEN("MDL_BACKUP_TRANS_DML") },
  { C_STRING_WITH_LEN("MDL_BACKUP_SYS_DML") },
  { C_STRING_WITH_LEN("MDL_BACKUP_DDL") },
  { C_STRING_WITH_LEN("MDL_BACKUP_BLOCK_DDL") },
  { C_STRING_WITH_LEN("MDL_BACKUP_ALTER_COPY") },
  { C_STRING_WITH_LEN("MDL_BACKUP_COMMIT") }
};


#ifdef HAVE_PSI_INTERFACE
void MDL_key::init_psi_keys()
{
  int i;
  int count;
  PSI_stage_info *info __attribute__((unused));

  count= array_elements(MDL_key::m_namespace_to_wait_state_name);
  for (i= 0; i<count; i++)
  {
    /* mysql_stage_register wants an array of pointers, registering 1 by 1. */
    info= & MDL_key::m_namespace_to_wait_state_name[i];
    mysql_stage_register("sql", &info, 1);
  }
}
#endif

static bool mdl_initialized= 0;
uint mdl_instances;

enum tal_status { TAL_ERROR, TAL_ACQUIRED, TAL_WAIT, TAL_NOWAIT };


/**
  A collection of all MDL locks. A singleton,
  there is only one instance of the map in the server.
*/

class MDL_map
{
public:
  void init();
  void destroy();
  enum tal_status try_acquire_lock(LF_PINS *pins, MDL_key *key,
                                   MDL_ticket *ticket, bool wait);
  unsigned long get_lock_owner(LF_PINS *pins, const MDL_key *key);
  void remove(LF_PINS *pins, const MDL_key *key)
  { lf_hash_delete(&m_locks, pins, key->ptr(), key->length()); }
  LF_PINS *get_pins() { return lf_hash_get_pins(&m_locks); }
private:
  LF_HASH m_locks; /**< All acquired locks in the server. */
  /** Pre-allocated MDL_lock object for BACKUP namespace. */
  MDL_lock *m_backup_lock;
  friend int mdl_iterate(mdl_iterator_callback, void *);
};


/**
  A context of the recursive traversal through all contexts
  in all sessions in search for deadlock.
*/

class Deadlock_detection_visitor: public MDL_wait_for_graph_visitor
{
public:
  Deadlock_detection_visitor(MDL_context *start_node_arg)
    : m_start_node(start_node_arg),
      m_victim(NULL),
      m_current_search_depth(0),
      m_found_deadlock(FALSE)
  {}
  bool enter_node(MDL_context *node) override;
  void leave_node(MDL_context *node) override;

  bool inspect_edge(MDL_context *dest) override;

  MDL_context *get_victim() const { return m_victim; }
private:
  /**
    Change the deadlock victim to a new one if it has lower deadlock
    weight.
  */
  void opt_change_victim_to(MDL_context *new_victim);
private:
  /**
    The context which has initiated the search. There
    can be multiple searches happening in parallel at the same time.
  */
  MDL_context *m_start_node;
  /** If a deadlock is found, the context that identifies the victim. */
  MDL_context *m_victim;
  /** Set to the 0 at start. Increased whenever
    we descend into another MDL context (aka traverse to the next
    wait-for graph node). When MAX_SEARCH_DEPTH is reached, we
    assume that a deadlock is found, even if we have not found a
    loop.
  */
  uint m_current_search_depth;
  /** TRUE if we found a deadlock. */
  bool m_found_deadlock;
  /**
    Maximum depth for deadlock searches. After this depth is
    achieved we will unconditionally declare that there is a
    deadlock.

    @note This depth should be small enough to avoid stack
          being exhausted by recursive search algorithm.

    TODO: Find out what is the optimal value for this parameter.
          Current value is safe, but probably sub-optimal,
          as there is an anecdotal evidence that real-life
          deadlocks are even shorter typically.
  */
  static const uint MAX_SEARCH_DEPTH= 32;
};

#ifndef DBUG_OFF

/*
  Print a list of all locks to DBUG trace to help with debugging
*/

const char *dbug_print_mdl(MDL_ticket *mdl_ticket)
{
  thread_local char buffer[256];
  const MDL_key *mdl_key= mdl_ticket->get_key();
  my_snprintf(buffer, sizeof(buffer) - 1, "%.*s/%.*s (%s)",
              (int) mdl_key->db_name_length(), mdl_key->db_name(),
              (int) mdl_key->name_length(),    mdl_key->name(),
              mdl_ticket->get_type_name()->str);
  return buffer;
}


const char *dbug_print(MDL_ticket *mdl_ticket)
{
  return dbug_print_mdl(mdl_ticket);
}


static int mdl_dbug_print_lock(MDL_ticket *mdl_ticket, void *arg, bool granted)
{
  String *tmp= (String*) arg;
  char buffer[256];
  size_t length= my_snprintf(buffer, sizeof(buffer) - 1,
                             "\n    %s (%s)", dbug_print_mdl(mdl_ticket),
                             granted ? "granted" : "waiting");
  tmp->append(buffer, length);
  return 0;
}

const char *mdl_dbug_print_locks()
{
  thread_local String tmp;
  tmp.length(0);
  mdl_iterate(mdl_dbug_print_lock, (void*) &tmp);
  return tmp.c_ptr();
}
#endif /* DBUG_OFF */

/**
  Enter a node of a wait-for graph. After
  a node is entered, inspect_edge() will be called
  for all wait-for destinations of this node. Then
  leave_node() will be called.
  We call "enter_node()" for all nodes we inspect,
  including the starting node.

  @retval  TRUE  Maximum search depth exceeded.
  @retval  FALSE OK.
*/

bool Deadlock_detection_visitor::enter_node(MDL_context *node)
{
  m_found_deadlock= ++m_current_search_depth >= MAX_SEARCH_DEPTH;
  if (m_found_deadlock)
  {
    DBUG_ASSERT(! m_victim);
    opt_change_victim_to(node);
  }
  return m_found_deadlock;
}


/**
  Done inspecting this node. Decrease the search
  depth. If a deadlock is found, and we are
  backtracking to the start node, optionally
  change the deadlock victim to one with lower
  deadlock weight.
*/

void Deadlock_detection_visitor::leave_node(MDL_context *node)
{
  --m_current_search_depth;
  if (m_found_deadlock)
    opt_change_victim_to(node);
}


/**
  Inspect a wait-for graph edge from one MDL context to another.

  @retval TRUE   A loop is found.
  @retval FALSE  No loop is found.
*/

bool Deadlock_detection_visitor::inspect_edge(MDL_context *node)
{
  m_found_deadlock= node == m_start_node;
  return m_found_deadlock;
}


/**
  Change the deadlock victim to a new one if it has lower deadlock
  weight.

  @retval new_victim  Victim is not changed.
  @retval !new_victim New victim became the current.
*/

void
Deadlock_detection_visitor::opt_change_victim_to(MDL_context *new_victim)
{
  if (m_victim == NULL ||
      m_victim->get_deadlock_weight() >= new_victim->get_deadlock_weight())
  {
    /* Swap victims, unlock the old one. */
    MDL_context *tmp= m_victim;
    m_victim= new_victim;
    m_victim->lock_deadlock_victim();
    if (tmp)
      tmp->unlock_deadlock_victim();
  }
}


/**
  The lock context. Created internally for an acquired lock.
  For a given name, there exists only one MDL_lock instance,
  and it exists only when the lock has been granted.
  Can be seen as an MDL subsystem's version of TABLE_SHARE.

  This is an abstract class which lacks information about
  compatibility rules for lock types. They should be specified
  in its descendants.
*/

class MDL_lock
{
  typedef mdl_bitmap_t bitmap_t;

  class Ticket_list
  {
    using List= ilist<MDL_ticket>;
  public:
    Ticket_list() :m_bitmap(0) { m_type_counters.fill(0); }

    void add_ticket(MDL_ticket *ticket);
    void remove_ticket(MDL_ticket *ticket);
    bool is_empty() const { return m_list.empty(); }
    bitmap_t bitmap() const { return m_bitmap; }
    List::const_iterator begin() const { return m_list.begin(); }
    List::const_iterator end() const { return m_list.end(); }
  private:
    /** List of tickets. */
    List m_list;
    /** Bitmap of types of tickets in this list. */
    bitmap_t m_bitmap;
    std::array<uint32_t, MDL_BACKUP_END> m_type_counters; // hash table
  };


  /**
    Helper struct which defines how different types of locks are handled
    for a specific MDL_lock. In practice we use only three strategies:
    "backup" lock strategy for locks in BACKUP namespace, "scoped" lock
    strategy for locks in SCHEMA namespace and "object" lock strategy for
    all other namespaces.
  */
  struct MDL_lock_strategy
  {
    virtual const bitmap_t *incompatible_granted_types_bitmap() const = 0;
    virtual const bitmap_t *incompatible_waiting_types_bitmap() const = 0;
    virtual bool needs_notification(const MDL_ticket *ticket) const = 0;
    virtual bool conflicting_locks(const MDL_ticket *ticket) const = 0;
    virtual bitmap_t hog_lock_types_bitmap() const = 0;
    virtual ~MDL_lock_strategy() = default;
  };


  /**
    An implementation of the scoped metadata lock. The only locking modes
    which are supported at the moment are SHARED and INTENTION EXCLUSIVE
    and EXCLUSIVE
  */
  struct MDL_scoped_lock : public MDL_lock_strategy
  {
    MDL_scoped_lock() = default;
    const bitmap_t *incompatible_granted_types_bitmap() const override
    { return m_granted_incompatible; }
    const bitmap_t *incompatible_waiting_types_bitmap() const override
    { return m_waiting_incompatible; }
    bool needs_notification(const MDL_ticket *ticket) const override
    { return (ticket->get_type() == MDL_SHARED); }

    /**
      Notify threads holding scoped IX locks which conflict with a pending
      S lock.

      Thread which holds global IX lock can be a handler thread for
      insert delayed. We need to kill such threads in order to get
      global shared lock. We do this my calling code outside of MDL.
    */
    bool conflicting_locks(const MDL_ticket *ticket) const override
    { return ticket->get_type() == MDL_INTENTION_EXCLUSIVE; }

    /*
      In scoped locks, only IX lock request would starve because of X/S. But that
      is practically very rare case. So just return 0 from this function.
    */
    bitmap_t hog_lock_types_bitmap() const override
    { return 0; }
  private:
    static const bitmap_t m_granted_incompatible[MDL_TYPE_END];
    static const bitmap_t m_waiting_incompatible[MDL_TYPE_END];
  };


  /**
    An implementation of a per-object lock. Supports SHARED, SHARED_UPGRADABLE,
    SHARED HIGH PRIORITY and EXCLUSIVE locks.
  */
  struct MDL_object_lock : public MDL_lock_strategy
  {
    MDL_object_lock() = default;
    const bitmap_t *incompatible_granted_types_bitmap() const override
    { return m_granted_incompatible; }
    const bitmap_t *incompatible_waiting_types_bitmap() const override
    { return m_waiting_incompatible; }
    bool needs_notification(const MDL_ticket *ticket) const override
    {
      return (MDL_BIT(ticket->get_type()) &
              (MDL_BIT(MDL_SHARED_NO_WRITE) |
               MDL_BIT(MDL_SHARED_NO_READ_WRITE) |
               MDL_BIT(MDL_EXCLUSIVE)));
    }

    /**
      Notify threads holding a shared metadata locks on object which
      conflict with a pending X, SNW or SNRW lock.

      If thread which holds conflicting lock is waiting on table-level
      lock or some other non-MDL resource we might need to wake it up
      by calling code outside of MDL.
    */
    bool conflicting_locks(const MDL_ticket *ticket) const override
    { return ticket->get_type() < MDL_SHARED_UPGRADABLE; }

    /*
      To prevent starvation, these lock types that are only granted
      max_write_lock_count times in a row while other lock types are
      waiting.
    */
    bitmap_t hog_lock_types_bitmap() const override
    {
      return (MDL_BIT(MDL_SHARED_NO_WRITE) |
              MDL_BIT(MDL_SHARED_NO_READ_WRITE) |
              MDL_BIT(MDL_EXCLUSIVE));
    }

  private:
    static const bitmap_t m_granted_incompatible[MDL_TYPE_END];
    static const bitmap_t m_waiting_incompatible[MDL_TYPE_END];
  };


  struct MDL_backup_lock: public MDL_lock_strategy
  {
    MDL_backup_lock() = default;
    const bitmap_t *incompatible_granted_types_bitmap() const override
    { return m_granted_incompatible; }
    const bitmap_t *incompatible_waiting_types_bitmap() const override
    { return m_waiting_incompatible; }
    bool needs_notification(const MDL_ticket *ticket) const override
    {
      return (MDL_BIT(ticket->get_type()) & MDL_BIT(MDL_BACKUP_FTWRL1));
    }

    /**
       Insert delayed threads may hold DML or TRANS_DML lock.
       We need to kill such threads in order to get lock for FTWRL statements.
       We do this by calling code outside of MDL.
    */
    bool conflicting_locks(const MDL_ticket *ticket) const override
    {
      return (MDL_BIT(ticket->get_type()) &
              (MDL_BIT(MDL_BACKUP_DML) |
               MDL_BIT(MDL_BACKUP_TRANS_DML)));
    }

    /*
      In backup namespace DML/DDL may starve because of concurrent FTWRL or
      BACKUP statements. This scenario is practically useless in real world,
      so we just return 0 here.
    */
    bitmap_t hog_lock_types_bitmap() const override
    { return 0; }
  private:
    static const bitmap_t m_granted_incompatible[MDL_BACKUP_END];
    static const bitmap_t m_waiting_incompatible[MDL_BACKUP_END];
  };


  /**
    Scalable handler for "lightweight" lock types.
  */
  class Fast_road
  {
    class Lane
    {
      alignas(CPU_LEVEL1_DCACHE_LINESIZE) mutable mysql_mutex_t m_mutex;
      ilist<MDL_ticket> m_list;
      /**
        Counts number of granted/pending non-fast lane locks.

        Having it as a member of Lane allows simpler and cleaner
        implementation. Otherwise it is a property of Fast_road.
      */
      uint32_t m_close_count;

      bool is_open() const { return m_close_count == 0; }


      /**
        upgrade/downgrade/release helper.

        ticket->m_fast_lane has to be double checked under fast lane mutex
        protection. Description in a comment to MDL_lock::release().

        @retval true  Requested action performed
        @retval false Lane closed, try conventional action
      */
      template <typename Functor>
      bool ticket_action(MDL_ticket *ticket, Functor action)
      {
        mysql_mutex_lock(&m_mutex);
        DBUG_ASSERT(is_open() || m_list.empty());
        Lane *lane= static_cast<Lane *>(
            ticket->m_fast_lane.load(std::memory_order_relaxed));
        if (likely(lane))
        {
          DBUG_ASSERT(is_open());
          DBUG_ASSERT(lane == this);
          action();
        }
        mysql_mutex_unlock(&m_mutex);
        return lane != nullptr;
      }

    public:
      Lane() noexcept: m_close_count(0)
      {
        mysql_mutex_init(key_MDL_lock_fast_lane_mutex, &m_mutex,
                         MY_MUTEX_INIT_FAST);
      }


      ~Lane()
      {
        DBUG_ASSERT(m_close_count <= 1);
        DBUG_ASSERT(m_list.empty());
        mysql_mutex_destroy(&m_mutex);
      }


      static void *operator new[](size_t size, const std::nothrow_t)
      {
        return aligned_malloc(size, CPU_LEVEL1_DCACHE_LINESIZE);
      }


      static void operator delete[](void *ptr) { aligned_free(ptr); }


      /**
        Registers ticket in fast lane.

        ticket->m_fast_lane has to be updated to point to this lane
        either before critical section or inside critical section.
        Before other threads can find it via m_list.

        In most cases this method is called with open fast lane,
        when there're no heavyweight locks in this MDL_lock. There is
        no point in attempting to avoid mutex lock for closed lanes
        by pre-checking lane_open().

        @retval true  Lock granted
        @retval false Lane closed, try conventional lock
      */
      bool try_acquire_lock(MDL_ticket *ticket)
      {
        DBUG_ASSERT(!ticket->m_fast_lane.load(std::memory_order_relaxed));
        mysql_mutex_lock(&m_mutex);
        bool res= is_open();
        DBUG_ASSERT(res || m_list.empty());
        if (likely(res))
        {
          m_list.push_back(*ticket);
          ticket->m_fast_lane.store(this, std::memory_order_relaxed);
        }
        mysql_mutex_unlock(&m_mutex);
        return res;
      }


      /**
        Releases previously acquired lock.

        @retval true  Lock released
        @retval false Lane closed, try conventional unlock
      */
      bool release(MDL_ticket *ticket)
      {
        return ticket_action(ticket,
                             [this, ticket]() { m_list.remove(*ticket); });
      }


      /**
        Updates ticket type.

        Used by upgrade/downgrade.

        @retval true  Success
        @retval false Lane closed, try conventional upgrade/downgrade
      */
      bool change_ticket_type(MDL_ticket *ticket, enum_mdl_type type)
      {
        return ticket_action(ticket,
                             [ticket, type]() { ticket->m_type= type; });
      }


      /**
        Closes fast lane, moves tickets to MDL_lock::m_granted.

        Closed fast lane means the following methods have to retry their
        action using conventional methods:
        MDL_lock::try_acquire_lock()
        MDL_lock::release()
        MDL_lock::upgrade()
        MDL_lock::downgrade()

        Lane can be closed multiple times.
      */
      void close()
      {
        mysql_mutex_lock(&m_mutex);
        DBUG_ASSERT(is_open() || m_list.empty());
        m_close_count++;
        while (!m_list.empty())
        {
          MDL_ticket *ticket= &m_list.front();
          m_list.pop_front();
          ticket->get_lock()->m_granted.add_ticket(ticket);
          DBUG_ASSERT(ticket->m_fast_lane.load(std::memory_order_relaxed) ==
                      this);
          ticket->m_fast_lane.store(nullptr, std::memory_order_relaxed);
        }
        mysql_mutex_unlock(&m_mutex);
      }


      /**
        Reopens fast lane.

        Fast lane can be used again once all closers are gone.
      */
      void reopen()
      {
        mysql_mutex_lock(&m_mutex);
        DBUG_ASSERT(m_close_count);
        DBUG_ASSERT(m_list.empty());
        m_close_count--;
        mysql_mutex_unlock(&m_mutex);
      }


      /**
        Iterates registered tickets.

        @retval true  callback returned true, abort iteration
        @retval false success
      */
      bool iterate(mdl_iterator_callback callback, void *argument) const
      {
        bool res= false;
        mysql_mutex_lock(&m_mutex);
        DBUG_ASSERT(is_open() || m_list.empty());
        for (MDL_ticket &ticket : m_list)
        {
          if (callback(&ticket, argument, true))
          {
            res= true;
            break;
          }
        }
        mysql_mutex_unlock(&m_mutex);
        return res;
      }


      /** Checks if lane is open, used by assertions. */
      bool is_closed() const
      {
        mysql_mutex_lock(&m_mutex);
        bool res= !is_open();
        mysql_mutex_unlock(&m_mutex);
        return res;
      }


      /** Checks if lane is empty, used by assertions. */
      bool is_empty() const
      {
        mysql_mutex_lock(&m_mutex);
        bool res= m_list.empty();
        mysql_mutex_unlock(&m_mutex);
        return res;
      }
    };


    Lane *m_fast_lane;
    mdl_bitmap_t m_supported_types;


    /**
      Checks if provided lock type can be served by fast lanes.

      Fast lane lock types must be fully compatible between each other.
    */
    bool supported_type(enum_mdl_type type) const
    {
      return MDL_BIT(type) & m_supported_types;
    }


    /**
      Checks if ticket is registered in fast lane and performs action().

      Another thread can be closing fast lanes concurrently and
      resetting ticket->m_fast_lane. To handle such scenario properly
      action() has to be double checked ticket->m_fast_lane under fast
      lane mutex protection. Accessing ticket->m_fast_lane in this
      method is the sole reason it is declared atomic.

      @retval true  Success
      @retval false Failure, try conventional action
    */
    template <typename Functor>
    bool lane_action(MDL_ticket *ticket, Functor action) const
    {
      if (Lane *lane= static_cast<Lane *>(
              ticket->m_fast_lane.load(std::memory_order_relaxed)))
      {
        DBUG_ASSERT(is_enabled());
        DBUG_ASSERT(supported_type(ticket->get_type()));
        if (action(lane))
        {
          DBUG_ASSERT(supported_type(ticket->get_type()));
          return true;
        }
      }
      return false;
    }


    /**
      Iterates available lanes and performs action() for each lane.

      @retval true  Iteration was aborted by action()
      @retval false action() was called for all available lanes successfully
    */
    template <typename Functor>
    bool all_lanes_action(Functor action) const
    {
      if (is_enabled())
      {
        for (uint i= 0; i < mdl_instances; i++)
        {
          if (action(&m_fast_lane[i]))
            return true;
        }
      }
      return false;
    }

  public:
    Fast_road(): m_fast_lane(nullptr), m_supported_types(0) {}
    ~Fast_road() { delete [] m_fast_lane; }


    /**
      Enables fast lanes.

      Once enabled, supported_types of lock requests can be served via
      fast lanes.
    */
    void enable(mdl_bitmap_t supported_types)
    {
      m_fast_lane= mdl_instances > 1 ? new (std::nothrow) Lane[mdl_instances]
                                     : nullptr;
      if (m_fast_lane)
        m_supported_types= supported_types;
    }


    bool is_enabled() const { return m_fast_lane; }


    /**
      Attempts to acquire lock.

      @retval true  Lock acquired
      @retval false request cannot be served by fast lanes,
                    try conventional acquire_lock
    */
    bool try_acquire_lock(MDL_ticket *ticket) const
    {
      if (is_enabled() && supported_type(ticket->get_type()))
      {
        DBUG_ASSERT(mdl_instances > 1);
        uint lane= ticket->get_ctx()->get_thd()->thread_id % mdl_instances;
        return m_fast_lane[lane].try_acquire_lock(ticket);
      }
      return false;
    }


    /**
      Attempts to release previously acquired lock.

      @retval true  Lock released
      @retval false ticket is not registered in fast lanes,
                    try conventional unlock
    */
    bool try_release(MDL_ticket *ticket) const
    {
      return lane_action(
          ticket, [ticket](Lane *lane) { return lane->release(ticket); });
    }


    /**
      Attempts to upgrade or downgrade lock.

      @retval true  Lock upgraded/downgraded
      @retval false request cannot be served by fast lanes,
                    try conventional acquire_lock
    */
    bool try_change_ticket_type(MDL_ticket *ticket, enum_mdl_type type) const
    {
       DBUG_ASSERT(supported_type(ticket->get_type()) || is_closed());
       return lane_action(ticket,
                          [ticket, type](Lane *lane)
                          { return lane->change_ticket_type(ticket, type); });
    }


    void close(enum_mdl_type type) const
    {
      if (!supported_type(type))
        all_lanes_action([](Lane *lane) { lane->close(); return false; });
    }


    void reopen(enum_mdl_type type) const
    {
      if (!supported_type(type))
        all_lanes_action([](Lane *lane) { lane->reopen(); return false; });
    }


    /**
      Iterates registered tickets.

      @retval true  callback returned true, abort iteration
      @retval false success
    */
    bool iterate(mdl_iterator_callback callback, void *argument) const
    {
      return all_lanes_action([callback, argument](Lane *lane)
                              { return lane->iterate(callback, argument); });
    }


    /** Checks if fast lanes are closed, used by assertions. */
    bool is_closed() const
    {
      return !all_lanes_action([](Lane *lane) { return !lane->is_closed(); });
    }


    /** Checks if fast lanes are empty, used by assertions. */
    bool is_empty() const
    {
      return !all_lanes_action([](Lane *lane) { return !lane->is_empty(); });
    }
  };


  /** The key of the object (data) being protected. */
  MDL_key key;

  /**
    Read-write lock protecting this lock context.

    @note The fact that we use read-write lock prefers readers here is
          important as deadlock detector won't work correctly otherwise.

          For example, imagine that we have following waiters graph:

                       ctxA -> obj1 -> ctxB -> obj1 -|
                        ^                            |
                        |----------------------------|

          and both ctxA and ctxB start deadlock detection process:

            ctxA read-locks obj1             ctxB read-locks obj2
            ctxA goes deeper                 ctxB goes deeper

          Now ctxC comes in who wants to start waiting on obj1, also
          ctxD comes in who wants to start waiting on obj2.

            ctxC tries to write-lock obj1   ctxD tries to write-lock obj2
            ctxC is blocked                 ctxD is blocked

          Now ctxA and ctxB resume their search:

            ctxA tries to read-lock obj2    ctxB tries to read-lock obj1

          If m_rwlock prefers writes (or fair) both ctxA and ctxB would be
          blocked because of pending write locks from ctxD and ctxC
          correspondingly. Thus we will get a deadlock in deadlock detector.
          If m_wrlock prefers readers (actually ignoring pending writers is
          enough) ctxA and ctxB will continue and no deadlock will occur.
  */
  mutable mysql_prlock_t m_rwlock;

  /** List of granted tickets for this lock. */
  Ticket_list m_granted;
  /** Tickets for contexts waiting to acquire a lock. */
  Ticket_list m_waiting;

  /**
    Number of times high priority lock requests have been granted while
    low priority lock requests were waiting.
  */
  ulong m_hog_lock_count;

  Fast_road m_fast_road;

  /**
    Locking strategy, e.g. backup/scoped/object.

    Initialized to appropriate strategy early, before lock becomes
    available via MDL_map. Reset to nullptr before lock is removed
    from MDL_map. Such value indicates that MDL_lock is being removed
    by a concurrent thread and the caller (specifically
    MDL_map::try_acquire_lock()) must retry.
  */
  const MDL_lock_strategy *m_strategy;

  static const MDL_backup_lock m_backup_lock_strategy;
  static const MDL_scoped_lock m_scoped_lock_strategy;
  static const MDL_object_lock m_object_lock_strategy;

  bool is_empty() const
  {
    return (m_granted.is_empty() && m_waiting.is_empty());
  }

  bool can_grant_lock(enum_mdl_type type, MDL_context *requstor_ctx,
                      bool ignore_lock_priority) const;

  void reschedule_waiters();

  void remove_ticket(LF_PINS *pins, Ticket_list MDL_lock::*queue,
                     MDL_ticket *ticket);

  void notify_conflicting_locks(MDL_context *ctx, bool abort_blocking)
  {
    DBUG_ASSERT(m_fast_road.is_closed());
    for (const auto &conflicting_ticket : m_granted)
    {
      if (conflicting_ticket.get_ctx() != ctx &&
          m_strategy->conflicting_locks(&conflicting_ticket))
      {
        MDL_context *conflicting_ctx= conflicting_ticket.get_ctx();

        ctx->get_owner()->
          notify_shared_lock(conflicting_ctx->get_owner(),
                             conflicting_ctx->get_needs_thr_lock_abort(),
                             abort_blocking);
      }
    }
  }

#ifndef DBUG_OFF
  bool check_if_conflicting_replication_locks(MDL_context *ctx);
#endif

public:
  const bitmap_t *incompatible_granted_types_bitmap() const
  { return m_strategy->incompatible_granted_types_bitmap(); }
  const bitmap_t *incompatible_waiting_types_bitmap() const
  { return m_strategy->incompatible_waiting_types_bitmap(); }


  /**
    Check if we have any pending locks which conflict with existing
    shared lock.

    @pre The ticket must match an acquired lock.

    @return TRUE if there is a conflicting lock request, FALSE otherwise.
  */
  bool has_pending_conflicting_lock(enum_mdl_type type)
  {
    bool result;
    DBUG_ASSERT(key.mdl_namespace() == MDL_key::TABLE);
    DBUG_ASSERT(!m_fast_road.is_enabled());
    mysql_prlock_rdlock(&m_rwlock);
    result= (m_waiting.bitmap() & incompatible_granted_types_bitmap()[type]);
    mysql_prlock_unlock(&m_rwlock);
    return result;
  }


  bool visit_subgraph(MDL_ticket *waiting_ticket,
                      MDL_wait_for_graph_visitor *gvisitor);

  MDL_lock()
    : m_hog_lock_count(0),
      m_strategy(0)
  { mysql_prlock_init(key_MDL_lock_rwlock, &m_rwlock); }

  MDL_lock(const MDL_key *key_arg)
  : key(key_arg),
    m_hog_lock_count(0),
    m_strategy(&m_backup_lock_strategy)
  {
    DBUG_ASSERT(key_arg->mdl_namespace() == MDL_key::BACKUP);
    mysql_prlock_init(key_MDL_lock_rwlock, &m_rwlock);
    m_fast_road.enable(MDL_BIT(MDL_BACKUP_DML) |
                       MDL_BIT(MDL_BACKUP_TRANS_DML) |
                       MDL_BIT(MDL_BACKUP_SYS_DML) |
                       MDL_BIT(MDL_BACKUP_DDL) |
                       MDL_BIT(MDL_BACKUP_ALTER_COPY) |
                       MDL_BIT(MDL_BACKUP_COMMIT));
  }

  ~MDL_lock()
  { mysql_prlock_destroy(&m_rwlock); }

  static void lf_alloc_constructor(uchar *arg)
  { new (arg + LF_HASH_OVERHEAD) MDL_lock(); }

  static void lf_alloc_destructor(uchar *arg)
  { ((MDL_lock*)(arg + LF_HASH_OVERHEAD))->~MDL_lock(); }

  static void lf_hash_initializer(LF_HASH *hash __attribute__((unused)),
                                  void *_lock, const void *_key_arg)
  {
    MDL_lock *lock= static_cast<MDL_lock *>(_lock);
    const MDL_key *key_arg= static_cast<const MDL_key *>(_key_arg);
    DBUG_ASSERT(key_arg->mdl_namespace() != MDL_key::BACKUP);
    new (&lock->key) MDL_key(key_arg);
    if (key_arg->mdl_namespace() == MDL_key::SCHEMA)
      lock->m_strategy= &m_scoped_lock_strategy;
    else
      lock->m_strategy= &m_object_lock_strategy;
  }

  static const uchar *mdl_locks_key(const void *record, size_t *length,
                                    my_bool)
  {
    const MDL_lock *lock= static_cast<const MDL_lock *>(record);
    *length= lock->key.length();
    return lock->key.ptr();
  }


  /**
    Return thread id of the thread to which the first ticket was
    granted.

    Intended for user level locks. They make use of SNW lock only,
    thus there can be only one granted ticket.

    We can skip check for m_strategy here, because m_granted
    must be empty for such locks anyway.
  */
  unsigned long get_lock_owner() const
  {
    unsigned long res= 0;
    DBUG_ASSERT(key.mdl_namespace() == MDL_key::USER_LOCK);
    DBUG_ASSERT(!m_fast_road.is_enabled());
    mysql_prlock_rdlock(&m_rwlock);
    DBUG_ASSERT(m_strategy || is_empty());
    if (!m_granted.is_empty())
      res= m_granted.begin()->get_ctx()->get_thread_id();
    mysql_prlock_unlock(&m_rwlock);
    return res;
  }


  /**
    Callback for mdl_iterate()

    Another thread may be deleting this MDL_lock concurrently.
    Being deleted lock can still be iterated since it must have
    valid empty granted/waiting lists.

    Iterate fast lanes first, then go for conventional m_granted
    and m_waiting lists. To make MDL_lock snapshot consistent, fast
    lanes iteration has to be performed under m_rwlock protection.

    Strictly speaking fast lanes iteration alone doesn't require
    m_rwlock protection. However another thread may be moving the
    ticket from fast lane to m_granted list concurrently.
    If fast lanes are iterated before m_rwlock critical section,
    then ticket iterator may see this ticket twice: once from fast
    lane and once from m_granted list.
    If fast lanes are iterated after m_rwlock critical section,
    then ticket iterator may miss this ticket: it can be removed
    from fast lane, but not yet inserted to m_granted list.

    @retval true  callback returned true, abort iteration
    @retval false success
  */
  bool iterate(mdl_iterator_callback callback, void *argument)
  {
    bool res= true;
    mysql_prlock_rdlock(&m_rwlock);
    DBUG_ASSERT(m_strategy || is_empty());
    DBUG_ASSERT(m_strategy || m_fast_road.is_empty());
    if (m_fast_road.iterate(callback, argument))
      goto end;
    for (MDL_ticket &ticket : m_granted)
    {
      if (callback(&ticket, argument, true))
        goto end;
    }
    for (MDL_ticket &ticket : m_waiting)
    {
      if (callback(&ticket, argument, false))
        goto end;
    }
    res= false;
end:
    mysql_prlock_unlock(&m_rwlock);
    return res;
  }


  /**
    MDL_context::clone_ticket() helper

    Cloned tickets don't seem to be used by performance critical
    code, so they're always added to conventional m_granted list.
    Support for fast lanes can be trivially implemented if needed.
  */
  void add_cloned_ticket(MDL_ticket *ticket)
  {
    m_fast_road.close(ticket->get_type());
    mysql_prlock_wrlock(&m_rwlock);
    m_granted.add_ticket(ticket);
    mysql_prlock_unlock(&m_rwlock);
  }


  /**
    MDL_context::downgrade_lock() helper

    To update state of MDL_lock object correctly we need to temporarily
    exclude ticket from the granted queue and then include it back.

    fast lane lock types can only be downgraded to weaker fast lane
    lock types. non-fast lane lock types can only be downgraded to
    weaker non-fast lane lock types.

    Note that we don't have to reschedule_waiters() when we perform
    downgrade via m_fast_road. There can't be any waiters in such case.
  */
  void downgrade(MDL_ticket *ticket, enum_mdl_type type)
  {
    if (m_fast_road.try_change_ticket_type(ticket, type))
      return;
    mysql_prlock_wrlock(&m_rwlock);
    m_granted.remove_ticket(ticket);
    ticket->m_type= type;
    m_granted.add_ticket(ticket);
    reschedule_waiters();
    mysql_prlock_unlock(&m_rwlock);
  }


  /**
    MDL_context::upgrade_shared_lock() helper

    To update state of MDL_lock object correctly we need to temporarily
    exclude ticket from the granted queue and then include it back.

    fast lane lock types can only be upgraded to stronger fast lane
    lock types. non-fast lane lock types can only be upgraded to
    stronger non-fast lane lock types.

    Non-fast lane locks close fast lanes whenever they're registered in
    MDL_lock. Whenever such locks are being deregistered, fast lanes must
    be reopened. Once all closers are gone, that is number of close calls
    equals to number of open calls, fast lanes become available again.
  */
  void upgrade(MDL_ticket *ticket, enum_mdl_type type,
               MDL_ticket *remove)
  {
    DBUG_ASSERT(!ticket->m_fast_lane.load(std::memory_order_relaxed));
    mysql_prlock_wrlock(&m_rwlock);
    if (remove && !m_fast_road.try_release(remove))
    {
      m_fast_road.reopen(remove->get_type());
      m_granted.remove_ticket(remove);
    }
    m_granted.remove_ticket(ticket);
    ticket->m_type= type;
    m_granted.add_ticket(ticket);
    mysql_prlock_unlock(&m_rwlock);
  }


  bool try_fast_upgrade(MDL_ticket *ticket, enum_mdl_type type)
  {
    return m_fast_road.try_change_ticket_type(ticket, type);
  }


  void notify_conflicting_locks_if_needed(MDL_ticket *ticket, bool abort_blocking)
  {
    if (m_strategy->needs_notification(ticket))
    {
      mysql_prlock_wrlock(&m_rwlock);
      notify_conflicting_locks(ticket->get_ctx(), abort_blocking);
      mysql_prlock_unlock(&m_rwlock);
    }
  }


  /**
    Attempt to acquire lock

    Before performing any action we must ensure that this lock
    is not being deleted by concurrent thread. We're safe when
    m_strategy != nullptr.

    When there're no conflicting granted/waiting locks, lock request
    can be served immediately. In this case the only action that has
    to be performed is adding ticket to m_granted list.

    If it is impossible to serve lock request immediately and the caller
    doesn't intend to wait for this lock, no action has to be performed.
    This is the case for MDL_context::acquire_lock() with
    lock_wait_timeout == 0 and MDL_context::try_acquire_lock().

    Otherwise it is regular MDL_context::acquire_lock(). Add ticket
    to m_waiting list and notify conflicting lock owners.

    Also assert that if we are trying to get an exclusive lock for a slave
    running parallel replication, then we are not blocked by another
    parallel slave thread that is not committed. This should never happen as
    the parallel replication scheduler should never schedule a DDL while
    DML's are still running.

    ticket->m_lock has to be updated to point to this lock either before
    critical section or inside critical section. Before other threads can
    find it via m_granted or m_waiting lists.

    ticket->m_lock is valid only when this method returns either TAL_ACQUIRED
    or TAL_WAIT. In other cases caller doesn't hold references to this lock,
    which means it can be detroyed. Caller is expected to dispose ticket
    immediately anyway.

    Certain lock requests can be served by multi-instance scalable fast lanes.
    The following requirements must be met to make it happen: fast lanes must
    be enabled for this MDL_lock, fast lanes must be open and lock request
    type must satisfy Fast_road::supported_type().

    Fast lanes are available for certain namespaces, e.g. initial implementation
    supported only BACKUP namespace and when fast lanes were allocated
    successfully.

    Non-fast lane lock requests are never served by fast lanes. Such lock
    requests close all fast lanes and move fast lane tickets to conventional
    MDL_lock::m_granted list. Fast lanes are reopened once all such locks are
    released or aborted.

    @retval TAL_ACQUIRED Acquired
    @retval TAL_WAIT     Not acquired, must be waited
    @retval TAL_NOWAIT   Not acquired, cannot be waited
    @retval TAL_ERROR    Lock is being deleted, retry
  */
  enum tal_status try_acquire_lock(MDL_ticket *ticket, bool wait)
  {
    MDL_context *mdl_context= ticket->get_ctx();
    // TAL_ACQUIRED to make less work on the hot path
    enum tal_status res= TAL_ACQUIRED;
    ticket->m_lock= this;

    if (m_fast_road.try_acquire_lock(ticket))
      return res;

    mysql_prlock_wrlock(&m_rwlock);
    if (likely(m_strategy))
    {
      m_fast_road.close(ticket->get_type());
      if (can_grant_lock(ticket->get_type(), mdl_context, false))
        m_granted.add_ticket(ticket);
      else
      {
        DBUG_ASSERT(!is_empty());
        if (wait)
        {
          res= TAL_WAIT;
          m_waiting.add_ticket(ticket);

          if (m_strategy->needs_notification(ticket))
            notify_conflicting_locks(mdl_context, false);

           DBUG_SLOW_ASSERT((ticket->get_type() != MDL_INTENTION_EXCLUSIVE &&
                            ticket->get_type() != MDL_EXCLUSIVE) ||
                           !(mdl_context->get_thd()->rgi_slave &&
                             mdl_context->get_thd()->rgi_slave->is_parallel_exec &&
                             check_if_conflicting_replication_locks(mdl_context)));
        }
        else
        {
          m_fast_road.reopen(ticket->get_type());
          res= TAL_NOWAIT;
        }
      }
    }
    else
    {
      DBUG_ASSERT(m_fast_road.is_closed());
      res= TAL_ERROR;
    }
    mysql_prlock_unlock(&m_rwlock);
    return res;
  }


  /**
    Releases previously acquired lock.

    Lock requests that were served by fast lanes are redirected to fast
    lanes. No waiters are possible in this case, there is nobody to awake.

    Lock requests that were served by fast lanes, which were closed
    in the meantime, are released conventionally.

    Conventional lock release consists of removing lock from m_granted
    list, awaking waiters and destroying MDL_lock if needed.
  */
  void release(LF_PINS *pins, MDL_ticket *ticket)
  {
    DBUG_ASSERT(key.mdl_namespace() == MDL_key::BACKUP ||
                !m_fast_road.is_enabled());
    if (m_fast_road.try_release(ticket))
      return;
    remove_ticket(pins, &MDL_lock::m_granted, ticket);
  }


  void abort_wait(LF_PINS *pins, MDL_ticket *ticket)
  { remove_ticket(pins, &MDL_lock::m_waiting, ticket); }


  const MDL_key *get_key() const { return &key; }
};


const MDL_lock::MDL_backup_lock MDL_lock::m_backup_lock_strategy;
const MDL_lock::MDL_scoped_lock MDL_lock::m_scoped_lock_strategy;
const MDL_lock::MDL_object_lock MDL_lock::m_object_lock_strategy;


static MDL_map mdl_locks;


/**
  Initialize the metadata locking subsystem.

  This function is called at server startup.

  In particular, initializes the new global mutex and
  the associated condition variable: LOCK_mdl and COND_mdl.
  These locking primitives are implementation details of the MDL
  subsystem and are private to it.
*/

void mdl_init()
{
  DBUG_ASSERT(! mdl_initialized);
  mdl_initialized= TRUE;

#ifdef HAVE_PSI_INTERFACE
  init_mdl_psi_keys();
#endif

  mdl_locks.init();
}


/**
  Release resources of metadata locking subsystem.

  Destroys the global mutex and the condition variable.
  Called at server shutdown.
*/

void mdl_destroy()
{
  if (mdl_initialized)
  {
    mdl_initialized= FALSE;
    mdl_locks.destroy();
  }
}


struct mdl_iterate_arg
{
  mdl_iterator_callback callback;
  void *argument;
};


static my_bool mdl_iterate_lock(void *lk, void *a)
{
  MDL_lock *lock= static_cast<MDL_lock*>(lk);
  mdl_iterate_arg *arg= static_cast<mdl_iterate_arg*>(a);
  return lock->iterate(arg->callback, arg->argument);
}


int mdl_iterate(mdl_iterator_callback callback, void *arg)
{
  DBUG_ENTER("mdl_iterate");
  mdl_iterate_arg argument= { callback, arg };
  int res= 1;

  if (LF_PINS *pins= mdl_locks.get_pins())
  {
    res= mdl_iterate_lock(mdl_locks.m_backup_lock, &argument) ||
         lf_hash_iterate(&mdl_locks.m_locks, pins, mdl_iterate_lock,
                         &argument);
    lf_hash_put_pins(pins);
  }
  DBUG_RETURN(res);
}


my_hash_value_type mdl_hash_function(CHARSET_INFO *cs,
                                     const uchar *key, size_t length)
{
  MDL_key *mdl_key= (MDL_key*) (key - offsetof(MDL_key, m_ptr));
  return mdl_key->hash_value();
}


/** Initialize the container for all MDL locks. */

void MDL_map::init()
{
  MDL_key backup_lock_key(MDL_key::BACKUP, "", "");

  m_backup_lock= new (std::nothrow) MDL_lock(&backup_lock_key);

  lf_hash_init(&m_locks, sizeof(MDL_lock), LF_HASH_UNIQUE, 0, 0,
               MDL_lock::mdl_locks_key, &my_charset_bin);
  m_locks.alloc.constructor= MDL_lock::lf_alloc_constructor;
  m_locks.alloc.destructor= MDL_lock::lf_alloc_destructor;
  m_locks.initializer= MDL_lock::lf_hash_initializer;
  m_locks.hash_function= mdl_hash_function;
}


/**
  Destroy the container for all MDL locks.
  @pre It must be empty.
*/

void MDL_map::destroy()
{
  delete m_backup_lock;

  DBUG_ASSERT(!lf_hash_size(&m_locks));
  lf_hash_destroy(&m_locks);
}


/**
  Find MDL_lock object corresponding to the key, create it
  if it does not exist.

  @retval TAL_ACQUIRED Acquired
  @retval TAL_WAIT     Not acquired, must be waited
  @retval TAL_NOWAIT   Not acquired, cannot be waited
  @retval TAL_ERROR    Failure (OOM)
*/

enum tal_status MDL_map::try_acquire_lock(LF_PINS *pins, MDL_key *mdl_key,
                                          MDL_ticket *ticket,
                                          bool wait)
{
  MDL_lock *lock;

  if (mdl_key->mdl_namespace() == MDL_key::BACKUP)
  {
    /*
      Use m_backup_lock shortcut. Such optimization allows to save
      one hash lookup for any statement changing data.

      It works since this namespace contains only one element so keys
      for them look like '<namespace-id>\0\0'.
    */
    DBUG_ASSERT(mdl_key->length() == 3);
    return m_backup_lock->try_acquire_lock(ticket, wait);
  }

retry:
  while (!(lock= (MDL_lock*) lf_hash_search(&m_locks, pins, mdl_key->ptr(),
                                            mdl_key->length())))
    if (lf_hash_insert(&m_locks, pins, (uchar*) mdl_key) == -1)
      return TAL_ERROR;

  enum tal_status res= lock->try_acquire_lock(ticket, wait);
  lf_hash_search_unpin(pins);
  if (res == TAL_ERROR)
    goto retry;

  return res;
}


/**
 * Return thread id of the owner of the lock, if it is owned.
 */

unsigned long
MDL_map::get_lock_owner(LF_PINS *pins, const MDL_key *mdl_key)
{
  DBUG_ASSERT(mdl_key->mdl_namespace() == MDL_key::USER_LOCK);
  if (MDL_lock *lock= (MDL_lock*) lf_hash_search(&m_locks, pins, mdl_key->ptr(),
                                                 mdl_key->length()))
  {
    unsigned long res= lock->get_lock_owner();
    lf_hash_search_unpin(pins);
    return res;
  }
  return 0;
}


/**
  Initialize a metadata locking context.

  This is to be called when a new server connection is created.
*/

MDL_context::MDL_context()
  :
  m_owner(NULL),
  m_needs_thr_lock_abort(FALSE),
  m_waiting_for(NULL),
  m_pins(NULL)
{
  mysql_prlock_init(key_MDL_context_LOCK_waiting_for, &m_LOCK_waiting_for);
}


/**
  Destroy metadata locking context.

  Assumes and asserts that there are no active or pending locks
  associated with this context at the time of the destruction.

  Currently does nothing. Asserts that there are no pending
  or satisfied lock requests. The pending locks must be released
  prior to destruction. This is a new way to express the assertion
  that all tables are closed before a connection is destroyed.
*/

void MDL_context::destroy()
{
  DBUG_ASSERT(m_tickets[MDL_STATEMENT].is_empty());
  DBUG_ASSERT(m_tickets[MDL_TRANSACTION].is_empty());
  DBUG_ASSERT(m_tickets[MDL_EXPLICIT].is_empty());

  mysql_prlock_destroy(&m_LOCK_waiting_for);
  if (m_pins)
    lf_hash_put_pins(m_pins);
}


bool MDL_context::fix_pins()
{
  return m_pins ? false : (m_pins= mdl_locks.get_pins()) == 0;
}


/**
  Initialize a lock request.

  This is to be used for every lock request.

  Note that initialization and allocation are split into two
  calls. This is to allow flexible memory management of lock
  requests. Normally a lock request is stored in statement memory
  (e.g. is a member of struct TABLE_LIST), but we would also like
  to allow allocation of lock requests in other memory roots,
  for example in the grant subsystem, to lock privilege tables.

  The MDL subsystem does not own or manage memory of lock requests.

  @param  mdl_namespace  Id of namespace of object to be locked
  @param  db             Name of database to which the object belongs
  @param  name           Name of of the object
  @param  mdl_type       The MDL lock type for the request.
*/

void MDL_request::init_with_source(MDL_key::enum_mdl_namespace mdl_namespace,
                       const char *db_arg,
                       const char *name_arg,
                       enum_mdl_type mdl_type_arg,
                       enum_mdl_duration mdl_duration_arg,
                       const char *src_file,
                       uint src_line)
{
  key.mdl_key_init(mdl_namespace, db_arg, name_arg);
  type= mdl_type_arg;
  duration= mdl_duration_arg;
  ticket= NULL;
  m_src_file= src_file;
  m_src_line= src_line;
}


/**
  Initialize a lock request using pre-built MDL_key.

  @sa MDL_request::init(namespace, db, name, type).

  @param key_arg       The pre-built MDL key for the request.
  @param mdl_type_arg  The MDL lock type for the request.
*/

void MDL_request::init_by_key_with_source(const MDL_key *key_arg,
                       enum_mdl_type mdl_type_arg,
                       enum_mdl_duration mdl_duration_arg,
                       const char *src_file,
                       uint src_line)
{
  key.mdl_key_init(key_arg);
  type= mdl_type_arg;
  duration= mdl_duration_arg;
  ticket= NULL;
  m_src_file= src_file;
  m_src_line= src_line;
}


MDL_ticket::MDL_ticket(MDL_context *ctx_arg, MDL_request *mdl_request):
#ifndef DBUG_OFF
   m_duration(mdl_request->duration),
#endif
   m_time(0),
   m_fast_lane(nullptr),
   m_type(mdl_request->type),
   m_ctx(ctx_arg),
   m_lock(nullptr)
{
  m_psi= mysql_mdl_create(this,
                          &mdl_request->key,
                          mdl_request->type,
                          mdl_request->duration,
                          PENDING,
                          mdl_request->m_src_file,
                          mdl_request->m_src_line);
}


MDL_ticket::~MDL_ticket()
{
  mysql_mdl_destroy(m_psi);
}


/**
  Return the 'weight' of this ticket for the
  victim selection algorithm. Requests with
  lower weight are preferred to requests
  with higher weight when choosing a victim.
*/

uint MDL_ticket::get_deadlock_weight() const
{
  if (get_key()->mdl_namespace() == MDL_key::BACKUP)
  {
    if (m_type == MDL_BACKUP_FTWRL1)
      return DEADLOCK_WEIGHT_FTWRL1;
    return DEADLOCK_WEIGHT_DDL;
  }
  return m_type >= MDL_SHARED_UPGRADABLE ?
         DEADLOCK_WEIGHT_DDL : DEADLOCK_WEIGHT_DML;
}


/** Construct an empty wait slot. */

MDL_wait::MDL_wait()
  :m_wait_status(EMPTY)
{
  mysql_mutex_init(key_MDL_wait_LOCK_wait_status, &m_LOCK_wait_status, NULL);
  mysql_cond_init(key_MDL_wait_COND_wait_status, &m_COND_wait_status, NULL);
}


/** Destroy system resources. */

MDL_wait::~MDL_wait()
{
  mysql_mutex_destroy(&m_LOCK_wait_status);
  mysql_cond_destroy(&m_COND_wait_status);
}


/**
  Set the status unless it's already set. Return FALSE if set,
  TRUE otherwise.
*/

bool MDL_wait::set_status(enum_wait_status status_arg)
{
  bool was_occupied= TRUE;
  mysql_mutex_lock(&m_LOCK_wait_status);
  if (m_wait_status == EMPTY)
  {
    was_occupied= FALSE;
    m_wait_status= status_arg;
    mysql_cond_signal(&m_COND_wait_status);
  }
  mysql_mutex_unlock(&m_LOCK_wait_status);
  return was_occupied;
}


/** Query the current value of the wait slot. */

MDL_wait::enum_wait_status MDL_wait::get_status()
{
  enum_wait_status result;
  mysql_mutex_lock(&m_LOCK_wait_status);
  result= m_wait_status;
  mysql_mutex_unlock(&m_LOCK_wait_status);
  return result;
}


/** Clear the current value of the wait slot. */

void MDL_wait::reset_status()
{
  mysql_mutex_lock(&m_LOCK_wait_status);
  m_wait_status= EMPTY;
  mysql_mutex_unlock(&m_LOCK_wait_status);
}


/**
  Wait for the status to be assigned to this wait slot.

  @param owner           MDL context owner.
  @param abs_timeout     Absolute time after which waiting should stop.
  @param set_status_on_timeout TRUE  - If in case of timeout waiting
                                       context should close the wait slot by
                                       sending TIMEOUT to itself.
                               FALSE - Otherwise.
  @param wait_state_name  Thread state name to be set for duration of wait.

  @returns Signal posted.
*/

MDL_wait::enum_wait_status
MDL_wait::timed_wait(MDL_context_owner *owner, struct timespec *abs_timeout,
                     bool set_status_on_timeout,
                     const PSI_stage_info *wait_state_name)
{
  PSI_stage_info old_stage;
  enum_wait_status result;
  int wait_result= 0;
  DBUG_ENTER("MDL_wait::timed_wait");

  mysql_mutex_lock(&m_LOCK_wait_status);

  owner->ENTER_COND(&m_COND_wait_status, &m_LOCK_wait_status,
                    wait_state_name, & old_stage);
  thd_wait_begin(NULL, THD_WAIT_META_DATA_LOCK);
  tpool::tpool_wait_begin();
  while (!m_wait_status && !owner->is_killed() &&
         wait_result != ETIMEDOUT && wait_result != ETIME)
  {
#ifdef WITH_WSREP
# ifdef ENABLED_DEBUG_SYNC
    // Allow tests to block thread before MDL-wait
    DEBUG_SYNC(owner->get_thd(), "wsrep_before_mdl_wait");
# endif
    if (WSREP_ON && wsrep_thd_is_BF(owner->get_thd(), false))
    {
      wait_result= mysql_cond_wait(&m_COND_wait_status, &m_LOCK_wait_status);
    }
    else
#endif /* WITH_WSREP */
    wait_result= mysql_cond_timedwait(&m_COND_wait_status, &m_LOCK_wait_status,
                                      abs_timeout);
  }
  tpool::tpool_wait_end();
  thd_wait_end(NULL);

  if (m_wait_status == EMPTY)
  {
    /*
      Wait has ended not due to a status being set from another
      thread but due to this connection/statement being killed or a
      time out.
      To avoid races, which may occur if another thread sets
      GRANTED status before the code which calls this method
      processes the abort/timeout, we assign the status under
      protection of the m_LOCK_wait_status, within the critical
      section. An exception is when set_status_on_timeout is
      false, which means that the caller intends to restart the
      wait.
    */
    if (owner->is_killed())
      m_wait_status= KILLED;
    else if (set_status_on_timeout)
      m_wait_status= TIMEOUT;
  }
  result= m_wait_status;

  owner->EXIT_COND(& old_stage);

  DBUG_RETURN(result);
}


/**
  Add ticket to MDL_lock's list of waiting requests and
  update corresponding bitmap of lock types.
*/

void MDL_lock::Ticket_list::add_ticket(MDL_ticket *ticket)
{
  /*
    Ticket being added to the list must have MDL_ticket::m_lock set,
    since for such tickets methods accessing this member might be
    called by other threads.
  */
  DBUG_ASSERT(ticket->get_lock());
#ifdef WITH_WSREP
  if (WSREP_ON && (this == &(ticket->get_lock()->m_waiting)) &&
      wsrep_thd_is_BF(ticket->get_ctx()->get_thd(), false))
  {
    DBUG_ASSERT(WSREP(ticket->get_ctx()->get_thd()));

    m_list.insert(std::find_if(ticket->get_lock()->m_waiting.begin(),
                               ticket->get_lock()->m_waiting.end(),
                               [](const MDL_ticket &waiting) {
                                 return !wsrep_thd_is_BF(
                                     waiting.get_ctx()->get_thd(), true);
                               }),
                  *ticket);
  }
  else
#endif /* WITH_WSREP */
  {
    /*
      Add ticket to the *back* of the queue to ensure fairness
      among requests with the same priority.
    */
    m_list.push_back(*ticket);
  }
  m_bitmap|= MDL_BIT(ticket->get_type());
  m_type_counters[ticket->get_type()]++;
}


/**
  Remove ticket from MDL_lock's list of requests and
  update corresponding bitmap of lock types.
*/

void MDL_lock::Ticket_list::remove_ticket(MDL_ticket *ticket)
{
  m_list.remove(*ticket);
  /*
    Check if waiting queue has another ticket with the same type as
    one which was removed. If there is no such ticket, i.e. we have
    removed last ticket of particular type, then we need to update
    bitmap of waiting ticket's types.
  */
  if (--m_type_counters[ticket->get_type()] == 0)
    m_bitmap&= ~MDL_BIT(ticket->get_type());
}


/**
  Determine waiting contexts which requests for the lock can be
  satisfied, grant lock to them and wake them up.

  @note Together with MDL_lock::add_ticket() this method implements
        fair scheduling among requests with the same priority.
        It tries to grant lock from the head of waiters list, while
        add_ticket() adds new requests to the back of this list.

*/

void MDL_lock::reschedule_waiters()
{
  bool skip_high_priority= false;
  bitmap_t hog_lock_types= m_strategy->hog_lock_types_bitmap();

  if (m_hog_lock_count >= max_write_lock_count)
  {
    /*
      If number of successively granted high-prio, strong locks has exceeded
      max_write_lock_count give a way to low-prio, weak locks to avoid their
      starvation.
    */

    if ((m_waiting.bitmap() & ~hog_lock_types) != 0)
    {
      /*
        Even though normally when m_hog_lock_count is non-0 there is
        some pending low-prio lock, we still can encounter situation
        when m_hog_lock_count is non-0 and there are no pending low-prio
        locks. This, for example, can happen when a ticket for pending
        low-prio lock was removed from waiters list due to timeout,
        and reschedule_waiters() is called after that to update the
        waiters queue. m_hog_lock_count will be reset to 0 at the
        end of this call in such case.

        Note that it is not an issue if we fail to wake up any pending
        waiters for weak locks in the loop below. This would mean that
        all of them are either killed, timed out or chosen as a victim
        by deadlock resolver, but have not managed to remove ticket
        from the waiters list yet. After tickets will be removed from
        the waiters queue there will be another call to
        reschedule_waiters() with pending bitmap updated to reflect new
        state of waiters queue.
      */
      skip_high_priority= true;
    }
  }

  /*
    Find the first (and hence the oldest) waiting request which
    can be satisfied (taking into account priority). Grant lock to it.
    Repeat the process for the remainder of waiters.
    Note we don't need to re-start iteration from the head of the
    list after satisfying the first suitable request as in our case
    all compatible types of requests have the same priority.

    TODO/FIXME: We should:
                - Either switch to scheduling without priorities
                  which will allow to stop iteration through the
                  list of waiters once we found the first ticket
                  which can't be  satisfied
                - Or implement some check using bitmaps which will
                  allow to stop iteration in cases when, e.g., we
                  grant SNRW lock and there are no pending S or
                  SH locks.
  */
  for (auto it= m_waiting.begin(); it != m_waiting.end(); ++it)
  {
    /*
      Skip high-prio, strong locks if earlier we have decided to give way to
      low-prio, weaker locks.
    */
    if (skip_high_priority &&
        ((MDL_BIT(it->get_type()) & hog_lock_types) != 0))
      continue;

    if (can_grant_lock(it->get_type(), it->get_ctx(),
                       skip_high_priority))
    {
      if (!it->get_ctx()->m_wait.set_status(MDL_wait::GRANTED))
      {
        /*
          Satisfy the found request by updating lock structures.
          It is OK to do so even after waking up the waiter since any
          session which tries to get any information about the state of
          this lock has to acquire MDL_lock::m_rwlock first and thus,
          when manages to do so, already sees an updated state of the
          MDL_lock object.
        */
        auto prev_it= std::prev(it); // this might be begin()-- but the hack
                                     // works because list is circular
        m_waiting.remove_ticket(&*it);
        m_granted.add_ticket(&*it);

        /*
          Increase counter of successively granted high-priority strong locks,
          if we have granted one.
        */
        if ((MDL_BIT(it->get_type()) & hog_lock_types) != 0)
          m_hog_lock_count++;

        it= prev_it;
      }
      /*
        If we could not update the wait slot of the waiter,
        it can be due to fact that its connection/statement was
        killed or it has timed out (i.e. the slot is not empty).
        Since in all such cases the waiter assumes that the lock was
        not been granted, we should keep the request in the waiting
        queue and look for another request to reschedule.
      */
    }
  }

  if ((m_waiting.bitmap() & ~hog_lock_types) == 0)
  {
    /*
      Reset number of successively granted high-prio, strong locks
      if there are no pending low-prio, weak locks.
      This ensures:
      - That m_hog_lock_count is correctly reset after strong lock
      is released and weak locks are granted (or there are no
      other lock requests).
      - That situation when SNW lock is granted along with some SR
      locks, but SW locks are still blocked are handled correctly.
      - That m_hog_lock_count is zero in most cases when there are no pending
      weak locks (see comment at the start of this method for example of
      exception). This allows to save on checks at the start of this method.
    */
    m_hog_lock_count= 0;
  }
}


/**
  Compatibility (or rather "incompatibility") matrices for scoped metadata
  lock.
  Scoped locks are database (or schema) locks.
  Arrays of bitmaps which elements specify which granted/waiting locks
  are incompatible with type of lock being requested.

  The first array specifies if particular type of request can be satisfied
  if there is granted scoped lock of certain type.

  (*)  Since intention shared scoped locks (IS) are compatible with all other
       type of locks, they don't need to be implemented and there is no code
       for them.

             | Type of active   |
     Request |   scoped lock    |
      type   | IS(*)  IX   S  X |
    ---------+------------------+
    IS(*)    |  +      +   +  + |
    IX       |  +      +   -  - |
    S        |  +      -   +  - |
    X        |  +      -   -  - |

  The second array specifies if particular type of request can be satisfied
  if there is already waiting request for the scoped lock of certain type.
  I.e. it specifies what is the priority of different lock types.

             |    Pending      |
     Request |  scoped lock    |
      type   | IS(*)  IX  S  X |
    ---------+-----------------+
    IS(*)    |  +      +  +  + |
    IX       |  +      +  -  - |
    S        |  +      +  +  - |
    X        |  +      +  +  + |

  Here: "+" -- means that request can be satisfied
        "-" -- means that request can't be satisfied and should wait

  Note that relation between scoped locks and objects locks requested
  by statement is not straightforward and is therefore fully defined
  by SQL-layer.
  For example, in order to support global read lock implementation
  SQL-layer acquires IX lock in GLOBAL namespace for each statement
  that can modify metadata or data (i.e. for each statement that
  needs SW, SU, SNW, SNRW or X object locks). OTOH, to ensure that
  DROP DATABASE works correctly with concurrent DDL, IX metadata locks
  in SCHEMA namespace are acquired for DDL statements which can update
  metadata in the schema (i.e. which acquire SU, SNW, SNRW and X locks
  on schema objects) and aren't acquired for DML.
*/

const MDL_lock::bitmap_t
MDL_lock::MDL_scoped_lock::m_granted_incompatible[MDL_TYPE_END]=
{
  MDL_BIT(MDL_EXCLUSIVE) | MDL_BIT(MDL_SHARED),
  MDL_BIT(MDL_EXCLUSIVE) | MDL_BIT(MDL_INTENTION_EXCLUSIVE),
  0, 0, 0, 0, 0, 0, 0,
  MDL_BIT(MDL_EXCLUSIVE) | MDL_BIT(MDL_SHARED) | MDL_BIT(MDL_INTENTION_EXCLUSIVE)
};

const MDL_lock::bitmap_t
MDL_lock::MDL_scoped_lock::m_waiting_incompatible[MDL_TYPE_END]=
{
  MDL_BIT(MDL_EXCLUSIVE) | MDL_BIT(MDL_SHARED),
  MDL_BIT(MDL_EXCLUSIVE), 0, 0, 0, 0, 0, 0, 0, 0
};


/**
  Compatibility (or rather "incompatibility") matrices for per-object
  metadata lock. Arrays of bitmaps which elements specify which granted/
  waiting locks are incompatible with type of lock being requested.

  The first array specifies if particular type of request can be satisfied
  if there is granted lock of certain type.

     Request  |  Granted requests for lock         |
      type    | S  SH  SR  SW  SU  SRO SNW SNRW X  |
    ----------+------------------------------------+
    S         | +   +   +   +   +   +   +   +   -  |
    SH        | +   +   +   +   +   +   +   +   -  |
    SR        | +   +   +   +   +   +   +   -   -  |
    SW        | +   +   +   +   +   -   -   -   -  |
    SU        | +   +   +   +   -   +   -   -   -  |
    SRO       | +   +   +   -   +   +   +   -   -  |
    SNW       | +   +   +   -   -   +   -   -   -  |
    SNRW      | +   +   -   -   -   -   -   -   -  |
    X         | -   -   -   -   -   -   -   -   -  |
    SU -> X   | -   -   -   -   0   -   0   0   0  |
    SNW -> X  | -   -   -   0   0   -   0   0   0  |
    SNRW -> X | -   -   0   0   0   0   0   0   0  |

  The second array specifies if particular type of request can be satisfied
  if there is waiting request for the same lock of certain type. In other
  words it specifies what is the priority of different lock types.

     Request  |  Pending requests for lock        |
      type    | S  SH  SR  SW  SU  SRO SNW SNRW X |
    ----------+-----------------------------------+
    S         | +   +   +   +   +   +   +   +   - |
    SH        | +   +   +   +   +   +   +   +   + |
    SR        | +   +   +   +   +   +   +   -   - |
    SW        | +   +   +   +   +   +   -   -   - |
    SU        | +   +   +   +   +   +   +   +   - |
    SRO       | +   +   +   -   +   +   +   -   - |
    SNW       | +   +   +   +   +   +   +   +   - |
    SNRW      | +   +   +   +   +   +   +   +   - |
    X         | +   +   +   +   +   +   +   +   + |
    SU -> X   | +   +   +   +   +   +   +   +   + |
    SNW -> X  | +   +   +   +   +   +   +   +   + |
    SNRW -> X | +   +   +   +   +   +   +   +   + |

  Here: "+" -- means that request can be satisfied
        "-" -- means that request can't be satisfied and should wait
        "0" -- means impossible situation which will trigger assert

  @note In cases then current context already has "stronger" type
        of lock on the object it will be automatically granted
        thanks to usage of the MDL_context::find_ticket() method.

  @note IX locks are excluded since they are not used for per-object
        metadata locks.
*/

const MDL_lock::bitmap_t
MDL_lock::MDL_object_lock::m_granted_incompatible[MDL_TYPE_END]=
{
  0,
  MDL_BIT(MDL_EXCLUSIVE),
  MDL_BIT(MDL_EXCLUSIVE),
  MDL_BIT(MDL_EXCLUSIVE) | MDL_BIT(MDL_SHARED_NO_READ_WRITE),
  MDL_BIT(MDL_EXCLUSIVE) | MDL_BIT(MDL_SHARED_NO_READ_WRITE) |
    MDL_BIT(MDL_SHARED_NO_WRITE) | MDL_BIT(MDL_SHARED_READ_ONLY),
  MDL_BIT(MDL_EXCLUSIVE) | MDL_BIT(MDL_SHARED_NO_READ_WRITE) |
    MDL_BIT(MDL_SHARED_NO_WRITE) | MDL_BIT(MDL_SHARED_UPGRADABLE),
  MDL_BIT(MDL_EXCLUSIVE) | MDL_BIT(MDL_SHARED_NO_READ_WRITE) |
    MDL_BIT(MDL_SHARED_WRITE),
  MDL_BIT(MDL_EXCLUSIVE) | MDL_BIT(MDL_SHARED_NO_READ_WRITE) |
    MDL_BIT(MDL_SHARED_NO_WRITE) | MDL_BIT(MDL_SHARED_UPGRADABLE) |
    MDL_BIT(MDL_SHARED_WRITE),
  MDL_BIT(MDL_EXCLUSIVE) | MDL_BIT(MDL_SHARED_NO_READ_WRITE) |
    MDL_BIT(MDL_SHARED_NO_WRITE) | MDL_BIT(MDL_SHARED_READ_ONLY) |
    MDL_BIT(MDL_SHARED_UPGRADABLE) | MDL_BIT(MDL_SHARED_WRITE) |
    MDL_BIT(MDL_SHARED_READ),
  MDL_BIT(MDL_EXCLUSIVE) | MDL_BIT(MDL_SHARED_NO_READ_WRITE) |
    MDL_BIT(MDL_SHARED_NO_WRITE) | MDL_BIT(MDL_SHARED_READ_ONLY) |
    MDL_BIT(MDL_SHARED_UPGRADABLE) | MDL_BIT(MDL_SHARED_WRITE) |
    MDL_BIT(MDL_SHARED_READ) | MDL_BIT(MDL_SHARED_HIGH_PRIO) |
    MDL_BIT(MDL_SHARED)
};


const MDL_lock::bitmap_t
MDL_lock::MDL_object_lock::m_waiting_incompatible[MDL_TYPE_END]=
{
  0,
  MDL_BIT(MDL_EXCLUSIVE),
  0,
  MDL_BIT(MDL_EXCLUSIVE) | MDL_BIT(MDL_SHARED_NO_READ_WRITE),
  MDL_BIT(MDL_EXCLUSIVE) | MDL_BIT(MDL_SHARED_NO_READ_WRITE) |
    MDL_BIT(MDL_SHARED_NO_WRITE),
  MDL_BIT(MDL_EXCLUSIVE),
  MDL_BIT(MDL_EXCLUSIVE) | MDL_BIT(MDL_SHARED_NO_READ_WRITE) |
    MDL_BIT(MDL_SHARED_WRITE),
  MDL_BIT(MDL_EXCLUSIVE),
  MDL_BIT(MDL_EXCLUSIVE),
  0
};


/**
  Compatibility (or rather "incompatibility") matrices for backup metadata
  lock. Arrays of bitmaps which elements specify which granted/waiting locks
  are incompatible with type of lock being requested.

  The first array specifies if particular type of request can be satisfied
  if there is granted backup lock of certain type.

     Request  |           Type of active backup lock                    |
      type    | S0  S1  S2  S3  S4  F1  F2   D  TD  SD   DD  BL  AC  C  |
    ----------+---------------------------------------------------------+
    S0        |  -   -   -   -   -   +   +   +   +   +   +   +   +   +  |
    S1        |  -   +   +   +   +   +   +   +   +   +   +   +   +   +  |
    S2        |  -   +   +   +   +   +   +   -   +   +   +   +   +   +  |
    S3        |  -   +   +   +   +   +   +   -   +   +   -   +   +   +  |
    S4        |  -   +   +   +   +   +   +   -   +   -   -   +   +   -  |
    FTWRL1    |  +   +   +   +   +   +   +   -   -   -   -   +   -   +  |
    FTWRL2    |  +   +   +   +   +   +   +   -   -   -   -   +   -   -  |
    D         |  +   -   -   -   -   -   -   +   +   +   +   +   +   +  |
    TD        |  +   +   +   +   +   -   -   +   +   +   +   +   +   +  |
    SD        |  +   +   +   +   -   -   -   +   +   +   +   +   +   +  |
    DDL       |  +   +   +   -   -   -   -   +   +   +   +   -   +   +  |
    BLOCK_DDL |  -   +   +   +   +   +   +   +   +   +   -   +   +   +  |
    ALTER_COP |  +   +   +   +   +   -   -   +   +   +   +   +   +   +  |
    COMMIT    |  +   +   +   +   -   +   -   +   +   +   +   +   +   +  |

  The second array specifies if particular type of request can be satisfied
  if there is already waiting request for the backup lock of certain type.
  I.e. it specifies what is the priority of different lock types.

     Request  |               Pending backup lock                       |
      type    | S0  S1  S2  S3  S4  F1  F2   D  TD  SD   DD  BL  AC  C  |
    ----------+---------------------------------------------------------+
    S0        |  +   -   -   -   -   +   +   +   +   +   +   +   +   +  |
    S1        |  +   +   +   +   +   +   +   +   +   +   +   +   +   +  |
    S2        |  +   +   +   +   +   +   +   +   +   +   +   +   +   +  |
    S3        |  +   +   +   +   +   +   +   +   +   +   +   +   +   +  |
    S4        |  +   +   +   +   +   +   +   +   +   +   +   +   +   +  |
    FTWRL1    |  +   +   +   +   +   +   +   +   +   +   +   +   +   +  |
    FTWRL2    |  +   +   +   +   +   +   +   +   +   +   +   +   +   +  |
    D         |  +   -   -   -   -   -   -   +   +   +   +   +   +   +  |
    TD        |  +   +   +   +   +   -   -   +   +   +   +   +   +   +  |
    SD        |  +   +   +   +   -   -   -   +   +   +   +   +   +   +  |
    DDL       |  +   +   +   -   -   -   -   +   +   +   +   -   +   +  |
    BLOCK_DDL |  +   +   +   +   +   +   +   +   +   +   +   +   +   +  |
    ALTER_COP |  +   +   +   +   +   -   -   +   +   +   +   +   +   +  |
    COMMIT    |  +   +   +   +   -   +   -   +   +   +   +   +   +   +  |

  Here: "+" -- means that request can be satisfied
        "-" -- means that request can't be satisfied and should wait
*/

/*
  NOTE: If you add a new MDL_BACKUP_XXX level lock, you have to also add it
  to MDL_BACKUP_START in the two arrays below!
*/

const MDL_lock::bitmap_t
MDL_lock::MDL_backup_lock::m_granted_incompatible[MDL_BACKUP_END]=
{
  /* MDL_BACKUP_START */
  MDL_BIT(MDL_BACKUP_START) | MDL_BIT(MDL_BACKUP_FLUSH) | MDL_BIT(MDL_BACKUP_WAIT_FLUSH) | MDL_BIT(MDL_BACKUP_WAIT_DDL) | MDL_BIT(MDL_BACKUP_WAIT_COMMIT) | MDL_BIT(MDL_BACKUP_BLOCK_DDL),
  MDL_BIT(MDL_BACKUP_START),
  MDL_BIT(MDL_BACKUP_START) | MDL_BIT(MDL_BACKUP_DML),
  MDL_BIT(MDL_BACKUP_START) | MDL_BIT(MDL_BACKUP_DML) | MDL_BIT(MDL_BACKUP_DDL),
  MDL_BIT(MDL_BACKUP_START) | MDL_BIT(MDL_BACKUP_DML) | MDL_BIT(MDL_BACKUP_SYS_DML) | MDL_BIT(MDL_BACKUP_DDL) | MDL_BIT(MDL_BACKUP_COMMIT),

  /* MDL_BACKUP_FTWRL1 */
  MDL_BIT(MDL_BACKUP_DML) | MDL_BIT(MDL_BACKUP_TRANS_DML) | MDL_BIT(MDL_BACKUP_SYS_DML) | MDL_BIT(MDL_BACKUP_DDL) | MDL_BIT(MDL_BACKUP_ALTER_COPY),
  MDL_BIT(MDL_BACKUP_DML) | MDL_BIT(MDL_BACKUP_TRANS_DML) | MDL_BIT(MDL_BACKUP_SYS_DML) | MDL_BIT(MDL_BACKUP_DDL) | MDL_BIT(MDL_BACKUP_ALTER_COPY) | MDL_BIT(MDL_BACKUP_COMMIT),
  /* MDL_BACKUP_DML */
  MDL_BIT(MDL_BACKUP_FLUSH) | MDL_BIT(MDL_BACKUP_WAIT_FLUSH) | MDL_BIT(MDL_BACKUP_WAIT_DDL) | MDL_BIT(MDL_BACKUP_WAIT_COMMIT) | MDL_BIT(MDL_BACKUP_FTWRL1) | MDL_BIT(MDL_BACKUP_FTWRL2),
  MDL_BIT(MDL_BACKUP_FTWRL1) | MDL_BIT(MDL_BACKUP_FTWRL2),
  MDL_BIT(MDL_BACKUP_WAIT_COMMIT) | MDL_BIT(MDL_BACKUP_FTWRL1) | MDL_BIT(MDL_BACKUP_FTWRL2),
  /* MDL_BACKUP_DDL */
  MDL_BIT(MDL_BACKUP_WAIT_DDL) | MDL_BIT(MDL_BACKUP_WAIT_COMMIT) | MDL_BIT(MDL_BACKUP_FTWRL1) | MDL_BIT(MDL_BACKUP_FTWRL2) | MDL_BIT(MDL_BACKUP_BLOCK_DDL),
  /* MDL_BACKUP_BLOCK_DDL */
  MDL_BIT(MDL_BACKUP_START) | MDL_BIT(MDL_BACKUP_FLUSH) | MDL_BIT(MDL_BACKUP_WAIT_FLUSH) | MDL_BIT(MDL_BACKUP_WAIT_DDL) | MDL_BIT(MDL_BACKUP_WAIT_COMMIT) | MDL_BIT(MDL_BACKUP_BLOCK_DDL) | MDL_BIT(MDL_BACKUP_DDL),
  MDL_BIT(MDL_BACKUP_FTWRL1) | MDL_BIT(MDL_BACKUP_FTWRL2),
  /* MDL_BACKUP_COMMIT */
  MDL_BIT(MDL_BACKUP_WAIT_COMMIT) | MDL_BIT(MDL_BACKUP_FTWRL2)
};


const MDL_lock::bitmap_t
MDL_lock::MDL_backup_lock::m_waiting_incompatible[MDL_BACKUP_END]=
{
  /* MDL_BACKUP_START */
  MDL_BIT(MDL_BACKUP_FLUSH) | MDL_BIT(MDL_BACKUP_WAIT_FLUSH) | MDL_BIT(MDL_BACKUP_WAIT_DDL) | MDL_BIT(MDL_BACKUP_WAIT_COMMIT) | MDL_BIT(MDL_BACKUP_BLOCK_DDL),
  0,
  0,
  0,
  0,
  /* MDL_BACKUP_FTWRL1 */
  0,
  0,

  /* MDL_BACKUP_DML */
  MDL_BIT(MDL_BACKUP_FLUSH) | MDL_BIT(MDL_BACKUP_WAIT_FLUSH) | MDL_BIT(MDL_BACKUP_WAIT_DDL) | MDL_BIT(MDL_BACKUP_WAIT_COMMIT) | MDL_BIT(MDL_BACKUP_FTWRL1) | MDL_BIT(MDL_BACKUP_FTWRL2),
  MDL_BIT(MDL_BACKUP_FTWRL1) | MDL_BIT(MDL_BACKUP_FTWRL2),
  MDL_BIT(MDL_BACKUP_WAIT_COMMIT) | MDL_BIT(MDL_BACKUP_FTWRL1) | MDL_BIT(MDL_BACKUP_FTWRL2),
  /* MDL_BACKUP_DDL */
  MDL_BIT(MDL_BACKUP_WAIT_DDL) | MDL_BIT(MDL_BACKUP_WAIT_COMMIT) | MDL_BIT(MDL_BACKUP_FTWRL1) | MDL_BIT(MDL_BACKUP_FTWRL2) | MDL_BIT(MDL_BACKUP_BLOCK_DDL),
  /* MDL_BACKUP_BLOCK_DDL */
  MDL_BIT(MDL_BACKUP_START),
  MDL_BIT(MDL_BACKUP_FTWRL1) | MDL_BIT(MDL_BACKUP_FTWRL2),
  /* MDL_BACKUP_COMMIT */
  MDL_BIT(MDL_BACKUP_WAIT_COMMIT) | MDL_BIT(MDL_BACKUP_FTWRL2)
};


/**
  Check if request for the metadata lock can be satisfied given its
  current state.

  New lock request can be satisfied iff:
  - There are no incompatible types of satisfied requests
    in other contexts
  - There are no waiting requests which have higher priority
    than this request when priority was not ignored.

  @param  type_arg             The requested lock type.
  @param  requestor_ctx        The MDL context of the requestor.
  @param  ignore_lock_priority Ignore lock priority.

  @retval TRUE   Lock request can be satisfied
  @retval FALSE  There is some conflicting lock.

  @note In cases then current context already has "stronger" type
        of lock on the object it will be automatically granted
        thanks to usage of the MDL_context::find_ticket() method.
*/

bool
MDL_lock::can_grant_lock(enum_mdl_type type_arg,
                         MDL_context *requestor_ctx,
                         bool ignore_lock_priority) const
{
  bitmap_t waiting_incompat_map= incompatible_waiting_types_bitmap()[type_arg];
  bitmap_t granted_incompat_map= incompatible_granted_types_bitmap()[type_arg];

#ifdef WITH_WSREP
  /*
    Approve lock request in BACKUP namespace for BF threads.
  */
  if (!wsrep_check_mode(WSREP_MODE_BF_MARIABACKUP) &&
      (wsrep_thd_is_toi(requestor_ctx->get_thd()) ||
       wsrep_thd_is_applying(requestor_ctx->get_thd())) &&
      key.mdl_namespace() == MDL_key::BACKUP)
  {
    bool waiting_incompatible= m_waiting.bitmap() & waiting_incompat_map;
    bool granted_incompatible= m_granted.bitmap() & granted_incompat_map;
    if (waiting_incompatible || granted_incompatible)
    {
      WSREP_DEBUG("global lock granted for BF%s: %lu %s",
                  waiting_incompatible ? " (waiting queue)" : "",
                  thd_get_thread_id(requestor_ctx->get_thd()),
                  wsrep_thd_query(requestor_ctx->get_thd()));
    }
    return true;
  }
#endif /* WITH_WSREP */

  if (!ignore_lock_priority && (m_waiting.bitmap() & waiting_incompat_map))
    return false;

  if (m_granted.bitmap() & granted_incompat_map)
  {
    bool can_grant= true;

    /* Check that the incompatible lock belongs to some other context. */
    for (const auto &ticket : m_granted)
    {
      if (ticket.get_ctx() != requestor_ctx &&
          ticket.is_incompatible_when_granted(type_arg))
      {
        can_grant= false;
#ifdef WITH_WSREP
        /*
          non WSREP threads must report conflict immediately
          note: RSU processing wsrep threads, have wsrep_on==OFF
        */
        if (WSREP(requestor_ctx->get_thd()) ||
            requestor_ctx->get_thd()->wsrep_cs().mode() ==
            wsrep::client_state::m_rsu)
        {
          wsrep_handle_mdl_conflict(requestor_ctx, &ticket, &key);
          if (wsrep_log_conflicts)
          {
            auto key= ticket.get_key();
            WSREP_INFO("MDL conflict db=%s table=%s ticket=%d solved by abort",
                       key->db_name(), key->name(), ticket.get_type());
          }
          continue;
        }
#endif /* WITH_WSREP */
        break;
      }
    }
    return can_grant;
  }
  return true;
}


/**
  Removes ticket from waiting or pending queue and awakes waiters.

  Once ticket is removed from the list, this thread doesn't hold
  references to this lock and it can be destroyed concurrently. It
  means once m_rwlock is released, "this" cannot be referenced anymore.

  Non-fast lane locks close fast lanes whenever they're registered in
  MDL_lock. Whenever such locks are being deregistered, fast lanes must
  be reopened. Once all closers are gone, that is number of close calls
  equals to number of open calls, fast lanes become available again.
*/

void MDL_lock::remove_ticket(LF_PINS *pins, Ticket_list MDL_lock::*list,
                             MDL_ticket *ticket)
{
  DBUG_ASSERT(!ticket->m_fast_lane.load(std::memory_order_relaxed));
  mysql_prlock_wrlock(&m_rwlock);
  (this->*list).remove_ticket(ticket);
  if (is_empty())
  {
    /* Never destroy pre-allocated MDL_lock object in BACKUP namespace. */
    if (key.mdl_namespace() != MDL_key::BACKUP)
    {
      m_strategy= 0;
      mysql_prlock_unlock(&m_rwlock);
      mdl_locks.remove(pins, &key);
      return;
    }
  }
  else
  {
    /*
      There can be some contexts waiting to acquire a lock
      which now might be able to do it. Grant the lock to
      them and wake them up!

      We always try to reschedule locks, since there is no easy way
      (i.e. by looking at the bitmaps) to find out whether it is
      required or not.
      In a general case, even when the queue's bitmap is not changed
      after removal of the ticket, there is a chance that some request
      can be satisfied (due to the fact that a granted request
      reflected in the bitmap might belong to the same context as a
      pending request).
    */
    reschedule_waiters();
  }
  m_fast_road.reopen(ticket->get_type());
  mysql_prlock_unlock(&m_rwlock);
}


MDL_wait_for_graph_visitor::~MDL_wait_for_graph_visitor()
= default;


MDL_wait_for_subgraph::~MDL_wait_for_subgraph()
= default;

/**
  Check if ticket represents metadata lock of "stronger" or equal type
  than specified one. I.e. if metadata lock represented by ticket won't
  allow any of locks which are not allowed by specified type of lock.

  @return TRUE  if ticket has stronger or equal type
          FALSE otherwise.
*/

bool MDL_ticket::has_stronger_or_equal_type(enum_mdl_type type) const
{
  const auto *granted_incompat_map= m_lock->incompatible_granted_types_bitmap();
  return ! (granted_incompat_map[type] & ~(granted_incompat_map[m_type]));
}


bool MDL_ticket::is_incompatible_when_granted(enum_mdl_type type) const
{
  return (MDL_BIT(m_type) &
          m_lock->incompatible_granted_types_bitmap()[type]);
}


bool MDL_ticket::is_incompatible_when_waiting(enum_mdl_type type) const
{
  return (MDL_BIT(m_type) &
          m_lock->incompatible_waiting_types_bitmap()[type]);
}


static const LEX_STRING
*get_mdl_lock_name(MDL_key::enum_mdl_namespace mdl_namespace,
                   enum_mdl_type type)
{
  return mdl_namespace == MDL_key::BACKUP ?
         &backup_lock_types[type] :
         &lock_types[type];
}


const LEX_STRING *MDL_ticket::get_type_name() const
{
  return get_mdl_lock_name(get_key()->mdl_namespace(), m_type);
}

const LEX_STRING *MDL_ticket::get_type_name(enum_mdl_type type) const
{
  return get_mdl_lock_name(get_key()->mdl_namespace(), type);
}


/**
  Check whether the context already holds a compatible lock ticket
  on an object.
  Start searching from list of locks for the same duration as lock
  being requested. If not look at lists for other durations.

  @param mdl_request  Lock request object for lock to be acquired
  @param[out] result_duration  Duration of lock which was found.

  @note Tickets which correspond to lock types "stronger" than one
        being requested are also considered compatible.

  @return A pointer to the lock ticket for the object or NULL otherwise.
*/

MDL_ticket *
MDL_context::find_ticket(MDL_request *mdl_request,
                         enum_mdl_duration *result_duration)
{
  MDL_ticket *ticket;
  int i;

  for (i= 0; i < MDL_DURATION_END; i++)
  {
    enum_mdl_duration duration= (enum_mdl_duration)((mdl_request->duration+i) %
                                                    MDL_DURATION_END);
    Ticket_iterator it(m_tickets[duration]);

    while ((ticket= it++))
    {
      if (mdl_request->key.is_equal(ticket->get_key()) &&
          ticket->has_stronger_or_equal_type(mdl_request->type))
      {
        DBUG_PRINT("info", ("Adding mdl lock %s to %s",
                            get_mdl_lock_name(mdl_request->key.mdl_namespace(),
                                              mdl_request->type)->str,
                            ticket->get_type_name()->str));
        *result_duration= duration;
        return ticket;
      }
    }
  }
  return NULL;
}


/**
  Try to acquire one lock.

  Unlike exclusive locks, shared locks are acquired one by
  one. This is interface is chosen to simplify introduction of
  the new locking API to the system. MDL_context::try_acquire_lock()
  is currently used from open_table(), and there we have only one
  table to work with.

  This function may also be used to try to acquire an exclusive
  lock on a destination table, by ALTER TABLE ... RENAME.

  Returns immediately without any side effect if encounters a lock
  conflict. Otherwise takes the lock.

  FIXME: Compared to lock_table_name_if_not_cached() (from 5.1)
         it gives slightly more false negatives.

  @param mdl_request [in/out] Lock request object for lock to be acquired

  @retval  FALSE   Success. The lock may have not been acquired.
                   Check the ticket, if it's NULL, a conflicting lock
                   exists.
  @retval  TRUE    Out of resources, an error has been reported.
*/

bool
MDL_context::try_acquire_lock(MDL_request *mdl_request)
{
  return try_acquire_lock_impl(mdl_request, nullptr);
}


/**
  Auxiliary method for acquiring lock without waiting.

  @param mdl_request [in/out] Lock request object for lock to be acquired
  @param out_ticket  [out]    Ticket for the request in case when lock
                              has not been acquired.

  @retval  FALSE   Success. The lock may have not been acquired.
                   Check MDL_request::ticket, if it's NULL, a conflicting
                   lock exists. In this case "out_ticket" out parameter
                   points to ticket which was constructed for the request.
                   MDL_ticket::m_lock points to the corresponding MDL_lock
                   object and MDL_lock::m_rwlock write-locked.
  @retval  TRUE    Out of resources, an error has been reported.
*/

bool
MDL_context::try_acquire_lock_impl(MDL_request *mdl_request,
                                   MDL_ticket **out_ticket)
{
  MDL_ticket *ticket;
  enum_mdl_duration found_duration;

  /* Don't take chances in production. */
  DBUG_ASSERT(mdl_request->ticket == NULL);
  mdl_request->ticket= NULL;

  /*
    Check whether the context already holds a shared lock on the object,
    and if so, grant the request.
  */
  if ((ticket= find_ticket(mdl_request, &found_duration)))
  {
    DBUG_ASSERT(ticket->m_lock);
    DBUG_ASSERT(ticket->has_stronger_or_equal_type(mdl_request->type));
    /*
      If the request is for a transactional lock, and we found
      a transactional lock, just reuse the found ticket.

      It's possible that we found a transactional lock,
      but the request is for a HANDLER lock. In that case HANDLER
      code will clone the ticket (see below why it's needed).

      If the request is for a transactional lock, and we found
      a HANDLER lock, create a copy, to make sure that when user
      does HANDLER CLOSE, the transactional lock is not released.

      If the request is for a handler lock, and we found a
      HANDLER lock, also do the clone. HANDLER CLOSE for one alias
      should not release the lock on the table HANDLER opened through
      a different alias.
    */
    mdl_request->ticket= ticket;
    if ((found_duration != mdl_request->duration ||
         mdl_request->duration == MDL_EXPLICIT) &&
        clone_ticket(mdl_request))
    {
      /* Clone failed. */
      mdl_request->ticket= NULL;
      return TRUE;
    }
    return FALSE;
  }

  if (fix_pins())
    return TRUE;

  if (!(ticket= new (std::nothrow) MDL_ticket(this, mdl_request)))
    return TRUE;

  if (metadata_lock_info_plugin_loaded)
    ticket->m_time= microsecond_interval_timer();

  switch (mdl_locks.try_acquire_lock(m_pins, &mdl_request->key, ticket,
                                     out_ticket != nullptr)) {
  case TAL_ACQUIRED:
    m_tickets[mdl_request->duration].push_front(ticket);
    mdl_request->ticket= ticket;
    mysql_mdl_set_status(ticket->m_psi, MDL_ticket::GRANTED);
    break;
  case TAL_WAIT:
    DBUG_ASSERT(out_ticket);
    *out_ticket= ticket;
    break;
  case TAL_NOWAIT:
    DBUG_PRINT("mdl", ("Nowait: <ticket->m_lock unavailable>"));
    delete ticket;
    break;
  default:
    delete ticket;
    return TRUE;
  }
  return FALSE;
}


/**
  Create a copy of a granted ticket.
  This is used to make sure that HANDLER ticket
  is never shared with a ticket that belongs to
  a transaction, so that when we HANDLER CLOSE,
  we don't release a transactional ticket, and
  vice versa -- when we COMMIT, we don't mistakenly
  release a ticket for an open HANDLER.

  @retval TRUE   Out of memory.
  @retval FALSE  Success.
*/

bool
MDL_context::clone_ticket(MDL_request *mdl_request)
{
  MDL_ticket *ticket;


  /*
    Since in theory we can clone ticket belonging to a different context
    we need to prepare target context for possible attempts to release
    lock and thus possible removal of MDL_lock from MDL_map container.
    So we allocate pins to be able to work with this container if they
    are not allocated already.
  */
  if (fix_pins())
    return TRUE;

  /*
    By submitting mdl_request->type to MDL_ticket constructor
    we effectively downgrade the cloned lock to the level of
    the request.
  */
  if (!(ticket= new (std::nothrow) MDL_ticket(this, mdl_request)))
    return TRUE;

  /* clone() is not supposed to be used to get a stronger lock. */
  DBUG_ASSERT(mdl_request->ticket->has_stronger_or_equal_type(ticket->m_type));

  ticket->m_lock= mdl_request->ticket->m_lock;
  ticket->m_time= mdl_request->ticket->m_time;
  mdl_request->ticket= ticket;

  ticket->m_lock->add_cloned_ticket(ticket);

  m_tickets[mdl_request->duration].push_front(ticket);

  mysql_mdl_set_status(ticket->m_psi, MDL_ticket::GRANTED);

  return FALSE;
}


/**
  Check if there is any conflicting lock that could cause this thread
  to wait for another thread which is not ready to commit.
  This is always an error, as the upper level of parallel replication
  should not allow a scheduling of a conflicting DDL until all earlier
  transactions have been committed.

  This function is only called for a slave using parallel replication
  and trying to get an exclusive lock for the table.
*/

#ifndef DBUG_OFF
bool MDL_lock::check_if_conflicting_replication_locks(MDL_context *ctx)
{
  rpl_group_info *rgi_slave= ctx->get_thd()->rgi_slave;

  if (!rgi_slave->gtid_sub_id)
    return 0;

  for (const auto &conflicting_ticket : m_granted)
  {
    if (conflicting_ticket.get_ctx() != ctx)
    {
      MDL_context *conflicting_ctx= conflicting_ticket.get_ctx();
      rpl_group_info *conflicting_rgi_slave;
      conflicting_rgi_slave= conflicting_ctx->get_thd()->rgi_slave;

      /*
        If the conflicting thread is another parallel replication
        thread for the same master and it's not in commit or post-commit stages,
        then the current transaction has started too early and something is
        seriously wrong.
      */
      if (conflicting_rgi_slave &&
          conflicting_rgi_slave->gtid_sub_id &&
          conflicting_rgi_slave->rli == rgi_slave->rli &&
          conflicting_rgi_slave->current_gtid.domain_id ==
          rgi_slave->current_gtid.domain_id &&
          !((conflicting_rgi_slave->did_mark_start_commit ||
             conflicting_rgi_slave->worker_error)           ||
            conflicting_rgi_slave->finish_event_group_called))
        return 1;                               // Fatal error
    }
  }
  return 0;
}
#endif


/**
  Acquire one lock with waiting for conflicting locks to go away if needed.

  @param mdl_request [in/out] Lock request object for lock to be acquired

  @param lock_wait_timeout [in] Seconds to wait before timeout.

  @retval  FALSE   Success. MDL_request::ticket points to the ticket
                   for the lock.
  @retval  TRUE    Failure (Out of resources or waiting is aborted),
*/

bool
MDL_context::acquire_lock(MDL_request *mdl_request, double lock_wait_timeout)
{
  MDL_lock *lock;
  MDL_ticket *ticket;
  MDL_wait::enum_wait_status wait_status;
  DBUG_ENTER("MDL_context::acquire_lock");
  DBUG_ASSERT(m_wait.get_status() == MDL_wait::EMPTY);
#ifdef DBUG_TRACE
  const char *mdl_lock_name= get_mdl_lock_name(
    mdl_request->key.mdl_namespace(), mdl_request->type)->str;
#endif
  DBUG_PRINT("enter", ("lock_type: %s  timeout: %f",
                       mdl_lock_name,
                       lock_wait_timeout));

  if (try_acquire_lock_impl(mdl_request, lock_wait_timeout ? &ticket : nullptr))
  {
    DBUG_PRINT("mdl", ("OOM: %s", mdl_lock_name));
    DBUG_RETURN(TRUE);
  }

  if (mdl_request->ticket)
  {
    /*
      We have managed to acquire lock without waiting.
      MDL_lock, MDL_context and MDL_request were updated
      accordingly, so we can simply return success.
    */
    DBUG_PRINT("info", ("Got lock without waiting"));
    DBUG_PRINT("mdl", ("Seized:   %s", dbug_print_mdl(mdl_request->ticket)));
    DBUG_RETURN(FALSE);
  }

  /*
    Our attempt to acquire lock without waiting has failed.
    As a result of this attempt we got MDL_ticket with m_lock
    member pointing to the corresponding MDL_lock object which
    has MDL_lock::m_rwlock write-locked.
  */
  if (lock_wait_timeout == 0)
  {
    my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
    DBUG_RETURN(TRUE);
  }

#ifdef DBUG_TRACE
    const char *ticket_msg= dbug_print_mdl(ticket);
#endif

#ifdef WITH_WSREP
  if (WSREP(get_thd()))
  {
    THD* requester= get_thd();
    bool requester_toi= wsrep_thd_is_toi(requester) || wsrep_thd_is_applying(requester);
    WSREP_DEBUG("::acquire_lock is TOI %d for %s", requester_toi,
                wsrep_thd_query(requester));
    if (requester_toi)
      THD_STAGE_INFO(requester, stage_waiting_ddl);
    else
      THD_STAGE_INFO(requester, stage_waiting_isolation);
  }
#endif /* WITH_WSREP */

  lock= ticket->m_lock;

#ifdef HAVE_PSI_INTERFACE
  PSI_metadata_locker_state state __attribute__((unused));
  PSI_metadata_locker *locker= NULL;

  if (ticket->m_psi != NULL)
    locker= PSI_CALL_start_metadata_wait(&state, ticket->m_psi, __FILE__, __LINE__);
#endif

  DBUG_PRINT("mdl", ("Waiting:  %s", ticket_msg));
  will_wait_for(ticket);

  /* There is a shared or exclusive lock on the object. */
  DEBUG_SYNC(get_thd(), "mdl_acquire_lock_wait");

  find_deadlock();

  struct timespec abs_timeout, abs_shortwait, abs_abort_blocking_timeout;
  bool abort_blocking_enabled= false;
  double abort_blocking_timeout= slave_abort_blocking_timeout;
  if (abort_blocking_timeout < lock_wait_timeout &&
      m_owner->get_thd()->rgi_slave)
  {
    /*
      After @@slave_abort_blocking_timeout seconds, kill non-replication
      queries that are blocking a replication event (such as an ALTER TABLE)
      from proceeding.
    */
    set_timespec_nsec(abs_abort_blocking_timeout,
                      (ulonglong)(abort_blocking_timeout * 1000000000ULL));
    abort_blocking_enabled= true;
  }
  set_timespec_nsec(abs_timeout,
                    (ulonglong)(lock_wait_timeout * 1000000000ULL));
  wait_status= MDL_wait::EMPTY;

  for (;;)
  {
    bool abort_blocking= false;
    set_timespec(abs_shortwait, 1);
    if (abort_blocking_enabled &&
        cmp_timespec(abs_shortwait, abs_abort_blocking_timeout) >= 0)
    {
      /*
        If a slave DDL has waited for --slave-abort-select-timeout, then notify
        any blocking SELECT once before continuing to wait until the full
        timeout.
      */
      abs_shortwait= abs_abort_blocking_timeout;
      abort_blocking= true;
      abort_blocking_enabled= false;
    }
    else if (cmp_timespec(abs_shortwait, abs_timeout) > 0)
      break;

    /* abs_timeout is far away. Wait a short while and notify locks. */
    wait_status= m_wait.timed_wait(m_owner, &abs_shortwait, FALSE,
                                   mdl_request->key.get_wait_state_name());

    if (wait_status != MDL_wait::EMPTY)
      break;
    /* Check if the client is gone while we were waiting. */
    if (! thd_is_connected(m_owner->get_thd()))
    {
      /*
       * The client is disconnected. Don't wait forever:
       * assume it's the same as a wait timeout, this
       * ensures all error handling is correct.
       */
      wait_status= MDL_wait::TIMEOUT;
      break;
    }

    lock->notify_conflicting_locks_if_needed(ticket, abort_blocking);
  }
  if (wait_status == MDL_wait::EMPTY)
    wait_status= m_wait.timed_wait(m_owner, &abs_timeout, TRUE,
                                   mdl_request->key.get_wait_state_name());

  done_waiting_for();

#ifdef HAVE_PSI_INTERFACE
  if (locker != NULL)
    PSI_CALL_end_metadata_wait(locker, 0);
#endif

  if (wait_status != MDL_wait::GRANTED)
  {
    lock->abort_wait(m_pins, ticket);
    m_wait.reset_status();
    delete ticket;
    switch (wait_status)
    {
    case MDL_wait::VICTIM:
      DBUG_PRINT("mdl", ("Deadlock: %s", ticket_msg));
      DBUG_PRINT("mdl_locks", ("Existing locks:%s", mdl_dbug_print_locks()));
      my_error(ER_LOCK_DEADLOCK, MYF(0));
      break;
    case MDL_wait::TIMEOUT:
      DBUG_PRINT("mdl", ("Timeout:  %s", ticket_msg));
      my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
      break;
    case MDL_wait::KILLED:
      DBUG_PRINT("mdl", ("Killed:  %s", ticket_msg));
      get_thd()->send_kill_message();
      break;
    default:
      DBUG_ASSERT(0);
      break;
    }
    DBUG_RETURN(TRUE);
  }

  /*
    We have been granted our request.
    State of MDL_lock object is already being appropriately updated by a
    concurrent thread (@sa MDL_lock:reschedule_waiters()).
    So all we need to do is to update MDL_context and MDL_request objects.
  */
  m_wait.reset_status();

  m_tickets[mdl_request->duration].push_front(ticket);

  mdl_request->ticket= ticket;

  mysql_mdl_set_status(ticket->m_psi, MDL_ticket::GRANTED);

  DBUG_PRINT("mdl", ("Acquired: %s", ticket_msg));
  DBUG_RETURN(FALSE);
}


extern "C" int mdl_request_ptr_cmp(const void* ptr1, const void* ptr2)
{
  MDL_request *req1= *(MDL_request**)ptr1;
  MDL_request *req2= *(MDL_request**)ptr2;
  return req1->key.cmp(&req2->key);
}


/**
  Acquire exclusive locks. There must be no granted locks in the
  context.

  This is a replacement of lock_table_names(). It is used in
  RENAME, DROP and other DDL SQL statements.

  @param  mdl_requests  List of requests for locks to be acquired.

  @param lock_wait_timeout  Seconds to wait before timeout.

  @note The list of requests should not contain non-exclusive lock requests.
        There should not be any acquired locks in the context.

  @note Assumes that one already owns scoped intention exclusive lock.

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool MDL_context::acquire_locks(MDL_request_list *mdl_requests,
                                double lock_wait_timeout)
{
  MDL_request_list::Iterator it(*mdl_requests);
  MDL_request **sort_buf, **p_req;
  MDL_savepoint mdl_svp= mdl_savepoint();
  ssize_t req_count= static_cast<ssize_t>(mdl_requests->elements());
  DBUG_ENTER("MDL_context::acquire_locks");

  if (req_count == 0)
    DBUG_RETURN(FALSE);

  /* Sort requests according to MDL_key. */
  if (! (sort_buf= (MDL_request **)my_malloc(key_memory_MDL_context_acquire_locks,
                                             req_count * sizeof(MDL_request*),
                                             MYF(MY_WME))))
    DBUG_RETURN(TRUE);

  for (p_req= sort_buf; p_req < sort_buf + req_count; p_req++)
    *p_req= it++;

  my_qsort(sort_buf, req_count, sizeof(MDL_request*),
           mdl_request_ptr_cmp);

  for (p_req= sort_buf; p_req < sort_buf + req_count; p_req++)
  {
    if (acquire_lock(*p_req, lock_wait_timeout))
      goto err;
  }
  my_free(sort_buf);
  DBUG_RETURN(FALSE);

err:
  /*
    Release locks we have managed to acquire so far.
    Use rollback_to_savepoint() since there may be duplicate
    requests that got assigned the same ticket.
  */
  rollback_to_savepoint(mdl_svp);
  /* Reset lock requests back to its initial state. */
  for (req_count= p_req - sort_buf, p_req= sort_buf;
       p_req < sort_buf + req_count; p_req++)
  {
    (*p_req)->ticket= NULL;
  }
  my_free(sort_buf);
  DBUG_RETURN(TRUE);
}


/**
  Upgrade a shared metadata lock.

  Used in ALTER TABLE.

  @param mdl_ticket         Lock to upgrade.
  @param new_type           Lock type to upgrade to.
  @param lock_wait_timeout  Seconds to wait before timeout.

  @note In case of failure to upgrade lock (e.g. because upgrader
        was killed) leaves lock in its original state (locked in
        shared mode).

  @note There can be only one upgrader for a lock or we will have deadlock.
        This invariant is ensured by the fact that upgradeable locks SU, SNW
        and SNRW are not compatible with each other and themselves.

  If mdl_ticket was granted via fast lanes it can only be upgraded to fast
  lane lock type. Try fast upgrade in this case, no need to acquire new
  ticket and do conventional upgrade as there're no waiters possible.
  If fast upgrade fails, do conventional upgrade.

  @retval FALSE  Success
  @retval TRUE   Failure (thread was killed)
*/

bool
MDL_context::upgrade_shared_lock(MDL_ticket *mdl_ticket,
                                 enum_mdl_type new_type,
                                 double lock_wait_timeout)
{
  MDL_request mdl_xlock_request;
  MDL_savepoint mdl_svp= mdl_savepoint();
  bool is_new_ticket;
  DBUG_ENTER("MDL_context::upgrade_shared_lock");
  DBUG_PRINT("enter",("old_type: %s  new_type: %s  lock_wait_timeout: %f",
                      mdl_ticket->get_type_name()->str,
                      mdl_ticket->get_type_name(new_type)->str,
                      lock_wait_timeout));
  DEBUG_SYNC(get_thd(), "mdl_upgrade_lock");

  /*
    Do nothing if already upgraded. Used when we FLUSH TABLE under
    LOCK TABLES and a table is listed twice in LOCK TABLES list.

    In BACKUP namespace upgrade must always happen. Even though
    MDL_BACKUP_START is not stronger than MDL_BACKUP_FLUSH from
    has_stronger_or_equal_type(), the latter effectively blocks
    new MDL_BACKUP_DML while the former doesn't.
  */
  if (mdl_ticket->has_stronger_or_equal_type(new_type) &&
      mdl_ticket->get_key()->mdl_namespace() != MDL_key::BACKUP)
    DBUG_RETURN(FALSE);

  if (mdl_ticket->get_lock()->try_fast_upgrade(mdl_ticket, new_type))
    DBUG_RETURN(false);

  MDL_REQUEST_INIT_BY_KEY(&mdl_xlock_request, mdl_ticket->get_key(),
                          new_type, MDL_TRANSACTION);

  if (acquire_lock(&mdl_xlock_request, lock_wait_timeout))
    DBUG_RETURN(TRUE);

  is_new_ticket= ! has_lock(mdl_svp, mdl_xlock_request.ticket);

  /* Merge the acquired and the original lock. */
  mdl_ticket->m_lock->upgrade(mdl_ticket, new_type,
                    is_new_ticket ? mdl_xlock_request.ticket : nullptr);

  if (is_new_ticket)
  {
    m_tickets[MDL_TRANSACTION].remove(mdl_xlock_request.ticket);
    delete mdl_xlock_request.ticket;
  }

  DBUG_RETURN(FALSE);
}


/**
  A fragment of recursive traversal of the wait-for graph
  in search for deadlocks. Direct the deadlock visitor to all
  contexts that own the lock the current node in the wait-for
  graph is waiting for.
  As long as the initial node is remembered in the visitor,
  a deadlock is found when the same node is seen twice.
*/

bool MDL_lock::visit_subgraph(MDL_ticket *waiting_ticket,
                              MDL_wait_for_graph_visitor *gvisitor)
{
  MDL_context *src_ctx= waiting_ticket->get_ctx();
  bool result= TRUE;

  mysql_prlock_rdlock(&m_rwlock);

  /*
    MDL_lock's waiting and granted queues and MDL_context::m_waiting_for
    member are updated by different threads when the lock is granted
    (see MDL_context::acquire_lock() and MDL_lock::reschedule_waiters()).
    As a result, here we may encounter a situation when MDL_lock data
    already reflects the fact that the lock was granted but
    m_waiting_for member has not been updated yet.

    For example, imagine that:

    thread1: Owns SNW lock on table t1.
    thread2: Attempts to acquire SW lock on t1,
             but sees an active SNW lock.
             Thus adds the ticket to the waiting queue and
             sets m_waiting_for to point to the ticket.
    thread1: Releases SNW lock, updates MDL_lock object to
             grant SW lock to thread2 (moves the ticket for
             SW from waiting to the active queue).
             Attempts to acquire a new SNW lock on t1,
             sees an active SW lock (since it is present in the
             active queue), adds ticket for SNW lock to the waiting
             queue, sets m_waiting_for to point to this ticket.

    At this point deadlock detection algorithm run by thread1 will see that:
    - Thread1 waits for SNW lock on t1 (since m_waiting_for is set).
    - SNW lock is not granted, because it conflicts with active SW lock
      owned by thread 2 (since ticket for SW is present in granted queue).
    - Thread2 waits for SW lock (since its m_waiting_for has not been
      updated yet!).
    - SW lock is not granted because there is pending SNW lock from thread1.
      Therefore deadlock should exist [sic!].

    To avoid detection of such false deadlocks we need to check the "actual"
    status of the ticket being waited for, before analyzing its blockers.
    We do this by checking the wait status of the context which is waiting
    for it. To avoid races this has to be done under protection of
    MDL_lock::m_rwlock lock.
  */
  if (src_ctx->m_wait.get_status() != MDL_wait::EMPTY)
  {
    result= FALSE;
    goto end;
  }

  /*
    To avoid visiting nodes which were already marked as victims of
    deadlock detection (or whose requests were already satisfied) we
    enter the node only after peeking at its wait status.
    This is necessary to avoid active waiting in a situation
    when previous searches for a deadlock already selected the
    node we're about to enter as a victim (see the comment
    in MDL_context::find_deadlock() for explanation why several searches
    can be performed for the same wait).
    There is no guarantee that the node isn't chosen a victim while we
    are visiting it but this is OK: in the worst case we might do some
    extra work and one more context might be chosen as a victim.
  */
  if (gvisitor->enter_node(src_ctx))
    goto end;

  /*
    We do a breadth-first search first -- that is, inspect all
    edges of the current node, and only then follow up to the next
    node. In workloads that involve wait-for graph loops this
    has proven to be a more efficient strategy [citation missing].
  */
  for (const auto& ticket : m_granted)
  {
    /* Filter out edges that point to the same node. */
    if (ticket.get_ctx() != src_ctx &&
        ticket.is_incompatible_when_granted(waiting_ticket->get_type()) &&
        gvisitor->inspect_edge(ticket.get_ctx()))
    {
      goto end_leave_node;
    }
  }

  for (const auto &ticket : m_waiting)
  {
    /* Filter out edges that point to the same node. */
    if (ticket.get_ctx() != src_ctx &&
        ticket.is_incompatible_when_waiting(waiting_ticket->get_type()) &&
        gvisitor->inspect_edge(ticket.get_ctx()))
    {
      goto end_leave_node;
    }
  }

  /* Recurse and inspect all adjacent nodes. */
  for (const auto &ticket : m_granted)
  {
    if (ticket.get_ctx() != src_ctx &&
        ticket.is_incompatible_when_granted(waiting_ticket->get_type()) &&
        ticket.get_ctx()->visit_subgraph(gvisitor))
    {
      goto end_leave_node;
    }
  }

  for (const auto &ticket : m_waiting)
  {
    if (ticket.get_ctx() != src_ctx &&
        ticket.is_incompatible_when_waiting(waiting_ticket->get_type()) &&
        ticket.get_ctx()->visit_subgraph(gvisitor))
    {
      goto end_leave_node;
    }
  }

  result= FALSE;

end_leave_node:
  gvisitor->leave_node(src_ctx);

end:
  mysql_prlock_unlock(&m_rwlock);
  return result;
}


/**
  Traverse a portion of wait-for graph which is reachable
  through the edge represented by this ticket and search
  for deadlocks.

  @retval TRUE  A deadlock is found. A pointer to deadlock
                 victim is saved in the visitor.
  @retval FALSE
*/

bool MDL_ticket::accept_visitor(MDL_wait_for_graph_visitor *gvisitor)
{
  return m_lock->visit_subgraph(this, gvisitor);
}


/**
  A fragment of recursive traversal of the wait-for graph of
  MDL contexts in the server in search for deadlocks.
  Assume this MDL context is a node in the wait-for graph,
  and direct the visitor to all adjacent nodes. As long
  as the starting node is remembered in the visitor, a
  deadlock is found when the same node is visited twice.
  One MDL context is connected to another in the wait-for
  graph if it waits on a resource that is held by the other
  context.

  @retval TRUE  A deadlock is found. A pointer to deadlock
                victim is saved in the visitor.
  @retval FALSE
*/

bool MDL_context::visit_subgraph(MDL_wait_for_graph_visitor *gvisitor)
{
  bool result= FALSE;

  mysql_prlock_rdlock(&m_LOCK_waiting_for);

  if (m_waiting_for)
    result= m_waiting_for->accept_visitor(gvisitor);

  mysql_prlock_unlock(&m_LOCK_waiting_for);

  return result;
}


/**
  Try to find a deadlock. This function produces no errors.

  @note If during deadlock resolution context which performs deadlock
        detection is chosen as a victim it will be informed about the
        fact by setting VICTIM status to its wait slot.
*/

void MDL_context::find_deadlock()
{
  while (1)
  {
    /*
      The fact that we use fresh instance of gvisitor for each
      search performed by find_deadlock() below is important,
      the code responsible for victim selection relies on this.
    */
    Deadlock_detection_visitor dvisitor(this);
    MDL_context *victim;

    if (! visit_subgraph(&dvisitor))
    {
      /* No deadlocks are found! */
      break;
    }

    victim= dvisitor.get_victim();

    /*
      Failure to change status of the victim is OK as it means
      that the victim has received some other message and is
      about to stop its waiting/to break deadlock loop.
      Even when the initiator of the deadlock search is
      chosen the victim, we need to set the respective wait
      result in order to "close" it for any attempt to
      schedule the request.
      This is needed to avoid a possible race during
      cleanup in case when the lock request on which the
      context was waiting is concurrently satisfied.
    */
    (void) victim->m_wait.set_status(MDL_wait::VICTIM);
    victim->inc_deadlock_overweight();
    victim->unlock_deadlock_victim();

    if (victim == this)
      break;
    /*
      After adding a new edge to the waiting graph we found that it
      creates a loop (i.e. there is a deadlock). We decided to destroy
      this loop by removing an edge, but not the one that we added.
      Since this doesn't guarantee that all loops created by addition
      of the new edge are destroyed, we have to repeat the search.
    */
  }
}


/**
  Release lock.

  @param duration Lock duration.
  @param ticket   Ticket for lock to be released.

*/

void MDL_context::release_lock(enum_mdl_duration duration, MDL_ticket *ticket)
{
  MDL_lock *lock= ticket->m_lock;
  DBUG_ENTER("MDL_context::release_lock");
  DBUG_PRINT("enter", ("db: '%s' name: '%s'",
                       ticket->get_key()->db_name(),
                       ticket->get_key()->name()));

  DBUG_ASSERT(this == ticket->get_ctx());
  DBUG_PRINT("mdl", ("Released: %s", dbug_print_mdl(ticket)));

  lock->release(m_pins, ticket);

  m_tickets[duration].remove(ticket);
  delete ticket;

  DBUG_VOID_RETURN;
}


/**
  Release lock with explicit duration.

  @param ticket   Ticket for lock to be released.

*/

void MDL_context::release_lock(MDL_ticket *ticket)
{
  DBUG_SLOW_ASSERT(ticket->m_duration == MDL_EXPLICIT);

  release_lock(MDL_EXPLICIT, ticket);
}


/**
  Release all locks associated with the context. If the sentinel
  is not NULL, do not release locks stored in the list after and
  including the sentinel.

  Statement and transactional locks are added to the beginning of
  the corresponding lists, i.e. stored in reverse temporal order.
  This allows to employ this function to:
  - back off in case of a lock conflict.
  - release all locks in the end of a statement or transaction
  - rollback to a savepoint.
*/

void MDL_context::release_locks_stored_before(enum_mdl_duration duration,
                                              MDL_ticket *sentinel)
{
  MDL_ticket *ticket;
  Ticket_iterator it(m_tickets[duration]);
  DBUG_ENTER("MDL_context::release_locks_stored_before");

  if (m_tickets[duration].is_empty())
    DBUG_VOID_RETURN;

  while ((ticket= it++) && ticket != sentinel)
  {
    DBUG_PRINT("info", ("found lock to release ticket=%p", ticket));
    release_lock(duration, ticket);
  }

  DBUG_VOID_RETURN;
}


/**
  Release all explicit locks in the context which correspond to the
  same name/object as this lock request.

  @param ticket    One of the locks for the name/object for which all
                   locks should be released.
*/

void MDL_context::release_all_locks_for_name(MDL_ticket *name)
{
  /* Use MDL_ticket::m_lock to identify other locks for the same object. */
  MDL_lock *lock= name->m_lock;

  /* Remove matching lock tickets from the context. */
  MDL_ticket *ticket;
  Ticket_iterator it_ticket(m_tickets[MDL_EXPLICIT]);

  while ((ticket= it_ticket++))
  {
    DBUG_ASSERT(ticket->m_lock);
    if (ticket->m_lock == lock)
      release_lock(MDL_EXPLICIT, ticket);
  }
}


/**
  Downgrade an EXCLUSIVE or SHARED_NO_WRITE lock to shared metadata lock.

  @param type  Type of lock to which exclusive lock should be downgraded.
*/

void MDL_ticket::downgrade_lock(enum_mdl_type type)
{
  DBUG_ENTER("MDL_ticket::downgrade_lock");
  DBUG_PRINT("enter",("old_type: %s  new_type: %s",
                      get_type_name()->str,
                      get_type_name(type)->str));
  /*
    Do nothing if already downgraded. Used when we FLUSH TABLE under
    LOCK TABLES and a table is listed twice in LOCK TABLES list.
    Note that this code might even try to "downgrade" a weak lock
    (e.g. SW) to a stronger one (e.g SNRW). So we can't even assert
    here that target lock is weaker than existing lock.
  */
  if (m_type == type || !has_stronger_or_equal_type(type))
  {
    DBUG_PRINT("info", ("Nothing to downgrade"));
    DBUG_VOID_RETURN;
  }

  /* Only allow downgrade in some specific known cases */
  DBUG_ASSERT((get_key()->mdl_namespace() != MDL_key::BACKUP &&
               (m_type == MDL_EXCLUSIVE ||
                m_type == MDL_SHARED_NO_WRITE)) ||
              (get_key()->mdl_namespace() == MDL_key::BACKUP &&
               (m_type == MDL_BACKUP_DDL ||
                m_type == MDL_BACKUP_BLOCK_DDL ||
                m_type == MDL_BACKUP_WAIT_FLUSH)));

  m_lock->downgrade(this, type);
  DBUG_VOID_RETURN;
}


/**
  Auxiliary function which allows to check if we have some kind of lock on
  a object. Returns TRUE if we have a lock of a given or stronger type.

  @param mdl_namespace Id of object namespace
  @param db            Name of the database
  @param name          Name of the object
  @param mdl_type      Lock type. Pass in the weakest type to find
                       out if there is at least some lock.

  @return TRUE if current context contains satisfied lock for the object,
          FALSE otherwise.
*/

bool
MDL_context::is_lock_owner(MDL_key::enum_mdl_namespace mdl_namespace,
                           const char *db, const char *name,
                           enum_mdl_type mdl_type)
{
  MDL_request mdl_request;
  enum_mdl_duration not_unused;
  /* We don't care about exact duration of lock here. */
  MDL_REQUEST_INIT(&mdl_request, mdl_namespace, db, name, mdl_type,
                   MDL_TRANSACTION);
  MDL_ticket *ticket= find_ticket(&mdl_request, &not_unused);

  DBUG_ASSERT(ticket == NULL || ticket->m_lock);

  return ticket;
}


/**
  Return thread id of the owner of the lock or 0 if
  there is no owner.
  @note: Lock type is not considered at all, the function
  simply checks that there is some lock for the given key.

  @return  thread id of the owner of the lock or 0
*/

unsigned long
MDL_context::get_lock_owner(MDL_key *key)
{
  fix_pins();
  return mdl_locks.get_lock_owner(m_pins, key);
}


/**
  Check if we have any pending locks which conflict with existing shared lock.

  @pre The ticket must match an acquired lock.

  @return TRUE if there is a conflicting lock request, FALSE otherwise.
*/

bool MDL_ticket::has_pending_conflicting_lock() const
{
  return m_lock->has_pending_conflicting_lock(m_type);
}

/** Return a key identifying this lock. */
const MDL_key *MDL_ticket::get_key() const
{
  return m_lock->get_key();
}

/**
  Releases metadata locks that were acquired after a specific savepoint.

  @note Used to release tickets acquired during a savepoint unit.
  @note It's safe to iterate and unlock any locks after taken after this
        savepoint because other statements that take other special locks
        cause a implicit commit (ie LOCK TABLES).
*/

void MDL_context::rollback_to_savepoint(const MDL_savepoint &mdl_savepoint)
{
  DBUG_ENTER("MDL_context::rollback_to_savepoint");

  /* If savepoint is NULL, it is from the start of the transaction. */
  release_locks_stored_before(MDL_STATEMENT, mdl_savepoint.m_stmt_ticket);
  release_locks_stored_before(MDL_TRANSACTION, mdl_savepoint.m_trans_ticket);

  DBUG_VOID_RETURN;
}


/**
  Release locks acquired by normal statements (SELECT, UPDATE,
  DELETE, etc) in the course of a transaction. Do not release
  HANDLER locks, if there are any.

  This method is used at the end of a transaction, in
  implementation of COMMIT (implicit or explicit) and ROLLBACK.
*/

void MDL_context::release_transactional_locks(THD *thd)
{
  DBUG_ENTER("MDL_context::release_transactional_locks");
  /* Fail if there are active transactions */
  DBUG_ASSERT(!(thd->server_status &
                (SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY)));
  release_locks_stored_before(MDL_STATEMENT, NULL);
  release_locks_stored_before(MDL_TRANSACTION, NULL);
  DBUG_VOID_RETURN;
}

void MDL_context::release_statement_locks()
{
  DBUG_ENTER("MDL_context::release_transactional_locks");
  release_locks_stored_before(MDL_STATEMENT, NULL);
  DBUG_VOID_RETURN;
}


/**
  Does this savepoint have this lock?

  @retval TRUE  The ticket is older than the savepoint or
                is an LT, HA or GLR ticket. Thus it belongs
                to the savepoint or has explicit duration.
  @retval FALSE The ticket is newer than the savepoint.
                and is not an LT, HA or GLR ticket.
*/

bool MDL_context::has_lock(const MDL_savepoint &mdl_savepoint,
                           MDL_ticket *mdl_ticket)
{
  MDL_ticket *ticket;
  /* Start from the beginning, most likely mdl_ticket's been just acquired. */
  MDL_context::Ticket_iterator s_it(m_tickets[MDL_STATEMENT]);
  MDL_context::Ticket_iterator t_it(m_tickets[MDL_TRANSACTION]);

  while ((ticket= s_it++) && ticket != mdl_savepoint.m_stmt_ticket)
  {
    if (ticket == mdl_ticket)
      return FALSE;
  }

  while ((ticket= t_it++) && ticket != mdl_savepoint.m_trans_ticket)
  {
    if (ticket == mdl_ticket)
      return FALSE;
  }
  return TRUE;
}


/**
  Change lock duration for transactional lock.

  @param ticket   Ticket representing lock.
  @param duration Lock duration to be set.

  @note This method only supports changing duration of
        transactional lock to some other duration.
*/

void MDL_context::set_lock_duration(MDL_ticket *mdl_ticket,
                                    enum_mdl_duration duration)
{
  DBUG_SLOW_ASSERT(mdl_ticket->m_duration == MDL_TRANSACTION &&
                   duration != MDL_TRANSACTION);

  m_tickets[MDL_TRANSACTION].remove(mdl_ticket);
  m_tickets[duration].push_front(mdl_ticket);
#ifndef DBUG_OFF
  mdl_ticket->m_duration= duration;
#endif
}


/**
  Set explicit duration for all locks in the context.
*/

void MDL_context::set_explicit_duration_for_all_locks()
{
  int i;
  MDL_ticket *ticket;

  /*
    In the most common case when this function is called list
    of transactional locks is bigger than list of locks with
    explicit duration. So we start by swapping these two lists
    and then move elements from new list of transactional
    locks and list of statement locks to list of locks with
    explicit duration.
  */

  m_tickets[MDL_EXPLICIT].swap(m_tickets[MDL_TRANSACTION]);

  for (i= 0; i < MDL_EXPLICIT; i++)
  {
    Ticket_iterator it_ticket(m_tickets[i]);

    while ((ticket= it_ticket++))
    {
      m_tickets[i].remove(ticket);
      m_tickets[MDL_EXPLICIT].push_front(ticket);
    }
  }

#ifndef DBUG_OFF
  Ticket_iterator exp_it(m_tickets[MDL_EXPLICIT]);

  while ((ticket= exp_it++))
    ticket->m_duration= MDL_EXPLICIT;
#endif
}


/**
  Set transactional duration for all locks in the context.
*/

void MDL_context::set_transaction_duration_for_all_locks()
{
  MDL_ticket *ticket;

  /*
    In the most common case when this function is called list
    of explicit locks is bigger than two other lists (in fact,
    list of statement locks is always empty). So we start by
    swapping list of explicit and transactional locks and then
    move contents of new list of explicit locks to list of
    locks with transactional duration.
  */

  DBUG_ASSERT(m_tickets[MDL_STATEMENT].is_empty());

  m_tickets[MDL_TRANSACTION].swap(m_tickets[MDL_EXPLICIT]);

  Ticket_iterator it_ticket(m_tickets[MDL_EXPLICIT]);

  while ((ticket= it_ticket++))
  {
    m_tickets[MDL_EXPLICIT].remove(ticket);
    m_tickets[MDL_TRANSACTION].push_front(ticket);
  }

#ifndef DBUG_OFF
  Ticket_iterator trans_it(m_tickets[MDL_TRANSACTION]);

  while ((ticket= trans_it++))
    ticket->m_duration= MDL_TRANSACTION;
#endif
}



void MDL_context::release_explicit_locks()
{
  release_locks_stored_before(MDL_EXPLICIT, NULL);
}

bool MDL_context::has_explicit_locks()
{
  MDL_ticket *ticket = NULL;

  Ticket_iterator it(m_tickets[MDL_EXPLICIT]);

  while ((ticket = it++))
  {
    return true;
  }

  return false;
}

#ifdef WITH_WSREP
static
const char *wsrep_get_mdl_namespace_name(MDL_key::enum_mdl_namespace ns)
{
  switch (ns)
  {
  case MDL_key::BACKUP    : return "BACKUP";
  case MDL_key::SCHEMA    : return "SCHEMA";
  case MDL_key::TABLE     : return "TABLE";
  case MDL_key::FUNCTION  : return "FUNCTION";
  case MDL_key::PROCEDURE : return "PROCEDURE";
  case MDL_key::PACKAGE_BODY: return "PACKAGE BODY";
  case MDL_key::TRIGGER   : return "TRIGGER";
  case MDL_key::EVENT     : return "EVENT";
  case MDL_key::USER_LOCK : return "USER_LOCK";
  default: break;
  }
  return "UNKNOWN";
}

void MDL_ticket::wsrep_report(bool debug) const
{
  if (!debug) return;

  const PSI_stage_info *psi_stage= get_key()->get_wait_state_name();
  WSREP_DEBUG("MDL ticket: type: %s space: %s db: %s name: %s (%s)",
              get_type_name()->str,
              wsrep_get_mdl_namespace_name(get_key()->mdl_namespace()),
              get_key()->db_name(),
              get_key()->name(),
              psi_stage->m_name);
}
#endif /* WITH_WSREP */
