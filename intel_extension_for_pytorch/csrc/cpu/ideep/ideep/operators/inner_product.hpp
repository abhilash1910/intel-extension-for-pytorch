#ifndef IDEEP_OPERATORS_INNER_PRODUCT_HPP
#define IDEEP_OPERATORS_INNER_PRODUCT_HPP

namespace ideep {

struct inner_product_forward
    : public dnnl::inner_product_forward,
      utils::computation_cache<dnnl::inner_product_forward::primitive_desc> {
  using super = dnnl::inner_product_forward;

  static void compute(
      const tensor& src,
      const tensor& weights,
      const tensor& bias,
      tensor& dst,
      const scale_t& src_scales = scale_t(),
      const scale_t& weights_scales = scale_t(),
      const scale_t& dst_scales = scale_t(),
      const attr_t& attr = attr_t(),
      const prop_kind aprop_kind = prop_kind::forward,
      const lowp_kind alowp_kind = u8s8,
      const engine& aengine = engine::cpu_engine()) {
    compute_impl</*with_bias=*/true>(
        src,
        weights,
        bias,
        dst,
        src_scales,
        weights_scales,
        dst_scales,
        attr,
        aprop_kind,
        alowp_kind,
        aengine);
  }

  static void compute(
      const tensor& src,
      const tensor& weights,
      tensor& dst,
      const scale_t& src_scales = scale_t(),
      const scale_t& weights_scales = scale_t(),
      const scale_t& dst_scales = scale_t(),
      const attr_t& attr = attr_t(),
      const prop_kind aprop_kind = prop_kind::forward,
      const lowp_kind alowp_kind = u8s8,
      const engine& aengine = engine::cpu_engine()) {
    static tensor dummy_bias;
    compute_impl</*with_bias=*/false>(
        src,
        weights,
        dummy_bias,
        dst,
        src_scales,
        weights_scales,
        dst_scales,
        attr,
        aprop_kind,
        alowp_kind,
        aengine);
  }

  static tensor::desc expected_weights_desc(
      const dims& weights_dims,
      const dims& src_dims = dims(),
      data_type dtype = data_type::f32,
      data_type x_dtype = data_type::f32,
      prop_kind aprop_kind = prop_kind::forward,
      const engine& aengine = engine::cpu_engine()) {
    auto x_dims = weights_dims;
    // 128 is default batch size for inner product
    x_dims[0] = src_dims.empty() ? 128 : src_dims[0];
    auto y_dims = {x_dims[0], weights_dims[0]};
    auto y_dtype = (dtype != data_type::s8) ? dtype : data_type::s32;

    IDEEP_ENFORCE(
        x_dims.size() == weights_dims.size(),
        "Invalid dims for data and weights");
    tensor::desc src_desc(x_dims, x_dtype, tag::any);
    tensor::desc dst_desc(y_dims, y_dtype, tag::any);
    tensor::desc weights_desc(weights_dims, dtype, tag::any);
    auto pd =
        primitive_desc({aprop_kind, src_desc, weights_desc, dst_desc}, aengine);
    return pd.weights_desc();
  }

  static primitive_desc get_primitive_desc(
      const tensor::desc& src_desc,
      const tensor::desc& weights_desc,
      const tensor::desc& dst_desc,
      const tensor::desc& bias_desc = tensor::desc(),
      const bool with_bias = false,
      const attr_t& attr = attr_t(),
      const prop_kind aprop_kind = prop_kind::forward,
      const engine& aengine = engine::cpu_engine()) {
    auto key = utils::create_key(
        aprop_kind,
        src_desc,
        weights_desc,
        bias_desc,
        dst_desc,
        attr,
        with_bias,
        omp_get_max_threads());
    return fetch_or_create(key, [&]() {
      if (with_bias) {
        return primitive_desc(
            {aprop_kind, src_desc, weights_desc, bias_desc, dst_desc},
            attr,
            aengine);
      } else {
        return primitive_desc(
            {aprop_kind, src_desc, weights_desc, dst_desc}, attr, aengine);
      }
    });
  };

 private:
  template <bool with_bias>
  static void compute_impl(
      const tensor& src,
      const tensor& weights,
      const tensor& bias,
      tensor& dst,
      const scale_t& src_scales,
      const scale_t& weights_scales,
      const scale_t& dst_scales,
      const attr_t& attr,
      const prop_kind aprop_kind,
      const lowp_kind alowp_kind,
      const engine& aengine) {
    // workaround: src and weights from caffe2 may have different dims.
    // It would be better for caffe2 to do this reshape anyway.
    auto src_ = src;
    if (src.ndims() != weights.ndims()) {
      auto new_dims = weights.get_dims();
      new_dims[0] = src.get_dim(0);
      src_.reshape(new_dims);
    }
    compute_impl_<with_bias>(
        src_,
        weights,
        bias,
        dst,
        src_scales,
        weights_scales,
        dst_scales,
        attr,
        aprop_kind,
        alowp_kind,
        aengine);
  }

  template <bool with_bias>
  static void compute_impl_(
      const tensor& src,
      const tensor& weights,
      const tensor& bias,
      tensor& dst,
      const scale_t& src_scales,
      const scale_t& weights_scales,
      const scale_t& dst_scales,
      const attr_t& attr,
      const prop_kind aprop_kind,
      const lowp_kind alowp_kind,
      const engine& aengine) {
    tensor::desc src_desc, weights_desc, bias_desc;
    attr_t op_attr, src_attr, weights_attr, bias_attr;
    scale_t dst_scales_in;
    data_type dst_data_type;
    auto dst_dims = {src.get_dim(0), weights.get_dim(0)};

    auto weights_scales_in =
        weights.has_scale() ? weights.get_scale() : weights_scales;

    // TODO(xpz): Remove int8 inner product implementation. We are switching to
    // matmul for quantized *mm ops
    if (!weights_scales_in.empty()) {
      IDEEP_ENFORCE(
          alowp_kind == u8s8 || alowp_kind == s8s8, "Unsupported lowp kind");

      auto src_scales_in = src.has_scale() ? src.get_scale()
          : src_scales.empty()             ? IDEEP_DEF_SCALE
                                           : src_scales;

      src_desc = {
          src.get_dims(),
          alowp_kind == u8s8 ? data_type::u8 : data_type::s8,
          tag::any};
      if (src.get_data_type() == data_type::f32) {
        src_attr = {0, src_scales_in};
      }

      int scale_size = weights_scales_in.size() > 1 ? weights.get_dim(0) : 1;

      weights_desc = {weights.get_dims(), data_type::s8, tag::any};
      if (weights.get_data_type() == data_type::f32) {
        weights_attr = {
            utils::tensor_scale_mask(scale_size, false), weights_scales_in};
      }

      // determine dst data type
      if (dst_scales.empty() || dst_scales == IDEEP_DEF_SCALE) {
        dst_data_type = data_type::f32;
      } else if (attr.non_negitive_output()) {
        dst_data_type = data_type::u8;
      } else {
        dst_data_type = data_type::s8;
      }

      // fill primitive attr
      scale_t op_scales(scale_size), bias_scales(scale_size);
      dst_scales_in = dst_scales.empty() || dst_data_type == data_type::f32
          ? IDEEP_DEF_SCALE
          : dst_scales;
      for (int i = 0; i < scale_size; i++) {
        bias_scales[i] = src_scales_in[0] * weights_scales_in[i];
        op_scales[i] = dst_scales_in[0] / bias_scales[i];
      }
      op_attr.set_output_scales(utils::op_scale_mask(scale_size), op_scales);

      if (with_bias) {
        bias_desc = {bias.get_dims(), data_type::s32, format_tag::any};
        if (bias.get_data_type() == data_type::f32) {
          bias_attr = {
              utils::tensor_scale_mask(scale_size, false), bias_scales};
        }
      }
    } else {
      op_attr = attr;
      if (src.has_scale()) {
        auto src_scale = src.get_scale();
        src_scale[0] = 1.f / src_scale[0];
        src_attr = {0, src_scale};
      }

      IDEEP_ENFORCE(
          utils::one_of(
              weights.get_data_type(), data_type::f32, data_type::bf16),
          "Incorrect data type in weights");
      if (dst.is_empty()) {
        // align weights data type with src
        dst_data_type = src.get_data_type() == data_type::bf16 ? data_type::bf16
                                                               : data_type::f32;
      } else {
        dst_data_type = dst.get_data_type();
      }
      src_desc = src.get_desc().to_type(src.get_data_type());
      weights_desc = weights.get_desc().to_type(src.get_data_type());
      if (with_bias) {
        IDEEP_ENFORCE(
            utils::one_of(
                bias.get_data_type(), data_type::f32, data_type::bf16),
            "Incorrect data type in bias");
        bias_desc = bias.get_desc();
      }
    }

    op_attr.set_fpmath_mode();
    tensor::desc dst_desc = dst.get_desc().to_type(dst_data_type);

    // Use user mode scratchpad
    op_attr.set_scratchpad_mode(dnnl::scratchpad_mode::user);

    auto pd = get_primitive_desc(
        src_desc,
        weights_desc,
        dst_desc,
        bias_desc,
        with_bias,
        op_attr,
        aprop_kind);

    // [ Note output buffer ]
    // In this case, dst is an empty ideep tensor, can be re-init
    // If dst is not empty, ideep must write result to dst's memory and it is
    // caller's duty to make sure dst is big enough to hold the result
    if (dst.is_empty()) {
      dst.init(pd.dst_desc());
    }

    if (!dst_scales.empty() &&
        utils::one_of(dst.get_data_type(), data_type::u8, data_type::s8)) {
      dst.set_scale(dst_scales_in);
    }

    tensor scratchpad(pd.scratchpad_desc());

    if (with_bias) {
      super(pd).execute(
          stream::default_stream(),
          {{DNNL_ARG_SRC, src},
           {DNNL_ARG_WEIGHTS, weights},
           {DNNL_ARG_BIAS, bias},
           {DNNL_ARG_DST, dst},
           {DNNL_ARG_SCRATCHPAD, scratchpad}});
    } else {
      super(pd).execute(
          stream::default_stream(),
          {{DNNL_ARG_SRC, src},
           {DNNL_ARG_WEIGHTS, weights},
           {DNNL_ARG_DST, dst},
           {DNNL_ARG_SCRATCHPAD, scratchpad}});
    }

    if (attr.non_negitive_output() && dst.get_data_type() == data_type::s8) {
      dst.to_type(data_type::u8);
    }
  }
};

struct inner_product_backward_data : public dnnl::inner_product_backward_data {
  using super = dnnl::inner_product_backward_data;

  static void compute(
      const tensor& diff_dst,
      const tensor& weights,
      const dims& diff_src_dims,
      tensor& diff_src,
      const engine& aengine = engine::cpu_engine()) {
    auto weights_ = weights;
    // workaround: diff_src and weights from caffe2 may have different dims.
    // It would be better for caffe2 to do this reshape anyway.
    if (diff_src_dims.size() != weights.ndims()) {
      auto new_dims = diff_src_dims;
      new_dims[0] = weights.get_dim(0);
      weights_.reshape(new_dims);
    }

    auto diff_dst_desc = diff_dst.get_desc();
    auto weights_desc = weights_.get_desc();
    auto diff_src_desc = diff_src.get_desc().to_type(diff_dst.get_data_type());

    auto forward_hints = inner_product_forward::get_primitive_desc(
        diff_src_desc, weights_desc, diff_dst_desc);

    // Use user mode scratchpad
    auto op_attr = dnnl::primitive_attr();
    op_attr.set_scratchpad_mode(dnnl::scratchpad_mode::user);

    auto pd = primitive_desc(
        {diff_src_desc, weights_desc, diff_dst_desc},
        op_attr,
        aengine,
        forward_hints);

    // diff_src's origin content are not used, so it can be re-init directly
    // It's caller's duty to make sure diff_src's buffer size is same with it
    // actually needed Here we dose not support to write to given strided buffer
    // since we know the grad is always contiguous
    if (diff_src.is_empty()) {
      diff_src.init(pd.diff_src_desc());
    } else {
      diff_src.init(pd.diff_src_desc(), diff_src.get_data_handle());
    }

    tensor scratchpad(pd.scratchpad_desc());
    super(pd).execute(
        stream::default_stream(),
        {{DNNL_ARG_DIFF_DST, diff_dst},
         {DNNL_ARG_WEIGHTS, weights_},
         {DNNL_ARG_DIFF_SRC, diff_src},
         {DNNL_ARG_SCRATCHPAD, scratchpad}});
  }
};

struct inner_product_backward_weights
    : public dnnl::inner_product_backward_weights {
  using super = dnnl::inner_product_backward_weights;

  static void compute(
      const tensor& src,
      const tensor& diff_dst,
      tensor& diff_weights,
      tensor& diff_bias,
      const data_type diff_weight_type = data_type::undef,
      const engine& aengine = engine::cpu_engine()) {
    compute_impl</*with_diff_bias=*/true>(
        src, diff_dst, diff_weights, diff_bias, diff_weight_type);
  }

  static void compute(
      const tensor& src,
      const tensor& diff_dst,
      tensor& diff_weights,
      const data_type diff_weight_type = data_type::undef,
      const engine& aengine = engine::cpu_engine()) {
    static tensor dummy_diff_bias;
    compute_impl</*with_diff_bias=*/false>(
        src, diff_dst, diff_weights, dummy_diff_bias, diff_weight_type);
  }

 private:
  template <bool with_diff_bias = true>
  static void compute_impl(
      const tensor& src,
      const tensor& diff_dst,
      tensor& diff_weights,
      tensor& diff_bias,
      const data_type diff_weight_type,
      const engine& aengine = engine::cpu_engine()) {
    auto src_desc = src.get_desc();
    auto diff_dst_desc = diff_dst.get_desc();
    auto diff_weights_dims = src.get_dims();
    diff_weights_dims[0] = diff_dst.get_dim(1);
    data_type diff_dst_type = diff_dst.get_data_type();
    data_type diff_weight_type_in =
        data_type::undef == diff_weight_type ? diff_dst_type : diff_weight_type;

    auto diff_weights_desc =
        diff_weights.get_desc().to_type(diff_weight_type_in);
    auto diff_bias_desc = diff_bias.get_desc().to_type(diff_weight_type_in);

    // for forward hint, weights_desc should have same data_type
    // with other input desc, expect for bias_desc
    auto weights_desc = diff_weights_desc;
    if (diff_weight_type_in != diff_dst_type) {
      weights_desc = weights_desc.to_type(diff_dst_type);
    }
    auto forward_hints = inner_product_forward::get_primitive_desc(
        src_desc, weights_desc, diff_dst_desc, diff_bias_desc, with_diff_bias);

    // Use user mode scratchpad
    auto op_attr = dnnl::primitive_attr();
    op_attr.set_scratchpad_mode(dnnl::scratchpad_mode::user);

    auto pd = with_diff_bias
        ? primitive_desc(
              {src_desc, diff_weights_desc, diff_bias_desc, diff_dst_desc},
              op_attr,
              aengine,
              forward_hints)
        : primitive_desc(
              {src_desc, diff_weights_desc, diff_dst_desc},
              op_attr,
              aengine,
              forward_hints);

    if (diff_weights.is_empty()) {
      diff_weights.init(pd.diff_weights_desc());
    }

    tensor scratchpad(pd.scratchpad_desc());

    exec_args args{
        {DNNL_ARG_DIFF_DST, diff_dst},
        {DNNL_ARG_SRC, src},
        {DNNL_ARG_DIFF_WEIGHTS, diff_weights},
        {DNNL_ARG_SCRATCHPAD, scratchpad}};

    if (with_diff_bias) {
      if (diff_bias.is_empty()) {
        diff_bias.init(pd.diff_bias_desc());
      } else {
        diff_bias.init(pd.diff_bias_desc(), diff_bias.get_data_handle());
      }
      args.insert({DNNL_ARG_DIFF_BIAS, diff_bias});
    }

    super(pd).execute(stream::default_stream(), args);
  }
};

} // namespace ideep

#endif