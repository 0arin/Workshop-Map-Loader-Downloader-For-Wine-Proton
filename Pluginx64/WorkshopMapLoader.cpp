#include "pch.h"
#include "WorkshopMapLoader.h"
#include "IMGUI/imgui_impl_dx11.h"

// zlib is already linked via pch. No extra headers needed for ZIP extraction.
#include <zlib.h>


BAKKESMOD_PLUGIN(Pluginx64, "Workshop Map Loader & Downloader", "1.15.3", 0)


namespace
{
	char dummyChar;
}



// ---------------------------------------------------------------------------
// ExtractZipCpp — pure C++ ZIP extraction using only zlib (already linked).
// Parses the ZIP local file headers directly, decompresses with zlib inflate.
// No minizip, no shell, no VBScript. Works on Windows and Wine/Proton identically.
// Returns true on success, false on any error.
// ---------------------------------------------------------------------------
bool ExtractZipCpp(const std::string& zipFilePath, const std::string& destDir)
{
	FILE* zf = fopen(zipFilePath.c_str(), "rb");
	if (!zf) return false;

	// Ensure dest dir exists
	try { fs::create_directories(fs::path(destDir)); }
	catch (...) { fclose(zf); return false; }

	// ZIP local file header signature
	static const uint32_t LOCAL_SIG = 0x04034b50;
	// ZIP data descriptor signature
	static const uint32_t DATA_DESC_SIG = 0x08074b50;

	const size_t BUF = 65536;
	std::vector<Bytef> inBuf(BUF), outBuf(BUF);
	bool success = true;

	while (success)
	{
		uint32_t sig = 0;
		if (fread(&sig, 4, 1, zf) != 1) break; // EOF or end of entries

		if (sig != LOCAL_SIG) break; // hit central directory or end

		// Local file header (after signature)
		uint16_t version, flags, method, modTime, modDate;
		uint32_t crc32val, compSize, uncompSize;
		uint16_t nameLen, extraLen;

		fread(&version,    2, 1, zf);
		fread(&flags,      2, 1, zf);
		fread(&method,     2, 1, zf);
		fread(&modTime,    2, 1, zf);
		fread(&modDate,    2, 1, zf);
		fread(&crc32val,   4, 1, zf);
		fread(&compSize,   4, 1, zf);
		fread(&uncompSize, 4, 1, zf);
		fread(&nameLen,    2, 1, zf);
		fread(&extraLen,   2, 1, zf);

		// Entry name
		std::string entryName(nameLen, '\0');
		fread(&entryName[0], 1, nameLen, zf);
		fseek(zf, extraLen, SEEK_CUR); // skip extra field

		// Normalize path separators
		std::replace(entryName.begin(), entryName.end(), '\\', '/');

		std::string destPath = destDir;
		if (destPath.back() != '/' && destPath.back() != '\\') destPath += '/';
		destPath += entryName;

		bool isDir = (!entryName.empty() && entryName.back() == '/');

		if (isDir)
		{
			try { fs::create_directories(fs::path(destPath)); }
			catch (...) {}
			continue;
		}

		// Create parent dirs
		try { fs::create_directories(fs::path(destPath).parent_path()); }
		catch (...) {}

		// Data descriptor flag (sizes may be zero in header, follow data)
		bool hasDataDesc = (flags & 0x0008) != 0;

		if (method == 0) // stored (no compression)
		{
			std::ofstream out(destPath, std::ios::binary);
			if (!out) { success = false; break; }
			uint32_t remaining = compSize;
			while (remaining > 0)
			{
				size_t toRead = (size_t)(std::min)((uint32_t)BUF, remaining);
				size_t got = fread(inBuf.data(), 1, toRead, zf);
				if (got == 0) { success = false; break; }
				out.write((char*)inBuf.data(), got);
				remaining -= (uint32_t)got;
			}
		}
		else if (method == 8) // deflate
		{
			std::ofstream out(destPath, std::ios::binary);
			if (!out) { success = false; break; }

			z_stream strm = {};
			if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) { success = false; break; }

			uint32_t remaining = compSize;
			int zret = Z_OK;
			while (remaining > 0 && zret != Z_STREAM_END)
			{
				size_t toRead = (size_t)(std::min)((uint32_t)BUF, remaining);
				size_t got = fread(inBuf.data(), 1, toRead, zf);
				if (got == 0) break;
				remaining -= (uint32_t)got;

				strm.avail_in = (uInt)got;
				strm.next_in  = inBuf.data();

				do {
					strm.avail_out = (uInt)BUF;
					strm.next_out  = outBuf.data();
					zret = inflate(&strm, Z_NO_FLUSH);
					if (zret == Z_STREAM_ERROR || zret == Z_DATA_ERROR || zret == Z_MEM_ERROR)
					{
						success = false; break;
					}
					size_t produced = BUF - strm.avail_out;
					out.write((char*)outBuf.data(), produced);
				} while (strm.avail_out == 0);

				if (!success) break;
			}
			inflateEnd(&strm);
		}
		else
		{
			// Unsupported compression method - skip
			if (!hasDataDesc && compSize > 0)
				fseek(zf, (long)compSize, SEEK_CUR);
		}

		// Skip data descriptor if present
		if (hasDataDesc)
		{
			uint32_t peek;
			fread(&peek, 4, 1, zf);
			if (peek != DATA_DESC_SIG) fseek(zf, -4, SEEK_CUR);
			fseek(zf, 12, SEEK_CUR); // crc32 + comp + uncomp sizes
		}
	}

	fclose(zf);
	return success;
}


// ---------------------------------------------------------------------------
// LoadTextureFromMemory — decode JPEG/PNG bytes via GDI+ and upload to D3D11.
// Must be called from the render thread. Returns nullptr on failure.
// ---------------------------------------------------------------------------
// Get the game's D3D11 device via ImGui's font texture SRV.
// ImGui's font texture was created with the game's real device — GetDevice() on
// that resource gives us the exact same device without any guesswork.
static ID3D11Device* s_cachedDevice = nullptr;
static ID3D11Device* GetGameD3DDevice()
{
	if (s_cachedDevice) return s_cachedDevice;

	ImGuiIO& io = ImGui::GetIO();
	if (!io.Fonts || !io.Fonts->TexID) return nullptr;

	auto* fontSRV = reinterpret_cast<ID3D11ShaderResourceView*>(io.Fonts->TexID);
	ID3D11Resource* res = nullptr;
	fontSRV->GetResource(&res);
	if (!res) return nullptr;

	res->GetDevice(&s_cachedDevice); // GetDevice AddRefs the device
	res->Release();
	return s_cachedDevice;
}


ID3D11ShaderResourceView* LoadTextureFromMemory(const std::vector<unsigned char>& data)
{
	if (data.empty()) return nullptr;

	ID3D11Device* device = GetGameD3DDevice();
	if (!device) return nullptr;

	static ULONG_PTR gdiplusToken = 0;
	static bool gdiplusInitialised = false;
	if (!gdiplusInitialised)
	{
		Gdiplus::GdiplusStartupInput startupInput;
		Gdiplus::GdiplusStartup(&gdiplusToken, &startupInput, nullptr);
		gdiplusInitialised = true;
	}

	IStream* stream = SHCreateMemStream(data.data(), static_cast<UINT>(data.size()));
	if (!stream) return nullptr;

	Gdiplus::Bitmap* bitmap = Gdiplus::Bitmap::FromStream(stream);
	stream->Release();

	if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok)
	{
		delete bitmap;
		return nullptr;
	}

	UINT width  = bitmap->GetWidth();
	UINT height = bitmap->GetHeight();
	Gdiplus::BitmapData bitmapData;
	Gdiplus::Rect rect(0, 0, (INT)width, (INT)height);
	if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bitmapData) != Gdiplus::Ok)
	{
		delete bitmap;
		return nullptr;
	}

	// GDI+ gives BGRA; D3D11 wants RGBA — swizzle B<->R
	std::vector<BYTE> rgba(width * height * 4);
	const BYTE* src = static_cast<const BYTE*>(bitmapData.Scan0);
	for (UINT y = 0; y < height; ++y)
	{
		const BYTE* row = src + y * bitmapData.Stride;
		BYTE* dst = rgba.data() + y * width * 4;
		for (UINT x = 0; x < width; ++x)
		{
			dst[0] = row[2]; dst[1] = row[1]; dst[2] = row[0]; dst[3] = row[3];
			row += 4; dst += 4;
		}
	}
	bitmap->UnlockBits(&bitmapData);
	delete bitmap;


	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width; desc.Height = height;
	desc.MipLevels = 1; desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA initData = {};
	initData.pSysMem = rgba.data();
	initData.SysMemPitch = width * 4;

	ID3D11Texture2D* tex = nullptr;
	if (FAILED(device->CreateTexture2D(&desc, &initData, &tex))) return nullptr;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	ID3D11ShaderResourceView* srv = nullptr;
	HRESULT hr = device->CreateShaderResourceView(tex, &srvDesc, &srv);
	tex->Release();
	if (FAILED(hr)) return nullptr;

	// Flush so Wine/Proton's D3D11 layer commits the texture before the next frame
	ID3D11DeviceContext* ctx = nullptr;
	device->GetImmediateContext(&ctx);
	if (ctx) { ctx->Flush(); ctx->Release(); }

	return srv;
}


std::string GameSetting::GetSelectedValue()
{
	return Values[selectedValue];
}


void Pluginx64::onLoad()
{
	BakkesmodPath = gameWrapper->GetBakkesModPath().string() + "\\";
	IfNoPreviewImagePath = BakkesmodPath + "data/WorkshopMapLoader/Search/NoPreview.jpg";

	std::string RLWin64_Path = std::filesystem::current_path().string();
	RLCookedPCConsole_Path = RLWin64_Path.substr(0, RLWin64_Path.length() - 14) + "TAGame\\CookedPCConsole";

	// Logo textures loaded lazily in Render() where D3D11 device is valid.
	logosLoaded = false;
	logoRLMAPS = logoMode1 = logoMode1Selected = logoMode2 = logoMode2Selected = nullptr;

	if (Directory_Or_File_Exists(BakkesmodPath + "data\\WorkshopMapLoader\\workshopmaploader.cfg"))
	{
		std::vector<std::string> CFGVariablesList = GetMapsFolderPathInCfg(BakkesmodPath + "data\\WorkshopMapLoader\\workshopmaploader.cfg");

		cvarManager->log("Workshop Maps Folder : " + CFGVariablesList.at(0));
		MapsFolderPath = CFGVariablesList.at(0);
		// unzipMethod is always CppZip
		dontAsk = std::stoi(CFGVariablesList.at(4));
		MapsDisplayMode = std::stoi(CFGVariablesList.at(5));
		nbTilesPerLine = std::stoi(CFGVariablesList.at(6));
		ControllerSensitivity = std::stoi(CFGVariablesList.at(7));
		ControllerScrollSensitivity = std::stoi(CFGVariablesList.at(8));

		if (CFGVariablesList.size() >= 11)
			UseController = (CFGVariablesList.at(10) == "1");

		FR = (CFGVariablesList.at(1) == "1");
		HasSeeNewUpdateAlert = (CFGVariablesList.at(3) == "1");

		if (CFGVariablesList.size() >= 12)
			EnableAntiFreezeFix = (CFGVariablesList.at(11) == "1");

		if (CFGVariablesList.size() >= 11)
			HasSeeNewUpdateAlert = (PluginVersion == CFGVariablesList.at(9));
		else
			HasSeeNewUpdateAlert = false;

		strncpy(MapsFolderPathBuf, MapsFolderPath.c_str(), IM_ARRAYSIZE(MapsFolderPathBuf));
	}
	else
	{
		cvarManager->log(BakkesmodPath + "data\\WorkshopMapLoader\\workshopmaploader.cfg : doesn't exist");

		if (!Directory_Or_File_Exists(RLCookedPCConsole_Path.string() + "\\mods"))
		{
			try { fs::create_directory(RLCookedPCConsole_Path.string() + "\\mods"); }
			catch (const std::exception& ex) { cvarManager->log(ex.what()); }
		}
		MapsFolderPath = RLCookedPCConsole_Path.string() + "\\mods";

		FR = false;
		// FIX: Default to CppZip — works on both Windows and Wine/Proton without any shell
		// unzipMethod is always CppZip (default)
		HasSeeNewUpdateAlert = false;
		dontAsk = 0;
		MapsDisplayMode = 0;
		nbTilesPerLine = 6;
		ControllerSensitivity = 10;
		ControllerScrollSensitivity = 10;
		PluginVersion = "1.15.2";
		EnableAntiFreezeFix = false;

		strncpy(MapsFolderPathBuf, MapsFolderPath.c_str(), IM_ARRAYSIZE(MapsFolderPathBuf));
		SaveInCFG();
	}

	{
		std::string imgCacheDir = BakkesmodPath + "data/WorkshopMapLoader/Search/img/RLMAPS";
		try { fs::create_directories(fs::path(imgCacheDir)); }
		catch (const std::exception& ex) { cvarManager->log(std::string("Failed to create image cache dir: ") + ex.what()); }
	}

	ApplyLanguage();
}



void Pluginx64::checkOpenMenuWithController(CanvasWrapper canvas)
{
	if (!UseController)
		return;

	if (gameWrapper->IsInOnlineGame())
		return;

	Gamepad ds4 = Gamepad(1);
	ds4.Update();

	if (ds4.Connected())
	{
		static bool ButtonsWasPressed = false;

		if (ds4.checkButtonPress(XINPUT_GAMEPAD_LEFT_THUMB) && ds4.checkButtonPress(XINPUT_GAMEPAD_RIGHT_THUMB) && !ButtonsWasPressed)
		{
			ButtonsWasPressed = true;
		}
		else if (!ds4.checkButtonPress(XINPUT_GAMEPAD_LEFT_THUMB) && !ds4.checkButtonPress(XINPUT_GAMEPAD_RIGHT_THUMB) && ButtonsWasPressed)
		{
			gameWrapper->Execute([&](GameWrapper* gw)
				{
					cvarManager->executeCommand("togglemenu " + GetMenuName());
				});
			ButtonsWasPressed = false;
		}
	}
}


//Local Maps

std::vector<std::string> Pluginx64::GetJSONLocalMapInfos(std::string jsonFilePath)
{
	std::vector<std::string> Infos;
	std::string Text;
	std::string line;
	std::ifstream myfile(jsonFilePath);

	if (myfile.is_open())
	{
		while (std::getline(myfile, line))
			Text += line;
		myfile.close();
	}

	Json::Value actualJson;
	Json::Reader reader;
	reader.parse(Text, actualJson);

	std::string MapTitle = actualJson["Title"].asString();
	std::string MapDescription = actualJson["Description"].asString();
	std::string MapAuthor = actualJson["Author"].asString();

	MapTitle.erase(std::remove(MapTitle.begin(), MapTitle.end(), '\n'), MapTitle.end());
	MapDescription.erase(std::remove(MapDescription.begin(), MapDescription.end(), '\n'), MapDescription.end());
	MapAuthor.erase(std::remove(MapAuthor.begin(), MapAuthor.end(), '\n'), MapAuthor.end());

	Infos.push_back(MapTitle);
	Infos.push_back(MapDescription);
	Infos.push_back(MapAuthor);

	return Infos;
}

void Pluginx64::RefreshMapsFunct(std::string mapsfolders)
{
	MapList.clear();
	selectedButton = 0;

	std::vector<std::filesystem::path> MapsDirectories;

	for (const auto& dir : fs::directory_iterator(mapsfolders))
	{
		if (dir.is_directory())
			MapsDirectories.push_back(dir.path());
	}

	if (MapsDirectories.size() == 0) { cvarManager->log("No maps detected"); return; }

	for (int i = 0; i < MapsDirectories.size(); i++)
	{
		cvarManager->log("Checking files in : " + MapsDirectories.at(i).string() + "/");

		std::filesystem::path CurrentMapDirectory = MapsDirectories.at(i);
		Map map;
		map.Folder = MapsDirectories.at(i).string();

		renameFileToUPK(CurrentMapDirectory);

		int nbFilesInDirectory = 0;
		for (const auto& file : fs::directory_iterator(CurrentMapDirectory))
			nbFilesInDirectory++;

		if (nbFilesInDirectory > 0)
		{
			int nbFiles = 0;
			bool hasFoundUPK = false;
			bool hasFoundPreview = false;
			bool hasFoundJSON = false;
			bool hasFoundZIP = false;

			for (const auto& file : fs::directory_iterator(CurrentMapDirectory))
			{
				std::string fileExtension = file.path().filename().extension().string();
				nbFiles++;

				if (!hasFoundJSON && fileExtension == ".json")
				{
					if (file.path().filename().string().substr(0, file.path().filename().string().length() - 5) == CurrentMapDirectory.filename().string())
					{
						map.JsonFile = file.path().string();
						hasFoundJSON = true;
						std::vector<std::string> MapInfosList = GetJSONLocalMapInfos(map.JsonFile);
						map.mapName = MapInfosList.at(0);
						map.mapDescription = MapInfosList.at(1);
						map.mapAuthor = MapInfosList.at(2);
						cvarManager->log("JSON name  : " + file.path().filename().string());
					}
				}
				else
				{
					if (nbFiles == nbFilesInDirectory && hasFoundJSON == false)
					{
						map.JsonFile = "NoInfos";
						cvarManager->log("No json found in this folder");
					}
				}

				if ((!hasFoundPreview && fileExtension == ".png") || (!hasFoundPreview && fileExtension == ".jpg") || (!hasFoundPreview && fileExtension == ".jfif"))
				{
					std::ifstream imgf(file.path(), std::ios::binary);
					if (imgf) map.PreviewImageBytes.assign(std::istreambuf_iterator<char>(imgf), std::istreambuf_iterator<char>());
					cvarManager->log("Preview bytes read : " + file.path().string());
					hasFoundPreview = true;
				}
				else
				{
					if (nbFiles == nbFilesInDirectory && hasFoundPreview == false)
					{
						std::ifstream imgf(IfNoPreviewImagePath, std::ios::binary);
						if (imgf) map.PreviewImageBytes.assign(std::istreambuf_iterator<char>(imgf), std::istreambuf_iterator<char>());
						cvarManager->log("No preview found in this folder");
					}
				}

				if (!hasFoundZIP && fileExtension == ".zip")
				{
					map.ZipFile = file.path();
					hasFoundZIP = true;
				}
				else
				{
					if (nbFiles == nbFilesInDirectory && hasFoundZIP == false)
					{
						map.ZipFile = "NoZipFound";
						cvarManager->log("No .zip file found in this folder : " + CurrentMapDirectory.string());
					}
				}

				if (!hasFoundUPK && fileExtension == ".upk")
				{
					map.UpkFile = file.path();
					hasFoundUPK = true;
				}
				else
				{
					if (nbFiles == nbFilesInDirectory && hasFoundUPK == false)
					{
						map.UpkFile = "NoUpkFound";
						cvarManager->log("No upk found in this folder : " + CurrentMapDirectory.string());
					}
				}
			}
		}
		else
		{
			cvarManager->log("Empty folder !");
			map.UpkFile = "EmptyFolder";
			map.ZipFile = "EmptyFolder";
			map.JsonFile = "EmptyFolder";
			{
			std::ifstream imgf(IfNoPreviewImagePath, std::ios::binary);
			if (imgf) map.PreviewImageBytes.assign(std::istreambuf_iterator<char>(imgf), std::istreambuf_iterator<char>());
		}
		}

		MapList.push_back(map);
		cvarManager->log("");
	}

	cachedNoUpkMapList.clear();
	cachedGoodMapList.clear();
	for (auto& map : MapList)
	{
		if (map.UpkFile == "NoUpkFound" && map.ZipFile != "EmptyFolder" && map.ZipFile != "NoZipFound")
			cachedNoUpkMapList.push_back(map);
		if (map.UpkFile != "NoUpkFound" && map.UpkFile != "EmptyFolder")
			cachedGoodMapList.push_back(map);
	}
}


void Pluginx64::AddMapManually(std::string mapName, std::string mapAuthor, std::string mapDescription, std::filesystem::path mapsDirectoryPath, std::filesystem::path mapFilePath, std::filesystem::path imagePath)
{
	std::string Workshop_Dl_Path = "";
	std::string workshopSafeMapName = replace(mapName, *" ", *"_");
	std::string specials[] = { "/", "\\", "?", ":", "*", "\"", "<", ">", "|", "-", "#" };
	for (auto special : specials)
		eraseAll(workshopSafeMapName, special);

	if (mapsDirectoryPath.string().back() == '/' || mapsDirectoryPath.string().back() == '\\')
		Workshop_Dl_Path = mapsDirectoryPath.string() + workshopSafeMapName;
	else
		Workshop_Dl_Path = mapsDirectoryPath.string() + "/" + workshopSafeMapName;

	try
	{
		fs::create_directory(Workshop_Dl_Path);
		cvarManager->log("Directory Created : " + Workshop_Dl_Path);
	}
	catch (const std::exception& ex)
	{
		cvarManager->log(ex.what());
		FolderErrorText = ex.what();
		FolderErrorBool = true;
		return;
	}

	if (Directory_Or_File_Exists(mapFilePath))
	{
		fs::copy(mapFilePath, Workshop_Dl_Path + "/" + mapFilePath.filename().string());
		cvarManager->log("Map pasted : " + Workshop_Dl_Path + "/" + mapFilePath.filename().string());
	}
	else
	{
		cvarManager->log("Couldn't find map file to paste");
		return;
	}

	CreateJSONLocalWorkshopInfos(workshopSafeMapName, Workshop_Dl_Path + "/", mapName, mapAuthor, mapDescription, "");
	cvarManager->log("JSON Created : " + Workshop_Dl_Path + "/" + workshopSafeMapName + ".json");

	if (Directory_Or_File_Exists(imagePath))
	{
		fs::copy(imagePath, Workshop_Dl_Path + "/" + imagePath.filename().string());
		cvarManager->log("Preview pasted : " + Workshop_Dl_Path + "/" + imagePath.filename().string());
	}
	else
	{
		cvarManager->log("Couldn't find preview to paste");
	}

	cvarManager->log(mapName + " added successfully!");
	AddedMapSccuessfully = true;
}


//Download Utils

void Pluginx64::CreateJSONLocalWorkshopInfos(std::string jsonFileName, std::string workshopMapPath, std::string mapTitle, std::string mapAuthor, std::string mapDescription,
	std::string mapPreviewUrl)
{
	std::string filename = workshopMapPath + jsonFileName + ".json";
	std::ofstream JSONFile(filename);
	JSONFile << "{\"Title\":\"" + mapTitle + "\",\"Author\":\"" + mapAuthor + "\",\"Description\":\"" + mapDescription + "\",\"PreviewUrl\":\"" + mapPreviewUrl + "\"}";
	JSONFile.close();
}



//rocketleaguemaps.us

void Pluginx64::GetResults(std::string keyWord, int IndexPage)
{
	RLMAPS_Searching = true;

	{
		std::lock_guard<std::mutex> lock(RLMAPS_ListMutex);
		RLMAPS_MapResultList.clear();
	}
	RLMAPS_PageSelected = IndexPage;
	if (IndexPage == 1)
	{
		NumPages = 0;
		std::thread t2(&Pluginx64::GetNumpPages, this, keyWord);
		t2.detach();
	}

	cpr::Response Request_MapInfos = cpr::Get(cpr::Url{ rlmaps_url + keyWord + "&page=" + std::to_string(IndexPage) });

	Json::Value actualJson;
	Json::Reader reader;
	reader.parse(Request_MapInfos.text, actualJson);
	const Json::Value maps = actualJson;

	RLMAPS_NumberOfMapsFound = maps.size();
	RLMAPS_MapResultList.reserve(maps.size());

	for (int index = 0; index < maps.size(); ++index)
	{
		std::thread t2(&Pluginx64::GetMapResult, this, maps, index);
		t2.detach();
		Sleep(100);
	}

	while (true)
	{
		std::size_t currentSize;
		{
			std::lock_guard<std::mutex> lock(RLMAPS_ListMutex);
			currentSize = RLMAPS_MapResultList.size();
		}
		if (currentSize == static_cast<std::size_t>(maps.size())) break;
		Sleep(10);
	}

	RLMAPS_Searching = false;
}


void Pluginx64::GetMapResult(Json::Value maps, int index)
{
	RLMAPS_MapResult result;
	result.ID = maps[index]["id"].asString();
	result.Name = maps[index]["name"].asString();
	result.Description = maps[index]["description"].asString();

	cpr::Response Request_MapDownloadLinks = cpr::Get(cpr::Url{ "https://celab.jetfox.ovh/api/v4/projects/" + result.ID + "/releases" });
	Json::Value actualJson2;
	Json::Reader reader2;
	reader2.parse(Request_MapDownloadLinks.text, actualJson2);
	const Json::Value maps2 = actualJson2;

	std::string projectPath = maps[index]["path_with_namespace"].asString();
	std::string pictureUrl = "https://celab.jetfox.ovh/" + projectPath + "/-/raw/master/RLMPreview.jpg";

	std::vector<RLMAPS_Release> releases;
	for (int release_index = 0; release_index < maps2.size(); ++release_index)
	{
		RLMAPS_Release release;
		release.name        = maps2[release_index]["name"].asString();
		release.tag_name    = maps2[release_index]["tag_name"].asString();
		release.description = maps2[release_index]["description"].asString();
		release.pictureLink = pictureUrl;

		const Json::Value& links = maps2[release_index]["assets"]["links"];
		for (int li = 0; li < (int)links.size(); ++li)
		{
			std::string linkType = links[li]["link_type"].asString();
			if (linkType != "image")
			{
				release.downloadLink = links[li]["url"].asString();
				release.zipName = links[li]["name"].asString();
				std::string specials[] = { "/", "\\", "?", ":", "*", "\"", "<", ">", "|", "#", "'", "`"};
				for (auto& s : specials)
					eraseAll(release.zipName, s);
				break;
			}
		}
		releases.push_back(release);
	}

	result.releases = releases;
	result.Size = "10000000";
	result.Author = maps[index]["namespace"]["path"].asString();
	result.PreviewUrl = pictureUrl;
	result.Image = nullptr;
	result.isImageLoaded = false;

	cvarManager->log("Map : " + result.Name);

	std::filesystem::path resultImagePath = BakkesmodPath + "data/WorkshopMapLoader/Search/img/RLMAPS/" + result.ID + ".jfif";

	if (Directory_Or_File_Exists(resultImagePath))
	{
		std::error_code ec;
		auto fileSize = fs::file_size(resultImagePath, ec);
		if (ec || fileSize < 1024)
		{
			cvarManager->log("Deleting stale/corrupt cache: " + resultImagePath.string());
			fs::remove(resultImagePath, ec);
		}
	}

	if (!Directory_Or_File_Exists(resultImagePath))
	{
		if (!result.PreviewUrl.empty())
		{
			result.IsDownloadingPreview = true;
			int listIndex;
			{
				std::lock_guard<std::mutex> lock(RLMAPS_ListMutex);
				RLMAPS_MapResultList.push_back(result);
				listIndex = (int)RLMAPS_MapResultList.size() - 1;
			}
			DownloadPreviewImage(result.PreviewUrl, resultImagePath.string(), listIndex);
		}
		else
		{
			std::lock_guard<std::mutex> lock(RLMAPS_ListMutex);
			RLMAPS_MapResultList.push_back(result);
		}
	}
	else
	{
		// Image already cached on disk. Push to list first, then load on render thread.
		result.ImagePath = resultImagePath;
		result.IsDownloadingPreview = false;

		int listIndex;
		{
			std::lock_guard<std::mutex> lock(RLMAPS_ListMutex);
			RLMAPS_MapResultList.push_back(result);
			listIndex = (int)RLMAPS_MapResultList.size() - 1;
		}

		// Read bytes on bg thread — render thread uploads via LoadTextureFromMemory
		{
			std::lock_guard<std::mutex> lock(RLMAPS_ListMutex);
			std::ifstream f(resultImagePath, std::ios::binary);
			if (f) RLMAPS_MapResultList[listIndex].RawImageBytes.assign(
				std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		}
	}
}

void Pluginx64::GetNumpPages(std::string keyWord)
{
	int ResultsSize = 20;
	NumPages = 0;
	while (ResultsSize == 20)
	{
		NumPages++;
		cpr::Response Request_Page = cpr::Get(cpr::Url{ rlmaps_url + keyWord + "&page=" + std::to_string(NumPages) });

		Json::Value actualJson;
		Json::Reader reader;
		reader.parse(Request_Page.text, actualJson);
		const Json::Value maps = actualJson;

		ResultsSize = maps.size();
		cvarManager->log("ResultsSize : " + std::to_string(ResultsSize));
	}
}

void Pluginx64::GetMapSize(std::string donwloadUrl)
{
	cpr::Response Request_Page = cpr::Head(cpr::Url{ donwloadUrl });
	std::string locationstr = Request_Page.raw_header.substr(Request_Page.raw_header.find("Location: ") + 10, Request_Page.raw_header.find("Vary:") - Request_Page.raw_header.find("Location: ") - 10);
	cvarManager->log("location found : " + locationstr);
	cpr::Response Request_size = cpr::Get(cpr::Url{ locationstr });
	cvarManager->log("HEADERS : " + Request_size.raw_header);
}


std::vector<Map> Pluginx64::QuickSearch_GetMapList(std::string keyWord)
{
	std::vector<Map> List;
	for (auto map : MapList)
	{
		std::string mapName = map.mapName;
		std::transform(mapName.begin(), mapName.end(), mapName.begin(), ::tolower);
		std::transform(keyWord.begin(), keyWord.end(), keyWord.begin(), ::tolower);
		if (mapName.find(keyWord) != std::string::npos)
			List.push_back(map);
	}
	return List;
}


void Pluginx64::RLMAPS_DownloadWorkshop(std::string folderpath, RLMAPS_MapResult mapResult, RLMAPS_Release release)
{
	UserIsChoosingYESorNO = true;
	while (UserIsChoosingYESorNO)
		Sleep(100);

	if (!AcceptTheDownload)
		return;

	std::string Workshop_Dl_Path = "";
	std::string workshopSafeMapName = replace(mapResult.Name, *" ", *"_");
	std::string specials[] = { "/", "\\", "?", ":", "*", "\"", "<", ">", "|", "-", "#" };
	for (auto special : specials)
		eraseAll(workshopSafeMapName, special);

	if (folderpath.back() == '/' || folderpath.back() == '\\')
		Workshop_Dl_Path = folderpath + workshopSafeMapName;
	else
		Workshop_Dl_Path = folderpath + "/" + workshopSafeMapName;

	try
	{
		fs::create_directory(Workshop_Dl_Path);
		cvarManager->log("Directory Created : " + Workshop_Dl_Path);
	}
	catch (const std::exception& ex)
	{
		cvarManager->log(ex.what());
		FolderErrorText = ex.what();
		FolderErrorBool = true;
		return;
	}

	CreateJSONLocalWorkshopInfos(workshopSafeMapName, Workshop_Dl_Path + "/", mapResult.Name, mapResult.Author, mapResult.Description, mapResult.PreviewUrl);
	cvarManager->log("JSON Created : " + Workshop_Dl_Path + "/" + workshopSafeMapName + ".json");

	if (Directory_Or_File_Exists(mapResult.ImagePath))
	{
		fs::copy(mapResult.ImagePath, Workshop_Dl_Path + "/" + workshopSafeMapName + ".jfif");
		cvarManager->log("Preview pasted : " + Workshop_Dl_Path + "/" + workshopSafeMapName + ".jfif");
	}
	else
	{
		cvarManager->log("Couldn't find preview to paste");
	}

	std::string download_url = release.downloadLink;
	cvarManager->log("Download URL : " + download_url);
	std::string Folder_Path = Workshop_Dl_Path + "/" + release.zipName;

	RLMAPS_WorkshopDownload_Progress = 0;
	RLMAPS_Download_Progress = 0;
	RLMAPS_IsDownloadingWorkshop = true;

	cvarManager->log("Download Starting...");

	CurlRequest req;
	req.url = download_url;
	req.progress_function = [this](double file_size, double downloaded, ...)
	{
		RLMAPS_Download_Progress = downloaded;
		RLMAPS_WorkshopDownload_FileSize = file_size;
	};

	HttpWrapper::SendCurlRequest(req, [this, Folder_Path, Workshop_Dl_Path](int code, char* data, size_t size)
		{
			std::ofstream out_file{ Folder_Path, std::ios_base::binary };
			if (out_file)
			{
				out_file.write(data, size);
				cvarManager->log("Workshop Downloaded in : " + Workshop_Dl_Path);
				RLMAPS_IsDownloadingWorkshop = false;
			}
		});

	while (RLMAPS_IsDownloadingWorkshop == true)
	{
		cvarManager->log("downloading...............");
		RLMAPS_WorkshopDownload_Progress = RLMAPS_Download_Progress;
		Sleep(500);
	}

	cvarManager->log("Extracting via CppZip (zlib)...");
	bool ok = ExtractZipCpp(Folder_Path, Workshop_Dl_Path);
	if (ok)
		cvarManager->log("CppZip extraction succeeded.");
	else
		cvarManager->log("CppZip extraction failed - zip may be corrupt or path issue.");

	int checkTime = 0;
	while (UdkInDirectory(Workshop_Dl_Path) == "Null")
	{
		cvarManager->log("Waiting for extracted file...");
		if (checkTime > 10)
		{
			cvarManager->log("Failed extracting the map zip file");
			return;
		}
		Sleep(1000);
		checkTime++;
	}

	cvarManager->log("File Extracted");
	renameFileToUPK(Workshop_Dl_Path);
}



//Textures

void Pluginx64::DownloadWorkshopTextures()
{
	std::string ZipFilePath = BakkesmodPath + "data\\WorkshopMapLoader\\Textures.zip";

	if (!Directory_Or_File_Exists(BakkesmodPath + "data\\WorkshopMapLoader\\Textures.zip"))
	{
		IsDownloading_WorkshopTextures = true;
		cvarManager->log("Starting download : Workshop Textures");

		CurlRequest req;
		req.url = "https://cdn.discordapp.com/attachments/1062156148054179850/1062156149257932821/Workshop-textures.zip";
		req.progress_function = [this](double file_size, double downloaded, ...)
		{
			Download_Textrures_Progress = downloaded;
		};

		HttpWrapper::SendCurlRequest(req, [this, ZipFilePath](int code, char* data, size_t size)
			{
				std::ofstream out_file{ ZipFilePath, std::ios_base::binary };
				if (out_file)
				{
					out_file.write(data, size);
					cvarManager->log("Textures downloaded : " + ZipFilePath);
					IsDownloading_WorkshopTextures = false;
				}
			});

		while (IsDownloading_WorkshopTextures)
		{
			cvarManager->log("downloading textures.......");
			DownloadTextrures_ProgressDisplayed = Download_Textrures_Progress;
			Sleep(500);
		}
	}

	cvarManager->log("Extracting textures via CppZip...");
	bool ok = ExtractZipCpp(ZipFilePath, RLCookedPCConsole_Path.string());
	if (ok)
		cvarManager->log("Texture extraction succeeded.");
	else
		cvarManager->log("Texture extraction failed.");

	cvarManager->log("File Extracted");
}

std::vector<std::string> Pluginx64::CheckExist_TexturesFiles()
{
	std::vector<std::string> missingFiles;
	for (auto textureFile : WorkshopTexturesFilesList)
	{
		if (!Directory_Or_File_Exists(RLCookedPCConsole_Path.string() + "\\" + textureFile))
			missingFiles.push_back(textureFile);
	}
	return missingFiles;
}


//Utils

std::wstring Pluginx64::s2ws(const std::string& s)
{
	int len;
	int slength = (int)s.length() + 1;
	len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, 0, 0);
	wchar_t* buf = new wchar_t[len];
	MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, buf, len);
	std::wstring r(buf);
	delete[] buf;
	return r;
}

std::string Pluginx64::replace(std::string s, char c1, char c2)
{
	int l = s.length();
	for (int i = 0; i < l; i++)
		if (s[i] == c1) s[i] = c2;
	return s;
}

std::string Pluginx64::convertToMB(std::string numberToConvert)
{
	if (numberToConvert.length() > 9 && numberToConvert.length() < 13)
	{
		std::string result = numberToConvert.insert(numberToConvert.length() - 6, ",");
		result = numberToConvert.erase(numberToConvert.length() - 6) + " GB";
		return result;
	}
	if (numberToConvert.length() > 6 && numberToConvert.length() < 10)
	{
		std::string result = numberToConvert.insert(numberToConvert.length() - 6, ",");
		result = numberToConvert.erase(numberToConvert.length() - 4) + " MB";
		return result;
	}
	if (numberToConvert.length() > 3 && numberToConvert.length() < 7)
	{
		std::string result = numberToConvert.insert(numberToConvert.length() - 3, ",");
		result = numberToConvert.erase(numberToConvert.length() - 2) + " kB";
		return result;
	}
	if (numberToConvert.length() > 0 && numberToConvert.length() < 4)
		return numberToConvert + " Bytes";
	return numberToConvert;
}

bool Pluginx64::Directory_Or_File_Exists(const fs::path& p, fs::file_status s)
{
	if (fs::status_known(s) ? fs::exists(s) : fs::exists(p))
		return true;
	return false;
}

std::vector<std::string> Pluginx64::FindAllSubstringInAString(std::string texte, std::string beginString, std::string endString)
{
	std::vector<std::string> List;
	std::vector<std::size_t> IndexPos;

	std::string::size_type posBegin = 0;
	std::string s = texte;

	while ((posBegin = s.find(beginString, posBegin)) != std::string::npos)
	{
		IndexPos.push_back(posBegin);
		posBegin += beginString.length();
	}

	for (int i = 0; i < IndexPos.size(); i++)
	{
		s = texte;
		s.erase(0, IndexPos.at(i));
		std::string resultString = s.substr(beginString.length(), s.find(endString) - beginString.length());
		List.push_back(resultString);
	}
	return List;
}

std::string Pluginx64::UdkInDirectory(std::string dirPath)
{
	for (const auto& file : fs::directory_iterator(dirPath))
	{
		if (file.path().extension().string() == ".udk" || file.path().extension().string() == ".upk")
			return file.path().string();
	}
	return "Null";
}

void Pluginx64::renameFileToUPK(std::filesystem::path filePath)
{
	if (!EnableAntiFreezeFix)
		return;

	for (std::string texture : WorkshopTexturesFilesList)
		if (filePath.filename().string() == texture) return;

	std::string UDKPath = UdkInDirectory(filePath.string());
	if (UDKPath == "Null") return;

	if (UDKPath.find("_antifreeze") != std::string::npos)
	{
		std::string oldUDKPath = UDKPath;
		eraseAll(UDKPath, "_antifreeze");
		if (rename(oldUDKPath.c_str(), UDKPath.c_str()) != 0)
			cvarManager->log("Error renaming file");
		else
			cvarManager->log("File renamed successfully");
	}
	else
	{
		std::string UPKPath_antifreeze = UDKPath.substr(0, UDKPath.length() - 4) + "_antifreeze.upk";
		if (rename(UDKPath.c_str(), UPKPath_antifreeze.c_str()) != 0)
			cvarManager->log("Error renaming file for antifreeze");
		else
			cvarManager->log("File renamed successfully for antifreeze");
	}
}

void Pluginx64::SaveInCFG()
{
	std::string filename = BakkesmodPath + "data\\WorkshopMapLoader\\workshopmaploader.cfg";
	std::ofstream CFGFile(filename);

	CFGFile << "MapsFolderPath = \"" + std::string(MapsFolderPathBuf) + "\"\n";
	CFGFile << "Language = \"" + std::to_string(FR) + "\"\n";
	CFGFile << "HasSeeNewUpdateAlert = \"" + std::to_string(HasSeeNewUpdateAlert) + "\"\n";
	CFGFile << "dontask = \"" + std::to_string(dontAsk) + "\"\n";
	CFGFile << "MapsDisplayMode = \"" + std::to_string(MapsDisplayMode) + "\"\n";
	CFGFile << "nbTilesPerLine = \"" + std::to_string(nbTilesPerLine) + "\"\n";
	CFGFile << "ControllerSensitivity = \"" + std::to_string(ControllerSensitivity) + "\"\n";
	CFGFile << "ControllerScrollSensitivity = \"" + std::to_string(ControllerScrollSensitivity) + "\"\n";
	CFGFile << "PluginVersion = \"" + PluginVersion + "\"\n";
	CFGFile << "UseController = \"" + std::to_string(UseController) + "\"\n";
	CFGFile << "EnableAntiFreezeFix = \"" + std::to_string(EnableAntiFreezeFix) + "\"";

	CFGFile.close();
	cvarManager->log("Saved in cfg");
}

std::vector<std::string> Pluginx64::GetMapsFolderPathInCfg(std::string cfgFilePath)
{
	std::vector<std::string> CfgInfosList;
	std::string line;
	std::ifstream myfile(cfgFilePath);

	if (myfile.is_open())
	{
		while (std::getline(myfile, line))
		{
			std::string value = FindAllSubstringInAString(line, "\"", "\"").at(0);
			CfgInfosList.push_back(value.substr(0, value.find("\"")));
		}
		myfile.close();
	}
	return CfgInfosList;
}

void Pluginx64::DownloadPreviewImage(std::string downloadUrl, std::string filePath, int mapResultIndex)
{
	std::string download_url = downloadUrl;
	std::string File_Path = filePath;
	std::replace(File_Path.begin(), File_Path.end(), '\\', '/');

	CurlRequest req;
	req.url = download_url;

	HttpWrapper::SendCurlRequest(req, [this, File_Path, mapResultIndex](int code, char* data, size_t size)
		{
			if (code != 200 || size == 0)
			{
				cvarManager->log("PREVIEW DOWNLOAD FAILED code=" + std::to_string(code) + " for index " + std::to_string(mapResultIndex));
				std::lock_guard<std::mutex> lock(RLMAPS_ListMutex);
				RLMAPS_MapResultList[mapResultIndex].IsDownloadingPreview = false;
				return;
			}

			try { fs::create_directories(fs::path(File_Path).parent_path()); }
			catch (...) {}

			std::ofstream imgFile(File_Path, std::ios_base::binary);
			if (!imgFile)
			{
				cvarManager->log("PREVIEW SAVE FAILED: " + File_Path);
				std::lock_guard<std::mutex> lock(RLMAPS_ListMutex);
				RLMAPS_MapResultList[mapResultIndex].IsDownloadingPreview = false;
				return;
			}
			imgFile.write(data, size);
			imgFile.close();
			cvarManager->log("PREVIEW SAVED: " + File_Path);

			// Store bytes — render thread uploads via LoadTextureFromMemory
			{
				std::lock_guard<std::mutex> lock(RLMAPS_ListMutex);
				RLMAPS_MapResultList[mapResultIndex].RawImageBytes.assign(data, data + size);
				RLMAPS_MapResultList[mapResultIndex].ImagePath = File_Path;
				RLMAPS_MapResultList[mapResultIndex].IsDownloadingPreview = false;
				cvarManager->log("Preview bytes ready: " + File_Path);
			}
		});
}

bool Pluginx64::FileIsInDirectoryRecursive(std::string dirPath, std::string filename)
{
	for (const auto& dir : fs::directory_iterator(dirPath))
	{
		if (dir.is_directory())
			for (const auto& file : fs::recursive_directory_iterator(dir.path()))
				if (!file.is_directory() && file.path().filename().string() == filename)
					return true;
	}
	return false;
}

float Pluginx64::DoRatio(float x, float y) { return x / y; }

void Pluginx64::CleanHTML(std::string& S)
{
	if (S == "") return;
	int n = S.length();
	int start = 0, end = 0;

	while (end != n - 1)
	{
		n = S.length();
		for (int i = 0; i < n; i++) {
			if (S[i] == '<') { start = i; break; }
			if (i == n - 1) return;
		}
		while (S[start] == ' ') start++;
		for (int i = start; i < n; i++) {
			if (S[i] == '>') { end = i; break; }
		}
		std::string result;
		for (int j = start; j <= end; j++) result += S[j];
		if (result == "<br>")
			S.replace(start, (end + 1) - start, "\n");
		else
			S.erase(start, (end + 1) - start);
	}
}

void Pluginx64::replaceAll(std::string& str, const std::string& from, const std::string& to) {
	if (from.empty()) return;
	size_t start_pos = 0;
	while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
		str.replace(start_pos, from.length(), to);
		start_pos += to.length();
	}
}

void Pluginx64::eraseAll(std::string& str, const std::string& from) {
	if (from.empty()) return;
	size_t start_pos = 0;
	while ((start_pos = str.find(from, start_pos)) != std::string::npos)
		str.erase(start_pos, from.length());
}

std::vector<std::string> Pluginx64::GetDrives()
{
	std::vector<std::string> Drives;
	int iCounter = 0;
	int iASCIILetter = (int)'a';

	DWORD dwDrivesMask = GetLogicalDrives();
	if (dwDrivesMask == 0) { printf("Failed to acquire mask of drives.\n"); exit(EXIT_FAILURE); }

	while (iCounter < 24) {
		if (dwDrivesMask & (1 << iCounter)) {
			char driveLetter = iASCIILetter + iCounter;
			Drives.push_back(std::string(1, driveLetter));
		}
		iCounter++;
	}
	return Drives;
}


void Pluginx64::ApplyLanguage()
{
	if (!FR)
	{
		SettingsText = "Settings";
		MultiplayerText = "Multiplayer";
		LastUpdateText = "Last Update";
		JoinCWGText = "Join Community Workshop Games discord server :";
		OpenCPCCText = "Open CookedPCConsole Directory";
		NoMapsCanBeJoinText = "No maps can be joined";
		MapsJoinableText = "Maps joinable";
		DlTexturesText = "Download Textures";
		LanguageText = "Language";
		ExtractMethodText = "Extract Method";
		WarningText = "Warning :";
		ControllerText = "Controller";
		UseControllerText = "Use Controller";
		ControllsText = "Controlls";
		ScrollSensitivityText = "Scroll Sensitivity";
		SensitivityText = "Sensitivity";
		ControllsLitText[0] = "Left Thumb + Right Thumb : open/close the menu";
		ControllsLitText[1] = "DPAD arrows : navigate through the maps";
		ControllsLitText[2] = "Left joystick : move the cursor";
		ControllsLitText[3] = "Right joystick : scroll";
		ControllsLitText[4] = "LB/L1 : click";
		ControllsLitText[5] = "B/O : close the menu";
		Tab1MapLoaderText = "Map Loader";
		Label1Text = "Put the path of the maps folder :";
		SelectMapsFolderText = "Select maps folder";
		RefreshMapsButtonText = "Refresh Maps";
		SavePathText = "Save Path";
		MapsPerLineText = "Maps Per Line :";
		OpenMapDirText = "Open map directory";
		DeleteMapText = "Delete map";
		CancelText = "Cancel";
		AddMapText = "Add Map";
		NameText = "Name :";
		AuthorText = "Author :";
		MapFilePathText = "Map File Path :";
		ImagePathText = "Image Path :";
		SelectFileText = "Select File";
		FieldEmptyText = "A field is empty !";
		ConfirmLabelText = "Do you really want to add this map ?";
		MapAddedSuccessfullyText = "Map added successfully !";
		DownloadButtonText = "Download";
		Label3Text = "Search A Workshop :";
		SearchButtonText = "Search";
		SearchingText = "Searching...";
		WorkshopsFoundText = "Workshops Found :";
		BrowseMapsText = "Browse Maps";
		Tab3SearchWorkshopText = "Search Workshop (rocketleaguemaps.us)";
		ResultByText = "By ";
		ResultSizeText = "Size : ";
		DownloadMapButtonText = "Download Map";
		DirNotExistText = "This directory is not valid !";
		DownloadFailedText = "Download Failed !" + DownloadFailedErrorText;
		WantToDawnloadText = "Do you really want to download?\nYou'll not be able to cancel if you start it.";
		YESButtonText = "YES";
		NOButtonText = "NO";
		IsDownloadDingWarningText = "A download is already running !\nYou cannot download 2 workshops at the same time.";
		PathSavedText = "Path saved successfully !";
		EMFMessageText1 = "The map isn't extracted from ";
		EMFMessageText2 = "\nChoose an extract method (you need to click on refresh maps after extracting) :";
		EMFStillDoesntWorkText = "Still not working";
		EMLabelText = "If both of the extract methods didn't work, you need to extract the files manually of ";
		DLTLabel1Text = "It seems like the workshop textures aren't installed in " + RLCookedPCConsole_Path.string();
		DLTLabel2Text = "You can still play without the workshop textures but some maps will have some white/weird textures.";
		DLTMissingFilesText = "Missing Files";
		DLTTexturesInstalledText = "Workshop textures installed !";
		CloseText = "Close";
		DontAskText = "Don't ask me again";
		NewFolderText = "New Folder";
		ConfirmText = "Confirm";
		SelectText = "Select";
	}
	else
	{
		SettingsText = "Parametres";
		MultiplayerText = "Multijoueur";
		LastUpdateText = "Derniere Maj";
		JoinCWGText = "Rejoins le serveur discord Community Workshop Games :";
		OpenCPCCText = "Ouvrir le dossier CookedPCConsole";
		NoMapsCanBeJoinText = "Aucune map ne peut etre rejoint";
		MapsJoinableText = "Maps rejoignables";
		DlTexturesText = "Telecharger les textures";
		LanguageText = "Langue";
		ExtractMethodText = "Methode d'extraction";
		WarningText = "Attention :";
		ControllerText = "Manette";
		UseControllerText = "Activer La Manette";
		ControllsText = "Commandes";
		ScrollSensitivityText = "Sensibilite du defilement";
		SensitivityText = "Sensibilite";
		ControllsLitText[0] = "Pouce Gauche + Pouce Droit : ouvrir/fermer le menu";
		ControllsLitText[1] = "Fleches : naviguer dans les maps";
		ControllsLitText[2] = "Joystick Gauche : bouger la souris";
		ControllsLitText[3] = "Joystick Droit : faire defiler";
		ControllsLitText[4] = "LB/L1 : cliquer";
		ControllsLitText[5] = "B/O : fermer le menu";
		Tab1MapLoaderText = "Charger Map";
		Label1Text = "Mets le chemin du dossier des maps :";
		SelectMapsFolderText = "Choisir Dossier Des Maps";
		RefreshMapsButtonText = "Rafraichir Les Maps";
		SavePathText = "Sauvegarder Le Chemin";
		MapsPerLineText = "Maps Par Ligne :";
		OpenMapDirText = "Ouvrir le dossier de la map";
		DeleteMapText = "Supprimer la map";
		CancelText = "Annuler";
		AddMapText = "Ajouter Map";
		NameText = "Nom :";
		AuthorText = "Auteur :";
		MapFilePathText = "Fichier De La Map :";
		ImagePathText = "Image :";
		SelectFileText = "Parcourir";
		FieldEmptyText = "Un champ est vide !";
		ConfirmLabelText = "Veux-tu vraiment ajouter cette map ?";
		MapAddedSuccessfullyText = "Map ajoute avec succes !";
		DownloadButtonText = "Telecharger";
		Label3Text = "Rechercher Un Workshop :";
		SearchButtonText = "Rechercher";
		SearchingText = "Recherche en cours...";
		WorkshopsFoundText = "Workshops Trouves :";
		BrowseMapsText = "Parcourir Les Maps";
		Tab3SearchWorkshopText = "Rechercher Workshop (rocketleaguemaps.us)";
		ResultByText = "Par ";
		ResultSizeText = "Taille : ";
		DownloadMapButtonText = "Telecharger La Map";
		DirNotExistText = "Ce chemin n'est pas valide !";
		DownloadFailedText = "Le telechargement a echoue !" + DownloadFailedErrorText;
		WantToDawnloadText = "Veux-tu vraiment telecharger?\nTu ne pourras plus l'annuler si tu le commence.";
		YESButtonText = "OUI";
		NOButtonText = "NON";
		IsDownloadDingWarningText = "Un telechargement est deja en cours !\nTu ne peux pas telecharger 2 workshops en meme temps.";
		PathSavedText = "Le chemin a ete sauvegarde !";
		EMFMessageText1 = "La map n'est pas extrait de ";
		EMFMessageText2 = "\nChoisis une methode d'extraction (rafraichis les maps apres l'extraction) :";
		EMFStillDoesntWorkText = "Ne fonctionne pas";
		EMLabelText = "Si les deux methodes d'extraction n'ont pas fonctionne, tu dois extraire les fichiers manuellement de ";
		DLTLabel1Text = "Les textures des workshops ne semblent pas etre installees dans " + RLCookedPCConsole_Path.string();
		DLTLabel2Text = "Tu peux toujours jouer sans mais des maps auront des textures blanches/bizarres.";
		DLTMissingFilesText = "Fichiers Manquants";
		DLTTexturesInstalledText = "Textures des workshops installees!";
		CloseText = "Fermer";
		DontAskText = "Ne plus me demander";
		NewFolderText = "Nouv. Dossier";
		ConfirmText = "Confirmer";
		SelectText = "Selectionner";
	}
}

void Pluginx64::onUnload()
{
	// Release raw D3D11 SRV textures before device teardown
	for (auto& map : MapList)
		if (map.PreviewImage) { map.PreviewImage->Release(); map.PreviewImage = nullptr; }
	for (auto& r : RLMAPS_MapResultList)
		if (r.Image) { r.Image->Release(); r.Image = nullptr; }
	if (logoRLMAPS)        { logoRLMAPS->Release();        logoRLMAPS = nullptr; }
	if (logoMode1)         { logoMode1->Release();         logoMode1 = nullptr; }
	if (logoMode2)         { logoMode2->Release();         logoMode2 = nullptr; }
	if (logoMode1Selected) { logoMode1Selected->Release(); logoMode1Selected = nullptr; }
	if (logoMode2Selected) { logoMode2Selected->Release(); logoMode2Selected = nullptr; }
	logosLoaded = false;
	if (s_cachedDevice) { s_cachedDevice->Release(); s_cachedDevice = nullptr; }

	RLMAPS_MapResultList.clear();
	MapList.clear();
	cachedNoUpkMapList.clear();
	cachedGoodMapList.clear();
}
