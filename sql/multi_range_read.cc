/* Copyright (C) 2010, 2011 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include "mariadb.h"
#include "sql_parse.h"
#include <my_bit.h>
#include "sql_select.h"
#include "key.h"
#include "sql_statistics.h"
#include "rowid_filter.h"
#include "optimizer_defaults.h"
#include "opt_hints.h"

static void get_sweep_read_cost(TABLE *table, ha_rows nrows, bool interrupted,
                                Cost_estimate *cost);



/*
  The following calculation is the same as in multi_range_read_info()

  @param cost              Total cost is stored here
  @param keyno             Key number
  @param n_ranges          Number of different ranges
  @param multi_row_ranges  Number of ranges that are not EQ_REF
  @param flags             Flags. Only HA_MRR_INDEX_ONLY is used.
  @param total_rows        Number of rows expected to be read.
  @param io_blocks         Number of blocks we expect to read for
                           a not clustered index.
  @param unassigned_single_point_ranges
                           Number of blocks we have not yet read for
                           a clustered index.
*/

void handler::calculate_costs(Cost_estimate *cost, uint keyno,
                              uint n_ranges, uint multi_row_ranges,
                              uint flags,
                              ha_rows total_rows,
                              ulonglong io_blocks,
                              ulonglong unassigned_single_point_ranges)
{
  cost->reset(this);

  if (!is_clustering_key(keyno))
  {
    cost->index_cost= ha_keyread_time(keyno, n_ranges,
                                      total_rows + multi_row_ranges,
                                      io_blocks);

    if (!(flags & HA_MRR_INDEX_ONLY))
    {
      /* ha_rnd_pos_time includes ROW_COPY_COST */
      cost->row_cost= ha_rnd_pos_time(total_rows);
      /* Adjust io cost to data size */
      cost->row_cost.io= MY_MIN(cost->row_cost.io, row_blocks());
    }
    else
    {
      /* Index only read */
      cost->copy_cost= rows2double(total_rows) * KEY_COPY_COST;
    }
  }
  else
  {
    /* Clustered index */
    io_blocks= unassigned_single_point_ranges;
    cost->index_cost= ha_keyread_time(keyno, n_ranges,
                                      total_rows + multi_row_ranges,
                                      io_blocks);
    cost->copy_cost=  rows2double(total_rows) * ROW_COPY_COST;
  }
  /* Adjust io cost to data size */
  cost->index_cost.io= MY_MIN(cost->index_cost.io, index_blocks(keyno));

  cost->comp_cost= rows2double(total_rows) * WHERE_COST;
  cost->setup_cost= MULTI_RANGE_READ_SETUP_COST;
}



/****************************************************************************
 * Default MRR implementation (MRR to non-MRR converter)
 ***************************************************************************/

/**
  Get cost and other information about MRR scan over a known list of ranges

  Calculate estimated cost and other information about an MRR scan for given
  sequence of ranges.

  @param keyno           Index number
  @param seq             Range sequence to be traversed
  @param seq_init_param  First parameter for seq->init()
  @param n_ranges_arg    Number of ranges in the sequence, or 0 if the caller
                         can't efficiently determine it
  @param bufsz    INOUT  IN:  Size of the buffer available for use
                         OUT: Size of the buffer that is expected to be
                              actually used, or 0 if buffer is not needed.
  @param flags    INOUT  A combination of HA_MRR_* flags
  @param cost     OUT    Estimated cost of MRR access

  @note
    This method (or an overriding one in a derived class) must check for
    thd->killed and return HA_POS_ERROR if it is not zero. This is required
    for a user to be able to interrupt the calculation by killing the
    connection/query.

  @retval
    HA_POS_ERROR  Error or the engine is unable to perform the requested
                  scan. Values of OUT parameters are undefined.
  @retval
    other         OK, *cost contains cost of the scan, *bufsz and *flags
                  contain scan parameters.
*/


ha_rows
handler::multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                     void *seq_init_param, uint n_ranges_arg,
                                     uint *bufsz, uint *flags,
                                     ha_rows top_limit,
                                     Cost_estimate *cost)
{
  KEY_MULTI_RANGE range;
  range_seq_t seq_it;
  ha_rows total_rows= 0;
  uint n_ranges=0;
  ha_rows max_rows= stats.records;
  THD *thd= table->in_use;
  ulonglong io_blocks;

  /*
     Counter of blocks that contain range edges for those ranges
     for which records_in_range() is called
  */
  ulonglong edge_blocks_cnt= 0;
 /*
   Counter of blocks that contain index tuples for those ranges
   for which records_in_range() is called
  */
  ulonglong range_blocks_cnt= 0;
  /*
    The position of the block containing the last record of the previous range
    for which the info about range position is provided
  */
  ulonglong prev_range_last_block= UNUSED_PAGE_NO;
  /* The counter of records the staring from prev_range_last_block */
  ulonglong prev_range_last_block_records= 0;
  /*
    The counter of single point ranges.
    (For single point ranges we do not call records_in_range())
  */
  ulonglong single_point_ranges= 0;
  /*
    The counter of of single point ranges that we succeded to assign
    to some blocks
  */
  ulonglong assigned_single_point_ranges= 0;
  /*
    Counter of single point ranges for which records_in_range in not
    called and that are encountered between two ranges without such property
    For example, let's have a subsequence of ranges
    R1,r1,....rk,R2
    where r1,...,rk are single point ranges for which records_in_range is
    called while R1 and R2 are not such ranges.
    Then single_point_ranges_delta will count ranges r1,...,rk.
  */
  ulonglong unassigned_single_point_ranges= 0;

  uint len= table->key_info[keyno].key_length + table->file->ref_length;
  if (table->file->is_clustering_key(keyno))
    len= table->s->stored_rec_length;
  /* Assume block is 75 % full */
  uint avg_block_records= ((uint) (stats.block_size*3/4))/len + 1;
  uint limit= thd->variables.eq_range_index_dive_limit;
  bool use_statistics_for_eq_range= eq_ranges_exceeds_limit(seq,
                                                            seq_init_param,
                                                            limit);
  DBUG_ENTER("multi_range_read_info_const");

  /* Default MRR implementation doesn't need buffer */
  *bufsz= 0;

  seq_it= seq->init(seq_init_param, n_ranges, *flags);
  while (!seq->next(seq_it, &range))
  {
    ha_rows rows;

    if (unlikely(thd->killed != 0))
      DBUG_RETURN(HA_POS_ERROR);

    n_ranges++;
    key_range *min_endp, *max_endp;
    if (range.range_flag & GEOM_FLAG)
    {
      /* In this case tmp_min_flag contains the handler-read-function */
      range.start_key.flag= (ha_rkey_function) (range.range_flag ^ GEOM_FLAG);
      min_endp= &range.start_key;
      max_endp= NULL;
    }
    else
    {
      min_endp= range.start_key.length? &range.start_key : NULL;
      max_endp= range.end_key.length? &range.end_key : NULL;
    }
    int keyparts_used= my_count_bits(range.start_key.keypart_map);

    if ((range.range_flag & UNIQUE_RANGE) && !(range.range_flag & NULL_RANGE))
    {
      rows= 1;
      /*
        In this case we do not call records_in_range() and as a result
        do not get any info on the edge blocks for this range. However if it
        happens that the range for which we have such info uses the same block
        for its first record as the last range for which such info is
        provided uses for its last record then this range can be assigned
        later to one of the blocks used by other ranges.

        Note that we don't have to increment edge_blocks_cnt or
        range_blocks_cnt here.
      */
      single_point_ranges++;
    }
    else if (use_statistics_for_eq_range &&
             !(range.range_flag & NULL_RANGE) &&
             (range.range_flag & EQ_RANGE) &&
             table->key_info[keyno].actual_rec_per_key(keyparts_used - 1) > 0.5)
    {
      rows= ((ha_rows) table->key_info[keyno].
             actual_rec_per_key(keyparts_used-1));
      range_blocks_cnt+= ((MY_MAX(rows, 1) - 1) / avg_block_records + 1);
    }
    else
    {
      page_range pages= unused_page_range;
      if ((rows= this->records_in_range(keyno, min_endp, max_endp, &pages)) ==
          HA_POS_ERROR)
      {
        /* Can't scan one range => can't do MRR scan at all */
        total_rows= HA_POS_ERROR;
        if (thd->is_error())
          DBUG_RETURN(HA_POS_ERROR);
        break;
      }
      if (pages.first_page == UNUSED_PAGE_NO)
      {
        /*
          The engine does not provide info on the range position.
          Place the range in a new block. Note that in this case
          any new range will be placed in a new block.
	*/
        ulonglong additional_blocks= ((MY_MAX(rows,1) - 1) / avg_block_records +
                                      1);
        edge_blocks_cnt+= additional_blocks == 1 ? 1 : 2;
        range_blocks_cnt+= additional_blocks;
      }
      else
      {
        /* The info on the range position is provided */
        if (pages.first_page == prev_range_last_block)
	{
          /*
            The new range starts in the same block that the last range
            for which the position of the range was provided.
	  */
          /*
            First add records of single point ranges that can be placed
            between these two ranges.
	  */
          prev_range_last_block_records+= (single_point_ranges -
                                           assigned_single_point_ranges);
          assigned_single_point_ranges= single_point_ranges;
          if (pages.first_page == pages.last_page)
	  {
            /*
              All records of the current range are in the same block
              Note that the prev_range_last_block_records can be much larger
              than max_records_in_block as the rows can be compressed!
            */
            prev_range_last_block_records+= rows;
            DBUG_ASSERT(prev_range_last_block_records <
                        stats.block_size);
          }
          else
	  {
            /*
              The current range spans more than one block

              Place part of the range records in 'prev_range_last_block'
              and the remaining records in additional blocks.

              We don't know where the first key was positioned in the
              block, so we assume the range started in the middle of the
              block.

              Note that prev_range_last_block_records > avg_block_records
              can be true in case of compressed rows.
            */
            ha_rows rem_rows= rows;

            if (avg_block_records > prev_range_last_block_records)
            {
              ha_rows space_left_in_prev_block=
                (avg_block_records - prev_range_last_block_records)/2;
              rem_rows= 0;
              if (rows > space_left_in_prev_block)
                rem_rows= rows - space_left_in_prev_block;
            }
            /* Calculate how many additional blocks we need for rem_rows */
            ulonglong additional_blocks= ((MY_MAX(rem_rows, 1) - 1) /
                                          avg_block_records + 1);
            edge_blocks_cnt++;
            range_blocks_cnt+= additional_blocks;
            prev_range_last_block= pages.last_page;
            /* There is at least one row on last page */
            prev_range_last_block_records= 1;
          }
        }
        else
	{
          /*
            The new range does not start in the same block that the last range
            for which the position of the range was provided.
            Note that rows may be 0!
	  */
          ulonglong additional_blocks= ((MY_MAX(rows, 1) - 1) /
                                        avg_block_records + 1);
          edge_blocks_cnt+= additional_blocks == 1 ? 1 : 2;
          range_blocks_cnt+= additional_blocks;
          unassigned_single_point_ranges+= (single_point_ranges -
                                            assigned_single_point_ranges);
          assigned_single_point_ranges= single_point_ranges;
          prev_range_last_block= pages.last_page;
          /* There is at least one row on last page */
          prev_range_last_block_records= 1;
        }
      }
    }
    total_rows+= rows;
  }
  /*
    Count the number of io_blocks that where not yet read and thus not cached.
    The number of equal read blocks that where not read are:

    (single_point_ranges - assigned_single_point_ranges).

    We don't add these to io_blocks as we don't want to penalize equal
    reads (if we did, a range that would read 5 rows would be
    regarded as better than one equal read).

    Better to assume we have done a records_in_range() for the equal
    range and it's also cached.

    One effect of this is that io_blocks for simple ranges are often 0,
    as the blocks where already read by records_in_range and we assume
    that we don't have to read it again.
  */
  io_blocks= (range_blocks_cnt - edge_blocks_cnt);
  unassigned_single_point_ranges+= (single_point_ranges -
                                    assigned_single_point_ranges);

  if (total_rows != HA_POS_ERROR)
  {
    set_if_smaller(total_rows, max_rows);
    *flags |= HA_MRR_USE_DEFAULT_IMPL;
    calculate_costs(cost, keyno, n_ranges,
                    n_ranges - (uint) single_point_ranges,
                    *flags, total_rows,
                    io_blocks, unassigned_single_point_ranges);
    if (top_limit < total_rows)
    {
      /*
        Calculate what the cost would be if we only have to read 'top_limit'
        rows. This is the lowest possible cost when using the range
        when we find the 'accepted rows' at once.
      */
      Cost_estimate limit_cost;
      calculate_costs(&limit_cost, keyno, n_ranges,
                      n_ranges - (uint)single_point_ranges,
                      *flags, top_limit, io_blocks,
                      unassigned_single_point_ranges);
      cost->limit_cost= limit_cost.total_cost();
    }
    DBUG_PRINT("statistics",
               ("key: %s  rows: %llu  total_cost: %.3f  io_blocks: %llu  "
                "cpu_cost: %.3f",
                table->s->keynames.type_names[keyno],
                (ulonglong) total_rows, cost->total_cost(),
                (ulonglong) (cost->row_cost.io + cost->index_cost.io),
                (double) (cost->row_cost.cpu + cost->index_cost.cpu)));
  }
  DBUG_RETURN(total_rows);
}


/**
  Get cost and other information about MRR scan over some sequence of ranges

  Calculate estimated cost and other information about an MRR scan for some
  sequence of ranges.

  The ranges themselves will be known only at execution phase. When this
  function is called we only know number of ranges and a (rough) E(#records)
  within those ranges.

  Currently this function is only called for "n-keypart singlepoint" ranges,
  i.e. each range is "keypart1=someconst1 AND ... AND keypartN=someconstN"

  The flags parameter is a combination of those flags: HA_MRR_SORTED,
  HA_MRR_INDEX_ONLY, HA_MRR_NO_ASSOCIATION, HA_MRR_LIMITS.

  @param keyno           Index number
  @param n_ranges        Estimated number of ranges (i.e. intervals) in the
                         range sequence.
  @param total_rows      Estimated total number of records contained within all
                         of the ranges
  @param bufsz    INOUT  IN:  Size of the buffer available for use
                         OUT: Size of the buffer that will be actually used, or
                              0 if buffer is not needed.
  @param flags    INOUT  A combination of HA_MRR_* flags
  @param cost     OUT    Estimated cost of MRR access

  @retval
    0     OK, *cost contains cost of the scan, *bufsz and *flags contain scan
          parameters.
  @retval
    other Error or can't perform the requested scan
*/

ha_rows handler::multi_range_read_info(uint keyno, uint n_ranges,
                                       uint total_rows,
                                       uint key_parts, uint *bufsz,
                                       uint *flags, Cost_estimate *cost)
{
  /* 
    Currently we expect this function to be called only in preparation of scan
    with HA_MRR_SINGLE_POINT property.
  */
  DBUG_ASSERT(*flags | HA_MRR_SINGLE_POINT);

  *bufsz= 0; /* Default implementation doesn't need a buffer */
  *flags |= HA_MRR_USE_DEFAULT_IMPL;

  cost->reset(this);

  /* Produce the same cost as non-MRR code does */
  if (!is_clustering_key(keyno))
  {
    cost->index_cost= ha_keyread_time(keyno, n_ranges, total_rows, 0);

    if (!(*flags & HA_MRR_INDEX_ONLY))
    {
      /* ha_rnd_pos_time includes ROW_COPY_COST */
      cost->row_cost= ha_rnd_pos_time(total_rows);
    }
    else
    {
      /* Index only read */
      cost->copy_cost= rows2double(total_rows) * KEY_COPY_COST;
    }
  }
  else
  {
    /* Clustering key */
    cost->index_cost= ha_keyread_clustered_time(keyno, n_ranges, total_rows,
                                                0);
    cost->copy_cost= rows2double(total_rows) * ROW_COPY_COST;
  }
  cost->comp_cost= rows2double(total_rows) * WHERE_COST;
  return 0;
}


/**
  Initialize the MRR scan

  Initialize the MRR scan. This function may do heavyweight scan 
  initialization like row prefetching/sorting/etc (NOTE: but better not do
  it here as we may not need it, e.g. if we never satisfy WHERE clause on
  previous tables. For many implementations it would be natural to do such
  initializations in the first multi_read_range_next() call)

  mode is a combination of the following flags: HA_MRR_SORTED,
  HA_MRR_INDEX_ONLY, HA_MRR_NO_ASSOCIATION 

  @param seq             Range sequence to be traversed
  @param seq_init_param  First parameter for seq->init()
  @param n_ranges        Number of ranges in the sequence
  @param mode            Flags, see the description section for the details
  @param buf             INOUT: memory buffer to be used

  @note
    One must have called index_init() before calling this function. Several
    multi_range_read_init() calls may be made in course of one query.

    Buffer memory management is done according to the following scenario:
    The caller allocates the buffer and provides it to the callee by filling
    the members of HANDLER_BUFFER structure.
    The callee consumes all or some fraction of the provided buffer space, and
    sets the HANDLER_BUFFER members accordingly.
    The callee may use the buffer memory until the next multi_range_read_init()
    call is made, all records have been read, or until index_end() call is
    made, whichever comes first.

  @retval 0  OK
  @retval 1  Error
*/

int
handler::multi_range_read_init(RANGE_SEQ_IF *seq_funcs, void *seq_init_param,
                               uint n_ranges, uint mode, HANDLER_BUFFER *buf)
{
  DBUG_ENTER("handler::multi_range_read_init");
  mrr_iter= seq_funcs->init(seq_init_param, n_ranges, mode);
  mrr_funcs= *seq_funcs;
  mrr_is_output_sorted= MY_TEST(mode & HA_MRR_SORTED);
  mrr_have_range= FALSE;
  DBUG_RETURN(0);
}

/**
  Get next record in MRR scan

  Default MRR implementation: read the next record

  @param range_info  OUT  Undefined if HA_MRR_NO_ASSOCIATION flag is in effect
                          Otherwise, the opaque value associated with the range
                          that contains the returned record.

  @retval 0      OK
  @retval other  Error code
*/

int handler::multi_range_read_next(range_id_t *range_info)
{
  int result= HA_ERR_END_OF_FILE;
  bool range_res;
  DBUG_ENTER("handler::multi_range_read_next");

  if (!mrr_have_range)
  {
    mrr_have_range= TRUE;
    goto start;
  }

  do
  {
    /* Save a call if there can be only one row in range. */
    if (mrr_cur_range.range_flag != (UNIQUE_RANGE | EQ_RANGE))
    {
      result= read_range_next();
      /* On success or non-EOF errors jump to the end. */
      if (result != HA_ERR_END_OF_FILE)
        break;
    }
    else
    {
      if (ha_was_semi_consistent_read())
      {
        /*
          The following assignment is redundant, but for extra safety and to
          remove the compiler warning:
        */
        range_res= FALSE;
        goto scan_it_again;
      }
      /*
        We need to set this for the last range only, but checking this
        condition is more expensive than just setting the result code.
      */
      result= HA_ERR_END_OF_FILE;
    }

start:
    /* Try the next range(s) until one matches a record. */
    while (!(range_res= mrr_funcs.next(mrr_iter, &mrr_cur_range)))
    {
scan_it_again:
      result= read_range_first(mrr_cur_range.start_key.keypart_map ?
                                 &mrr_cur_range.start_key : 0,
                               mrr_cur_range.end_key.keypart_map ?
                                 &mrr_cur_range.end_key : 0,
                               MY_TEST(mrr_cur_range.range_flag & EQ_RANGE),
                               mrr_is_output_sorted);
      if (result != HA_ERR_END_OF_FILE)
        break;
    }
  }
  while ((result == HA_ERR_END_OF_FILE) && !range_res);

  *range_info= mrr_cur_range.ptr;
  DBUG_PRINT("exit",("handler::multi_range_read_next result %d", result));
  DBUG_RETURN(result);
}

/****************************************************************************
 * Mrr_*_reader classes (building blocks for DS-MRR)
 ***************************************************************************/

int Mrr_simple_index_reader::init(handler *h_arg, RANGE_SEQ_IF *seq_funcs, 
                                  void *seq_init_param, uint n_ranges,
                                  uint mode,  Key_parameters *key_par_arg,
                                  Lifo_buffer *key_buffer_arg,
                                  Buffer_manager *buf_manager_arg)
{
  HANDLER_BUFFER no_buffer = {NULL, NULL, NULL};
  file= h_arg;
  return file->handler::multi_range_read_init(seq_funcs, seq_init_param,
                                              n_ranges, mode, &no_buffer);
}


int Mrr_simple_index_reader::get_next(range_id_t *range_info)
{
  int res;
  while (!(res= file->handler::multi_range_read_next(range_info)))
  {
    KEY_MULTI_RANGE *curr_range= &file->handler::mrr_cur_range;
    if (!file->mrr_funcs.skip_index_tuple ||
        !file->mrr_funcs.skip_index_tuple(file->mrr_iter, curr_range->ptr))
      break;
  }
  if (res && res != HA_ERR_END_OF_FILE && res != HA_ERR_KEY_NOT_FOUND)
    file->print_error(res, MYF(0));             // Fatal error
  return res;
}


/**
  @brief Get next index record

  @param range_info  OUT identifier of range that the returned record belongs to
  
  @note
    We actually iterate over nested sequences:
    - an ordered sequence of groups of identical keys
      - each key group has key value, which has multiple matching records 
        - thus, each record matches all members of the key group

  @retval 0                   OK, next record was successfully read
  @retval HA_ERR_END_OF_FILE  End of records
  @retval Other               Some other error; Error is printed
*/

int Mrr_ordered_index_reader::get_next(range_id_t *range_info)
{
  int res;
  DBUG_ENTER("Mrr_ordered_index_reader::get_next");
  
  for(;;)
  {
    if (!scanning_key_val_iter)
    {
      while ((res= kv_it.init(this)))
      {
        if ((res != HA_ERR_KEY_NOT_FOUND && res != HA_ERR_END_OF_FILE))
          DBUG_RETURN(res); /* Some fatal error */

        if (key_buffer->is_empty())
        {
          DBUG_RETURN(HA_ERR_END_OF_FILE);
        }
      }
      scanning_key_val_iter= TRUE;
    }

    if ((res= kv_it.get_next(range_info)))
    {
      scanning_key_val_iter= FALSE;
      if ((res != HA_ERR_KEY_NOT_FOUND && res != HA_ERR_END_OF_FILE))
        DBUG_RETURN(res);
      kv_it.move_to_next_key_value();
      continue;
    }
    if (!skip_index_tuple(*range_info) &&
        !skip_record(*range_info, NULL))
    {
      break;
    }
    /* Go get another (record, range_id) combination */
  } /* while */

  DBUG_RETURN(0);
}


/*
  Supply index reader with the O(1)space it needs for scan interrupt/restore
  operation
*/

bool Mrr_ordered_index_reader::set_interruption_temp_buffer(uint rowid_length,
                                                            uint key_len, 
                                                            uint saved_pk_len,
                                                            uchar **space_start,
                                                            uchar *space_end)
{
  if (space_end - *space_start <= (ptrdiff_t)(rowid_length + key_len + saved_pk_len))
    return TRUE;
  support_scan_interruptions= TRUE; 
  
  saved_rowid= *space_start;
  *space_start += rowid_length;
  
  if (saved_pk_len)
  {
    saved_primary_key= *space_start;
    *space_start += saved_pk_len;
  }
  else
    saved_primary_key= NULL;

  saved_key_tuple= *space_start;
  *space_start += key_len;

  have_saved_rowid= FALSE;
  read_was_interrupted= FALSE;
  return FALSE;
}

void Mrr_ordered_index_reader::set_no_interruption_temp_buffer()
{
  support_scan_interruptions= FALSE;
  saved_key_tuple= saved_rowid= saved_primary_key= NULL; /* safety */
  have_saved_rowid= FALSE;
  read_was_interrupted= FALSE;
}

void Mrr_ordered_index_reader::interrupt_read()
{
  DBUG_ASSERT(support_scan_interruptions);
  TABLE *table= file->get_table();
  KEY *used_index= &table->key_info[file->active_index];
  /* Save the current key value */
  key_copy(saved_key_tuple, table->record[0],
           used_index, used_index->key_length);
  
  if (saved_primary_key)
  {
    key_copy(saved_primary_key, table->record[0], 
             &table->key_info[table->s->primary_key],
             table->key_info[table->s->primary_key].key_length);
  }
  read_was_interrupted= TRUE;

  /* Save the last rowid */
  memcpy(saved_rowid, file->ref, file->ref_length);
  have_saved_rowid= TRUE;
}

void Mrr_ordered_index_reader::position()
{
  if (have_saved_rowid)
    memcpy(file->ref, saved_rowid, file->ref_length);
  else
    Mrr_index_reader::position();
}

void Mrr_ordered_index_reader::resume_read()
{
  TABLE *table= file->get_table();

  if (!read_was_interrupted)
    return;

  KEY *used_index= &table->key_info[file->active_index];
  key_restore(table->record[0], saved_key_tuple, 
              used_index, used_index->key_length);
  if (saved_primary_key)
  {
    key_restore(table->record[0], saved_primary_key, 
                &table->key_info[table->s->primary_key],
                table->key_info[table->s->primary_key].key_length);
  }
}


/**
  Fill the buffer with (lookup_tuple, range_id) pairs and sort

  @return 
    0                   OK, the buffer is non-empty and sorted
    HA_ERR_END_OF_FILE  Source exhausted, the buffer is empty.
*/

int Mrr_ordered_index_reader::refill_buffer(bool initial)
{
  KEY_MULTI_RANGE cur_range;
  DBUG_ENTER("Mrr_ordered_index_reader::refill_buffer");

  DBUG_ASSERT(key_buffer->is_empty());

  if (source_exhausted)
    DBUG_RETURN(HA_ERR_END_OF_FILE);

  buf_manager->reset_buffer_sizes(buf_manager->arg);
  key_buffer->reset();
  key_buffer->setup_writing(keypar.key_size_in_keybuf,
                            is_mrr_assoc? sizeof(range_id_t) : 0);

  while (key_buffer->can_write() && 
         !(source_exhausted= mrr_funcs.next(mrr_iter, &cur_range)))
  {
    DBUG_ASSERT(cur_range.range_flag & EQ_RANGE);

    /* Put key, or {key, range_id} pair into the buffer */
    key_buffer->write_ptr1= keypar.use_key_pointers ?
                              (uchar*)&cur_range.start_key.key : 
                              (uchar*)cur_range.start_key.key;
    key_buffer->write_ptr2= (uchar*)&cur_range.ptr;
    key_buffer->write();
  }
  
  /* Force get_next() to start with kv_it.init() call: */
  scanning_key_val_iter= FALSE;

  if (source_exhausted && key_buffer->is_empty())
    DBUG_RETURN(HA_ERR_END_OF_FILE);

  if (!initial)
  {
    /* This is a non-initial buffer fill and we've got a non-empty buffer */
    THD *thd= current_thd;
    status_var_increment(thd->status_var.ha_mrr_key_refills_count);
  }

  key_buffer->sort((key_buffer->type() == Lifo_buffer::FORWARD)
                       ? Mrr_ordered_index_reader::compare_keys_reverse
                       : Mrr_ordered_index_reader::compare_keys,
                   this);
  DBUG_RETURN(0);
}


int Mrr_ordered_index_reader::init(handler *h_arg, RANGE_SEQ_IF *seq_funcs,
                                   void *seq_init_param, uint n_ranges,
                                   uint mode, Key_parameters *key_par_arg,
                                   Lifo_buffer *key_buffer_arg,
                                   Buffer_manager *buf_manager_arg)
{
  file= h_arg;
  key_buffer= key_buffer_arg;
  buf_manager= buf_manager_arg;
  keypar= *key_par_arg;

  KEY *key_info= &file->get_table()->key_info[file->active_index];
  keypar.index_ranges_unique= MY_TEST(key_info->flags & HA_NOSAME &&
                                      key_info->user_defined_key_parts ==
                                      my_count_bits(keypar.key_tuple_map));

  mrr_iter= seq_funcs->init(seq_init_param, n_ranges, mode);
  is_mrr_assoc= !MY_TEST(mode & HA_MRR_NO_ASSOCIATION);
  mrr_funcs= *seq_funcs;
  source_exhausted= FALSE;
  read_was_interrupted= false;
  have_saved_rowid= FALSE;
  return 0;
}


static int rowid_cmp_reverse(void *file, const void *a, const void *b)
{
  return -(static_cast<handler *>(file))
              ->cmp_ref(static_cast<const uchar *>(a),
                        static_cast<const uchar *>(b));
}


int Mrr_ordered_rndpos_reader::init(handler *h_arg, 
                                    Mrr_index_reader *index_reader_arg,
                                    uint mode,
                                    Lifo_buffer *buf,
                                    Rowid_filter *filter)
{
  file= h_arg;
  index_reader= index_reader_arg;
  rowid_buffer= buf;
  is_mrr_assoc= !MY_TEST(mode & HA_MRR_NO_ASSOCIATION);
  index_reader_exhausted= FALSE;
  index_reader_needs_refill= TRUE;
  rowid_filter= filter;

  return 0;
}


/**
  DS-MRR: Fill and sort the rowid buffer

  Scan the MRR ranges and collect ROWIDs (or {ROWID, range_id} pairs) into 
  buffer. When the buffer is full or scan is completed, sort the buffer by 
  rowid and return.

  When this function returns, either rowid buffer is not empty, or the source
  of lookup keys (i.e. ranges) is exhaused.
  
  @retval 0      OK, the next portion of rowids is in the buffer,
                 properly ordered
  @retval other  Error
*/

int Mrr_ordered_rndpos_reader::refill_buffer(bool initial)
{
  int res;
  bool first_call= initial;
  DBUG_ENTER("Mrr_ordered_rndpos_reader::refill_buffer");

  if (index_reader_exhausted)
    DBUG_RETURN(HA_ERR_END_OF_FILE);

  while (initial || index_reader_needs_refill || 
         (res= refill_from_index_reader()) == HA_ERR_END_OF_FILE)
  {
    if ((res= index_reader->refill_buffer(initial)))
    {
      if (res == HA_ERR_END_OF_FILE)
        index_reader_exhausted= TRUE;
      break;
    }
    initial= FALSE;
    index_reader_needs_refill= FALSE;
  }

  if (!first_call && !index_reader_exhausted)
  {
    /* Ok, this was a successful buffer refill operation */
    THD *thd= current_thd;
    status_var_increment(thd->status_var.ha_mrr_rowid_refills_count);
  }

  DBUG_RETURN(res);
}


void Mrr_index_reader::position()
{
  file->position(file->get_table()->record[0]);
}


/* 
  @brief Try to refill the rowid buffer without calling
  index_reader->refill_buffer(). 
*/

int Mrr_ordered_rndpos_reader::refill_from_index_reader()
{
  range_id_t range_info;
  int res;
  DBUG_ENTER("Mrr_ordered_rndpos_reader::refill_from_index_reader");

  DBUG_ASSERT(rowid_buffer->is_empty());
  index_rowid= index_reader->get_rowid_ptr();
  rowid_buffer->reset();
  rowid_buffer->setup_writing(file->ref_length,
                              is_mrr_assoc? sizeof(range_id_t) : 0);

  last_identical_rowid= NULL;

  index_reader->resume_read();
  while (rowid_buffer->can_write())
  {
    res= index_reader->get_next(&range_info);

    if (res)
    {
      if (res != HA_ERR_END_OF_FILE)
        DBUG_RETURN(res);
      index_reader_needs_refill=TRUE;
      break;
    }

    index_reader->position();

    /*
      If the built rowid filter cannot be used at the engine level, use it here.
    */
    if (rowid_filter && !file->pushed_rowid_filter &&
        !rowid_filter->check((char *)index_rowid))
      continue;

    /* Put rowid, or {rowid, range_id} pair into the buffer */
    rowid_buffer->write_ptr1= index_rowid;
    rowid_buffer->write_ptr2= (uchar*)&range_info;
    rowid_buffer->write();
  }
  
  /*
    When index_reader_needs_refill=TRUE, this means we've got all of index
    tuples for lookups keys that index_reader had. We are not in the middle
    of an index read, so there is no need to call interrupt_read.

    Actually, we must not call interrupt_read(), because it could be that we
    haven't read a single row (because all index lookups returned
    HA_ERR_KEY_NOT_FOUND). In this case, interrupt_read() will cause [harmless]
    valgrind warnings when trying to save garbage from table->record[0].
  */
  if (!index_reader_needs_refill)
    index_reader->interrupt_read();
  /* Sort the buffer contents by rowid */
  rowid_buffer->sort(rowid_cmp_reverse, (void*)file);

  rowid_buffer->setup_reading(file->ref_length,
                              is_mrr_assoc ? sizeof(range_id_t) : 0);
  DBUG_RETURN(rowid_buffer->is_empty()? HA_ERR_END_OF_FILE : 0);
}


/*
  Get the next {record, range_id} using ordered array of rowid+range_id pairs

  @note
    Since we have sorted rowids, we try not to make multiple rnd_pos() calls
    with the same rowid value.
*/

int Mrr_ordered_rndpos_reader::get_next(range_id_t *range_info)
{
  int res;
  
  /* 
    First, check if rowid buffer has elements with the same rowid value as
    the previous.
  */
  while (last_identical_rowid)
  {
    /*
      Current record (the one we've returned in previous call) was obtained
      from a rowid that matched multiple range_ids. Return this record again,
      with next matching range_id.
    */
    (void)rowid_buffer->read();

    if (rowid_buffer->read_ptr1 == last_identical_rowid)
      last_identical_rowid= NULL; /* reached the last of identical rowids */

    if (!is_mrr_assoc)
      return 0;

    memcpy(range_info, rowid_buffer->read_ptr2, sizeof(range_id_t));
    if (!index_reader->skip_record(*range_info, rowid_buffer->read_ptr1))
      return 0;
  }
  
  /* 
     Ok, last_identical_rowid==NULL, it's time to read next different rowid
     value and get record for it.
  */
  for(;;)
  {
    /* Return eof if there are no rowids in the buffer after re-fill attempt */
    if (rowid_buffer->read())
      return HA_ERR_END_OF_FILE;

    if (is_mrr_assoc)
    {
      memcpy(range_info, rowid_buffer->read_ptr2, sizeof(range_id_t));
      if (index_reader->skip_record(*range_info, rowid_buffer->read_ptr1))
        continue;
    }

    res= file->ha_rnd_pos(file->get_table()->record[0], 
                          rowid_buffer->read_ptr1);

    if (res)
      return res; /* Some fatal error */

    break; /* Got another record */
  }

  /* 
    Check if subsequent buffer elements have the same rowid value as this
    one. If yes, remember this fact so that we don't make any more rnd_pos()
    calls with this value.

    Note: this implies that SQL layer doesn't touch table->record[0]
    between calls.
  */
  Lifo_buffer_iterator it;
  it.init(rowid_buffer);
  while (!it.read())
  {
    if (file->cmp_ref(it.read_ptr1, rowid_buffer->read_ptr1))
      break;
    last_identical_rowid= it.read_ptr1;
  }
  return 0;
}


/****************************************************************************
 * Top-level DS-MRR implementation functions (the ones called by storage engine)
 ***************************************************************************/

/**
  DS-MRR: Initialize and start MRR scan

  Initialize and start the MRR scan. Depending on the mode parameter, this
  may use default or DS-MRR implementation.

  @param h_arg           Table handler to be used
  @param key             Index to be used
  @param seq_funcs       Interval sequence enumeration functions
  @param seq_init_param  Interval sequence enumeration parameter
  @param n_ranges        Number of ranges in the sequence.
  @param mode            HA_MRR_* modes to use
  @param buf             INOUT Buffer to use

  @retval 0     Ok, Scan started.
  @retval other Error
*/

int DsMrr_impl::dsmrr_init(handler *h_arg, RANGE_SEQ_IF *seq_funcs, 
                           void *seq_init_param, uint n_ranges, uint mode,
                           HANDLER_BUFFER *buf)
{
  TABLE *table= h_arg->get_table();
  THD *thd= table->in_use;
  int res;
  Key_parameters keypar;
  uint UNINIT_VAR(key_buff_elem_size); /* set/used when do_sort_keys==TRUE */
  handler *h_idx;
  Mrr_ordered_rndpos_reader *disk_strategy= NULL;
  bool do_sort_keys= FALSE;
  DBUG_ENTER("DsMrr_impl::dsmrr_init");
  /*
    index_merge may invoke a scan on an object for which dsmrr_info[_const]
    has not been called, so set the owner handler here as well.
  */
  primary_file= h_arg;
  is_mrr_assoc= !MY_TEST(mode & HA_MRR_NO_ASSOCIATION);

  strategy_exhausted= FALSE;
  
  /* By default, have do-nothing buffer manager */
  buf_manager.arg= this;
  buf_manager.reset_buffer_sizes= do_nothing;
  buf_manager.redistribute_buffer_space= do_nothing;

  if (!hint_key_state(thd, table, h_arg->active_index,
                      MRR_HINT_ENUM, OPTIMIZER_SWITCH_MRR) ||
      mode & (HA_MRR_USE_DEFAULT_IMPL | HA_MRR_SORTED))
    goto use_default_impl;
  
  /*
    Determine whether we'll need to do key sorting and/or rnd_pos() scan
  */
  index_strategy= NULL;
  if ((mode & HA_MRR_SINGLE_POINT) &&
      optimizer_flag(thd, OPTIMIZER_SWITCH_MRR_SORT_KEYS))
  {
    do_sort_keys= TRUE;
    index_strategy= &reader_factory.ordered_index_reader;
  }
  else
    index_strategy= &reader_factory.simple_index_reader;

  strategy= index_strategy;
  /*
    We don't need a rowid-to-rndpos step if
     - We're doing a scan on clustered primary key
     - [In the future] We're doing an index_only read
  */
  DBUG_ASSERT(primary_file->inited == handler::INDEX || 
              (primary_file->inited == handler::RND && 
               secondary_file && 
               secondary_file->inited == handler::INDEX));

  h_idx= (primary_file->inited == handler::INDEX)? primary_file: secondary_file;
  keyno= h_idx->active_index;

  if (! h_idx->is_clustering_key(keyno))
  {
    strategy= disk_strategy= &reader_factory.ordered_rndpos_reader;
    if (h_arg->pushed_rowid_filter)
    {
      /*
        Currently usage of a rowid filter within InnoDB engine is not supported
        if the table is accessed by the primary key.
        With optimizer switches ''mrr' and 'mrr_sort_keys' are both enabled
        any access by a secondary index is converted to the rndpos access. In
        InnoDB the rndpos access is always uses the primary key.
        Do not use pushed rowid filter if the table is accessed actually by the
        primary key. Use the rowid filter outside the engine code (see
        Mrr_ordered_rndpos_reader::refill_from_index_reader).
      */
      rowid_filter= h_arg->pushed_rowid_filter;
      h_arg->cancel_pushed_rowid_filter();
    }
  }

  full_buf= buf->buffer;
  full_buf_end= buf->buffer_end;

  if (do_sort_keys)
  {
    /* Pre-calculate some parameters of key sorting */
    keypar.use_key_pointers= MY_TEST(mode & HA_MRR_MATERIALIZED_KEYS);
    seq_funcs->get_key_info(seq_init_param, &keypar.key_tuple_length, 
                            &keypar.key_tuple_map);
    keypar.key_size_in_keybuf= keypar.use_key_pointers? 
                                 sizeof(char*) : keypar.key_tuple_length;
    key_buff_elem_size= keypar.key_size_in_keybuf + (int)is_mrr_assoc * sizeof(void*);
    
    /* Ordered index reader needs some space to store an index tuple */
    if (strategy != index_strategy)
    {
      uint saved_pk_length=0;
      uint pk= h_idx->get_table()->s->primary_key;
      if (h_idx->pk_is_clustering_key(pk))
      {
        saved_pk_length= h_idx->get_table()->key_info[pk].key_length;
      }
      
      KEY *used_index= &h_idx->get_table()->key_info[h_idx->active_index];
      if (reader_factory.ordered_index_reader.
            set_interruption_temp_buffer(primary_file->ref_length,
                                         used_index->key_length,
                                         saved_pk_length,
                                         &full_buf, full_buf_end))
        goto use_default_impl;
    }
    else
      reader_factory.ordered_index_reader.set_no_interruption_temp_buffer();
  }

  if (strategy == index_strategy)
  {
    /* 
      Index strategy alone handles the record retrieval. Give all buffer space
      to it. Key buffer should have forward orientation so we can return the
      end of it.
    */
    key_buffer= &forward_key_buf;
    key_buffer->set_buffer_space(full_buf, full_buf_end);
    
    /* Safety: specify that rowid buffer has zero size: */
    rowid_buffer.set_buffer_space(full_buf_end, full_buf_end);

    if (do_sort_keys && !key_buffer->have_space_for(key_buff_elem_size))
      goto use_default_impl;

    if ((res= index_strategy->init(primary_file, seq_funcs, seq_init_param, n_ranges,
                                   mode, &keypar, key_buffer, &buf_manager)))
      goto error;
  }
  else
  {
    /* We'll have both index and rndpos strategies working together */
    if (do_sort_keys)
    {
      /* Both strategies will need buffer space, share the buffer */
      if (setup_buffer_sharing(keypar.key_size_in_keybuf, keypar.key_tuple_map))
        goto use_default_impl;

      buf_manager.reset_buffer_sizes= reset_buffer_sizes;
      buf_manager.redistribute_buffer_space= redistribute_buffer_space;
    }
    else
    {
      /* index strategy doesn't need buffer, give all space to rowids*/
      rowid_buffer.set_buffer_space(full_buf, full_buf_end);
      if (!rowid_buffer.have_space_for(primary_file->ref_length + 
                                       (int)is_mrr_assoc * sizeof(range_id_t)))
        goto use_default_impl;
    }

    // setup_two_handlers() will call dsmrr_close() will clears the filter.
    // Save its value and restore afterwards.
    Rowid_filter *tmp = rowid_filter;
    if ((res= setup_two_handlers()))
      goto error;
    rowid_filter= tmp;

    if ((res= index_strategy->init(secondary_file, seq_funcs, seq_init_param,
                                   n_ranges, mode, &keypar, key_buffer, 
                                   &buf_manager)) || 
        (res= disk_strategy->init(primary_file, index_strategy, mode, 
                                  &rowid_buffer, rowid_filter)))
    {
      goto error;
    }
  }
  
  /* 
    At this point, we're sure that we're running a native MRR scan (i.e. we
    didnt fall back to default implementation for some reason).
  */
  status_var_increment(thd->status_var.ha_mrr_init_count);

  res= strategy->refill_buffer(TRUE);
  if (res)
  {
    if (res != HA_ERR_END_OF_FILE)
      goto error;
    strategy_exhausted= TRUE;
  }

  /*
    If we have scanned through all intervals in *seq, then adjust *buf to 
    indicate that the remaining buffer space will not be used.
  */
//  if (dsmrr_eof) 
//    buf->end_of_used_area= rowid_buffer.end_of_space();

  
  DBUG_RETURN(0);
error:
  close_second_handler();
   /* Safety, not really needed but: */
  strategy= NULL;
  DBUG_RETURN(res);

use_default_impl:
  if (primary_file->inited != handler::INDEX)
  {
    /* We can get here when 
       - we've previously successfully done a DS-MRR scan (and so have 
         secondary_file!= NULL, secondary_file->inited= INDEX, 
         primary_file->inited=RND)
       - for this invocation, we haven't got enough buffer space, and so we
         have to use the default MRR implementation.

      note: primary_file->ha_index_end() will call dsmrr_close() which will
      close/destroy the secondary_file, this is intentional. 
      (Yes this is slow, but one can't expect performance with join buffer 
       so small that it can accomodate one rowid and one index tuple)
    */
    if ((res= primary_file->ha_rnd_end()) || 
        (res= primary_file->ha_index_init(keyno, MY_TEST(mode & HA_MRR_SORTED))))
    {
      DBUG_RETURN(res);
    }
  }
  /* Call correct init function and assign to top level object */
  Mrr_simple_index_reader *s= &reader_factory.simple_index_reader;
  res= s->init(primary_file, seq_funcs, seq_init_param, n_ranges, mode, NULL, 
               NULL, NULL);
  strategy= s;
  DBUG_RETURN(res);
}


/*
  Whatever the current state is, make it so that we have two handler objects:
  - primary_file       -  initialized for rnd_pos() scan
  - secondary_file     -  initialized for scanning the index specified in
                          this->keyno
  RETURN 
    0        OK
    HA_XXX   Error code
*/

int DsMrr_impl::setup_two_handlers()
{
  int res;
  THD *thd= primary_file->get_table()->in_use;
  DBUG_ENTER("DsMrr_impl::setup_two_handlers");
  if (!secondary_file)
  {
    handler *new_h2;
    Item *pushed_cond= NULL;
    DBUG_ASSERT(primary_file->inited == handler::INDEX);
    /* Create a separate handler object to do rnd_pos() calls. */
    /*
      ::clone() takes up a lot of stack, especially on 64 bit platforms.
      The constant 5 is an empiric result.
    */
    if (check_stack_overrun(thd, 5*STACK_MIN_SIZE, (uchar*) &new_h2))
      DBUG_RETURN(1);

    /* Create a separate handler object to do rnd_pos() calls. */
    if (!(new_h2= primary_file->clone(primary_file->get_table()->s->
                                      normalized_path.str,
                                      thd->mem_root)) || 
        new_h2->ha_external_lock(thd, F_RDLCK))
    {
      delete new_h2;
      DBUG_RETURN(1);
    }

    if (keyno == primary_file->pushed_idx_cond_keyno)
      pushed_cond= primary_file->pushed_idx_cond;
    
    Mrr_reader *save_strategy= strategy;
    strategy= NULL;
    /*
      Caution: this call will invoke this->dsmrr_close(). Do not put the
      created secondary table handler new_h2 into this->secondary_file or it 
      will delete it. Also, save the picked strategy
    */
    res= primary_file->ha_index_end();

    strategy= save_strategy;
    secondary_file= new_h2;

    if (res || (res= (primary_file->ha_rnd_init(FALSE))))
      goto error;

    table->prepare_for_position();
    secondary_file->extra(HA_EXTRA_KEYREAD);
    secondary_file->mrr_iter= primary_file->mrr_iter;

    if ((res= secondary_file->ha_index_init(keyno, FALSE)))
      goto error;

    if (pushed_cond)
      secondary_file->idx_cond_push(keyno, pushed_cond);
  }
  else
  {
    DBUG_ASSERT(secondary_file && secondary_file->inited==handler::INDEX);
    /* 
      We get here when the access alternates between MRR scan(s) and non-MRR
      scans.

      Calling primary_file->index_end() will invoke dsmrr_close() for this
      object, which will delete secondary_file. We need to keep it, so put it
      away and don't let it be deleted:
    */
    if (primary_file->inited == handler::INDEX)
    {
      handler *save_h2= secondary_file;
      Mrr_reader *save_strategy= strategy;
      secondary_file= NULL;
      strategy= NULL;
      res= primary_file->ha_index_end();
      secondary_file= save_h2;
      strategy= save_strategy;
      if (res)
        goto error;
    }
    if ((primary_file->inited != handler::RND) && 
        (res= primary_file->ha_rnd_init(FALSE)))
      goto error;
  }
  DBUG_RETURN(0);

error:
  DBUG_RETURN(res);
}


void DsMrr_impl::close_second_handler()
{
  if (secondary_file)
  {
    secondary_file->extra(HA_EXTRA_NO_KEYREAD);
    secondary_file->ha_index_or_rnd_end();
    secondary_file->ha_external_unlock(current_thd);
    secondary_file->ha_close();
    delete secondary_file;
    secondary_file= NULL;
  }
}


void DsMrr_impl::dsmrr_close()
{
  DBUG_ENTER("DsMrr_impl::dsmrr_close");
  rowid_filter= NULL;
  close_second_handler();
  strategy= NULL;
  DBUG_VOID_RETURN;
}


/* 
  my_qsort2-compatible static member function to compare key tuples 
*/

int Mrr_ordered_index_reader::compare_keys(void *arg, const void *key1_arg_,
                                           const void *key2_arg_)
{
  auto key1_arg= static_cast<const uchar *>(key1_arg_);
  auto key2_arg= static_cast<const uchar *>(key2_arg_);
  auto reader= static_cast<const Mrr_ordered_index_reader *>(arg);
  TABLE *table= reader->file->get_table();
  KEY_PART_INFO *part= table->key_info[reader->file->active_index].key_part;
  const uchar *key1, *key2;

  if (reader->keypar.use_key_pointers)
  {
    /* the buffer stores pointers to keys, get to the keys */
    memcpy(&key1, key1_arg, sizeof(char*));
    memcpy(&key2, key2_arg, sizeof(char*));
  }
  else
  {
    key1= key1_arg;
    key2= key2_arg;
  }

  return key_tuple_cmp(part, key1, key2, reader->keypar.key_tuple_length);
}


int Mrr_ordered_index_reader::compare_keys_reverse(void *arg, const void *key1,
                                                   const void *key2)
{
  return -compare_keys(arg, key1, key2);
}


/**
  Set the buffer space to be shared between rowid and key buffer

  @return FALSE  ok 
  @return TRUE   There is so little buffer space that we won't be able to use
                 the strategy. 
                 This happens when we don't have enough space for one rowid 
                 element and one key element so this is mainly targeted at
                 testing.
*/

bool DsMrr_impl::setup_buffer_sharing(uint key_size_in_keybuf, 
                                      key_part_map key_tuple_map)
{
  long key_buff_elem_size= key_size_in_keybuf + 
                           (int)is_mrr_assoc * sizeof(range_id_t);
  
  KEY *key_info= &primary_file->get_table()->key_info[keyno];
  /* 
    Ok if we got here we need to allocate one part of the buffer 
    for keys and another part for rowids.
  */
  ulonglong rowid_buf_elem_size= primary_file->ref_length + 
                                 (int)is_mrr_assoc * sizeof(range_id_t);
  
  /*
    Use rec_per_key statistics as a basis to find out how many rowids 
    we'll get for each key value.
     TODO: what should be the default value to use when there is no 
           statistics?
  */
  uint parts= my_count_bits(key_tuple_map);
  ha_rows rpc;
  ulonglong rowids_size= rowid_buf_elem_size;
  if ((rpc= (ha_rows) key_info->actual_rec_per_key(parts - 1)))
    rowids_size= rowid_buf_elem_size * rpc;

  double fraction_for_rowids=
    (ulonglong2double(rowids_size) / 
     (ulonglong2double(rowids_size) + key_buff_elem_size));

  ptrdiff_t bytes_for_rowids= 
    (ptrdiff_t)floor(0.5 + fraction_for_rowids * (full_buf_end - full_buf));
  
  ptrdiff_t bytes_for_keys= (full_buf_end - full_buf) - bytes_for_rowids;

  if (bytes_for_keys < key_buff_elem_size + 1 ||
      bytes_for_rowids < (ptrdiff_t)rowid_buf_elem_size + 1)
    return TRUE; /* Failed to provide minimum space for one of the buffers */

  rowid_buffer_end= full_buf + bytes_for_rowids;
  rowid_buffer.set_buffer_space(full_buf, rowid_buffer_end);
  key_buffer= &backward_key_buf;
  key_buffer->set_buffer_space(rowid_buffer_end, full_buf_end); 

  /* The above code guarantees that the buffers are big enough */
  DBUG_ASSERT(key_buffer->have_space_for(key_buff_elem_size) &&
              rowid_buffer.have_space_for((size_t)rowid_buf_elem_size));

  return FALSE;
}


void DsMrr_impl::do_nothing(void *dsmrr_arg)
{
  /* Do nothing */
}


void DsMrr_impl::reset_buffer_sizes(void *dsmrr_arg)
{
  DsMrr_impl *dsmrr= (DsMrr_impl*)dsmrr_arg;
  dsmrr->rowid_buffer.set_buffer_space(dsmrr->full_buf, 
                                       dsmrr->rowid_buffer_end);
  dsmrr->key_buffer->set_buffer_space(dsmrr->rowid_buffer_end, 
                                      dsmrr->full_buf_end);
}


/*
  Take unused space from the key buffer and give it to the rowid buffer
*/

void DsMrr_impl::redistribute_buffer_space(void *dsmrr_arg)
{
  DsMrr_impl *dsmrr= (DsMrr_impl*)dsmrr_arg;
  uchar *unused_start, *unused_end;
  dsmrr->key_buffer->remove_unused_space(&unused_start, &unused_end);
  dsmrr->rowid_buffer.grow(unused_start, unused_end);
}


/*
  @brief Initialize the iterator
  
  @note
  Initialize the iterator to produce matches for the key of the first element 
  in owner_arg->key_buffer

  @retval  0                    OK
  @retval  HA_ERR_END_OF_FILE   Either the owner->key_buffer is empty or 
                                no matches for the key we've tried (check
                                key_buffer->is_empty() to tell these apart)
  @retval  other code           Fatal error
*/

int Key_value_records_iterator::init(Mrr_ordered_index_reader *owner_arg)
{
  int res;
  owner= owner_arg;

  identical_key_it.init(owner->key_buffer);
  owner->key_buffer->setup_reading(owner->keypar.key_size_in_keybuf,
                                   owner->is_mrr_assoc ? sizeof(void*) : 0);

  if (identical_key_it.read())
    return HA_ERR_END_OF_FILE;

  uchar *key_in_buf= last_identical_key_ptr= identical_key_it.read_ptr1;

  uchar *index_tuple= key_in_buf;
  if (owner->keypar.use_key_pointers)
    memcpy(&index_tuple, key_in_buf, sizeof(char*));
  
  /* Check out how many more identical keys are following */
  while (!identical_key_it.read())
  {
    if (Mrr_ordered_index_reader::compare_keys(owner, key_in_buf, 
                                               identical_key_it.read_ptr1))
      break;
    last_identical_key_ptr= identical_key_it.read_ptr1;
  }
  identical_key_it.init(owner->key_buffer);
  res= owner->file->ha_index_read_map(owner->file->get_table()->record[0], 
                                      index_tuple, 
                                      owner->keypar.key_tuple_map, 
                                      HA_READ_KEY_EXACT);

  if (res)
  {
    /* Failed to find any matching records */
    move_to_next_key_value();
    return res;
  }
  owner->have_saved_rowid= FALSE;
  get_next_row= FALSE;
  return 0;
}


int Key_value_records_iterator::get_next(range_id_t *range_info)
{
  int res;

  if (get_next_row)
  {
    if (owner->keypar.index_ranges_unique)
    {
      /* We're using a full unique key, no point to call index_next_same */
      return HA_ERR_END_OF_FILE;
    }
    
    handler *h= owner->file;
    uchar *lookup_key;
    if (owner->keypar.use_key_pointers)
      memcpy(&lookup_key, identical_key_it.read_ptr1, sizeof(void*));
    else
      lookup_key= identical_key_it.read_ptr1;

    if ((res= h->ha_index_next_same(h->get_table()->record[0], 
                                    lookup_key,
                                    owner->keypar.key_tuple_length)))
    {
      /* It's either HA_ERR_END_OF_FILE or some other error */
      return res; 
    }
    identical_key_it.init(owner->key_buffer);
    owner->have_saved_rowid= FALSE;
    get_next_row= FALSE;
  }

  identical_key_it.read(); /* This gets us next range_id */
  memcpy(range_info, identical_key_it.read_ptr2, sizeof(range_id_t));

  if (!last_identical_key_ptr || 
      (identical_key_it.read_ptr1 == last_identical_key_ptr))
  {
    /* 
      We've reached the last of the identical keys that current record is a
      match for.  Set get_next_row=TRUE so that we read the next index record
      on the next call to this function.
    */
    get_next_row= TRUE;
  }
  return 0;
}


void Key_value_records_iterator::move_to_next_key_value()
{
  while (!owner->key_buffer->read() && 
         (owner->key_buffer->read_ptr1 != last_identical_key_ptr)) {}
}


/**
  DS-MRR implementation: multi_range_read_next() function.

  Calling convention is like multi_range_read_next() has.
*/

int DsMrr_impl::dsmrr_next(range_id_t *range_info)
{
  int res;
  if (strategy_exhausted)
    return HA_ERR_END_OF_FILE;

  while ((res= strategy->get_next(range_info)) == HA_ERR_END_OF_FILE)
  {
    if ((res= strategy->refill_buffer(FALSE)))
      break; /* EOF or error */
  }
  return res;
}


/**
  DS-MRR implementation: multi_range_read_info() function
*/
ha_rows DsMrr_impl::dsmrr_info(uint keyno, uint n_ranges, uint rows, 
                               uint key_parts,
                               uint *bufsz, uint *flags, Cost_estimate *cost)
{  
  ha_rows res __attribute__((unused));
  uint def_flags= *flags;
  uint def_bufsz= *bufsz;

  /* Get cost/flags/mem_usage of default MRR implementation */
  res= primary_file->handler::multi_range_read_info(keyno, n_ranges, rows,
                                                    key_parts, &def_bufsz, 
                                                    &def_flags, cost);
  DBUG_ASSERT(!res);

  if ((*flags & HA_MRR_USE_DEFAULT_IMPL) || 
      choose_mrr_impl(keyno, rows, flags, bufsz, cost))
  {
    /* Default implementation is chosen */
    DBUG_PRINT("info", ("Default MRR implementation chosen"));
    *flags= def_flags;
    *bufsz= def_bufsz;
  }
  else
  {
    /* *flags and *bufsz were set by choose_mrr_impl */
    DBUG_PRINT("info", ("DS-MRR implementation chosen"));
  }
  return 0;
}


/**
  DS-MRR Implementation: multi_range_read_info_const() function
*/

ha_rows DsMrr_impl::dsmrr_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                     void *seq_init_param, uint n_ranges,
                                     uint *bufsz, uint *flags, ha_rows limit,
                                     Cost_estimate *cost)
{
  ha_rows rows;
  uint def_flags= *flags;
  uint def_bufsz= *bufsz;
  /* Get cost/flags/mem_usage of default MRR implementation */
  rows= primary_file->handler::multi_range_read_info_const(keyno, seq, 
                                                           seq_init_param,
                                                           n_ranges, 
                                                           &def_bufsz, 
                                                           &def_flags,
                                                           limit,
                                                           cost);
  if (rows == HA_POS_ERROR)
  {
    /* Default implementation can't perform MRR scan => we can't either */
    return rows;
  }

  /*
    If HA_MRR_USE_DEFAULT_IMPL has been passed to us, that is an order to
    use the default MRR implementation (we need it for UPDATE/DELETE).
    Otherwise, make a choice based on cost and @@optimizer_switch settings
  */
  if ((*flags & HA_MRR_USE_DEFAULT_IMPL) ||
      choose_mrr_impl(keyno, rows, flags, bufsz, cost))
  {
    DBUG_PRINT("info", ("Default MRR implementation chosen"));
    *flags= def_flags;
    *bufsz= def_bufsz;
  }
  else
  {
    /* *flags and *bufsz were set by choose_mrr_impl */
    DBUG_PRINT("info", ("DS-MRR implementation chosen"));
  }
  return rows;
}


/**
  Check if key has partially-covered columns

  We can't use DS-MRR to perform range scans when the ranges are over
  partially-covered keys, because we'll not have full key part values
  (we'll have their prefixes from the index) and will not be able to check
  if we've reached the end the range.

  @param keyno  Key to check

  @todo
    Allow use of DS-MRR in cases where the index has partially-covered
    components but they are not used for scanning.

  @retval TRUE   Yes
  @retval FALSE  No
*/

bool key_uses_partial_cols(TABLE_SHARE *share, uint keyno)
{
  KEY_PART_INFO *kp= share->key_info[keyno].key_part;
  KEY_PART_INFO *kp_end= kp + share->key_info[keyno].user_defined_key_parts;
  for (; kp != kp_end; kp++)
  {
    if (!kp->field->part_of_key.is_set(keyno))
      return TRUE;
  }
  return FALSE;
}


/*
  Check if key/flags allow DS-MRR/CPK strategy to be used
  
  @param thd
  @param keyno      Index that will be used
  @param  mrr_flags  
  
  @retval TRUE   DS-MRR/CPK should be used
  @retval FALSE  Otherwise
*/

bool DsMrr_impl::check_cpk_scan(THD *thd, TABLE_SHARE *share, uint keyno, 
                                uint mrr_flags)
{
  return MY_TEST((mrr_flags & HA_MRR_SINGLE_POINT) &&
                 primary_file->is_clustering_key(keyno) &&
                 optimizer_flag(thd, OPTIMIZER_SWITCH_MRR_SORT_KEYS));
}


/*
  DS-MRR Internals: Choose between Default MRR implementation and DS-MRR

  Make the choice between using Default MRR implementation and DS-MRR.
  This function contains common functionality factored out of dsmrr_info()
  and dsmrr_info_const(). The function assumes that the default MRR
  implementation's applicability requirements are satisfied.

  @param keyno       Index number
  @param rows        E(full rows to be retrieved)
  @param flags  IN   MRR flags provided by the MRR user
                OUT  If DS-MRR is chosen, flags of DS-MRR implementation
                     else the value is not modified
  @param bufsz  IN   If DS-MRR is chosen, buffer use of DS-MRR implementation
                     else the value is not modified
  @param cost   IN   Cost of default MRR implementation
                OUT  If DS-MRR is chosen, cost of DS-MRR scan
                     else the value is not modified

  @retval TRUE   Default MRR implementation should be used
  @retval FALSE  DS-MRR implementation should be used
*/


bool DsMrr_impl::choose_mrr_impl(uint keyno, ha_rows rows, uint *flags,
                                 uint *bufsz, Cost_estimate *cost)
{
  Cost_estimate dsmrr_cost;
  bool res;
  THD *thd= primary_file->get_table()->in_use;
  TABLE_SHARE *share= primary_file->get_table_share();

  const bool mrr_on= hint_key_state(thd, table, keyno, MRR_HINT_ENUM,
                                    OPTIMIZER_SWITCH_MRR);
  const bool force_dsmrr_by_hints=
    hint_key_state(thd, table, keyno, MRR_HINT_ENUM, 0) ||
    hint_table_state(thd, table, BKA_HINT_ENUM, false);

  bool doing_cpk_scan= check_cpk_scan(thd, share, keyno, *flags); 
  bool using_cpk= primary_file->is_clustering_key(keyno);
  *flags &= ~HA_MRR_IMPLEMENTATION_FLAGS;
  if (!(mrr_on || force_dsmrr_by_hints) ||
      *flags & HA_MRR_INDEX_ONLY ||
      (using_cpk && !doing_cpk_scan) || key_uses_partial_cols(share, keyno))
  {
    /* Use the default implementation */
    *flags |= HA_MRR_USE_DEFAULT_IMPL;
    *flags &= ~HA_MRR_IMPLEMENTATION_FLAGS;
    return TRUE;
  }

  uint add_len= share->key_info[keyno].key_length + primary_file->ref_length; 
  if (get_disk_sweep_mrr_cost(keyno, rows, *flags, bufsz, add_len,
                              &dsmrr_cost))
    return TRUE;

  /* 
    If mrr_cost_based flag is not set, then set cost of DS-MRR to be minimum of
    DS-MRR and Default implementations cost. This allows one to force use of
    DS-MRR whenever it is applicable without affecting other cost-based
    choices. Note that if MRR or BKA hint is
    specified, DS-MRR will be used regardless of cost.
  */
  const bool force_dsmrr=
    (force_dsmrr_by_hints ||
     !optimizer_flag(thd, OPTIMIZER_SWITCH_MRR_COST_BASED));
  if (force_dsmrr && dsmrr_cost.total_cost() > cost->total_cost())
    dsmrr_cost= *cost;

  if (force_dsmrr || dsmrr_cost.total_cost() <= cost->total_cost())
  {
    *flags &= ~HA_MRR_USE_DEFAULT_IMPL;  /* Use the DS-MRR implementation */
    *flags &= ~HA_MRR_SORTED;          /* We will return unordered output */
    *cost= dsmrr_cost;
    res= FALSE;


    if ((using_cpk && doing_cpk_scan) ||
        (optimizer_flag(thd, OPTIMIZER_SWITCH_MRR_SORT_KEYS) &&
         *flags & HA_MRR_SINGLE_POINT))
    {
      *flags |= DSMRR_IMPL_SORT_KEYS;
    }
    
    if (!(using_cpk && doing_cpk_scan) &&
        !(*flags & HA_MRR_INDEX_ONLY))
    {
      *flags |= DSMRR_IMPL_SORT_ROWIDS;
    }
    /*
    if ((*flags & HA_MRR_SINGLE_POINT) && 
         optimizer_flag(thd, OPTIMIZER_SWITCH_MRR_SORT_KEYS))
      *flags |= HA_MRR_MATERIALIZED_KEYS;
    */
  }
  else
  {
    /* Use the default MRR implementation */
    res= TRUE;
  }
  return res;
}

/*
  Take the flags we've returned previously and print one of
  - Key-ordered scan
  - Rowid-ordered scan
  - Key-ordered Rowid-ordered scan
*/

int DsMrr_impl::dsmrr_explain_info(uint mrr_mode, char *str, size_t size)
{
  const char *key_ordered=   "Key-ordered scan";
  const char *rowid_ordered= "Rowid-ordered scan";
  const char *both_ordered=  "Key-ordered Rowid-ordered scan";
  const char *used_str="";
  const uint BOTH_FLAGS= (DSMRR_IMPL_SORT_KEYS | DSMRR_IMPL_SORT_ROWIDS);

  if (!(mrr_mode & HA_MRR_USE_DEFAULT_IMPL))
  {
    if ((mrr_mode & BOTH_FLAGS) == BOTH_FLAGS)
      used_str= both_ordered;
    else if (mrr_mode & DSMRR_IMPL_SORT_KEYS)
      used_str= key_ordered;
    else if (mrr_mode & DSMRR_IMPL_SORT_ROWIDS)
      used_str= rowid_ordered;

    size_t used_str_len= strlen(used_str);
    size_t copy_len= MY_MIN(used_str_len, size);
    memcpy(str, used_str, copy_len);
    return (int)copy_len;
  }
  return 0;
}


static void get_sort_and_sweep_cost(TABLE *table, ha_rows nrows,
                                    Cost_estimate *cost);


/**
  Get cost of DS-MRR scan

  @param keynr              Index to be used
  @param rows               E(Number of rows to be scanned)
  @param flags              Scan parameters (HA_MRR_* flags)
  @param buffer_size INOUT  Buffer size
                            IN: Buffer of size 0 means the function
                            will determine the best size and return it.
  @param extra_mem_overhead Extra memory overhead of the MRR implementation
                            (the function assumes this many bytes of buffer
                             space will not be usable by DS-MRR)
  @param cost        OUT    The cost

  @retval FALSE  OK
  @retval TRUE   Error, DS-MRR cannot be used (the buffer is too small
                 for even 1 rowid)
*/

bool DsMrr_impl::get_disk_sweep_mrr_cost(uint keynr, ha_rows rows, uint flags,
                                         uint *buffer_size,
                                         uint extra_mem_overhead,
                                         Cost_estimate *cost)
{
  ulong max_buff_entries, elem_size;
  ha_rows rows_in_full_step;
  ha_rows rows_in_last_step;
  uint n_full_steps;

  elem_size= primary_file->ref_length + 
             sizeof(void*) * (!MY_TEST(flags & HA_MRR_NO_ASSOCIATION));

  if (!*buffer_size)
  {
    /*
      We are requested to determine how much memory we need.
      Request memory to finish the scan in one pass but do not request
      more than @@mrr_buff_size.
    */
    *buffer_size= (uint) MY_MIN(extra_mem_overhead + elem_size*(ulong)rows,
                                MY_MAX(table->in_use->variables.mrr_buff_size,
                                       extra_mem_overhead));
  }

  if (elem_size + extra_mem_overhead > *buffer_size)
    return TRUE; /* Buffer has not enough space for even 1 rowid */

  max_buff_entries = (*buffer_size - extra_mem_overhead) / elem_size;

  /* Number of iterations we'll make with full buffer */
  n_full_steps= (uint)floor(rows2double(rows) / max_buff_entries);
  
  /* 
    Get numbers of rows we'll be processing in 
     - non-last sweep, with full buffer 
     - last iteration, with non-full buffer
  */
  rows_in_full_step= max_buff_entries;
  rows_in_last_step= rows % max_buff_entries;
  
  cost->reset(primary_file);

  /* Adjust buffer size if we expect to use only part of the buffer */
  if (n_full_steps)
  {
    get_sort_and_sweep_cost(table, rows_in_full_step, cost);
    cost->multiply(n_full_steps);
  }
  else
  {
    *buffer_size= ((uint) MY_MAX(*buffer_size,
                                 (size_t)(1.2*rows_in_last_step) * elem_size +
                                 primary_file->ref_length +
                                 table->key_info[keynr].key_length));
  }
  
  Cost_estimate last_step_cost;
  last_step_cost.avg_io_cost= cost->avg_io_cost;
  get_sort_and_sweep_cost(table, rows_in_last_step, &last_step_cost);
  cost->add(&last_step_cost);
 
  /* Total cost of all index accesses */
  cost->index_cost= primary_file->ha_keyread_and_copy_time(keynr, 1, rows, 0);
  cost->comp_cost= rows2double(rows) * primary_file->WHERE_COST;
  cost->setup_cost= primary_file->MULTI_RANGE_READ_SETUP_COST;
  return FALSE;
}


/* 
  Get cost of one sort-and-sweep step
  
  It consists of two parts:
   - sort an array of #nrows ROWIDs using qsort
   - read #nrows records from table in a sweep.

  @param table       Table being accessed
  @param nrows       Number of rows to be sorted and retrieved
  @param cost   OUT  The cost of scan
*/

static 
void get_sort_and_sweep_cost(TABLE *table, ha_rows nrows, Cost_estimate *cost)
{
  if (nrows)
  {
    get_sweep_read_cost(table, nrows, FALSE, cost);
    /* Add cost of qsort call: n * log2(n) * cost(rowid_comparison) */
    double cmp_op= rows2double(nrows) * ROWID_COMPARE_COST_THD(table->in_use);
    if (cmp_op < 3)
      cmp_op= 3;
    cost->cpu_cost += cmp_op * log2(cmp_op);
  }
}


/**
  Get cost of reading nrows table records in a "disk sweep"

  @param table             Table to be accessed
  @param nrows             Number of rows to retrieve
  @param interrupted       TRUE <=> Assume that the disk sweep will be
                           interrupted by other disk IO. FALSE - otherwise.
  @param cost         OUT  The cost.
*/

static void get_sweep_read_cost(TABLE *table, ha_rows nrows, bool interrupted,
                                Cost_estimate *cost)
{
  DBUG_ENTER("get_sweep_read_cost");

#ifndef OLD_SWEEP_COST
  cost->row_cost= table->file->ha_rnd_pos_call_time(nrows);
#else
  if (table->file->pk_is_clustering_key(table->s->primary_key))
  {
    cost->cpu_cost= table->file->ha_read_and_copy_time(table->s->primary_key,
                                                       (uint) nrows, nrows);
  }
  else if ((cost->avg_io_cost= table->file->avg_io_cost()) >= 0.999)
  {
    double n_blocks=
      ceil(ulonglong2double(table->file->stats.data_file_length) / IO_SIZE);
    double busy_blocks=
      n_blocks * (1.0 - pow(1.0 - 1.0/n_blocks, rows2double(nrows)));
    if (busy_blocks < 1.0)
      busy_blocks= 1.0;

    DBUG_PRINT("info",("sweep: nblocks=%g, busy_blocks=%g", n_blocks,
                       busy_blocks));
    cost->io_count= busy_blocks;

    if (!interrupted)
    {
      /* Assume reading is done in one 'sweep' */
      cost->avg_io_cost= (DISK_SEEK_BASE_COST +
                          DISK_SEEK_PROP_COST*n_blocks/busy_blocks);
    }
  }
  cost->cpu_cost+= rows2double(n_rows) * ROW_COPY_COST;
#endif
  DBUG_PRINT("info",("returning cost: %g", cost->total_cost()));
  DBUG_VOID_RETURN;
}


/* **************************************************************************
 * DS-MRR implementation ends
 ***************************************************************************/
