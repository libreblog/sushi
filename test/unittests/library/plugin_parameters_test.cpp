#include "gtest/gtest.h"

#include "library/plugin_parameters.h"
#include "test_utils.h"

#define private public

using namespace sushi;

class TestParameterPreProcessor : public ::testing::Test
{
protected:
    TestParameterPreProcessor() {}

    ParameterPreProcessor<float> _module_under_test{-10, 10};
};

TEST_F(TestParameterPreProcessor, TestClipping)
{
    EXPECT_FLOAT_EQ(1.0f, _module_under_test.process(1.0f));
    EXPECT_FLOAT_EQ(10.0f, _module_under_test.process(10.0f));
    EXPECT_FLOAT_EQ(-4.0, _module_under_test.process(-4.0f));

    EXPECT_FLOAT_EQ(-10.0, _module_under_test.process(-14.0f));
    EXPECT_FLOAT_EQ(10.0, _module_under_test.process(267890.5f));
}


class TestdBToLinPreProcessor : public ::testing::Test
{
protected:
    TestdBToLinPreProcessor() {}

    dBToLinPreProcessor _module_under_test{-24.0f, 24.0f};
};

TEST_F(TestdBToLinPreProcessor, TestProcessing)
{
    EXPECT_NEAR(1.0, _module_under_test.process(0.0f), test_utils::DECIBEL_ERROR);
    EXPECT_NEAR(2.0, _module_under_test.process(6.0f), test_utils::DECIBEL_ERROR);
    EXPECT_NEAR(0.25, _module_under_test.process(-12.0f), test_utils::DECIBEL_ERROR);
}

/*
 * Templated testing is difficult since we want to test with different values for each type
 * Therefore we test each type separately.
 */

class TestStompBoxParameter : public ::testing::Test
{
protected:
    TestStompBoxParameter() {}

    FloatStompBoxParameter _module_under_test_float{"float_parameter", "FloatParameter", 1.0f, new ParameterPreProcessor<float>(-10, 10)};
    IntStompBoxParameter _module_under_test_int{"int_parameter", "IntParameter", 0, new ParameterPreProcessor<int>(-10, 10)};
    BoolStompBoxParameter _module_under_test_bool{"bool_parameter", "BoolParameter", false, new ParameterPreProcessor<bool>(0, 1)};
};

TEST_F(TestStompBoxParameter, TestType)
{
    EXPECT_EQ(StompBoxParameterType::BOOL, _module_under_test_bool.type());
    EXPECT_EQ(StompBoxParameterType::FLOAT, _module_under_test_float.type());
    EXPECT_EQ(StompBoxParameterType::INT, _module_under_test_int.type());
}

TEST_F(TestStompBoxParameter, TestDefaultValue)
{
    EXPECT_FLOAT_EQ(1.0f, _module_under_test_float.value());
    EXPECT_EQ(0, _module_under_test_int.value());
    EXPECT_FALSE(_module_under_test_bool.value());

    EXPECT_FLOAT_EQ(1.0f, _module_under_test_float.raw_value());
    EXPECT_EQ(0, _module_under_test_int.raw_value());
    EXPECT_FALSE(_module_under_test_bool.raw_value());
}

TEST_F(TestStompBoxParameter, TestSet)
{
    _module_under_test_float.set(13.25f);
    EXPECT_FLOAT_EQ(10.0f, _module_under_test_float.value());
    EXPECT_FLOAT_EQ(13.25f, _module_under_test_float.raw_value());

    _module_under_test_int.set(5);
    EXPECT_EQ(5, _module_under_test_int.value());
    EXPECT_EQ(5, _module_under_test_int.raw_value());

    _module_under_test_bool.set(true);
    EXPECT_TRUE(_module_under_test_bool.value());
    EXPECT_TRUE(_module_under_test_bool.raw_value());
}

TEST_F(TestStompBoxParameter, TestAsString)
{
    EXPECT_EQ("1.000000", _module_under_test_float.as_string());
    EXPECT_EQ("0", _module_under_test_int.as_string());
    EXPECT_EQ("False", _module_under_test_bool.as_string());
    // Test that we get the correct format even when we call as_string() when
    // The parameter is accessed as an instance of BaseStompBoxParameter
    EXPECT_EQ("1.000000", static_cast<BaseStompBoxParameter*>(&_module_under_test_float)->as_string());
    EXPECT_EQ("False", static_cast<BaseStompBoxParameter*>(&_module_under_test_bool)->as_string());
}