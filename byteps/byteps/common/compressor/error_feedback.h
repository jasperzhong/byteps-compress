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

#ifndef BYTEPS_COMPRESSOR_ERROR_FEEDBACK_H
#define BYTEPS_COMPRESSOR_ERROR_FEEDBACK_H

#include "../cpu_reducer.h"
#include "compressor.h"

namespace byteps {
namespace common {
namespace compressor {

/*!
 *  \brief Error feedback Decorator
 *
 * paper: 1-bit stochastic gradient descent and its application to data-parallel
 * distributed training of speech dnns
 * https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/IS140694.pdf
 *
 * 1. UpdateGradient: g <- g + e
 * 2. UpdateError: e <- g - c
 *
 * These two functions should be implemented in children classes.
 *
 * \par
 * The caller do not need allocate an additional buffer to store error. There is
 * a buffer already inside the class.
 *
 * \par
 * Add error feedback behavior to any compressor at run-time via decorator
 * pattern. It keeps the same interface as Compressor. Compress and Decompress
 * have been implemented and can not be changed in children classes.
 *
 * \sa Compressor, VanillaErrorFeedbackCompressor
 */
class ErrorFeedback : public Compressor {
 public:
  // error buffer should be cleared to zeros at the beginning.
  ErrorFeedback(size_t size, DataType dtype, std::unique_ptr<Compressor> cptr)
      : Compressor(size, dtype), _cptr(std::move(cptr)) {}
  ~ErrorFeedback() override = default;

  void Compress(tensor_t grad, tensor_t& output) final;

  void Decompress(tensor_t compressed, tensor_t& output) final;

 protected:
  /*!
   * \brief Correct gradient with error
   *
   * grad += error
   *
   * \note it is an inplace operation.
   *
   * \param grad input gradient to be updated inplace
   * \param dtype type
   */
  virtual void UpdateGradient(tensor_t grad) = 0;

 private:
  /*! \brief compressor pointer */
  std::unique_ptr<Compressor> _cptr;
};
}  // namespace compressor
}  // namespace common
}  // namespace byteps

#endif  // BYTEPS_COMPRESSOR_ERROR_FEEDBACK_H