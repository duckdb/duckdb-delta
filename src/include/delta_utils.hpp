#pragma once

#include "generated_delta_kernel_ffi.hpp"
#define DEFINE_DEFAULT_ENGINE_BASE 1
#include "delta_kernel_ffi.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/planner/filter/struct_filter.hpp"
#include <iostream>

#include "duckdb/planner/tableref/bound_at_clause.hpp"

// TODO: clean up this file as we go

namespace duckdb {
class DatabaseInstance;

// Allocator for errors that the kernel might throw
struct DuckDBEngineError : ffi::EngineError {
	// Allocate a DuckDBEngineError, function ptr passed to kernel for error allocation
	static ffi::EngineError *AllocateError(ffi::KernelError etype, ffi::KernelStringSlice msg);
	static ffi::EngineError *AllocateError(ffi::KernelError etype, const string &msg);
	// Convert a kernel error enum to a string
	static string KernelErrorEnumToString(ffi::KernelError err);

	// Return the error as a string (WARNING: consumes the object by calling `delete this`)
	string IntoString();

	// The error message from Kernel
	string error_message;
};

//! Object to pass catalog information about a table's latest log entries to the kernel
struct DeltaLogPathArray {
	DeltaLogPathArray(Value val);

	// Construct the FFI safe (non-owning) object for kernel to read the log path
	ffi::LogPathArray GetFFIPtr();

	// For passing to ffi
	unique_ptr<StringHeap> string_heap;
	vector<ffi::FfiLogPath> log_entries;
};

// Mirrors the Delta protocol's `delta.columnMapping.mode` table property.
// Determines how readers resolve logical columns to parquet columns:
//   ID   -> resolve by parquet field_id
//   NAME -> resolve by physical name
//   NONE -> resolve by display (logical) name
enum class DeltaColumnMappingMode { NONE, ID, NAME };

struct KernelUtils {
	static LogicalType GetLogPathType();
	static ffi::KernelStringSlice ToDeltaString(const string &str);
	static string FromDeltaString(const struct ffi::KernelStringSlice slice);
	static vector<bool> FromDeltaBoolSlice(const struct ffi::KernelBoolSlice slice);
	static string FetchFromStringMap(ffi::Handle<ffi::SharedExternEngine> engine, const ffi::CStringMap *map,
	                                 const string &key);
	// Read `delta.columnMapping.mode` from the snapshot's table-property
	// configuration. Returns NONE when the property is absent or `"none"`.
	static DeltaColumnMappingMode ReadColumnMappingMode(ffi::SharedSnapshot *snapshot);

	static void *StringAllocationNew(const struct ffi::KernelStringSlice slice) {
		return new string(slice.ptr, slice.len);
	}

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

	template <class T>
	static ffi::OptionalValue<T> OptionalSome(T &val) {
		ffi::OptionalValue<T> some = {};
		some.tag = ffi::OptionalValue<T>::Tag::Some;
		some.some = {val};
		return some;
	}

	template <class T>
	static ffi::OptionalValue<T> OptionalNone() {
		ffi::OptionalValue<T> none = {};
		none.tag = ffi::OptionalValue<T>::Tag::None;
		return none;
	}

	static vector<unique_ptr<ParsedExpression>>
	UnpackTransformExpression(const vector<unique_ptr<ParsedExpression>> &parsed_expression);
};

class ExpressionVisitor : public ffi::EngineExpressionVisitor {
	using FieldList = vector<unique_ptr<ParsedExpression>>;

public:
	unique_ptr<vector<unique_ptr<ParsedExpression>>>
	VisitKernelExpression(const ffi::Handle<ffi::SharedExpression> *expression);
	unique_ptr<vector<unique_ptr<ParsedExpression>>>
	VisitKernelPredicate(const ffi::Handle<ffi::SharedPredicate> *predicate);
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
	static void VisitTransformExpression(void *data, uintptr_t sibling_list_id, uintptr_t input_path_list_id,
	                                     uintptr_t child_list_id);
	static void VisitFieldTransform(void *data, uintptr_t sibling_list_id, const ffi::KernelStringSlice *field_name,
	                                uintptr_t expr_list_id, bool is_replace);
	static void VisitNotExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id);
	static void VisitIsNullExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id);
	static void VisitLiteralMap(void *data, uintptr_t sibling_list_id, uintptr_t key_list_id, uintptr_t value_list_id);
	static void VisitOpaqueExpression(void *data, uintptr_t sibling_list_id,
	                                  ffi::Handle<ffi::SharedOpaqueExpressionOp> op, uintptr_t child_list_id);
	static void VisitOpaquePredicate(void *data, uintptr_t sibling_list_id,
	                                 ffi::Handle<ffi::SharedOpaquePredicateOp> op, uintptr_t child_list_id);
	static void VisitUnknown(void *data, uintptr_t sibling_list_id, ffi::KernelStringSlice name);
	static void VisitParseJsonExpression(void *data, uintptr_t sibling_list_id, uintptr_t child_list_id,
	                                     ffi::Handle<ffi::SharedSchema> output_schema);

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
	static void VisitSubtractionExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id);
	static void VisitDivideExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id);
	static void VisitCoalesceExpression(void *state, uintptr_t sibling_list_id, uintptr_t child_list_id);
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

// TODO once nullability upstreamed in duckdb remove this class
struct DeltaMultiFileColumnDefinition : public MultiFileColumnDefinition {
	DeltaMultiFileColumnDefinition(const string &name, const LogicalType &type, bool nullable_p)
	    : MultiFileColumnDefinition(name, type), children(), nullable(nullable_p) {
	}

	static vector<MultiFileColumnDefinition> ConvertToBase(vector<DeltaMultiFileColumnDefinition> col_def) {
		vector<MultiFileColumnDefinition> res;
		for (auto &col : col_def) {
			res.push_back(col.ToBaseColdef());
		}
		return res;
	}

	MultiFileColumnDefinition ToBaseColdef() {
		auto res = MultiFileColumnDefinition(name, type);

		for (auto &child : children) {
			res.children.push_back(child.ToBaseColdef());
		}

		res.default_expression = default_expression->Copy();
		res.identifier = identifier;

		return res;
	}

	static void Print(vector<DeltaMultiFileColumnDefinition> schema, const string &name) {
		return;
		idx_t nest_level = 0;
		printf("\nSchema '%s':\n", name.c_str());
		for (auto &col : schema) {
			col.Print(nest_level);
		}
	}
	void Print(idx_t nest_level) {
		string prefix = StringUtil::Repeat("  ", nest_level) + "- ";
		printf("%s%s %s (identifier: %s)\n", prefix.c_str(), name.c_str(), type.ToString().c_str(),
		       identifier.ToString().c_str());
		for (auto &child : children) {
			child.Print(nest_level + 1);
		}
	}

	vector<DeltaMultiFileColumnDefinition> children;
	bool nullable = true;
};

// SchemaVisitor is used to parse the schema of a Delta table from the Kernel
class SchemaVisitor {
public:
	SchemaVisitor(ffi::Handle<ffi::SharedExternEngine> engine_p, DeltaColumnMappingMode mapping_mode_p)
	    : engine(engine_p), mapping_mode(mapping_mode_p) {};

	static vector<DeltaMultiFileColumnDefinition> VisitSnapshotSchema(ffi::Handle<ffi::SharedExternEngine> engine,
	                                                                  ffi::SharedSnapshot *snapshot,
	                                                                  DeltaColumnMappingMode mapping_mode);
	static vector<DeltaMultiFileColumnDefinition>
	VisitSnapshotGlobalReadSchema(ffi::Handle<ffi::SharedExternEngine> engine, ffi::SharedScan *state, bool logical,
	                              DeltaColumnMappingMode mapping_mode);
	static vector<DeltaMultiFileColumnDefinition> VisitWriteContextSchema(ffi::Handle<ffi::SharedExternEngine> engine,
	                                                                      ffi::SharedWriteContext *write_context);

private:
	unordered_map<uintptr_t, vector<DeltaMultiFileColumnDefinition>> inflight_lists;
	uintptr_t next_id = 1;

	ffi::SharedExternEngine *engine = nullptr;
	DeltaColumnMappingMode mapping_mode = DeltaColumnMappingMode::NONE;
	ErrorData error;

	static ffi::EngineSchemaVisitor CreateSchemaVisitor(SchemaVisitor &state);

	typedef void(SimpleTypeVisitorFunction)(void *, uintptr_t, ffi::KernelStringSlice, bool is_nullable,
	                                        const ffi::CStringMap *metadata);

	// Set `col_def.identifier` so DuckDB's MultiFileReader resolves the column
	// the way the Delta protocol's "Reader Requirements for Column Mapping"
	// require for the active mode. Identifier type drives the dispatch:
	// BIGINT -> match by parquet field_id, VARCHAR -> match by name. Leaving
	// it unset matches by display (logical) name.
	static void ApplyDeltaColumnMapping(SchemaVisitor &state, const ffi::CStringMap *metadata,
	                                    DeltaMultiFileColumnDefinition &col_def) {
		switch (state.mapping_mode) {
		case DeltaColumnMappingMode::ID: {
			auto id = KernelUtils::FetchFromStringMap(state.engine, metadata, "parquet.field.id");
			if (!id.empty()) {
				col_def.identifier = Value(id).DefaultCastAs(LogicalType::BIGINT);
			}
			break;
		}
		case DeltaColumnMappingMode::NAME: {
			auto name = KernelUtils::FetchFromStringMap(state.engine, metadata, "delta.columnMapping.physicalName");
			if (!name.empty()) {
				col_def.identifier = Value(name);
			}
			break;
		}
		case DeltaColumnMappingMode::NONE:
			break;
		}
		col_def.default_expression = make_uniq<ConstantExpression>(Value(col_def.type));
	}

	template <LogicalTypeId TypeId>
	static SimpleTypeVisitorFunction *VisitSimpleType() {
		return (SimpleTypeVisitorFunction *)&VisitSimpleTypeImpl<TypeId>;
	}
	template <LogicalTypeId TypeId>
	static void VisitSimpleTypeImpl(SchemaVisitor *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
	                                bool is_nullable, const ffi::CStringMap *metadata) {
		DeltaMultiFileColumnDefinition col_def(KernelUtils::FromDeltaString(name), TypeId, is_nullable);
		ApplyDeltaColumnMapping(*state, metadata, col_def);

		state->AppendToList(sibling_list_id, name, std::move(col_def));
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
	static void VisitVariant(SchemaVisitor *state, uintptr_t sibling_list_id, ffi::KernelStringSlice name,
	                         bool is_nullable, const ffi::CStringMap *metadata);

	uintptr_t MakeFieldListImpl(uintptr_t capacity_hint);
	void AppendToList(uintptr_t id, ffi::KernelStringSlice name, DeltaMultiFileColumnDefinition &&child);
	vector<DeltaMultiFileColumnDefinition> TakeFieldList(uintptr_t id);
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

	KernelType *release() {
		auto copy = ptr;
		ptr = nullptr;
		return copy;
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
typedef TemplatedUniqueKernelPointer<ffi::SharedScanMetadataIterator, ffi::free_scan_metadata_iter>
    KernelScanDataIterator;
typedef TemplatedUniqueKernelPointer<ffi::ExclusiveTransaction, ffi::free_transaction> KernelExclusiveTransaction;
typedef TemplatedUniqueKernelPointer<ffi::ExclusiveEngineData, ffi::free_engine_data> KernelEngineData;

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
struct DeltaTableFilters;

class PredicateVisitor : public ffi::EnginePredicate {
public:
	PredicateVisitor(const vector<DeltaMultiFileColumnDefinition> &columns,
	                 optional_ptr<const DeltaTableFilters> filters);

	ErrorData error_data;

private:
	unordered_map<string, optional_ptr<const ExpressionFilter>> column_filters;
	// Top-level column types, keyed by the same name as column_filters, used to reject filters whose subject is a bare
	// reference to a nested (struct/list/map) column - the path to the scalar leaf is not recoverable in that form.
	unordered_map<string, LogicalType> column_types;

	static uintptr_t VisitPredicate(PredicateVisitor *predicate, ffi::KernelExpressionVisitorState *state);

	uintptr_t VisitConstantFilter(const string &col_name, ExpressionType comparison_type, const Value &value,
	                              ffi::KernelExpressionVisitorState *state);
	uintptr_t VisitFilterExpression(const string &col_name, const Expression &expr,
	                                ffi::KernelExpressionVisitorState *state);

	uintptr_t VisitIsNull(const string &col_name, ffi::KernelExpressionVisitorState *state);
	uintptr_t VisitIsNotNull(const string &col_name, ffi::KernelExpressionVisitorState *state);

	uintptr_t VisitFilter(const string &col_name, const ExpressionFilter &filter,
	                      ffi::KernelExpressionVisitorState *state);
};

// Singleton class to forward logs to DuckDB
class LoggerCallback {
public:
	//! The Callback for the DuckDB setting to hook up Delta Kernel Logging to the DuckDB logger
	static void DuckDBSettingCallBack(ClientContext &context, SetScope scope, Value &parameter);

	//! Singleton GetInstance
	static LoggerCallback &GetInstance();
	static void Initialize(DatabaseInstance &db);
	static bool TryLog(const char *type_, LogLevel level, const string &msg);
	static void CallbackEvent(ffi::Event log_line);

	static LogLevel GetDuckDBLogLevel(ffi::Level);

protected:
	LoggerCallback() {
	}

	mutex lock;
	weak_ptr<DatabaseInstance> db;
	atomic<bool> enabled {false};
};

} // namespace duckdb
