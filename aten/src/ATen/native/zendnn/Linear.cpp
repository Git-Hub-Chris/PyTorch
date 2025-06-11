#include <ATen/native/zendnn/Linear_utils.hpp>
#if !AT_ZENDNN_ENABLED()
namespace zendnn {
at::Tensor zendnn_linear(const at::Tensor &input, const at::Tensor &weight,
                           const std::optional<at::Tensor> &bias,
                           std::string zendnn_op_name) {
  TORCH_CHECK(false, "zendnn_linear: ATen not compiled with ZenDNN support");
}
} // namespace zendnn

#else // !AT_ZENDNN_ENABLED()

namespace zendnnl {
using namespace zendnnl::interface;

at::Tensor zendnn_linear(const at::Tensor &input, const at::Tensor &weight,
                           const std::optional<at::Tensor> &bias,
                           std::string zendnn_op_name) {
  at::Tensor result;
  return result;
}
} // namespace zendnnl

#endif // !AT_ZENDNN_ENABLED()
