/*
 * Copyright (c) 2022-2025, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "tensorrt_llm/common/cudaDriverWrapper.h"
#include "trtllmGenSrc/DevKernel.h"
#include "trtllmGenSrc/Dtype.h"
#include "trtllmGenSrc/RoutingKernel.h"
#include "trtllmGenSrc/SfLayoutDecl.h"
#include <string>

namespace tensorrt_llm
{
namespace kernels
{
namespace trtllmGenFp8BlockScaleMoe
{

namespace Routing
{
inline int32_t getMaxPermutedPaddedCount(
    const int32_t numTokens, const int32_t expertsPerToken, const int32_t numExperts, const int32_t padding)
{

    const int32_t expandedRowCount = numTokens * expertsPerToken;
    const int32_t maxPaddingRequired = (padding - 1) * numExperts;
    return expandedRowCount + maxPaddingRequired;
}

class Runner
{
public:
    explicit Runner();

    void run(void* routingLogits, void* routingBias, int32_t num_tokens, int32_t num_experts, int32_t top_k,
        int32_t n_groups, int32_t topk_groups, int32_t local_expert_offset, int32_t local_num_experts,
        float routed_scaling_factor, int32_t* routingExpertIndexes, int32_t* expertCountHistogram,
        int32_t* permuted_idx_size, int32_t* expanded_idx_to_permuted_idx, int32_t* permuted_idx_to_expanded_idx,
        int32_t* permuted_idx_to_token_idx, void* expert_weights, int32_t* num_tokens_per_expert,
        int32_t* cta_idx_xy_to_batch_idx, int32_t* cta_idx_xy_to_mn_limit, int32_t* num_non_exiting_ctas,
        trtllm::gen::Dtype dtypeElt, bool use_routing_scales_on_input, bool use_deep_seek_fp8, cudaStream_t stream);
};
} // namespace Routing

namespace PermuteGemm1
{
class Runner
{
public:
    explicit Runner(trtllm::gen::Dtype dtypeElt);

    void run(void* hidden_state, void* hidden_state_scale, void* weight, void* weight_scale, void* expert_weights,
        float* output_scales_scalar, float* output_scales_gate_scalar, void* output, void* output_scale, int32_t top_k,
        int32_t hidden_size, int32_t intermediate_size, int32_t num_experts, int32_t num_tokens,
        int32_t* permuted_idx_to_token_idx, int32_t* ptr_num_non_exiting_ctas, int32_t* ptr_total_num_padded_tokens,
        int32_t* ptr_cta_idx_xy_to_batch_idx, int32_t* ptr_cta_idx_xy_to_mn_limit, bool use_routing_scales_on_input,
        bool use_deep_seek_fp8, cudaStream_t stream);

private:
    trtllm::gen::Dtype mDtypeElt;
};
} // namespace PermuteGemm1

namespace Gemm2
{
class Runner
{
public:
    explicit Runner(trtllm::gen::Dtype dtypeElt, trtllm::gen::Dtype outputDtype = trtllm::gen::Dtype::E4m3);

    void run(void* permuted_hidden_state, void* permuted_hidden_state_scale, void* weight, void* weight_scale,
        float* output_scales_scalar, void* output, void* output_scale, int32_t top_k, int32_t hidden_size,
        int32_t intermediate_size, int32_t num_experts, int32_t num_tokens, int32_t* ptr_num_non_exiting_ctas,
        int32_t* ptr_total_num_padded_tokens, int32_t* ptr_cta_idx_xy_to_batch_idx, int32_t* ptr_cta_idx_xy_to_mn_limit,
        bool use_deep_seek_fp8, cudaStream_t stream);

private:
    trtllm::gen::Dtype mDtypeElt;
    trtllm::gen::Dtype mOutputDtype;
};
} // namespace Gemm2

namespace MoE
{
namespace tg = trtllm::gen;

struct MoERunnerArgs
{
    void* routing_logits
        = nullptr; // [num_tokens, num_experts] in float, generated after gemm(hidden_state, routing_weights)
    void* routing_bias = nullptr;  // [num_experts] in bfloat16 for now = mDtypeExpW
    void* hidden_states = nullptr; // [num_tokens, hidden_size] in fp8 = mDtypeElt
    // [hidden_size/128, num_tokens] in float for e4m3 DS recipe
    // and [num_tokens, hidden_size/16] in float for e2m1
    void* hidden_states_scale = nullptr;

    // Gemm input:
    void* gemm1_weights = nullptr;
    void* gemm1_weights_scale = nullptr;
    void* gemm2_weights = nullptr;
    void* gemm2_weights_scale = nullptr;

    int32_t num_tokens{0};
    int32_t num_experts{0};
    int32_t hidden_size{0};
    // TODO: only compiled routing kernel supports top_k = 8
    int32_t top_k{0};
    int32_t n_group{0};
    // TODO: only compiled routing kernel supports topk_group = 4
    int32_t topk_group{0};
    float routed_scaling_factor{0.0f};
    int32_t intermediate_size{0};
    int32_t local_expert_offset{0};
    int32_t local_num_experts{0};
    // TODO: support other types
    tg::Dtype mDtypeElt{tg::Dtype::Void};
    tg::Dtype mDtypeExpW{tg::Dtype::Bfloat16};
    tg::Dtype mDtypeOut{tg::Dtype::Bfloat16};

    // Apply routing scale factors to input activations
    bool mUseRoutingScalesOnInput{false};
    bool mUseDeepSeekFp8{false};

    float* output1_scales_scalar = nullptr;
    float* output1_scales_gate_scalar = nullptr;
    float* output2_scales_scalar = nullptr;

    // Output:
    void* output = nullptr;
    float* output_scale = nullptr;
};

struct MoEWorkspace
{
    // Routing intermediate outputs:
    int32_t* routing_expert_indexes = nullptr;
    int32_t* permuted_idx_size = nullptr;
    int32_t* total_num_padded_tokens = nullptr; // TODO: duplicate of permuted_idx_size
    int32_t total_max_padded_tokens{0};

    int32_t* expanded_idx_to_permuted_idx = nullptr;
    int32_t* permuted_idx_to_expanded_idx = nullptr;
    int32_t* permuted_idx_to_token_idx = nullptr;
    void* expert_weights = nullptr; // [num_tokens, top_k] in bfloat16 = mDtypeExpW

    int32_t* cta_idx_xy_to_batch_idx = nullptr;
    int32_t* cta_idx_xy_to_mn_limit = nullptr;
    int32_t* num_non_exiting_ctas = nullptr;

    void* hidden_states_scale_linear = nullptr;

    // Permute intermediate outputs:
    void* permuted_hidden_states = nullptr;
    float* permuted_hidden_states_scale = nullptr;

    // Gemm1 intermediate outputs:
    int32_t ProjUpTileN{0};
    void* gemm1_output = nullptr;
    float* gemm1_output_scale = nullptr;

    // Activation intermediate outputs:
    void* activation_output = nullptr;
    float* activation_output_scale = nullptr;

    // Gemm2 intermediate outputs:
    void* gemm2_output = nullptr;
    float* gemm2_output_scale = nullptr;

    // Finalize intermediate outputs (placeholder not used)
    void* finalize_output = nullptr;
    float* finalize_output_scale = nullptr;
};

class Runner
{
public:
    explicit Runner();

    void run(MoERunnerArgs const& args, MoEWorkspace const& workspace, cudaStream_t stream);

private:
    void setOpsData(MoERunnerArgs const& args, MoEWorkspace const& workspace, moe::dev::convertsf::Data& convertSfData,
        moe::dev::activation::Data& activationData, moe::dev::finalize::Data& finalizeData);
};
} // namespace MoE

} // namespace trtllmGenFp8BlockScaleMoe
} // namespace kernels
} // namespace tensorrt_llm
