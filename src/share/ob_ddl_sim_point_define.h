/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

// DDL_SIM_POINT_DEFINE accept parameters like: (type, name, id, desc, action, args...)
//
// Available actions:
//  RET_ERR(ret_code, ...) // return fixed or random ret_code, execute once
//  N_RET_ERR(max_repeat_times, ret_code, ...) // return fixed or random ret_code, execute multi times
//  SLEEP_MS(min_sleep_ms, [max_sleep_ms]) // sleep random time between min_sleep_ms and max_sleep_ms, execute once
//  N_SLEEP_MS(max_repeat_times, min_sleep_ms, [max_sleep_ms]) // sleep random time between min_sleep_ms and max_sleep_ms, execute multi times
//
// Examples:
// DDL_SIM_POINT_DEFINE(DDL_SIM_POINT_EXAMPLE_1, 1, "return fixed ret_code once", RET_ERR(OB_TASK_EXPIRED))
// DDL_SIM_POINT_DEFINE(DDL_SIM_POINT_EXAMPLE_2, 2, "return random ret_code once", RET_ERR(OB_NOT_MASTER, OB_EAGAIN, OB_INVALID_ARGUMENT, OB_NOT_INIT))
// DDL_SIM_POINT_DEFINE(DDL_SIM_POINT_EXAMPLE_3, 3, "return fixed ret_code for 3 times", N_RET_ERR(3, OB_EAGAIN))
// DDL_SIM_POINT_DEFINE(DDL_SIM_POINT_EXAMPLE_4, 4, "return random ret_code for 5 times", N_RET_ERR(5, OB_NOT_MASTER, OB_EAGAIN, OB_INVALID_ARGUMENT, OB_NOT_INIT))
// DDL_SIM_POINT_DEFINE(DDL_SIM_POINT_EXAMPLE_5, 5, "sleep 10ms once", SLEEP_MS(10))
// DDL_SIM_POINT_DEFINE(DDL_SIM_POINT_EXAMPLE_6, 6, "random sleep 5-10ms once", SLEEP_MS(5, 10))
// DDL_SIM_POINT_DEFINE(DDL_SIM_POINT_EXAMPLE_7, 7, "sleep 10ms, execute 2 times", N_SLEEP_MS(2, 10))
// DDL_SIM_POINT_DEFINE(DDL_SIM_POINT_EXAMPLE_8, 8, "random sleep 5-10ms, execute 3 times", N_SLEEP_MS(3, 5, 10))
#ifdef DDL_SIM_POINT_DEFINE
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, SCHEDULE_DDL_TASK_FAILED, 1, "schedule ddl task failed, rely on recover thread", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, INSERT_CHILD_DDL_TASK_RECORD_EXIST, 2, "insert ddl task record, but the record already exist", RET_ERR(OB_ENTRY_EXIST))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, ON_COLUMN_CHECKSUM_REPLY_FAILED, 3, "receive column checksum reply, but process failed", RET_ERR(OB_ENTRY_NOT_EXIST))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, UPDATE_COMPLETE_SSTABLE_FAILED, 4, "update data complement failed", RET_ERR(OB_ENTRY_NOT_EXIST, OB_TASK_EXPIRED))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REPORT_DDL_CHECKSUM_FAILED, 5, "report ddl checksum failed", N_RET_ERR(5, OB_NOT_MASTER, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, CHECK_SCHEMA_TRANS_END_SLOW, 6, "check schema trans end execute slow", N_SLEEP_MS(1000, 100, 200))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, WRITE_DUPLICATED_DDL_REDO_LOG, 7, "write duplicated ddl redo logs", RET_ERR(OB_NOT_MASTER))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, PUSH_TASK_INTO_QUEUE_FAILED, 8, "push task into queue failed", RET_ERR(OB_STATE_NOT_MATCH, OB_ENTRY_EXIST))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REMOVE_TASK_FROM_QUEUE_FAILED, 9, "remove task from queue failed", RET_ERR(OB_STATE_NOT_MATCH, OB_ENTRY_NOT_EXIST))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, GET_TASK_FROM_QUEUE_FAILED, 10, "get task from queue failed", RET_ERR(OB_STATE_NOT_MATCH, OB_ENTRY_NOT_EXIST))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, CANCEL_SYS_TASK_FAILED, 11, "cancel sys task failed", RET_ERR(OB_ENTRY_NOT_EXIST))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TABLE_UPDATE_TASK_INFO_FAILED, 12, "redef task update task info failed", RET_ERR(OB_EAGAIN, OB_ENTRY_NOT_EXIST))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TABLE_ABORT_FAILED, 13, "redef task abort failed", RET_ERR(OB_EAGAIN, OB_ENTRY_NOT_EXIST))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TABLE_COPY_DEPES_FAILED, 14, "redef task copy deps failed", RET_ERR(OB_EAGAIN, OB_ENTRY_NOT_EXIST))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TABLE_FINISH_FAILED, 15, "redef task finish failed", RET_ERR(OB_EAGAIN, OB_ENTRY_NOT_EXIST))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, HEART_BEAT_UPDATE_ACTIVE_TIME, 16, "heart beat mgr update task active time", RET_ERR(OB_ALLOCATE_MEMORY_FAILED))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_SCHEDULER_STOPPED, 17, "ddl scheduler stopped", RET_ERR(OB_NOT_RUNNING))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_SCHEDULER_ADD_SYS_TASK_FAILED, 18, "ddl scheduler add sys task failed", RET_ERR(OB_ENTRY_EXIST))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_SCHEDULER_REMOVE_SYS_TASK_FAILED, 19, "ddl scheduler remove sys task failed", RET_ERR(OB_ENTRY_NOT_EXIST))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, TASK_STATUS_OPERATOR_SLOW, 20, "ddl task status query or modify slow", N_SLEEP_MS(100, 1000))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, GET_FREEZE_INFO_FAILED, 21, "get freeze info failed", RET_ERR(OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, CHECK_TRANS_END_FAILED, 22, "check trans end failed", RET_ERR(OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, CHECK_TENANT_STANDBY_FAILED, 23, "check tenant standby failed", RET_ERR(OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REPORT_DDL_RET_CODE_FAILED, 24, "report ddl ret code failed", RET_ERR(OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, BATCH_RELEASE_SNAPSHOT_FAILED, 25, "release snapshot failed", N_RET_ERR(10, OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, QUERY_SQL_PLAN_MONITOR_SLOW, 26, "query sql plan monitor slow", N_SLEEP_MS(100, 1000))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, CALC_COLUMN_CHECKSUM_RPC_SLOW, 27, "calculate column checksum rpc slow", N_SLEEP_MS(1000, 100, 200))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, CHECK_MODIFY_TIME_ELAPSED_SLOW, 28, "check modify time elapsed rpc slow", N_SLEEP_MS(1000, 100, 200))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, CREATE_INDEX_BUILD_SSTABLE_FAILED, 29, "create index build sstable failed", N_RET_ERR(10, OB_REPLICA_NOT_READABLE, OB_ERR_INSUFFICIENT_PX_WORKER))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, PROCESS_COLUMN_CHECKSUM_RESPONSE_SLOW, 30, "process column checksum response slow", N_SLEEP_MS(1000, 100, 200))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, PROCESS_BUILD_SSTABLE_RESPONSE_SLOW, 31, "process build sstable response slow", N_SLEEP_MS(1000, 100, 200))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_TASK_HOLD_SNAPSHOT_FAILED, 32, "ddl task hold snapshot failed", N_RET_ERR(5, OB_TIMEOUT, OB_SNAPSHOT_DISCARDED))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, CHECK_OLD_COMPLEMENT_TASK_FAILED, 33, "check old complement task failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_TASK_RELEASE_SNAPSHOT_FAILED, 34, "create index relase snapshot failed", N_RET_ERR(5, OB_EAGAIN, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_TASK_COLLECT_LONGOPS_STAT_FAILED, 35, "collect longops stat failed", N_RET_ERR(5, OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, PROCESS_CHILD_TASK_FINISH_FAILED, 36, "process child task finish failed", RET_ERR(OB_ALLOCATE_MEMORY_FAILED))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, UPDATE_INDEX_STATUS_FAILED, 37, "update index status failed", RET_ERR(OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DROP_INDEX_RPC_FAILED, 38, "drop index rpc failed", RET_ERR(OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REFRESH_SCHEMA_VERSION_FAILED, 39, "refresh schema version failed", RET_ERR(OB_SCHEMA_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, SINGLE_REPLICA_EXECUTOR_BUILD_FAILED, 40, "single replica executor build failed", RET_ERR(OB_ALLOCATE_MEMORY_FAILED))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, SINGLE_REPLICA_EXECUTOR_SCHEDULE_TASK_FAILED, 41, "single replica executor schedule task failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_SSTABLE_BULD_TASK_INIT_FAILED, 42, "redef sstable build task init failed", RET_ERR(OB_EAGAIN, OB_ALLOCATE_MEMORY_FAILED))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_SSTABLE_BULD_TASK_PROCESS_FAILED, 43, "redef sstable build task process failed", RET_ERR(OB_ALLOCATE_MEMORY_FAILED))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REAP_OLD_REPLICA_BUILD_TASK_FAILED, 44, "reap old replica build task failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, LOCK_TABLE_FAILED, 45, "lock table failed", N_RET_ERR(5, OB_EAGAIN, OB_TIMEOUT, OB_NOT_MASTER))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, UNLOCK_TABLE_FAILED, 46, "unlock table failed", N_RET_ERR(5, OB_EAGAIN, OB_TIMEOUT, OB_NOT_MASTER))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, BUILD_REPLICA_ASYNC_TASK_FAILED, 47, "build replica async t task failed", RET_ERR(OB_EAGAIN, OB_NOT_MASTER))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TASK_CHECK_TABLE_EMPTY_FAILED, 48, "redef task check table empty failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TASK_GET_CHECKSUM_COLUMNS_FAILED, 49, "redef task get checksum columns failed", RET_ERR(OB_EAGAIN))

DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, ADD_CONSTRAINT_DDL_TASK_FAILED, 52, "add constraint ddl task failed", RET_ERR(OB_EAGAIN, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, ADD_FOREIGN_KEY_DDL_TASK_FAILED, 53, "add foreign key ddl task failed", RET_ERR(OB_EAGAIN, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, SYNC_AUTOINC_POSITION_FAILED, 54, "sync auto inc position failed", RET_ERR(OB_EAGAIN, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, MODIFY_AUTOINC_FAILED, 55, "redef task modify auto inc failed", RET_ERR(OB_EAGAIN, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TASK_FINISH_FAILED, 56, "redef task finish failed", N_RET_ERR(10, OB_EAGAIN, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TASK_CHECK_HEALTH_FAILED, 57, "redef task check health failed", N_RET_ERR(5, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TASK_SYNC_STATS_INFO_FAILED, 58, "redef task sync stats info failed", N_RET_ERR(5, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TASK_SYNC_TABLET_AUTOINC_SEQ_FAILED, 59, "redef task sync tablet auto inc sequence failed", RET_ERR(OB_TIMEOUT, OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TASK_CHECK_REBUILD_CONSTRAINT_FAILED, 60, "redef task check rebuild constraint failed", RET_ERR(OB_ALLOCATE_MEMORY_FAILED, OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TASK_GET_ALL_TABLET_COUNT_FAILED, 61, "redef task get all tablet count failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_TASK_INIT_BY_RECORD_FAILED, 62, "ddl task init by record failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_TASK_ENCODE_MESSAGE_FAILED, 63, "ddl task encode message failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_TASK_DECODE_MESSAGE_FAILED, 64, "ddl task decode message failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, RETRY_TASK_UPDATE_BY_CHILD_FAILED, 65, "retry task update by child failed", RET_ERR(OB_STATE_NOT_MATCH, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, UPDATE_TASK_RECORD_ON_TASK_STATUS_FAILED, 66, "update task record on task status failed", N_RET_ERR(10, OB_TIMEOUT, OB_TRY_LOCK_ROW_CONFLICT, OB_TRANSACTION_SET_VIOLATION))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, UPDATE_TASK_RECORD_ON_SNAPSHOT_VERSION_FAILED, 67, "update task record on snapshot version failed", N_RET_ERR(10, OB_TIMEOUT, OB_TRY_LOCK_ROW_CONFLICT, OB_TRANSACTION_SET_VIOLATION))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, UPDATE_TASK_RECORD_ON_RET_CODE_FAILED, 68, "update task record on ret code failed", N_RET_ERR(10, OB_TIMEOUT, OB_TRY_LOCK_ROW_CONFLICT, OB_TRANSACTION_SET_VIOLATION))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, UPDATE_TASK_RECORD_ON_EXECUTION_ID_FAILED, 69, "update task record on execution id failed", N_RET_ERR(10, OB_TIMEOUT, OB_TRY_LOCK_ROW_CONFLICT, OB_TRANSACTION_SET_VIOLATION))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, UPDATE_TASK_RECORD_ON_MESSAGE_FAILED, 70, "update task record on message failed", N_RET_ERR(10, OB_TIMEOUT, OB_TRY_LOCK_ROW_CONFLICT, OB_TRANSACTION_SET_VIOLATION))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, UPDATE_TASK_RECORD_ON_STATUS_AND_MESSAGE_FAILED, 71, "update task record on status and message failed", N_RET_ERR(10, OB_TIMEOUT, OB_TRY_LOCK_ROW_CONFLICT, OB_TRANSACTION_SET_VIOLATION))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DELETE_TASK_RECORD_FAILED, 72, "delete task record failed", N_RET_ERR(10, OB_TIMEOUT, OB_TRY_LOCK_ROW_CONFLICT, OB_TRANSACTION_SET_VIOLATION))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, QUERY_TASK_RECORD_CHECK_CONFLICT_DDL_FAILED, 73, "query task record check conflict ddl failed", N_RET_ERR(10, OB_TIMEOUT, OB_TRY_LOCK_ROW_CONFLICT, OB_TRANSACTION_SET_VIOLATION))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, SELECT_TASK_RECORD_FOR_UPDATE_FAILED, 74, "select task record for update failed", N_RET_ERR(10, OB_TIMEOUT, OB_TRY_LOCK_ROW_CONFLICT, OB_TRANSACTION_SET_VIOLATION))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, KILL_TASK_BY_INNER_SQL_FAILED, 75, "kill task by inner sql failed", N_RET_ERR(10, OB_TIMEOUT, OB_TRY_LOCK_ROW_CONFLICT, OB_TRANSACTION_SET_VIOLATION))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, RETRY_TASK_DROP_SCHEMA_FAILED, 76, "rety task drop schema failed", N_RET_ERR(10, OB_TIMEOUT, OB_TRY_LOCK_ROW_CONFLICT, OB_TRANSACTION_SET_VIOLATION))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, RETRY_TASK_WAIT_ALTER_TABLE_FAILED, 77, "rety task wait alter table failed", N_RET_ERR(5, OB_TIMEOUT, OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, RETRY_TASK_CHECK_SCHEMA_CHANGED_FAILED, 78, "rety task check schema changed failed", N_RET_ERR(5, OB_TIMEOUT, OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, RETRY_TASK_CHECK_SCHEMA_CHANGED_SLOW, 79, "rety task check schema changed slow", N_SLEEP_MS(5, 1000, 2000))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_REDEF_TASK_CHECK_COLUMN_CHECKSUM_FAILED, 80, "ddl redef task check column checksum slow", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_TASK_SEND_BUILD_REPLICA_REQUEST_FAILED, 81, "ddl task send build replica failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, TABLE_REDEF_TASK_CHECK_USE_HEAP_PLAN_FAILED, 82, "table redef task use heap plan failed", RET_ERR(OB_EAGAIN))

DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TASK_COPY_INDEX_FAILED, 84, "table redef task copy index failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TASK_COPY_CONSTRAINT_FAILED, 85, "table redef task copy constraint failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TASK_COPY_FOREIGN_KEY_FAILED, 86, "table redef task copy foreign key failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, REDEF_TASK_COPY_DEPENDENT_OBJECTS_FAILED, 87, "redef task copy dependent object failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_TASK_TAKE_EFFECT_FAILED, 88, "ddl task take effect failed", N_RET_ERR(5, OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, TABLE_REDEF_TASK_REPENDING_FAILED, 89, "table redef task repending failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, TABLE_REDEF_TASK_GET_DIRECT_LOAD_JOB_STAT_FAILED, 90, "table redef task get direct load job stat failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, TABLE_REDEF_TASK_GET_DIRECT_LOAD_JOB_STAT_SLOW, 91, "table redef task get direct load stat slow", N_SLEEP_MS(10, 1000, 2000))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, CONSTRAINT_TASK_SET_VALIDATED, 92, "constraint task set validated failed", N_RET_ERR(5, OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, CONSTRAINT_TASK_ROLL_BACK_SCHEMA, 93, "constraint task rollback schema failed", N_RET_ERR(5, OB_EAGAIN, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, VALIDATE_CONSTRAINT_OR_FOREIGN_KEY_TASK_FAILED, 94, "check constraint valid task failed", N_RET_ERR(5, OB_EAGAIN, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, UPDATE_AUTOINC_SEQUENCE_FAILED, 95, "update auto inc sequence failed", N_RET_ERR(5, OB_EAGAIN, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_ERR_MESSAGE_OPERATOR_SLOW, 96, "ddl error message operator slow", N_SLEEP_MS(10, 1000, 2000))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_ERR_MESSAGE_OPERATOR_REPORT_FAILED, 97, "ddl error message operator report failed", N_RET_ERR(5, OB_EAGAIN, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_ERR_MESSAGE_OPERATOR_LOAD_FAILED, 98, "ddl error message operator load failed", N_RET_ERR(5, OB_EAGAIN, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_ERR_MESSAGE_OPERATOR_GENERATE_FAILED, 99, "ddl error message operator generate message failed", N_RET_ERR(5, OB_EAGAIN, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, GENERATE_BUILD_REPLICA_SQL, 100, "generate build replica sql failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, GET_DATA_FORMAT_VERISON_FAILED, 101, "get data format version failed", N_RET_ERR(10, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, CHECK_TABLET_CHECKSUM_STATUS_FAILED, 102, "check tablet checksum status failed", N_RET_ERR(10, OB_EAGAIN, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, CHECK_TABLET_CHECKSUM_STATUS_SLOW, 103, "check tablet checksum status slow", N_SLEEP_MS(10, 1000, 2000))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, UPDATE_DDL_CHECKSUM_FAILED, 104, "update ddl checksum failed", N_RET_ERR(10, OB_TRY_LOCK_ROW_CONFLICT, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, UPDATE_DDL_CHECKSUM_SLOW, 105, "update ddl checksum slow", N_SLEEP_MS(10, 1000, 2000))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, GET_TABLE_COLUMN_CHECKSUM_FAILED, 106, "get table column checksum failed", N_RET_ERR(10, OB_EAGAIN, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, GET_TABLE_COLUMN_CHECKSUM_SLOW, 107, "get table column slow", N_SLEEP_MS(10, 1000, 2000))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, GET_TABLET_COLUMN_CHECKSUM_FAILED, 108, "get tablet column checksum failed", N_RET_ERR(10, OB_EAGAIN, OB_TIMEOUT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, GET_TABLET_COLUMN_CHECKSUM_SLOW, 109, "get tablet column slow", N_SLEEP_MS(10, 1000, 2000))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DELETE_DDL_CHECKSUM_FAILED, 110, "delete ddl checksum failed", N_RET_ERR(10, OB_EAGAIN, OB_NOT_MASTER, OB_TRY_LOCK_ROW_CONFLICT))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DELETE_DDL_CHECKSUM_SLOW, 111, "delete ddl checksum slow", N_SLEEP_MS(10, 1000, 2000))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, UNIQUE_INDEX_CHECKER_SCAN_TABLE_WITH_CHECKSUM_FAILED, 112, "unique index checker scan table with checksum failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, UNIQUE_INDEX_CHECKER_GENERATE_INDEX_OUTPUT_PARAM_FAILED, 113, "unique index checker generate index output param failed", RET_ERR(OB_ALLOCATE_MEMORY_FAILED))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, UNIQUE_INDEX_CHECKER_WAIT_TRANS_END_FAILED, 114, "unique index checker wait trans end failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, CREATE_HIDDEN_TABLE_RPC_FAILED , 115, "create hidden table rpc failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, CREATE_HIDDEN_TABLE_RPC_SLOW , 116, "create hidden table rpc slow", N_SLEEP_MS(10, 1000, 2000))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, COPY_TABLE_DEPENDENTS_RPC_FAILED, 117, "copy table dependents rpc failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, COPY_TABLE_DEPENDENTS_RPC_SLOW, 118, "copy table dependents rpc slow", N_SLEEP_MS(10, 1000, 2000))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, FINISH_REDEF_TABLE_RPC_FAILED, 119, "finish redef table rpc failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, FINISH_REDEF_TABLE_RPC_SLOW, 120, "finish redef table rpc slow", N_SLEEP_MS(10, 1000, 2000))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, ABORT_REDEF_TABLE_RPC_FAILED, 121, "abort redef table rpc failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, ABORT_REDEF_TABLE_RPC_SLOW, 122, "abort redef table rpc slow", N_SLEEP_MS(10, 1000, 2000))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, WAIT_REDEF_TASK_REACH_PENDING_FAILED, 123, "wait redef task reach pending failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, WAIT_REDEF_TASK_REACH_PENDING_SLOW, 124, "wait redef task reach pending slow", N_SLEEP_MS(10, 1000, 2000))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_REDO_WRITER_SPEED_CONTROL_FAILED, 125, "ddl redo log writer speed control failed", RET_ERR(OB_TASK_EXPIRED))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_REDO_WRITER_WRITE_MACRO_LOG_FAILED, 126, "ddl redo writer write macro log failed", RET_ERR(OB_STATE_NOT_MATCH, OB_NOT_MASTER, OB_TASK_EXPIRED))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_REDO_WRITER_WRITE_START_LOG_FAILED, 127, "ddl redo writer write start log failed", RET_ERR(OB_STATE_NOT_MATCH, OB_NOT_MASTER, OB_TASK_EXPIRED))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_REDO_WRITER_WRITE_COMMIT_LOG_FAILED, 128, "ddl redo writer write commit log failed", RET_ERR(OB_STATE_NOT_MATCH, OB_NOT_MASTER, OB_TASK_EXPIRED))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_INSERT_SSTABLE_GET_NEXT_ROW_FAILED, 129, "ddl insert sstable get next row failed", RET_ERR(OB_REPLICA_NOT_READABLE, OB_ERR_INSUFFICIENT_PX_WORKER, OB_TABLE_NOT_EXIST))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, COMPLEMENT_DATA_TASK_SPLIT_RANGE_FAILED, 130, "complement data task split range failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, COMPLEMENT_DATA_TASK_LOCAL_SCAN_FAILED, 131, "complement data task local scan failed", RET_ERR(OB_EAGAIN))
DDL_SIM_POINT_DEFINE(SIM_TYPE_DDL, DDL_TASK_SWITCH_INDEX_NAME_FAILED, 132, "ddl task switch index name failed", N_RET_ERR(5, OB_EAGAIN))
#endif
