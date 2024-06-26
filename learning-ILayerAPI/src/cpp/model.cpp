#include <cstring>
#include <memory>
#include <iostream>
#include <fstream>
#include <string>
#include <type_traits>
#include <assert.h>
#include <utility>
#include <algorithm>

#include "model.hpp"
#include "NvInfer.h"
#include "NvOnnxParser.h"
#include "utils.hpp"
#include "cuda_runtime.h"
#include "math.h"

static float input_5x5[] = {
    0.7576, 0.2793, 0.4031, 0.7347, 0.0293,
    0.7999, 0.3971, 0.7544, 0.5695, 0.4388,
    0.6387, 0.5247, 0.6826, 0.3051, 0.4635,
    0.4550, 0.5725, 0.4980, 0.9371, 0.6556,
    0.3138, 0.1980, 0.4162, 0.2843, 0.3398};

static float input_1x5[] = {
    0.7576, 0.2793, 0.4031, 0.7347, 0.0293};


using namespace std;
class Logger : public nvinfer1::ILogger{
public:
    virtual void log (Severity severity, const char* msg) noexcept override{
        string str;
        switch (severity){
            case Severity::kINTERNAL_ERROR: str = RED    "[fatal]: " CLEAR; break;
            case Severity::kERROR:          str = RED    "[error]: " CLEAR; break;
            case Severity::kWARNING:        str = BLUE   "[warn]: "  CLEAR; break;
            case Severity::kINFO:           str = YELLOW "[info]: "  CLEAR; break;
            case Severity::kVERBOSE:        str = PURPLE "[verb]: "  CLEAR; break;
        }
        if (severity <= Severity::kINFO)
            cout << str << string(msg) << endl;
    }
};

struct InferDeleter
{
    template <typename T>
    void operator()(T* obj) const
    {
        delete obj;
    }
};

template <typename T>
using make_unique = std::unique_ptr<T, InferDeleter>;

Model::Model(string path){
    if (getFileType(path) == ".onnx")
        mOnnxPath = path;
    else if (getFileType(path) == ".weights")
        mWtsPath = path;
    else 
        LOGE("ERROR: %s, wrong weight or model type selected. Program terminated", getFileType(path).c_str());

    mEnginePath = getEnginePath(path);
}

/*
decode一个weights文件，并保存到map中
weights的格式是:
   count
   [name][len][weights value in hex mode]
   [name][len][weights value in hex mode]
   ...
*/
// 导入权重信息
map<string, nvinfer1::Weights> Model::loadWeights(){ 
    ifstream f;
    if (!fileExists(mWtsPath)){ 
        LOGE("ERROR: %s not found", mWtsPath.c_str());
    }

    f.open(mWtsPath);

    int32_t size;
    map<string, nvinfer1::Weights> maps;
    f >> size; 

    if (size <= 0) {
        LOGE("ERROR: no weights found in %s", mWtsPath.c_str());
    }

    while (size > 0) {
        nvinfer1::Weights weight;
        string name;
        int weight_length;

        f >> name;
        f >> std::dec >> weight_length; // 以十进制的格式存到变量中

        uint32_t* values = new uint32_t[weight_length];
        for (int i = 0; i < weight_length; i ++) {
            f >> std::hex >> values[i];  // 以十六进制的格式存到变量中
        }

        weight.type = nvinfer1::DataType::kFLOAT;
        weight.count = weight_length;
        weight.values = std::move(values);

        maps[name] = weight;

        size --;
    }

    return maps;
}


bool Model::build() {
    if(!mOnnxPath.empty()) return build_from_onnx();
    return build_from_weights();
}

bool Model::build_from_weights(){
    if (fileExists(mEnginePath)){
        LOG("%s has been generated!", mEnginePath.c_str());
        return true;
    } 
        
    LOG("%s not found. Building engine...", mEnginePath.c_str());
    

    mWts = loadWeights();

    Logger logger;
    auto builder       = make_unique<nvinfer1::IBuilder>(nvinfer1::createInferBuilder(logger));
    auto network       = make_unique<nvinfer1::INetworkDefinition>(builder->createNetworkV2(1));
    auto config        = make_unique<nvinfer1::IBuilderConfig>(builder->createBuilderConfig());

    // 根据不同的网络架构创建不同的TensorRT网络，这里使用几个简单的例子
    if (mWtsPath == "models/weights/sample_linear.weights") {
        build_linear(*network, mWts);
    } else if (mWtsPath == "models/weights/sample_conv.weights") {
        build_conv(*network, mWts);
    }  else if (mWtsPath == "models/weights/sample_permute.weights") {
            build_permute(*network, mWts);
    } else if (mWtsPath == "models/weights/sample_reshape.weights") {
    build_reshape(*network, mWts);
    } else if (mWtsPath == "models/weights/sample_batchNorm.weights") {
        build_batchNorm(*network, mWts);
    } else if (mWtsPath == "models/weights/sample_cbr.weights") {
        build_cbr(*network, mWts);
    } else if (mWtsPath == "models/weights/sample_pooling.weights") {
        build_pooling(*network, mWts);
    } else if (mWtsPath == "models/weights/sample_upsample.weights") {
        build_upsample(*network, mWts);
    } else if (mWtsPath == "models/weights/sample_deconv.weights") {
        build_deconv(*network, mWts);
    } else if (mWtsPath == "models/weights/sample_concat.weights") {
        build_concat(*network, mWts);
    } else if (mWtsPath == "models/weights/sample_elementwise.weights") {
        build_elementwise(*network, mWts);
    } else if (mWtsPath == "models/weights/sample_reduce.weights") {
        build_reduce(*network, mWts);
    } else if (mWtsPath == "models/weights/sample_slice.weights") {
        build_slice(*network, mWts);
    } else {
        return false;
    }

    config->setMaxWorkspaceSize(1<<28);
    builder->setMaxBatchSize(1);

    auto engine        = make_unique<nvinfer1::ICudaEngine>(builder->buildEngineWithConfig(*network, *config));
    auto plan          = builder->buildSerializedNetwork(*network, *config);
    auto runtime       = make_unique<nvinfer1::IRuntime>(nvinfer1::createInferRuntime(logger));

    std::ofstream f(mEnginePath, std::ios::binary);
    f.write(reinterpret_cast<const char*>(plan->data()), plan->size());
    f.close();

    mEngine            = shared_ptr<nvinfer1::ICudaEngine>(runtime->deserializeCudaEngine(plan->data(), plan->size()), InferDeleter());
    mInputDims         = network->getInput(0)->getDimensions();
    mOutputDims        = network->getOutput(0)->getDimensions();

    // 把优化前和优化后的各个层的信息打印出来
    LOG("Before TensorRT optimization");
    print_network(*network, false);
    LOG("");
    LOG("After TensorRT optimization");
    print_network(*network, true);

    // 最后把map给delete掉
    for (auto& mem : mWts) {
        delete[] mem.second.values;
    }
    LOG("Finished building engine");
    return true;
}

bool Model::build_from_onnx(){
    if (fileExists(mEnginePath)){
        LOG("%s has been generated!", mEnginePath.c_str());
        return true;
    } else {
        LOG("%s not found. Building engine...", mEnginePath.c_str());
    }
    Logger logger;
    auto builder       = make_unique<nvinfer1::IBuilder>(nvinfer1::createInferBuilder(logger));
    auto network       = make_unique<nvinfer1::INetworkDefinition>(builder->createNetworkV2(1));
    auto config        = make_unique<nvinfer1::IBuilderConfig>(builder->createBuilderConfig());
    auto parser        = make_unique<nvonnxparser::IParser>(nvonnxparser::createParser(*network, logger));

    config->setMaxWorkspaceSize(1<<28);

    if (!parser->parseFromFile(mOnnxPath.c_str(), 1)){
        LOGE("ERROR: failed to %s", mOnnxPath.c_str());
        return false;
    }

    auto engine        = make_unique<nvinfer1::ICudaEngine>(builder->buildEngineWithConfig(*network, *config));
    auto plan          = builder->buildSerializedNetwork(*network, *config);
    auto runtime       = make_unique<nvinfer1::IRuntime>(nvinfer1::createInferRuntime(logger));

    std::ofstream f(mEnginePath, std::ios::binary);
    f.write(reinterpret_cast<const char*>(plan->data()), plan->size());
    f.close();

    mEngine            = shared_ptr<nvinfer1::ICudaEngine>(runtime->deserializeCudaEngine(plan->data(), plan->size()), InferDeleter());
    mInputDims         = network->getInput(0)->getDimensions();
    mOutputDims        = network->getOutput(0)->getDimensions();

    // 把优化前和优化后的各个层的信息打印出来
    LOG("Before TensorRT optimization");
    print_network(*network, false);
    LOG("");
    LOG("After TensorRT optimization");
    print_network(*network, true);

    LOG("Finished building engine");
    return true;
};

bool Model::infer(){
    /*
        在infer需要做的事情
        1. 读取model => 创建runtime, engine, context
        2. 把数据进行host->device传输
        3. 使用context推理
        4. 把数据进行device->host传输
    */

    // 读取model => 创建runtime, engine, context 
    if (!fileExists(mEnginePath)) {
        LOGE("ERROR: %s not found", mEnginePath.c_str());
        return false;
    }

    vector<unsigned char> modelData;
    modelData = loadFile(mEnginePath);
    
    Logger logger;
    auto runtime     = make_unique<nvinfer1::IRuntime>(nvinfer1::createInferRuntime(logger));
    auto engine      = make_unique<nvinfer1::ICudaEngine>(runtime->deserializeCudaEngine(modelData.data(), modelData.size()));
    auto context     = make_unique<nvinfer1::IExecutionContext>(engine->createExecutionContext());

    auto input_dims   = context->getBindingDimensions(0);
    auto output_dims  = context->getBindingDimensions(1);

    LOG("input dim shape is:  %s", printDims(input_dims).c_str());
    LOG("output dim shape is: %s", printDims(output_dims).c_str());

    // 创建流 
    cudaStream_t stream;
    cudaStreamCreate(&stream);

    // 初始化input，以及在host/device上分配空间 
    init_data(input_dims, output_dims);

    // host->device的数据传递
    cudaMemcpyAsync(mInputDevice, mInputHost, mInputSize, cudaMemcpyKind::cudaMemcpyHostToDevice, stream);

    // 模型推理, 最后做同步处理 
    float* bindings[]{mInputDevice, mOutputDevice};
    bool success = context->enqueueV2(reinterpret_cast<void**>(bindings), stream, nullptr);

    // device->host的数据传递 
    cudaMemcpyAsync(mOutputHost, mOutputDevice, mOutputSize, cudaMemcpyKind::cudaMemcpyDeviceToHost, stream);
    cudaStreamSynchronize(stream);

    LOG("input data is:  %s", printTensor(mInputHost, mInputSize / sizeof(float), input_dims).c_str());
    LOG("output data is: %s", printTensor(mOutputHost, mOutputSize / sizeof(float), output_dims).c_str());
    LOG("finished inference");
    return true;
}

void Model::print_network(nvinfer1::INetworkDefinition &network, bool optimized) {

    int inputCount = network.getNbInputs();
    int outputCount = network.getNbOutputs();
    string layer_info;

    for (int i = 0; i < inputCount; i++) {
        auto input = network.getInput(i);
        LOG("Input info: %s:%s", input->getName(), printTensorShape(input).c_str());
    }

    for (int i = 0; i < outputCount; i++) {
        auto output = network.getOutput(i);
        LOG("Output info: %s:%s", output->getName(), printTensorShape(output).c_str());
    }
    
    int layerCount = optimized ? mEngine->getNbLayers() : network.getNbLayers();
    LOG("network has %d layers", layerCount);

    if (!optimized) {
        for (int i = 0; i < layerCount; i++) {
            char layer_info[1000];
            auto layer   = network.getLayer(i);
            auto input   = layer->getInput(0);
            int n = 0;
            if (input == nullptr){
                continue;
            }
            auto output  = layer->getOutput(0);

            LOG("layer_info: %-40s:%-25s->%-25s[%s]", 
                layer->getName(),
                printTensorShape(input).c_str(),
                printTensorShape(output).c_str(),
                getPrecision(layer->getPrecision()).c_str());
        }

    } else {
        auto inspector = make_unique<nvinfer1::IEngineInspector>(mEngine->createEngineInspector());
        for (int i = 0; i < layerCount; i++) {
            string info = inspector->getLayerInformation(i, nvinfer1::LayerInformationFormat::kONELINE);
            if (!info.empty()) {
                info.pop_back();  // 去掉字符串末尾的换行符
            }
            LOG("layer_info: %s", info.c_str());
        }
    }
}

void Model::init_data(nvinfer1::Dims input_dims, nvinfer1::Dims output_dims) {
    mInputSize = getDimSize(input_dims) * sizeof(float);
    mOutputSize = getDimSize(output_dims) * sizeof(float);

    cudaMallocHost(&mInputHost, mInputSize);
    cudaMallocHost(&mOutputHost, mOutputSize);

    if (mInputSize == 5) {
        mInputHost = input_1x5;
    } else {
        mInputHost = input_5x5;
    }

    cudaMalloc(&mInputDevice, mInputSize);
    cudaMalloc(&mOutputDevice, mOutputSize);
}

/*
 * network
 *
 *  -- input --    ITensor
 *  ---- | ----
 *  ---linear--    Ilayer
 *  ---- | ----
 *  -- output -    ITensor
 */
// 最基本的Fully Connected层的创建
void Model::build_linear(nvinfer1::INetworkDefinition& network, map<string, nvinfer1::Weights> mWts) {
    auto data          = network.addInput("input0", nvinfer1::DataType::kFLOAT, nvinfer1::Dims4{1, 1, 1, 5});
    auto fc            = network.addFullyConnected(*data, 1, mWts["linear.weight"], {});
    fc->setName("linear1"); // 每个层都需要设置一个名字

    fc->getOutput(0) ->setName("output0");
    network.markOutput(*fc->getOutput(0));
}

/*
 * network
 *
 *  -- input --    ITensor
 *  ---- | ----
 *  --- conv --    Ilayer
 *  ---- | ----
 *  -- output -    ITensor
 */
// 最基本的convolution层的创建
void Model::build_conv(nvinfer1::INetworkDefinition& network, map<string, nvinfer1::Weights> mWts) {
    auto data          = network.addInput("input0", nvinfer1::DataType::kFLOAT, nvinfer1::Dims4{1, 1, 5, 5});
    // output channel 等于 3
    auto conv          = network.addConvolutionNd(*data, 3, nvinfer1::DimsHW{3, 3}, mWts["conv.weight"], mWts["conv.bias"]);
    conv->setName("conv1");
    conv->setStride(nvinfer1::DimsHW(1, 1));
    conv->getOutput(0) ->setName("output0");
    network.markOutput(*conv->getOutput(0));
}

/*
 * network
 *
 *  -- input --    ITensor
 *  ---- | ----
 *  --- conv --    Ilayer
 *  ---- | ----
 *  --- pool --    IPoolingLayer
 *  ---- | ----
 *  -- output -    ITensor
 */
// 给之前的案例加一个pooling层
void Model::build_pooling(nvinfer1::INetworkDefinition& network, map<string, nvinfer1::Weights> mWts) {
    auto data = network.addInput("input0", nvinfer1::DataType::kFLOAT, nvinfer1::Dims4{1, 1, 5, 5});
    // output channel 等于 3
    auto conv = network.addConvolutionNd(*data, 3, nvinfer1::DimsHW{3, 3}, mWts["conv.weight"], mWts["conv.bias"]);
    conv->setName("conv1");
    conv->setStride(nvinfer1::DimsHW(1, 1));

    auto pool = network.addPoolingNd(*conv->getOutput(0), nvinfer1::PoolingType::kMAX, nvinfer1::DimsHW{2, 2});
    pool->setStride(nvinfer1::DimsHW{2, 2});
    pool->setName("pool1");

    pool->getOutput(0)->setName("output0");
    network.markOutput(*pool->getOutput(0));
}

/*
 * network
 *
 *  -- input --    ITensor
 *  ---- | ----
 *  --- conv --    Ilayer
 *  ---- | ----
 *  - Upsample -   IResizeLayer
 *  ---- | ----
 *  -- output -    ITensor
 */
// 上采样层的创建
void Model::build_upsample(nvinfer1::INetworkDefinition& network, map<string, nvinfer1::Weights> mWts) {
    auto data = network.addInput("input0", nvinfer1::DataType::kFLOAT, nvinfer1::Dims4{1, 1, 5, 5});
    // output channel 等于 3
    auto conv = network.addConvolutionNd(*data, 3, nvinfer1::DimsHW{3, 3}, mWts["conv.weight"], mWts["conv.bias"]);
    conv->setName("conv1");
    conv->setStride(nvinfer1::DimsHW(1, 1));

    auto upsample = network.addResize(*conv->getOutput(0));
    // upsample->setAlignCorners(true);
    upsample->setOutputDimensions(nvinfer1::Dims4{1, 3, 6, 6});
    upsample->setResizeMode(nvinfer1::ResizeMode::kNEAREST);
    upsample->setName("upsample");

    upsample->getOutput(0)->setName("output0");
    network.markOutput(*upsample->getOutput(0));
}

/*
 * network
 *
 *  -- input --    ITensor
 *  ---- | ----
 *  --- conv --    Ilayer
 *  ---- | ----
 *  -- deconv--   IDeconvolutionLayer
 *  ---- | ----
 *  -- output -    ITensor
 */
// 反卷积层的创建
void Model::build_deconv(nvinfer1::INetworkDefinition& network, map<string, nvinfer1::Weights> mWts) {
    auto data = network.addInput("input0", nvinfer1::DataType::kFLOAT, nvinfer1::Dims4{1, 1, 5, 5});
    // output channel 等于 3
    auto conv = network.addConvolutionNd(*data, 3, nvinfer1::DimsHW{3, 3}, mWts["conv.weight"], mWts["conv.bias"]);
    conv->setName("conv1");
    conv->setStrideNd(nvinfer1::DimsHW(1, 1));

    auto deconv1 = network.addDeconvolutionNd(*conv->getOutput(0), conv->getNbOutputMaps(), nvinfer1::DimsHW{3, 3}, mWts["deconv.weight"], mWts["deconv.bias"]);
    deconv1->setStrideNd(nvinfer1::DimsHW{1, 1});
    deconv1->setNbOutputMaps(1);
    deconv1->setName("deconv");
    
    deconv1->getOutput(0)->setName("output0");
    network.markOutput(*deconv1->getOutput(0));
}

//      input
//       /   \
//    conv1  conv2
//     |      |
//      \    /
//       \  /
//      concat  IConcatenationLayer
//        |
//      output
// concat层的创建
void Model::build_concat(nvinfer1::INetworkDefinition& network, map<string, nvinfer1::Weights> mWts) {
    auto data = network.addInput("input0", nvinfer1::DataType::kFLOAT, nvinfer1::Dims4{1, 1, 5, 5});
    auto conv1 = network.addConvolutionNd(*data, 3, nvinfer1::DimsHW{3, 3}, mWts["conv1.weight"], mWts["conv1.bias"]);
    conv1->setName("conv1");
    conv1->setStrideNd(nvinfer1::DimsHW(1, 1));

    auto conv2 = network.addConvolutionNd(*data, 3, nvinfer1::DimsHW{3, 3}, mWts["conv2.weight"], mWts["conv2.bias"]);
    conv2->setName("conv2");
    conv2->setStrideNd(nvinfer1::DimsHW(1, 1));

    nvinfer1::ITensor* Tensors[]{conv1->getOutput(0), conv2->getOutput(0)};
    auto cat = network.addConcatenation(Tensors, 2);
    cat->setName("concat");

    cat->getOutput(0)->setName("output0");
    network.markOutput(*cat->getOutput(0));
}


// elementwise和const的创建
void Model::build_elementwise(nvinfer1::INetworkDefinition& network, map<string, nvinfer1::Weights> mWts) {
    auto data = network.addInput("input0", nvinfer1::DataType::kFLOAT, nvinfer1::Dims4{1, 1, 5, 5});
    // output channel 等于 3
    auto conv = network.addConvolutionNd(*data, 3, nvinfer1::DimsHW{3, 3}, mWts["conv.weight"], mWts["conv.bias"]);
    conv->setName("conv1");
    conv->setStrideNd(nvinfer1::DimsHW(1, 1));

    nvinfer1::Weights Div_225{nvinfer1::DataType::kFLOAT, nullptr, 3};
    float* wgt = reinterpret_cast<float*>(malloc(sizeof(float) * 3));
    for (int i = 0; i < 3; ++i) {
        wgt[i] = 255.0f;
    }
    Div_225.values = wgt;

    auto con = network.addConstant(nvinfer1::Dims4{1, 3, 1, 1}, Div_225);
    // 通道维度上做除法
    auto elem = network.addElementWise(*conv->getOutput(0), *con->getOutput(0), nvinfer1::ElementWiseOperation::kDIV);

    elem->setName("elem");

    elem->getOutput(0)->setName("output0");
    network.markOutput(*elem->getOutput(0));
}

/*
 * network
 *
 *  -- input --    ITensor
 *  ---- | ----
 *  ---linear--    Ilayer
 *  ---- | ----
 *  -- reduce --   IReduceLayer
 *  ---- | ----
 *  - softmax -    ISoftMaxLayer
 *  ---- | ----
 *  -- output -    ITensor
 */
// reduce和softmax层的创建
void Model::build_reduce(nvinfer1::INetworkDefinition& network, map<string, nvinfer1::Weights> mWts){
    auto data = network.addInput("input0", nvinfer1::DataType::kFLOAT, nvinfer1::Dims4{1, 1, 1, 5});
    auto fc = network.addFullyConnected(*data, 1, mWts["linear.weight"], {});
    fc->setName("linear1");

    // reduce常见的操作包括求和（sum）、求平均（average）、求最大值（max）等
    // 可以指定是否在输出中保留减少的维度
    auto reduce = network.addReduce(*fc->getOutput(0), nvinfer1::ReduceOperation::kAVG, 1, false);

    auto softmax = network.addSoftMax(*reduce->getOutput(0));
    //! Bit 0 corresponds to the N dimension boolean.
    //! Bit 1 corresponds to the C dimension boolean.
    //! Bit 2 corresponds to the H dimension boolean.
    //! Bit 3 corresponds to the W dimension boolean.
    softmax->setAxes(1);

    softmax->getOutput(0)->setName("output0");
    network.markOutput(*softmax->getOutput(0));
}

// slice层的创建
void Model::build_slice(nvinfer1::INetworkDefinition& network, map<string, nvinfer1::Weights> mWts) {
    auto data = network.addInput("input0", nvinfer1::DataType::kFLOAT, nvinfer1::Dims4{1, 1, 5, 5});
    // output channel 等于 3
    auto conv = network.addConvolutionNd(*data, 4, nvinfer1::DimsHW{3, 3}, mWts["conv.weight"], mWts["conv.bias"]);
    conv->setName("conv1");
    conv->setStrideNd(nvinfer1::DimsHW(1, 1));

    // 获取卷积层输出的维度
    auto d = conv->getOutput(0)->getDimensions();

    // 添加第一个切片层，提取前两个通道
    auto split1 = network.addSlice(*conv->getOutput(0), nvinfer1::Dims4{0, 0, 0, 0}, nvinfer1::Dims4{1, d.d[1] / 2, d.d[2], d.d[3]}, nvinfer1::Dims4{1, 1, 1, 1});
    split1->setName("slice1");

    // 添加第二个切片层，提取后两个通道
    // auto split2 = network.addSlice(*conv->getOutput(0), nvinfer1::Dims4{0, 2, 0, 0}, nvinfer1::Dims4{1, d.d[1] / 2, d.d[2], d.d[3]}, nvinfer1::Dims4{1, 1, 1, 1});
    // split2->setName("slice2");

    // 将切片层的输出标记为网络输出
    split1->getOutput(0)->setName("output1");
    // split2->getOutput(0)->setName("output2");
    network.markOutput(*split1->getOutput(0));
    // network.markOutput(*split2->getOutput(0));
}


/*
 * network
 *
 *  -- input --    ITensor
 *  ---- | ----
 *  --- conv --    ILayer (IConvolutionLayer)
 *  ---- | ----
 *  - permute -    ILayer (IShuffleLayer)
 *  ---- | ----
 *  -- output -    ITensor
 */
// shuffle层的创建
void Model::build_permute(nvinfer1::INetworkDefinition& network, map<string, nvinfer1::Weights> mWts) {
    auto data          = network.addInput("input0", nvinfer1::DataType::kFLOAT, nvinfer1::Dims4{1, 1, 5, 5});
    auto conv          = network.addConvolutionNd(*data, 3, nvinfer1::DimsHW{3, 3}, mWts["conv.weight"], mWts["conv.bias"]);
    conv->setName("conv1");
    conv->setStride(nvinfer1::DimsHW(1, 1));
    
    auto permute       = network.addShuffle(*conv->getOutput(0));
    // setFirstTranspose代表nvinfer1::IShuffleLayer层 在进行reshape操作之前的transpose操作
    // setSecondTranspose 表示的是进行reshape操作之后的transpose操作
    permute->setFirstTranspose(nvinfer1::Permutation{0, 2, 3, 1}); // B, C, H, W -> B, H, W, C
    permute->setName("permute1");

    permute->getOutput(0)->setName("output0");
    network.markOutput(*permute->getOutput(0));
}

/*
 * network 
 *
 *  -- input --    ITensor
 *  ---- | ----
 *  --- conv --    ILayer (IConvolutionLayer)
 *  ---- | ----
 *  -- view ---    ILayer (IShuffleLayer) 
 *  ---- | ----
 *  - permute -    ILayer (IShuffleLayer) // 0️⃣同一层干pytorch两个op的事
 *  ---- | ----
 *  -- output -    ITensor
*/

// shuffle层中处理多次维度上的操作
void Model::build_reshape(nvinfer1::INetworkDefinition& network, map<string, nvinfer1::Weights> mWts) {
    auto data          = network.addInput("input0", nvinfer1::DataType::kFLOAT, nvinfer1::Dims4{1, 1, 5, 5});

    auto conv          = network.addConvolutionNd(*data, 3, nvinfer1::DimsHW{3, 3}, mWts["conv.weight"], mWts["conv.bias"]);
    conv->setName("conv1");
    conv->setStride(nvinfer1::DimsHW(1, 1));

    auto reshape       = network.addShuffle(*conv->getOutput(0));
    // 指定新形状：定义目标形状，它可以使用明确的数值，也可以使用特殊值，如 0 或 -1
    // 重新排列元素：根据指定的新形状重新排列元素，但不改变元素的顺序
    // 0 值：表示该维度的大小保持与输入张量的对应维度相同
    // -1 值：表示该维度的大小由数据总数量和其他维度的大小推断出，只能出现一次
    reshape->setReshapeDimensions(nvinfer1::Dims3{1, 3, -1});
    reshape->setSecondTranspose(nvinfer1::Permutation{0, 2, 1});      
    reshape->setName("reshape + permute1");
    // 注意，因为reshape和transpose都是属于iShuffleLayer做的事情，所以需要指明是reshape在前，还是transpose在前
    // 通过这里我们可以看到，在TRT中连续的对tensor的维度的操作其实是可以在TRT中用一个层来处理，属于一种layer fusion优化

    reshape->getOutput(0)->setName("output0");
    network.markOutput(*reshape->getOutput(0));
}


/*
 * network 
 *
 *  -- input --    ITensor
 *  ---- | ----
 *  --- conv --    ILayer (IConvolutionLayer)
 *  ---- | ----
 *  --  BN   --    ILayer (IScaleLayer)
 *  ---- | ----
 *  -- output -    ITensor
*/
// 自定义的IScaleLayer来实现BatchNorm的创建
void Model::build_batchNorm(nvinfer1::INetworkDefinition& network, map<string, nvinfer1::Weights> mWts) {
    auto data          = network.addInput("input0", nvinfer1::DataType::kFLOAT, nvinfer1::Dims4{1, 1, 5, 5});
    auto conv          = network.addConvolutionNd(*data, 3, nvinfer1::DimsHW{3, 3}, mWts["conv.weight"], mWts["conv.bias"]);
    conv->setName("conv1");
    conv->setStride(nvinfer1::DimsHW(1, 1));

    // 因为TensorRT内部没有BatchNorm的实现，但是我们只要知道BatchNorm的计算原理，就可以使用IScaleLayer来创建BN的计算
    // IScaleLayer主要是用在quantization和dequantization，这里用它来搭建于一个BN的parser
    // IScaleLayer可以实现: y = (x * scale + shift) ^ pow
    
    float* gamma   = const_cast<float*>(reinterpret_cast<const float*>(mWts["norm.weight"].values));
    float* beta    = const_cast<float*>(reinterpret_cast<const float*>(mWts["norm.bias"].values));
    float* mean    = const_cast<float*>(reinterpret_cast<const float*>(mWts["norm.running_mean"].values));
    float* var     = const_cast<float*>(reinterpret_cast<const float*>(mWts["norm.running_var"].values));
    float  eps     = 1e-5;
    
    int    count   = mWts["norm.running_var"].count;

    float* scales  = const_cast<float*>(reinterpret_cast<const float*>(malloc(count * sizeof(float))));
    float* shifts  = const_cast<float*>(reinterpret_cast<const float*>(malloc(count * sizeof(float))));
    float* pows    = const_cast<float*>(reinterpret_cast<const float*>(malloc(count * sizeof(float))));
    
    // 这里具体参考一下batch normalization的计算公式，网上有很多
    for (int i = 0; i < count; i ++) {
        scales[i] = gamma[i] / sqrt(var[i] + eps);
        shifts[i] = beta[i] - (mean[i] * gamma[i] / sqrt(var[i] + eps));
        pows[i]   = 1.0;
    }

    // 将计算得到的这些值写入到Weight中
    auto scales_weights = nvinfer1::Weights{nvinfer1::DataType::kFLOAT, scales, count};
    auto shifts_weights = nvinfer1::Weights{nvinfer1::DataType::kFLOAT, shifts, count};
    auto pows_weights   = nvinfer1::Weights{nvinfer1::DataType::kFLOAT, pows, count};

    // 创建IScaleLayer并将这些weights传进去，这里使用channel作为scale model
    auto scale = network.addScale(*conv->getOutput(0), nvinfer1::ScaleMode::kCHANNEL, shifts_weights, scales_weights, pows_weights);
    scale->setName("batchNorm1");

    scale->getOutput(0) ->setName("output0");
    network.markOutput(*scale->getOutput(0));
}

/*
 * network
 *
 *  -- input --    ITensor
 *  ---- | ----
 *  --- conv --    ILayer (IConvolutionLayer)
 *  ---- | ----
 *  --  BN   --    ILayer (IScaleLayer)
 *  ---- | ----
 *  -LeakyReLU-    ILayer (IActivationLayer)
 *  ---- | ----
 *  -- output -    ITensor
 */
// 做一个conv + batchNorm + LeakyReLU的网络
void Model::build_cbr(nvinfer1::INetworkDefinition& network, map<string, nvinfer1::Weights> mWts) {
    auto data          = network.addInput("input0", nvinfer1::DataType::kFLOAT, nvinfer1::Dims4{1, 1, 5, 5});
    auto conv          = network.addConvolutionNd(*data, 3, nvinfer1::DimsHW{3, 3}, mWts["conv.weight"], mWts["conv.bias"]);
    conv->setName("conv1");
    conv->setStride(nvinfer1::DimsHW(1, 1));

    // 因为TensorRT内部没有BatchNorm的实现，但是我们只要知道BatchNorm的计算原理，就可以使用IScaleLayer来创建BN的计算
    // IScaleLayer主要是用在quantization和dequantization，这里用它来搭建于一个BN的parser
    // IScaleLayer可以实现: y = (x * scale + shift) ^ pow

    float* gamma   = const_cast<float*>(reinterpret_cast<const float*>(mWts["norm.weight"].values));
    float* beta    = const_cast<float*>(reinterpret_cast<const float*>(mWts["norm.bias"].values));
    float* mean    = const_cast<float*>(reinterpret_cast<const float*>(mWts["norm.running_mean"].values));
    float* var     = const_cast<float*>(reinterpret_cast<const float*>(mWts["norm.running_var"].values));
    float  eps     = 1e-5;
    
    int    count   = mWts["norm.running_var"].count;

    float* scales  = const_cast<float*>(reinterpret_cast<const float*>(malloc(count * sizeof(float))));
    float* shifts  = const_cast<float*>(reinterpret_cast<const float*>(malloc(count * sizeof(float))));
    float* pows    = const_cast<float*>(reinterpret_cast<const float*>(malloc(count * sizeof(float))));
    
    // 这里具体参考一下batch normalization的计算公式，网上有很多
    for (int i = 0; i < count; i ++) {
        scales[i] = gamma[i] / sqrt(var[i] + eps);
        shifts[i] = beta[i] - (mean[i] * gamma[i] / sqrt(var[i] + eps));
        pows[i]   = 1.0;
    }

    // 将计算得到的这些值写入到Weight中
    auto scales_weights = nvinfer1::Weights{nvinfer1::DataType::kFLOAT, scales, count};
    auto shifts_weights = nvinfer1::Weights{nvinfer1::DataType::kFLOAT, shifts, count};
    auto pows_weights   = nvinfer1::Weights{nvinfer1::DataType::kFLOAT, pows, count};

    // 创建IScaleLayer并将这些weights传进去，这里使用channel作为scale model
    auto bn = network.addScale(*conv->getOutput(0), nvinfer1::ScaleMode::kCHANNEL, shifts_weights, scales_weights, pows_weights);
    bn->setName("batchNorm1");

    auto leaky = network.addActivation(*bn->getOutput(0), nvinfer1::ActivationType::kLEAKY_RELU);
    leaky->setName("leaky1");

    leaky->getOutput(0) ->setName("output0");
    network.markOutput(*leaky->getOutput(0));
}
