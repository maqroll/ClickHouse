#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>
#include <Functions/IFunctionImpl.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <Functions/extractTimeZoneFromFunctionArguments.h>
#include <Functions/DateTimeTransforms.h>
#include <IO/WriteHelpers.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

template<class Transform>
struct WithDateTime64Converter : public Transform
{
    UInt8 scale;
    Transform transform;

    explicit WithDateTime64Converter(UInt8 scale_, Transform transform_ = {})
        : scale(scale_),
          transform(std::move(transform_))
    {}

    inline auto execute(DataTypeDateTime64::FieldType t, const DateLUTImpl & time_zone) const
    {
        auto x = DateTime64(t);
        auto res = transform.execute(static_cast<UInt32>(DecimalUtils::getWholePart(x, scale)), time_zone);
        return res;
    }
};


/// See DateTimeTransforms.h
template <typename ToDataType, typename Transform>
class FunctionDateOrDateTimeToSomething : public IFunction
{
public:
    static constexpr auto name = Transform::name;
    static FunctionPtr create(const Context &) { return std::make_shared<FunctionDateOrDateTimeToSomething>(); }

    String getName() const override
    {
        return name;
    }

    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        if (arguments.size() == 1)
        {
            if (!isDateOrDateTime(arguments[0].type))
                throw Exception(
                    "Illegal type " + arguments[0].type->getName() + " of argument of function " + getName()
                        + ". Should be a date or a date with time",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
        }
        else if (arguments.size() == 2)
        {
            if (!isDateOrDateTime(arguments[0].type))
                throw Exception(
                    "Illegal type " + arguments[0].type->getName() + " of argument of function " + getName()
                        + ". Should be a date or a date with time",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
            if (!isString(arguments[1].type))
                throw Exception(
                    "Function " + getName() + " supports 1 or 2 arguments. The 1st argument "
                          "must be of type Date or DateTime. The 2nd argument (optional) must be "
                          "a constant string with timezone name",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
            if (isDate(arguments[0].type) && std::is_same_v<ToDataType, DataTypeDate>)
                throw Exception(
                    "The timezone argument of function " + getName() + " is allowed only when the 1st argument has the type DateTime",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
        }
        else
            throw Exception(
                "Number of arguments for function " + getName() + " doesn't match: passed " + toString(arguments.size())
                    + ", should be 1 or 2",
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        /// For DateTime, if time zone is specified, attach it to type.
        if constexpr (std::is_same_v<ToDataType, DataTypeDateTime>)
            return std::make_shared<ToDataType>(extractTimeZoneNameFromFunctionArguments(arguments, 1, 0));
        if constexpr (std::is_same_v<ToDataType, DataTypeDateTime64>)
            return std::make_shared<ToDataType>(extractTimeZoneNameFromFunctionArguments(arguments, 1, 0));
        else
            return std::make_shared<ToDataType>();
    }

    bool useDefaultImplementationForConstants() const override { return true; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const override { return {1}; }

    void executeImpl(Block & block, const ColumnNumbers & arguments, size_t result, size_t input_rows_count) override
    {
        const IDataType * from_type = block.getByPosition(arguments[0]).type.get();
        WhichDataType which(from_type);

        if (which.isDate())
            DateTimeTransformImpl<DataTypeDate, ToDataType, Transform>::execute(block, arguments, result, input_rows_count);
        else if (which.isDateTime())
            DateTimeTransformImpl<DataTypeDateTime, ToDataType, Transform>::execute(block, arguments, result, input_rows_count);
        else if (which.isDateTime64())
        {
            const auto scale = static_cast<const DataTypeDateTime64 *>(from_type)->getScale();
            WithDateTime64Converter<Transform> transformer(scale);

            DateTimeTransformImpl<DataTypeDateTime64, ToDataType, WithDateTime64Converter<Transform>>::execute(block, arguments, result, input_rows_count, transformer);
        }
        else
            throw Exception("Illegal type " + block.getByPosition(arguments[0]).type->getName() + " of argument of function " + getName(),
                ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
    }


    bool hasInformationAboutMonotonicity() const override
    {
        return true;
    }

    Monotonicity getMonotonicityForRange(const IDataType & type, const Field & left, const Field & right) const override
    {
        IFunction::Monotonicity is_monotonic { true };
        IFunction::Monotonicity is_not_monotonic;

        if (std::is_same_v<typename Transform::FactorTransform, ZeroTransform>)
        {
            is_monotonic.is_always_monotonic = true;
            return is_monotonic;
        }

        /// This method is called only if the function has one argument. Therefore, we do not care about the non-local time zone.
        const DateLUTImpl & date_lut = DateLUT::instance();

        if (left.isNull() || right.isNull())
            return is_not_monotonic;

        /// The function is monotonous on the [left, right] segment, if the factor transformation returns the same values for them.

        if (checkAndGetDataType<DataTypeDate>(&type))
        {
            return Transform::FactorTransform::execute(UInt16(left.get<UInt64>()), date_lut)
                == Transform::FactorTransform::execute(UInt16(right.get<UInt64>()), date_lut)
                ? is_monotonic : is_not_monotonic;
        }
        else
        {
            return Transform::FactorTransform::execute(UInt32(left.get<UInt64>()), date_lut)
                == Transform::FactorTransform::execute(UInt32(right.get<UInt64>()), date_lut)
                ? is_monotonic : is_not_monotonic;
        }
    }
};

}

