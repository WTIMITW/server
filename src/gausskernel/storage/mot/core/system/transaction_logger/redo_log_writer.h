/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 *
 * redo_log_writer.h
 *    Helper class for writing redo log entries.
 *
 * IDENTIFICATION
 *    src/gausskernel/storage/mot/core/system/transaction_logger/redo_log_writer.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef REDO_LOG_WRITER_H
#define REDO_LOG_WRITER_H

#include <array>
#include <iostream>
#include <list>
#include <pthread.h>
#include "spin_lock.h"
#include "rwspinlock.h"
#include "global.h"
#include "utilities.h"
#include "redo_log_buffer.h"
#include "redo_statistics.h"
#include "txn_table.h"
#include "bitmapset.h"

namespace MOT {
class TxnManager;
class Key;

/**
 * @struct EndSegmentBlock
 * @brief describes a transaction's information:
 * opCode, Csn and transaction ids
 */
struct EndSegmentBlock {
    OperationCode m_opCode;
    uint16_t m_flags;
    uint32_t m_reserved;
    uint64_t m_csn;
    uint64_t m_externalTransactionId;
    uint64_t m_internalTransactionId;

    EndSegmentBlock()
        : m_opCode(INVALID_OPERATION_CODE),
          m_flags(0),
          m_reserved(0),
          m_csn(0),
          m_externalTransactionId(INVALID_TRANSACTION_ID),
          m_internalTransactionId(INVALID_TRANSACTION_ID)
    {}
    EndSegmentBlock(OperationCode opCode, uint16_t flags, uint64_t csn, uint64_t externalTransactionId,
        uint64_t internalTransactionId)
        : m_opCode(opCode),
          m_flags(flags),
          m_reserved(0),
          m_csn(csn),
          m_externalTransactionId(externalTransactionId),
          m_internalTransactionId(internalTransactionId)
    {}

    static constexpr uint16_t MOT_UPDATE_INDEX_COLUMN_FLAG = 0x0001;
};

/**
 * @class EndSegmentBlockSerializer
 * @brief serializes and deserializes an end segment
 * block
 */
class EndSegmentBlockSerializer {
public:
    /**
     * @brief provides the size of the needed buffer
     * @param block the segment block.
     * @return Size_t the size.
     */
    static size_t SerializeSize(EndSegmentBlock* block);

    /**
     * @brief serializes a block
     * @param block the segment block.
     * @param dataOut the data buffer to serialize to.
     */
    static void Serialize(EndSegmentBlock* block, char* dataOut);

    /**
     * @brief deserializes a block from a buffer
     * @param block the segment block.
     * @param dataIn the data buffer to serialize from.
     */
    static void Deserialize(EndSegmentBlock* block, const char* dataIn);
};

/**
 * @class RedoLogWriter
 * @brief Helper class for writing redo log entries.
 */
class RedoLogWriter {
public:
    /**
     * @brief Appends update row redo log entry.
     * @param redoLogBuffer The redo buffer of the transaction.
     * @param row The updated row.
     * @param modifiedColumns the row's modified columns.
     */

    /**
     * @brief Appends update row redo log entry.
     * @param redoLogBuffer The redo buffer of the transaction.
     * @param row The row.
     * @param modifiedColumns Bitmap set of modified columns.
     * @param version updated row's version (csn).
     */
    static bool AppendUpdate(
        RedoLogBuffer& redoLogBuffer, Row* row, BitmapSet* modifiedColumns, uint64_t previous_version, bool idxUpd);

    /**
     * @brief Appends New row redo log entry.
     * @param redoLogBuffer The redo buffer of the transaction.
     * @param table The table identifier.
     * @param primaryKey The primary key.
     */
    static bool AppendCreateRow(RedoLogBuffer& redoLogBuffer, uint64_t table, Key* primaryKey, const void* rowData,
        uint64_t rowDataSize, uint64_t externalId, uint64_t rowId);

    /**
     * @brief Appends remove row redo log entry.
     * @param redoLogBuffer The redo buffer of the transaction.
     * @param table The table identifier.
     * @param primaryKey The primary key.
     * @param external table id.
     * @param version The deleted row version.
     */
    static bool AppendRemove(
        RedoLogBuffer& redoLogBuffer, uint64_t table, const Key* primaryKey, uint64_t version, uint64_t externalId);

    /**
     * @brief Appends a commit transaction redo log entry.
     * @param redoLogBuffer The redo buffer of the transaction.
     */
    static void AppendCommit(RedoLogBuffer& redoLogBuffer, TxnManager* txn);

    /**
     * @brief Appends a commit transaction redo log entry.
     * @param redoLogBuffer The redo buffer of the transaction.
     */
    static void AppendPrepare(RedoLogBuffer& redoLogBuffer, TxnManager* txn);

    /**
     * @brief Appends a commit transaction redo log entry.
     * @param redoLogBuffer The redo buffer of the transaction.
     */
    static void AppendCommitPrepared(RedoLogBuffer& redoLogBuffer, TxnManager* txn);

    /**
     * @brief Appends a commit transaction redo log entry.
     * @param redoLogBuffer The redo buffer of the transaction.
     */
    static void AppendRollbackPrepared(RedoLogBuffer& redoLogBuffer, TxnManager* txn);

    /**
     * @brief Appends a rollback transaction redo log entry.
     * @param redoLogBuffer The redo buffer of the transaction.
     */
    static void AppendRollback(RedoLogBuffer& redoLogBuffer, TxnManager* txn);

    /**
     * @brief Appends a partial redo log control entry.
     */
    static void AppendPartial(RedoLogBuffer& redoLogBuffer, TxnManager* txn);

    /**
     * Adds a table to the log.
     * @param buffer The buffer that the table will be serialized to.
     * @param len The data buffer length.
     */
    static bool AppendTable(RedoLogBuffer& buffer, Table* table);

    /**
     * Adds a drop table entry to the log.
     * @param buffer The buffer that the drop table will be serialized to.
     * @param The dropped table.
     */
    static bool AppendDropTable(RedoLogBuffer& buffer, Table* table);

    /**
     * Adds an index to the log.
     * @param buffer The buffer that the index will be serialized to.
     * @param len The data buffer length.
     */
    static bool AppendIndex(RedoLogBuffer& buffer, Table* table, MOT::Index* index);

    /**
     * Adds a drop index entry to the log.
     * @param buffer The buffer that the index will be serialized to.
     * @param table The table the index belongs to.
     * @param index The index to drop.
     */
    static bool AppendDropIndex(RedoLogBuffer& buffer, Table* table, MOT::Index* index);

    /**
     * Adds a truncate table entry to the log.
     * @param buffer The buffer that the index will be serialized to.
     * @param table The table to truncate.
     * @param transactionId The transaction id.
     */
    static bool AppendTruncateTable(RedoLogBuffer& buffer, Table* table);

    /**
     * Adds a alter table add column entry to the log.
     * @param buffer The buffer that the index will be serialized to.
     * @param table The table to alter.
     * @param col the added column.
     */
    static bool AppendAlterTableAddColumn(RedoLogBuffer& buffer, Table* table, Column* col);

    /**
     * Adds a alter table drop column entry to the log.
     * @param buffer The buffer that the index will be serialized to.
     * @param table The table to alter.
     * @param col the dropped column.
     */
    static bool AppendAlterTableDropColumn(RedoLogBuffer& buffer, Table* table, Column* col);

    /**
     * Adds a alter table rename column entry to the log.
     * @param buffer The buffer that the index will be serialized to.
     * @param table The table to alter.
     * @param alter Structure that contains table and rename.
     */
    static bool AppendAlterTableRenameColumn(RedoLogBuffer& buffer, Table* table, DDLAlterTableRenameColumn* alter);

    /**
     * Prints the operation code of a data buffer.
     * @param data The data buffer.
     * @param len The data buffer length.
     */
    static void ApplyData(const void* data, size_t len);

    /* @def Create Table redo format overhead. */
    static constexpr uint32_t REDO_CREATE_TABLE_FORMAT_OVERHEAD =
        sizeof(OperationCode) + sizeof(size_t) + sizeof(EndSegmentBlock);

    /* @def Max limit of table serialize size. */
    static constexpr uint32_t REDO_MAX_TABLE_SERIALIZE_SIZE =
        RedoLogBuffer::REDO_DEFAULT_BUFFER_SIZE - REDO_CREATE_TABLE_FORMAT_OVERHEAD;

    /* @def Create Index redo format overhead. */
    static constexpr uint32_t REDO_CREATE_INDEX_FORMAT_OVERHEAD =
        sizeof(OperationCode) + sizeof(size_t) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(EndSegmentBlock);

    /* @def Max limit of index serialize size. */
    static constexpr uint32_t REDO_MAX_INDEX_SERIALIZE_SIZE =
        RedoLogBuffer::REDO_DEFAULT_BUFFER_SIZE - REDO_CREATE_INDEX_FORMAT_OVERHEAD;

    /* @def Alter Table redo format overhead. */
    static constexpr uint32_t REDO_ALTER_TABLE_FORMAT_OVERHEAD =
        sizeof(OperationCode) + sizeof(size_t) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(EndSegmentBlock);
};
}  // End of namespace MOT

#endif /* REDO_LOG_WRITER_H */
