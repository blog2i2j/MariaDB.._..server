/*
   Copyright (c) 2005, 2010, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_cursor.h"
#include "probes_mysql.h"
#include "sql_parse.h"                        // mysql_execute_command

/****************************************************************************
  Declarations.
****************************************************************************/

/**
  Materialized_cursor -- an insensitive materialized server-side
  cursor. The result set of this cursor is saved in a temporary
  table at open. The cursor itself is simply an interface for the
  handler of the temporary table.
*/

class Materialized_cursor: public Server_side_cursor
{
  MEM_ROOT main_mem_root;
  /* A fake unit to supply to select_send when fetching */
  SELECT_LEX_UNIT fake_unit;
  TABLE *table;
  List<Item> item_list;
  ulong fetch_limit;
  ulong fetch_count;
  bool is_rnd_inited;
public:
  Materialized_cursor(select_result *result, TABLE *table);

  int send_result_set_metadata(THD *thd, List<Item> &send_result_set_metadata);
  bool is_open() const override { return table != 0; }
  int open(JOIN *join __attribute__((unused))) override;
  void fetch(ulong num_rows) override;
  void close() override;
  bool export_structure(THD *thd, Row_definition_list *defs) override
  {
    return table->export_structure(thd, defs);
  }
  ~Materialized_cursor() override;

  void on_table_fill_finished();
};


/**
  Select_materialize -- a mediator between a cursor query and the
  protocol. In case we were not able to open a non-materialzed
  cursor, it creates an internal temporary HEAP table, and insert
  all rows into it. When the table reaches max_heap_table_size,
  it's converted to a MyISAM table. Later this table is used to
  create a Materialized_cursor.
*/

class Select_materialize: public select_unit
{
  select_result *result; /**< the result object of the caller (PS or SP) */
public:
  Materialized_cursor *materialized_cursor;
  Select_materialize(THD *thd_arg, select_result *result_arg):
    select_unit(thd_arg), result(result_arg), materialized_cursor(0) {}
  bool send_result_set_metadata(List<Item> &list, uint flags) override;
  bool send_eof() override { return false; }
  bool view_structure_only() const override
  {
    return result->view_structure_only();
  }
};


/**************************************************************************/

/**
  Attempt to open a materialized cursor.

  @param      thd           thread handle
  @param[in]  result        result class of the caller used as a destination
                            for the rows fetched from the cursor
  @param[out] pcursor       a pointer to store a pointer to cursor in

  @retval
    0                 the query has been successfully executed; in this
    case pcursor may or may not contain
    a pointer to an open cursor.
  @retval
    non-zero          an error, 'pcursor' has been left intact.
*/

int mysql_open_cursor(THD *thd, select_result *result,
                      Server_side_cursor **pcursor)
{
  sql_digest_state *parent_digest;
  PSI_statement_locker *parent_locker;
  select_result *save_result;
  Select_materialize *result_materialize;
  LEX *lex= thd->lex;
  int rc;

  if (!(result_materialize= new (thd->mem_root) Select_materialize(thd, result)))
    return 1;

  save_result= lex->result;

  lex->result= result_materialize;

  MYSQL_QUERY_EXEC_START(thd->query(),
                         thd->thread_id,
                         thd->get_db(),
                         &thd->security_ctx->priv_user[0],
                         (char *) thd->security_ctx->host_or_ip,
                         2);
  parent_digest= thd->m_digest;
  parent_locker= thd->m_statement_psi;
  thd->m_digest= NULL;
  thd->m_statement_psi= NULL;
  /* Mark that we can't use query cache with cursors */
  thd->query_cache_is_applicable= 0;
  rc= mysql_execute_command(thd);
  thd->lex->restore_set_statement_var();
  thd->m_digest= parent_digest;
  thd->m_statement_psi= parent_locker;
  MYSQL_QUERY_EXEC_DONE(rc);

  lex->result= save_result;
  /*
    Possible options here:
    - a materialized cursor is open. In this case rc is 0 and
      result_materialize->materialized is not NULL
    - an error occurred during materialization.
      result_materialize->materialized_cursor is not NULL, but rc != 0
    - successful completion of mysql_execute_command without
      a cursor: rc is 0, result_materialize->materialized_cursor is NULL.
      This is possible if some command writes directly to the
      network, bypassing select_result mechanism. An example of
      such command is SHOW VARIABLES or SHOW STATUS.
  */
  if (rc)
  {
    if (result_materialize->materialized_cursor)
    {
      /* Rollback metadata in the client-server protocol. */
      result_materialize->abort_result_set();

      delete result_materialize->materialized_cursor;
    }

    goto end;
  }

  if (result_materialize->materialized_cursor)
  {
    Materialized_cursor *materialized_cursor=
      result_materialize->materialized_cursor;

    /*
      NOTE: close_thread_tables() has been called in
      mysql_execute_command(), so all tables except from the cursor
      temporary table have been closed.
    */

    if ((rc= materialized_cursor->open(0)))
    {
      delete materialized_cursor;
      goto end;
    }

    *pcursor= materialized_cursor;
    rc|= (thd->stmt_arena->cleanup_stmt(true)? 1 : 0);
  }

end:
  delete result_materialize;
  return rc;
}

/****************************************************************************
  Server_side_cursor
****************************************************************************/

Server_side_cursor::~Server_side_cursor() = default;


void Server_side_cursor::operator delete(void *ptr, size_t size)
{
  Server_side_cursor *cursor= (Server_side_cursor*) ptr;
  MEM_ROOT own_root= *cursor->mem_root;

  DBUG_ENTER("Server_side_cursor::operator delete");
  TRASH_FREE(ptr, size);
  /*
    If this cursor has never been opened mem_root is empty. Otherwise
    mem_root points to the memory the cursor object was allocated in.
    In this case it's important to call free_root last, and free a copy
    instead of *mem_root to avoid writing into freed memory.
  */
  free_root(&own_root, MYF(0));
  DBUG_VOID_RETURN;
}


/***************************************************************************
 Materialized_cursor
****************************************************************************/

Materialized_cursor::Materialized_cursor(select_result *result_arg,
                                         TABLE *table_arg)
  :Server_side_cursor(&table_arg->mem_root, result_arg),
  table(table_arg),
  fetch_limit(0),
  fetch_count(0),
  is_rnd_inited(0)
{
  fake_unit.init_query();
  fake_unit.thd= table->in_use;
}


/**
  Preserve the original metadata to be sent to the client.
  Initiate sending of the original metadata to the client
  (call Protocol::send_result_set_metadata()).

  @param thd Thread identifier.
  @param send_result_set_metadata List of fields that would be sent.
*/

int Materialized_cursor::send_result_set_metadata(
  THD *thd, List<Item> &send_result_set_metadata)
{
  Query_arena backup_arena;
  int rc;
  List_iterator_fast<Item> it_org(send_result_set_metadata);
  List_iterator_fast<Item> it_dst(item_list);
  Item *item_org;
  Item *item_dst;

  thd->set_n_backup_active_arena(this, &backup_arena);

  if ((rc= table->fill_item_list(&item_list)))
    goto end;

  DBUG_ASSERT(send_result_set_metadata.elements == item_list.elements);

  /*
    Unless we preserve the original metadata, it will be lost,
    since new fields describe columns of the temporary table.
    Allocate a copy of the name for safety only. Currently
    items with original names are always kept in memory,
    but in case this changes a memory leak may be hard to notice.
  */
  while ((item_dst= it_dst++, item_org= it_org++))
  {
    Item_ident *ident= static_cast<Item_ident *>(item_dst);
    Send_field send_field(thd, item_org);

    ident->db_name= Lex_ident_db(thd->strmake_lex_cstring(send_field.db_name));
    ident->table_name= Lex_ident_table(thd->strmake_lex_cstring(
                                              send_field.table_name));
  }

  /*
    Original metadata result set should be sent here. After
    mysql_execute_command() is finished, item_list can not be used for
    sending metadata, because it references closed table.
  */
  rc= result->send_result_set_metadata(item_list, Protocol::SEND_NUM_ROWS);

end:
  thd->restore_active_arena(this, &backup_arena);
  /* Check for thd->is_error() in case of OOM */
  return rc || thd->is_error();
}


int Materialized_cursor::open(JOIN *join __attribute__((unused)))
{
  THD *thd= fake_unit.thd;
  int rc;
  Query_arena backup_arena;

  thd->set_n_backup_active_arena(this, &backup_arena);

  /* Create a list of fields and start sequential scan. */

  rc= result->prepare(item_list, &fake_unit);
  rc= !rc && table->file->ha_rnd_init_with_error(TRUE);
  is_rnd_inited= !rc;

  thd->restore_active_arena(this, &backup_arena);

  /* Commit or rollback metadata in the client-server protocol. */

  if (!rc)
  {
    thd->server_status|= SERVER_STATUS_CURSOR_EXISTS;
    result->send_eof();
  }
  else
  {
    result->abort_result_set();
  }

  on_table_fill_finished();

  return rc;
}


/**
  Fetch up to the given number of rows from a materialized cursor.

    Precondition: the cursor is open.

    If the cursor points after the last row, the fetch will automatically
    close the cursor and not send any data (except the 'EOF' packet
    with SERVER_STATUS_LAST_ROW_SENT). This is an extra round trip
    and probably should be improved to return
    SERVER_STATUS_LAST_ROW_SENT along with the last row.
*/

void Materialized_cursor::fetch(ulong num_rows)
{
  THD *thd= table->in_use;

  int res= 0;
  result->begin_dataset();
  for (fetch_limit+= num_rows; fetch_count < fetch_limit; fetch_count++)
  {
    if ((res= table->file->ha_rnd_next(table->record[0])))
      break;
    /* Send data only if the read was successful. */
    /*
      If network write failed (i.e. due to a closed socked),
      the error has already been set. Just return.
    */
    if (result->send_data(item_list) > 0)
      return;
  }

  switch (res) {
  case 0:
    thd->server_status|= SERVER_STATUS_CURSOR_EXISTS;
    result->send_eof();
    break;
  case HA_ERR_END_OF_FILE:
    thd->server_status|= SERVER_STATUS_LAST_ROW_SENT;
    result->send_eof();
    close();
    break;
  default:
    table->file->print_error(res, MYF(0));
    close();
    break;
  }
}


void Materialized_cursor::close()
{
  /* Free item_list items */
  free_items();
  if (is_rnd_inited)
    (void) table->file->ha_rnd_end();
  /*
    We need to grab table->mem_root to prevent free_tmp_table from freeing:
    the cursor object was allocated in this memory.
  */
  main_mem_root= table->mem_root;
  mem_root= &main_mem_root;
  clear_alloc_root(&table->mem_root);
  free_tmp_table(table->in_use, table);
  table= 0;
}


Materialized_cursor::~Materialized_cursor()
{
  if (is_open())
    close();
}


/*
  @brief
    Perform actions that are to be done when cursor materialization has
    finished.

  @detail
    This function is called when "OPEN $cursor" has finished filling the
    temporary table with rows that the cursor will return.

    Temporary table has table->field->orig_table pointing at the tables
    that are used in the cursor definition query. Pointers to these tables
    will not be valid after the query finishes.  So, we do what is done for
    regular tables: have orig_table point at the table that the fields belong
    to.
*/

void Materialized_cursor::on_table_fill_finished()
{
  uint fields= table->s->fields;
  for (uint i= 0; i < fields; i++)
    table->field[i]->orig_table= table->field[i]->table;
}

/***************************************************************************
 Select_materialize
****************************************************************************/

bool Select_materialize::send_result_set_metadata(List<Item> &list, uint flags)
{
  DBUG_ASSERT(table == 0);
  if (create_result_table(unit->thd, unit->get_column_types(true),
                          FALSE,
                          thd->variables.option_bits | TMP_TABLE_ALL_COLUMNS,
                          &empty_clex_str, FALSE, TRUE, TRUE, 0))
    return TRUE;

  materialized_cursor= new (&table->mem_root)
                       Materialized_cursor(result, table);

  if (!materialized_cursor)
  {
    free_tmp_table(table->in_use, table);
    table= 0;
    return TRUE;
  }

  if (materialized_cursor->send_result_set_metadata(unit->thd, list))
  {
    delete materialized_cursor;
    table= 0;
    materialized_cursor= 0;
    return TRUE;
  }

  return FALSE;
}

