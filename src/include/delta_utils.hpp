#pragma once

#include "delta_kernel_ffi.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include <duckdb/planner/filter/null_filter.hpp>
#include <iostream>

// TODO: clean up this file as we go

namespace duckdb {
class DatabaseInstance;

class ExpressionVisitor : public ffi::EngineExpressionVisitor {
	using FieldList = vector<unique_ptr<ParsedExpression>>;

public:
	unique_ptr<vector<unique_ptr<ParsedExpression>>>
	VisitKernelExpression(const ffi::Handle<ffi::SharedExpression> *expression);
	unique_ptr<vector<unique_ptr<ParsedExpression>>> VisitKernelExpression(const ffi::Expression *expression);
	ffi::EngineExpressionVisitor CreateVisitor(ExpressionVisitor &state);

private:
	unordered_map<uintptr_t, unique_ptr<FieldList>> inflight_lists;
	uintptr_t next_id = 1;

	ErrorData error;

	// Literals
	template <typename CPP_TYPE, Value (*CREATE_VALUE_FUN)(CPP_TYPE)>
	static ffi::VisitLiteralFn<CPP_TYPE> VisitPrimitiveLiteral() {
		return (ffi::VisitLiteralFn<CPP_TYPE>)&VisitPrimitiveLiteral<CPP_TYPE, CREATE_VALUE_FUN>;
	}
	template <typename CPP_TYPE, typename CREATE_VALUE_FUN>
	static void VisitPrimitiveLiteral(void *state, uintptr_t sibling_list_id, CPP_TYPE value) {
		auto state_cast = static_cast<ExpressionVisitor *>(state);
		auto duckdb_value = CREATE_VALUE_FUN(value);
		auto expression = make_uniq<ConstantExpression>(duckdb_value);
		state_cast->AppendToList(sibling_list_id, std::move(expression));
	}

	static void VisitPrimitiveLiteralBool(void *state, uintptr_t sibling_list_id, bool value);
	static void VisitPrimitiveLiteralByte(void *state, uintptr_t sibling_list_id, int8_t value);
	static void VisitPrimitiveLiteralShort(void *state, uintptr_t sibling_list_id, int16_t value);
	static void VisitPrimitiveLiteralInt(void *state, uintptr_t sibling_list_id, int32_t value);
	static void VisitPrimitiveLiteralLong(void *state, uintptr_t sibling_list_id, int64_t value);
	static void VisitPrimitiveLiteralFloat(void *state, uintptr_t sibling_list_id, float value);
	static void VisitPrimitiveLiteralDouble(void *state, uintptr_t sibling_list_id, double value);

	static void VisitTimestampLiteral(void *state, uintptr_t sibling_list_id, int64_t value);
	static void VisitTimestampNtzLiteral(void *state, uintptr_t sibling_list_id, int64_t value);
	static void VisitDateLiteral(void *state, uintptr_t sibling_list_id, int32_t value);
	static void VisitStringLiteral(void *state, uintptr_t sibling_list_id, ffi::KernelStringSlice value);
	static void VisitBinaryLiteral(void *state, uintptr_t sibling_list_id, const uint8_t *buffer, uintptr_t len);
	static void VisitNullLiteral(void *state, uintptr_t sibling_list_id);
	static void VisitArrayLiteral(void *state, uintptr_t sibling_list_id, uintptr_t child_id);
	static void VisitStructLiteral(void *data, uintptr_t sibling_list_id, uintptr_t child_field_list_value,
	                               uintptr_t child_value_list_id);
	static void VisitDecimalLiteral(void *state, uintptr_t sibling_list_id, int64_t value_ms, uint64_t value_ls,
	                                uint8_t precision, uint8_t scale);
	static void VisitColumnExpression(void *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name);
	static void VisitStructExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id);
	static void VisitNotExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id);
	static void VisitIsNullExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id);

	template <ExpressionType EXPRESSION_TYPE, typename EXPRESSION_TYPENAME>
	static ffi::VisitJunctionFn VisitUnaryExpression() {
		return &VisitVariadicExpression<EXPRESSION_TYPE, EXPRESSION_TYPENAME>;
	}
	template <ExpressionType EXPRESSION_TYPE, typename EXPRESSION_TYPENAME>
	static ffi::VisitJunctionFn VisitBinaryExpression() {
		return &VisitBinaryExpression<EXPRESSION_TYPE, EXPRESSION_TYPENAME>;
	}
	template <ExpressionType EXPRESSION_TYPE, typename EXPRESSION_TYPENAME>
	static ffi::VisitJunctionFn VisitVariadicExpression() {
		return &VisitVariadicExpression<EXPRESSION_TYPE, EXPRESSION_TYPENAME>;
	}

	template <ExpressionType EXPRESSION_TYPE, typename EXPRESSION_TYPENAME>
	static void VisitVariadicExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id) {
		auto state_cast = static_cast<ExpressionVisitor *>(state);
		auto children = state_cast->TakeFieldList(child_list_id);
		if (!children) {
			state_cast->AppendToList(sibling_list_id, std::move(make_uniq<ConstantExpression>(Value(42))));
			return;
		}
		unique_ptr<ParsedExpression> expression = make_uniq<EXPRESSION_TYPENAME>(EXPRESSION_TYPE, std::move(*children));
		state_cast->AppendToList(sibling_list_id, std::move(expression));
	}

	static void VisitAdditionExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id);
	static void VisitSubctractionExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id);
	static void VisitDivideExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id);
	static void VisitMultiplyExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id);

	template <ExpressionType EXPRESSION_TYPE, typename EXPRESSION_TYPENAME>
	static void VisitBinaryExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id) {
		auto state_cast = static_cast<ExpressionVisitor *>(state);
		auto children = state_cast->TakeFieldList(child_list_id);
		if (!children) {
			state_cast->AppendToList(sibling_list_id, std::move(make_uniq<ConstantExpression>(Value(42))));
			return;
		}

		if (children->size() != 2) {
			state_cast->AppendToList(sibling_list_id, std::move(make_uniq<ConstantExpression>(Value(42))));
			state_cast->error =
			    ErrorData("INCORRECT SIZE IN VISIT_BINARY_EXPRESSION" + EnumUtil::ToString(EXPRESSION_TYPE));
			return;
		}

		auto &lhs = children->at(0);
		auto &rhs = children->at(1);
		unique_ptr<ParsedExpression> expression =
		    make_uniq<EXPRESSION_TYPENAME>(EXPRESSION_TYPE, std::move(lhs), std::move(rhs));
		state_cast->AppendToList(sibling_list_id, std::move(expression));
	}

	static void VisitComparisonExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id);

	// List functions
	static uintptr_t MakeFieldList(ExpressionVisitor *state, uintptr_t capacity_hint);
	void AppendToList(uintptr_t id, unique_ptr<ParsedExpression> child);
	uintptr_t MakeFieldListImpl(uintptr_t capacity_hint);
	unique_ptr<FieldList> TakeFieldList(uintptr_t id);
};

// SchemaVisitor is used to parse the schema of a Delta table from the Kernel
class SchemaVisitor {
public:
	using FieldList = child_list_t<LogicalType>;

	static unique_ptr<FieldList> VisitSnapshotSchema(ffi::SharedSnapshot *snapshot);
	static unique_ptr<FieldList> VisitSnapshotGlobalReadSchema(ffi::SharedGlobalScanState *state, bool logical);

private:
	unordered_map<uintptr_t, unique_ptr<FieldList>> inflight_lists;
	uintptr_t next_id = 1;

	ErrorData error;

	static ffi::EngineSchemaVisitor CreateSchemaVisitor(SchemaVisitor &state);

	typedef void(SimpleTypeVisitorFunction)(void *, uintptr_t, ffi::KernelStringSlice, bool is_nullable,
	                                        const ffi::CStringMap *metadata);

	template <LogicalTypeId TypeId>
	static SimpleTypeVisitorFunction *VisitSimpleType() {
		return (SimpleTypeVisitorFunction *)&VisitSimpleTypeImpl<TypeId>;
	}
	template <LogicalTypeId TypeId>
	static void VisitSimpleTypeImpl(SchemaVisitor *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
	                                bool is_nullable, const ffi::CStringMap *metadata) {
		state->AppendToList(sibling_list_id, name, TypeId);
	}

	static void VisitDecimal(SchemaVisitor *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
	                         bool is_nullable, const ffi::CStringMap *metadata, uint8_t precision, uint8_t scale);
	static uintptr_t MakeFieldList(SchemaVisitor *state, uintptr_t capacity_hint);
	static void VisitStruct(SchemaVisitor *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
	                        bool is_nullable, const ffi::CStringMap *metadata, uintptr_t child_list_id);
	static void VisitArray(SchemaVisitor *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
	                       bool is_nullable, const ffi::CStringMap *metadata, uintptr_t child_list_id);
	static void VisitMap(SchemaVisitor *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name, bool is_nullable,
	                     const ffi::CStringMap *metadata, uintptr_t child_list_id);

	uintptr_t MakeFieldListImpl(uintptr_t capacity_hint);
	void AppendToList(uintptr_t id, ffi::KernelStringSlice name, LogicalType &&child);
	unique_ptr<FieldList> TakeFieldList(uintptr_t id);
};

// Allocator for errors that the kernel might throw
struct DuckDBEngineError : ffi::EngineError {
	// Allocate a DuckDBEngineError, function ptr passed to kernel for error allocation
	static ffi::EngineError *AllocateError(ffi::KernelError etype, ffi::KernelStringSlice msg);
	// Convert a kernel error enum to a string
	static string KernelErrorEnumToString(ffi::KernelError err);

	// Return the error as a string (WARNING: consumes the object by calling `delete this`)
	string IntoString();

	// The error message from Kernel
	string error_message;
};

// RAII wrapper that returns ownership of a kernel pointer to kernel when it goes out of
// scope. Similar to std::unique_ptr. but does not define operator->() and does not require the
// kernel type to be complete.
template <typename KernelType>
struct UniqueKernelPointer {
	UniqueKernelPointer() : ptr(nullptr), free(nullptr) {
	}

	// Takes ownership of a pointer with associated deleter.
	UniqueKernelPointer(KernelType *ptr, void (*free)(KernelType *)) : ptr(ptr), free(free) {
	}

	// movable but not copyable
	UniqueKernelPointer(UniqueKernelPointer &&other) : ptr(other.ptr) {
		other.ptr = nullptr;
	}
	UniqueKernelPointer &operator=(UniqueKernelPointer &&other) {
		std::swap(ptr, other.ptr);
		std::swap(free, other.free);
		return *this;
	}
	UniqueKernelPointer(const UniqueKernelPointer &) = delete;
	UniqueKernelPointer &operator=(const UniqueKernelPointer &) = delete;

	~UniqueKernelPointer() {
		if (ptr && free) {
			free(ptr);
		}
	}

	KernelType *get() const {
		return ptr;
	}

private:
	KernelType *ptr;
	void (*free)(KernelType *) = nullptr;
};

// Syntactic sugar around the different kernel types
template <typename KernelType, void (*DeleteFunction)(KernelType *)>
struct TemplatedUniqueKernelPointer : public UniqueKernelPointer<KernelType> {
	TemplatedUniqueKernelPointer() : UniqueKernelPointer<KernelType>() {};
	TemplatedUniqueKernelPointer(KernelType *ptr) : UniqueKernelPointer<KernelType>(ptr, DeleteFunction) {};
};

typedef TemplatedUniqueKernelPointer<ffi::SharedSnapshot, ffi::free_snapshot> KernelSnapshot;
typedef TemplatedUniqueKernelPointer<ffi::SharedExternEngine, ffi::free_engine> KernelExternEngine;
typedef TemplatedUniqueKernelPointer<ffi::SharedScan, ffi::free_scan> KernelScan;
typedef TemplatedUniqueKernelPointer<ffi::SharedGlobalScanState, ffi::free_global_scan_state> KernelGlobalScanState;
typedef TemplatedUniqueKernelPointer<ffi::SharedScanMetadataIterator, ffi::free_scan_metadata_iter> KernelScanDataIterator;

template <typename KernelType, void (*DeleteFunction)(KernelType *)>
struct SharedKernelPointer;

// A reference to a SharedKernelPointer, only 1 can be handed out at the same time
template <typename KernelType, void (*DeleteFunction)(KernelType *)>
struct SharedKernelRef {
	friend struct SharedKernelPointer<KernelType, DeleteFunction>;

public:
	KernelType *GetPtr() {
		return owning_pointer.kernel_ptr.get();
	}
	~SharedKernelRef() {
		owning_pointer.lock.unlock();
	}

protected:
	SharedKernelRef(SharedKernelPointer<KernelType, DeleteFunction> &owning_pointer_p)
	    : owning_pointer(owning_pointer_p) {
		owning_pointer.lock.lock();
	}

protected:
	// The pointer that owns this ref
	SharedKernelPointer<KernelType, DeleteFunction> &owning_pointer;
};

// Wrapper around ffi objects to share between threads
template <typename KernelType, void (*DeleteFunction)(KernelType *)>
struct SharedKernelPointer {
	friend struct SharedKernelRef<KernelType, DeleteFunction>;

public:
	SharedKernelPointer(TemplatedUniqueKernelPointer<KernelType, DeleteFunction> unique_kernel_ptr)
	    : kernel_ptr(unique_kernel_ptr) {
	}
	SharedKernelPointer(KernelType *ptr) : kernel_ptr(ptr) {
	}
	SharedKernelPointer() {
	}

	SharedKernelPointer(SharedKernelPointer &&other) : SharedKernelPointer() {
		other.lock.lock();
		lock.lock();
		kernel_ptr = std::move(other.kernel_ptr);
		lock.lock();
		other.lock.lock();
	}

	// Returns a reference to the underlying kernel object. The SharedKernelPointer to this object will be locked for
	// the lifetime of this reference
	SharedKernelRef<KernelType, DeleteFunction> GetLockingRef() {
		return SharedKernelRef<KernelType, DeleteFunction>(*this);
	}

protected:
	TemplatedUniqueKernelPointer<KernelType, DeleteFunction> kernel_ptr;
	mutex lock;
};

typedef SharedKernelPointer<ffi::SharedSnapshot, ffi::free_snapshot> SharedKernelSnapshot;

struct KernelUtils {
	static ffi::KernelStringSlice ToDeltaString(const string &str);
	static string FromDeltaString(const struct ffi::KernelStringSlice slice);
	static vector<bool> FromDeltaBoolSlice(const struct ffi::KernelBoolSlice slice);

	// Unpacks (and frees) a kernel result, either storing the result in out_value, or setting error_data
	template <class T>
	static ErrorData TryUnpackResult(ffi::ExternResult<T> result, T &out_value) {
		if (result.tag == ffi::ExternResult<T>::Tag::Err) {
			if (result.err._0) {
				auto error_cast = static_cast<DuckDBEngineError *>(result.err._0);
				return ErrorData(ExceptionType::IO, error_cast->IntoString());
			}
			return ErrorData(ExceptionType::IO, StringUtil::Format("Unknown Delta kernel error"));
		}
		if (result.tag == ffi::ExternResult<T>::Tag::Ok) {
			out_value = result.ok._0;
			return {};
		}
		return ErrorData(ExceptionType::IO, "Invalid Delta kernel ExternResult");
	}

	static vector<unique_ptr<ParsedExpression>> &
	UnpackTopLevelStruct(const vector<unique_ptr<ParsedExpression>> &parsed_expression);
};

class PredicateVisitor : public ffi::EnginePredicate {
public:
	PredicateVisitor(const vector<string> &column_names, optional_ptr<const TableFilterSet> filters);

	ErrorData error_data;

private:
	unordered_map<string, TableFilter *> column_filters;

	static uintptr_t VisitPredicate(PredicateVisitor *predicate, ffi::KernelExpressionVisitorState *state);

	uintptr_t VisitConstantFilter(const string &col_name, const ConstantFilter &filter,
	                              ffi::KernelExpressionVisitorState *state);
	uintptr_t VisitAndFilter(const string &col_name, const ConjunctionAndFilter &filter,
	                         ffi::KernelExpressionVisitorState *state);

	uintptr_t VisitIsNull(const string &col_name, ffi::KernelExpressionVisitorState *state);
	uintptr_t VisitIsNotNull(const string &col_name, ffi::KernelExpressionVisitorState *state);

	uintptr_t VisitFilter(const string &col_name, const TableFilter &filter, ffi::KernelExpressionVisitorState *state);
};

// Singleton class to forward logs to DuckDB
class LoggerCallback {
public:
	//! The Callback for the DuckDB setting to hook up Delta Kernel Logging to the DuckDB logger
	static void DuckDBSettingCallBack(ClientContext &context, SetScope scope, Value &parameter);

	//! Singleton GetInstance
	static LoggerCallback &GetInstance();
	static void Initialize(DatabaseInstance &db);
	static void CallbackEvent(ffi::Event log_line);

	static LogLevel GetDuckDBLogLevel(ffi::Level);

protected:
	LoggerCallback() {
	}

	mutex lock;
	weak_ptr<DatabaseInstance> db;
};

} // namespace duckdb
