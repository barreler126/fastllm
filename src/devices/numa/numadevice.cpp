//
// Created by huangyuyang on 11/4/24.
//

#include <sys/mman.h>
#include <fcntl.h>

#include "devices/numa/numadevice.h"
#include "devices/cpu/cpudevice.h"
#include "devices/numa/fastllm-numa.h"
#include "devices/cpu/alivethreadpool.h"

#include <cstring>
#include <thread>

#include <cfloat>
#include <cmath>

#ifdef __aarch64__
#include <arm_neon.h>
#include "armMath.h"
#endif

#include "utils.h"

namespace fastllm {
    extern FP16ToFP32Manager fp16tofp32;
    extern void Float16ToFloat32(uint16_t *float16, float *float32, int len);
    extern void OnlineQuantization(float *inputData, std::vector<uint8_t> &uinput, std::vector<LowBitConfig> &inputConfigs, 
        int n, int m, int group, int groupCnt,
        std::vector <float> &inputSums, std::vector <float> &iscales, std::vector <float> &izeros, int permuteType);

    NumaClient *numaClient = new NumaClient();

    NumaClient *GetNumaClient() {
        if (numaClient == nullptr) {
            numaClient = new NumaClient();
        }
        return numaClient;
    }

    NumaDevice::NumaDevice() {
        this->deviceType = "numa";
        this->ops["Linear"] = (BaseOperator *) (new NumaLinearOp());
        this->ops["MergeMOE"] = (BaseOperator*)(new NumaMergeMOE());

        /*this->ops["CatDirect"] = (BaseOperator *) (new NumaCatDirectOp());
        this->ops["Attention"] = (BaseOperator *) (new NumaAttention());

        this->ops["AttentionBatch"] = (BaseOperator *) (new NumaAttentionBatchOp());
        this->ops["CatDirectBatch"] = (BaseOperator *) (new NumaCatDirectBatchOp());*/
    }

    bool NumaDevice::Malloc(void **ret, size_t size) {
        *ret = (void*)new uint8_t [size];
        return true;
    }

    bool NumaDevice::Free(void *ret) {
        delete[] (uint8_t *)ret;
        return true;
    }

    bool NumaDevice::CopyDataToCPU(void *dst, void *src, size_t size) {
        return true;
    }
    
    bool NumaDevice::CopyDataFromCPU(void *dst, void *src, size_t size) {
        return true;
    }

    bool NumaLinearOp::CanRun(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        if (datas.find("weight") == datas.end()) {
            return true;
        }
        Data *input = (datas.find("input")->second);
        Data *weight = (datas.find("weight")->second);
        return weight == nullptr ||
                weight->dataType == DataType::INT4_NOZERO ||
                weight->dataType == DataType::INT8 ||
                weight->dataType == DataType::INT4_GROUP ||
                weight->dataType == DataType::FLOAT32 ||
                weight->dataType == DataType::FLOAT16 || 
                weight->dataType == DataType::FP8_E4M3 ||
                weight->dataType == DataType::DATA_GGUF_FORMAT;
    }

    void NumaLinearOp::Reshape(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        Data &input = *(datas.find("input")->second);
        Data &output = *(datas.find("output")->second);
        Data &weight = *(datas.find("weight")->second);

        AssertInFastLLM(weight.dims.size() == 2, "Linear's weight's shape's size should be 2.\n");
        AssertInFastLLM(input.dims.back() == weight.dims[1], "Linear's weight's shape error.\n");

        weight.weightType = WeightType::LINEAR;
        std::vector <int> dims = input.dims;
        dims.back() = weight.dims[0];

        if (intParams.find("exType") != intParams.end()) {
            LinearExType type = (LinearExType)intParams.find("exType")->second;
            if (type == LinearExType::ExSwiglu) {
                dims.back() /= 2;
            }
        }

        output.dataType = input.dataType;
        output.Resize(dims);
    }

    void NumaLinearOp::Run(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
// float inner = 0.0;        
// auto st = std::chrono::system_clock::now();
        Data &input = *(datas.find("input")->second);
        Data &output = *(datas.find("output")->second);
        Data &weight = *(datas.find("weight")->second);
        Data &bias = *(datas.find("bias")->second);

        output.Allocate();
        int n = input.Count(0) / input.dims.back();
        int m = input.dims.back();
        int k = output.dims.back();
        LinearExType exType = LinearExType::ExTypeNone;
        if (intParams.find("exType") != intParams.end()) {
            exType = (LinearExType)intParams.find("exType")->second;
            if (exType == LinearExType::ExSwiglu) {
                k *= 2;
            }
        }

        if (input.dataType == DataType::FLOAT32 && output.dataType == DataType::FLOAT32) {
            if (weight.dataType == DataType::FLOAT32 || weight.dataType == DataType::FLOAT16 || weight.dataType == DataType::FP8_E4M3
                || weight.dataType == DataType::DATA_GGUF_FORMAT) {
                GetNumaClient()->RunNumaLinearF(n, m, k, &weight, &bias, (float*)input.cpuData, (float*)output.cpuData, exType, input.dataType);
/*
                std::vector <float> tempOutputs;
                for (int i = 0; i < output.Count(0); i++) {
                    tempOutputs.push_back(((float*)output.cpuData)[i]);
                }
                CpuLinearOp::Run(opType, datas, floatParams, intParams);
                for (int i = 0; i < output.Count(0); i++) {
                    if (fabs(((float*)output.cpuData)[i] - tempOutputs[i]) > 1e-5) {
                        printf("wrong %d %f %f.\n", i, ((float*)output.cpuData)[i], tempOutputs[i]);
                        exit(0);
                    }
                }
*/
            } else if (weight.dataType == DataType::INT4 || 
                    weight.dataType == DataType::INT4_NOZERO ||
                    weight.dataType == DataType::INT4_GROUP ||
                    weight.dataType == DataType::INT8) {                
                float *inputData = (float *) input.cpuData;
                uint8_t *weightData = (uint8_t *) weight.cpuData;
                float *outputData = (float *) output.cpuData;
                float *biasData = bias.dims.size() > 0 ? (float *) bias.cpuData : nullptr;

                int permuteType = 1;
                if (weight.dataType == DataType::INT8) {
                    permuteType = 0;
                }

                weight.CalcWeightSum();

                int group = weight.group, groupCnt = weight.groupCnt;
                if (weight.dataType != DataType::INT4_GROUP) {
                    group = 1;
                    groupCnt = m;
                }

                std::vector<LowBitConfig> inputConfigs;
                inputConfigs.resize(n * group);
                std::vector<uint8_t> uinput;
                uinput.resize(n * m);

                {
                    if (n > 1) {
                        auto pool = GetAlivePool();
                        int threadNum = pool->threads.size();
                        int per = n / pool->threads.size();
                        int cur = 0;
                        std::vector<fastllm::MultiThreadOnlineQuantizationOp*> ops;
                        for (int i = 0; i < threadNum; i++) {
                            int end = (i == threadNum - 1 ? n : cur + per + (cur + per * (threadNum - i) < n));
                            ops.push_back(new MultiThreadOnlineQuantizationOp(
                                            inputData + cur * m, uinput.data() + cur * m, inputConfigs.data() + cur * group,
                                            end - cur, m, group, groupCnt, nullptr, nullptr, nullptr, permuteType));
                            cur = end;
                        }
                        for (int i = 0; i < threadNum; i++) {
                            pool->PushOp(i, ops[i]);
                        }
                        for (int i = 0; i < threadNum; i++) {
                            pool->Wait(i);
                            delete ops[i];
                        }
                    } else {
                        MultiThreadOnlineQuantizationOp(inputData, uinput.data(), inputConfigs.data(), n, m, group, groupCnt, nullptr, nullptr, nullptr, permuteType).Run();
                    }
                }

                if (weight.dataType == DataType::INT4) {
                    ErrorInFastLLM("Linear error: unsupport weight's dataType.\n");
                } else if (weight.dataType == DataType::INT8 || weight.dataType == DataType::INT4_NOZERO || weight.dataType == DataType::INT4_GROUP) {
// auto st = std::chrono::system_clock::now();
                    GetNumaClient()->RunNumaLinearU(n, m, k, group, groupCnt, &weight, &bias, &inputConfigs, uinput.data(), outputData, exType, output.dataType);
// float spend = GetSpan(st, std::chrono::system_clock::now());
// float gops = (float)n * m * k / spend / 1e9;
// inner = spend;
// if (n > 0) printf("n = %d, m = %d, k = %d, spend %f s, gops = %f (inner)\n", n, m, k, spend, gops);
                }
            } else {
                ErrorInFastLLM("Linear error: unsupport weight's dataType.\n");
            }
        } else if (input.dataType == DataType::FLOAT16 && output.dataType == DataType::FLOAT16) {
            if (weight.dataType == DataType::FLOAT32 || weight.dataType == DataType::FLOAT16) {
                ErrorInFastLLM("Linear error: unsupport weight's dataType.\n");
            } else if (weight.dataType == DataType::INT4 || 
                    weight.dataType == DataType::INT4_NOZERO ||
                    weight.dataType == DataType::INT4_GROUP ||
                    weight.dataType == DataType::INT8) {                
                uint16_t *inputData = (uint16_t *) input.cpuData;
                uint8_t *weightData = (uint8_t *) weight.cpuData;
                uint16_t *outputData = (uint16_t *) output.cpuData;
                float *biasData = bias.dims.size() > 0 ? (float *) bias.cpuData : nullptr;
                weight.CalcWeightSum();

                int permuteType = 1;
                if (weight.dataType == DataType::INT8) {
                    permuteType = 0;
                }

                int group = weight.group, groupCnt = weight.groupCnt;
                if (weight.dataType != DataType::INT4_GROUP) {
                    group = 1;
                    groupCnt = m;
                }

                int outputLen = output.Count(0);
                std::vector <float> floatInputData;
                floatInputData.resize(n * m);
                Float16ToFloat32(inputData, floatInputData.data(), n * m);
                
                std::vector<LowBitConfig> inputConfigs;
                inputConfigs.resize(n * group);
                std::vector<uint8_t> uinput;
                uinput.resize(n * m);

                if (n > 1) {
                    auto pool = GetAlivePool();
                    int threadNum = pool->threads.size();
                    int per = n / pool->threads.size();
                    int cur = 0;
                    std::vector<fastllm::MultiThreadOnlineQuantizationOp*> ops;
                    for (int i = 0; i < threadNum; i++) {
                        int end = (i == threadNum - 1 ? n : cur + per + (cur + per * (threadNum - i) < n));
                        ops.push_back(new MultiThreadOnlineQuantizationOp(
                                        floatInputData.data() + cur * m, uinput.data() + cur * m, inputConfigs.data() + cur * group,
                                        end - cur, m, group, groupCnt, nullptr, nullptr, nullptr, permuteType));
                        cur = end;
                    }
                    for (int i = 0; i < threadNum; i++) {
                        pool->PushOp(i, ops[i]);
                    }
                    for (int i = 0; i < threadNum; i++) {
                        pool->Wait(i);
                        delete ops[i];
                    }
                } else {
                    MultiThreadOnlineQuantizationOp(floatInputData.data(), uinput.data(), inputConfigs.data(), n, m, group, groupCnt, nullptr, nullptr, nullptr, permuteType).Run();
                }
                if (weight.dataType == DataType::INT4) {
                    ErrorInFastLLM("Linear error: unsupport weight's dataType.\n");
                } else if (weight.dataType == DataType::INT8 || weight.dataType == DataType::INT4_NOZERO || weight.dataType == DataType::INT4_GROUP) {
// auto st = std::chrono::system_clock::now();
                    GetNumaClient()->RunNumaLinearU(n, m, k, group, groupCnt, &weight, &bias, &inputConfigs, uinput.data(), (float*)outputData, exType, output.dataType);
// float spend = GetSpan(st, std::chrono::system_clock::now());
// float gops = (float)n * m * k / spend / 1e9;
// inner = spend;
// if (n > 0) printf("n = %d, m = %d, k = %d, spend %f s, gops = %f (inner)\n", n, m, k, spend, gops);
                }
            } else {
                ErrorInFastLLM("Linear error: unsupport weight's dataType.\n");
            }
        } else {
            ErrorInFastLLM("Linear error: unsupport weight's dataType.\n");
        }
// float spend = GetSpan(st, std::chrono::system_clock::now());
// float gops = (float)n * m * k / spend / 1e9;
// if (n > 0) printf("n = %d, m = %d, k = %d, spend %f s, gops = %f, outer = %f\n", n, m, k, spend, gops, spend - inner);
    }

    long long int NumaLinearOp::Ops(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        Data &input = *(datas.find("input")->second);
        Data &output = *(datas.find("output")->second);
        Data &weight = *(datas.find("weight")->second);

        int n = input.Count(0) / input.dims.back();
        int m = input.dims.back();
        int k = output.dims.back();

        return (long long int) n * m * k;
    }

    bool NumaMergeMOE::CanRun(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        Data &input = *(datas.find("input")->second);
        Data **biass = (Data**)(datas.find("biass")->second);
        if (biass[0] != nullptr && biass[0]->dims.size() > 0) {
            return false;
        }
        return true;
    }
    
    void NumaMergeMOE::Run(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
// auto ttt = std::chrono::system_clock::now();
// std::vector <std::pair <std::string, float> > record;
        Data &input = *(datas.find("input")->second);
        Data &output = *(datas.find("output")->second);
        Data &gateBias = *(datas.find("gateBias")->second);
        Data &logits = *(datas.find("logits")->second);
        Data **weights = (Data**)(datas.find("weights")->second);
        int topk = intParams.find("topk") != intParams.end() ? intParams.find("topk")->second : 1;
        int needNorm = intParams.find("needNorm") != intParams.end() ? intParams.find("needNorm")->second : 0;
        float sharedScale = floatParams.find("sharedScale") != floatParams.end() ? floatParams.find("sharedScale")->second : 1.0f;        
        float routeScale = floatParams.find("routeScale") != floatParams.end() ? floatParams.find("routeScale")->second : 1.0f;        
        output.Allocate();

        int dimsLen = logits.dims.size();
        int outer = logits.Count(0) / logits.Count(dimsLen - 1);
        int channels = logits.dims[dimsLen - 1];
        int n = input.dims[0], m = input.dims[1], k = output.dims[1];

        if (weights[2]->dataType == DataType::FP8_E4M3 ||
            weights[2]->dataType == DataType::FLOAT16 ||
            weights[2]->dataType == DataType::BFLOAT16 ||
            weights[2]->dataType == DataType::DATA_GGUF_FORMAT) {
            if (n > 31) {
                std::vector <std::vector <fastllm::Data*> > ws;
                std::vector <std::vector <float> > factors;
                ws.resize(n);
                factors.resize(n);
                for (int o = 0; o < n; o++) {
                    std::vector <std::pair <float, int> > oriV;
                    for (int j = 0; j < channels; j++) {
                        oriV.push_back(std::make_pair(-((float*)logits.cpuData)[o * channels + j], j));
                    }
                    if (gateBias.dims.size() > 0) {
                        ToDataType(gateBias, DataType::FLOAT32);
                        gateBias.ToDevice(DataDevice::CPU);
                        float *cpuBias = (float*)gateBias.cpuData;
                        for (int i = 0; i < channels; i++) {
                            oriV[i].first -= cpuBias[i];
                        }
                    }
                    // sort(oriV.begin(), oriV.end());
                    std::nth_element(oriV.begin(), oriV.begin() + topk, oriV.end());
                    float sum = 1.0;
                    if (needNorm) {
                        sum = 0.0;
                        for (int j = 0; j < topk; j++) {
                            sum += ((float*)logits.cpuData)[o * channels + oriV[j].second];
                        }
                    }

                    std::vector <std::pair <int, float> > v;
                    for (int j = 0; j < topk; j++) {
                        v.push_back(std::make_pair(oriV[j].second + 1, ((float*)logits.cpuData)[o * channels + oriV[j].second] / sum * routeScale));
                    }
                    v.push_back(std::make_pair(0, sharedScale));
                    for (int i = 0; i < v.size(); i++) {
                        if (weights[v[i].first * 2] == nullptr) {
                            continue;
                        }
                        ws[o].push_back(weights[v[i].first * 2]);
                        ws[o].push_back(weights[v[i].first * 2 + 1]);
                        factors[o].push_back(v[i].second);
                    }
                }
                GetNumaClient()->RunNumaMOEFMultiRow(n, m, k, ws, factors, ((float *) input.cpuData), ((float*)output.cpuData), output.dataType);
            } else {
                float *floatLogits = ((float*)logits.cpuData);
                for (int o = 0; o < outer; o++) {
                    std::vector <std::pair <float, int> > oriV;
                    oriV.resize(channels);
                    for (int j = 0; j < channels; j++) {
                        oriV[j].first = -floatLogits[o * channels + j];
                        oriV[j].second = j;
                    }
                    if (gateBias.dims.size() > 0) {
                        if (gateBias.dataType != DataType::FLOAT32) {
                            ToDataType(gateBias, DataType::FLOAT32);
                        }
                        float *cpuBias = (float*)gateBias.cpuData;
                        for (int i = 0; i < channels; i++) {
                            oriV[i].first -= cpuBias[i];
                        }
                    }
                    std::partial_sort(oriV.begin(), oriV.begin() + topk, oriV.end());

                    float sum = 1.0;
                    if (needNorm) {
                        sum = 0.0;
                        for (int j = 0; j < topk; j++) {
                            sum += floatLogits[o * channels + oriV[j].second];
                        }
                    }
                    std::vector <std::pair <int, float> > v;
                    for (int j = 0; j < topk; j++) {
                        v.push_back(std::make_pair(oriV[j].second + 1, floatLogits[o * channels + oriV[j].second] / sum * routeScale));
                    }
                    v.push_back(std::make_pair(0, sharedScale));
                    float *inputData = ((float *) input.cpuData) + o * m;
                    std::vector <fastllm::Data*> ws;
                    std::vector <float> factors;
                    for (int i = 0; i < v.size(); i++) {
                        if (weights[v[i].first * 2] == nullptr) {
                            continue;
                        }
                        ws.push_back(weights[v[i].first * 2]);
                        ws.push_back(weights[v[i].first * 2 + 1]);
                        factors.push_back(v[i].second);
                    }
// record.push_back(std::make_pair("prepare", GetSpan(ttt, std::chrono::system_clock::now())));
                    GetNumaClient()->RunNumaMOEF(1, m, k, ws, factors, inputData, ((float*)output.cpuData) + o * k, output.dataType);
// record.push_back(std::make_pair("finish output", GetSpan(ttt, std::chrono::system_clock::now())));
                }
// for (int i = 0; i < record.size(); i++) {
        // printf("%s spend %f s.\n", record[i].first.c_str(), record[i].second);
// }
            }
        } else {
            int permuteType = 1;
            if (weights[2]->dataType == DataType::INT8) {
                permuteType = 0;
            }

            if (n > 31) {
    // auto st = std::chrono::system_clock::now();
                int group = weights[2]->group, groupCnt = weights[2]->groupCnt;
                if (weights[2]->dataType != DataType::INT4_GROUP) {
                    group = 1;
                    groupCnt = m;
                }
                float *inputData = ((float *) input.cpuData);
                std::vector<LowBitConfig> inputConfigs;
                std::vector<uint8_t> uinput;
                std::vector <float> inputSums;
                std::vector <float> iscales, izeros;

                // 注意，这里因为ComputeServer会做反量化，所以不用管输入本身是什么type，都不用旋转
                OnlineQuantization(inputData, uinput, inputConfigs, n, m, group, groupCnt, inputSums, iscales, izeros, -1);

                std::vector <std::vector <fastllm::Data*> > ws;
                std::vector <std::vector <float> > factors;
                ws.resize(n);
                factors.resize(n);
                for (int o = 0; o < n; o++) {
                    std::vector <std::pair <float, int> > oriV;
                    for (int j = 0; j < channels; j++) {
                        oriV.push_back(std::make_pair(-((float*)logits.cpuData)[o * channels + j], j));
                    }
                    if (gateBias.dims.size() > 0) {
                        ToDataType(gateBias, DataType::FLOAT32);
                        gateBias.ToDevice(DataDevice::CPU);
                        float *cpuBias = (float*)gateBias.cpuData;
                        for (int i = 0; i < channels; i++) {
                            oriV[i].first -= cpuBias[i];
                        }
                    }
                    // sort(oriV.begin(), oriV.end());
                    std::nth_element(oriV.begin(), oriV.begin() + topk, oriV.end());
                    float sum = 1.0;
                    if (needNorm) {
                        sum = 0.0;
                        for (int j = 0; j < topk; j++) {
                            sum += ((float*)logits.cpuData)[o * channels + oriV[j].second];
                        }
                    }

                    std::vector <std::pair <int, float> > v;
                    for (int j = 0; j < topk; j++) {
                        v.push_back(std::make_pair(oriV[j].second + 1, ((float*)logits.cpuData)[o * channels + oriV[j].second] / sum * routeScale));
                    }
                    v.push_back(std::make_pair(0, sharedScale));
                    for (int i = 0; i < v.size(); i++) {
                        if (weights[v[i].first * 2] == nullptr) {
                            continue;
                        }
                        ws[o].push_back(weights[v[i].first * 2]);
                        ws[o].push_back(weights[v[i].first * 2 + 1]);
                        factors[o].push_back(v[i].second);
                    }
                }
                GetNumaClient()->RunNumaMOEUMultiRow(n, m, k, group, groupCnt, ws, factors, &inputConfigs, uinput.data(), ((float*)output.cpuData), output.dataType);
            } else {
                float *floatLogits = ((float*)logits.cpuData);
                for (int o = 0; o < outer; o++) {
                    std::vector <std::pair <float, int> > oriV;
                    oriV.resize(channels);
                    for (int j = 0; j < channels; j++) {
                        oriV[j].first = -floatLogits[o * channels + j];
                        oriV[j].second = j;
                    }
                    if (gateBias.dims.size() > 0) {
                        if (gateBias.dataType != DataType::FLOAT32) {
                            ToDataType(gateBias, DataType::FLOAT32);
                        }
                        float *cpuBias = (float*)gateBias.cpuData;
                        for (int i = 0; i < channels; i++) {
                            oriV[i].first -= cpuBias[i];
                        }
                    }
                    std::partial_sort(oriV.begin(), oriV.begin() + topk, oriV.end());

                    float sum = 1.0;
                    if (needNorm) {
                        sum = 0.0;
                        for (int j = 0; j < topk; j++) {
                            sum += floatLogits[o * channels + oriV[j].second];
                        }
                    }
                    std::vector <std::pair <int, float> > v;
                    for (int j = 0; j < topk; j++) {
                        v.push_back(std::make_pair(oriV[j].second + 1, floatLogits[o * channels + oriV[j].second] / sum * routeScale));
                    }
                    v.push_back(std::make_pair(0, sharedScale));
                    float *inputData = ((float *) input.cpuData) + o * m;
                    int group = weights[2]->group, groupCnt = weights[2]->groupCnt;
                    if (weights[2]->dataType != DataType::INT4_GROUP) {
                        group = 1;
                        groupCnt = m;
                    }
                    std::vector<LowBitConfig> inputConfigs;
                    std::vector<uint8_t> uinput;
                    std::vector <float> inputSums;
                    std::vector <float> iscales, izeros;
                    OnlineQuantization(inputData, uinput, inputConfigs, 1, m, group, groupCnt, 
                                        inputSums, iscales, izeros, permuteType);
                    
                    std::vector <fastllm::Data*> ws;
                    std::vector <float> factors;
                    for (int i = 0; i < v.size(); i++) {
                        if (weights[v[i].first * 2] == nullptr) {
                            continue;
                        }
                        ws.push_back(weights[v[i].first * 2]);
                        ws.push_back(weights[v[i].first * 2 + 1]);
                        factors.push_back(v[i].second);
                    }
    // record.push_back(std::make_pair("prepare", GetSpan(ttt, std::chrono::system_clock::now())));
                    GetNumaClient()->RunNumaMOEU(1, m, k, group, groupCnt, ws, factors, &inputConfigs, uinput.data(), ((float*)output.cpuData) + o * k, output.dataType);
    // record.push_back(std::make_pair("finish output", GetSpan(ttt, std::chrono::system_clock::now())));
                }
    // for (int i = 0; i < record.size(); i++) {
        // printf("%s spend %f s.\n", record[i].first.c_str(), record[i].second);
    // }
            }
        }
    }

    void NumaCatDirectOp::Run(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        Data *input0 = (datas.find("input0")->second);
        Data *input1 = (datas.find("input1")->second);

        if (input0->isKVCache) {
            // 如果是kvCache，那么要同步到server上
            GetNumaClient()->AppendKVCache(input0->cacheUid, input1);

            int axis = intParams.find("axis") != intParams.end() ? intParams.find("axis")->second : -1;
            if (input0->dims.size() == 0) {
                input0->Resize(input1->dims);
                return;
            }

            int dimsLen = input0->dims.size();
            axis = (axis % dimsLen + dimsLen) % dimsLen;

            std::vector <int> dims = input0->dims;
            std::vector <int> oldDims = dims;
            dims[axis] += input1->dims[axis];
            input0->Resize(dims);
        } else {
            CpuCatDirectOp::Run(opType, datas, floatParams, intParams);
        }
    }

    void NumaAttention::Run(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        Data &q = *(datas.find("q")->second);
        Data &k = *(datas.find("k")->second);
        Data &v = *(datas.find("v")->second);
        int maskType = intParams.find("maskType") != intParams.end() ? intParams.find("maskType")->second : 0;

        if (!k.isKVCache || !v.isKVCache || maskType != 0) {
            CpuAttention::Run(opType, datas, floatParams, intParams);
            return;
        }

        int group = intParams.find("group") != intParams.end() ? intParams.find("group")->second : q.dims[0] / k.dims[0];
        float scale = floatParams.find("scale") != floatParams.end() ? floatParams.find("scale")->second : 1.0;

        Data &output = *(datas.find("output")->second);
        output.Allocate();

        GetNumaClient()->Attention(&q, &k, &v, group, scale, maskType, &output);
/*
        std::vector <float> tempOutput;
        for (int i = 0; i < output.Count(0); i++) {
            tempOutput.push_back(((float*)output.cpuData)[i]);
        }

        CpuAttention::Run(opType, datas, floatParams, intParams);
        for (int i = 0; i < output.Count(0); i++) {
            float x = ((float*)output.cpuData)[i];
            if (fabs(x - tempOutput[i]) > 1e-5) {
                printf("wrong %d %f %f\n", i, x, tempOutput[i]);
                exit(0);
            }
        }
*/
    }

    void NumaAttentionBatchOp::Run(const std::string &opType, const fastllm::DataDict &datas,
                                    const fastllm::FloatDict &floatParams, const fastllm::IntDict &intParams) {
        fastllm::BaseOperator *op = (fastllm::BaseOperator*)(new NumaAttention());
        int batch = intParams.find("q___batch")->second;
        DataDict tempDatas = datas;
        for (int i = 0; i < batch; i++) {
            tempDatas["q"] = ((Data**)datas.find("q")->second)[i];
            tempDatas["k"] = ((Data**)datas.find("k")->second)[i];
            tempDatas["v"] = ((Data**)datas.find("v")->second)[i];
            tempDatas["mask"] = ((Data**)datas.find("mask")->second)[i];
            tempDatas["output"] = ((Data**)datas.find("output")->second)[i];
            op->Run("Attention", tempDatas, floatParams, intParams);
        }
        delete op;
    }

    void NumaCatDirectBatchOp::Run(const std::string &opType, const fastllm::DataDict &datas,
                                  const fastllm::FloatDict &floatParams, const fastllm::IntDict &intParams) {
        fastllm::BaseOperator *op = (fastllm::BaseOperator*)(new NumaCatDirectOp());
        int batch = intParams.find("input0___batch")->second;
        DataDict tempDatas = datas;
        for (int i = 0; i < batch; i++) {
            tempDatas["input0"] = ((Data**)datas.find("input0")->second)[i];
            tempDatas["input1"] = ((Data**)datas.find("input1")->second)[i];
            op->Run("CatDirect", tempDatas, floatParams, intParams);
        }
        delete op;
    }

    void RegisterFastllmData(fastllm::Data *data, const std::string &weightType) {
        GetNumaClient()->RegisterFastllmData(data, weightType);
    }
}