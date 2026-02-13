
// MSVC C2526 workaround: force-instantiate ExternResult<T> specializations
// inside extern "C" before any function declarations use them as return types.
// See https://github.com/mozilla/cbindgen/issues/402#issuecomment-578680163
//
// If MSVC fails with C2526 for a new ExternResult<T>, add it here.
struct msvc_compat_force_instantiate {
  ExternResult<bool> _0;
  ExternResult<uint64_t> _1;
  ExternResult<uintptr_t> _2;
  ExternResult<KernelBoolSlice> _3;
  ExternResult<KernelRowIndexArray> _4;
  ExternResult<NullableCvoid> _5;
  ExternResult<ArrowFFIData> _6;
  ExternResult<ArrowFFIData*> _7;
  ExternResult<EngineBuilder*> _8;
  ExternResult<Handle<SharedExternEngine>> _9;
  ExternResult<Handle<SharedSnapshot>> _10;
  ExternResult<Handle<SharedScan>> _11;
  ExternResult<Handle<SharedScanMetadataIterator>> _12;
  ExternResult<Handle<ExclusiveFileReadResultIterator>> _13;
  ExternResult<Handle<ExclusiveEngineData>> _14;
  ExternResult<Handle<ExclusiveTransaction>> _15;
  ExternResult<Handle<SharedExpressionEvaluator>> _16;
  ExternResult<Handle<ExclusiveTableChanges>> _17;
  ExternResult<Handle<SharedTableChangesScan>> _18;
  ExternResult<Handle<SharedScanTableChangesIterator>> _19;
  ExternResult<Handle<SharedExpression>> _20;
  ExternResult<Handle<SharedPredicate>> _21;
  ExternResult<OptionalValue<int64_t>> _22;
  OptionalValue<Handle<SharedExpression>> _23;
};

