#include "storage/delta_transaction_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "functions/delta_scan/delta_multi_file_list.hpp"

namespace duckdb {

DeltaTransactionManager::DeltaTransactionManager(AttachedDatabase &db_p, DeltaCatalog &delta_catalog)
    : TransactionManager(db_p), delta_catalog(delta_catalog) {
}

Transaction &DeltaTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<DeltaTransaction>(delta_catalog, *this, context);
	transaction->Start();
	auto &result = *transaction;
	lock_guard<mutex> l(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

static ErrorData HandleConflict(DeltaTransaction &transaction, ErrorData &original_error) {
	try {
		transaction.CleanUpFiles();
	} catch (std::exception &ex) {
		ErrorData new_error(ex);
		string new_message =
		    StringUtil::Format("Multiple exceptions happened. Firstly, the DeltaTransaction failed to commit with "
		                       "'%s'. Secondly, DuckDB failed to clean up the files produced by this transaction: '%s'",
		                       original_error.Message());
		return ErrorData(original_error.Type(), new_message);
	}
	return original_error;
}

ErrorData DeltaTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	auto &delta_transaction = transaction.Cast<DeltaTransaction>();
	try {
		delta_transaction.Commit(context);
	} catch (std::exception &ex) {
		ErrorData err(ex);
		return HandleConflict(delta_transaction, err);
	}
	lock_guard<mutex> l(transaction_lock);
	transactions.erase(transaction);
	return ErrorData();
}

void DeltaTransactionManager::RollbackTransaction(Transaction &transaction) {
	auto &delta_transaction = transaction.Cast<DeltaTransaction>();
	delta_transaction.Rollback();
	lock_guard<mutex> l(transaction_lock);
	transactions.erase(transaction);
}

void DeltaTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// NOP
}

} // namespace duckdb
