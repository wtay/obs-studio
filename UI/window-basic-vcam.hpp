#pragma once

#include <string>

#ifdef _WIN32
#define DEFAULT_VCAM_ID "dshow_output"
#elif defined(__APPLE__)
#define DEFAULT_VCAM_ID "mach_output"
#else
#define DEFAULT_VCAM_ID "v4l2_output"
#endif

enum VCamOutputType {
	InternalOutput,
	SceneOutput,
	SourceOutput,
};

enum VCamInternalType {
	Default,
	Preview,
};

struct VCamConfig {
	VCamOutputType type = VCamOutputType::InternalOutput;
	VCamInternalType internal = VCamInternalType::Default;
	std::string scene;
	std::string source;
};
