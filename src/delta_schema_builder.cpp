#include "delta_schema_builder.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

ffi::EngineSchema DeltaSchemaBuilder::CreateEngineSchema() {
	ffi::EngineSchema schema;
	schema.schema = this;
	schema.visitor = Build;
	return schema;
}

uintptr_t DeltaSchemaBuilder::Unpack(ffi::ExternResult<uintptr_t> result) {
	uintptr_t field_id = 0;
	auto unpack_error = KernelUtils::TryUnpackResult(result, field_id);
	if (unpack_error.HasError()) {
		unpack_error.Throw();
	}
	return field_id;
}

uintptr_t DeltaSchemaBuilder::VisitField(ffi::KernelSchemaVisitorState *state, const string &name,
                                         const LogicalType &type, bool nullable) {
	auto name_slice = KernelUtils::ToDeltaString(name);
	auto allocate_error = DuckDBEngineError::AllocateError;

	// Mirrors SchemaVisitor's kernel->DuckDB mapping, so a create/read round trip is lossless.
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return Unpack(ffi::visit_field_boolean(state, name_slice, nullable, allocate_error));
	case LogicalTypeId::TINYINT:
		return Unpack(ffi::visit_field_byte(state, name_slice, nullable, allocate_error));
	case LogicalTypeId::SMALLINT:
		return Unpack(ffi::visit_field_short(state, name_slice, nullable, allocate_error));
	case LogicalTypeId::INTEGER:
		return Unpack(ffi::visit_field_integer(state, name_slice, nullable, allocate_error));
	case LogicalTypeId::BIGINT:
		return Unpack(ffi::visit_field_long(state, name_slice, nullable, allocate_error));
	case LogicalTypeId::FLOAT:
		return Unpack(ffi::visit_field_float(state, name_slice, nullable, allocate_error));
	case LogicalTypeId::DOUBLE:
		return Unpack(ffi::visit_field_double(state, name_slice, nullable, allocate_error));
	case LogicalTypeId::VARCHAR:
		return Unpack(ffi::visit_field_string(state, name_slice, nullable, allocate_error));
	case LogicalTypeId::BLOB:
		return Unpack(ffi::visit_field_binary(state, name_slice, nullable, allocate_error));
	case LogicalTypeId::DATE:
		return Unpack(ffi::visit_field_date(state, name_slice, nullable, allocate_error));
	case LogicalTypeId::TIMESTAMP_TZ:
		return Unpack(ffi::visit_field_timestamp(state, name_slice, nullable, allocate_error));
	case LogicalTypeId::TIMESTAMP:
		return Unpack(ffi::visit_field_timestamp_ntz(state, name_slice, nullable, allocate_error));
	case LogicalTypeId::DECIMAL:
		return Unpack(ffi::visit_field_decimal(state, name_slice, DecimalType::GetWidth(type),
		                                       DecimalType::GetScale(type), nullable, allocate_error));
	case LogicalTypeId::STRUCT: {
		auto &children = StructType::GetChildTypes(type);
		vector<uintptr_t> child_ids;
		for (idx_t i = 0; i < children.size(); i++) {
			child_ids.push_back(VisitField(state, children[i].first, children[i].second, true));
		}
		return Unpack(ffi::visit_field_struct(state, name_slice, child_ids.data(), child_ids.size(), nullable,
		                                      allocate_error));
	}
	case LogicalTypeId::LIST: {
		auto element_id = VisitField(state, "element", ListType::GetChildType(type), true);
		return Unpack(ffi::visit_field_array(state, name_slice, element_id, nullable, allocate_error));
	}
	case LogicalTypeId::MAP: {
		auto key_id = VisitField(state, "key", MapType::KeyType(type), false);
		auto value_id = VisitField(state, "value", MapType::ValueType(type), true);
		return Unpack(ffi::visit_field_map(state, name_slice, key_id, value_id, nullable, allocate_error));
	}
	default:
		throw NotImplementedException("Delta CREATE TABLE does not support column '%s' of type %s", name,
		                              type.ToString());
	}
}

uintptr_t DeltaSchemaBuilder::Build(void *data, ffi::KernelSchemaVisitorState *state) {
	auto &builder = *static_cast<DeltaSchemaBuilder *>(data);

	try {
		vector<uintptr_t> field_ids;
		for (auto &col : builder.columns.Logical()) {
			field_ids.push_back(builder.VisitField(state, col.Name().GetIdentifierName(), col.Type(), true));
		}

		// Kernel discards the root struct's name; the C reference example passes a placeholder too.
		auto root_name = KernelUtils::ToDeltaString("root");
		return builder.Unpack(ffi::visit_field_struct(state, root_name, field_ids.data(), field_ids.size(), false,
		                                              DuckDBEngineError::AllocateError));
	} catch (std::exception &e) {
		builder.error = ErrorData(e);
	} catch (...) {
		builder.error = ErrorData(ExceptionType::INTERNAL, "Unknown error building Delta schema");
	}

	// 0 is kernel's "no such field id", which fails the schema extraction on the kernel side.
	return 0;
}

} // namespace duckdb
