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

#include "sparse_error_feedback.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <iterator>

#include "../compressor_registry.h"

namespace byteps {
namespace common {
namespace compressor {
namespace {
CompressorRegistry::Register reg(
    "sparse_ef",
    [](const kwargs_t& kwargs, size_t size, DataType dtype,
       std::unique_ptr<Compressor> cptr) -> std::unique_ptr<Compressor> {
      // register cptr
      BPS_CHECK_NE(cptr, nullptr);
      auto factor = HyperParamFinder<float>(kwargs, "compressor_k", false,
                                            [](float x) { return x > 0; });
      unsigned k;
      if (factor < 1) {
        k = static_cast<unsigned>(factor * size / getDataTypeLength(dtype));
        if (k == 0) k = 1;
      } else {
        k = static_cast<unsigned>(factor);
      }

      auto seed = HyperParamFinder<unsigned>(kwargs, "seed", true,
                                             [](unsigned x) { return x != 0; });

      BPS_LOG(INFO) << "sparse error feedback is registered. "
                    << "\tsize=" << size << "\tk=" << k << "\tseed=" << seed;

      return std::unique_ptr<SparseErrorFeedbackCompressor>(
          new SparseErrorFeedbackCompressor(size, dtype, std::move(cptr), k,
                                            seed));
    });
}

SparseErrorFeedbackCompressor::SparseErrorFeedbackCompressor(
    size_t size, DataType dtype, std::unique_ptr<Compressor> cptr, size_t k,
    unsigned int seed)
    : ErrorFeedback(size, dtype, std::move(cptr)), _k(k) {
  if (seed) {
    BPS_LOG(INFO) << "SET SEED = " << seed + k;
    _rng.set_seed(seed + k);
  }
  _fd = open("lr.s", O_RDONLY);
  BPS_CHECK(_fd > 0) << "open lr.s failed, errno=" << strerror(errno);
  void* ptr = mmap(nullptr, 8, PROT_READ, MAP_SHARED, _fd, 0);
  BPS_CHECK_NE(ptr, MAP_FAILED) << "mmap failed, errno=" << strerror(errno);
  _mm = ptr;
  _pre_lr = _cur_lr = *reinterpret_cast<double*>(_mm);
}

SparseErrorFeedbackCompressor::~SparseErrorFeedbackCompressor() {
  munmap(_mm, 8);
  close(_fd);
}

void SparseErrorFeedbackCompressor::UpdateGradient(tensor_t grad) {
  _cur_lr = *reinterpret_cast<double*>(_mm);

#ifndef BYTEPS_BUILDING_SERVER
  sum(grad.data, _buf.get(), grad.size, static_cast<DataType>(grad.dtype),
      (_pre_lr / _cur_lr));
#else
  size_t len = grad.size / getDataTypeLength(grad.dtype);

  for (size_t i = 0; i < this->_k; ++i) {
    _selected_idx.push_back(_rng.Randint(0, len));
  }

  sparse_sum(grad.data, _buf.get(), grad.size,
             static_cast<DataType>(grad.dtype), (_pre_lr / _cur_lr),
             _selected_idx);

  _selected_idx.clear();
#endif
  _pre_lr = _cur_lr;
}

}  // namespace compressor
}  // namespace common
}  // namespace byteps
