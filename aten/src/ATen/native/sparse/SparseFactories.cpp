#include <ATen/Dispatch.h>
#include <ATen/SparseTensorImpl.h>
#include <ATen/SparseTensorUtils.h>
#include <ATen/TensorIndexing.h>
#include <ATen/TensorIterator.h>
#include <ATen/core/ATen_fwd.h>
#include <ATen/core/Tensor.h>
#include <ATen/native/sparse/SparseFactories.h>
#include <c10/core/Scalar.h>
#include <c10/util/ArrayRef.h>
#include <c10/util/Exception.h>

#ifndef AT_PER_OPERATOR_HEADERS
#include <ATen/Functions.h>
#include <ATen/NativeFunctions.h>
#else
#include <ATen/ops/sparse_coo_tensor.h>
#endif

namespace at {
namespace native {
using namespace at::sparse;
/******************************************************************************
 * Build sparse from diagonals
 ******************************************************************************/

// --------------------------------------------------------------------
// spdiags(D, O, (N,M)) -> S
//
// Take rows of D and place them on the diagonals specified by offsets O of a
// new (NxM) sparse matrix S If D is (P x Q) then O must be a row vector (P, ).
// It does not matter if Q values fit  on any diagonal of S, or if S has no
// O[i]th diagonal (those values/diagonals are simply skipped)
// --------------------------------------------------------------------

namespace {
void _spdiags_kernel_cpu(TensorIterator& iter, int64_t diag_stride) {
  AT_DISPATCH_ALL_TYPES_AND_COMPLEX_AND4(
      at::ScalarType::BFloat16,
      at::ScalarType::Half,
      at::ScalarType::Bool,
      at::ScalarType::ComplexHalf,
      iter.dtype(),
      "spdiags_cpu",
      [&] {
        auto loop = [&](char** data, const int64_t* strides, int64_t n) {
          // We really only need to use the prefix sum at the start of a chunk
          // if the loop is split every chunk will have the pointers into the
          // NNZ len output buffers at the begining becasue we restrided those
          // to zero We push them over to the corect start position before
          // entering the chunk. Within the subloop for the chunk we update the
          // pointer by nnz per diag, so we don't need to use the prefix sum
          // again. If we could ensure the loop is never split we wouldnt need
          // the prefix sum but that would defeat the whole purpose

          auto* values_bytes = data[0];
          auto* row_indices_bytes = data[1];
          auto* col_indices_bytes = data[2];
          auto* diagonals_bytes = data[3];
          auto* offsets_bytes = data[4];
          int64_t* index_buffer_data = reinterpret_cast<int64_t*>(data[5]);
          auto* nnz_per_diag_bytes = data[6];
          auto* nnz_prefix_bytes = data[7];

          auto diagonals_stride = strides[3];
          auto offsets_stride = strides[4];
          auto nnz_per_diag_stride = strides[6];
          auto nnz_prefix_stride = strides[7];
          for (int64_t offset_index = 0; offset_index < n; ++offset_index) {
            int64_t* row_indices_data =
                reinterpret_cast<int64_t*>(row_indices_bytes);
            int64_t* col_indices_data =
                reinterpret_cast<int64_t*>(col_indices_bytes);
            scalar_t* values_data = reinterpret_cast<scalar_t*>(values_bytes);
            scalar_t* diag_data = reinterpret_cast<scalar_t*>(diagonals_bytes);
            int64_t offset = *reinterpret_cast<int64_t*>(offsets_bytes);
            int64_t nnz_len = *reinterpret_cast<int64_t*>(nnz_per_diag_bytes);
            int64_t nnz_prefix = *reinterpret_cast<int64_t*>(nnz_prefix_bytes);

            int64_t col_offset = std::max<int64_t>(offset, 0);
            int64_t row_offset = col_offset - offset;

            for (int64_t i = 0; i < nnz_len; ++i) {
              values_data[nnz_prefix + i] =
                  diag_data[(col_offset + i) * diag_stride];
            }

            std::copy(
                index_buffer_data + row_offset,
                index_buffer_data + row_offset + nnz_len,
                row_indices_data + nnz_prefix);
            std::copy(
                index_buffer_data + col_offset,
                index_buffer_data + col_offset + nnz_len,
                col_indices_data + nnz_prefix);

            diagonals_bytes += diagonals_stride;
            offsets_bytes += offsets_stride;
            nnz_per_diag_bytes += nnz_per_diag_stride;
            nnz_prefix_bytes += nnz_prefix_stride;
          }
        };
        iter.for_each(loop);
      });
}

// Check offsets for duplicates, and out-of-bounds diagonals
// While checking offsets, compute nnz per diagonal array, prefix sum array, and
// nnz total for later use
void _spdiags_setup_cpu(
    TensorIterator& iter,
    int64_t n_row_out,
    int64_t n_col_in,
    int64_t n_col_out,
    int64_t& nnz_out) {
  const int64_t min_col_in_out = std::min(n_col_in, n_col_out);
  auto loop = [&](char** data, const int64_t* strides, int64_t n) {
    auto* nnz_per_diag_bytes = data[0];
    auto* offsets_bytes = data[1];
    std::set<int64_t> seen_offsets;
    for (int64_t d = 0; d < n; ++d) {
      auto* nnz_per_diag_data = reinterpret_cast<int64_t*>(nnz_per_diag_bytes);
      auto offset = *reinterpret_cast<int64_t*>(offsets_bytes);
      TORCH_CHECK(
          seen_offsets.insert(offset).second,
          "spidags(): Offset array contains duplicate values");
      TORCH_CHECK(
          ((-1 * n_row_out) < offset) && (offset < n_col_out),
          "spdiags(): Diagonal ",
          offset,
          " does not exist in output shape (",
          n_row_out,
          ",",
          n_col_out,
          "). Valid offsets for this shape: [",
          (-n_row_out) + 1,
          ",",
          n_col_out - 1,
          "]");
      if (offset >= 0) {
        *nnz_per_diag_data =
            std::max<int64_t>(std::min(min_col_in_out - offset, n_row_out), 0);
      } else {
        *nnz_per_diag_data =
            std::max<int64_t>(std::min(min_col_in_out, n_row_out + offset), 0);
      }
      nnz_out += *nnz_per_diag_data;
      nnz_per_diag_bytes += strides[0];
      offsets_bytes += strides[1];
    }
  };
  iter.for_each(loop);
}

} // namespace

Tensor spdiags_cpu(
    const Tensor& diagonals,
    const Tensor& offsets,
    IntArrayRef shape,
    c10::optional<Layout> layout) {
  return impl::spdiags_impl(
      diagonals,
      offsets,
      shape,
      layout,
      _spdiags_setup_cpu,
      _spdiags_kernel_cpu);
}
} // namespace native
} // namespace at