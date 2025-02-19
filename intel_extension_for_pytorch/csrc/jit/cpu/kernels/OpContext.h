
#pragma once

#include <ATen/Tensor.h>
#include <torch/custom_class.h>

#include "ContextConvTranspose.h"
#include "ContextConvolution.h"
#include "ContextLinear.h"
#include "csrc/cpu/ideep/ideep.hpp"

namespace torch_ipex {
namespace cpu {

using SerializationTypeConvolutionPrePack = std::tuple<
    at::Tensor,
    c10::optional<at::Tensor>,
    std::vector<int64_t>,
    std::vector<int64_t>,
    std::vector<int64_t>,
    std::vector<int64_t>,
    int64_t,
    int64_t,
    bool,
    std::vector<int64_t>>;

class ConvolutionOpContext : public torch::jit::CustomClassHolder {
 protected:
  // these origin parameters are used for serialization
  at::Tensor orig_weight_;
  c10::optional<at::Tensor> orig_bias_;
  std::vector<int64_t> stride_;
  std::vector<int64_t> padding_;
  std::vector<int64_t> dilation_;
  std::vector<int64_t> kernel_size_;
  int64_t groups_;
  int64_t output_channel_;
  bool weight_is_channels_last_;
  std::vector<int64_t> input_size_;

 public:
  SerializationTypeConvolutionPrePack unpack() {
    return std::make_tuple(
        orig_weight_,
        orig_bias_,
        stride_,
        padding_,
        dilation_,
        kernel_size_,
        groups_,
        output_channel_,
        weight_is_channels_last_,
        input_size_);
  }

  virtual at::Tensor run(
      const at::Tensor& input,
      const ideep::attr_t& attr) = 0;
  virtual at::Tensor& run(
      const at::Tensor& input,
      at::Tensor& accumu,
      const ideep::attr_t& attr) = 0;

  // Runing backward for conv by given grad_output, input and grad_masks.
  // Will using the mkldnn_weight/bias stored in the context
  virtual std::tuple<at::Tensor, at::Tensor, at::Tensor> run_backward(
      const at::Tensor& input,
      const at::Tensor& grad_output,
      std::array<bool, 3> output_mask) = 0;

  // Return the n-D ATen weight which sharing same memory with the mkldnn packed
  // weight This n-D ATen weight will be used for autograd and optimizer update
  virtual at::Tensor get_at_packed_weight() = 0;

  // Pack given tensor to same format with mkldnn packed weight
  virtual at::Tensor pack(const at::Tensor& tensor) = 0;

  // Unpack given tensor to same format with original public format for weight
  virtual at::Tensor to_public(const at::Tensor& tensor) = 0;

  std::vector<int64_t> get_stride();

  std::vector<int64_t> get_padding();

  std::vector<int64_t> get_dilation();

  int64_t get_groups();

  virtual detail::ContextConvolution& get_conetxt() = 0;
};

class IpexConvolutionOpContext final : public ConvolutionOpContext {
 private:
  detail::ContextConvolution op_context_;

 public:
  IpexConvolutionOpContext(
      at::Tensor&& weight,
      c10::optional<at::Tensor>&& bias,
      std::vector<int64_t>&& stride,
      std::vector<int64_t>&& padding,
      std::vector<int64_t>&& dilation,
      std::vector<int64_t>&& kernel_size,
      int64_t groups,
      int64_t output_channel,
      bool weight_is_channels_last,
      std::vector<int64_t>&& input_size,
      detail::ContextConvolution&& op_context)
      : op_context_(std::move(op_context)) {
    orig_weight_ = std::move(weight);
    orig_bias_ = std::move(bias);
    stride_ = std::move(stride);
    padding_ = std::move(padding);
    dilation_ = std::move(dilation);
    kernel_size_ = std::move(kernel_size);
    input_size_ = std::move(input_size);
    groups_ = groups;
    output_channel_ = output_channel;
    weight_is_channels_last_ = weight_is_channels_last;
  }

  virtual at::Tensor run(const at::Tensor& input, const ideep::attr_t& attr)
      override;

  virtual at::Tensor& run(
      const at::Tensor& input,
      at::Tensor& accumu,
      const ideep::attr_t& attr) override;

  virtual std::tuple<at::Tensor, at::Tensor, at::Tensor> run_backward(
      const at::Tensor& input,
      const at::Tensor& grad_output,
      std::array<bool, 3> output_mask) override;

  virtual at::Tensor get_at_packed_weight() override;

  virtual at::Tensor pack(const at::Tensor& tensor) override;

  virtual at::Tensor to_public(const at::Tensor& tensor) override;

  virtual detail::ContextConvolution& get_conetxt() override;

  static c10::intrusive_ptr<ConvolutionOpContext> create_context(
      at::Tensor&& weight,
      c10::optional<at::Tensor>&& bias,
      std::vector<int64_t>&& stride,
      std::vector<int64_t>&& padding,
      std::vector<int64_t>&& dilation,
      std::vector<int64_t>&& kernel_size,
      int64_t groups,
      int64_t output_channel,
      bool weight_is_channels_last,
      std::vector<int64_t>&& input_size,
      const ideep::attr_t& attr);
};

// linear op
using SerializationTypeLinearPrePack = std::tuple<
    at::Tensor,
    c10::optional<at::Tensor>,
    int64_t,
    int64_t,
    c10::optional<int64_t>>;

class LinearOpContext : public torch::jit::CustomClassHolder {
 protected:
  // these origin parameters are used for serialization
  at::Tensor orig_weight_;
  c10::optional<at::Tensor> orig_bias_;
  // these shapes related args are used for calculate shapes in concat linear
  int64_t out_features_;
  int64_t in_features_;
  c10::optional<int64_t> batch_size_;

 public:
  SerializationTypeLinearPrePack unpack() {
    return std::make_tuple(
        orig_weight_, orig_bias_, out_features_, in_features_, batch_size_);
  }

  virtual at::Tensor run(
      const at::Tensor& input,
      const ideep::attr_t& attr) = 0;

  virtual at::Tensor& run(
      const at::Tensor& input,
      at::Tensor& accumu,
      const ideep::attr_t& attr) = 0;

  // Runing backward for linear by given grad_output, input and grad_masks.
  // Will using the mkldnn_weight stored in the context
  virtual std::tuple<at::Tensor, at::Tensor, at::Tensor> run_backward(
      const at::Tensor& input,
      const at::Tensor& grad_output,
      std::array<bool, 3> output_mask) = 0;

  // Return the n-D ATen weight which sharing same memory with the mkldnn packed
  // weight This n-D ATen weight will be used for autograd and optimizer update
  virtual at::Tensor get_at_packed_weight() = 0;

  // update the bias stored in context
  virtual void set_bias(at::Tensor& tensor) = 0;

  // update the weight stored in context (update both n-D ATen weight and mkldnn
  // weight)
  virtual void set_weight(at::Tensor& tensor) = 0;

  // Pack given tensor to same format with mkldnn packed weight
  virtual at::Tensor pack(const at::Tensor& tensor) = 0;

  // Unpack given tensor to same format with original public format for weight
  virtual at::Tensor to_public(const at::Tensor& tensor) = 0;

  // query best weight format by given input size, and re-pack the mkldnn weight
  // to newly queried format
  virtual void may_repack(int64_t batch_size) = 0;

  int64_t get_out_features();

  int64_t get_in_features();

  c10::optional<int64_t> get_batchsize();
};

class IpexLinearOpContext final : public LinearOpContext {
 private:
  detail::ContextLinear op_context_;

 public:
  IpexLinearOpContext(
      at::Tensor&& weight,
      c10::optional<at::Tensor>&& bias,
      int64_t out_features,
      int64_t in_features,
      c10::optional<int64_t> batch_size,
      detail::ContextLinear&& op_context)
      : op_context_(std::move(op_context)) {
    orig_weight_ = std::move(weight);
    orig_bias_ = std::move(bias);
    out_features_ = out_features;
    in_features_ = in_features;
    batch_size_ = batch_size;
  }

  virtual at::Tensor run(const at::Tensor& input, const ideep::attr_t& attr)
      override;

  virtual at::Tensor& run(
      const at::Tensor& input,
      at::Tensor& accumu,
      const ideep::attr_t& attr) override;

  virtual std::tuple<at::Tensor, at::Tensor, at::Tensor> run_backward(
      const at::Tensor& input,
      const at::Tensor& grad_output,
      std::array<bool, 3> output_mask) override;

  virtual at::Tensor get_at_packed_weight() override;

  virtual void set_bias(at::Tensor& tensor) override;

  virtual void set_weight(at::Tensor& tensor) override;

  virtual at::Tensor pack(const at::Tensor& tensor) override;

  virtual at::Tensor to_public(const at::Tensor& tensor) override;

  virtual void may_repack(int64_t batch_size) override;

  static c10::intrusive_ptr<LinearOpContext> create_context(
      at::Tensor&& weight,
      c10::optional<at::Tensor>&& bias,
      int64_t out_features,
      int64_t in_features,
      c10::optional<int64_t> batch_size);
};

// deconv op
using SerializationTypeConvTransposePrePack = std::tuple<
    at::Tensor,
    c10::optional<at::Tensor>,
    std::vector<int64_t>,
    std::vector<int64_t>,
    std::vector<int64_t>,
    int64_t,
    std::vector<int64_t>,
    std::vector<int64_t>,
    int64_t,
    bool,
    std::vector<int64_t>>;

class ConvTransposeOpContext : public torch::jit::CustomClassHolder {
 protected:
  // these origin parameters are used for serialization
  at::Tensor orig_weight_;
  c10::optional<at::Tensor> orig_bias_;
  std::vector<int64_t> stride_;
  std::vector<int64_t> padding_;
  std::vector<int64_t> output_padding_;
  std::vector<int64_t> dilation_;
  std::vector<int64_t> kernel_size_;
  std::vector<int64_t> input_size_;
  int64_t groups_;
  int64_t output_channel_;
  bool weight_is_channels_last_;

 public:
  SerializationTypeConvTransposePrePack unpack() {
    return std::make_tuple(
        orig_weight_,
        orig_bias_,
        stride_,
        padding_,
        output_padding_,
        groups_,
        dilation_,
        kernel_size_,
        output_channel_,
        weight_is_channels_last_,
        input_size_);
  }

  virtual at::Tensor run(
      const at::Tensor& input,
      const ideep::attr_t& attr) = 0;

  // Runing backward for conv_transpose by given grad_output, input and
  // grad_masks. Will using the mkldnn_weight stored in the context
  virtual std::tuple<at::Tensor, at::Tensor, at::Tensor> run_backward(
      const at::Tensor& input,
      const at::Tensor& grad_output,
      std::array<bool, 3> output_mask) = 0;

  // Return the n-D ATen weight which sharing same memory with the mkldnn packed
  // weight This n-D ATen weight will be used for autograd and optimizer update
  virtual at::Tensor get_at_packed_weight() = 0;

  // Pack given tensor to same format with mkldnn packed weight
  virtual at::Tensor pack(const at::Tensor& tensor) = 0;

  // Unpack given tensor to same format with original public format for weight
  virtual at::Tensor to_public(const at::Tensor& tensor) = 0;

  // query best weight format by given input size, and re-pack the mkldnn weight
  // to newly queried format
  virtual void may_repack(std::vector<int64_t> input_size) = 0;
};

class IpexConvTransposeOpContext final : public ConvTransposeOpContext {
 private:
  detail::ContextConvTranspose op_context_;

 public:
  IpexConvTransposeOpContext(
      at::Tensor&& weight,
      c10::optional<at::Tensor>&& bias,
      std::vector<int64_t>&& stride,
      std::vector<int64_t>&& padding,
      std::vector<int64_t>&& output_padding,
      std::vector<int64_t>&& dilation,
      std::vector<int64_t>&& kernel_size,
      std::vector<int64_t>&& input_size,
      int64_t groups,
      int64_t output_channel,
      bool weight_is_channels_last,
      detail::ContextConvTranspose&& op_context)
      : op_context_(std::move(op_context)) {
    orig_weight_ = std::move(weight);
    orig_bias_ = std::move(bias);
    stride_ = std::move(stride);
    padding_ = std::move(padding);
    output_padding_ = std::move(output_padding);
    dilation_ = std::move(dilation);
    kernel_size_ = std::move(kernel_size);
    input_size_ = std::move(input_size);
    groups_ = groups;
    output_channel_ = output_channel;
    weight_is_channels_last_ = weight_is_channels_last;
  }

  virtual at::Tensor run(const at::Tensor& input, const ideep::attr_t& attr)
      override;

  virtual std::tuple<at::Tensor, at::Tensor, at::Tensor> run_backward(
      const at::Tensor& input,
      const at::Tensor& grad_output,
      std::array<bool, 3> output_mask) override;

  virtual at::Tensor get_at_packed_weight() override;

  virtual at::Tensor pack(const at::Tensor& tensor) override;

  virtual at::Tensor to_public(const at::Tensor& tensor) override;

  virtual void may_repack(std::vector<int64_t> input_size) override;

  static c10::intrusive_ptr<ConvTransposeOpContext> create_context(
      at::Tensor&& weight,
      c10::optional<at::Tensor>&& bias,
      std::vector<int64_t>&& stride,
      std::vector<int64_t>&& padding,
      std::vector<int64_t>&& output_padding,
      std::vector<int64_t>&& dilation,
      std::vector<int64_t>&& kernel_size,
      int64_t groups,
      int64_t output_channel,
      bool weight_is_channels_last,
      std::vector<int64_t>&& input_size);
};

} // namespace cpu
} // namespace torch_ipex
