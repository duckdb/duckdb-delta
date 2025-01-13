#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <ostream>

namespace ffi {

enum class KernelError {
	UnknownError,
	FFIError,
#if (defined(DEFINE_DEFAULT_ENGINE) || defined(DEFINE_SYNC_ENGINE))
	ArrowError,
#endif
	EngineDataTypeError,
	ExtractError,
	GenericError,
	IOErrorError,
#if (defined(DEFINE_DEFAULT_ENGINE) || defined(DEFINE_SYNC_ENGINE))
	ParquetError,
#endif
#if defined(DEFINE_DEFAULT_ENGINE)
	ObjectStoreError,
#endif
#if defined(DEFINE_DEFAULT_ENGINE)
	ObjectStorePathError,
#endif
#if defined(DEFINE_DEFAULT_ENGINE)
	ReqwestError,
#endif
	FileNotFoundError,
	MissingColumnError,
	UnexpectedColumnTypeError,
	MissingDataError,
	MissingVersionError,
	DeletionVectorError,
	InvalidUrlError,
	MalformedJsonError,
	MissingMetadataError,
	MissingProtocolError,
	MissingMetadataAndProtocolError,
	ParseError,
	JoinFailureError,
	Utf8Error,
	ParseIntError,
	InvalidColumnMappingModeError,
	InvalidTableLocationError,
	InvalidDecimalError,
	InvalidStructDataError,
	InternalError,
	InvalidExpression,
	InvalidLogPath,
};

struct CStringMap;

/// this struct can be used by an engine to materialize a selection vector
struct DvInfo;

#if defined(DEFINE_DEFAULT_ENGINE)
/// A builder that allows setting options on the `Engine` before actually building it
struct EngineBuilder;
#endif

/// an opaque struct that encapsulates data read by an engine. this handle can be passed back into
/// some kernel calls to operate on the data, or can be converted into the raw data as read by the
/// [`delta_kernel::Engine`] by calling [`get_raw_engine_data`]
struct ExclusiveEngineData;

struct ExclusiveFileReadResultIterator;

struct KernelExpressionVisitorState;

struct SharedExternEngine;

struct SharedGlobalScanState;

struct SharedScan;

struct SharedScanDataIterator;

struct SharedSchema;

struct SharedSnapshot;

struct StringSliceIterator;

/// Represents an owned slice of boolean values allocated by the kernel. Any time the engine
/// receives a `KernelBoolSlice` as a return value from a kernel method, engine is responsible
/// to free that slice, by calling [super::free_bool_slice] exactly once.
struct KernelBoolSlice {
	bool *ptr;
	uintptr_t len;
};

/// An owned slice of u64 row indexes allocated by the kernel. The engine is responsible for
/// freeing this slice by calling [super::free_row_indexes] once.
struct KernelRowIndexArray {
	uint64_t *ptr;
	uintptr_t len;
};

/// Represents an object that crosses the FFI boundary and which outlives the scope that created
/// it. It can be passed freely between rust code and external code. The
///
/// An accompanying [`HandleDescriptor`] trait defines the behavior of each handle type:
///
/// * The true underlying ("target") type the handle represents. For safety reasons, target type
///   must always be [`Send`].
///
/// * Mutable (`Box`-like) vs. shared (`Arc`-like). For safety reasons, the target type of a
///   shared handle must always be [`Send`]+[`Sync`].
///
/// * Sized vs. unsized. Sized types allow handle operations to be implemented more efficiently.
///
/// # Validity
///
/// A `Handle` is _valid_ if all of the following hold:
///
/// * It was created by a call to [`Handle::from`]
/// * Not yet dropped by a call to [`Handle::drop_handle`]
/// * Not yet consumed by a call to [`Handle::into_inner`]
///
/// Additionally, in keeping with the [`Send`] contract, multi-threaded external code must
/// enforce mutual exclusion -- no mutable handle should ever be passed to more than one kernel
/// API call at a time. If thread races are possible, the handle should be protected with a
/// mutex. Due to Rust [reference
/// rules](https://doc.rust-lang.org/book/ch04-02-references-and-borrowing.html#the-rules-of-references),
/// this requirement applies even for API calls that appear to be read-only (because Rust code
/// always receives the handle as mutable).
///
/// NOTE: Because the underlying type is always [`Sync`], multi-threaded external code can
/// freely access shared (non-mutable) handles.
///
template <typename H>
using Handle = H *;

/// An error that can be returned to the engine. Engines that wish to associate additional
/// information can define and use any type that is [pointer
/// interconvertible](https://en.cppreference.com/w/cpp/language/static_cast#pointer-interconvertible)
/// with this one -- e.g. by subclassing this struct or by embedding this struct as the first member
/// of a [standard layout](https://en.cppreference.com/w/cpp/language/data_members#Standard-layout)
/// class.
struct EngineError {
	KernelError etype;
};

/// Semantics: Kernel will always immediately return the leaked engine error to the engine (if it
/// allocated one at all), and engine is responsible for freeing it.
template <typename T>
struct ExternResult {
	enum class Tag {
		Ok,
		Err,
	};

	struct Ok_Body {
		T _0;
	};

	struct Err_Body {
		EngineError *_0;
	};

	Tag tag;
	union {
		Ok_Body ok;
		Err_Body err;
	};
};

/// A non-owned slice of a UTF8 string, intended for arg-passing between kernel and engine. The
/// slice is only valid until the function it was passed into returns, and should not be copied.
///
/// # Safety
///
/// Intentionally not Copy, Clone, Send, nor Sync.
///
/// Whoever instantiates the struct must ensure it does not outlive the data it points to. The
/// compiler cannot help us here, because raw pointers don't have lifetimes. To reduce the risk of
/// accidental misuse, it is recommended to only instantiate this struct as a function arg, by
/// converting a string slice `Into` a `KernelStringSlice`. That way, the borrowed reference at call
/// site protects the `KernelStringSlice` until the function returns. Meanwhile, the callee should
/// assume that the slice is only valid until the function returns, and must not retain any
/// references to the slice or its data that could outlive the function call.
///
/// ```
/// # use delta_kernel_ffi::KernelStringSlice;
/// fn wants_slice(slice: KernelStringSlice) { }
/// let msg = String::from("hello");
/// wants_slice(msg.into());
/// ```
struct KernelStringSlice {
	const char *ptr;
	uintptr_t len;
};

using AllocateErrorFn = EngineError *(*)(KernelError etype, KernelStringSlice msg);

using NullableCvoid = void *;

/// Allow engines to allocate strings of their own type. the contract of calling a passed allocate
/// function is that `kernel_str` is _only_ valid until the return from this function
using AllocateStringFn = NullableCvoid (*)(KernelStringSlice kernel_str);

struct FileMeta {
	KernelStringSlice path;
	int64_t last_modified;
	uintptr_t size;
};

/// Model iterators. This allows an engine to specify iteration however it likes, and we simply wrap
/// the engine functions. The engine retains ownership of the iterator.
struct EngineIterator {
	void *data;
	/// A function that should advance the iterator and return the next time from the data
	/// If the iterator is complete, it should return null. It should be safe to
	/// call `get_next()` multiple times if it returns null.
	const void *(*get_next)(void *data);
};

/// ABI-compatible struct for ArrowArray from C Data Interface
/// See <https://arrow.apache.org/docs/format/CDataInterface.html#structure-definitions>
///
/// ```
/// # use arrow_data::ArrayData;
/// # use arrow_data::ffi::FFI_ArrowArray;
/// fn export_array(array: &ArrayData) -> FFI_ArrowArray {
///     FFI_ArrowArray::new(array)
/// }
/// ```
struct FFI_ArrowArray {
	int64_t length;
	int64_t null_count;
	int64_t offset;
	int64_t n_buffers;
	int64_t n_children;
	const void **buffers;
	FFI_ArrowArray **children;
	FFI_ArrowArray *dictionary;
	void (*release)(FFI_ArrowArray *arg1);
	void *private_data;
};

/// ABI-compatible struct for `ArrowSchema` from C Data Interface
/// See <https://arrow.apache.org/docs/format/CDataInterface.html#structure-definitions>
///
/// ```
/// # use arrow_schema::DataType;
/// # use arrow_schema::ffi::FFI_ArrowSchema;
/// fn array_schema(data_type: &DataType) -> FFI_ArrowSchema {
///     FFI_ArrowSchema::try_from(data_type).unwrap()
/// }
/// ```
///
struct FFI_ArrowSchema {
	const char *format;
	const char *name;
	const char *metadata;
	/// Refer to [Arrow Flags](https://arrow.apache.org/docs/format/CDataInterface.html#c.ArrowSchema.flags)
	int64_t flags;
	int64_t n_children;
	FFI_ArrowSchema **children;
	FFI_ArrowSchema *dictionary;
	void (*release)(FFI_ArrowSchema *arg1);
	void *private_data;
};

#if defined(DEFINE_DEFAULT_ENGINE)
/// Struct to allow binding to the arrow [C Data
/// Interface](https://arrow.apache.org/docs/format/CDataInterface.html). This includes the data and
/// the schema.
struct ArrowFFIData {
	FFI_ArrowArray array;
	FFI_ArrowSchema schema;
};
#endif

/// A predicate that can be used to skip data when scanning.
///
/// When invoking [`scan::scan`], The engine provides a pointer to the (engine's native) predicate,
/// along with a visitor function that can be invoked to recursively visit the predicate. This
/// engine state must be valid until the call to `scan::scan` returns. Inside that method, the
/// kernel allocates visitor state, which becomes the second argument to the predicate visitor
/// invocation along with the engine-provided predicate pointer. The visitor state is valid for the
/// lifetime of the predicate visitor invocation. Thanks to this double indirection, engine and
/// kernel each retain ownership of their respective objects, with no need to coordinate memory
/// lifetimes with the other.
struct EnginePredicate {
	void *predicate;
	uintptr_t (*visitor)(void *predicate, KernelExpressionVisitorState *state);
};

/// Give engines an easy way to consume stats
struct Stats {
	/// For any file where the deletion vector is not present (see [`DvInfo::has_vector`]), the
	/// `num_records` statistic must be present and accurate, and must equal the number of records
	/// in the data file. In the presence of Deletion Vectors the statistics may be somewhat
	/// outdated, i.e. not reflecting deleted rows yet.
	uint64_t num_records;
};

using CScanCallback = void (*)(NullableCvoid engine_context, KernelStringSlice path, int64_t size, const Stats *stats,
                               const DvInfo *dv_info, const CStringMap *partition_map);

// This trickery is from https://github.com/mozilla/cbindgen/issues/402#issuecomment-578680163
struct im_an_unused_struct_that_tricks_msvc_into_compilation {
	ExternResult<KernelBoolSlice> field;
	ExternResult<bool> field2;
	ExternResult<EngineBuilder *> field3;
	ExternResult<Handle<SharedExternEngine>> field4;
	ExternResult<Handle<SharedSnapshot>> field5;
	ExternResult<uintptr_t> field6;
	ExternResult<ArrowFFIData *> field7;
	ExternResult<Handle<SharedScanDataIterator>> field8;
	ExternResult<Handle<SharedScan>> field9;
	ExternResult<Handle<ExclusiveFileReadResultIterator>> field10;
	ExternResult<KernelRowIndexArray> field11;
};

/// The `EngineSchemaVisitor` defines a visitor system to allow engines to build their own
/// representation of a schema from a particular schema within kernel.
///
/// The model is list based. When the kernel needs a list, it will ask engine to allocate one of a
/// particular size. Once allocated the engine returns an `id`, which can be any integer identifier
/// ([`usize`]) the engine wants, and will be passed back to the engine to identify the list in the
/// future.
///
/// Every schema element the kernel visits belongs to some list of "sibling" elements. The schema
/// itself is a list of schema elements, and every complex type (struct, map, array) contains a list
/// of "child" elements.
///  1. Before visiting schema or any complex type, the kernel asks the engine to allocate a list to
///     hold its children
///  2. When visiting any schema element, the kernel passes its parent's "child list" as the
///     "sibling list" the element should be appended to:
///      - For the top-level schema, visit each top-level column, passing the column's name and type
///      - For a struct, first visit each struct field, passing the field's name, type, nullability,
///        and metadata
///      - For a map, visit the key and value, passing its special name ("map_key" or "map_value"),
///        type, and value nullability (keys are never nullable)
///      - For a list, visit the element, passing its special name ("array_element"), type, and
///        nullability
///  3. When visiting a complex schema element, the kernel also passes the "child list" containing
///     that element's (already-visited) children.
///  4. The [`visit_schema`] method returns the id of the list of top-level columns
struct EngineSchemaVisitor {
	/// opaque state pointer
	void *data;
	/// Creates a new field list, optionally reserving capacity up front
	uintptr_t (*make_field_list)(void *data, uintptr_t reserve);
	/// Indicate that the schema contains a `Struct` type. The top level of a Schema is always a
	/// `Struct`. The fields of the `Struct` are in the list identified by `child_list_id`.
	void (*visit_struct)(void *data, uintptr_t sibling_list_id, KernelStringSlice name, uintptr_t child_list_id);
	/// Indicate that the schema contains an Array type. `child_list_id` will be a _one_ item list
	/// with the array's element type
	void (*visit_array)(void *data, uintptr_t sibling_list_id, KernelStringSlice name, bool contains_null,
	                    uintptr_t child_list_id);
	/// Indicate that the schema contains an Map type. `child_list_id` will be a _two_ item list
	/// where the first element is the map's key type and the second element is the
	/// map's value type
	void (*visit_map)(void *data, uintptr_t sibling_list_id, KernelStringSlice name, bool value_contains_null,
	                  uintptr_t child_list_id);
	/// visit a `decimal` with the specified `precision` and `scale`
	void (*visit_decimal)(void *data, uintptr_t sibling_list_id, KernelStringSlice name, uint8_t precision,
	                      uint8_t scale);
	/// Visit a `string` belonging to the list identified by `sibling_list_id`.
	void (*visit_string)(void *data, uintptr_t sibling_list_id, KernelStringSlice name);
	/// Visit a `long` belonging to the list identified by `sibling_list_id`.
	void (*visit_long)(void *data, uintptr_t sibling_list_id, KernelStringSlice name);
	/// Visit an `integer` belonging to the list identified by `sibling_list_id`.
	void (*visit_integer)(void *data, uintptr_t sibling_list_id, KernelStringSlice name);
	/// Visit a `short` belonging to the list identified by `sibling_list_id`.
	void (*visit_short)(void *data, uintptr_t sibling_list_id, KernelStringSlice name);
	/// Visit a `byte` belonging to the list identified by `sibling_list_id`.
	void (*visit_byte)(void *data, uintptr_t sibling_list_id, KernelStringSlice name);
	/// Visit a `float` belonging to the list identified by `sibling_list_id`.
	void (*visit_float)(void *data, uintptr_t sibling_list_id, KernelStringSlice name);
	/// Visit a `double` belonging to the list identified by `sibling_list_id`.
	void (*visit_double)(void *data, uintptr_t sibling_list_id, KernelStringSlice name);
	/// Visit a `boolean` belonging to the list identified by `sibling_list_id`.
	void (*visit_boolean)(void *data, uintptr_t sibling_list_id, KernelStringSlice name);
	/// Visit `binary` belonging to the list identified by `sibling_list_id`.
	void (*visit_binary)(void *data, uintptr_t sibling_list_id, KernelStringSlice name);
	/// Visit a `date` belonging to the list identified by `sibling_list_id`.
	void (*visit_date)(void *data, uintptr_t sibling_list_id, KernelStringSlice name);
	/// Visit a `timestamp` belonging to the list identified by `sibling_list_id`.
	void (*visit_timestamp)(void *data, uintptr_t sibling_list_id, KernelStringSlice name);
	/// Visit a `timestamp` with no timezone belonging to the list identified by `sibling_list_id`.
	void (*visit_timestamp_ntz)(void *data, uintptr_t sibling_list_id, KernelStringSlice name);
};

extern "C" {

/// # Safety
///
/// Caller is responsible for passing a valid handle.
void free_bool_slice(KernelBoolSlice slice);

/// # Safety
///
/// Caller is responsible for passing a valid handle.
void free_row_indexes(KernelRowIndexArray slice);

/// Drop an `ExclusiveEngineData`.
///
/// # Safety
///
/// Caller is responsible for passing a valid handle as engine_data
void free_engine_data(Handle<ExclusiveEngineData> engine_data);

#if defined(DEFINE_DEFAULT_ENGINE)
/// Get a "builder" that can be used to construct an engine. The function
/// [`set_builder_option`] can be used to set options on the builder prior to constructing the
/// actual engine
///
/// # Safety
/// Caller is responsible for passing a valid path pointer.
ExternResult<EngineBuilder *> get_engine_builder(KernelStringSlice path, AllocateErrorFn allocate_error);
#endif

#if defined(DEFINE_DEFAULT_ENGINE)
/// Set an option on the builder
///
/// # Safety
///
/// Caller must pass a valid EngineBuilder pointer, and valid slices for key and value
void set_builder_option(EngineBuilder *builder, KernelStringSlice key, KernelStringSlice value);
#endif

#if defined(DEFINE_DEFAULT_ENGINE)
/// Consume the builder and return a `default` engine. After calling, the passed pointer is _no
/// longer valid_.
///
///
/// # Safety
///
/// Caller is responsible to pass a valid EngineBuilder pointer, and to not use it again afterwards
ExternResult<Handle<SharedExternEngine>> builder_build(EngineBuilder *builder);
#endif

#if defined(DEFINE_DEFAULT_ENGINE)
/// # Safety
///
/// Caller is responsible for passing a valid path pointer.
ExternResult<Handle<SharedExternEngine>> get_default_engine(KernelStringSlice path, AllocateErrorFn allocate_error);
#endif

#if defined(DEFINE_SYNC_ENGINE)
/// # Safety
///
/// Caller is responsible for passing a valid path pointer.
ExternResult<Handle<SharedExternEngine>> get_sync_engine(AllocateErrorFn allocate_error);
#endif

/// # Safety
///
/// Caller is responsible for passing a valid handle.
void free_engine(Handle<SharedExternEngine> engine);

/// Get the latest snapshot from the specified table
///
/// # Safety
///
/// Caller is responsible for passing valid handles and path pointer.
ExternResult<Handle<SharedSnapshot>> snapshot(KernelStringSlice path, Handle<SharedExternEngine> engine);

/// # Safety
///
/// Caller is responsible for passing a valid handle.
void free_snapshot(Handle<SharedSnapshot> snapshot);

/// Get the version of the specified snapshot
///
/// # Safety
///
/// Caller is responsible for passing a valid handle.
uint64_t version(Handle<SharedSnapshot> snapshot);

/// Get the resolved root of the table. This should be used in any future calls that require
/// constructing a path
///
/// # Safety
///
/// Caller is responsible for passing a valid handle.
NullableCvoid snapshot_table_root(Handle<SharedSnapshot> snapshot, AllocateStringFn allocate_fn);

/// # Safety
///
/// The iterator must be valid (returned by [kernel_scan_data_init]) and not yet freed by
/// [kernel_scan_data_free]. The visitor function pointer must be non-null.
bool string_slice_next(Handle<StringSliceIterator> data, NullableCvoid engine_context,
                       void (*engine_visitor)(NullableCvoid engine_context, KernelStringSlice slice));

/// # Safety
///
/// Caller is responsible for (at most once) passing a valid pointer to a [`StringSliceIterator`]
void free_string_slice_data(Handle<StringSliceIterator> data);

/// Call the engine back with the next `EngingeData` batch read by Parquet/Json handler. The
/// _engine_ "owns" the data that is passed into the `engine_visitor`, since it is allocated by the
/// `Engine` being used for log-replay. If the engine wants the kernel to free this data, it _must_
/// call [`free_engine_data`] on it.
///
/// # Safety
///
/// The iterator must be valid (returned by [`read_parquet_file`]) and not yet freed by
/// [`free_read_result_iter`]. The visitor function pointer must be non-null.
ExternResult<bool> read_result_next(Handle<ExclusiveFileReadResultIterator> data, NullableCvoid engine_context,
                                    void (*engine_visitor)(NullableCvoid engine_context,
                                                           Handle<ExclusiveEngineData> engine_data));

/// Free the memory from the passed read result iterator
/// # Safety
///
/// Caller is responsible for (at most once) passing a valid pointer returned by a call to
/// [`read_parquet_file`].
void free_read_result_iter(Handle<ExclusiveFileReadResultIterator> data);

/// Use the specified engine's [`delta_kernel::ParquetHandler`] to read the specified file.
///
/// # Safety
/// Caller is responsible for calling with a valid `ExternEngineHandle` and `FileMeta`
ExternResult<Handle<ExclusiveFileReadResultIterator>>
read_parquet_file(Handle<SharedExternEngine> engine, const FileMeta *file, Handle<SharedSchema> physical_schema);

uintptr_t visit_expression_and(KernelExpressionVisitorState *state, EngineIterator *children);

uintptr_t visit_expression_lt(KernelExpressionVisitorState *state, uintptr_t a, uintptr_t b);

uintptr_t visit_expression_le(KernelExpressionVisitorState *state, uintptr_t a, uintptr_t b);

uintptr_t visit_expression_gt(KernelExpressionVisitorState *state, uintptr_t a, uintptr_t b);

uintptr_t visit_expression_ge(KernelExpressionVisitorState *state, uintptr_t a, uintptr_t b);

uintptr_t visit_expression_eq(KernelExpressionVisitorState *state, uintptr_t a, uintptr_t b);

/// # Safety
/// The string slice must be valid
ExternResult<uintptr_t> visit_expression_column(KernelExpressionVisitorState *state, KernelStringSlice name,
                                                AllocateErrorFn allocate_error);

uintptr_t visit_expression_not(KernelExpressionVisitorState *state, uintptr_t inner_expr);

uintptr_t visit_expression_is_null(KernelExpressionVisitorState *state, uintptr_t inner_expr);

/// # Safety
/// The string slice must be valid
ExternResult<uintptr_t> visit_expression_literal_string(KernelExpressionVisitorState *state, KernelStringSlice value,
                                                        AllocateErrorFn allocate_error);

uintptr_t visit_expression_literal_int(KernelExpressionVisitorState *state, int32_t value);

uintptr_t visit_expression_literal_long(KernelExpressionVisitorState *state, int64_t value);

uintptr_t visit_expression_literal_short(KernelExpressionVisitorState *state, int16_t value);

uintptr_t visit_expression_literal_byte(KernelExpressionVisitorState *state, int8_t value);

uintptr_t visit_expression_literal_float(KernelExpressionVisitorState *state, float value);

uintptr_t visit_expression_literal_double(KernelExpressionVisitorState *state, double value);

uintptr_t visit_expression_literal_bool(KernelExpressionVisitorState *state, bool value);

/// Get the number of rows in an engine data
///
/// # Safety
/// `data_handle` must be a valid pointer to a kernel allocated `ExclusiveEngineData`
uintptr_t engine_data_length(Handle<ExclusiveEngineData> *data);

/// Allow an engine to "unwrap" an [`ExclusiveEngineData`] into the raw pointer for the case it wants
/// to use its own engine data format
///
/// # Safety
///
/// `data_handle` must be a valid pointer to a kernel allocated `ExclusiveEngineData`. The Engine must
/// ensure the handle outlives the returned pointer.
void *get_raw_engine_data(Handle<ExclusiveEngineData> data);

#if defined(DEFINE_DEFAULT_ENGINE)
/// Get an [`ArrowFFIData`] to allow binding to the arrow [C Data
/// Interface](https://arrow.apache.org/docs/format/CDataInterface.html). This includes the data and
/// the schema.
///
/// # Safety
/// data_handle must be a valid ExclusiveEngineData as read by the
/// [`delta_kernel::engine::default::DefaultEngine`] obtained from `get_default_engine`.
ExternResult<ArrowFFIData *> get_raw_arrow_data(Handle<ExclusiveEngineData> data, Handle<SharedExternEngine> engine);
#endif

/// Drops a scan.
/// # Safety
/// Caller is responsible for passing a [valid][Handle#Validity] scan handle.
void free_scan(Handle<SharedScan> scan);

/// Get a [`Scan`] over the table specified by the passed snapshot.
/// # Safety
///
/// Caller is responsible for passing a valid snapshot pointer, and engine pointer
ExternResult<Handle<SharedScan>> scan(Handle<SharedSnapshot> snapshot, Handle<SharedExternEngine> engine,
                                      EnginePredicate *predicate);

/// Get the global state for a scan. See the docs for [`delta_kernel::scan::state::GlobalScanState`]
/// for more information.
///
/// # Safety
/// Engine is responsible for providing a valid scan pointer
Handle<SharedGlobalScanState> get_global_scan_state(Handle<SharedScan> scan);

/// Get the kernel view of the physical read schema that an engine should read from parquet file in
/// a scan
///
/// # Safety
/// Engine is responsible for providing a valid GlobalScanState pointer
Handle<SharedSchema> get_global_read_schema(Handle<SharedGlobalScanState> state);

/// Free a global read schema
///
/// # Safety
/// Engine is responsible for providing a valid schema obtained via [`get_global_read_schema`]
void free_global_read_schema(Handle<SharedSchema> schema);

/// Get a count of the number of partition columns for this scan
///
/// # Safety
/// Caller is responsible for passing a valid global scan pointer.
uintptr_t get_partition_column_count(Handle<SharedGlobalScanState> state);

/// Get an iterator of the list of partition columns for this scan.
///
/// # Safety
/// Caller is responsible for passing a valid global scan pointer.
Handle<StringSliceIterator> get_partition_columns(Handle<SharedGlobalScanState> state);

/// # Safety
///
/// Caller is responsible for passing a valid global scan state pointer.
void free_global_scan_state(Handle<SharedGlobalScanState> state);

/// Get an iterator over the data needed to perform a scan. This will return a
/// [`KernelScanDataIterator`] which can be passed to [`kernel_scan_data_next`] to get the actual
/// data in the iterator.
///
/// # Safety
///
/// Engine is responsible for passing a valid [`SharedExternEngine`] and [`SharedScan`]
ExternResult<Handle<SharedScanDataIterator>> kernel_scan_data_init(Handle<SharedExternEngine> engine,
                                                                   Handle<SharedScan> scan);

/// # Safety
///
/// The iterator must be valid (returned by [kernel_scan_data_init]) and not yet freed by
/// [`free_kernel_scan_data`]. The visitor function pointer must be non-null.
ExternResult<bool> kernel_scan_data_next(Handle<SharedScanDataIterator> data, NullableCvoid engine_context,
                                         void (*engine_visitor)(NullableCvoid engine_context,
                                                                Handle<ExclusiveEngineData> engine_data,
                                                                KernelBoolSlice selection_vector));

/// # Safety
///
/// Caller is responsible for (at most once) passing a valid pointer returned by a call to
/// [`kernel_scan_data_init`].
void free_kernel_scan_data(Handle<SharedScanDataIterator> data);

/// allow probing into a CStringMap. If the specified key is in the map, kernel will call
/// allocate_fn with the value associated with the key and return the value returned from that
/// function. If the key is not in the map, this will return NULL
///
/// # Safety
///
/// The engine is responsible for providing a valid [`CStringMap`] pointer and [`KernelStringSlice`]
NullableCvoid get_from_map(const CStringMap *map, KernelStringSlice key, AllocateStringFn allocate_fn);

/// Get a selection vector out of a [`DvInfo`] struct
///
/// # Safety
/// Engine is responsible for providing valid pointers for each argument
ExternResult<KernelBoolSlice> selection_vector_from_dv(const DvInfo *dv_info, Handle<SharedExternEngine> engine,
                                                       Handle<SharedGlobalScanState> state);

/// Get a vector of row indexes out of a [`DvInfo`] struct
///
/// # Safety
/// Engine is responsible for providing valid pointers for each argument
ExternResult<KernelRowIndexArray> row_indexes_from_dv(const DvInfo *dv_info, Handle<SharedExternEngine> engine,
                                                      Handle<SharedGlobalScanState> state);

/// Shim for ffi to call visit_scan_data. This will generally be called when iterating through scan
/// data which provides the data handle and selection vector as each element in the iterator.
///
/// # Safety
/// engine is responsbile for passing a valid [`ExclusiveEngineData`] and selection vector.
void visit_scan_data(Handle<ExclusiveEngineData> data, KernelBoolSlice selection_vec, NullableCvoid engine_context,
                     CScanCallback callback);

/// Visit the schema of the passed `SnapshotHandle`, using the provided `visitor`. See the
/// documentation of [`EngineSchemaVisitor`] for a description of how this visitor works.
///
/// This method returns the id of the list allocated to hold the top level schema columns.
///
/// # Safety
///
/// Caller is responsible for passing a valid snapshot handle and schema visitor.
uintptr_t visit_schema(Handle<SharedSnapshot> snapshot, EngineSchemaVisitor *visitor);

} // extern "C"

} // namespace ffi
