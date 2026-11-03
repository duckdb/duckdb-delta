#include "delta_schema_builder.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string_util.hpp"

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

optional_ptr<const LogicalType> DeltaSchemaBuilder::FindUnsupportedType(const LogicalType &type) {
	// Every type VisitField has a case for, and nothing else
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::VARIANT:
		return nullptr;
	case LogicalTypeId::STRUCT:
		for (auto &child : StructType::GetChildTypes(type)) {
			auto result = FindUnsupportedType(child.second);
			if (result) {
				return result;
			}
		}
		return nullptr;
	case LogicalTypeId::LIST:
		return FindUnsupportedType(ListType::GetChildType(type));
	case LogicalTypeId::MAP: {
		auto result = FindUnsupportedType(MapType::KeyType(type));
		return result ? result : FindUnsupportedType(MapType::ValueType(type));
	}
	default:
		return &type;
	}
}

uintptr_t DeltaSchemaBuilder::VisitField(ffi::KernelSchemaVisitorState *state, const string &name,
                                         const LogicalType &type, bool nullable) {
	auto name_slice = KernelUtils::ToDeltaString(name);
	// Explicitly typed: AllocateError is overloaded, so `auto` cannot pick an overload here.
	ffi::AllocateErrorFn allocate_error = DuckDBEngineError::AllocateError;

	// FindUnsupportedType is the one list of the types Delta can store; every type it accepts has a case below
	auto unsupported = FindUnsupportedType(type);
	if (unsupported) {
		auto nested =
		    unsupported.get() == &type ? string() : StringUtil::Format(" (it holds a %s)", unsupported->ToString());
		throw NotImplementedException("Delta CREATE TABLE does not support column '%s' of type %s%s", name,
		                              type.ToString(), nested);
	}

	// Mirrors SchemaVisitor's kernel->DuckDB mapping, so a create/read round trip is lossless.
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return Unpack(ffi::visit_field_boolean(state, name_slice, nullable, nullptr, allocate_error));
	case LogicalTypeId::TINYINT:
		return Unpack(ffi::visit_field_byte(state, name_slice, nullable, nullptr, allocate_error));
	case LogicalTypeId::SMALLINT:
		return Unpack(ffi::visit_field_short(state, name_slice, nullable, nullptr, allocate_error));
	case LogicalTypeId::INTEGER:
		return Unpack(ffi::visit_field_integer(state, name_slice, nullable, nullptr, allocate_error));
	case LogicalTypeId::BIGINT:
		return Unpack(ffi::visit_field_long(state, name_slice, nullable, nullptr, allocate_error));
	case LogicalTypeId::FLOAT:
		return Unpack(ffi::visit_field_float(state, name_slice, nullable, nullptr, allocate_error));
	case LogicalTypeId::DOUBLE:
		return Unpack(ffi::visit_field_double(state, name_slice, nullable, nullptr, allocate_error));
	case LogicalTypeId::VARCHAR:
		return Unpack(ffi::visit_field_string(state, name_slice, nullable, nullptr, allocate_error));
	case LogicalTypeId::BLOB:
		return Unpack(ffi::visit_field_binary(state, name_slice, nullable, nullptr, allocate_error));
	case LogicalTypeId::DATE:
		return Unpack(ffi::visit_field_date(state, name_slice, nullable, nullptr, allocate_error));
	case LogicalTypeId::TIMESTAMP_TZ:
		return Unpack(ffi::visit_field_timestamp(state, name_slice, nullable, nullptr, allocate_error));
	case LogicalTypeId::TIMESTAMP:
		return Unpack(ffi::visit_field_timestamp_ntz(state, name_slice, nullable, nullptr, allocate_error));
	case LogicalTypeId::DECIMAL:
		return Unpack(ffi::visit_field_decimal(state, name_slice, DecimalType::GetWidth(type),
		                                       DecimalType::GetScale(type), nullable, nullptr, allocate_error));
	case LogicalTypeId::VARIANT: {
		// Delta declares a variant by its unshredded physical struct (metadata, value) no matter how files
		// are shredded later; shredding is a per-file parquet layout, allowed only when
		// delta.enableVariantShredding is true. Kernel's CREATE TABLE refuses that property, so tables made
		// here are always written unshredded.
		const string metadata_name = "metadata";
		const string value_name = "value";
		uintptr_t physical_ids[2] = {Unpack(ffi::visit_field_binary(state, KernelUtils::ToDeltaString(metadata_name),
		                                                            false, nullptr, allocate_error)),
		                             Unpack(ffi::visit_field_binary(state, KernelUtils::ToDeltaString(value_name),
		                                                            false, nullptr, allocate_error))};
		auto physical_id =
		    Unpack(ffi::visit_field_struct(state, name_slice, physical_ids, 2, false, nullptr, allocate_error));
		return Unpack(ffi::visit_field_variant(state, name_slice, physical_id, nullable, nullptr, allocate_error));
	}
	case LogicalTypeId::STRUCT: {
		auto &children = StructType::GetChildTypes(type);
		vector<uintptr_t> child_ids;
		for (idx_t i = 0; i < children.size(); i++) {
			child_ids.push_back(VisitField(state, children[i].first.GetIdentifierName(), children[i].second, true));
		}
		return Unpack(ffi::visit_field_struct(state, name_slice, child_ids.data(), child_ids.size(), nullable, nullptr,
		                                      allocate_error));
	}
	case LogicalTypeId::LIST: {
		auto element_id = VisitField(state, "element", ListType::GetChildType(type), true);
		return Unpack(ffi::visit_field_array(state, name_slice, element_id, nullable, nullptr, allocate_error));
	}
	case LogicalTypeId::MAP: {
		auto key_id = VisitField(state, "key", MapType::KeyType(type), false);
		auto value_id = VisitField(state, "value", MapType::ValueType(type), true);
		return Unpack(ffi::visit_field_map(state, name_slice, key_id, value_id, nullable, nullptr, allocate_error));
	}
	default:
		throw InternalException("Delta schema builder has no case for type %s, which FindUnsupportedType accepts",
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
		// ToDeltaString borrows, so the slice must never outlive a temporary -- keep the string named.
		const string root_name = "root";
		return builder.Unpack(ffi::visit_field_struct(state, KernelUtils::ToDeltaString(root_name), field_ids.data(),
		                                              field_ids.size(), false, nullptr,
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
