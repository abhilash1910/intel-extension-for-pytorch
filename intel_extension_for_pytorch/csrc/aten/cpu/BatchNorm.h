#pragma once

#include <ATen/ATen.h>
#include <ATen/Tensor.h>
#include <torch/csrc/autograd/custom_function.h>

#include "csrc/cpu/ideep/ideep.hpp"

namespace torch_ipex {
namespace cpu {

std::tuple<at::Tensor, at::Tensor, at::Tensor> batch_norm_forward(
    const at::Tensor& input,
    const at::Tensor& weight,
    const at::Tensor& bias,
    const c10::optional<at::Tensor>& running_mean_opt,
    const c10::optional<at::Tensor>& running_var_opt,
    bool train,
    double momentum,
    double eps);

std::tuple<at::Tensor, at::Tensor, at::Tensor> batch_norm_backward(
    const at::Tensor& grad_output,
    const at::Tensor& input,
    const at::Tensor& weight,
    const at::Tensor& save_mean,
    const at::Tensor& save_var,
    bool train,
    double eps,
    std::array<bool, 3> grad_input_mask);

class IPEXBatchNormOp : public torch::autograd::Function<IPEXBatchNormOp> {
 public:
  static at::Tensor forward(
      torch::autograd::AutogradContext* ctx,
      const at::Tensor& input,
      const at::Tensor& weight,
      const at::Tensor& bias,
      const c10::optional<at::Tensor>& running_mean_opt,
      const c10::optional<at::Tensor>& running_var_opt,
      bool train,
      double momentum,
      double eps);

  static torch::autograd::variable_list backward(
      torch::autograd::AutogradContext* ctx,
      torch::autograd::variable_list grad_outputs);
};

at::Tensor batch_norm(
    const at::Tensor& input,
    const c10::optional<at::Tensor>& weight_opt,
    const c10::optional<at::Tensor>& bias_opt,
    const c10::optional<at::Tensor>& running_mean_opt,
    const c10::optional<at::Tensor>& running_var_opt,
    bool train,
    double momentum,
    double eps,
    bool cudnn_enabled);

at::Tensor frozen_batch_norm(
    const at::Tensor& input,
    const at::Tensor& weight,
    const at::Tensor& bias,
    const at::Tensor& running_mean,
    const at::Tensor& running_var,
    double eps);

} // namespace cpu
} // namespace torch_ipex
