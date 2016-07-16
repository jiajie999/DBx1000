#include "txn.h"
#include "row.h"
#include "wl.h"
#include "ycsb.h"
#include "thread.h"
#include "mem_alloc.h"
#include "occ.h"
#include "table.h"
#include "catalog.h"
#include "index_btree.h"
#include "index_hash.h"
#include "log.h"
#include "parallel_log.h"

void txn_man::init(thread_t * h_thd, workload * h_wl, uint64_t thd_id) {
	this->h_thd = h_thd;
	this->h_wl = h_wl;
	pthread_mutex_init(&txn_lock, NULL);
	lock_ready = false;
	ready_part = 0;
	row_cnt = 0;
	pred_size = 0;
	wr_cnt = 0;
	insert_cnt = 0;
	accesses = (Access **) _mm_malloc(sizeof(Access *) * MAX_ROW_PER_TXN, 64);
	for (int i = 0; i < MAX_ROW_PER_TXN; i++)
		accesses[i] = NULL;
	num_accesses_alloc = 0;
#if CC_ALG == TICTOC || CC_ALG == SILO
	_pre_abort = g_pre_abort; 
 	// XXX XXX
	_validation_no_wait = true;
/*	if (g_validation_lock == "no-wait")
		_validation_no_wait = true;
	else if (g_validation_lock == "waiting")
		_validation_no_wait = false;
	else 
		assert(false);
		*/
#endif
#if CC_ALG == TICTOC
	_max_wts = 0;
	// XXX XXX 
	//_write_copy_ptr = (g_write_copy_form == "ptr");
	_write_copy_ptr = false; //(g_write_copy_form == "ptr");
	_atomic_timestamp = g_atomic_timestamp;
#elif CC_ALG == SILO
	_cur_tid = 0;
#endif

#if LOG_REDO && LOG_ALGORITHM == LOG_PARALLEL
	_predecessors = new uint64_t[MAX_ROW_PER_TXN];
#endif
}

void txn_man::set_txn_id(txnid_t txn_id) {
	this->txn_id = txn_id;
}

txnid_t txn_man::get_txn_id() {
	return this->txn_id;
}

workload * txn_man::get_wl() {
	return h_wl;
}

uint64_t txn_man::get_thd_id() {
	return h_thd->get_thd_id();
}

void txn_man::set_ts(ts_t timestamp) {
	this->timestamp = timestamp;
}

ts_t txn_man::get_ts() {
	return this->timestamp;
}

void txn_man::cleanup(RC rc) {
#if CC_ALG == HEKATON
	row_cnt = 0;
	wr_cnt = 0;
	insert_cnt = 0;
	return;
#endif
	for (int rid = row_cnt - 1; rid >= 0; rid --) {
		row_t * orig_r = accesses[rid]->orig_row;
		access_t type = accesses[rid]->type;
		if (type == WR && rc == Abort)
			type = XP;

#if (CC_ALG == NO_WAIT || CC_ALG == DL_DETECT) && ISOLATION_LEVEL == REPEATABLE_READ
		if (type == RD) {
			accesses[rid]->data = NULL;
			continue;
		}
#endif

		if (ROLL_BACK && type == XP &&
					(CC_ALG == DL_DETECT || 
					CC_ALG == NO_WAIT || 
					CC_ALG == WAIT_DIE)) 
		{
			orig_r->return_row(type, this, accesses[rid]->orig_data);
		} else {
			orig_r->return_row(type, this, accesses[rid]->data);
		}
#if CC_ALG != TICTOC && CC_ALG != SILO
		accesses[rid]->data = NULL;
#endif
	}

	if (rc == Abort) {
		for (UInt32 i = 0; i < insert_cnt; i ++) {
			row_t * row = insert_rows[i];
			assert(g_part_alloc == false);
#if CC_ALG != HSTORE && CC_ALG != OCC
			mem_allocator.free(row->manager, 0);
#endif
			row->free_row();
			mem_allocator.free(row, sizeof(row));
		}
	}
	// Logging
#if LOG_REDO
	if (rc == RCOK)
	{
        if (wr_cnt > 0) {
			uint64_t before_log_time = get_sys_clock();
			uint32_t size = get_log_entry_size();			
			char entry[size];// = NULL;
			create_log_entry(entry);
			// call log_manager to log the entry.
			// TODO for parallel logging, _predecessors stores the last writers.  
#if LOG_ALGORITHM == LOG_SERIAL
			log_manager.logTxn(entry, size);
#elif LOG_ALGORITHM == LOG_PARALLEL
			log_manager.parallelLogTxn(entry, size, _predecessors, pred_size, get_txn_id(), get_thd_id());
#endif
			uint64_t after_log_time = get_sys_clock();
			INC_STATS(get_thd_id(), time_log, after_log_time - before_log_time);
		}
	}
#endif
	row_cnt = 0;
	pred_size = 0;
	wr_cnt = 0;
	insert_cnt = 0;
#if CC_ALG == DL_DETECT
	dl_detector.clear_dep(get_txn_id());
#endif
}

row_t * txn_man::get_row(row_t * row, access_t type) {
	if (CC_ALG == HSTORE)
		return row;
	uint64_t starttime = get_sys_clock();
	RC rc = RCOK;
	if (accesses[row_cnt] == NULL) {
		Access * access = (Access *) _mm_malloc(sizeof(Access), 64);
		accesses[row_cnt] = access;
#if (CC_ALG == SILO || CC_ALG == TICTOC)
		access->data = (row_t *) _mm_malloc(sizeof(row_t), 64);
		access->data->init(MAX_TUPLE_SIZE);
		access->orig_data = (row_t *) _mm_malloc(sizeof(row_t), 64);
		access->orig_data->init(MAX_TUPLE_SIZE);
#elif (CC_ALG == DL_DETECT || CC_ALG == NO_WAIT || CC_ALG == WAIT_DIE)
		access->orig_data = (row_t *) _mm_malloc(sizeof(row_t), 64);
		access->orig_data->init(MAX_TUPLE_SIZE);
#endif
		num_accesses_alloc ++;
	}
	
	rc = row->get_row(type, this, accesses[ row_cnt ]->data);
#if LOG_REDO && LOG_ALGORITHM == LOG_PARALLEL
	bool found = false;
	for (int i = 0; i < pred_size; i ++ )  
		if (_predecessors[pred_size] == accesses[ row_cnt ]->data->get_last_writer())
			found = true;
	if (!found)
		_predecessors[pred_size ++] = accesses[ row_cnt ]->data->get_last_writer();
	//printf("pred = %ld\n", _predecessors[row_cnt]);
#endif
	if (rc == Abort) {
		return NULL;
	}
	accesses[row_cnt]->type = type;
	accesses[row_cnt]->orig_row = row;
#if CC_ALG == TICTOC
	accesses[row_cnt]->wts = last_wts;
	accesses[row_cnt]->rts = last_rts;
#elif CC_ALG == SILO
	accesses[row_cnt]->tid = last_tid;
#elif CC_ALG == HEKATON
	accesses[row_cnt]->history_entry = history_entry;
#endif

#if ROLL_BACK && (CC_ALG == DL_DETECT || CC_ALG == NO_WAIT || CC_ALG == WAIT_DIE)
	if (type == WR) {
		accesses[row_cnt]->orig_data->table = row->get_table();
		accesses[row_cnt]->orig_data->copy(row);
	}
#endif

#if (CC_ALG == NO_WAIT || CC_ALG == DL_DETECT) && ISOLATION_LEVEL == REPEATABLE_READ
	if (type == RD)
		row->return_row(type, this, accesses[ row_cnt ]->data);
#endif
	
	row_cnt ++;
	if (type == WR)
		wr_cnt ++;

	uint64_t timespan = get_sys_clock() - starttime;
	INC_TMP_STATS(get_thd_id(), time_man, timespan);
	return accesses[row_cnt - 1]->data;
}

void txn_man::insert_row(row_t * row, table_t * table) {
	if (CC_ALG == HSTORE)
		return;
	assert(insert_cnt < MAX_ROW_PER_TXN);
	insert_rows[insert_cnt ++] = row;
}

itemid_t *
txn_man::index_read(INDEX * index, idx_key_t key, int part_id) {
	//uint64_t starttime = get_sys_clock();
	itemid_t * item;
	index->index_read(key, item, part_id, get_thd_id());
	//INC_TMP_STATS(get_thd_id(), time_index, get_sys_clock() - starttime);
	return item;
}

void 
txn_man::index_read(INDEX * index, idx_key_t key, int part_id, itemid_t *& item) {
	uint64_t starttime = get_sys_clock();
	index->index_read(key, item, part_id, get_thd_id());
	INC_TMP_STATS(get_thd_id(), time_index, get_sys_clock() - starttime);
}

RC txn_man::finish(RC rc) {
#if CC_ALG == HSTORE
	return RCOK;
#endif
	uint64_t starttime = get_sys_clock();
#if CC_ALG == OCC
	if (rc == RCOK)
		rc = occ_man.validate(this);
	else 
		cleanup(rc);
#elif CC_ALG == TICTOC
	if (rc == RCOK)
		rc = validate_tictoc();
	else 
		cleanup(rc);
#elif CC_ALG == SILO
	if (rc == RCOK)
		rc = validate_silo();
	else 
		cleanup(rc);
#elif CC_ALG == HEKATON
	rc = validate_hekaton(rc);
	cleanup(rc);
#else 
	cleanup(rc);
#endif
	uint64_t timespan = get_sys_clock() - starttime;
	INC_TMP_STATS(get_thd_id(), time_man,  timespan);
	INC_STATS(get_thd_id(), time_cleanup,  timespan);
	return rc;
}

void
txn_man::release() {
	for (int i = 0; i < num_accesses_alloc; i++)
		mem_allocator.free(accesses[i], 0);
	mem_allocator.free(accesses, 0);
}

void 
txn_man::recover() {
	/*
	// call readFromLog()
	uint32_t num_keys;
	string * table_names;
	uint64_t * keys;
	uint32_t * lengths;
	char ** after_images;
	uint64_t starttime = get_sys_clock();
    uint64_t num_records = 0;
	//ycsb_wl * wl = (ycsb_wl *) h_wl;
	while (log_manager.readFromLog(num_keys, table_names, keys, lengths, after_images))
	{
        num_records ++;
        assert(num_keys > 0); 
		// update the database using these information.
		// Here is the (pseudo) code:
		//
		// for each key in keys	:
		// for (uint32_t i = 0; i < num_keys; i++) {
		//   // Find the row using the key.
		//   itemid_t * m_item = index_read(wl->the_index, keys[i], 0);
		//   row_t * row = ((row_t *)m_item->location);
		//   char * data = row->get_data();
		//   memcpy(data, after_images[i], lengths[i]);
		// }
	}
	uint64_t timespan = get_sys_clock() - starttime;
    INC_STATS(get_thd_id(), txn_cnt, num_records);
	INC_STATS(get_thd_id(), time_man, timespan);
	*/
}

uint32_t
txn_man::get_log_entry_size()
{
  uint32_t buffsize = 0;
  buffsize += sizeof(txn_id) + sizeof(wr_cnt);
  // for table names
  // TODO. right now, only store tableID
  buffsize += sizeof(uint32_t) * wr_cnt;
  // for keys
  buffsize += sizeof(uint64_t) * wr_cnt;
  // for data length
  buffsize += sizeof(uint32_t) * wr_cnt; 
  // for data
  for (int i=0; i < wr_cnt; i++)
    buffsize += accesses[i]->orig_row->get_tuple_size();
  return buffsize;  
}

void 
txn_man::create_log_entry(char * entry)
{
  uint32_t offset = 0;
  memcpy(entry + offset, &txn_id, sizeof(txn_id));
  offset += sizeof(txn_id);
  memcpy(entry + offset, &wr_cnt, sizeof(wr_cnt));
  offset += sizeof(wr_cnt);
  // table IDs
  for(int j = 0; j < wr_cnt; j++) { 
    // TODO all tables have ID = 0
    uint32_t table_id = 0;
    memcpy(entry + offset, &table_id, sizeof(table_id));
    offset += sizeof(table_id);
  }
  // keys
  for (int j=0; j < wr_cnt; j++)
  {
	uint64_t key = accesses[j]->orig_row->get_primary_key();
    memcpy(entry + offset, &key, sizeof(key));
    offset += sizeof(key);
  }
  for (int j=0; j < wr_cnt; j++)
  {
    uint32_t length = accesses[j]->orig_row->get_tuple_size();
    memcpy(entry + offset, &length, sizeof(length));
    offset += sizeof(length);
  }
  for (int j=0; j < wr_cnt; j++)
  {
	char * data = accesses[j]->data->get_data();
    uint32_t length = accesses[j]->orig_row->get_tuple_size();
    memcpy(entry + offset, data, length);
    offset += length;
  }
  //M_ASSERT(offset == buffsize, "offset=%d, buffsize=%d\n", offset, buffsize);
}

