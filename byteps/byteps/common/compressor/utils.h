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

#ifndef BYTEPS_COMPRESSOR_UTILS_H
#define BYTEPS_COMPRESSOR_UTILS_H

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>

#include "../half.h"
#include "common.h"

namespace byteps {
namespace common {
namespace compressor {

/*!
 * \brief serialize key-vals hyper-params for network transmission
 *
 * \param kwargs hyper-params
 * \return std::string serialized data
 */
inline std::string Serialize(const kwargs_t& kwargs) {
  std::ostringstream os;
  os << kwargs.size();
  for (auto const& kwarg : kwargs) {
    os << " " << kwarg.first << " " << kwarg.second;
  }
  return os.str();
}

/*!
 * \brief deserialize serialized data into key-vals hyper-params
 *
 * \param content serialized data
 * \return kwargs_t hyper-params
 */
inline kwargs_t Deserialize(const std::string& content) {
  kwargs_t kwargs;
  std::istringstream is(content);
  size_t size = 0;
  is >> size;
  for (size_t i = 0; i < size; ++i) {
    kwargs_t::key_type key;
    kwargs_t::mapped_type val;
    is >> key >> val;
    kwargs[key] = val;
  }

  return kwargs;
}

/*!
 * \brief random number generator based on xorshift128plus
 *
 * refer to https://en.wikipedia.org/wiki/Xorshift#xorshift+
 */
class XorShift128PlusBitShifterRNG {
 public:
  XorShift128PlusBitShifterRNG() {
    std::random_device rd;
    _state = {rd(), rd()};
  }

  // uniform int among [low, high)
  uint64_t Randint(uint64_t low, uint64_t high) {
    return xorshift128p() % (high - low) + low;
  };

  // uniform [0, 1]
  double Rand() { return double(xorshift128p()) / MAX; }

  // Bernoulli Distributation
  bool Bernoulli(double p) { return xorshift128p() < p * MAX; }

  void set_seed(uint64_t seed) { _state = {seed, seed}; }

  uint64_t xorshift128p() {
    uint64_t t = _state.a;
    uint64_t const s = _state.b;
    _state.a = s;
    t ^= t << 23;        // a
    t ^= t >> 17;        // b
    t ^= s ^ (s >> 26);  // c
    _state.b = t;
    return t + s;
  };

 private:
  struct xorshift128p_state {
    uint64_t a, b;
  };

  xorshift128p_state _state;

  static constexpr uint64_t MAX = std::numeric_limits<uint64_t>::max();
};

/*!
 * \brief Bit Writer
 *
 */
template <typename T>
class BitWriter {
 public:
  explicit BitWriter(T* data)
      : _dptr(data), _accum(0), _used_bits(0), _blocks(0) {}
  void Put(bool x) {
    _accum <<= 1;
    _accum |= x;

    if (++_used_bits == PACKING_SIZE) {
      _dptr[_blocks++] = _accum;
      _used_bits = 0;
    }
  }

  void Flush() {
    if (_used_bits > 0) {
      size_t padding_size = PACKING_SIZE - _used_bits;
      _accum <<= padding_size;
      _dptr[_blocks] = _accum;
    }
  }

  size_t bits() const { return _blocks * PACKING_SIZE + _used_bits; }
  size_t blocks() const { return std::ceil((float)bits() / PACKING_SIZE); }

 private:
  static constexpr size_t PACKING_SIZE = sizeof(T) * 8;
  T* _dptr;  // allocated
  T _accum;
  size_t _used_bits;
  size_t _blocks;
};

/*!
 * \brief Bit Reader
 *
 */
template <typename T>
class BitReader {
 public:
  explicit BitReader(const T* data) : _dptr(data), _used_bits(0), _blocks(0) {}
  bool Get() {
    if (_used_bits == 0) {
      _accum = _dptr[_blocks++];
      _used_bits = PACKING_SIZE;
    }
    return _accum & (1 << --_used_bits);
  }

  size_t bits() const { return _blocks * PACKING_SIZE - _used_bits; }

 private:
  static constexpr size_t PACKING_SIZE = sizeof(T) * 8;
  const T* _dptr;  // allocated
  size_t _used_bits;
  size_t _blocks;
  T _accum;
};

inline auto RoundNextPow2(uint32_t v) -> uint32_t {
  v -= 1;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v += 1;
  return v;
}

template <typename T>
void EliasDeltaEncode(BitWriter<T>& bit_writer, unsigned long x) {
  int len = 1 + std::floor(std::log2(x));
  int lenth_of_len = std::floor(std::log2(len));

  for (int i = lenth_of_len; i > 0; --i) bit_writer.Put(0);
  for (int i = lenth_of_len; i >= 0; --i) bit_writer.Put((len >> i) & 1);
  for (int i = len - 2; i >= 0; i--) bit_writer.Put((x >> i) & 1);
}

template <typename T>
auto EliasDeltaDecode(BitReader<T>& bit_reader) -> unsigned long {
  unsigned long num = 1;
  int len = 1;
  int lenth_of_len = 0;
  while (!bit_reader.Get()) lenth_of_len++;
  for (int i = 0; i < lenth_of_len; i++) {
    len <<= 1;
    if (bit_reader.Get()) len |= 1;
  }
  for (int i = 0; i < len - 1; i++) {
    num <<= 1;
    if (bit_reader.Get()) num |= 1;
  }
  return num;
}

template <typename T, class F = std::function<bool(T)>>
auto HyperParamFinder(
    const kwargs_t& kwargs, std::string name, bool optional = false,
    F&& check = [](T) -> bool { return true; }) -> T {
  static_assert(std::is_fundamental<T>::value,
                "custom type is not allow for HyperParamFinder");
  T value{T()};
  auto iter = kwargs.find(name);
  if (iter == kwargs.end()) {
    // necessary hp
    if (optional == false) {
      BPS_LOG(FATAL) << "Hyper-parameter '" << name
                     << "' is not found! Aborted.";
    }
    return value;
  } else {
    std::istringstream ss(iter->second);
    if (std::is_same<T, bool>::value) {
      ss >> std::boolalpha >> value;
    } else {
      ss >> value;
    }
    if (!check(value)) {
      BPS_LOG(FATAL) << "Hyper-parameter '" << name << "' should not be "
                     << value << "! Aborted.";
    }
  }

  BPS_LOG(INFO) << "Register hyper-parameter '" << name << "'=" << value;
  return value;
}

inline void memcpy_multithread(void* __restrict__ dst,
                               const void* __restrict__ src, size_t len) {
  auto in = (float*)src;
  auto out = (float*)dst;
#pragma omp parallel for simd
  for (size_t i = 0; i < len / 4; ++i) {
    out[i] = in[i];
  }
  if (len % 4) {
    std::memcpy(out + len / 4, in + len / 4, len % 4);
  }
}

template <typename T>
auto sgn(T val) -> int {
  return (T(0) < val) - (val < T(0));
}

template <typename T>
int _sum(T* __restrict__ dst, const T* __restrict__ src, size_t len,
         float alpha) {
#pragma omp parallel for simd
  for (size_t i = 0; i < len / (size_t)sizeof(T); ++i) {
    dst[i] = dst[i] + alpha * src[i];
  }
  return 0;
}

inline int sum(void* dst, const void* src, size_t len, DataType dtype,
               float alpha) {
  switch (dtype) {
    case BYTEPS_FLOAT32:
      return _sum(reinterpret_cast<float*>(dst),
                  reinterpret_cast<const float*>(src), len, alpha);
    case BYTEPS_FLOAT64:
      return _sum(reinterpret_cast<double*>(dst),
                  reinterpret_cast<const double*>(src), len, alpha);
    case BYTEPS_FLOAT16:
      return _sum(reinterpret_cast<half_t*>(dst),
                  reinterpret_cast<const half_t*>(src), len, alpha);
    case BYTEPS_UINT8:
      return _sum(reinterpret_cast<uint8_t*>(dst),
                  reinterpret_cast<const uint8_t*>(src), len, alpha);
    case BYTEPS_INT32:
      return _sum(reinterpret_cast<int32_t*>(dst),
                  reinterpret_cast<const int32_t*>(src), len, alpha);
    case BYTEPS_INT8:
      return _sum(reinterpret_cast<int8_t*>(dst),
                  reinterpret_cast<const int8_t*>(src), len, alpha);
    case BYTEPS_INT64:
      return _sum(reinterpret_cast<int64_t*>(dst),
                  reinterpret_cast<const int64_t*>(src), len, alpha);
    default:
      BPS_CHECK(0) << "Unsupported data type: " << dtype;
  }
  return 0;
}

template <typename T>
int _sum(T* __restrict__ dst, const T* __restrict__ src1,
         const T* __restrict__ src2, size_t len, float alpha) {
#pragma omp parallel for simd
  for (size_t i = 0; i < len / (size_t)sizeof(T); ++i) {
    dst[i] = src1[i] + alpha * src2[i];
  }
  return 0;
}

inline int sum(void* dst, const void* src1, const void* src2, size_t len,
               DataType dtype, float alpha) {
  switch (dtype) {
    case BYTEPS_FLOAT32:
      return _sum(reinterpret_cast<float*>(dst),
                  reinterpret_cast<const float*>(src1),
                  reinterpret_cast<const float*>(src2), len, alpha);
    case BYTEPS_FLOAT64:
      return _sum(reinterpret_cast<double*>(dst),
                  reinterpret_cast<const double*>(src1),
                  reinterpret_cast<const double*>(src2), len, alpha);
    case BYTEPS_FLOAT16:
      return _sum(reinterpret_cast<half_t*>(dst),
                  reinterpret_cast<const half_t*>(src1),
                  reinterpret_cast<const half_t*>(src2), len, alpha);
    case BYTEPS_UINT8:
      return _sum(reinterpret_cast<uint8_t*>(dst),
                  reinterpret_cast<const uint8_t*>(src1),
                  reinterpret_cast<const uint8_t*>(src2), len, alpha);
    case BYTEPS_INT32:
      return _sum(reinterpret_cast<int32_t*>(dst),
                  reinterpret_cast<const int32_t*>(src1),
                  reinterpret_cast<const int32_t*>(src2), len, alpha);
    case BYTEPS_INT8:
      return _sum(reinterpret_cast<int8_t*>(dst),
                  reinterpret_cast<const int8_t*>(src1),
                  reinterpret_cast<const int8_t*>(src2), len, alpha);
    case BYTEPS_INT64:
      return _sum(reinterpret_cast<int64_t*>(dst),
                  reinterpret_cast<const int64_t*>(src1),
                  reinterpret_cast<const int64_t*>(src2), len, alpha);
    default:
      BPS_CHECK(0) << "Unsupported data type: " << dtype;
  }
  return 0;
}

template <typename T>
int _sparse_sum(T* __restrict__ dst, T* __restrict__ src, size_t len,
                float alpha, const std::vector<uint32_t>& idx_list) {
  size_t size = idx_list.size();

#pragma omp parallel for simd
  for (size_t i = 0; i < size; ++i) {
    dst[i] += src[idx_list[i]] * alpha;
    src[idx_list[i]] = 0;
  }

  return 0;
}

inline int sparse_sum(void* dst, void* src, size_t size, DataType dtype,
                      float alpha, const std::vector<uint32_t>& idx_list) {
  switch (dtype) {
    case BYTEPS_FLOAT32:
      return _sparse_sum(reinterpret_cast<float*>(dst),
                         reinterpret_cast<float*>(src), size / sizeof(float),
                         alpha, idx_list);
    case BYTEPS_FLOAT64:
      return _sparse_sum(reinterpret_cast<double*>(dst),
                         reinterpret_cast<double*>(src),

                         size / sizeof(double), alpha, idx_list);
    case BYTEPS_FLOAT16:
      return _sparse_sum(reinterpret_cast<half_t*>(dst),
                         reinterpret_cast<half_t*>(src),

                         size / sizeof(half_t), alpha, idx_list);
    default:
      BPS_CHECK(0) << "Unsupported data type: " << dtype;
  }
  return 0;
}

}  // namespace compressor
}  // namespace common
}  // namespace byteps

#endif  // BYTEPS_COMPRESSOR_UTILS_H