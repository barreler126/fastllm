//
// Created by huangyuyang on 6/17/25.
//

#ifndef FASTLLM_MINIMAX_H
#define FASTLLM_MINIMAX_H

#include "basellm.h"
#include "llama.h"

#include "cmath"

#include <iostream>

namespace fastllm {
    class MinimaxModel : public basellm {
    public:
        MinimaxModel (); // 构造函数

        virtual void InitParams(); // 初始化参数信息

        // 推理
        virtual int Forward(
                const Data &inputIds,
                const Data &attentionMask,
                const Data &positionIds,
                std::vector <std::pair <Data, Data> > &pastKeyValues,
                const GenerationConfig &generationConfig = GenerationConfig(),
                const LastTokensManager &lastTokens = LastTokensManager(),
                std::vector <float> *logits = nullptr);

        std::vector <int> ForwardBatch(
                int batch,
                const Data &inputIds,
                const Data &attentionMask,
                const Data &positionIds,
                std::vector <std::pair <Data, Data> > &pastKeyValues,
                const GenerationConfig &generationConfig = GenerationConfig(),
                const LastTokensManager &lastTokens = LastTokensManager(),
                std::vector <std::vector <float>*> *logits = nullptr);

        std::vector <int> ForwardBatch(
                int batch,
                const Data &inputIds,
                const std::vector <Data*> &attentionMask,
                const std::vector <Data*> &positionIds,
                const std::vector <int> &seqLens,
                std::vector <std::pair <Data*, Data*> > &pastKeyValues,
                const std::vector <GenerationConfig> &generationConfigs,
                const LastTokensManager &lastTokens = LastTokensManager(),
                std::vector <std::vector <float>*> *logits = nullptr);
                
        // 根据输入的tokens生成LLM推理的输入
        virtual void FillLLMInputsBatch(std::vector <std::vector <float> > &inputTokens,
                                        const std::vector <std::map <std::string, int> > &params,
                                        Data &inputIds, Data &attentionMask, Data &positionIds);

        // 是否需要生成AttentionMask
        virtual bool NeedAttentionMask(int qlen, int klen);
        
        virtual void WarmUp(); // 预热

        virtual std::string MakeInput(const std::string &history, int round, const std::string &input); // 根据历史信息和当前输入生成prompt

        virtual std::string MakeHistory(const std::string &history, int round, const std::string &input, const std::string &output); // 根据当前回复更新history

        std::pair<std::vector<float>, std::vector<float>> UpdateRotaryPosEmb(float base, float factor, int seqLen = 0); // 更新位置编码

    protected:
        RoPEType rope_type = RoPEType::BASE;

        float rope_base = 10000.f;

        float rope_factor = 1.f;

        int num_key_value_heads = num_attention_heads;

        float rms_norm_eps = 1e-6;

        bool mergeQKV = false;
        bool mergeSwiglu = false;

        std::vector <std::vector <Data*> > weights;
        std::vector <std::vector <Data*> > biass;

        float routed_scaling_factor = 1.0f;

        std::vector <int> attn_type_list; // 每一层是什么attention类型

        float layernorm_full_attention_alpha, layernorm_full_attention_beta, layernorm_linear_attention_alpha, layernorm_linear_attention_beta;
        float layernorm_mlp_alpha, layernorm_mlp_beta;

        std::map <int, Data*> ratioDatas, qDecays, kDecays, diagDecays, blockDecays;
    };
}

#endif //FASTLLM_MINIMAX_H
