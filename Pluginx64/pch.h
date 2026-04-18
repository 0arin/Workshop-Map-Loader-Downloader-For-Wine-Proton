#pragma once

#pragma comment(lib, "zlib.lib")
#pragma comment(lib, "libcurl.lib")
#pragma comment(lib, "cpr.lib")
#pragma comment(lib, "jsoncpp.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment (lib, "crypt32")
#pragma comment (lib, "Wldap32.lib")
#pragma comment( lib, "pluginsdk.lib" )
#include <urlmon.h>                 //Needed for the URLDownloadToFile() function
#pragma comment(lib, "urlmon.lib")  //Needed for the URLDownloadToFile() function
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "gdiplus.lib")

// GDI+ must be included before WIN32_LEAN_AND_MEAN is defined,
// because it needs GDI types that LEAN_AND_MEAN strips out.
#include <gdiplus.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX  // prevent windows.h from defining min/max macros that break std::min/max
#define _CRT_SECURE_NO_WARNINGS
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include <cpr/cpr.h>
#include <curl/curl.h>
#include <json/json.h>
#include <d3d11.h>

namespace fs = std::filesystem;


#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <fstream>
#include <iostream>
#include <cstddef>
#include <thread>
#include <mutex>
#include <filesystem>

#include "IMGUI/imgui.h"
