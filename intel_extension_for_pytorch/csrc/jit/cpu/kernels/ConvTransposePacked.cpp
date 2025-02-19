#include "ConvTransposePacked.h"
#include "csrc/aten/cpu/ConvTranspose.h"
#include "csrc/aten/cpu/ParamUtils.h"
#include "csrc/aten/cpu/WeightPack.h"
#include "csrc/cpu/ideep/IDeepConversions.h"
#include "csrc/cpu/ideep/ideep.hpp"
#include "csrc/utils/ipex_op_profile.h"

namespace torch_ipex {
namespace cpu {
namespace detail {
namespace conv_transpose2d {

c10::intrusive_ptr<ConvTransposeOpContext> createConvTransposePrePackOpContext(
    at::Tensor&& weight,
    c10::optional<at::Tensor>&& bias,
    std::vector<int64_t>&& stride,
    std::vector<int64_t>&& padding,
    std::vector<int64_t>&& output_padding,
    int64_t groups,
    std::vector<int64_t>&& dilation,
    std::vector<int64_t>&& kernel_size,
    int64_t output_channel,
    bool weight_is_channels_last,
    std::vector<int64_t>&& input_size) {
  IPEX_RECORD_FUNCTION(
      "ipex_prepack::createConvTransposePrePackOpContext",
      std::vector<c10::IValue>({}));

  return IpexConvTransposeOpContext::create_context(
      std::move(weight),
      std::move(bias),
      std::move(stride),
      std::move(padding),
      std::move(output_padding),
      std::move(dilation),
      std::move(kernel_size),
      groups,
      output_channel,
      weight_is_channels_last,
      std::move(input_size));
}

at::Tensor conv_transpose2d_run(
    const at::Tensor& input,
    const c10::intrusive_ptr<ConvTransposeOpContext>& op_context) {
  IPEX_RECORD_FUNCTION(
      "ipex_prepack::conv_transpose2d_run", std::vector<c10::IValue>({}));

  return op_context->run(input, ideep::attr_t());
}

ContextConvTranspose create(
    const at::Tensor& weight,
    const c10::optional<at::Tensor>& bias,
    const at::IntArrayRef stride,
    const at::IntArrayRef padding,
    const at::IntArrayRef output_padding,
    const at::IntArrayRef dilation,
    const at::IntArrayRef kernel_size,
    const int64_t groups,
    const int64_t output_channel,
    const bool weight_is_channels_last,
    const at::IntArrayRef input_size) {
  const auto stride_expanded = expand_param_if_needed(stride, "stride", 2);
  const auto padding_expanded = expand_param_if_needed(padding, "padding", 2);
  const auto output_padding_expanded =
      expand_param_if_needed(output_padding, "output_padding", 2);
  const auto dilation_expanded =
      expand_param_if_needed(dilation, "dilation", 2);

  bool weight_is_channels_last_ = weight_is_channels_last;

  weight_is_channels_last_ =
      weight.suggest_memory_format() == at::MemoryFormat::ChannelsLast;
  auto memory_format = weight_is_channels_last_ ? at::MemoryFormat::ChannelsLast
                                                : at::MemoryFormat::Contiguous;
  auto weight_ = weight.contiguous(memory_format);

  auto w = itensor_view_from_dense(weight_);
  ideep::tensor::desc ori_desc(w.get_desc());
  ideep::data_type dtype = w.get_data_type();
  // TODO: adjust padding_r
  auto expected_desc = get_conv_transpose2d_expected_weights_desc(
      w.get_dims(),
      dtype,
      {stride_expanded.begin(), stride_expanded.end()},
      {padding_expanded.begin(), padding_expanded.end()},
      {padding_expanded.begin(), padding_expanded.end()},
      {dilation_expanded.begin(), dilation_expanded.end()},
      groups,
      weight_is_channels_last_,
      ideep::algorithm::deconvolution_direct,
      dtype,
      input_size.vec());

  auto weight_dtype = w.get_data_type();
  expected_desc = expected_desc.to_type(weight_dtype);
  auto at_weight = empty_aten_tensor_from_desc(expected_desc, weight.options());
  ideep::tensor packed_weight;
  if (ideep::data_type::f32 == weight_dtype) {
    packed_weight.init(expected_desc, at_weight.template data_ptr<float>());
  } else {
    packed_weight.init(
        expected_desc, at_weight.template data_ptr<c10::BFloat16>());
  }

  w.transpose_(0, 1);
  auto w_transpose = w.make_grouped_weights(groups, true);
  packed_weight.feed_from(w_transpose);

  return ContextConvTranspose{
      std::move(ori_desc),
      std::move(packed_weight),
      std::move(at_weight),
      bias.has_value() ? c10::make_optional(*bias) : c10::nullopt,
      {padding_expanded[0], padding_expanded[1]},
      {output_padding_expanded[0], output_padding_expanded[1]},
      {stride_expanded[0], stride_expanded[1]},
      {dilation_expanded[0], dilation_expanded[1]},
      kernel_size.vec(),
      groups,
      input_size.vec(),
      weight.sizes().vec(),
      weight_is_channels_last_};
}

at::Tensor run(
    const ContextConvTranspose& context,
    const at::Tensor& input,
    const ideep::attr_t& attr) {
  bool use_channels_last =
      input.suggest_memory_format() == at::MemoryFormat::ChannelsLast ||
      context.weight_is_channels_last_;
  auto memory_format = use_channels_last ? at::MemoryFormat::ChannelsLast
                                         : at::MemoryFormat::Contiguous;
  auto input_ = input.contiguous(memory_format);

  return conv_transpose2d_kernel_impl(
      input_,
      context.weight_packed_,
      context.bias_,
      context.stride_,
      context.padding_,
      context.output_padding_,
      context.groups_,
      context.dilation_,
      context.origin_weight_dims_,
      attr);
}

std::tuple<at::Tensor, at::Tensor, at::Tensor> run_backward(
    ContextConvTranspose& context,
    const at::Tensor& input,
    const at::Tensor& grad_output,
    std::array<bool, 3> output_mask) {
  return conv_transpose2d_backward_kernel_impl(
      input,
      grad_output,
      context.at_weight_,
      context.weight_packed_,
      context.stride_,
      context.padding_,
      context.output_padding_,
      context.groups_,
      context.dilation_,
      context.kernel_size_,
      output_mask,
      context.weight_is_channels_last_);
}

at::Tensor get_at_packed_weight(ContextConvTranspose& context) {
  return context.at_weight_;
}

at::Tensor pack(ContextConvTranspose& context, const at::Tensor& tensor) {
  auto ideep_tensor = itensor_view_from_dense(tensor);
  auto dtype = ideep_tensor.get_data_type();
  auto expected_desc = context.weight_packed_.get_desc().to_type(dtype);
  auto packed_at_tensor =
      empty_aten_tensor_from_desc(expected_desc, tensor.options());
  ideep::tensor packed_tensor;
  if (ideep::data_type::f32 == dtype) {
    packed_tensor.init(
        expected_desc, packed_at_tensor.template data_ptr<float>());
  } else {
    packed_tensor.init(
        expected_desc, packed_at_tensor.template data_ptr<c10::BFloat16>());
  }
  ideep_tensor.transpose_(0, 1);
  auto ideep_tensor_transpose =
      ideep_tensor.make_grouped_weights(context.groups_, true);
  packed_tensor.feed_from(ideep_tensor_transpose);
  return packed_at_tensor;
}

at::Tensor unpack(ContextConvTranspose& context, const at::Tensor& tensor) {
  auto dtype = get_mkldnn_dtype(tensor.scalar_type());
  auto expected_desc = context.weight_packed_.get_desc().to_type(dtype);
  ideep::tensor blocked_tensor;
  if (ideep::data_type::f32 == dtype) {
    blocked_tensor.init(expected_desc, tensor.template data_ptr<float>());
  } else {
    blocked_tensor.init(
        expected_desc, tensor.template data_ptr<c10::BFloat16>());
  }

  at::Tensor result = at::empty(context.origin_weight_dims_, tensor.options());
  if (context.weight_is_channels_last_) {
    result = result.to(at::MemoryFormat::ChannelsLast);
  }
  ideep::tensor pub_tensor = itensor_view_from_dense(result);
  auto pub_tensor_desc = context.original_desc_.to_type(dtype);
  if (ideep::data_type::f32 == dtype) {
    pub_tensor.init(pub_tensor_desc, result.template data_ptr<float>());
  } else {
    pub_tensor.init(pub_tensor_desc, result.template data_ptr<c10::BFloat16>());
  }
  pub_tensor.transpose_(0, 1);
  pub_tensor = pub_tensor.make_grouped_weights(context.groups_, true);
  pub_tensor.feed_from(blocked_tensor);
  return result;
}

void repack_for(
    ContextConvTranspose& context,
    std::vector<int64_t> input_size) {
  auto dtype = context.original_desc_.get_data_type();
  ideep::tensor packed_weight;
  auto packed_desc = get_conv_transpose2d_expected_weights_desc(
      context.origin_weight_dims_,
      dtype,
      context.stride_,
      context.padding_,
      context.padding_,
      context.dilation_,
      context.groups_,
      context.weight_is_channels_last_,
      ideep::algorithm::deconvolution_direct,
      dtype,
      input_size);
  auto new_at_weight =
      empty_aten_tensor_from_desc(packed_desc, context.at_weight_.options());
  if (ideep::data_type::f32 == dtype) {
    packed_weight.init(packed_desc, new_at_weight.template data_ptr<float>());
  } else {
    packed_weight.init(
        packed_desc, new_at_weight.template data_ptr<c10::BFloat16>());
  }
  packed_weight.feed_from(context.weight_packed_);
  context.at_weight_ = new_at_weight;
  context.weight_packed_ = packed_weight;
}

} // namespace conv_transpose2d
} // namespace detail
} // namespace cpu
} // namespace torch_ipex
