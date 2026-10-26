#include <ydb/core/io_formats/cell_maker/cell_maker.h>
#include <library/cpp/testing/unittest/registar.h>
#include <cstring>

namespace NKikimr::NFormats {
namespace {

template <typename T>
void CheckInline(const NJson::TJsonValue& json, TStringBuf text, NScheme::TTypeId type, T expected) {
    for (bool fromJson : {false, true}) {
        TMemoryPool pool(256);
        TString error;
        TCell cell;
        const bool ok = fromJson
            ? MakeCell(cell, json, NScheme::TTypeInfo(type), pool, error)
            : MakeCell(cell, text, NScheme::TTypeInfo(type), pool, error);
        UNIT_ASSERT_C(ok, error);
        UNIT_ASSERT(cell.IsInline());
        UNIT_ASSERT_VALUES_EQUAL(pool.MemoryAllocated(), 0);
        pool.Clear();
        memset(pool.Allocate(256), 0xff, 256);
        UNIT_ASSERT_VALUES_EQUAL(cell.AsValue<T>(), expected);
    }
}

}

Y_UNIT_TEST_SUITE(CellMaker) {
    Y_UNIT_TEST(InlineNumericValuesOwnTheirBytes) {
        CheckInline(NJson::TJsonValue(true), "true", NScheme::NTypeIds::Bool, true);
        CheckInline(NJson::TJsonValue(i64(-128)), "-128", NScheme::NTypeIds::Int8, i8(-128));
        CheckInline(NJson::TJsonValue(ui64(255)), "255", NScheme::NTypeIds::Uint8, ui8(255));
        CheckInline(NJson::TJsonValue(i64(-32768)), "-32768", NScheme::NTypeIds::Int16, i16(-32768));
        CheckInline(NJson::TJsonValue(ui64(65535)), "65535", NScheme::NTypeIds::Uint16, ui16(65535));
        CheckInline(NJson::TJsonValue(i64(-2147483648LL)), "-2147483648", NScheme::NTypeIds::Int32, i32(-2147483647 - 1));
        CheckInline(NJson::TJsonValue(ui64(4294967295ULL)), "4294967295", NScheme::NTypeIds::Uint32, ui32(4294967295ULL));
        CheckInline(NJson::TJsonValue(i64(-9223372036854775807LL - 1)), "-9223372036854775808", NScheme::NTypeIds::Int64, i64(-9223372036854775807LL - 1));
        CheckInline(NJson::TJsonValue(ui64(18446744073709551615ULL)), "18446744073709551615", NScheme::NTypeIds::Uint64, ui64(18446744073709551615ULL));
        CheckInline(NJson::TJsonValue(1.25), "1.25", NScheme::NTypeIds::Float, float(1.25));
        CheckInline(NJson::TJsonValue(1.25), "1.25", NScheme::NTypeIds::Double, double(1.25));
    }

    Y_UNIT_TEST(InlineDateConversion) {
        const NJson::TJsonValue date("1970-01-02T00:00:00Z");
        CheckInline(date, date.GetString(), NScheme::NTypeIds::Date, ui16(1));
        CheckInline(date, date.GetString(), NScheme::NTypeIds::Datetime, ui32(86400));
        CheckInline(date, date.GetString(), NScheme::NTypeIds::Timestamp, ui64(86400000000ULL));
        CheckInline(NJson::TJsonValue(i64(-1)), "-1", NScheme::NTypeIds::Interval, i64(-1));
    }

    Y_UNIT_TEST(WideValuesStillUsePool) {
        const NScheme::TTypeInfo type(NScheme::TDecimalType(22, 9));
        TMemoryPool jsonPool(256), textPool(256);
        TCell fromJson, fromText;
        TString error;
        UNIT_ASSERT_C(MakeCell(fromJson, NJson::TJsonValue("123.45"), type, jsonPool, error), error);
        UNIT_ASSERT_C(MakeCell(fromText, TStringBuf("123.45"), type, textPool, error), error);
        UNIT_ASSERT(!fromJson.IsInline());
        UNIT_ASSERT(!fromText.IsInline());
        UNIT_ASSERT_VALUES_EQUAL(fromJson.Size(), 16);
        UNIT_ASSERT_VALUES_EQUAL(fromJson.AsBuf(), fromText.AsBuf());
        UNIT_ASSERT(jsonPool.MemoryAllocated() >= 16);
        UNIT_ASSERT(textPool.MemoryAllocated() >= 16);
    }
}
}
