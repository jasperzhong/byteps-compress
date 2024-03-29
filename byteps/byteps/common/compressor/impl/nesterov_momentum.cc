// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include "nesterov_momentum.h"

#include "../compressor_registry.h"

namespace byteps {
namespace common {
namespace compressor {
namespace {
CompressorRegistry::Register reg(
    "nesterov_momentum",
    [](const kwargs_t& kwargs, size_t size, DataType dtype,
       std::unique_ptr<Compressor> cptr) -> std::unique_ptr<Compressor> {
      // register cptr
      BPS_CHECK_NE(cptr, nullptr);
      // find \mu
      auto mu = HyperParamFinder<float>(kwargs, "momentum_mu");

      BPS_LOG(INFO) << "nesterov momentum is registered.";
      return std::unique_ptr<NesterovMomentumCompressor>(
          new NesterovMomentumCompressor(size, dtype, std::move(cptr), mu));
    });
}

void NesterovMomentumCompressor::UpdateMom(tensor_t grad) {
  // m_t = \mu * m_{t-1} + g_t
  sum(_buf.get(), grad.data, _buf.get(), grad.size,
      static_cast<DataType>(grad.dtype), _mu);
}

void NesterovMomentumCompressor::UpdateGradient(tensor_t grad) {
  // p_t = \mu m_t + g_t
  sum(grad.data, _buf.get(), grad.size, static_cast<DataType>(grad.dtype), _mu);
}

}  // namespace compressor
}  // namespace common
}  // namespace byteps