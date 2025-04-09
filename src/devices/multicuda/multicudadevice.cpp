//
// Created by huangyuyang on 8/2/24.
//

#include "devices/cpu/cpudevice.h"
#include "devices/cuda/cudadevice.h"
#include "devices/cuda/fastllm-cuda.cuh"
#include "devices/multicuda/multicudadevice.h"

#include "fastllm-multicuda.cuh"

#include "utils.h"

namespace fastllm {
    MultiCudaDevice::MultiCudaDevice(CudaDevice *cudaDevice) {
        this->cudaDevice = cudaDevice;
        this->deviceType = "multicuda";

        this->ops["MLP"] = (BaseOperator*)(new MultiCudaMLPOp());
        this->ops["Linear"] = (BaseOperator*)(new MultiCudaLinearOp());
        // this->ops["MergeAttention"] = (BaseOperator*)(new MultiCudaMergeAttention());
    }

    bool MultiCudaDevice::Malloc(void **ret, size_t size) {
        *ret = FastllmCudaMalloc(size);
        return true;
    }

    bool MultiCudaDevice::Free(void *ret) {
        FastllmCudaFree(ret);
        return true;
    }

    bool MultiCudaDevice::CopyDataFromCPU(void *dst, void *src, size_t size) {
        FastllmCudaCopyFromHostToDevice(dst, src, size);
        return true;
    }

    bool MultiCudaDevice::CopyDataToCPU(void *dst, void *src, size_t size) {
        FastllmCudaCopyFromDeviceToHost(dst, src, size);
        return true;
    }

    bool MultiCudaDevice::CanRun(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        if (this->ops.find(opType) == this->ops.end()) {
            if (((BaseDevice*)this->cudaDevice)->ops.find(opType) == ((BaseDevice*)this->cudaDevice)->ops.end()) {
                return false;
            } else {
                return ((BaseDevice*)this->cudaDevice)->CanRun(opType, datas, floatParams, intParams);
            }
        } else {
            return this->ops[opType]->CanRun(opType, datas, floatParams, intParams);
        }
    }

    // 对某一个算子进行形状推理
    void MultiCudaDevice::Reshape(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        if (this->ops.find(opType) == this->ops.end()) {
            ((BaseDevice*)this->cudaDevice)->Reshape(opType, datas, floatParams, intParams);
        } else {
            this->ops[opType]->Reshape(opType, datas, floatParams, intParams);
        }
    }

    // 对某一个算子进行推理
    void MultiCudaDevice::Run(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        if (this->ops.find(opType) == this->ops.end()) {
            ((BaseDevice*)this->cudaDevice)->Run(opType, datas, floatParams, intParams);
        } else {
            this->ops[opType]->Run(opType, datas, floatParams, intParams);
        }
    }

    void MultiCudaMLPOp::Reshape(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        Data &input = *(datas.find("input")->second);
        Data &output = *(datas.find("output")->second);
        Data &weight0 = *(datas.find("weight0")->second);
        Data &weight1 = *(datas.find("weight1")->second);

        AssertInFastLLM(weight0.dims.size() == 2 && weight1.dims.size() == 2, "MLP's weight's shape's size should be 2.\n");
        AssertInFastLLM(input.dims.back() == weight0.dims[1], "MLP's weight's shape error.\n");
        AssertInFastLLM(weight0.dims[0] / 2 == weight1.dims[1], "MLP's weight's shape error.\n");
        AssertInFastLLM(weight0.dataType == weight1.dataType, "MLP's weight's data type error.\n");

        weight0.weightType = WeightType::LINEAR;
        weight1.weightType = WeightType::LINEAR;
        std::vector <int> dims = input.dims;
        dims.back() = weight1.dims[0];

        output.dataType = input.dataType;
        output.Resize(dims);
    }

    void MultiCudaMLPOp::Run(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        Data &input = *(datas.find("input")->second);
        Data &output = *(datas.find("output")->second);
        Data &weight0 = *(datas.find("weight0")->second);
        Data &bias0 = *(datas.find("bias0")->second);
        Data &weight1 = *(datas.find("weight1")->second);
        Data &bias1 = *(datas.find("bias1")->second);

        output.Allocate();
        FastllmMultiCudaMLP(input, weight0, weight1, output);
    }

    bool MultiCudaLinearOp::CanRun(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        if (intParams.find("exType") != intParams.end()) {
            return false;
        }
        Data &weight = *(datas.find("weight")->second);
        return weight.dims[0] > 10000 || weight.dims[1] > 10000;
    }

    void MultiCudaLinearOp::Run(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
// auto st = std::chrono::system_clock::now();
        Data &input = *(datas.find("input")->second);
        Data &output = *(datas.find("output")->second);
        Data &weight = *(datas.find("weight")->second);
        Data &bias = *(datas.find("bias")->second);

        output.Allocate();
        int n = input.Count(0) / input.dims.back();
        int m = input.dims.back();
        int k = output.dims.back();

        if (input.dataType == DataType::FLOAT16) {
            if (weight.dataType == DataType::FLOAT16 ||
                weight.dataType == DataType::INT8 ||
                weight.dataType == DataType::INT4_NOZERO ||
                weight.dataType == DataType::INT4_GROUP) {
                FastllmMultiCudaMatMul(input, weight, bias, output, n, m, k);
            } else {
                ErrorInFastLLM("Linear error: unsupport weight's dataType.\n");
            }
        } else if (input.dataType == DataType::FLOAT32) {
            if (weight.dataType == DataType::FLOAT32) {
                FastllmCudaMatMulFloat32(input, weight, bias, output, n, m, k);
            } else if (weight.dataType == DataType::FLOAT16 ||
                        weight.dataType == DataType::INT8 ||
                        weight.dataType == DataType::INT4_NOZERO ||
                        weight.dataType == DataType::INT4_GROUP) {
                FastllmMultiCudaMatMul(input, weight, bias, output, n, m, k);
            } else if (weight.dataType == DataType::INT4) {
                FastllmCudaMatMulFloatInt4(input, weight, bias, output, n, m, k);
            } else {
                ErrorInFastLLM("Linear error: unsupport weight's dataType.\n");
            }
        } else {
            ErrorInFastLLM("Linear error: unsupport input's dataType.\n");
        }
// float spend = GetSpan(st, std::chrono::system_clock::now());
// float gops = (float)n * m * k / spend / 1e9;
// printf("n = %d, m = %d, k = %d, spend %f s, gops = %f\n", n, m, k, spend, gops);
    }
    
    void MultiCudaMergeAttention::Reshape(const std::string &opType, const fastllm::DataDict &datas,
                               const fastllm::FloatDict &floatParams, const fastllm::IntDict &intParams) {
        Data &input = *(datas.find("input")->second);
        Data &weight1 = *(datas.find("weight1")->second);
        Data &output = *(datas.find("output")->second);
        std::vector <int> dims = input.dims;
        dims.back() = weight1.dims[0];
        output.dataType = input.dataType;
        output.Resize(dims);
    }

    struct MultiCudaDoMergeAttentionOp : MultiThreadBaseOp {
        uint8_t *oriCudaInput, *oriCpuInput, *partOutput;
        Data *input, *weight0, *bias0, *weight1, *bias1;
        Data *qkv, *q, *k, *v;
        int qNum, kvNum, headDim, rotDim;
        float attentionScale;
        Data *positionIds, *sinData, *cosData;
        Data **keys, **values, **masks;
        Data *output;
        int deviceId;

        MultiCudaDoMergeAttentionOp(uint8_t *oriCudaInput, uint8_t *oriCpuInput, uint8_t *partOutput,
                            Data *input, Data *weight0, Data *bias0, Data *weight1, Data *bias1, 
                            Data *qkv, Data *q, Data *k, Data *v,
                            int qNum, int kvNum, int headDim, int rotDim, float attentionScale,
                            Data *positionIds, Data *sinData, Data *cosData,
                            Data** keys, Data** values, Data** masks, 
                            Data *output, int deviceId) : 
                oriCudaInput(oriCudaInput), oriCpuInput(oriCpuInput), partOutput(partOutput),
                input(input), weight0(weight0), bias0(bias0), weight1(weight1), bias1(bias1), 
                qkv(qkv), q(q), k(k), v(v), 
                qNum(qNum), kvNum(kvNum), headDim(headDim), rotDim(rotDim), attentionScale(attentionScale),
                positionIds(positionIds), sinData(sinData), cosData(cosData),
                keys(keys), values(values), masks(masks), 
                output(output), deviceId(deviceId) {}

        void Run() {
            FastllmCudaSetDevice(deviceId);
            if (deviceId == 0) {
                input->cudaData = oriCudaInput;
            } else {
                input->Allocate();
                FastllmCudaCopyFromHostToDevice(input->cudaData, oriCpuInput, input->GetBytes());
            }

            int bsz = input->dims[0], seqlen = input->dims[1];

            DoCudaLinearReshape(*input, *weight0, *qkv);
            DoCudaLinear(*input, *weight0, *bias0, *qkv);
            int per = qkv->dims.back() / (qNum / kvNum + 2);
            int qdim = per * (qNum / kvNum);
            DoCudaSplitReshape(*qkv, -1, 0, qdim, *q);
            DoCudaSplitReshape(*qkv, -1, qdim, qdim + per, *k);
            DoCudaSplitReshape(*qkv, -1, qdim + per, qdim + per * 2, *v);
            DoCudaSplit(*qkv, -1, 0, qdim, *q);
            DoCudaSplit(*qkv, -1, qdim, qdim + per, *k);
            DoCudaSplit(*qkv, -1, qdim + per, qdim + per * 2, *v);

            std::vector <int> qkvSize = {bsz, seqlen, -1, headDim};
            q->Reshape(qkvSize);
            k->Reshape(qkvSize);
            v->Reshape(qkvSize);

            FastllmCudaLlamaRotatePosition2D(*q, *positionIds, *sinData, *cosData, rotDim);
            FastllmCudaLlamaRotatePosition2D(*k, *positionIds, *sinData, *cosData, rotDim);

            DoCudaPermuteSelf(*q, {0, 2, 1, 3});
            DoCudaPermuteSelf(*k, {0, 2, 1, 3});
            DoCudaPermuteSelf(*v, {0, 2, 1, 3});

            qkvSize = {-1, seqlen, headDim};
            q->Reshape(qkvSize);
            k->Reshape(qkvSize);
            v->Reshape(qkvSize);

            int unitLen = 128;
            Data &pastKey = *keys[0];
            Data &pastValue = *values[0];
            while ((pastKey.dims.size() == 0 && (pastKey.expansionDims.size() == 0 || seqlen > pastKey.expansionDims[1]))
                || (pastKey.dims.size() > 0 && pastKey.dims[1] + seqlen > pastKey.expansionDims[1])) {
                std::vector <int> newDims;
                if (pastKey.Count(0) == 0 || pastKey.dims.size() == 0) {
                    newDims = std::vector <int> {kvNum, ((seqlen - 1) / unitLen + 1) * unitLen, headDim};
                } else {
                    newDims = pastKey.dims;
                    newDims[1] += ((seqlen - 1) / unitLen + 1) * unitLen;
                }
                pastKey.Expansion(newDims);
            }
            while ((pastValue.dims.size() == 0 && (pastValue.expansionDims.size() == 0 || seqlen > pastValue.expansionDims[1]))
                || (pastValue.dims.size() > 0 && pastValue.dims[1] + seqlen > pastValue.expansionDims[1])) {
                std::vector <int> newDims;
                if (pastValue.Count(0) == 0 || pastValue.dims.size() == 0) {
                    newDims = std::vector <int> {kvNum, ((seqlen - 1) / unitLen + 1) * unitLen, headDim};
                } else {
                    newDims = pastValue.dims;
                    newDims[1] += ((seqlen - 1) / unitLen + 1) * unitLen;
                }
                pastValue.Expansion(newDims);
            }
            DoCudaCatDirect(pastKey, *k, 1);
            DoCudaCatDirect(pastValue, *v, 1);
            DoCudaAttentionReshape(*q, pastValue, *qkv);
            DoCudaAttention(*q, pastKey, pastValue, *masks[0], *qkv, q->dims[0] / pastKey.dims[0], attentionScale, 1);
            DoCudaPermuteSelf(*qkv, {1, 0, 2});
            qkv->Reshape({seqlen, bsz, -1});
            DoCudaPermuteSelf(*qkv, {1, 0, 2});
            DoCudaLinearReshape(*qkv, *weight1, *output);

            if (deviceId == 0) {
                output->UpdateUnitSize();
                output->cudaData = partOutput;
                output->expansionSize = output->Count(0);
                output->expansionBytes = (output->Count(0) * output->unitSize - 1) / output->unitSizeDiv + 1;
            }
            DoCudaLinear(*qkv, *weight1, *bias1, *output);
            if (deviceId != 0) {
                FastllmCudaCopyFromDeviceToDevice(partOutput, output->cudaData, output->GetBytes());
            }
        }
    };

    void MultiCudaMergeAttention::Run(const std::string &opType, const fastllm::DataDict &datas,
                           const fastllm::FloatDict &floatParams, const fastllm::IntDict &intParams) {
        Data &input = *(datas.find("input")->second);
        Data &weight0 = *(datas.find("weight0")->second);
        Data &bias0 = *(datas.find("bias0")->second);
        Data &weight1 = *(datas.find("weight1")->second);
        Data &bias1 = *(datas.find("bias1")->second);
        Data &positionIds = *(datas.find("positionIds")->second);
        Data &sinData = *(datas.find("sinData")->second);
        Data &cosData = *(datas.find("cosData")->second);
        Data &output = *(datas.find("output")->second);
        Data &qkv = *(datas.find("qkv")->second);
        Data &q = *(datas.find("q")->second);
        Data &k = *(datas.find("k")->second);
        Data &v = *(datas.find("v")->second);
        int qNum = intParams.find("qNum")->second;
        int kvNum = intParams.find("kvNum")->second;
        int headDim = intParams.find("headDim")->second;
        int rotDim = intParams.find("rotDim")->second;
        float attentionScale = floatParams.find("attentionScale")->second;
        Data **keys = (Data**)(datas.find("keys")->second);
        Data **values = (Data**)(datas.find("values")->second);
        Data **masks = (Data**)(datas.find("masks")->second);
        output.Allocate();
// auto st = std::chrono::system_clock::now();
        int group = qNum / kvNum;
        int vDim = weight1.dims[0] / qNum;
        std::vector <int> devices;
        std::map <int, int> ratios;
        FastllmGetMulticudaDeviceAndRatio(devices, ratios, true);
        std::vector <int> points = FastllmMultiCudaGetSplitPoints(devices, ratios, kvNum, 1);
        DivisionScheme divisionScheme, divisionSchemeO;
        for (int i = 0; i < devices.size(); i++) {
            int st = points[i], end = points[i + 1];
            int deviceId = devices[i];
            int qgap = qNum * headDim, qkgap = (qNum + kvNum) * headDim;
            divisionScheme[deviceId].push_back(std::make_pair(st * group * headDim, end * group * headDim));
            divisionScheme[deviceId].push_back(std::make_pair(qgap + st * headDim, qgap + end * headDim));
            divisionScheme[deviceId].push_back(std::make_pair(qkgap + st * headDim, qkgap + end * headDim));

            divisionSchemeO[deviceId].push_back(std::make_pair(st * group * vDim, end * group * vDim));
        }
        SplitMultiCudaWeight(weight0, bias0, devices, divisionScheme, 0);
        SplitMultiCudaWeight(weight1, bias1, devices, divisionSchemeO, 1);
        CopyToMultiDevices(qkv, devices, true);
        CopyToMultiDevices(q, devices, true);
        CopyToMultiDevices(k, devices, true);
        CopyToMultiDevices(v, devices, true);
        CopyToMultiDevices(positionIds, devices, true);
        CopyToMultiDevices(sinData, devices, true);
        CopyToMultiDevices(cosData, devices, true);
        CopyToMultiDevices(*keys[0], devices, true);
        CopyToMultiDevices(*values[0], devices, true);
        CopyToMultiDevices(*masks[0], devices, true);
        std::map <int, std::vector <Data*> > curKeys, curValues, curMasks;
        for (int device : devices) {
            for (int i = 0; i < 1; i++) {
                curKeys[device].push_back(keys[i]->multiDeviceDatas[device]);
                curValues[device].push_back(values[i]->multiDeviceDatas[device]);
                curMasks[device].push_back(masks[i]->multiDeviceDatas[device]);
            }
        }
        Data curInput, curOutput;
        CopyToMultiDevices(input, devices, false);
        curOutput.dataDevice = input.dataDevice;
        CopyToMultiDevices(curOutput, devices, false);
        std::vector <uint8_t> cpuInput;
        cpuInput.resize(input.GetBytes());
        FastllmCudaSetDevice(0);
        FastllmCudaCopyFromDeviceToHost(cpuInput.data(), input.cudaData, input.GetBytes());
        uint8_t *partOutput = (uint8_t*)FastllmCudaMalloc(output.GetBytes() * devices.size());
        auto *pool = fastllm::GetAlivePool();
        std::vector<fastllm::MultiThreadBaseOp*> ops;
        for (int i = 0; i < devices.size(); i++) {
            auto device = devices[i];
            ops.push_back(new MultiCudaDoMergeAttentionOp (
                (uint8_t*)input.cudaData, (uint8_t*)cpuInput.data(), partOutput + output.GetBytes() * i,
                input.multiDeviceDatas[device], 
                weight0.multiDeviceDatas[device], bias0.multiDeviceDatas[device], 
                weight1.multiDeviceDatas[device], bias1.multiDeviceDatas[device], 
                qkv.multiDeviceDatas[device], q.multiDeviceDatas[device], k.multiDeviceDatas[device], v.multiDeviceDatas[device], 
                qNum, kvNum, headDim, rotDim, attentionScale, 
                positionIds.multiDeviceDatas[device], sinData.multiDeviceDatas[device], cosData.multiDeviceDatas[device], 
                curKeys[device].data(), curValues[device].data(), curMasks[device].data(), 
                curOutput.multiDeviceDatas[device], device));
        }
        for (int i = 0; i < devices.size(); i++) {
            pool->PushOp(i, ops[i]);
        }
        // ops[0]->Run();
        for (int i = 0; i < devices.size(); i++) {
            pool->Wait(i);
            delete ops[i];
        }
// printf("calc spend %f s.\n", GetSpan(st, std::chrono::system_clock::now()));
        FastllmReduce((uint8_t*)output.cudaData, partOutput, output.Count(0), devices.size(), output.dataType);
// printf("FastllmReduce spend %f s.\n", GetSpan(st, std::chrono::system_clock::now()));
        keys[0]->dims = keys[0]->multiDeviceDatas[devices[0]]->dims;
        keys[0]->expansionDims = keys[0]->multiDeviceDatas[devices[0]]->expansionDims;
        values[0]->dims = values[0]->multiDeviceDatas[devices[0]]->dims;
        values[0]->expansionDims = values[0]->multiDeviceDatas[devices[0]]->expansionDims;
// printf("last spend %f s.\n", GetSpan(st, std::chrono::system_clock::now()));
    }
}