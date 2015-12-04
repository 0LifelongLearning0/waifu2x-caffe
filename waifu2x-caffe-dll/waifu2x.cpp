#include "waifu2x.h"
#include <caffe/caffe.hpp>
#include <cudnn.h>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <chrono>
#include <cuda_runtime.h>


#ifdef _DEBUG
#pragma comment(lib, "caffe-d.lib")
#pragma comment(lib, "proto-d.lib")
#pragma comment(lib, "libboost_system-vc120-mt-gd-1_59.lib")
#pragma comment(lib, "libboost_thread-vc120-mt-gd-1_59.lib")
#pragma comment(lib, "libboost_filesystem-vc120-mt-gd-1_59.lib")
#pragma comment(lib, "glogd.lib")
#pragma comment(lib, "gflagsd.lib")
#pragma comment(lib, "libprotobufd.lib")
#pragma comment(lib, "libhdf5_hl_D.lib")
#pragma comment(lib, "libhdf5_D.lib")
#pragma comment(lib, "zlibstaticd.lib")
#pragma comment(lib, "libopenblas.dll.a")
#pragma comment(lib, "cudart.lib")
#pragma comment(lib, "curand.lib")
#pragma comment(lib, "cublas.lib")
#pragma comment(lib, "cudnn.lib")

#pragma comment(lib, "opencv_calib3d249d.lib")
#pragma comment(lib, "opencv_contrib249d.lib")
#pragma comment(lib, "opencv_core249d.lib")
#pragma comment(lib, "opencv_highgui249d.lib")
#pragma comment(lib, "opencv_imgproc249d.lib")
#else
#pragma comment(lib, "caffe.lib")
#pragma comment(lib, "proto.lib")
#pragma comment(lib, "libboost_system-vc120-mt-1_59.lib")
#pragma comment(lib, "libboost_thread-vc120-mt-1_59.lib")
#pragma comment(lib, "libboost_filesystem-vc120-mt-1_59.lib")
#pragma comment(lib, "glog.lib")
#pragma comment(lib, "gflags.lib")
#pragma comment(lib, "libprotobuf.lib")
#pragma comment(lib, "libhdf5_hl.lib")
#pragma comment(lib, "libhdf5.lib")
#pragma comment(lib, "zlibstatic.lib")
#pragma comment(lib, "libopenblas.dll.a")
#pragma comment(lib, "cudart.lib")
#pragma comment(lib, "curand.lib")
#pragma comment(lib, "cublas.lib")
#pragma comment(lib, "cudnn.lib")

#pragma comment(lib, "opencv_calib3d249.lib")
#pragma comment(lib, "opencv_contrib249.lib")
#pragma comment(lib, "opencv_core249.lib")
#pragma comment(lib, "opencv_highgui249.lib")
#pragma comment(lib, "opencv_imgproc249.lib")
#endif

// ���͉摜�̃I�t�Z�b�g
const int offset = 0;
// srcnn.prototxt�Œ�`���ꂽ���C���[�̐�
const int layer_num = 7;

const int ConvertMode = CV_RGB2YUV;
const int ConvertInverseMode = CV_YUV2RGB;

// �Œ���K�v��CUDA�h���C�o�[�̃o�[�W����
const int MinCudaDriverVersion = 6050;

static std::once_flag waifu2x_once_flag;
static std::once_flag waifu2x_cudnn_once_flag;
static std::once_flag waifu2x_cuda_once_flag;

#ifndef CUDA_CHECK_WAIFU2X
#define CUDA_CHECK_WAIFU2X(condition) \
 do { \
    cudaError_t error = condition; \
    if(error != cudaSuccess) throw error; \
 } while (0)
#endif

#define CUDA_HOST_SAFE_FREE(ptr) \
	do { \
		if (ptr) { \
			cudaFreeHost(ptr); \
			ptr = nullptr; \
		} \
	} while (0)

#define SAFE_DELETE_WAIFU2X(ptr) \
	do { \
		if (ptr) { \
			delete [] ptr; \
			ptr = nullptr; \
		} \
	} while (0)

namespace
{
	class IgnoreErrorCV
	{
	private:
		static int handleError(int status, const char* func_name,
			const char* err_msg, const char* file_name,
			int line, void* userdata)
		{
			return 0;
		}

	public:
		IgnoreErrorCV()
		{
			cv::redirectError(handleError);
		}
	};

	IgnoreErrorCV g_IgnoreErrorCV;
}

Waifu2x::Waifu2x() : is_inited(false), isCuda(false), input_block(nullptr), dummy_data(nullptr), output_block(nullptr)
{
}

Waifu2x::~Waifu2x()
{
	destroy();
}

// �摜��ǂݍ���Œl��0.0f�`1.0f�͈̔͂ɕϊ�
Waifu2x::eWaifu2xError Waifu2x::LoadMat(cv::Mat &float_image, const uint32_t* source, int width, int height)
{
	float_image = cv::Mat(cv::Size(width, height), CV_MAKETYPE(CV_8U, 4));

	const auto LinePixel = float_image.step1() / float_image.channels();
	const auto Channel = float_image.channels();
	const auto Width = float_image.size().width;
	const auto Height = float_image.size().height;

	const uint8_t *sptr = (const uint8_t *)source;
	auto ptr = float_image.data;
	for (int i = 0; i < height; i++)
	{
		for (int j = 0; j < width; j++)
		{
			for (int ch = 0; ch < Channel; ch++)
				ptr[(i * LinePixel + j) * 4 + ch] = sptr[(i * width + j) * 4 + ch];
		}
	}

	// RGB������BGR�ɕϊ�
	/*
	for (int i = 0; i < height; i++)
	{
		for (int j = 0; j < width; j++)
			std::swap(ptr[(i * LinePixel + j) * 4 + 0], ptr[(i * LinePixel + j) * 4 + 2]);
	}
	*/

	cv::Mat convert;
	float_image.convertTo(convert, CV_32F, 1.0 / 255.0);
	float_image.release();

	{
		// �A���t�@�`�����l���t���������烿��Z�ς݂ɂ���

		std::vector<cv::Mat> planes;
		cv::split(convert, planes);

		cv::Mat w = planes[3];

		planes[0] = planes[0].mul(w);
		planes[1] = planes[1].mul(w);
		planes[2] = planes[2].mul(w);

		cv::merge(planes, convert);
	}

	float_image = convert;

	return eWaifu2xError_OK;
}

// ���͉摜��(Photoshop�ł���)�L�����o�X�T�C�Y��output_size�̔{���ɕύX
// �摜�͍���z�u�A�]����cv::BORDER_REPLICATE�Ŗ��߂�
Waifu2x::eWaifu2xError Waifu2x::PaddingImage(const cv::Mat &input, cv::Mat &output)
{
	const auto h_blocks = (int)floor(input.size().width / output_size) + (input.size().width % output_size == 0 ? 0 : 1);
	const auto w_blocks = (int)floor(input.size().height / output_size) + (input.size().height % output_size == 0 ? 0 : 1);
	const auto height = offset + h_blocks * output_size + offset;
	const auto width = offset + w_blocks * output_size + offset;
	const auto pad_h1 = offset;
	const auto pad_w1 = offset;
	const auto pad_h2 = (height - offset) - input.size().width;
	const auto pad_w2 = (width - offset) - input.size().height;

	cv::copyMakeBorder(input, output, pad_w1, pad_w2, pad_h1, pad_h2, cv::BORDER_REPLICATE);

	return eWaifu2xError_OK;
}

// �摜��cv::INTER_NEAREST�œ�{�Ɋg�債�āAPaddingImage()�Ńp�f�B���O����
Waifu2x::eWaifu2xError Waifu2x::Zoom2xAndPaddingImage(const cv::Mat &input, cv::Mat &output, cv::Size_<int> &zoom_size)
{
	zoom_size = input.size();
	zoom_size.width *= 2;
	zoom_size.height *= 2;

	cv::Mat zoom_image;
	cv::resize(input, zoom_image, zoom_size, 0.0, 0.0, cv::INTER_NEAREST);

	return PaddingImage(zoom_image, output);
}

// ���͉摜��zoom_size�̑傫����cv::INTER_CUBIC�Ŋg�債�A�F���݂̂��c��
Waifu2x::eWaifu2xError Waifu2x::CreateZoomColorImage(const cv::Mat &float_image, const cv::Size_<int> &zoom_size, std::vector<cv::Mat> &cubic_planes)
{
	cv::Mat zoom_cubic_image;
	cv::resize(float_image, zoom_cubic_image, zoom_size, 0.0, 0.0, cv::INTER_CUBIC);

	cv::Mat converted_cubic_image;
	cv::cvtColor(zoom_cubic_image, converted_cubic_image, ConvertMode);
	zoom_cubic_image.release();

	cv::split(converted_cubic_image, cubic_planes);
	converted_cubic_image.release();

	// ����Y�����͎g��Ȃ��̂ŉ��
	cubic_planes[0].release();

	return eWaifu2xError_OK;
}

// ���f���t�@�C������l�b�g���[�N���\�z
// process��cudnn���w�肳��Ȃ������ꍇ��cuDNN���Ăяo����Ȃ��悤�ɕύX����
Waifu2x::eWaifu2xError Waifu2x::ConstractNet(boost::shared_ptr<caffe::Net<float>> &net, const std::string &model_path, const std::string &param_path, const std::string &process)
{
	const std::string caffemodel_path = param_path + ".caffemodel";
	const std::string modelbin_path = model_path + ".protobin";

	FILE *fp = fopen(caffemodel_path.c_str(), "rb");
	const bool isModelExist = fp != nullptr;
	if (fp) fclose(fp);

	fp = fopen(modelbin_path.c_str(), "rb");
	const bool isModelBinExist = fp != nullptr;
	if (fp) fclose(fp);

	caffe::NetParameter param;
	if (isModelExist && isModelBinExist && caffe::ReadProtoFromBinaryFile(modelbin_path, &param))
	{
		const auto ret = SetParameter(param);
		if (ret != eWaifu2xError_OK)
			return ret;

		net = boost::shared_ptr<caffe::Net<float>>(new caffe::Net<float>(param));
		net->CopyTrainedLayersFrom(caffemodel_path);

		input_plane = param.input_dim(1);
	}
	else
		return eWaifu2xError_FailedConstructModel;

	return eWaifu2xError_OK;
}

Waifu2x::eWaifu2xError Waifu2x::SetParameter(caffe::NetParameter &param) const
{
	param.mutable_state()->set_phase(caffe::TEST);

	{
		auto mid = param.mutable_input_dim();

		if (mid->size() != 4)
			return eWaifu2xError_FailedParseModelFile;

		*mid->Mutable(0) = batch_size;
		*mid->Mutable(2) = input_block_size;
		*mid->Mutable(3) = input_block_size;
	}

	for (int i = 0; i < param.layer_size(); i++)
	{
		caffe::LayerParameter *layer_param = param.mutable_layer(i);
		const std::string& type = layer_param->type();
		if (type == "Convolution")
		{
			if (process == "cudnn")
				layer_param->mutable_convolution_param()->set_engine(caffe::ConvolutionParameter_Engine_CUDNN);
			else
				layer_param->mutable_convolution_param()->set_engine(caffe::ConvolutionParameter_Engine_CAFFE);
		}
		else if (type == "ReLU")
		{
			if (process == "cudnn")
				layer_param->mutable_relu_param()->set_engine(caffe::ReLUParameter_Engine_CUDNN);
			else
				layer_param->mutable_relu_param()->set_engine(caffe::ReLUParameter_Engine_CAFFE);
		}
	}

	return eWaifu2xError_OK;
}

// �l�b�g���[�N���g���ĉ摜���č\�z����
Waifu2x::eWaifu2xError Waifu2x::ReconstructImage(boost::shared_ptr<caffe::Net<float>> net, cv::Mat &im)
{
	const auto Height = im.size().height;
	const auto Width = im.size().width;
	const auto Line = im.step1();

	assert(Width % output_size == 0);
	assert(Height % output_size == 0);

	assert(im.channels() == 1 || im.channels() == 3);

	cv::Mat outim(im.rows, im.cols, im.type());

	// float *imptr = (float *)im.data;
	float *imptr = (float *)outim.data;

	try
	{
		auto input_blobs = net->input_blobs();
		auto input_blob = net->input_blobs()[0];

		input_blob->Reshape(batch_size, input_plane, input_block_size, input_block_size);

		assert(im.channels() == input_plane);
		assert(input_blob->shape(1) == input_plane);

		const int WidthNum = Width / output_size;
		const int HeightNum = Height / output_size;

		const int BlockNum = WidthNum * HeightNum;

		const int input_block_plane_size = input_block_size * input_block_size * input_plane;
		const int output_block_plane_size = output_block_size * output_block_size * input_plane;

		const int output_padding = inner_padding + outer_padding - layer_num;

		// �摜��(��������̓s����)output_size*output_size�ɕ����čč\�z����
		for (int num = 0; num < BlockNum; num += batch_size)
		{
			const int processNum = (BlockNum - num) >= batch_size ? batch_size : BlockNum - num;

			if (processNum < batch_size)
				input_blob->Reshape(processNum, input_plane, input_block_size, input_block_size);

			for (int n = 0; n < processNum; n++)
			{
				const int wn = (num + n) % WidthNum;
				const int hn = (num + n) / WidthNum;

				const int w = wn * output_size;
				const int h = hn * output_size;

				if (w + crop_size <= Width && h + crop_size <= Height)
				{
					int x, y;
					x = w - inner_padding;
					y = h - inner_padding;

					int width, height;

					width = crop_size + inner_padding * 2;
					height = crop_size + inner_padding * 2;

					int top, bottom, left, right;

					top = outer_padding;
					bottom = outer_padding;
					left = outer_padding;
					right = outer_padding;

					if (x < 0)
					{
						left += -x;
						width -= -x;
						x = 0;
					}

					if (x + width > Width)
					{
						right += (x + width) - Width;
						width = Width - x;
					}

					if (y < 0)
					{
						top += -y;
						height -= -y;
						y = 0;
					}

					if (y + height > Height)
					{
						bottom += (y + height) - Height;
						height = Height - y;
					}

					cv::Mat someimg = im(cv::Rect(x, y, width, height));

					cv::Mat someborderimg;
					// �摜�𒆉��Ƀp�f�B���O�B�]����cv::BORDER_REPLICATE�Ŗ��߂�
					// ����im�ŉ�f�����݂��镔���͗]���ƔF������Ȃ����Ainner_padding��layer_num��outer_padding��1�ȏ�Ȃ炻���̕����̉�f�͌��ʉ摜�Ƃ��Ď��o�������ɂ͉e�����Ȃ�
					cv::copyMakeBorder(someimg, someborderimg, top, bottom, left, right, cv::BORDER_REPLICATE);
					someimg.release();

					// �摜�𒼗�ɕϊ�
					{
						float *fptr = input_block + (input_block_plane_size * n);
						const float *uptr = (const float *)someborderimg.data;

						const auto Line = someborderimg.step1();

						if (someborderimg.channels() == 1)
						{
							if (input_block_size == Line)
								memcpy(fptr, uptr, input_block_size * input_block_size * sizeof(float));
							else
							{
								for (int i = 0; i < input_block_size; i++)
									memcpy(fptr + i * input_block_size, uptr + i * Line, input_block_size * sizeof(float));
							}
						}
						else
						{
							const auto LinePixel = someborderimg.step1() / someborderimg.channels();
							const auto Channel = someborderimg.channels();
							const auto Width = someborderimg.size().width;
							const auto Height = someborderimg.size().height;

							for (int i = 0; i < Height; i++)
							{
								for (int j = 0; j < LinePixel; j++)
								{
									for (int ch = 0; ch < Channel; ch++)
										fptr[(ch * Height + i) * Width + j] = uptr[(i * LinePixel + j) * Channel + ch];
								}
							}

							/*
							{
								cv::Mat im(someborderimg.size(), CV_32F, fptr, Width * sizeof(float));

								cv::Mat write_iamge;
								im.convertTo(write_iamge, CV_8U, 255.0);
								im.release();

								if (!cv::imwrite("test_in.png", write_iamge))
									return eWaifu2xError_FailedOpenOutputFile;
							}
							*/
						}
					}
				}
			}

			assert(input_blob->count() == input_block_plane_size * processNum);

			// �l�b�g���[�N�ɉ摜�����
			input_blob->set_cpu_data(input_block);

			// �v�Z
			auto out = net->ForwardPrefilled(nullptr);

			auto b = out[0];

			assert(b->count() == output_block_plane_size * processNum);

			const float *ptr = nullptr;

			if (caffe::Caffe::mode() == caffe::Caffe::CPU)
				ptr = b->cpu_data();
			else
				ptr = b->gpu_data();

			caffe::caffe_copy(output_block_plane_size * processNum, ptr, output_block);

			for (int n = 0; n < processNum; n++)
			{
				const int wn = (num + n) % WidthNum;
				const int hn = (num + n) / WidthNum;

				const int w = wn * output_size;
				const int h = hn * output_size;

				const float *fptr = output_block + (output_block_plane_size * n);

				// ���ʂ��o�͉摜�ɃR�s�[
				if (outim.channels() == 1)
				{
					for (int i = 0; i < crop_size; i++)
						memcpy(imptr + (h + i) * Line + w, fptr + (i + output_padding) * output_block_size + output_padding, crop_size * sizeof(float));
				}
				else
				{
					const auto LinePixel = outim.step1() / outim.channels();
					const auto Channel = outim.channels();

					for (int i = 0; i < crop_size; i++)
					{
						for (int j = 0; j < crop_size; j++)
						{
							for (int ch = 0; ch < Channel; ch++)
								imptr[((h + i) * LinePixel + (w + j)) * Channel + ch] = fptr[(ch * output_block_size + i + output_padding) * output_block_size + j + output_padding];
						}
					}

					/*
					{
						cv::Mat im(someborderimg.size(), CV_32F, fptr, Width * sizeof(float));

						cv::Mat write_iamge;
						im.convertTo(write_iamge, CV_8U, 255.0);
						im.release();

						if (!cv::imwrite("test_in.png", write_iamge))
							return eWaifu2xError_FailedOpenOutputFile;
					}
					*/
				}
			}
		}
	}
	catch (...)
	{
		return eWaifu2xError_FailedProcessCaffe;
	}

	im = outim;

	return eWaifu2xError_OK;
}

Waifu2x::eWaifu2xError Waifu2x::init(int argc, char** argv, const std::string &Mode, const int NoiseLevel, const std::string &ModelDir, const std::string &Process,
	const int CropSize, const int BatchSize)
{
	Waifu2x::eWaifu2xError ret;

	if (is_inited)
		return eWaifu2xError_OK;

	try
	{
		mode = Mode;
		noise_level = NoiseLevel;
		model_dir = ModelDir;
		process = Process;

		crop_size = CropSize;
		batch_size = BatchSize;

		inner_padding = layer_num;
		outer_padding = 1;

		output_size = crop_size - offset * 2;
		input_block_size = crop_size + (inner_padding + outer_padding) * 2;
		original_width_height = 128 + layer_num * 2;

		output_block_size = crop_size + (inner_padding + outer_padding - layer_num) * 2;

		std::call_once(waifu2x_once_flag, [argc, argv]()
		{
			assert(argc >= 1);

			int tmpargc = 1;
			char* tmpargvv[] = { argv[0] };
			char** tmpargv = tmpargvv;
			// glog���̏�����
			caffe::GlobalInit(&tmpargc, &tmpargv);
		});

		const auto cuDNNCheckStartTime = std::chrono::system_clock::now();

		if (process == "gpu")
			process = "cudnn";

		const auto cuDNNCheckEndTime = std::chrono::system_clock::now();

		boost::filesystem::path mode_dir_path(model_dir);
		if (!mode_dir_path.is_absolute()) // model_dir�����΃p�X�Ȃ��΃p�X�ɒ���
		{
			// �܂��̓J�����g�f�B���N�g�����ɂ��邩�T��
			mode_dir_path = boost::filesystem::absolute(model_dir);
			if (!boost::filesystem::exists(mode_dir_path) && argc >= 1) // ����������argv[0]������s�t�@�C���̂���t�H���_�𐄒肵�A���̃t�H���_���ɂ��邩�T��
			{
				boost::filesystem::path a0(argv[0]);
				if (a0.is_absolute())
					mode_dir_path = a0.branch_path() / model_dir;
			}
		}

		if (!boost::filesystem::exists(mode_dir_path))
			return eWaifu2xError_FailedOpenModelFile;

		if (process == "cpu")
		{
			caffe::Caffe::set_mode(caffe::Caffe::CPU);
			isCuda = false;
		}
		else
		{
			caffe::Caffe::set_mode(caffe::Caffe::GPU);
			isCuda = true;
		}

		if (mode == "noise" || mode == "noise_scale" || mode == "auto_scale")
		{
			const std::string model_path = (mode_dir_path / "srcnn.prototxt").string();
			const std::string param_path = (mode_dir_path / ("noise" + std::to_string(noise_level) + "_model.json")).string();

			ret = ConstractNet(net_noise, model_path, param_path, process);
			if (ret != eWaifu2xError_OK)
				return ret;
		}

		if (mode == "scale" || mode == "noise_scale" || mode == "auto_scale")
		{
			const std::string model_path = (mode_dir_path / "srcnn.prototxt").string();
			const std::string param_path = (mode_dir_path / "scale2.0x_model.json").string();

			ret = ConstractNet(net_scale, model_path, param_path, process);
			if (ret != eWaifu2xError_OK)
				return ret;
		}

		const int input_block_plane_size = input_block_size * input_block_size * input_plane;
		const int output_block_plane_size = output_block_size * output_block_size * input_plane;

		if (isCuda)
		{
			CUDA_CHECK_WAIFU2X(cudaHostAlloc(&input_block, sizeof(float) * input_block_plane_size * batch_size, cudaHostAllocWriteCombined));
			CUDA_CHECK_WAIFU2X(cudaHostAlloc(&dummy_data, sizeof(float) * input_block_plane_size * batch_size, cudaHostAllocWriteCombined));
			CUDA_CHECK_WAIFU2X(cudaHostAlloc(&output_block, sizeof(float) * output_block_plane_size * batch_size, cudaHostAllocDefault));
		}
		else
		{
			input_block = new float[input_block_plane_size * batch_size];
			dummy_data = new float[input_block_plane_size * batch_size];
			output_block = new float[output_block_plane_size * batch_size];
		}

		for (size_t i = 0; i < input_block_plane_size * batch_size; i++)
			dummy_data[i] = 0.0f;

		is_inited = true;
	}
	catch (...)
	{
		return eWaifu2xError_InvalidParameter;
	}

	return eWaifu2xError_OK;
}

void Waifu2x::destroy()
{
	net_noise.reset();
	net_scale.reset();

	if (isCuda)
	{
		CUDA_HOST_SAFE_FREE(input_block);
		CUDA_HOST_SAFE_FREE(dummy_data);
		CUDA_HOST_SAFE_FREE(output_block);
	}
	else
	{
		SAFE_DELETE_WAIFU2X(input_block);
		SAFE_DELETE_WAIFU2X(dummy_data);
		SAFE_DELETE_WAIFU2X(output_block);
	}

	is_inited = false;
}

Waifu2x::eWaifu2xError Waifu2x::waifu2x(int factor, const uint32_t* source, uint32_t* dest, int width, int height)
{
	Waifu2x::eWaifu2xError ret;

	if (!is_inited)
		return eWaifu2xError_NotInitialized;

	cv::Mat float_image;
	ret = LoadMat(float_image, source, width, height);
	if (ret != eWaifu2xError_OK)
		return ret;

	cv::Mat im;
	if (input_plane == 1)
		return eWaifu2xError_NotInitialized;
	else
	{
		std::vector<cv::Mat> planes;
		cv::split(float_image, planes);

		if (float_image.channels() == 4)
			planes.resize(3);

		// BGR����RGB�ɂ���
		//std::swap(planes[0], planes[2]);

		cv::merge(planes, im);
	}
	cv::Size_<int> image_size = im.size();

	const bool isReconstructNoise = mode == "noise" || mode == "noise_scale" || mode == "auto_scale";
	const bool isReconstructScale = mode == "scale" || mode == "noise_scale";

	if (isReconstructNoise)
	{
		PaddingImage(im, im);

		ret = ReconstructImage(net_noise, im);
		if (ret != eWaifu2xError_OK)
			return ret;

		// �p�f�B���O����蕥��
		im = im(cv::Rect(offset, offset, image_size.width, image_size.height));
	}

	const int scale2 = ceil(log2((double)factor));
	const double shrinkRatio = (double)factor / std::pow(2.0, (double)scale2);

	if (isReconstructScale)
	{
		bool isError = false;
		for (int i = 0; i < scale2; i++)
		{
			Zoom2xAndPaddingImage(im, im, image_size);

			ret = ReconstructImage(net_scale, im);
			if (ret != eWaifu2xError_OK)
				return ret;

			// �p�f�B���O����蕥��
			im = im(cv::Rect(offset, offset, image_size.width, image_size.height));
		}
	}

	cv::Mat process_image;
	if (input_plane == 1)
	{
		// �č\�z�����P�x�摜��CreateZoomColorImage()�ō쐬�����F�����}�[�W���Ēʏ�̉摜�ɕϊ����A��������

		std::vector<cv::Mat> color_planes;
		CreateZoomColorImage(float_image, image_size, color_planes);

		float_image.release();

		color_planes[0] = im;
		im.release();

		cv::Mat converted_image;
		cv::merge(color_planes, converted_image);
		color_planes.clear();

		cv::cvtColor(converted_image, process_image, ConvertInverseMode);
		converted_image.release();
	}
	else
	{
		std::vector<cv::Mat> planes;
		cv::split(im, planes);

		// RGB����BGR�ɒ���
		//std::swap(planes[0], planes[2]);

		cv::merge(planes, process_image);
	}

	cv::Mat alpha;
	if (float_image.channels() == 4)
	{
		std::vector<cv::Mat> planes;
		cv::split(float_image, planes);
		alpha = planes[3];

		cv::resize(alpha, alpha, image_size, 0.0, 0.0, cv::INTER_CUBIC);
	}

	// �A���t�@�`�����l������������A�A���t�@��t�����ăJ���[����A���t�@�̉e���𔲂�
	if (!alpha.empty())
	{
		std::vector<cv::Mat> planes;
		cv::split(process_image, planes);
		process_image.release();

		planes.push_back(alpha);

		cv::Mat w2 = planes[3];

		planes[0] = (planes[0]).mul(1.0 / w2);
		planes[1] = (planes[1]).mul(1.0 / w2);
		planes[2] = (planes[2]).mul(1.0 / w2);

		cv::merge(planes, process_image);
	}

	const cv::Size_<int> ns(image_size.width * shrinkRatio, image_size.height * shrinkRatio);
	if (image_size.width != ns.width || image_size.height != ns.height)
		cv::resize(process_image, process_image, ns, 0.0, 0.0, cv::INTER_LINEAR);

	cv::Mat write_iamge;
	process_image.convertTo(write_iamge, CV_8U, 255.0);
	process_image.release();

	/*
	ret = WriteMat(write_iamge, output_file);
	if (ret != eWaifu2xError_OK)
	return ret;

	write_iamge.release();
	*/

	{
		const auto width = write_iamge.size().width;
		const auto stride = write_iamge.step1();
		for (int i = 0; i < write_iamge.size().height; i++)
			memcpy(dest + width * i, write_iamge.data + stride * i, stride);
	}

	return eWaifu2xError_OK;
}

const std::string& Waifu2x::used_process() const
{
	return process;
}
