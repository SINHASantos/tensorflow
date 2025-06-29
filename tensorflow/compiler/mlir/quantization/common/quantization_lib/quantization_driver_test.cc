/* Copyright 2024 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/mlir/quantization/common/quantization_lib/quantization_driver.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/strings/string_view.h"
#include "llvm/ADT/DenseMap.h"
#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/Quant/IR/QuantTypes.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/OwningOpRef.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/quantization/common/attrs_and_constraints.h"  // IWYU pragma: keep
#include "tensorflow/compiler/mlir/quantization/common/func.h"
#include "tensorflow/compiler/mlir/quantization/common/ir/QuantOps.h"
#include "tensorflow/compiler/mlir/quantization/common/quantization_lib/quantization_utils.h"
#include "tensorflow/compiler/mlir/quantization/common/test_base.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"

namespace mlir::quant {
namespace {

using ApplyQuantizationParamsPropagationTest =
    mlir::quant::QuantizationTestBase;
using ::testing::IsEmpty;
using ::testing::Not;

constexpr absl::string_view kModuleTFLite = R"mlir(
  module {
    func.func @main(%arg0: tensor<1x4x4x3xf32>) -> tensor<1x4x4x3xf32> attributes {_from_xla_call_module} {
      %cst_0 = arith.constant dense<1.0> : tensor<3x1x1x3xf32>
      %cst_1 = arith.constant dense<2.0> : tensor<3xf32>
      %0 = "tf.XlaCallModule"(%arg0, %cst_0, %cst_1) <{Sout = [#tf_type.shape<1x4x4x3>], module = "", version = 9 : i64}> {_entry_function = @composite_fn_1, _stablehlo_version = "1.0.0", _original_entry_function = "composite_fn_1", _tfl_quant_trait = "fully_quantizable"} : (tensor<1x4x4x3xf32>, tensor<3x1x1x3xf32>, tensor<3xf32>) -> tensor<1x4x4x3xf32>
      %1 = "tf.XlaCallModule"(%0, %cst_0, %cst_1) <{Sout = [#tf_type.shape<1x4x4x3>], module = "", version = 9 : i64}> {_entry_function = @composite_fn_2, _stablehlo_version = "1.0.0", _original_entry_function = "composite_fn_2", _tfl_quant_trait = "fully_quantizable"} : (tensor<1x4x4x3xf32>, tensor<3x1x1x3xf32>, tensor<3xf32>) -> tensor<1x4x4x3xf32>
      return %1 : tensor<1x4x4x3xf32>
    }
    func.func private @composite_fn_1(%arg0: tensor<1x4x4x3xf32>, %arg1: tensor<3x1x1x3xf32>, %arg2: tensor<3xf32>) -> tensor<1x4x4x3xf32> attributes {tf_quant.composite_function} {
      %perm = "tf.Const"() { value = dense<[1, 2, 3, 0]> : tensor<4xi32> } : () -> tensor<4xi32>
      %filter_hwio = "tf.Transpose"(%arg1, %perm) : (tensor<3x1x1x3xf32>, tensor<4xi32>) -> tensor<1x1x3x3xf32>
      %0 = "tf.Conv2D"(%arg0, %filter_hwio) {
        dilations = [1, 1, 1, 1],
        strides = [1, 1, 1, 1],
        padding = "VALID",
        data_format = "NHWC"
      } : (tensor<1x4x4x3xf32>, tensor<1x1x3x3xf32>) -> tensor<1x4x4x3xf32>
      %1 = "tf.BiasAdd"(%0, %arg2) {
        data_format = "NHWC"
      } : (tensor<1x4x4x3xf32>, tensor<3xf32>) -> tensor<1x4x4x3xf32>
      %2 = "tf.Relu"(%1) : (tensor<1x4x4x3xf32>) -> tensor<1x4x4x3xf32>
      return %2 : tensor<1x4x4x3xf32>
    }
    func.func private @composite_fn_2(%arg0: tensor<1x4x4x3xf32>, %arg1: tensor<3x1x1x3xf32>, %arg2: tensor<3xf32>) -> tensor<1x4x4x3xf32> attributes {tf_quant.composite_function} {
      %perm = "tf.Const"() { value = dense<[1, 2, 3, 0]> : tensor<4xi32> } : () -> tensor<4xi32>
      %filter_hwio = "tf.Transpose"(%arg1, %perm) : (tensor<3x1x1x3xf32>, tensor<4xi32>) -> tensor<1x1x3x3xf32>
      %0 = "tf.Conv2D"(%arg0, %filter_hwio) {
        dilations = [1, 1, 1, 1],
        strides = [1, 1, 1, 1],
        padding = "VALID",
        data_format = "NHWC"
      } : (tensor<1x4x4x3xf32>, tensor<1x1x3x3xf32>) -> tensor<1x4x4x3xf32>
      %1 = "tf.BiasAdd"(%0, %arg2) {
        data_format = "NHWC"
      } : (tensor<1x4x4x3xf32>, tensor<3xf32>) -> tensor<1x4x4x3xf32>
      %2 = "tf.Relu"(%1) : (tensor<1x4x4x3xf32>) -> tensor<1x4x4x3xf32>
      return %2 : tensor<1x4x4x3xf32>
    }
  }
)mlir";

// TOOD: b/323478683 - Directly use types rather than creating a `unique_ptr`.
std::unique_ptr<OpQuantSpec> GetOpQuantSpec(
    const mlir::Operation* op,
    bool disable_per_channel_for_dense_layers = false) {
  auto spec = std::make_unique<OpQuantSpec>();
  spec->coeff_op_quant_dim[1] = 3;
  spec->biases_params[2] = {{0, 1}, GetUniformQuantizedTypeForBias};
  for (const auto& [key, value] : spec->coeff_op_quant_dim) {
    spec->quantizable_operands.insert(key);
  }
  return spec;
}

TEST_F(ApplyQuantizationParamsPropagationTest,
       ConstsUsedMultipleTimesAreDuplicated) {
  const OwningOpRef<ModuleOp> module_op_ref =
      mlir::quant::QuantizationTestBase::ParseModuleOpString(kModuleTFLite);
  func::FuncOp main_fn = mlir::quant::FindMainFuncOp(*module_op_ref);

  auto op_quant_spec_getter = [&](mlir::Operation* op) {
    return GetOpQuantSpec(op, /*disable_per_channel_for_dense_layers=*/false);
  };
  QuantizationDriver quantization_driver(
      main_fn, /*is_signed=*/true, /*bit_width=*/8,
      /*disable_per_channel=*/false, op_quant_spec_getter,
      GetDefaultQuantScaleSpec,
      /*infer_tensor_range=*/true, /*legacy_float_scale=*/false,
      /*is_qdq_conversion=*/false);

  quantization_driver.Initialize();

  int64_t num_constant_op = 0;
  main_fn.walk([&](arith::ConstantOp cst) { ++num_constant_op; });
  EXPECT_EQ(num_constant_op, 4);
}

TEST_F(ApplyQuantizationParamsPropagationTest,
       PropagateParamsCreatesQuantState) {
  const OwningOpRef<ModuleOp> module_op_ref =
      ParseModuleOpString(kModuleTFLite);
  func::FuncOp main_fn = mlir::quant::FindMainFuncOp(*module_op_ref);

  auto op_quant_spec_getter = [&](mlir::Operation* op) {
    return GetOpQuantSpec(op, /*disable_per_channel_for_dense_layers=*/false);
  };
  QuantizationDriver quantization_driver(
      main_fn, /*is_signed=*/true, /*bit_width=*/8,
      /*disable_per_channel=*/false, op_quant_spec_getter,
      GetDefaultQuantScaleSpec,
      /*infer_tensor_range=*/true, /*legacy_float_scale=*/false,
      /*is_qdq_conversion=*/false);

  quantization_driver.Initialize();
  ASSERT_TRUE(quantization_driver.PropagateParamsAndReturnIfChanged());
  EXPECT_THAT(quantization_driver.GetArgs(), Not(IsEmpty()));

  for (const auto& arg : quantization_driver.GetArgs()) {
    const QuantState& state = quantization_driver.GetArgQuantState(arg);
    EXPECT_TRUE(isa<QuantizedType>(state.params));
  }
  for (const auto& result : quantization_driver.GetResultStates()) {
    Operation* op = result.first.first;
    const int res_index = result.first.second;
    const QuantState state =
        quantization_driver.GetResultQuantState(op, res_index);
    EXPECT_TRUE(isa<QuantizedType>(state.params));
  }
}

TEST_F(ApplyQuantizationParamsPropagationTest, FinalizeInsertsQDQOps) {
  const OwningOpRef<ModuleOp> module_op_ref =
      ParseModuleOpString(kModuleTFLite);
  func::FuncOp main_fn = mlir::quant::FindMainFuncOp(*module_op_ref);

  auto op_quant_spec_getter = [&](mlir::Operation* op) {
    return GetOpQuantSpec(op, /*disable_per_channel_for_dense_layers=*/false);
  };
  ApplyQuantizationParamsPropagation(
      main_fn, /*is_signed=*/true, /*bit_width=*/8,
      /*disable_per_channel=*/false, op_quant_spec_getter,
      /*infer_tensor_ranges=*/true, /*legacy_float_scale=*/false,
      /*is_qdq_conversion=*/false);
  Operation* xla_call_module_op =
      mlir::quant::FindOperationOfType<TF::XlaCallModuleOp>(main_fn);
  Operation* filter_dcast_op =
      xla_call_module_op->getOperand(1).getDefiningOp();
  Operation* filter_qcast_op = filter_dcast_op->getOperand(0).getDefiningOp();
  ASSERT_NE(filter_qcast_op, nullptr);
  EXPECT_TRUE(isa<mlir::quant::ir::QuantizeCastOp>(filter_qcast_op));
  EXPECT_TRUE(isa<mlir::quant::ir::DequantizeCastOp>(filter_dcast_op));
  EXPECT_TRUE(isa<UniformQuantizedPerAxisType>(
      mlir::cast<TensorType>(filter_qcast_op->getResult(0).getType())
          .getElementType()));
}

}  // namespace
}  // namespace mlir::quant
