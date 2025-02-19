#include <csrc/aten/cpu/AddSoftmax.h>

#if defined(CPU_CAPABILITY_AVX512)
#include "csrc/cpu/vec512/add_softmax.h"
#endif

namespace torch_ipex {
namespace cpu {

namespace {

at::Tensor div_add_softmax_kernel_impl(
    at::Tensor& a,
    const at::Tensor& b,
    const float& dim_per_head) {
#if defined(CPU_CAPABILITY_AVX512)
  if (a.scalar_type() == at::kFloat && b.scalar_type() == at::kFloat) {
    return torch_ipex::cpu::kernel::vec::vec512::dil_div_add_softmax<float>(
        a, b, dim_per_head);
  } else if (
      a.scalar_type() == at::kBFloat16 && b.scalar_type() == at::kBFloat16) {
    return torch_ipex::cpu::kernel::vec::vec512::dil_div_add_softmax<
        at::BFloat16>(a, b, dim_per_head);
  }
#endif
  a = at::div(a, dim_per_head);
  return at::softmax(at::add(a, b, 1.0f), -1);
}

} // anonymous namespace

REGISTER_DISPATCH(div_add_softmax_kernel_stub, &div_add_softmax_kernel_impl);

} // namespace cpu
} // namespace torch_ipex
