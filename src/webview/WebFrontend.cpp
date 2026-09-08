#include "Common/precompiled.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "Common/version.h"

#include "application/ApplicationRuntime.h"
#include "application/ApplicationPaths.h"
#include "application/ApplicationHost.h"
#include "application/EmulationController.h"
#include "application/LoggingFacade.h"
#include "application/EmulatedUsbFacade.h"
#include "application/DiagnosticFacade.h"
#include "application/MemorySearchFacade.h"
#include "application/PpcDebuggerFacade.h"
#include "audio/IAudioAPI.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/LaunchSettings.h"
#include "frontend/CemuExtendFrontendBridge.h"
#include "frontend/FrontendRuntime.h"
#include "input/InputManager.h"
#include "input/emulated/EmulatedController.h"
#include "webview/MainWindowState.h"
#include "webview/NativeWindowHost.h"
#include "webview/NativeKeyboardInput.h"
#include "webview/NativeFileDialog.h"
#include "webview/RendererHost.h"
#include "webview/RpcDispatcher.h"
#include "webview/ToolWindowSupport.h"
#include "webview/UpdatePlanRegistry.h"
#include "webview/WebHostState.h"
#include "webview/WebHostServices.h"
#include "webview/cef/CefOverlayInput.h"
#include "webview/generated/RpcMethods.h"
#include "webview/generated/WindowRoles.h"
#if defined(CEMU_OVERLAY_BACKEND_CEF)
#include "webview/CemodWebUiFrontend.h"
#include "webview/cef/CefNativeUiLoop.h"
#include "webview/cef/CefOverlayRuntime.h"
#endif
#include "util/helpers/helpers.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <boost/nowide/cstdio.hpp>
#include <png.h>
#if BOOST_OS_LINUX
#include <sys/prctl.h>
#endif

namespace
{
	using WebFrontend::CreateNativeWindowHost;
	using WebFrontend::CreateRendererHost;
	using WebFrontend::CreateToolWindowSupport;
	using WebFrontend::INativeWindowHost;
	using WebFrontend::IRendererHost;
	using WebFrontend::IToolWindowSupport;
	using WebFrontend::MainWindowState;
	using WebFrontend::RpcDispatcher;
	using WebFrontend::WebHostServices;
	using WebFrontend::WebHostState;
	class Runtime;
	struct RuntimeCallbackGate
	{
		std::mutex mutex;
		Runtime* target{};
	};
	constexpr std::uint64_t kPadOverlayWindowId = std::numeric_limits<std::uint64_t>::max();
	constexpr std::uint64_t kMainOverlayWindowId = kPadOverlayWindowId - 1;

	std::string JsonString(std::string_view value)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
		return {buffer.GetString(), buffer.GetSize()};
	}

	constexpr std::array<std::string_view, 20> kSupportedUiLanguages{
		"system", "en", "ar", "ca", "de", "es", "fr", "he", "hu", "it",
		"ja", "ko", "nb", "nl", "pl", "pt", "ru", "sv", "tr", "uk"};

	std::string NormalizeUiLanguage(std::string_view language)
	{
		if (language == "zh" || std::ranges::find(kSupportedUiLanguages, language) !=
									kSupportedUiLanguages.end())
			return std::string(language);
		return "system";
	}

	std::string Base64(std::string_view value)
	{
		static constexpr std::string_view alphabet =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string output;
		output.reserve((value.size() + 2) / 3 * 4);
		for (std::size_t offset = 0; offset < value.size(); offset += 3)
		{
			const auto remaining = value.size() - offset;
			const auto first = static_cast<unsigned char>(value[offset]);
			const auto second = remaining > 1 ? static_cast<unsigned char>(value[offset + 1]) : 0;
			const auto third = remaining > 2 ? static_cast<unsigned char>(value[offset + 2]) : 0;
			const std::uint32_t bits = (first << 16) | (second << 8) | third;
			output.push_back(alphabet[(bits >> 18) & 0x3f]);
			output.push_back(alphabet[(bits >> 12) & 0x3f]);
			output.push_back(remaining > 1 ? alphabet[(bits >> 6) & 0x3f] : '=');
			output.push_back(remaining > 2 ? alphabet[bits & 0x3f] : '=');
		}
		return output;
	}

	std::optional<std::string> TgaDataUrl(const std::vector<std::uint8_t>& data)
	{
		if (data.size() < 18 || data[1] != 0 ||
			(data[2] != 2 && data[2] != 3))
			return std::nullopt;
		const auto readU16 = [&data](std::size_t offset) {
			return static_cast<std::uint16_t>(data[offset]) |
				   static_cast<std::uint16_t>(data[offset + 1] << 8);
		};
		const auto width = readU16(12);
		const auto height = readU16(14);
		const auto bitsPerPixel = data[16];
		const auto channels = static_cast<std::size_t>(bitsPerPixel / 8);
		if (width == 0 || height == 0 || width > 4096 || height > 4096 ||
			(data[2] == 2 && channels != 3 && channels != 4) ||
			(data[2] == 3 && channels != 1))
			return std::nullopt;
		const auto pixelOffset = static_cast<std::size_t>(18 + data[0]);
		const auto pixelCount = static_cast<std::size_t>(width) * height;
		if (pixelOffset > data.size() ||
			pixelCount > (data.size() - pixelOffset) / channels)
			return std::nullopt;

		const bool topOrigin = (data[17] & 0x20) != 0;
		const bool rightOrigin = (data[17] & 0x10) != 0;
		std::vector<std::uint8_t> rgba(pixelCount * 4);
		for (std::size_t y = 0; y < height; ++y)
		{
			const auto sourceY = topOrigin ? y : height - y - 1;
			for (std::size_t x = 0; x < width; ++x)
			{
				const auto sourceX = rightOrigin ? width - x - 1 : x;
				const auto source = pixelOffset +
									(sourceY * static_cast<std::size_t>(width) + sourceX) * channels;
				const auto destination =
					(y * static_cast<std::size_t>(width) + x) * 4;
				if (channels == 1)
				{
					rgba[destination] = data[source];
					rgba[destination + 1] = data[source];
					rgba[destination + 2] = data[source];
					rgba[destination + 3] = 0xff;
				}
				else
				{
					rgba[destination] = data[source + 2];
					rgba[destination + 1] = data[source + 1];
					rgba[destination + 2] = data[source];
					rgba[destination + 3] = channels == 4 ? data[source + 3] : 0xff;
				}
			}
		}

		png_image image{};
		image.version = PNG_IMAGE_VERSION;
		image.width = width;
		image.height = height;
		image.format = PNG_FORMAT_RGBA;
		png_alloc_size_t encodedSize{};
		if (!png_image_write_to_memory(&image, nullptr, &encodedSize, 0,
									   rgba.data(), 0, nullptr) ||
			encodedSize == 0)
		{
			png_image_free(&image);
			return std::nullopt;
		}
		std::vector<std::uint8_t> encoded(encodedSize);
		if (!png_image_write_to_memory(&image, encoded.data(), &encodedSize, 0,
									   rgba.data(), 0, nullptr))
		{
			png_image_free(&image);
			return std::nullopt;
		}
		png_image_free(&image);
		encoded.resize(encodedSize);
		return "data:image/png;base64," +
			   Base64(std::string_view(
				   reinterpret_cast<const char*>(encoded.data()), encoded.size()));
	}

	std::optional<fs::path> ScreenshotPath(bool mainWindow)
	{
		static std::mutex pathMutex;
		std::scoped_lock lock(pathMutex);
		const auto directory = ActiveSettings::GetUserDataPath("screenshots");
		std::error_code error;
		fs::create_directories(directory, error);
		if (error)
			return {};
		const auto now = std::chrono::system_clock::to_time_t(
			std::chrono::system_clock::now());
		std::tm local{};
#if BOOST_OS_WINDOWS
		localtime_s(&local, &now);
#else
		localtime_r(&now, &local);
#endif
		const auto stem = fmt::format("Screenshot_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}{}",
									  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
									  local.tm_hour, local.tm_min, local.tm_sec, mainWindow ? "" : "_GamePad");
		for (unsigned suffix = 0; suffix < 1000; ++suffix)
		{
			const auto candidate = directory / (suffix == 0 ? fmt::format("{}.png", stem) : fmt::format("{}_{}.png", stem, suffix + 1));
			if (!fs::exists(candidate, error) && !error)
				return candidate;
			error.clear();
		}
		return {};
	}

	bool WriteRgbPng(const fs::path& path, std::span<const std::uint8_t> pixels,
					 int width, int height)
	{
		if (width <= 0 || height <= 0 || pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3)
			return false;
		auto temporary = path;
		temporary += ".tmp";
		const auto temporaryUtf8 = _pathToUtf8(temporary);
		FILE* file = boost::nowide::fopen(temporaryUtf8.c_str(), "wb");
		if (!file)
			return false;
		png_image image{};
		image.version = PNG_IMAGE_VERSION;
		image.width = static_cast<png_uint_32>(width);
		image.height = static_cast<png_uint_32>(height);
		image.format = PNG_FORMAT_RGB;
		const bool written = png_image_write_to_stdio(&image, file, 0,
													  pixels.data(), 0, nullptr) != 0;
		png_image_free(&image);
		const bool closed = std::fclose(file) == 0;
		if (!written || !closed)
		{
			std::error_code ignored;
			fs::remove(temporary, ignored);
			return false;
		}
		std::error_code error;
		fs::rename(temporary, path, error);
		if (error)
		{
			fs::remove(temporary, error);
			return false;
		}
		return true;
	}

	std::string TitleIdString(std::uint64_t titleId)
	{
		std::array<char, 17> text{};
		std::snprintf(text.data(), text.size(), "%016llx",
					  static_cast<unsigned long long>(titleId));
		return text.data();
	}

	using JsonWriter = rapidjson::Writer<rapidjson::StringBuffer>;

	std::uint64_t ParseDecimalUint64(std::string_view text, std::string_view name)
	{
		std::uint64_t value{};
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
		if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
			throw std::invalid_argument(std::string(name) + " must be an unsigned decimal string");
		return value;
	}

	std::string_view CemodErrorName(Application::CemodManagerError error)
	{
		switch (error)
		{
		case Application::CemodManagerError::None:
			return "none";
		case Application::CemodManagerError::Conflict:
			return "conflict";
		case Application::CemodManagerError::NotFound:
			return "notFound";
		case Application::CemodManagerError::InvalidPermissions:
			return "invalidPermissions";
		case Application::CemodManagerError::InspectionFailed:
			return "inspectionFailed";
		case Application::CemodManagerError::SaveFailed:
			return "saveFailed";
		case Application::CemodManagerError::ImportFailed:
			return "importFailed";
		}
		return "inspectionFailed";
	}

	std::string CemodSnapshotJson(const Application::CemodManagerSnapshot& snapshot)
	{
		rapidjson::StringBuffer buffer;
		JsonWriter writer(buffer);
		writer.StartObject();
		writer.Key("generation");
		writer.String(std::to_string(snapshot.generation).c_str());
		writer.Key("selectedTitleId");
		if (snapshot.selectedTitleId)
			writer.String(TitleIdString(*snapshot.selectedTitleId).c_str());
		else
			writer.Null();
		writer.Key("packages");
		writer.StartArray();
		for (const auto& package : snapshot.packages)
		{
			writer.StartObject();
			auto string = [&writer](const char* name, const std::string& value) {
				writer.Key(name);
				writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
			};
			string("packageKey", package.packageKey);
			string("modId", package.modId);
			string("principal", package.principal);
			string("modIdentity", package.modIdentity);
			string("packageDigest", package.packageDigest);
			string("pluginName", package.pluginName);
			string("author", package.author);
			string("version", package.version);
			string("description", package.description);
			string("scope", package.scope);
			string("status", package.status);
			string("approvalReason", package.approvalReason);
			writer.Key("titleIds");
			writer.StartArray();
			for (const auto titleId : package.titleIds)
				writer.String(TitleIdString(titleId).c_str());
			writer.EndArray();
			writer.Key("warnings");
			writer.StartArray();
			for (const auto& warning : package.warnings)
				writer.String(warning.data(), static_cast<rapidjson::SizeType>(warning.size()));
			writer.EndArray();
			writer.Key("permissions");
			writer.StartArray();
			for (const auto& permission : package.permissions)
			{
				writer.StartObject();
				string("name", permission.name);
				writer.Key("bit");
				writer.String(std::to_string(permission.bit).c_str());
				writer.Key("requested");
				writer.Bool(permission.requested);
				writer.Key("granted");
				writer.Bool(permission.granted);
				writer.Key("dangerous");
				writer.Bool(permission.dangerous);
				writer.Key("manifestMismatch");
				writer.Bool(permission.manifestMismatch);
				writer.EndObject();
			}
			writer.EndArray();
			writer.Key("requestedPermissions");
			writer.String(std::to_string(package.requestedPermissions).c_str());
			writer.Key("grantedPermissions");
			writer.String(std::to_string(package.grantedPermissions).c_str());
			writer.Key("approved");
			writer.Bool(package.approved);
			writer.Key("signedPackage");
			writer.Bool(package.signedPackage);
			writer.Key("trustedNative");
			writer.Bool(package.trustedNative);
			writer.Key("wups");
			writer.Bool(package.wups);
			writer.Key("headless");
			writer.Bool(package.headless);
			writer.Key("runtimeAvailable");
			writer.Bool(package.runtimeAvailable);
			writer.Key("valid");
			writer.Bool(package.valid);
			writer.Key("enabled");
			writer.Bool(package.enabled);
			writer.Key("trustUpdates");
			writer.Bool(package.trustUpdates);
			writer.EndObject();
		}
		writer.EndArray();
		writer.Key("cancelled");
		writer.Bool(snapshot.cancelled);
		writer.EndObject();
		return {buffer.GetString(), buffer.GetSize()};
	}

	std::string CemodResultJson(const Application::CemodManagerResult& result)
	{
		return std::string(R"({"ok":)") + (result ? "true" : "false") +
			   R"(,"error":)" + JsonString(CemodErrorName(result.error)) +
			   R"(,"diagnostic":)" + JsonString(result.diagnostic) +
			   R"(,"snapshot":)" + CemodSnapshotJson(result.snapshot) + "}";
	}

	void WriteAccount(JsonWriter& writer, const Application::AccountInfo& account)
	{
		writer.StartObject();
		writer.Key("persistentId");
		writer.Uint(account.persistentId);
		writer.Key("persistentIdHex");
		std::array<char, 9> persistentId{};
		std::snprintf(persistentId.data(), persistentId.size(), "%08x", account.persistentId);
		writer.String(persistentId.data());
		writer.Key("miiName");
		const auto miiName = boost::nowide::narrow(account.miiName);
		writer.String(miiName.data(), static_cast<rapidjson::SizeType>(miiName.size()));
		writer.Key("birthYear");
		writer.Uint(account.birthYear);
		writer.Key("birthMonth");
		writer.Uint(account.birthMonth);
		writer.Key("birthDay");
		writer.Uint(account.birthDay);
		writer.Key("gender");
		writer.Uint(account.gender);
		writer.Key("email");
		writer.String(account.email.data(),
					  static_cast<rapidjson::SizeType>(account.email.size()));
		writer.Key("country");
		writer.Uint(account.country);
		writer.Key("validOnlineAccount");
		writer.Bool(account.validOnlineAccount);
		writer.EndObject();
	}

	std::string_view AccountNetworkServiceName(Application::AccountNetworkService service)
	{
		switch (service)
		{
		case Application::AccountNetworkService::Nintendo:
			return "nintendo";
		case Application::AccountNetworkService::Pretendo:
			return "pretendo";
		case Application::AccountNetworkService::Custom:
			return "custom";
		case Application::AccountNetworkService::Plasma:
			return "plasma";
		default:
			return "offline";
		}
	}

	Application::AccountNetworkService ParseAccountNetworkService(std::string_view service)
	{
		if (service == "offline")
			return Application::AccountNetworkService::Offline;
		if (service == "nintendo")
			return Application::AccountNetworkService::Nintendo;
		if (service == "pretendo")
			return Application::AccountNetworkService::Pretendo;
		if (service == "custom")
			return Application::AccountNetworkService::Custom;
		if (service == "plasma")
			return Application::AccountNetworkService::Plasma;
		throw std::invalid_argument("unknown account network service");
	}

	std::string_view AccountFileStateName(Application::AccountFileState state)
	{
		switch (state)
		{
		case Application::AccountFileState::Corrupted:
			return "corrupted";
		case Application::AccountFileState::Ok:
			return "ok";
		default:
			return "missing";
		}
	}

	std::string_view AccountOnlineErrorName(Application::AccountOnlineError error)
	{
		switch (error)
		{
		case Application::AccountOnlineError::NoAccountId:
			return "noAccountId";
		case Application::AccountOnlineError::NoPasswordCached:
			return "noPasswordCached";
		case Application::AccountOnlineError::PasswordCacheEmpty:
			return "passwordCacheEmpty";
		case Application::AccountOnlineError::NoPrincipalId:
			return "noPrincipalId";
		default:
			return "none";
		}
	}

	std::string AccountJson(const Application::AccountInfo& account)
	{
		rapidjson::StringBuffer buffer;
		JsonWriter writer(buffer);
		WriteAccount(writer, account);
		return {buffer.GetString(), buffer.GetSize()};
	}

	void WriteGraphicPack(JsonWriter& writer, const Application::GraphicPackInfo& pack)
	{
		writer.StartObject();
		auto string = [&writer](const char* key, const std::string& value) {
			writer.Key(key);
			writer.String(value.data(),
						  static_cast<rapidjson::SizeType>(value.size()));
		};
		string("key", pack.key);
		string("virtualPath", pack.virtualPath);
		string("name", pack.name);
		string("description", pack.description);
		writer.Key("version");
		writer.Int(pack.version);
		writer.Key("universal");
		writer.Bool(pack.universal);
		writer.Key("enabled");
		writer.Bool(pack.enabled);
		writer.Key("activated");
		writer.Bool(pack.activated);
		writer.Key("defaultEnabled");
		writer.Bool(pack.defaultEnabled);
		writer.Key("hasShaders");
		writer.Bool(pack.hasShaders);
		writer.Key("hasPatches");
		writer.Bool(pack.hasPatches);
		writer.Key("hasCustomVsync");
		writer.Bool(pack.hasCustomVsync);
		writer.Key("supportedVersion");
		writer.Bool(pack.supportedVersion);
		writer.Key("titleIds");
		writer.StartArray();
		for (const auto titleId : pack.titleIds)
		{
			const auto value = TitleIdString(titleId);
			writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
		}
		writer.EndArray();
		writer.Key("presetOrder");
		writer.StartArray();
		for (const auto& category : pack.presetOrder)
			writer.String(category.data(), static_cast<rapidjson::SizeType>(category.size()));
		writer.EndArray();
		writer.Key("presets");
		writer.StartArray();
		for (const auto& preset : pack.presets)
		{
			writer.StartObject();
			string("category", preset.category);
			string("name", preset.name);
			writer.Key("active");
			writer.Bool(preset.active);
			writer.Key("visible");
			writer.Bool(preset.visible);
			writer.EndObject();
		}
		writer.EndArray();
		writer.EndObject();
	}

	std::string GraphicPackMutationJson(const Application::GraphicPackResult& result)
	{
		if (!result)
			throw std::runtime_error(result.diagnostic.empty() ? "graphic pack operation failed" : result.diagnostic);
		rapidjson::StringBuffer buffer;
		JsonWriter writer(buffer);
		writer.StartObject();
		writer.Key("changed");
		writer.Bool(result.changed);
		writer.Key("titleRunning");
		writer.Bool(result.titleRunning);
		writer.Key("requiresRestart");
		writer.Bool(result.requiresRestart);
		writer.Key("applied");
		writer.Bool(result.applied);
		writer.Key("reloaded");
		writer.Bool(result.reloaded);
		writer.Key("diagnostic");
		writer.String(result.diagnostic.data(),
					  static_cast<rapidjson::SizeType>(result.diagnostic.size()));
		if (result.info)
		{
			writer.Key("info");
			WriteGraphicPack(writer, *result.info);
		}
		writer.EndObject();
		return {buffer.GetString(), buffer.GetSize()};
	}

	std::string_view GraphicPackInstallPhaseName(
		Application::GraphicPackInstallPhase phase)
	{
		switch (phase)
		{
		case Application::GraphicPackInstallPhase::Downloading:
			return "downloading";
		case Application::GraphicPackInstallPhase::Extracting:
			return "extracting";
		case Application::GraphicPackInstallPhase::Refreshing:
			return "refreshing";
		default:
			return "checking";
		}
	}

	std::string_view GraphicPackInstallErrorName(
		Application::GraphicPackInstallError error)
	{
		switch (error)
		{
		case Application::GraphicPackInstallError::ConfirmationRequired:
			return "confirmationRequired";
		case Application::GraphicPackInstallError::Cancelled:
			return "cancelled";
		case Application::GraphicPackInstallError::InvalidUrl:
			return "invalidUrl";
		case Application::GraphicPackInstallError::ConnectionFailed:
			return "connectionFailed";
		case Application::GraphicPackInstallError::InvalidArchive:
			return "invalidArchive";
		case Application::GraphicPackInstallError::Conflict:
			return "conflict";
		case Application::GraphicPackInstallError::IoFailure:
			return "ioFailure";
		default:
			return "none";
		}
	}

	std::string FrontendSettingsJson(
		const Application::FrontendSettingsSnapshot& snapshot)
	{
		rapidjson::StringBuffer buffer;
		JsonWriter writer(buffer);
		writer.StartObject();
		writer.Key("revision");
		writer.Uint64(snapshot.revision);
		writer.Key("gamePaths");
		writer.StartArray();
		for (const auto& path : snapshot.gamePaths)
		{
			const auto value = _pathToUtf8(path);
			writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
		}
		writer.EndArray();
		writer.Key("startFullscreen");
		writer.Bool(snapshot.startFullscreen);
		writer.Key("openPad");
		writer.Bool(snapshot.openPad);
		writer.Key("checkUpdates");
		writer.Bool(snapshot.checkUpdates);
		writer.Key("saveScreenshots");
		writer.Bool(snapshot.saveScreenshots);
		writer.Key("updateChecksSupported");
		writer.Bool(snapshot.updateChecksSupported);
		writer.Key("portableMode");
		writer.Bool(snapshot.portableMode);
		writer.Key("titleRunning");
		writer.Bool(snapshot.titleRunning);
		writer.Key("setupCompleted");
		writer.Bool(snapshot.setupCompleted);
		writer.Key("fullscreenOverride");
		if (snapshot.fullscreenOverride)
			writer.Bool(*snapshot.fullscreenOverride);
		else
			writer.Null();
		writer.Key("crashDump");
		writer.String(snapshot.crashDump.c_str());
		writer.Key("crashDumpChoices");
		writer.StartArray();
		for (const auto& choice : snapshot.crashDumpChoices)
			writer.String(choice.c_str());
		writer.EndArray();
		writer.EndObject();
		return {buffer.GetString(), buffer.GetSize()};
	}

	std::string_view FrontendSettingsErrorName(Application::FrontendSettingsError error)
	{
		switch (error)
		{
		case Application::FrontendSettingsError::Conflict:
			return "conflict";
		case Application::FrontendSettingsError::TitleRunning:
			return "titleRunning";
		case Application::FrontendSettingsError::FullscreenOverride:
			return "fullscreenOverride";
		case Application::FrontendSettingsError::UpdateUnsupported:
			return "updateUnsupported";
		case Application::FrontendSettingsError::InvalidPath:
			return "invalidPath";
		case Application::FrontendSettingsError::StorageFailed:
			return "storageFailed";
		case Application::FrontendSettingsError::SaveFailed:
			return "saveFailed";
		default:
			return "none";
		}
	}

	std::string FrontendSettingsResultJson(
		const Application::FrontendSettingsResult& result)
	{
		return std::string(R"({"ok":)") + (result ? "true" : "false") +
			   R"(,"error":)" + JsonString(FrontendSettingsErrorName(result.error)) +
			   R"(,"snapshot":)" + FrontendSettingsJson(result.snapshot) +
			   R"(,"diagnostic":)" + JsonString(result.diagnostic) + "}";
	}

	std::string_view HotkeyActionName(Application::HotkeyAction action)
	{
		switch (action)
		{
		case Application::HotkeyAction::ToggleFullscreen:
			return "toggleFullscreen";
		case Application::HotkeyAction::ToggleFullscreenAlternative:
			return "toggleFullscreenAlternative";
		case Application::HotkeyAction::ExitFullscreen:
			return "exitFullscreen";
		case Application::HotkeyAction::TakeScreenshot:
			return "takeScreenshot";
		case Application::HotkeyAction::ToggleFastForward:
			return "toggleFastForward";
		case Application::HotkeyAction::EndEmulation:
			return "endEmulation";
		case Application::HotkeyAction::ExitApplication:
			return "exitApplication";
		}
		return "toggleFullscreen";
	}

	Application::HotkeyAction ParseHotkeyAction(std::string_view action)
	{
		if (action == "toggleFullscreen")
			return Application::HotkeyAction::ToggleFullscreen;
		if (action == "toggleFullscreenAlternative")
			return Application::HotkeyAction::ToggleFullscreenAlternative;
		if (action == "exitFullscreen")
			return Application::HotkeyAction::ExitFullscreen;
		if (action == "takeScreenshot")
			return Application::HotkeyAction::TakeScreenshot;
		if (action == "toggleFastForward")
			return Application::HotkeyAction::ToggleFastForward;
		if (action == "endEmulation")
			return Application::HotkeyAction::EndEmulation;
		if (action == "exitApplication")
			return Application::HotkeyAction::ExitApplication;
		throw std::invalid_argument("unknown hotkey action");
	}

	std::string_view HotkeySettingsErrorName(Application::HotkeySettingsError error)
	{
		switch (error)
		{
		case Application::HotkeySettingsError::Conflict:
			return "conflict";
		case Application::HotkeySettingsError::InvalidBinding:
			return "invalidBinding";
		case Application::HotkeySettingsError::DuplicateBinding:
			return "duplicateBinding";
		case Application::HotkeySettingsError::SaveFailed:
			return "saveFailed";
		default:
			return "none";
		}
	}

	const rapidjson::Value& RequiredMember(const rapidjson::Value& object,
										   const char* name)
	{
		if (!object.IsObject())
			throw std::invalid_argument("RPC parameters must be an object");
		const auto member = object.FindMember(name);
		if (member == object.MemberEnd())
			throw std::invalid_argument(std::string(name) + " is required");
		return member->value;
	}

	std::string_view RequiredString(const rapidjson::Value& object, const char* name)
	{
		const auto& value = RequiredMember(object, name);
		if (!value.IsString())
			throw std::invalid_argument(std::string(name) + " must be a string");
		return {value.GetString(), value.GetStringLength()};
	}

	std::uint32_t RequiredUint(const rapidjson::Value& object, const char* name)
	{
		const auto& value = RequiredMember(object, name);
		if (!value.IsUint())
			throw std::invalid_argument(std::string(name) + " must be an unsigned integer");
		return value.GetUint();
	}

	std::uint64_t RequiredUint64(const rapidjson::Value& object, const char* name)
	{
		const auto& value = RequiredMember(object, name);
		if (!value.IsUint64())
			throw std::invalid_argument(std::string(name) + " must be an unsigned integer");
		return value.GetUint64();
	}

	std::uint32_t RequiredHexAddress(const rapidjson::Value& object, const char* name)
	{
		const auto text = RequiredString(object, name);
		if (text.size() != 8)
			throw std::invalid_argument(std::string(name) + " must be an 8-digit hexadecimal address");
		std::uint32_t value{};
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
		if (error != std::errc{} || end != text.data() + text.size())
			throw std::invalid_argument(std::string(name) + " must be an 8-digit hexadecimal address");
		return value;
	}

	std::uint64_t RequiredHexIdentity(const rapidjson::Value& object, const char* name)
	{
		const auto text = RequiredString(object, name);
		if (text.size() != 16)
			throw std::invalid_argument(std::string(name) + " must be a 16-digit hexadecimal identity");
		std::uint64_t value{};
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
		if (error != std::errc{} || end != text.data() + text.size())
			throw std::invalid_argument(std::string(name) + " must be a 16-digit hexadecimal identity");
		return value;
	}

	std::uint32_t RequiredBoundedUint(const rapidjson::Value& object, const char* name,
									  std::uint32_t minimum, std::uint32_t maximum)
	{
		const auto value = RequiredUint(object, name);
		if (value < minimum || value > maximum)
			throw std::invalid_argument(std::string(name) + " is outside the supported range");
		return value;
	}

	bool RequiredBool(const rapidjson::Value& object, const char* name)
	{
		const auto& value = RequiredMember(object, name);
		if (!value.IsBool())
			throw std::invalid_argument(std::string(name) + " must be a boolean");
		return value.GetBool();
	}

	double RequiredDouble(const rapidjson::Value& object, const char* name)
	{
		const auto& value = RequiredMember(object, name);
		if (!value.IsNumber() || !std::isfinite(value.GetDouble()))
			throw std::invalid_argument(std::string(name) + " must be a finite number");
		return value.GetDouble();
	}

	Application::MemoryValueType ParseMemoryValueType(std::string_view type)
	{
		if (type == "int8")
			return Application::MemoryValueType::Int8;
		if (type == "int16")
			return Application::MemoryValueType::Int16;
		if (type == "int32")
			return Application::MemoryValueType::Int32;
		if (type == "int64")
			return Application::MemoryValueType::Int64;
		if (type == "float32")
			return Application::MemoryValueType::Float32;
		if (type == "float64")
			return Application::MemoryValueType::Float64;
		throw std::invalid_argument("unknown memory value type");
	}

	std::string_view MemoryValueTypeName(Application::MemoryValueType type)
	{
		switch (type)
		{
		case Application::MemoryValueType::Int8:
			return "int8";
		case Application::MemoryValueType::Int16:
			return "int16";
		case Application::MemoryValueType::Int32:
			return "int32";
		case Application::MemoryValueType::Int64:
			return "int64";
		case Application::MemoryValueType::Float32:
			return "float32";
		case Application::MemoryValueType::Float64:
			return "float64";
		}
		throw std::invalid_argument("unknown memory value type");
	}

	template<typename T>
	T ParseMemoryInteger(std::string_view text)
	{
		T value{};
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
		if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
			throw std::invalid_argument("memory value is outside the selected integer type");
		return value;
	}

	Application::MemorySearchValue ParseMemoryValue(const rapidjson::Value& object)
	{
		const auto type = ParseMemoryValueType(RequiredString(object, "type"));
		const auto text = RequiredString(object, "text");
		switch (type)
		{
		case Application::MemoryValueType::Int8:
			return {type, ParseMemoryInteger<std::int8_t>(text)};
		case Application::MemoryValueType::Int16:
			return {type, ParseMemoryInteger<std::int16_t>(text)};
		case Application::MemoryValueType::Int32:
			return {type, ParseMemoryInteger<std::int32_t>(text)};
		case Application::MemoryValueType::Int64:
			return {type, ParseMemoryInteger<std::int64_t>(text)};
		case Application::MemoryValueType::Float32:
		case Application::MemoryValueType::Float64:
		{
			std::string owned(text);
			char* end{};
			errno = 0;
			const auto value = std::strtod(owned.c_str(), &end);
			if (end != owned.c_str() + owned.size() || errno == ERANGE || !std::isfinite(value))
				throw std::invalid_argument("memory value must be a finite number");
			if (type == Application::MemoryValueType::Float32)
			{
				const auto narrowed = static_cast<float>(value);
				if (!std::isfinite(narrowed))
					throw std::invalid_argument("memory value is outside the float32 range");
				return {type, narrowed};
			}
			return {type, value};
		}
		}
		throw std::invalid_argument("unknown memory value type");
	}

	std::string MemoryValueText(const Application::MemorySearchValue& value)
	{
		return std::visit([](auto scalar) {
			using T = decltype(scalar);
			if constexpr (std::is_same_v<T, std::int8_t>)
				return std::to_string(static_cast<int>(scalar));
			else if constexpr (std::is_floating_point_v<T>)
				return fmt::format("{:.{}g}", scalar, std::numeric_limits<T>::max_digits10);
			else
				return std::to_string(scalar);
		},
						  value.value);
	}

	std::string MemorySessionJson(const Application::MemorySearchSessionInfo& info)
	{
		return std::string(R"({"sessionToken":)") + JsonString(info.sessionToken) +
			   R"(,"generation":")" + std::to_string(info.generation) +
			   R"(,"mapGeneration":")" + std::to_string(info.mapGeneration) +
			   R"(","bytesTotal":)" + std::to_string(info.bytesTotal) + "}";
	}

	std::string_view MemoryStateName(Application::MemorySearchState state)
	{
		switch (state)
		{
		case Application::MemorySearchState::Scanning:
			return "scanning";
		case Application::MemorySearchState::Complete:
			return "complete";
		case Application::MemorySearchState::Cancelled:
			return "cancelled";
		case Application::MemorySearchState::Failed:
			return "failed";
		}
		return "failed";
	}

	std::string MemoryStatusJson(const Application::MemorySearchStatus& status)
	{
		return std::string(R"({"generation":")") + std::to_string(status.generation) +
			   R"(","state":)" + JsonString(MemoryStateName(status.state)) +
			   R"(,"bytesScanned":)" + std::to_string(status.bytesScanned) +
			   R"(,"bytesTotal":)" + std::to_string(status.bytesTotal) +
			   R"(,"resultCount":)" + std::to_string(status.resultCount) +
			   R"(,"resultCapReached":)" + (status.resultCapReached ? "true" : "false") +
			   R"(,"scanCapReached":)" + (status.scanCapReached ? "true" : "false") +
			   R"(,"diagnostic":)" + JsonString(status.diagnostic) + "}";
	}

	std::string MemoryPageJson(const Application::MemorySearchPage& page)
	{
		rapidjson::StringBuffer buffer;
		JsonWriter writer(buffer);
		writer.StartObject();
		writer.Key("generation");
		const auto generation = std::to_string(page.generation);
		writer.String(generation.data(), generation.size());
		writer.Key("offset");
		writer.Uint(page.offset);
		writer.Key("total");
		writer.Uint(page.total);
		writer.Key("results");
		writer.StartArray();
		for (const auto& result : page.results)
		{
			writer.StartObject();
			writer.Key("address");
			writer.StartObject();
			writer.Key("space");
			writer.String("wiiu-virtual");
			const auto address = fmt::format("0x{:08X}", result.address.value);
			writer.Key("value");
			writer.String(address.data(), address.size());
			writer.EndObject();
			writer.Key("value");
			writer.StartObject();
			const auto type = MemoryValueTypeName(result.value.type);
			writer.Key("type");
			writer.String(type.data(), type.size());
			const auto text = MemoryValueText(result.value);
			writer.Key("text");
			writer.String(text.data(), text.size());
			writer.EndObject();
			writer.EndObject();
		}
		writer.EndArray();
		writer.EndObject();
		return {buffer.GetString(), buffer.GetSize()};
	}

	std::string PpcDebuggerSnapshotJson(const Application::PpcDebuggerSnapshot& snapshot)
	{
		rapidjson::StringBuffer buffer;
		JsonWriter writer(buffer);
		auto address = [](std::uint32_t value) {
			return fmt::format("{:08X}", value);
		};
		writer.StartObject();
		writer.Key("generation");
		const auto generation = std::to_string(snapshot.generation);
		writer.String(generation.data(), generation.size());
		writer.Key("available");
		writer.Bool(snapshot.available);
		writer.Key("trapped");
		writer.Bool(snapshot.trapped);
		writer.Key("instructionPointer");
		const auto ip = address(snapshot.instructionPointer.value);
		writer.String(ip.data(), ip.size());
		writer.Key("linkRegister");
		const auto lr = address(snapshot.linkRegister);
		writer.String(lr.data(), lr.size());
		writer.Key("gpr");
		writer.StartArray();
		for (const auto value : snapshot.gpr)
		{
			const auto formatted = address(value);
			writer.String(formatted.data(), formatted.size());
		}
		writer.EndArray();
		writer.Key("instructions");
		writer.StartArray();
		for (const auto& instruction : snapshot.instructions)
		{
			writer.StartObject();
			const auto formattedAddress = address(instruction.address.value);
			writer.Key("address");
			writer.String(formattedAddress.data(), formattedAddress.size());
			const auto opcode = address(instruction.opcode);
			writer.Key("opcode");
			writer.String(opcode.data(), opcode.size());
			writer.Key("mnemonic");
			writer.String(instruction.mnemonic.data(), instruction.mnemonic.size());
			writer.Key("operands");
			writer.String(instruction.operands.data(), instruction.operands.size());
			writer.Key("current");
			writer.Bool(instruction.current);
			writer.Key("breakpoint");
			writer.Bool(instruction.breakpoint);
			writer.EndObject();
		}
		writer.EndArray();
		writer.Key("breakpoints");
		writer.StartArray();
		for (const auto& breakpoint : snapshot.breakpoints)
		{
			writer.StartObject();
			writer.Key("identity");
			writer.String(breakpoint.identity.data(), breakpoint.identity.size());
			const auto formattedAddress = address(breakpoint.address.value);
			writer.Key("address");
			writer.String(formattedAddress.data(), formattedAddress.size());
			writer.Key("enabled");
			writer.Bool(breakpoint.enabled);
			writer.Key("logging");
			writer.Bool(breakpoint.logging);
			writer.EndObject();
		}
		writer.EndArray();
		writer.Key("breakpointCapReached");
		writer.Bool(snapshot.breakpointCapReached);
		writer.Key("diagnostic");
		writer.String(snapshot.diagnostic.data(), snapshot.diagnostic.size());
		writer.EndObject();
		return {buffer.GetString(), buffer.GetSize()};
	}

	struct WindowDescriptor
	{
		std::string_view role;
		std::string_view title;
		int width;
		int height;
		bool modal;
	};

	enum class UiTheme
	{
		Light,
		Dark,
	};

	constexpr std::string_view UiThemeName(UiTheme theme)
	{
		return theme == UiTheme::Dark ? "dark" : "light";
	}

	constexpr std::string_view PlatformName()
	{
#if BOOST_OS_WINDOWS
		return "windows";
#elif BOOST_OS_MACOS
		return "macos";
#else
		return "linux";
#endif
	}

	constexpr std::array WindowDescriptors{
		WindowDescriptor{"cemod-permissions", "CemuExtend Permissions", 760, 620, true},
	};

	const WindowDescriptor& DescribeWindow(std::string_view role)
	{
		const auto found = std::ranges::find(WindowDescriptors, role, &WindowDescriptor::role);
		if (found == WindowDescriptors.end())
			throw std::invalid_argument("unknown window role");
		return *found;
	}

	std::uint64_t ParseTitleId(const rapidjson::Value& params)
	{
		const auto found = params.FindMember("titleId");
		if (found == params.MemberEnd() || !found->value.IsString() ||
			found->value.GetStringLength() != 16)
			throw std::invalid_argument("titleId must contain exactly 16 hexadecimal digits");
		std::uint64_t value{};
		const std::string_view text(found->value.GetString(), found->value.GetStringLength());
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
			throw std::invalid_argument("titleId must contain exactly 16 hexadecimal digits");
		return value;
	}

	std::uint64_t ParseOpaqueUid(const rapidjson::Value& params)
	{
		const auto text = RequiredString(params, "locationUid");
		std::uint64_t value{};
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
		if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
			throw std::invalid_argument("locationUid must be an unsigned decimal string");
		return value;
	}

	std::uint32_t ParsePersistentId(const rapidjson::Value& params,
									const char* field = "persistentId")
	{
		const auto text = RequiredString(params, field);
		if (text.size() != 8)
			throw std::invalid_argument(std::string(field) + " must contain exactly 8 hexadecimal digits");
		std::uint32_t value{};
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
			value < Application::kMinimumPersistentId)
			throw std::invalid_argument(std::string(field) + " is not a supported persistent id");
		return value;
	}

	std::string PersistentIdString(std::uint32_t persistentId)
	{
		std::array<char, 9> text{};
		std::snprintf(text.data(), text.size(), "%08x", persistentId);
		return text.data();
	}

	std::string_view SaveStateName(Application::SaveEntryState state)
	{
		switch (state)
		{
		case Application::SaveEntryState::Directory:
			return "directory";
		case Application::SaveEntryState::NonDirectory:
			return "nonDirectory";
		default:
			return "missing";
		}
	}

	std::string_view SaveErrorName(Application::SaveOperationError error)
	{
		switch (error)
		{
		case Application::SaveOperationError::None:
			return "none";
		case Application::SaveOperationError::InvalidPersistentId:
			return "invalidPersistentId";
		case Application::SaveOperationError::TitleRunning:
			return "titleRunning";
		case Application::SaveOperationError::Scanning:
			return "scanning";
		case Application::SaveOperationError::NotFound:
			return "notFound";
		case Application::SaveOperationError::TargetExists:
			return "targetExists";
		case Application::SaveOperationError::InvalidTarget:
			return "invalidTarget";
		case Application::SaveOperationError::ArchiveInvalid:
			return "archiveInvalid";
		case Application::SaveOperationError::PathUnsafe:
			return "pathUnsafe";
		case Application::SaveOperationError::Cancelled:
			return "cancelled";
		case Application::SaveOperationError::IoFailure:
			return "ioFailure";
		case Application::SaveOperationError::MetadataFailure:
			return "metadataFailure";
		default:
			return "backendFailure";
		}
	}

	std::uint16_t UsbHidUsage(std::uint32_t key)
	{
		if (key >= 'A' && key <= 'Z')
			return static_cast<std::uint16_t>(0x04 + key - 'A');
		if (key >= 'a' && key <= 'z')
			return static_cast<std::uint16_t>(0x04 + key - 'a');
		if (key >= '1' && key <= '9')
			return static_cast<std::uint16_t>(0x1e + key - '1');
		if (key == '0')
			return 0x27;
		switch (key)
		{
		case '-':
			return 0x2d;
		case '=':
			return 0x2e;
		case '[':
			return 0x2f;
		case ']':
			return 0x30;
		case '\\':
			return 0x31;
		case ';':
			return 0x33;
		case '\'':
			return 0x34;
		case '`':
			return 0x35;
		case ',':
			return 0x36;
		case '.':
			return 0x37;
		case '/':
			return 0x38;
		default:
			break;
		}
#if BOOST_OS_WINDOWS
		switch (key)
		{
		case VK_RETURN:
			return 0x28;
		case VK_ESCAPE:
			return 0x29;
		case VK_BACK:
			return 0x2a;
		case VK_TAB:
			return 0x2b;
		case VK_SPACE:
			return 0x2c;
		case VK_CAPITAL:
			return 0x39;
		case VK_F1:
			return 0x3a;
		case VK_F2:
			return 0x3b;
		case VK_F3:
			return 0x3c;
		case VK_F4:
			return 0x3d;
		case VK_F5:
			return 0x3e;
		case VK_F6:
			return 0x3f;
		case VK_F7:
			return 0x40;
		case VK_F8:
			return 0x41;
		case VK_F9:
			return 0x42;
		case VK_F10:
			return 0x43;
		case VK_F11:
			return 0x44;
		case VK_F12:
			return 0x45;
		case VK_F13:
			return 0x68;
		case VK_F14:
			return 0x69;
		case VK_F15:
			return 0x6a;
		case VK_F16:
			return 0x6b;
		case VK_F17:
			return 0x6c;
		case VK_F18:
			return 0x6d;
		case VK_F19:
			return 0x6e;
		case VK_F20:
			return 0x6f;
		case VK_F21:
			return 0x70;
		case VK_F22:
			return 0x71;
		case VK_F23:
			return 0x72;
		case VK_F24:
			return 0x73;
		case VK_SNAPSHOT:
			return 0x46;
		case VK_SCROLL:
			return 0x47;
		case VK_PAUSE:
			return 0x48;
		case VK_INSERT:
			return 0x49;
		case VK_HOME:
			return 0x4a;
		case VK_PRIOR:
			return 0x4b;
		case VK_DELETE:
			return 0x4c;
		case VK_END:
			return 0x4d;
		case VK_NEXT:
			return 0x4e;
		case VK_RIGHT:
			return 0x4f;
		case VK_LEFT:
			return 0x50;
		case VK_DOWN:
			return 0x51;
		case VK_UP:
			return 0x52;
		case VK_NUMLOCK:
			return 0x53;
		case VK_DIVIDE:
			return 0x54;
		case VK_MULTIPLY:
			return 0x55;
		case VK_SUBTRACT:
			return 0x56;
		case VK_ADD:
			return 0x57;
		case VK_NUMPAD1:
			return 0x59;
		case VK_NUMPAD2:
			return 0x5a;
		case VK_NUMPAD3:
			return 0x5b;
		case VK_NUMPAD4:
			return 0x5c;
		case VK_NUMPAD5:
			return 0x5d;
		case VK_NUMPAD6:
			return 0x5e;
		case VK_NUMPAD7:
			return 0x5f;
		case VK_NUMPAD8:
			return 0x60;
		case VK_NUMPAD9:
			return 0x61;
		case VK_NUMPAD0:
			return 0x62;
		case VK_DECIMAL:
			return 0x63;
		case VK_APPS:
			return 0x65;
		case VK_OEM_MINUS:
			return 0x2d;
		case VK_OEM_PLUS:
			return 0x2e;
		case VK_OEM_4:
			return 0x2f;
		case VK_OEM_6:
			return 0x30;
		case VK_OEM_5:
			return 0x31;
		case VK_OEM_1:
			return 0x33;
		case VK_OEM_7:
			return 0x34;
		case VK_OEM_3:
			return 0x35;
		case VK_OEM_COMMA:
			return 0x36;
		case VK_OEM_PERIOD:
			return 0x37;
		case VK_OEM_2:
			return 0x38;
		case VK_LCONTROL:
			return 0xe0;
		case VK_LSHIFT:
			return 0xe1;
		case VK_LMENU:
			return 0xe2;
		case VK_LWIN:
			return 0xe3;
		case VK_RCONTROL:
			return 0xe4;
		case VK_RSHIFT:
			return 0xe5;
		case VK_RMENU:
			return 0xe6;
		case VK_RWIN:
			return 0xe7;
		default:
			return 0;
		}
#elif BOOST_OS_MACOS
		switch (key)
		{
		case 0x00:
			return 0x04;
		case 0x0b:
			return 0x05;
		case 0x08:
			return 0x06;
		case 0x02:
			return 0x07;
		case 0x0e:
			return 0x08;
		case 0x03:
			return 0x09;
		case 0x05:
			return 0x0a;
		case 0x04:
			return 0x0b;
		case 0x22:
			return 0x0c;
		case 0x26:
			return 0x0d;
		case 0x28:
			return 0x0e;
		case 0x25:
			return 0x0f;
		case 0x2e:
			return 0x10;
		case 0x2d:
			return 0x11;
		case 0x1f:
			return 0x12;
		case 0x23:
			return 0x13;
		case 0x0c:
			return 0x14;
		case 0x0f:
			return 0x15;
		case 0x01:
			return 0x16;
		case 0x11:
			return 0x17;
		case 0x20:
			return 0x18;
		case 0x09:
			return 0x19;
		case 0x0d:
			return 0x1a;
		case 0x07:
			return 0x1b;
		case 0x10:
			return 0x1c;
		case 0x06:
			return 0x1d;
		case 0x12:
			return 0x1e;
		case 0x13:
			return 0x1f;
		case 0x14:
			return 0x20;
		case 0x15:
			return 0x21;
		case 0x17:
			return 0x22;
		case 0x16:
			return 0x23;
		case 0x1a:
			return 0x24;
		case 0x1c:
			return 0x25;
		case 0x19:
			return 0x26;
		case 0x1d:
			return 0x27;
		case 0x1b:
			return 0x2d;
		case 0x18:
			return 0x2e;
		case 0x21:
			return 0x2f;
		case 0x1e:
			return 0x30;
		case 0x2a:
			return 0x31;
		case 0x29:
			return 0x33;
		case 0x27:
			return 0x34;
		case 0x32:
			return 0x35;
		case 0x2b:
			return 0x36;
		case 0x2f:
			return 0x37;
		case 0x2c:
			return 0x38;
		default:
			break;
		}
		static constexpr std::array<std::pair<std::uint32_t, std::uint16_t>, 41> usages{{{0x24, 0x28}, {0x35, 0x29}, {0x33, 0x2a}, {0x30, 0x2b}, {0x31, 0x2c}, {0x7a, 0x3a}, {0x78, 0x3b}, {0x63, 0x3c}, {0x76, 0x3d}, {0x60, 0x3e}, {0x61, 0x3f}, {0x62, 0x40}, {0x64, 0x41}, {0x65, 0x42}, {0x6d, 0x43}, {0x67, 0x44}, {0x6f, 0x45}, {0x69, 0x68}, {0x6b, 0x69}, {0x71, 0x6a}, {0x6a, 0x6b}, {0x40, 0x6c}, {0x4f, 0x6d}, {0x50, 0x6e}, {0x5a, 0x6f}, {0x72, 0x49}, {0x73, 0x4a}, {0x74, 0x4b}, {0x75, 0x4c}, {0x77, 0x4d}, {0x79, 0x4e}, {0x7c, 0x4f}, {0x7b, 0x50}, {0x7d, 0x51}, {0x7e, 0x52}, {0x3b, 0xe0}, {0x38, 0xe1}, {0x3a, 0xe2}, {0x37, 0xe3}, {0x3e, 0xe4}, {0x3c, 0xe5}}};
		for (const auto& [native, usage] : usages)
			if (native == key)
				return usage;
		return 0;
#else
		switch (key)
		{
		case 0xff0d:
			return 0x28;
		case 0xff1b:
			return 0x29;
		case 0xff08:
			return 0x2a;
		case 0xff09:
			return 0x2b;
		case 0x20:
			return 0x2c;
		case 0xffe5:
			return 0x39;
		case 0xffbe:
			return 0x3a;
		case 0xffbf:
			return 0x3b;
		case 0xffc0:
			return 0x3c;
		case 0xffc1:
			return 0x3d;
		case 0xffc2:
			return 0x3e;
		case 0xffc3:
			return 0x3f;
		case 0xffc4:
			return 0x40;
		case 0xffc5:
			return 0x41;
		case 0xffc6:
			return 0x42;
		case 0xffc7:
			return 0x43;
		case 0xffc8:
			return 0x44;
		case 0xffc9:
			return 0x45;
		case 0xffca:
			return 0x68;
		case 0xffcb:
			return 0x69;
		case 0xffcc:
			return 0x6a;
		case 0xffcd:
			return 0x6b;
		case 0xffce:
			return 0x6c;
		case 0xffcf:
			return 0x6d;
		case 0xffd0:
			return 0x6e;
		case 0xffd1:
			return 0x6f;
		case 0xffd2:
			return 0x70;
		case 0xffd3:
			return 0x71;
		case 0xffd4:
			return 0x72;
		case 0xffd5:
			return 0x73;
		case 0xff63:
			return 0x49;
		case 0xff50:
			return 0x4a;
		case 0xff55:
			return 0x4b;
		case 0xffff:
			return 0x4c;
		case 0xff57:
			return 0x4d;
		case 0xff56:
			return 0x4e;
		case 0xff53:
			return 0x4f;
		case 0xff51:
			return 0x50;
		case 0xff54:
			return 0x51;
		case 0xff52:
			return 0x52;
		case 0xffe3:
			return 0xe0;
		case 0xffe1:
			return 0xe1;
		case 0xffe9:
			return 0xe2;
		case 0xffeb:
			return 0xe3;
		case 0xffe4:
			return 0xe4;
		case 0xffe2:
			return 0xe5;
		case 0xffea:
			return 0xe6;
		case 0xffec:
			return 0xe7;
		default:
			return 0;
		}
#endif
	}

	std::vector<std::uint32_t> DecodeUtf8(std::string_view text)
	{
		std::vector<std::uint32_t> result;
		for (std::size_t i = 0; i < text.size();)
		{
			const auto first = static_cast<unsigned char>(text[i++]);
			std::uint32_t codepoint{};
			std::size_t continuation{};
			if (first < 0x80)
				codepoint = first;
			else if ((first & 0xe0) == 0xc0)
			{
				codepoint = first & 0x1f;
				continuation = 1;
			}
			else if ((first & 0xf0) == 0xe0)
			{
				codepoint = first & 0x0f;
				continuation = 2;
			}
			else if ((first & 0xf8) == 0xf0)
			{
				codepoint = first & 0x07;
				continuation = 3;
			}
			else
				continue;
			if (i + continuation > text.size())
				break;
			bool valid = true;
			for (std::size_t offset = 0; offset < continuation; ++offset)
			{
				const auto byte = static_cast<unsigned char>(text[i++]);
				if ((byte & 0xc0) != 0x80)
				{
					valid = false;
					break;
				}
				codepoint = (codepoint << 6) | (byte & 0x3f);
			}
			if (valid && codepoint <= 0x10ffff && !(codepoint >= 0xd800 && codepoint <= 0xdfff))
				result.push_back(codepoint);
		}
		return result;
	}

	std::string RuntimeOverlayJson(const RuntimeOverlay::Snapshot& snapshot, bool resumeRequired = false,
								   std::uint64_t focusRevision = 0)
	{
		rapidjson::StringBuffer buffer;
		JsonWriter writer(buffer);
		const auto now = std::chrono::steady_clock::now();
		auto writeStyle = [&writer](const RuntimeOverlay::TextStyle& style) {
			writer.StartObject();
			writer.Key("position");
			writer.String(RuntimeOverlay::PositionName(style.position).data());
			writer.Key("color");
			writer.Uint(style.color);
			writer.Key("scale");
			writer.Uint(style.scale);
			writer.EndObject();
		};
		writer.StartObject();
		writer.Key("sequence");
		writer.String(std::to_string(snapshot.sequence).c_str());
		writer.Key("resumeRequired");
		writer.Bool(resumeRequired);
		writer.Key("focusRevision");
		writer.String(std::to_string(focusRevision).c_str());
		writer.Key("overlayStyle");
		writeStyle(snapshot.overlayStyle);
		writer.Key("notificationStyle");
		writeStyle(snapshot.notificationStyle);
		writer.Key("visibility");
		writer.StartObject();
		writer.Key("fps");
		writer.Bool(snapshot.visibility.fps);
		writer.Key("drawCalls");
		writer.Bool(snapshot.visibility.drawCalls);
		writer.Key("cpuUsage");
		writer.Bool(snapshot.visibility.cpuUsage);
		writer.Key("cpuPerCore");
		writer.Bool(snapshot.visibility.cpuPerCore);
		writer.Key("ramUsage");
		writer.Bool(snapshot.visibility.ramUsage);
		writer.Key("vramUsage");
		writer.Bool(snapshot.visibility.vramUsage);
		writer.Key("debug");
		writer.Bool(snapshot.visibility.debug);
		writer.EndObject();
		writer.Key("stats");
		writer.StartObject();
		writer.Key("fps");
		writer.Double(snapshot.stats.fps);
		writer.Key("drawCalls");
		writer.Uint(snapshot.stats.drawCalls);
		writer.Key("fastDrawCalls");
		writer.Uint(snapshot.stats.fastDrawCalls);
		writer.Key("cpuUsage");
		writer.Double(snapshot.stats.cpuUsage);
		writer.Key("cpuPerCore");
		writer.StartArray();
		for (const auto value : snapshot.stats.cpuPerCore)
			writer.Double(value);
		writer.EndArray();
		writer.Key("ramUsageMb");
		writer.Uint(snapshot.stats.ramUsageMb);
		writer.Key("vramUsageMb");
		writer.Int(snapshot.stats.vramUsageMb);
		writer.Key("vramTotalMb");
		writer.Int(snapshot.stats.vramTotalMb);
		writer.Key("debugLines");
		writer.StartArray();
		for (const auto& [label, value] : snapshot.stats.debugLines)
		{
			writer.StartObject();
			writer.Key("label");
			writer.String(label.data(), label.size());
			writer.Key("value");
			writer.String(value.data(), value.size());
			writer.EndObject();
		}
		writer.EndArray();
		writer.EndObject();
		writer.Key("notices");
		writer.StartArray();
		for (const auto& notice : snapshot.notices)
		{
			writer.StartObject();
			writer.Key("id");
			writer.String(std::to_string(notice.id).c_str());
			writer.Key("kind");
			writer.String(RuntimeOverlay::NoticeKindName(notice.kind).data());
			writer.Key("text");
			writer.String(notice.text.data(), notice.text.size());
			if (notice.player)
			{
				writer.Key("player");
				writer.Uint(*notice.player);
			}
			writer.Key("remainingMs");
			writer.Uint64(notice.expiresAt == std::chrono::steady_clock::time_point{}
							  ? 0
							  : static_cast<std::uint64_t>(std::max<std::int64_t>(1,
																				  std::chrono::duration_cast<std::chrono::milliseconds>(notice.expiresAt - now).count())));
			writer.EndObject();
		}
		writer.EndArray();
		writer.Key("shaderProgress");
		writer.StartObject();
		writer.Key("generation");
		writer.String(std::to_string(snapshot.shaderProgress.generation).c_str());
		writer.Key("visible");
		writer.Bool(snapshot.shaderProgress.visible);
		writer.Key("pipelines");
		writer.Bool(snapshot.shaderProgress.pipelines);
		writer.Key("current");
		writer.Uint(snapshot.shaderProgress.current);
		writer.Key("total");
		writer.Uint(snapshot.shaderProgress.total);
		writer.Key("vertexShaders");
		writer.Uint(snapshot.shaderProgress.vertexShaders);
		writer.Key("pixelShaders");
		writer.Uint(snapshot.shaderProgress.pixelShaders);
		writer.Key("geometryShaders");
		writer.Uint(snapshot.shaderProgress.geometryShaders);
		writer.Key("backgroundImageAvailable");
		writer.Bool(snapshot.shaderProgress.backgroundImageTv ||
					snapshot.shaderProgress.backgroundImagePad);
		writer.EndObject();
		writer.Key("keyboard");
		writer.StartObject();
		writer.Key("generation");
		writer.String(std::to_string(snapshot.keyboard.generation).c_str());
		writer.Key("active");
		writer.Bool(snapshot.keyboard.active);
		writer.Key("keyboardOnly");
		writer.Bool(snapshot.keyboard.keyboardOnly);
		writer.Key("shifted");
		writer.Bool(snapshot.keyboard.shifted);
		writer.Key("maximumLength");
		writer.Uint(snapshot.keyboard.maximumLength);
		writer.Key("text");
		writer.String(snapshot.keyboard.text.data(), snapshot.keyboard.text.size());
		writer.EndObject();
		writer.Key("errorDialog");
		writer.StartObject();
		writer.Key("generation");
		writer.String(std::to_string(snapshot.errorDialog.generation).c_str());
		writer.Key("active");
		writer.Bool(snapshot.errorDialog.active);
		writer.Key("title");
		writer.String(snapshot.errorDialog.title.data(), snapshot.errorDialog.title.size());
		writer.Key("message");
		writer.String(snapshot.errorDialog.message.data(), snapshot.errorDialog.message.size());
		writer.Key("leftButton");
		writer.String(snapshot.errorDialog.leftButton.data(), snapshot.errorDialog.leftButton.size());
		writer.Key("rightButton");
		writer.String(snapshot.errorDialog.rightButton.data(), snapshot.errorDialog.rightButton.size());
		writer.Key("opacity");
		writer.Double(snapshot.errorDialog.opacity);
		writer.EndObject();
		writer.Key("interaction");
		switch (snapshot.interaction)
		{
		case RuntimeOverlay::Interaction::SoftwareKeyboard:
			writer.String("softwareKeyboard");
			break;
		case RuntimeOverlay::Interaction::ErrorDialog:
			writer.String("errorDialog");
			break;
		default:
			writer.String("passive");
			break;
		}
		writer.EndObject();
		return {buffer.GetString(), buffer.GetSize()};
	}

	class Runtime final
	{
	  public:
		Runtime()
		{
			m_language = NormalizeUiLanguage(GetConfig().frontend.ui_language.GetValue());
			try
			{
#if !defined(CEMU_OVERLAY_BACKEND_CEF)
				throw std::runtime_error("the React frontend requires the CEF backend");
#else
				if (!WebFrontend::CefNative::InitializeNativeUiLoop())
					throw std::runtime_error("failed to initialize the native CEF UI loop");
				m_nativeUiLoopInitialized = true;
				if (!WebFrontend::CefOverlay::InitializeProcessRuntime())
					throw std::runtime_error("failed to initialize the CEF process runtime");
				// Chromium installs its own handlers for the signals that dump core,
				// replacing the ones startup put in place. Losing those means a crash
				// writes no stack trace to the log at all, so claim them back now that
				// CEF has had its turn.
				ExceptionHandler_Init();
				// On Linux CEF must initialize before GTK. CefInitialize configures
				// Chromium's GTK integration (including locale handling), while the
				// native host constructor calls gtk_init_check and opens the display.
				// Both operations still run on this frontend UI thread.
				m_nativeWindow = CreateNativeWindowHost();
#endif
				m_nativeWindow->SetCloseHandler([this] { HandleLauncherClose(); });
				m_nativeWindow->SetGameCloseHandler([this] { HandleGameWindowClose(); });
				m_nativeWindow->SetMetricsHandler(
					[this](Host::WindowMetricsSnapshot metrics) { HandleMetrics(metrics); });
				m_nativeWindow->SetInputHandler(
					[this](const WebFrontend::NativeInputEvent& event) { HandleNativeInput(event); });
				m_nativeWindow->SetPadCloseHandler([this] {
					const auto expectedGeneration = m_padGeneration;
					PostToUi([this, expectedGeneration] {
						(void)ClosePadRenderRegion(expectedGeneration);
					});
				});
				m_mainWindowPublication = m_hostState->PublishMainWindow(
					m_nativeWindow->GetMainWindowHandle());
#if defined(CEMU_OVERLAY_BACKEND_CEF)
				m_cefOverlay = WebFrontend::CefOverlay::CreateBrowserRuntime(
					[this](std::uint64_t windowId, std::string_view request) {
						return DispatchCefRpc(windowId, request);
					},
					[this](Host::PointerSurface surface) {
						if (m_nativeWindow)
							m_nativeWindow->RequestRenderRedraw(surface);
					},
					{},
					[this](std::uint64_t windowId) { HandleCefWindowClosed(windowId); },
					[this](Host::PointerSurface surface,
						   const WebFrontend::CefOverlay::InputOwnership& ownership) {
						const auto index = surface == Host::PointerSurface::Main ? 0U : 1U;
						m_surfaceInputOwnership[index] = ownership;
						if (m_managedInputFocus)
						{
							PublishInputOwnership(Host::PointerSurface::Main,
												  WebFrontend::CefOverlay::MainStreamOwnership(m_inputFocus.Window(),
																							   m_surfaceInputOwnership[0], m_surfaceInputOwnership[1]));
							if (surface == Host::PointerSurface::Pad)
								PublishInputOwnership(surface, ownership);
						}
						else
							PublishInputOwnership(surface, ownership);
						m_webUiPointerOwnership[surface == Host::PointerSurface::Main ? 0 : 1] =
							ownership.pointer == WebFrontend::CefOverlay::InputOwner::WebUi;
						(void)RefreshPointerPolicy(surface);
					});
				if (!m_cefOverlay)
					throw std::runtime_error("failed to create the shared CEF browser runtime");
				m_cemodWebUi = WebFrontend::CemodWebUiFrontend::Create(
					m_nativeWindow->GetNativeWindow(), m_cefOverlay,
					[this](std::function<void()> action) { return PostToUi(std::move(action)); });
				if (!m_cemodWebUi)
					throw std::runtime_error("failed to create the Cemod Web UI frontend");
#endif
				m_rendererHost = CreateRendererHost(
					m_hostState, m_hostState, m_hostState, m_cefOverlay,
					[gate = m_callbackGate](bool mainWindow) {
						std::scoped_lock lock(gate->mutex);
						if (gate->target)
							gate->target->SignalFramePresented(mainWindow);
					});
				m_hostServices = std::make_shared<WebHostServices>(
					m_hostState, *m_nativeWindow,
					[this](std::function<void()> action) { return PostToUi(std::move(action)); },
					[this] { return RecreateCanvasForHost(); },
					[this](bool fullscreen) {
						m_fullscreen = fullscreen;
						m_nativeWindow->SetFullscreen(fullscreen);
					});
				RefreshHotkeyBindings(m_controller.GetHotkeySettings());
				m_hostServices->SetControllerObserver(
					[this](const ControllerState& current, const ControllerState& previous) {
						HandleControllerHotkeys(current, previous);
					});
				Application::ConnectHost({
					.windowMetrics = m_hostServices,
					.windowControl = m_hostServices,
					.clipboard = m_hostServices,
					.externalLauncher = m_hostServices,
					.inputFocus = m_hostServices,
					.canvas = m_hostServices,
					.cemodWebUi = m_cemodWebUi,
				});
				InputManager::instance().ConfigureHost(*m_hostServices, *m_hostServices,
													   *m_hostServices, *m_hostServices);
				IAudioAPI::ConfigureNativeSurfaceProvider(m_hostServices.get());
				m_hostConnected = true;
				m_windowState = std::make_unique<MainWindowState>(reinterpret_cast<std::uintptr_t>(
					m_nativeWindow->GetNativeWindow()));
				m_callbackGate->target = this;
#if defined(CEMU_OVERLAY_BACKEND_CEF)
				RuntimeOverlay::Model::Instance().SetChangeHandler([gate = m_callbackGate] {
					std::scoped_lock lock(gate->mutex);
					if (gate->target)
						gate->target->SignalRuntimeOverlayChanged();
				});
				// Anything the model published while no handler was attached never
				// reached the overlay, so start from the current state rather than
				// whatever the previous session left on screen.
				SignalRuntimeOverlayChanged();
#endif
				m_emulatedUsb.SetObserver([gate = m_callbackGate](const Application::UsbDeviceChange& change) {
					std::scoped_lock lock(gate->mutex);
					if (!gate->target)
						return;
					auto* runtime = gate->target;
					const auto payload = runtime->UsbDeviceChangeJson(change);
					(void)runtime->PostToUi([runtime, payload] {
						if (runtime->m_mainWorkspaceRole == "emulated-usb-devices")
							runtime->EmitToWindow(0, "usb.devicesChanged", payload);
						else if (const auto window = runtime->m_windowByRole.find("emulated-usb-devices");
								 window != runtime->m_windowByRole.end())
							runtime->EmitToWindow(window->second, "usb.devicesChanged", payload);
					});
				});
				RegisterRpc();
				m_applicationEvents = m_controller.Events().Subscribe(
					[gate = m_callbackGate](const Application::Event& event) {
						std::scoped_lock lock(gate->mutex);
						if (gate->target)
							gate->target->ForwardEvent(event);
					});
				m_titleEvents = m_controller.SubscribeTitleCatalog(
					[gate = m_callbackGate](const Application::TitleCatalogEvent&) {
						std::scoped_lock lock(gate->mutex);
						if (gate->target)
							gate->target->Emit("titles.changed", "{}");
					});
				m_loggingEvents = m_logging.Subscribe(
					[gate = m_callbackGate](const Application::LoggingEntry&) {
						std::scoped_lock lock(gate->mutex);
						if (gate->target)
							gate->target->SignalLoggingChanged();
					});
#if defined(CEMU_OVERLAY_BACKEND_CEF)
				const auto bounds = m_nativeWindow->GetBrowserBounds();
				WebFrontend::CefOverlay::BrowserDescriptor browser;
				browser.windowId = 0;
				browser.role = "main-library";
				browser.bootstrapJson = BrowserBootstrapJson(0, "main-library");
				browser.contextJson = "{}";
				browser.initialUrl = FrontendUrl();
				browser.presentation = WebFrontend::CefOverlay::BrowserPresentation::NativeChild;
				browser.nativeParent = m_nativeWindow->GetBrowserParentWindow();
				browser.bounds = bounds;
				browser.bounds.width = std::max(browser.bounds.width, 1);
				browser.bounds.height = std::max(browser.bounds.height, 1);
				browser.dpiScale = m_nativeWindow->GetBrowserDpiScale();
				browser.nativeBrowserCreated = [this](void* child) {
					if (m_nativeWindow)
						m_nativeWindow->AttachBrowser(child);
				};
				browser.nativeBrowserClosing = [this](void* child) {
					if (m_nativeWindow)
						m_nativeWindow->DetachBrowser(child);
				};
				if (!m_cefOverlay->CreateBrowser(browser))
					throw std::runtime_error("failed to create the CEF launcher browser");
#endif
			} catch (...)
			{
				Cleanup();
				throw;
			}
		}

		~Runtime()
		{
			Cleanup();
		}

		void Run()
		{
			m_nativeWindow->Show();
			m_nativeWindow->FocusBrowser();
			if (LaunchSettings::GetLoadFile() || LaunchSettings::GetLoadTitleID())
			{
				m_commandLineLaunch = true;
				if (!PostToUi([this] { LaunchFromCommandLine(); }))
				{
					m_exitCode = EXIT_FAILURE;
					return;
				}
			}
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			WebFrontend::CefNative::RunNativeUiLoop();
#endif
		}

		[[nodiscard]] int ExitCode() const noexcept
		{
			return m_exitCode;
		}

	  private:
		struct ToolWindow
		{
			std::uint64_t id{};
			std::uint64_t generation{};
			std::string role;
			std::optional<std::uint64_t> titleContext;
			std::string packageContext;
			std::optional<std::uint64_t> generationContext;
			std::unique_ptr<IToolWindowSupport> nativeSupport;
			bool closeRequested{};
			bool closing{};
			std::shared_ptr<std::atomic_bool> lifetime{
				std::make_shared<std::atomic_bool>(true)};
		};

		struct BackgroundJob
		{
			std::uint64_t id{};
			std::uint64_t ownerWindow{};
			std::uint64_t ownerGeneration{};
			std::weak_ptr<std::atomic_bool> ownerLifetime;
			std::shared_ptr<std::atomic_bool> cancelled;
			std::jthread worker;
		};

		struct BackgroundJobEvent
		{
			std::shared_ptr<RuntimeCallbackGate> gate;
			std::uint64_t jobId{};
			std::uint64_t ownerWindow{};
			std::string type;
			std::string payload;
			bool final{};
		};

		enum class SaveTicketKind
		{
			Import,
			Export,
			Delete,
			Transfer
		};
		struct SaveTicket
		{
			std::uint64_t ownerWindow{};
			SaveTicketKind kind{};
			std::filesystem::path archivePath;
			std::uint64_t titleId{};
			std::uint32_t sourcePersistentId{};
			std::uint32_t targetPersistentId{};
			bool overwrite{};
		};
		struct WuaPlanRecord
		{
			std::uint64_t owner{};
			std::uint64_t titleId{};
			std::uint64_t preferredLocationUid{};
			Application::WuaConversionPlan plan;
		};
		struct InstallPlanRecord
		{
			std::uint64_t owner{};
			Application::TitleInstallPlan plan;
		};
		struct DeletePlanRecord
		{
			std::uint64_t owner{};
			Application::ManagedContentDeletePlan plan;
		};
		struct NativePathRecord
		{
			std::uint64_t owner{};
			fs::path path;
		};
		struct PendingLaunch
		{
			fs::path path;
			std::uint64_t titleId{};
			std::uint64_t ownerWindow{};
			std::uint64_t ownerGeneration{};
			std::uint64_t permissionWindow{};
			std::string packageKey;
			std::uint64_t generation{};
			bool decisionSaved{};
			bool approved{};
		};

		static void PostBackgroundJobEvent(std::shared_ptr<RuntimeCallbackGate> gate,
										   std::uint64_t jobId, std::uint64_t ownerWindow, std::string type,
										   std::string payload, bool final)
		{
			auto event = std::make_shared<BackgroundJobEvent>(BackgroundJobEvent{
				std::move(gate), jobId, ownerWindow, std::move(type), std::move(payload), final});
			std::scoped_lock lock(event->gate->mutex);
			if (!event->gate->target)
				return;
			auto* runtime = event->gate->target;
			(void)runtime->PostToUi([event] {
				std::scoped_lock callbackLock(event->gate->mutex);
				if (event->gate->target)
					event->gate->target->DeliverBackgroundJobEvent(*event);
			});
		}

		void DeliverBackgroundJobEvent(const BackgroundJobEvent& event)
		{
			const auto job = m_backgroundJobs.find(event.jobId);
			if (job == m_backgroundJobs.end() || job->second->ownerWindow != event.ownerWindow)
				return;
			bool ownerAlive = event.ownerWindow == 0;
			if (event.ownerWindow != 0)
			{
				const auto owner = m_toolWindows.find(event.ownerWindow);
				const auto lifetime = job->second->ownerLifetime.lock();
				ownerAlive = owner != m_toolWindows.end() && lifetime &&
							 lifetime->load(std::memory_order_acquire) &&
							 owner->second->generation == job->second->ownerGeneration;
			}
			if (ownerAlive)
				EmitToWindow(event.ownerWindow, event.type, event.payload);
			if (event.final)
				m_backgroundJobs.erase(job);
		}

		std::uint64_t StartGraphicPackInstallJob(
			Application::GraphicPackInstallRequest request)
		{
			if (std::ranges::any_of(m_backgroundJobs,
									[this](const auto& entry) { return entry.second->ownerWindow == m_invokingWindow; }))
				throw std::runtime_error("this window already has a background operation in progress");
			constexpr std::size_t kMaximumBackgroundJobs = 4;
			if (m_backgroundJobs.size() >= kMaximumBackgroundJobs)
				throw std::runtime_error("too many background operations are in progress");
			if (m_nextBackgroundJobId >= 9007199254740991ULL)
				throw std::runtime_error("background job identifier space is exhausted");
			const auto id = ++m_nextBackgroundJobId;
			const auto owner = m_invokingWindow;
			auto job = std::make_unique<BackgroundJob>();
			job->id = id;
			job->ownerWindow = owner;
			if (owner != 0)
			{
				const auto window = m_toolWindows.find(owner);
				if (window == m_toolWindows.end())
					throw std::runtime_error("background-job owner is no longer active");
				job->ownerGeneration = window->second->generation;
				job->ownerLifetime = window->second->lifetime;
			}
			job->cancelled = std::make_shared<std::atomic_bool>();
			auto cancelled = job->cancelled;
			auto gate = m_callbackGate;
			auto* controller = &m_controller;
			m_backgroundJobs.emplace(id, std::move(job));
			try
			{
				m_backgroundJobs.at(id)->worker = std::jthread(
					[controller, gate = std::move(gate), cancelled, id, owner,
					 request = std::move(request)](std::stop_token stopToken) mutable {
						auto isCancelled = [cancelled, stopToken] {
							return cancelled->load(std::memory_order_acquire) || stopToken.stop_requested();
						};
						auto progress = [gate, id, owner](
											const Application::GraphicPackInstallProgress& value) {
							PostBackgroundJobEvent(gate, id, owner, "jobs.progress",
												   std::string(R"({"jobId":)") + JsonString(std::to_string(id)) +
													   R"(,"windowId":)" + JsonString(std::to_string(owner)) +
													   R"(,"phase":)" + JsonString(GraphicPackInstallPhaseName(value.phase)) +
													   R"(,"completed":)" + std::to_string(value.completed) +
													   R"(,"total":)" + std::to_string(value.total) +
													   R"(,"currentPath":)" + JsonString(value.currentPath) + "}",
												   false);
						};
						Application::GraphicPackInstallResult result;
						try
						{
							result = controller->InstallGraphicPacks(request, std::move(progress),
																	 std::move(isCancelled));
						} catch (const std::exception& error)
						{
							result = {Application::GraphicPackInstallError::IoFailure, error.what()};
						} catch (...)
						{
							result = {Application::GraphicPackInstallError::IoFailure,
									  "graphic-pack worker failed with an unknown error"};
						}
						rapidjson::StringBuffer buffer;
						JsonWriter writer(buffer);
						writer.StartObject();
						writer.Key("jobId");
						writer.String(std::to_string(id).c_str());
						writer.Key("windowId");
						writer.String(std::to_string(owner).c_str());
						writer.Key("ok");
						writer.Bool(static_cast<bool>(result));
						writer.Key("error");
						writer.String(GraphicPackInstallErrorName(result.error).data());
						writer.Key("diagnostic");
						writer.String(result.diagnostic.data(),
									  static_cast<rapidjson::SizeType>(result.diagnostic.size()));
						writer.Key("upToDate");
						writer.Bool(result.upToDate);
						writer.Key("removedEnabledPaths");
						writer.StartArray();
						for (const auto& path : result.removedEnabledPaths)
							writer.String(path.data(), static_cast<rapidjson::SizeType>(path.size()));
						writer.EndArray();
						writer.EndObject();
						PostBackgroundJobEvent(gate, id, owner, "jobs.completed",
											   {buffer.GetString(), buffer.GetSize()}, true);
					});
			} catch (...)
			{
				m_backgroundJobs.erase(id);
				throw;
			}
			return id;
		}

		std::uint64_t StartWuaConversionJob(Application::WuaConversionPlan plan,
											fs::path outputPath)
		{
			if (std::ranges::any_of(m_backgroundJobs,
									[this](const auto& entry) { return entry.second->ownerWindow == m_invokingWindow; }))
				throw std::runtime_error("this window already has a background operation in progress");
			if (m_backgroundJobs.size() >= 4)
				throw std::runtime_error("too many background operations are in progress");
			const auto id = ++m_nextBackgroundJobId;
			const auto owner = m_invokingWindow;
			auto job = std::make_unique<BackgroundJob>();
			job->id = id;
			job->ownerWindow = owner;
			job->cancelled = std::make_shared<std::atomic_bool>();
			auto cancelled = job->cancelled;
			auto gate = m_callbackGate;
			auto* controller = &m_controller;
			m_backgroundJobs.emplace(id, std::move(job));
			try
			{
				m_backgroundJobs.at(id)->worker = std::jthread(
					[controller, gate = std::move(gate), cancelled, id, owner,
					 plan = std::move(plan), outputPath = std::move(outputPath)](std::stop_token stopToken) mutable {
						auto isCancelled = [cancelled, stopToken] {
							return cancelled->load(std::memory_order_acquire) || stopToken.stop_requested();
						};
						auto progress = [gate, id, owner](const Application::ContentOperationProgress& value) {
							static constexpr std::array phases{"counting", "collecting", "converting", "hashing", "finalizing"};
							const auto index = std::min<std::size_t>(static_cast<std::size_t>(value.phase), phases.size() - 1);
							PostBackgroundJobEvent(gate, id, owner, "jobs.progress",
												   std::string(R"({"jobId":)") + JsonString(std::to_string(id)) +
													   R"(,"windowId":)" + JsonString(std::to_string(owner)) +
													   R"(,"operation":"wuaConversion","phase":)" + JsonString(phases[index]) +
													   R"(,"filesCompleted":)" + std::to_string(value.filesCompleted) +
													   R"(,"filesTotal":)" + std::to_string(value.filesTotal) +
													   R"(,"bytesCompleted":)" + std::to_string(value.bytesCompleted) +
													   R"(,"bytesTotal":)" + std::to_string(value.bytesTotal) + "}",
												   false);
						};
						Application::ContentOperationResult result;
						try
						{
							result = controller->ConvertToWua(plan, outputPath, progress, isCancelled);
						} catch (const std::exception& error)
						{
							result = {Application::ContentOperationError::ReadFailure, error.what()};
						} catch (...)
						{
							result = {Application::ContentOperationError::ReadFailure, "WUA conversion worker failed"};
						}
						rapidjson::StringBuffer buffer;
						JsonWriter writer(buffer);
						writer.StartObject();
						writer.Key("jobId");
						writer.String(std::to_string(id).c_str());
						writer.Key("windowId");
						writer.String(std::to_string(owner).c_str());
						writer.Key("operation");
						writer.String("wuaConversion");
						writer.Key("ok");
						writer.Bool(static_cast<bool>(result));
						writer.Key("error");
						writer.Uint(static_cast<unsigned>(result.error));
						writer.Key("diagnostic");
						writer.String(result.diagnostic.data(), result.diagnostic.size());
						writer.EndObject();
						PostBackgroundJobEvent(gate, id, owner, "jobs.completed", {buffer.GetString(), buffer.GetSize()}, true);
					});
			} catch (...)
			{
				m_backgroundJobs.erase(id);
				throw;
			}
			return id;
		}

		std::uint64_t StartChecksumJob(std::uint64_t locationUid)
		{
			if (std::ranges::any_of(m_backgroundJobs,
									[this](const auto& entry) { return entry.second->ownerWindow == m_invokingWindow; }))
				throw std::runtime_error("this window already has a background operation in progress");
			if (m_backgroundJobs.size() >= 4)
				throw std::runtime_error("too many background operations are in progress");
			if (m_nextBackgroundJobId >= 9007199254740991ULL)
				throw std::runtime_error("background job identifier space is exhausted");
			const auto id = ++m_nextBackgroundJobId;
			const auto owner = m_invokingWindow;
			auto job = std::make_unique<BackgroundJob>();
			job->id = id;
			job->ownerWindow = owner;
			if (owner != 0)
			{
				const auto window = m_toolWindows.find(owner);
				if (window == m_toolWindows.end())
					throw std::runtime_error("background-job owner is no longer active");
				job->ownerGeneration = window->second->generation;
				job->ownerLifetime = window->second->lifetime;
			}
			job->cancelled = std::make_shared<std::atomic_bool>();
			auto cancelled = job->cancelled;
			auto gate = m_callbackGate;
			auto* controller = &m_controller;
			m_backgroundJobs.emplace(id, std::move(job));
			try
			{
				m_backgroundJobs.at(id)->worker = std::jthread(
					[controller, gate = std::move(gate), cancelled, id, owner, locationUid](std::stop_token stopToken) {
						auto isCancelled = [cancelled, stopToken] {
							return cancelled->load(std::memory_order_acquire) || stopToken.stop_requested();
						};
						auto phaseName = [](Application::ContentOperationPhase phase) {
							switch (phase)
							{
							case Application::ContentOperationPhase::Counting:
								return "counting";
							case Application::ContentOperationPhase::Collecting:
								return "collecting";
							case Application::ContentOperationPhase::Converting:
								return "converting";
							case Application::ContentOperationPhase::Hashing:
								return "hashing";
							case Application::ContentOperationPhase::Finalizing:
								return "finalizing";
							}
							return "unknown";
						};
						auto progress = [gate, id, owner, phaseName](const Application::ContentOperationProgress& value) {
							PostBackgroundJobEvent(gate, id, owner, "jobs.progress",
												   std::string(R"({"jobId":)") + JsonString(std::to_string(id)) +
													   R"(,"windowId":)" + JsonString(std::to_string(owner)) +
													   R"(,"phase":)" + JsonString(phaseName(value.phase)) +
													   R"(,"filesCompleted":)" + std::to_string(value.filesCompleted) +
													   R"(,"filesTotal":)" + std::to_string(value.filesTotal) +
													   R"(,"bytesCompleted":)" + std::to_string(value.bytesCompleted) +
													   R"(,"bytesTotal":)" + std::to_string(value.bytesTotal) + "}",
												   false);
						};
						Application::ContentChecksumResult result;
						try
						{
							result = controller->ComputeTitleChecksum(locationUid, progress, isCancelled);
						} catch (const std::exception& error)
						{
							result.error = Application::ContentOperationError::ReadFailure;
							result.diagnostic = error.what();
						} catch (...)
						{
							result.error = Application::ContentOperationError::ReadFailure;
							result.diagnostic = "checksum worker failed";
						}
						constexpr std::size_t kMaximumResultFiles = 20000;
						if (result && result.checksum->files.size() > kMaximumResultFiles)
						{
							result.error = Application::ContentOperationError::VerificationFailure;
							result.diagnostic = "checksum contains too many files to display safely";
							result.checksum.reset();
						}
						rapidjson::StringBuffer buffer;
						JsonWriter writer(buffer);
						writer.StartObject();
						writer.Key("jobId");
						writer.String(std::to_string(id).c_str());
						writer.Key("windowId");
						writer.String(std::to_string(owner).c_str());
						writer.Key("ok");
						writer.Bool(static_cast<bool>(result));
						auto errorName = [](Application::ContentOperationError error) {
							switch (error)
							{
							case Application::ContentOperationError::None:
								return "none";
							case Application::ContentOperationError::NotFound:
								return "notFound";
							case Application::ContentOperationError::Cancelled:
								return "cancelled";
							case Application::ContentOperationError::UnableToCreateOutput:
								return "unableToCreateOutput";
							case Application::ContentOperationError::ReadFailure:
								return "readFailure";
							case Application::ContentOperationError::VerificationFailure:
								return "verificationFailure";
							case Application::ContentOperationError::RenameFailure:
								return "renameFailure";
							}
							return "unknown";
						};
						writer.Key("error");
						writer.String(errorName(result.error));
						writer.Key("diagnostic");
						writer.String(result.diagnostic.data(), result.diagnostic.size());
						writer.Key("checksum");
						if (!result.checksum)
							writer.Null();
						else
						{
							const auto& checksum = *result.checksum;
							writer.StartObject();
							writer.Key("titleId");
							writer.String(TitleIdString(checksum.titleId).c_str());
							writer.Key("version");
							writer.Uint(checksum.version);
							writer.Key("region");
							writer.Uint(checksum.region);
							writer.Key("imageSha256");
							writer.String(checksum.imageSha256.data(), checksum.imageSha256.size());
							writer.Key("files");
							writer.StartArray();
							for (const auto& file : checksum.files)
							{
								writer.StartObject();
								writer.Key("path");
								writer.String(file.path.data(), file.path.size());
								writer.Key("sha256");
								writer.String(file.sha256.data(), file.sha256.size());
								writer.EndObject();
							}
							writer.EndArray();
							writer.EndObject();
						}
						writer.EndObject();
						PostBackgroundJobEvent(gate, id, owner, "jobs.completed", {buffer.GetString(), buffer.GetSize()}, true);
					});
			} catch (...)
			{
				m_backgroundJobs.erase(id);
				throw;
			}
			return id;
		}

		std::uint64_t StartCemodSnapshotJob(std::optional<std::uint64_t> titleId)
		{
			if (std::ranges::any_of(m_backgroundJobs,
									[this](const auto& entry) { return entry.second->ownerWindow == m_invokingWindow; }))
				throw std::runtime_error("this window already has a background operation in progress");
			if (m_backgroundJobs.size() >= 4)
				throw std::runtime_error("too many background operations are in progress");
			if (m_nextBackgroundJobId >= 9007199254740991ULL)
				throw std::runtime_error("background job identifier space is exhausted");
			const auto id = ++m_nextBackgroundJobId;
			const auto owner = m_invokingWindow;
			auto job = std::make_unique<BackgroundJob>();
			job->id = id;
			job->ownerWindow = owner;
			if (owner != 0)
			{
				const auto window = m_toolWindows.find(owner);
				if (window == m_toolWindows.end())
					throw std::runtime_error("background-job owner is no longer active");
				job->ownerGeneration = window->second->generation;
				job->ownerLifetime = window->second->lifetime;
			}
			job->cancelled = std::make_shared<std::atomic_bool>();
			auto cancelled = job->cancelled;
			auto gate = m_callbackGate;
			auto* controller = &m_controller;
			m_backgroundJobs.emplace(id, std::move(job));
			try
			{
				m_backgroundJobs.at(id)->worker = std::jthread(
					[controller, gate = std::move(gate), cancelled, id, owner, titleId](std::stop_token stopToken) {
						auto isCancelled = [cancelled, stopToken] {
							return cancelled->load(std::memory_order_acquire) || stopToken.stop_requested();
						};
						Application::CemodManagerResult result;
						try
						{
							result.snapshot = controller->GetCemodManagerSnapshot(titleId, isCancelled);
							if (result.snapshot.cancelled)
								result.diagnostic = "CemuMod package inspection was cancelled";
						} catch (const std::exception& error)
						{
							result.error = Application::CemodManagerError::InspectionFailed;
							result.diagnostic = error.what();
						} catch (...)
						{
							result.error = Application::CemodManagerError::InspectionFailed;
							result.diagnostic = "CemuMod package inspection failed";
						}
						auto payload = std::string(R"({"jobId":)") + JsonString(std::to_string(id)) +
									   R"(,"windowId":)" + JsonString(std::to_string(owner)) + R"(,"ok":)" +
									   (result ? "true" : "false") + R"(,"error":)" + JsonString(CemodErrorName(result.error)) +
									   R"(,"diagnostic":)" + JsonString(result.diagnostic) +
									   R"(,"snapshot":)" + CemodSnapshotJson(result.snapshot) + "}";
						PostBackgroundJobEvent(gate, id, owner, "cemod.snapshot", std::move(payload), true);
					});
			} catch (...)
			{
				m_backgroundJobs.erase(id);
				throw;
			}
			return id;
		}

		std::uint64_t StartTitleInstallJob(Application::TitleInstallPlan plan,
										   Application::TitleInstallDecision decision)
		{
			if (std::ranges::any_of(m_backgroundJobs,
									[this](const auto& entry) { return entry.second->ownerWindow == m_invokingWindow; }))
				throw std::runtime_error("this window already has a background operation in progress");
			if (m_backgroundJobs.size() >= 4)
				throw std::runtime_error("too many background operations are in progress");
			if (m_nextBackgroundJobId >= 9007199254740991ULL)
				throw std::runtime_error("background job identifier space is exhausted");
			const auto owner = m_invokingWindow;
			const auto window = m_toolWindows.find(owner);
			if (owner == 0 || window == m_toolWindows.end())
				throw std::runtime_error("title installation requires a live tool-window owner");
			const auto id = ++m_nextBackgroundJobId;
			auto job = std::make_unique<BackgroundJob>();
			job->id = id;
			job->ownerWindow = owner;
			job->ownerGeneration = window->second->generation;
			job->ownerLifetime = window->second->lifetime;
			job->cancelled = std::make_shared<std::atomic_bool>();
			auto cancelled = job->cancelled;
			auto gate = m_callbackGate;
			auto* controller = &m_controller;
			m_backgroundJobs.emplace(id, std::move(job));
			try
			{
				m_backgroundJobs.at(id)->worker = std::jthread(
					[controller, gate = std::move(gate), cancelled, id, owner,
					 plan = std::move(plan), decision](std::stop_token stopToken) mutable {
						auto isCancelled = [cancelled, stopToken] {
							return cancelled->load(std::memory_order_acquire) ||
								   stopToken.stop_requested();
						};
						auto progress = [gate, id, owner](
											const Application::TitleInstallProgress& value) {
							const auto currentPath = _pathToUtf8(value.currentPath);
							PostBackgroundJobEvent(gate, id, owner, "jobs.progress",
												   std::string(R"({"jobId":)") + JsonString(std::to_string(id)) +
													   R"(,"windowId":)" + JsonString(std::to_string(owner)) +
													   R"(,"operation":"titleInstall","phase":"copying","bytesCompleted":)" +
													   std::to_string(value.bytesCompleted) +
													   R"(,"bytesTotal":)" + std::to_string(value.bytesTotal) +
													   R"(,"filesCompleted":0,"filesTotal":0,"currentPath":)" +
													   JsonString(currentPath) + "}",
												   false);
						};
						Application::TitleInstallResult result;
						try
						{
							result = controller->InstallTitle(plan, decision,
															  std::move(progress), std::move(isCancelled));
						} catch (const std::exception& error)
						{
							result = {Application::TitleInstallError::CopyFailure, error.what(), {}};
						} catch (...)
						{
							result = {Application::TitleInstallError::CopyFailure,
									  "title-install worker failed",
									  {}};
						}
						rapidjson::StringBuffer buffer;
						JsonWriter writer(buffer);
						writer.StartObject();
						writer.Key("jobId");
						writer.String(std::to_string(id).c_str());
						writer.Key("windowId");
						writer.String(std::to_string(owner).c_str());
						writer.Key("operation");
						writer.String("titleInstall");
						writer.Key("ok");
						writer.Bool(static_cast<bool>(result));
						writer.Key("error");
						writer.Uint(static_cast<unsigned>(result.error));
						writer.Key("diagnostic");
						writer.String(result.diagnostic.data(),
									  static_cast<rapidjson::SizeType>(result.diagnostic.size()));
						writer.Key("titleId");
						writer.String(TitleIdString(plan.titleId).c_str());
						writer.EndObject();
						PostBackgroundJobEvent(gate, id, owner, "jobs.completed",
											   {buffer.GetString(), buffer.GetSize()}, true);
					});
			} catch (...)
			{
				m_backgroundJobs.erase(id);
				throw;
			}
			return id;
		}

		std::string IssueSaveTicket(SaveTicket ticket)
		{
			if (m_saveTickets.size() >= 32)
				throw std::runtime_error("too many pending save confirmations");
			const auto token = std::string("save-") + std::to_string(++m_nextSaveTicketId);
			m_saveTickets.emplace(token, std::move(ticket));
			return token;
		}

		SaveTicket TakeSaveTicket(std::string_view token, SaveTicketKind expected)
		{
			const auto found = m_saveTickets.find(std::string(token));
			if (found == m_saveTickets.end() || found->second.ownerWindow != m_invokingWindow ||
				found->second.kind != expected)
				throw std::invalid_argument("save confirmation token is invalid or expired");
			auto ticket = std::move(found->second);
			m_saveTickets.erase(found);
			return ticket;
		}

		std::uint64_t StartSaveArchiveJob(SaveTicket ticket)
		{
			if (std::ranges::any_of(m_backgroundJobs,
									[this](const auto& entry) { return entry.second->ownerWindow == m_invokingWindow; }))
				throw std::runtime_error("this window already has a background operation in progress");
			if (m_backgroundJobs.size() >= 4)
				throw std::runtime_error("too many background operations are in progress");
			if (m_nextBackgroundJobId >= 9007199254740991ULL)
				throw std::runtime_error("background job identifier space is exhausted");
			const auto id = ++m_nextBackgroundJobId;
			const auto owner = m_invokingWindow;
			auto job = std::make_unique<BackgroundJob>();
			job->id = id;
			job->ownerWindow = owner;
			job->cancelled = std::make_shared<std::atomic_bool>();
			auto cancelled = job->cancelled;
			auto gate = m_callbackGate;
			auto* controller = &m_controller;
			m_backgroundJobs.emplace(id, std::move(job));
			try
			{
				m_backgroundJobs.at(id)->worker = std::jthread(
					[controller, gate = std::move(gate), cancelled, id, owner,
					 ticket = std::move(ticket)](std::stop_token stopToken) mutable {
						auto isCancelled = [cancelled, stopToken] {
							return cancelled->load(std::memory_order_acquire) || stopToken.stop_requested();
						};
						auto progress = [gate, id, owner](const Application::SaveOperationProgress& value) {
							PostBackgroundJobEvent(gate, id, owner, "jobs.progress",
												   std::string(R"({"jobId":)") + JsonString(std::to_string(id)) +
													   R"(,"windowId":)" + JsonString(std::to_string(owner)) +
													   R"(,"phase":"archive","filesCompleted":)" + std::to_string(value.filesCompleted) +
													   R"(,"filesTotal":)" + std::to_string(value.filesTotal) +
													   R"(,"bytesCompleted":)" + std::to_string(value.bytesCompleted) +
													   R"(,"bytesTotal":)" + std::to_string(value.bytesTotal) + "}",
												   false);
						};
						Application::SaveOperationResult result;
						try
						{
							if (ticket.kind == SaveTicketKind::Import)
								result = controller->ImportSave(ticket.archivePath, ticket.titleId,
																ticket.targetPersistentId, ticket.overwrite, progress, isCancelled);
							else
								result = controller->ExportSave(ticket.titleId, ticket.sourcePersistentId,
																ticket.archivePath, ticket.overwrite, progress, isCancelled);
						} catch (const std::exception& error)
						{
							result = {Application::SaveOperationError::BackendFailure, error.what()};
						} catch (...)
						{
							result = {Application::SaveOperationError::BackendFailure, "save worker failed"};
						}
						rapidjson::StringBuffer buffer;
						JsonWriter writer(buffer);
						writer.StartObject();
						writer.Key("jobId");
						writer.String(std::to_string(id).c_str());
						writer.Key("windowId");
						writer.String(std::to_string(owner).c_str());
						writer.Key("ok");
						writer.Bool(static_cast<bool>(result));
						writer.Key("operation");
						writer.String(ticket.kind == SaveTicketKind::Import ? "import" : "export");
						writer.Key("error");
						writer.String(SaveErrorName(result.error).data());
						writer.Key("diagnostic");
						writer.String(result.diagnostic.data(), result.diagnostic.size());
						writer.EndObject();
						PostBackgroundJobEvent(gate, id, owner, "jobs.completed",
											   {buffer.GetString(), buffer.GetSize()}, true);
					});
			} catch (...)
			{
				m_backgroundJobs.erase(id);
				throw;
			}
			return id;
		}
		void CancelBackgroundJobsForWindow(std::uint64_t owner) noexcept
		{
			for (auto& [id, job] : m_backgroundJobs)
			{
				(void)id;
				if (job->ownerWindow == owner)
				{
					job->cancelled->store(true, std::memory_order_release);
					job->worker.request_stop();
				}
			}
		}

		void StopAllBackgroundJobs() noexcept
		{
			for (auto& [id, job] : m_backgroundJobs)
			{
				(void)id;
				job->cancelled->store(true, std::memory_order_release);
				job->worker.request_stop();
			}
			m_backgroundJobs.clear();
		}

		static std::string FrontendUrl()
		{
			if (const char* devUrl = std::getenv("CEMU_WEB_UI_DEV_URL"); devUrl && *devUrl)
			{
				const std::string_view url(devUrl);
				if (!url.starts_with("http://127.0.0.1:") && !url.starts_with("http://localhost:"))
					throw std::runtime_error("CEMU_WEB_UI_DEV_URL must use a loopback HTTP origin");
				return std::string(url);
			}
			return "cemu://ui/index.html";
		}

		static std::string BrowserContextJson(
			std::optional<std::uint64_t> titleContext = std::nullopt,
			std::string_view packageContext = {},
			std::optional<std::uint64_t> generationContext = std::nullopt)
		{
			std::string context{"{"};
			bool separator{};
			if (titleContext)
			{
				context += R"("titleId":)" + JsonString(TitleIdString(*titleContext));
				separator = true;
			}
			if (!packageContext.empty())
			{
				if (separator)
					context += ',';
				context += R"("packageKey":)" + JsonString(packageContext);
				separator = true;
			}
			if (generationContext)
			{
				if (separator)
					context += ',';
				context += R"("generation":)" + JsonString(std::to_string(*generationContext));
			}
			return context + '}';
		}

		std::string BrowserBootstrapJson(
			std::uint64_t windowId, std::string_view role,
			std::optional<std::uint64_t> titleContext = std::nullopt,
			std::string_view packageContext = {},
			std::optional<std::uint64_t> generationContext = std::nullopt) const
		{
			const auto accountSnapshot = m_controller.GetAccountManagerSnapshot();
			const auto activeAccount = std::ranges::find_if(
				accountSnapshot.accounts, [&accountSnapshot](const auto& account) {
					return account.persistentId == accountSnapshot.activePersistentId;
				});
			const auto activeAccountName =
				activeAccount == accountSnapshot.accounts.end()
					? std::string{}
					: boost::nowide::narrow(activeAccount->miiName);
			return std::string(R"({"windowId":)") + JsonString(std::to_string(windowId)) +
				   R"(,"windowRole":)" + JsonString(role) +
				   R"(,"appVersion":)" + JsonString(BUILD_VERSION_STRING) +
				   R"(,"platform":)" + JsonString(PlatformName()) +
				   R"(,"activeAccountName":)" + JsonString(activeAccountName) +
				   R"(,"context":)" +
				   BrowserContextJson(titleContext, packageContext, generationContext) +
				   R"(,"theme":)" + JsonString(UiThemeName(m_theme)) +
				   R"(,"themeRevision":)" + JsonString(std::to_string(m_themeRevision)) +
				   R"(,"language":)" + JsonString(m_language) +
				   R"(,"languageRevision":)" + JsonString(std::to_string(m_languageRevision)) +
				   R"(,"shuttingDown":false})";
		}

		std::string_view RoleForWindow(std::uint64_t windowId) const
		{
			if (windowId == 0)
				return "main-library";
			if (windowId == kMainOverlayWindowId || windowId == kPadOverlayWindowId)
				return "runtime-overlay";
			const auto found = m_toolWindows.find(windowId);
			if (found == m_toolWindows.end())
				throw std::runtime_error("the RPC caller window is no longer active");
			return found->second->role;
		}

		std::uint64_t QueueToolWindow(std::string_view role, std::string requestId,
									  std::optional<std::uint64_t> titleContext = std::nullopt,
									  std::string packageContext = {},
									  std::optional<std::uint64_t> generationContext = std::nullopt,
									  bool launchContinuation = false)
		{
			if (m_rpc.IsShuttingDown())
				throw std::runtime_error("the application is shutting down");
			(void)DescribeWindow(role);
			if (std::ranges::find(WebFrontend::Generated::ImplementedWindowRoles, role) ==
				WebFrontend::Generated::ImplementedWindowRoles.end())
				throw std::runtime_error("the requested window has not been migrated yet");
			const std::string ownedRole(role);
			if (const auto existing = m_windowByRole.find(ownedRole);
				existing != m_windowByRole.end())
			{
				if (role == "cemod-permissions")
					throw std::runtime_error("an exact CemuMod approval dialog is already open");
				const auto id = existing->second;
				if (const auto found = m_toolWindows.find(id); found != m_toolWindows.end())
				{
					found->second->titleContext = titleContext;
					found->second->packageContext = packageContext;
					found->second->generationContext = generationContext;
					Emit("window.contextChanged", std::string(R"({"windowId":)") +
													  JsonString(std::to_string(id)) + R"(,"titleId":)" +
													  (titleContext ? JsonString(TitleIdString(*titleContext)) : "null") + "}");
				}
				(void)PostToUi([this, id] {
					if (const auto found = m_toolWindows.find(id);
						found != m_toolWindows.end() && found->second->nativeSupport)
						found->second->nativeSupport->Focus();
				});
				if (!requestId.empty())
					Emit("window.opened", std::string(R"({"requestId":)") +
											  JsonString(requestId) + R"(,"windowId":)" + JsonString(std::to_string(id)) +
											  R"(,"role":)" + JsonString(ownedRole) + "}");
				return id;
			}
			if (const auto pending = m_pendingWindowRoles.find(ownedRole);
				pending != m_pendingWindowRoles.end())
			{
				if (const auto context = m_pendingWindowContexts.find(ownedRole);
					context != m_pendingWindowContexts.end() && context->second != titleContext)
					throw std::runtime_error(
						"the window is already opening with a different title context");
				if (m_pendingPackageContexts.at(ownedRole) != packageContext ||
					m_pendingGenerationContexts.at(ownedRole) != generationContext)
					throw std::runtime_error("the window is already opening with a different exact package context");
				if (!requestId.empty())
					m_pendingWindowRequests[ownedRole].push_back(std::move(requestId));
				return pending->second;
			}
			if (m_nextWindowId >= 9007199254740991ULL)
				throw std::runtime_error("tool window identifier space is exhausted");
			const auto id = ++m_nextWindowId;
			if (launchContinuation)
			{
				if (!m_pendingLaunch)
					throw std::runtime_error("the pending title launch is no longer active");
				m_pendingLaunch->permissionWindow = id;
			}
			m_pendingWindowRoles.emplace(ownedRole, id);
			m_pendingWindowContexts[ownedRole] = titleContext;
			m_pendingPackageContexts[ownedRole] = std::move(packageContext);
			m_pendingGenerationContexts[ownedRole] = generationContext;
			if (!requestId.empty())
				m_pendingWindowRequests[ownedRole].push_back(std::move(requestId));
			if (!PostToUi([this, role = ownedRole, id, launchContinuation] {
					auto notify = [this, &role, id](std::string_view event,
													std::string_view message = {}) {
						const auto requests = m_pendingWindowRequests.extract(role);
						if (requests.empty())
							return;
						for (const auto& requestId : requests.mapped())
						{
							auto payload = std::string(R"({"requestId":)") + JsonString(requestId) +
										   R"(,"windowId":)" + JsonString(std::to_string(id)) + R"(,"role":)" +
										   JsonString(role);
							if (!message.empty())
								payload += R"(,"message":)" + JsonString(message);
							Emit(event, payload + "}");
						}
					};
					if (launchContinuation && (!m_pendingLaunch ||
											   m_pendingLaunch->permissionWindow != id))
					{
						m_pendingWindowRoles.erase(role);
						m_pendingWindowContexts.erase(role);
						m_pendingPackageContexts.erase(role);
						m_pendingGenerationContexts.erase(role);
						return;
					}
					if (m_rpc.IsShuttingDown())
					{
						m_pendingWindowRoles.erase(role);
						m_pendingWindowContexts.erase(role);
						m_pendingPackageContexts.erase(role);
						m_pendingGenerationContexts.erase(role);
						notify("window.openFailed", "the application is shutting down");
						HandleLaunchPermissionOpenFailure(id,
														  "Application shutdown prevented the approval dialog from opening.");
						return;
					}
					try
					{
						const auto context = m_pendingWindowContexts.contains(role) ? m_pendingWindowContexts.at(role) : std::optional<std::uint64_t>{};
						const auto package = m_pendingPackageContexts.contains(role) ? m_pendingPackageContexts.at(role) : std::string{};
						const auto generation = m_pendingGenerationContexts.contains(role) ? m_pendingGenerationContexts.at(role) : std::optional<std::uint64_t>{};
						CreateToolWindow(role, id, context, package, generation);
						m_pendingWindowContexts.erase(role);
						m_pendingPackageContexts.erase(role);
						m_pendingGenerationContexts.erase(role);
						notify("window.opened");
					} catch (const std::exception& error)
					{
						m_pendingWindowRoles.erase(role);
						m_pendingWindowContexts.erase(role);
						m_pendingPackageContexts.erase(role);
						m_pendingGenerationContexts.erase(role);
						notify("window.openFailed", error.what());
						HandleLaunchPermissionOpenFailure(id, error.what());
					}
				}))
			{
				m_pendingWindowRoles.erase(ownedRole);
				m_pendingWindowRequests.erase(ownedRole);
				m_pendingWindowContexts.erase(ownedRole);
				m_pendingPackageContexts.erase(ownedRole);
				m_pendingGenerationContexts.erase(ownedRole);
				throw std::runtime_error("the UI dispatcher is shutting down");
			}
			return id;
		}

		void CreateToolWindow(std::string_view role, std::uint64_t id,
							  std::optional<std::uint64_t> titleContext, std::string packageContext,
							  std::optional<std::uint64_t> generationContext)
		{
			const auto& descriptor = DescribeWindow(role);
			if (const auto existing = m_windowByRole.find(std::string(role));
				existing != m_windowByRole.end())
			{
				const auto window = m_toolWindows.find(existing->second);
				if (window != m_toolWindows.end() && window->second->nativeSupport)
				{
					window->second->nativeSupport->Focus();
					m_pendingWindowRoles.erase(std::string(role));
					return;
				}
				m_windowByRole.erase(existing);
			}
			if (descriptor.modal)
			{
				for (const auto& [id, window] : m_toolWindows)
				{
					(void)id;
					if (DescribeWindow(window->role).modal)
						throw std::runtime_error("another modal window is already open");
				}
			}

			auto window = std::make_unique<ToolWindow>();
			window->id = id;
			window->generation = ++m_nextWindowGeneration;
			window->role = role;
			window->titleContext = titleContext;
			window->packageContext = std::move(packageContext);
			window->generationContext = generationContext;
			window->nativeSupport = CreateToolWindowSupport(
				m_nativeWindow->GetNativeWindow(), descriptor.modal, [this, id] {
					(void)PostToUi([this, id] { RequestToolWindowClose(id); });
				});
			window->nativeSupport->SetTitle(descriptor.title);
			window->nativeSupport->SetSize(descriptor.width, descriptor.height);
			try
			{
#if defined(CEMU_OVERLAY_BACKEND_CEF)
				auto* support = window->nativeSupport.get();
				const auto bounds = support->GetBrowserBounds();
				WebFrontend::CefOverlay::BrowserDescriptor browser;
				browser.windowId = window->id;
				browser.role = window->role;
				browser.contextJson = BrowserContextJson(
					window->titleContext, window->packageContext, window->generationContext);
				browser.bootstrapJson = BrowserBootstrapJson(
					window->id, window->role, window->titleContext,
					window->packageContext, window->generationContext);
				browser.initialUrl = FrontendUrl();
				browser.presentation = WebFrontend::CefOverlay::BrowserPresentation::NativeChild;
				browser.nativeParent = support->GetBrowserParentWindow();
				browser.bounds = bounds;
				browser.bounds.width = std::max(browser.bounds.width, 1);
				browser.bounds.height = std::max(browser.bounds.height, 1);
				browser.dpiScale = support->GetBrowserDpiScale();
				browser.nativeBrowserCreated = [support](void* child) {
					support->AttachBrowser(child);
				};
				browser.nativeBrowserClosing = [support](void* child) {
					support->DetachBrowser(child);
				};
				if (!m_cefOverlay || !m_cefOverlay->CreateBrowser(browser))
					throw std::runtime_error("failed to create the CEF tool browser");
#endif
				m_windowByRole.emplace(window->role, id);
				m_toolWindows.emplace(id, std::move(window));
				m_toolWindows.at(id)->nativeSupport->Show();
				RefreshInputConfigurationFocus();
				m_pendingWindowRoles.erase(std::string(role));
				return;
			} catch (...)
			{
				m_windowByRole.erase(std::string(role));
				if (!window)
				{
					if (const auto inserted = m_toolWindows.find(id); inserted != m_toolWindows.end())
					{
						window = std::move(inserted->second);
						m_toolWindows.erase(inserted);
					}
				}
				if (!window)
					throw;
				window->nativeSupport.reset();
				throw;
			}
		}

		void CloseToolWindow(std::uint64_t id) noexcept
		{
			const auto found = m_toolWindows.find(id);
			if (found == m_toolWindows.end())
				return;
			auto window = std::move(found->second);
			const bool launchPermissionWindow = m_pendingLaunch &&
												m_pendingLaunch->permissionWindow == id;
			const bool launchOwnerWindow = m_pendingLaunch &&
										   m_pendingLaunch->ownerWindow == id;
			const auto launchPermissionWindowId = launchOwnerWindow ? m_pendingLaunch->permissionWindow : 0;
			const bool continueLaunch = launchPermissionWindow &&
										m_pendingLaunch->decisionSaved && m_pendingLaunch->approved;
			window->lifetime->store(false, std::memory_order_release);
			m_updatePlans.RevokeOwner(id, window->generation);
			m_memorySearch.CloseOwner(id);
			m_ppcDebugger.CloseOwner(id);
			CancelBackgroundJobsForWindow(id);
			std::erase_if(m_saveTickets, [id](const auto& item) {
				return item.second.ownerWindow == id;
			});
			std::erase_if(m_wuaPlans, [id](const auto& item) { return item.second.owner == id; });
			std::erase_if(m_installPlans, [id](const auto& item) { return item.second.owner == id; });
			std::erase_if(m_deletePlans, [id](const auto& item) { return item.second.owner == id; });
			std::erase_if(m_installSources, [id](const auto& item) { return item.second.owner == id; });
			std::erase_if(m_wuaDestinations, [id](const auto& item) { return item.second.owner == id; });
			m_toolWindows.erase(found);
			m_windowByRole.erase(window->role);
			RefreshInputConfigurationFocus();
			window->nativeSupport.reset();
			if (launchOwnerWindow)
			{
				CancelPendingLaunch("cancelled",
									"Closing the launch owner cancelled the pending title launch.");
				if (m_toolWindows.contains(launchPermissionWindowId))
					RequestToolWindowClose(launchPermissionWindowId);
			}
			else if (launchPermissionWindow)
			{
				if (continueLaunch)
				{
					try
					{
						if (!PostToUi([this, id] {
								if (m_pendingLaunch &&
									m_pendingLaunch->permissionWindow == id &&
									m_pendingLaunch->decisionSaved &&
									m_pendingLaunch->approved)
									ResumePendingLaunchNoexcept();
							}))
							CancelPendingLaunch("shutdown",
												"The UI dispatcher stopped before title launch could resume.");
					} catch (...)
					{
						CancelPendingLaunch("failed",
											"Title launch continuation could not be queued.");
					}
				}
				else
					CancelPendingLaunch(m_pendingLaunch->decisionSaved ? "permissionDenied" : "cancelled",
										m_pendingLaunch->decisionSaved ? "The exact package approval was denied." : "The exact package approval was cancelled.");
			}
			MaybeTerminateAfterShutdown();
		}

		void RefreshInputConfigurationFocus()
		{
			const bool editing = m_mainWorkspaceRole == "input-settings" ||
								 m_mainWorkspaceRole == "hotkey-settings" ||
								 m_windowByRole.contains("input-settings") ||
								 m_windowByRole.contains("hotkey-settings");
			m_hotkeyEditing.store(editing, std::memory_order_release);
			if (m_hostServices)
				m_hostServices->SetInputConfigurationFocused(editing);
		}

		void MaybeTerminateAfterShutdown() noexcept
		{
			if (m_terminateWhenToolsClosed && !m_mainReplyPending &&
				m_toolWindows.empty())
			{
#if defined(CEMU_OVERLAY_BACKEND_CEF)
				WebFrontend::CefNative::QuitNativeUiLoop();
#endif
			}
		}

		void RequestToolWindowClose(std::uint64_t id) noexcept
		{
			const auto found = m_toolWindows.find(id);
			if (found == m_toolWindows.end() || std::exchange(found->second->closing, true))
				return;
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			if (m_cefOverlay && m_cefOverlay->CloseWindow(id))
				return;
#endif
			found->second->closing = false;
			CloseToolWindow(id);
		}

		void RequestAllToolWindowsClose() noexcept
		{
			std::vector<std::uint64_t> ids;
			ids.reserve(m_toolWindows.size());
			for (const auto& [id, window] : m_toolWindows)
			{
				(void)window;
				ids.push_back(id);
			}
			for (const auto id : ids)
				RequestToolWindowClose(id);
		}

		void CloseAllToolWindows() noexcept
		{
			m_pendingWindowRoles.clear();
			RequestAllToolWindowsClose();
		}

		void Cleanup() noexcept
		{
			if (std::exchange(m_cleanedUp, true))
				return;
			m_stopping.store(true, std::memory_order_release);
			m_eventStopping->store(true, std::memory_order_release);
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			if (m_cemodWebUi)
				m_cemodWebUi->BeginShutdown();
#endif
			m_memorySearch.BeginShutdown();
			m_ppcDebugger.BeginShutdown();
			CancelPendingLaunch("shutdown",
								"Application shutdown cancelled the pending title launch.");
			StopAllBackgroundJobs();
			m_emulatedUsb.Close();
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			RuntimeOverlay::Model::Instance().ClearChangeHandler();
#endif
			{
				std::scoped_lock lock(m_callbackGate->mutex);
				m_callbackGate->target = nullptr;
			}
			m_titleEvents.Reset();
			m_applicationEvents.Reset();
			m_loggingEvents.Reset();
			m_rpc.BeginShutdown();
			constexpr unsigned maximumShutdownAttempts = 500;
			unsigned shutdownAttempt{};
			while (!TryShutdownApplication(false) &&
				   ++shutdownAttempt < maximumShutdownAttempts)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			if (!m_applicationShutdown)
			{
				cemuLog_log(LogType::Force,
							"Cemu could not safely release emulation resources during final shutdown; terminating without destroying native renderer surfaces");
				std::_Exit(EXIT_FAILURE);
			}
			if (!DestroyMainRenderRegion())
			{
				cemuLog_log(LogType::Force,
							"Cemu could not safely detach native renderer surfaces during final shutdown");
				std::_Exit(EXIT_FAILURE);
			}
			if (m_hostConnected)
			{
				InputManager::instance().Shutdown();
				Application::DisconnectHost();
				InputManager::instance().ClearHost();
				IAudioAPI::ConfigureNativeSurfaceProvider(nullptr);
				m_hostConnected = false;
			}
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			if (m_cemodWebUi)
				m_cemodWebUi->Shutdown();
			if (m_cefOverlay)
			{
				m_cefOverlay->CloseAll();
				m_cefOverlay.reset();
			}
			m_cemodWebUi.reset();
#endif
			if (m_windowState)
				(void)m_windowState->BeginShutdown();
			CloseAllToolWindows();
			m_nativeWindow->SetCloseHandler({});
			m_nativeWindow->SetGameCloseHandler({});
			m_nativeWindow->SetMetricsHandler({});
			m_nativeWindow->SetPadCloseHandler({});
			m_nativeWindow->SetInputHandler({});
			m_nativeWindow->UpdateTextInput({});
			if (m_hostServices)
				m_hostServices->Deactivate();
			if (m_mainWindowPublication)
			{
				m_hostState->ClearMainWindow(m_mainWindowPublication);
				m_mainWindowPublication = {};
			}
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			WebFrontend::CefOverlay::ShutdownProcessRuntime();
			if (m_nativeUiLoopInitialized)
			{
				WebFrontend::CefNative::ShutdownNativeUiLoop();
				m_nativeUiLoopInitialized = false;
			}
#endif
		}

		bool RequestShutdown(bool deferMainReply = false)
		{
			if (m_rpc.IsShuttingDown())
				return true;
			CancelPendingLaunch("shutdown",
								"Application shutdown cancelled the pending title launch.");
			if (!TryShutdownApplication())
			{
				Emit("system.diagnostic",
					 std::string(R"({"message":)") +
						 JsonString("Cemu could not stop the running title; shutdown was cancelled") + "}");
				return false;
			}
			if (!DestroyMainRenderRegion())
				return false;
			ShowLibraryContent();
			(void)m_windowState->FinishEmulation();
			m_rpc.BeginShutdown();
			(void)m_windowState->BeginShutdown();
			m_terminateWhenToolsClosed = true;
			m_mainReplyPending = deferMainReply;
			if (!m_toolWindows.empty())
				RequestAllToolWindowsClose();
			MaybeTerminateAfterShutdown();
			return true;
		}

		bool TryShutdownApplication(bool reportFailure = true)
		{
			if (m_applicationShutdown)
				return true;
			const auto result = m_controller.ShutdownApplication();
			if (result.stopped)
			{
				m_applicationShutdown = true;
				return true;
			}
			if (reportFailure)
				cemuLog_log(LogType::Force,
							"Web frontend shutdown could not release emulation resources: {}",
							result.diagnostic);
			return false;
		}

		bool StopEmulation()
		{
			const auto result = m_controller.Stop();
			if (!result.stopped)
			{
				cemuLog_log(LogType::Force, "Game window close could not stop the title: {}",
							result.diagnostic);
				return false;
			}
			if (!DestroyMainRenderRegion())
			{
				cemuLog_log(LogType::Force,
							"Game window close stopped the title but could not detach its render surfaces");
				return false;
			}
			(void)m_windowState->FinishEmulation();
			FinishGameWindowLifetime();
			return true;
		}

		void HandleGameWindowClose()
		{
			// Hyprland killactive and the native title-bar close button both arrive
			// here as graceful close requests. Coalesce repeats while teardown is in
			// progress and bind the request to the game generation that received it.
			if (m_gameClosePending.exchange(true, std::memory_order_acq_rel))
				return;
			const auto expectedGeneration = m_windowState->Snapshot().generation;
			if (!PostToUi([this, expectedGeneration] {
					const auto state = m_windowState->Snapshot();
					if (state.mode == WebFrontend::MainWindowContentMode::Playing &&
						state.generation == expectedGeneration)
						(void)StopEmulation();
					m_gameClosePending.store(false, std::memory_order_release);
				}))
				m_gameClosePending.store(false, std::memory_order_release);
		}

		void HandleLauncherClose()
		{
			const auto mode = m_windowState ? m_windowState->Snapshot().mode
											: WebFrontend::MainWindowContentMode::Library;
			if (mode == WebFrontend::MainWindowContentMode::Playing ||
				mode == WebFrontend::MainWindowContentMode::LaunchPending ||
				m_controller.State() == Application::EmulationState::Running)
			{
				m_launcherClosed = true;
				m_nativeWindow->HideLauncher();
				return;
			}
			(void)RequestShutdown();
		}

		void FinishGameWindowLifetime()
		{
			if (m_commandLineLaunch || m_launcherClosed)
				(void)RequestShutdown();
			else
				ShowLibraryContent();
		}

		void ShowLibraryContent()
		{
			if (!m_launcherClosed)
				m_nativeWindow->ShowLibrary();
		}

		void ShowRenderContent()
		{
			m_nativeWindow->ShowRenderRegion();
		}

		void CreateMainRuntimeOverlay(Host::IRenderRegion& region)
		{
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			if (!m_cefOverlay)
				return;
			const auto metrics = m_hostState->GetWindowMetrics();
			const auto bounds = region.GetBounds();
			if (!m_cefOverlay->Create(Host::PointerSurface::Main, kMainOverlayWindowId,
									  metrics.physicalWidth > 0 ? metrics.physicalWidth : bounds.width,
									  metrics.physicalHeight > 0 ? metrics.physicalHeight : bounds.height,
									  metrics.dpiScale))
				cemuLog_log(LogType::Force, "Main CEF Runtime Overlay browser could not be created; continuing without overlay");
#endif
		}

		void DestroyMainRuntimeOverlay() noexcept
		{
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			if (m_cefOverlay)
				m_cefOverlay->Close(Host::PointerSurface::Main);
#endif
		}

		void CreatePadRuntimeOverlay(Host::IRenderRegion& region)
		{
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			if (!m_cefOverlay)
				return;
			const auto metrics = m_hostState->GetWindowMetrics();
			const auto bounds = region.GetBounds();
			if (!m_cefOverlay->Create(Host::PointerSurface::Pad, kPadOverlayWindowId,
									  metrics.physicalPadWidth > 0 ? metrics.physicalPadWidth : bounds.width,
									  metrics.physicalPadHeight > 0 ? metrics.physicalPadHeight : bounds.height,
									  metrics.padDpiScale))
				cemuLog_log(LogType::Force, "GamePad CEF Runtime Overlay browser could not be created; continuing without overlay");
#endif
		}

		void DestroyPadRuntimeOverlay() noexcept
		{
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			if (m_cefOverlay)
				m_cefOverlay->Close(Host::PointerSurface::Pad);
#endif
		}

		bool DestroyMainRenderRegion()
		{
			if (!ClosePadRenderRegion())
				return false;
			ReleaseNativeInput(true);
			if (m_rendererHost)
				m_rendererHost->PrepareMainDestroy();
			DestroyMainRuntimeOverlay();
			m_nativeWindow->DestroyMainRenderRegion();
			return true;
		}

		bool ClosePadRenderRegion(std::optional<std::uint64_t> expectedGeneration = {})
		{
			if (expectedGeneration && *expectedGeneration != m_padGeneration)
				return true;
			if (!m_nativeWindow->IsPadRenderRegionOpen())
				return true;
			m_nativeWindow->SetPadMetricsEnabled(false);
			auto metrics = m_nativeWindow->GetMetrics();
			metrics.padOpen = false;
			metrics.padWidth = 0;
			metrics.padHeight = 0;
			metrics.physicalPadWidth = 0;
			metrics.physicalPadHeight = 0;
			m_hostState->UpdateMetrics(metrics);
			ReleasePointerSurface(Host::PointerSurface::Pad);
			try
			{
				if (m_rendererHost)
					m_rendererHost->PreparePadDestroy();
			} catch (const std::exception& error)
			{
				m_nativeWindow->SetPadMetricsEnabled(true);
				m_hostState->UpdateMetrics(m_nativeWindow->GetMetrics());
				cemuLog_log(LogType::Force, "Unable to safely close the GamePad surface: {}",
							error.what());
				Emit("system.diagnostic", std::string(R"({"message":)") +
											  JsonString(std::string("Unable to safely close GamePad view: ") + error.what()) + "}");
				return false;
			}
			DestroyPadRuntimeOverlay();
			m_nativeWindow->DestroyPadRenderRegion();
			++m_padGeneration;
			return true;
		}

		void TogglePadRenderRegion()
		{
			if (m_controller.State() != Application::EmulationState::Running)
			{
				Emit("system.diagnostic", R"({"message":"GamePad view is only available while a title is running"})");
				return;
			}
			if (m_nativeWindow->IsPadRenderRegionOpen())
			{
				(void)ClosePadRenderRegion();
				return;
			}
			try
			{
				++m_padGeneration;
				auto& region = m_nativeWindow->CreatePadRenderRegion();
				CreatePadRuntimeOverlay(region);
				m_nativeWindow->SetPadMetricsEnabled(true);
				m_hostState->UpdateMetrics(m_nativeWindow->GetMetrics());
				m_rendererHost->InitializePad(region);
				region.SetVisible(true);
				region.RequestFocus();
			} catch (const std::exception& error)
			{
				(void)ClosePadRenderRegion();
				Emit("system.diagnostic", std::string(R"({"message":)") +
											  JsonString(std::string("Unable to open GamePad view: ") + error.what()) + "}");
			}
		}

		void PublishInputOwnership(Host::PointerSurface surface,
								   const WebFrontend::CefOverlay::InputOwnership& ownership)
		{
			Application::PhysicalInputOwnership update;
			update.surface = surface == Host::PointerSurface::Main
								 ? Application::PointerSurface::Tv
								 : Application::PointerSurface::Drc;
			update.keyboardOwner = static_cast<std::uint8_t>(ownership.keyboard);
			update.pointerOwner = static_cast<std::uint8_t>(ownership.pointer);
			update.textOwner = static_cast<std::uint8_t>(ownership.text);
			update.flags =
				(ownership.webUiTextFocused
					 ? static_cast<std::uint8_t>(cemuextend::wire::InputOwnershipFlag::WebUiTextFocused)
					 : 0) |
				(ownership.webUiPointerCaptured
					 ? static_cast<std::uint8_t>(cemuextend::wire::InputOwnershipFlag::WebUiPointerCaptured)
					 : 0);
			m_controller.UpdatePhysicalInputOwnership(update);
			cemuLog_log(LogType::Force,
						"CEX2-INPUT ownership surface={} keyboard={} pointer={} text={}",
						static_cast<unsigned>(surface),
						static_cast<unsigned>(ownership.keyboard),
						static_cast<unsigned>(ownership.pointer),
						static_cast<unsigned>(ownership.text));
		}

		static WebFrontend::InputWindow SurfaceWindow(Host::PointerSurface surface)
		{
			return surface == Host::PointerSurface::Main ? WebFrontend::InputWindow::Game
														 : WebFrontend::InputWindow::GamePad;
		}

		bool InputAllowed(Host::PointerSurface surface) const
		{
			return !m_managedInputFocus || m_inputFocus.Allows(SurfaceWindow(surface));
		}

		void ApplyManagedInputFocus(const char* reason)
		{
			// Release before restoring any owner. Visibility/DOM focus intent survives.
			ReleaseNativeInput(true, false);
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			if (m_cefOverlay)
				for (const auto surface : {Host::PointerSurface::Main, Host::PointerSurface::Pad})
					m_cefOverlay->SetInputSuspended(surface, !InputAllowed(surface));
#endif
			for (const auto surface : {Host::PointerSurface::Main, Host::PointerSurface::Pad})
				(void)RefreshPointerPolicy(surface);
			cemuLog_log(LogType::Force,
						"CEX2-FOCUS window={} mainAllowed={} padAllowed={} revision={} reason={}",
						static_cast<unsigned>(m_inputFocus.Window()), InputAllowed(Host::PointerSurface::Main),
						InputAllowed(Host::PointerSurface::Pad), m_inputFocus.Revision(), reason);
			if (InputAllowed(Host::PointerSurface::Main))
				RefreshTextInput();
			SignalRuntimeOverlayChanged();
		}

		void HandleMetrics(Host::WindowMetricsSnapshot metrics)
		{
			bool focusChanged = false;
			if (const auto window = m_nativeWindow->GetActiveInputWindow())
			{
				m_managedInputFocus = true;
				const bool playing = m_windowState && m_windowState->Snapshot().mode ==
														  WebFrontend::MainWindowContentMode::Playing;
				focusChanged = m_inputFocus.Observe(*window, playing);
				// Query current native activation; a queued size/focus callback may
				// carry an obsolete snapshot. Waiting for a click is not OS focus loss.
				metrics.appActive = playing ? m_inputFocus.RenderActive()
											: *window == WebFrontend::InputWindow::Launcher;
			}
			else
				metrics.appActive = m_nativeKeyboardFocus.EffectivePointerFocus(metrics.appActive);
			const auto previous = m_hostState->GetWindowMetrics();
			m_hostState->UpdateMetrics(metrics);
			if (focusChanged)
				ApplyManagedInputFocus("activation");
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			if (m_cefOverlay)
			{
				m_cefOverlay->SetInputSuspended(Host::PointerSurface::Main,
												!metrics.appActive || !InputAllowed(Host::PointerSurface::Main));
				m_cefOverlay->SetInputSuspended(Host::PointerSurface::Pad,
												!metrics.appActive || !InputAllowed(Host::PointerSurface::Pad));
				if (m_cefOverlay->HasWindow(0))
				{
					const auto browserBounds = m_nativeWindow->GetBrowserBounds();
					m_cefOverlay->ResizeWindow(0, std::max(browserBounds.width, 1),
											   std::max(browserBounds.height, 1), m_nativeWindow->GetBrowserDpiScale());
					// Window 0 is the launcher browser. Game-window metrics must never
					// focus it: its native X11 child otherwise takes keyboard focus back
					// from the render window and leaves both Minecraft and the OSR Dock
					// without usable input until the user changes focus manually.
					const bool launcherFocused =
						m_managedInputFocus
							? m_inputFocus.Window() == WebFrontend::InputWindow::Launcher
							: metrics.appActive && (!m_windowState || m_windowState->Snapshot().mode ==
																		  WebFrontend::MainWindowContentMode::Library);
					m_cefOverlay->SetWindowFocus(0, launcherFocused);
				}
				m_cefOverlay->Resize(Host::PointerSurface::Main, metrics.physicalWidth,
									 metrics.physicalHeight, metrics.dpiScale);
				if (metrics.padOpen)
					m_cefOverlay->Resize(Host::PointerSurface::Pad, metrics.physicalPadWidth,
										 metrics.physicalPadHeight, metrics.padDpiScale);
			}
#endif
			if (previous.appActive != metrics.appActive)
			{
				cemuLog_log(LogType::Force, "CEX2-FOCUS appActive={}",
							metrics.appActive);
				m_controller.PointerFocusChanged(metrics.appActive);
				if (!metrics.appActive && m_hostServices)
				{
					ReleaseNativeInput(false, false);
				}
			}
		}

		Frontend::CemuExtendFrontendBridge& PointerBridge(Host::PointerSurface surface)
		{
			return m_pointerBridges[surface == Host::PointerSurface::Main ? 0 : 1];
		}

		std::uint32_t PointerButtonMask(std::uint32_t nativeButton) const
		{
			using Button = Frontend::CemuExtendMouseButton;
			switch (nativeButton)
			{
			case 1:
				return static_cast<std::uint32_t>(Button::Left);
			case 3:
				return static_cast<std::uint32_t>(Button::Right);
			case 2:
				return static_cast<std::uint32_t>(Button::Middle);
			case 8:
				return static_cast<std::uint32_t>(Button::X1);
			case 9:
				return static_cast<std::uint32_t>(Button::X2);
			default:
				return 0;
			}
		}

		Frontend::CemuExtendPointerDecision RefreshPointerPolicy(Host::PointerSurface surface)
		{
			auto& bridge = PointerBridge(surface);
			const auto index = surface == Host::PointerSurface::Main ? 0U : 1U;
			if (!InputAllowed(surface))
			{
				auto decision = bridge.ApplyPointerPolicy(0, 0, 0, false, false);
				decision.showCursor = true;
				m_nativeWindow->ApplyPointerPresentation({.surface = surface,
														  .showCursor = true,
														  .leavingPolicy = true});
				return decision;
			}
			if (m_webUiPointerOwnership[index])
			{
				// The Dock owns the complete interactive surface. Keep this override in
				// the common refresh path so a later physical mouse event cannot reapply
				// Minecraft's relative/raw policy and hide the cursor again.
				auto decision = bridge.ApplyPointerPolicy(0, 0, 0, false, false);
				decision.ownsPointer = true;
				decision.showCursor = true;
				decision.requestRawMouse = false;
				m_nativeWindow->ApplyPointerPresentation({
					.surface = surface,
					.ownsPointer = true,
					.showCursor = true,
					.leavingPolicy = decision.leavingPolicy,
					.rawMouseEnabled = false,
				});
				return decision;
			}
			const auto policy = m_controller.GetPointerPolicy();
			const auto metrics = m_hostState->GetWindowMetrics();
			const bool hasCanvas = surface == Host::PointerSurface::Main
									   ? m_windowState && m_windowState->Snapshot().mode == WebFrontend::MainWindowContentMode::Playing
									   : m_nativeWindow->IsPadRenderRegionOpen();
			const auto decision = bridge.ApplyPointerPolicy(policy.mode, policy.cursor,
															policy.flags, metrics.appActive, hasCanvas);
			m_nativeWindow->ApplyPointerPresentation({
				.surface = surface,
				.ownsPointer = decision.ownsPointer,
				.showCursor = decision.showCursor,
				.confine = decision.confine,
				.enteringCapture = decision.enteringCapture,
				.leavingPolicy = decision.leavingPolicy,
				.requestRawMouse = decision.requestRawMouse,
				.rawMouseEnabled = bridge.RawMouseRequested(),
				.cursor = decision.cursor,
			});
			return decision;
		}

		void SubmitPointer(const WebFrontend::NativeInputEvent& event,
						   std::int32_t deltaX, std::int32_t deltaY, std::int32_t wheelX,
						   std::int32_t wheelY, std::uint32_t changedButtons,
						   std::uint32_t deviceButtons, bool raw)
		{
			auto& state = m_pointerStates[event.surface == Host::PointerSurface::Main ? 0 : 1];
			const auto metrics = m_hostState->GetWindowMetrics();
			m_controller.SubmitMouse({
				.surface = event.surface == Host::PointerSurface::Main
							   ? Application::PointerSurface::Tv
							   : Application::PointerSurface::Drc,
				.deviceId = event.deviceId,
				.x = state.x,
				.y = state.y,
				.deltaX = deltaX,
				.deltaY = deltaY,
				.wheelX = wheelX,
				.wheelY = wheelY,
				.buttons = PointerBridge(event.surface).MouseButtons(),
				.changedButtons = changedButtons,
				.deviceButtons = deviceButtons,
				.contentWidth = state.width,
				.contentHeight = state.height,
				.insideContent = state.inside,
				.focused = m_managedInputFocus ? m_inputFocus.RenderActive()
											   : m_nativeKeyboardFocus.EffectivePointerFocus(metrics.appActive || event.windowActive),
				.flags = raw
							 ? static_cast<std::uint8_t>(Frontend::CemuExtendMouseEventFlag::RawRelative)
							 : static_cast<std::uint8_t>(0),
			});
		}

		void ReleasePointerSurface(Host::PointerSurface surface)
		{
			const auto index = surface == Host::PointerSurface::Main ? 0U : 1U;
			auto& bridge = m_pointerBridges[index];
			const auto released = bridge.UpdateButtons(
				Frontend::CemuExtendMouseTransition::Aggregate, 0xffffffffU, 0);
			if (released.changed)
			{
				WebFrontend::NativeInputEvent event{.kind = WebFrontend::NativeInputKind::PointerButton,
													.surface = surface};
				SubmitPointer(event, 0, 0, 0, 0, released.changed,
							  released.deviceButtons, false);
			}
			(void)bridge.ApplyPointerPolicy(0, 0, 0, false, false);
			bridge.ResetPointerPosition();
			m_pointerStates[index] = {};
			m_nativeWindow->ApplyPointerPresentation({.surface = surface,
													  .showCursor = true,
													  .leavingPolicy = true});
		}

		void ReleaseNativeInput(bool resetTextInput, bool releaseOverlayIntent = true)
		{
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			if (m_cefOverlay && releaseOverlayIntent)
			{
				m_cefOverlay->ReleaseInput(Host::PointerSurface::Main);
				m_cefOverlay->ReleaseInput(Host::PointerSurface::Pad);
			}
#endif
			m_nativeKeyboardFocus.Reset();
			if (m_hostServices)
				m_hostServices->ReleaseKeys();
			m_controller.KeyboardFocusLost();
			ReleasePointerSurface(Host::PointerSurface::Main);
			ReleasePointerSurface(Host::PointerSurface::Pad);
			if (resetTextInput)
			{
				m_textInputSequence = 0;
				m_nativeWindow->UpdateTextInput({});
			}
		}

		void ConfirmNativeFocusLoss()
		{
			// A deferred GTK focus-out is authoritative. Publish it directly to the
			// cached snapshot as well as CEX2 so the next WindowGet cannot resurrect
			// the pre-idle active state or retain a transient corrected value.
			auto metrics = m_hostState->GetWindowMetrics();
			metrics.appActive = false;
			m_hostState->UpdateMetrics(metrics);
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			if (m_cefOverlay)
			{
				m_cefOverlay->SetInputSuspended(Host::PointerSurface::Main, true);
				m_cefOverlay->SetInputSuspended(Host::PointerSurface::Pad, true);
				if (m_cefOverlay->HasWindow(0))
					m_cefOverlay->SetWindowFocus(0, false);
			}
#endif
			m_controller.PointerFocusChanged(false);
		}

		void ConfirmNativeKeyboardFocus()
		{
			// PointerFocusChanged publishes the ordered MouseV2 edge, but WindowGet
			// reads WebHostState independently. Update both from the same native
			// key-down so a previously cached false value cannot end a long hold at
			// Aqua's next periodic window poll.
			auto metrics = m_hostState->GetWindowMetrics();
			metrics.appActive = true;
			m_hostState->UpdateMetrics(metrics);
			// Window 0 is the launcher browser, not the in-game OSR overlay. Giving
			// it native CEF focus here steals X focus from the render window and
			// immediately turns this same key-down into a synthetic FocusLost/up.
			// OSR input focus is maintained by the overlay runtime's layer routing.
			m_controller.PointerFocusChanged(true);
		}

		void RefreshHotkeyBindings(const Application::HotkeySettingsModel& model)
		{
			std::scoped_lock lock(m_hotkeyMutex);
			m_hotkeySettings = model;
		}

		std::optional<Application::HotkeyAction> MatchKeyboardHotkey(
			std::uint16_t usage, std::uint8_t modifiers) const
		{
			if (!usage || m_hotkeyEditing.load(std::memory_order_acquire))
				return {};
			std::scoped_lock lock(m_hotkeyMutex);
			const auto found = std::ranges::find_if(m_hotkeySettings.bindings,
													[usage, modifiers](const Application::HotkeyBinding& binding) {
														return binding.keyboardUsage == usage &&
															   binding.keyboardModifiers == (modifiers & 0x0f);
													});
			return found == m_hotkeySettings.bindings.end() ? std::nullopt : std::optional{found->action};
		}

		void HandleControllerHotkeys(const ControllerState& current,
									 const ControllerState& previous)
		{
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			HandleRuntimeOverlayControllerNavigation();
#endif
			if (m_stopping.load(std::memory_order_acquire) ||
				m_hotkeyEditing.load(std::memory_order_acquire))
				return;
			std::optional<Application::HotkeyAction> action;
			{
				std::scoped_lock lock(m_hotkeyMutex);
				if (!m_hotkeySettings.controllerModifier ||
					!current.buttons.GetButtonState(*m_hotkeySettings.controllerModifier))
					return;
				for (const auto button : current.buttons.GetButtonList())
				{
					if (previous.buttons.GetButtonState(button))
						continue;
					const auto found = std::ranges::find_if(m_hotkeySettings.bindings,
															[button](const Application::HotkeyBinding& binding) {
																return binding.controllerButton == button;
															});
					if (found == m_hotkeySettings.bindings.end())
						continue;
					action = found->action;
					break;
				}
			}
			if (action)
				(void)PostToUi([this, action = *action] { ExecuteHotkey(action); });
		}

#if defined(CEMU_OVERLAY_BACKEND_CEF)
		void HandleRuntimeOverlayControllerNavigation()
		{
			std::array<bool, 7> current{};
			if (m_overlayInteraction.load(std::memory_order_acquire) !=
				RuntimeOverlay::Interaction::Passive)
			{
				for (int index = 0; index < InputManager::kMaxController; ++index)
				{
					const auto controller = InputManager::instance().get_controller(index);
					if (!controller)
						continue;
					current[0] = current[0] || controller->is_start_down();
					current[1] = current[1] || controller->is_a_down();
					current[2] = current[2] || controller->is_b_down();
					current[3] = current[3] || controller->is_left_down();
					current[4] = current[4] || controller->is_right_down();
					current[5] = current[5] || controller->is_up_down();
					current[6] = current[6] || controller->is_down_down();
				}
			}
			constexpr std::array<std::string_view, 7> actions{
				"input", "activate", "cancel", "left", "right", "up", "down"};
			for (std::size_t index = 0; index < current.size(); ++index)
			{
				if (!current[index] || m_overlayNavigationButtons[index])
					continue;
				const auto action = std::string(actions[index]);
				(void)PostToUi([this, action] {
					if (!m_cefOverlay || m_stopping.load(std::memory_order_acquire))
						return;
					const auto script =
						"window.dispatchEvent(new CustomEvent('cemu-overlay-navigate',{detail:" +
						JsonString(action) + "}))";
					m_cefOverlay->ExecuteScript(Host::PointerSurface::Main, script);
					m_cefOverlay->ExecuteScript(Host::PointerSurface::Pad, script);
				});
			}
			m_overlayNavigationButtons = current;
		}
#endif

		void ExecuteHotkey(Application::HotkeyAction action)
		{
			try
			{
				switch (action)
				{
				case Application::HotkeyAction::ToggleFullscreen:
				case Application::HotkeyAction::ToggleFullscreenAlternative:
					m_fullscreen = !m_fullscreen;
					m_nativeWindow->SetFullscreen(m_fullscreen);
					break;
				case Application::HotkeyAction::ExitFullscreen:
					if (m_fullscreen)
					{
						m_fullscreen = false;
						m_nativeWindow->SetFullscreen(false);
					}
					break;
				case Application::HotkeyAction::TakeScreenshot:
					RequestScreenshot();
					break;
				case Application::HotkeyAction::ToggleFastForward:
					ActiveSettings::SetTimerShiftFactor(
						ActiveSettings::GetTimerShiftFactor() < 3 ? 3 : 1);
					break;
				case Application::HotkeyAction::EndEmulation:
					StopEmulation();
					break;
				case Application::HotkeyAction::ExitApplication:
					(void)RequestShutdown();
					break;
				}
			} catch (const std::exception& error)
			{
				Emit("system.diagnostic", std::string(R"({"message":)") +
											  JsonString(error.what()) + "}");
			}
		}

		void RequestScreenshot()
		{
			if (!g_renderer)
				throw std::runtime_error("a title must be running before taking a screenshot");
			const bool saveToDisk = m_controller.GetFrontendSettings().saveScreenshots;
			const auto gate = m_callbackGate;
			const auto request = g_renderer->RequestScreenshot(
				[saveToDisk, gate](const std::vector<std::uint8_t>& pixels, int width, int height,
								   bool mainWindow) -> std::optional<std::string> {
					if (saveToDisk)
					{
						const auto path = ScreenshotPath(mainWindow);
						if (!path || !WriteRgbPng(*path, pixels, width, height))
							return std::string("Failed to save screenshot");
						return std::string("Screenshot saved to ") + _pathToUtf8(*path);
					}
					auto copy = std::make_shared<std::vector<std::uint8_t>>(pixels);
					bool queued{};
					{
						std::scoped_lock lock(gate->mutex);
						if (gate->target)
							queued = gate->target->PostToUi([gate, copy, width, height] {
								std::scoped_lock callbackLock(gate->mutex);
								if (!gate->target)
									return;
								if (!gate->target->m_nativeWindow->SetClipboardImage(*copy, width, height))
									gate->target->Emit("system.diagnostic",
													   R"({"message":"Failed to copy screenshot to the clipboard"})");
							});
					}
					return queued ? std::optional<std::string>("Screenshot copied to clipboard") : std::optional<std::string>("Failed to copy screenshot to clipboard");
				});
			if (!request)
				throw std::runtime_error("a screenshot request is already active");
		}

		void HandleNativeInput(const WebFrontend::NativeInputEvent& incoming)
		{
			if (m_stopping.load(std::memory_order_acquire) || !m_hostServices)
				return;
			// The overlay turns a key into the code Chromium expects by way of its USB
			// HID usage, which the window layer does not carry: on Linux it reports the
			// platform keyval and nothing else. Resolve it here, before the overlay is
			// offered the event, or an offscreen overlay - the software keyboard among
			// them - receives keystrokes it cannot interpret.
			auto event = incoming;
			if (event.kind == WebFrontend::NativeInputKind::Key && !event.usage)
				event.usage = UsbHidUsage(event.key);
			WebFrontend::NativeInputEvent normalized = event;
			if (normalized.kind == WebFrontend::NativeInputKind::Key && !normalized.usage)
				normalized.usage = UsbHidUsage(normalized.key);
			if (!normalized.deviceId)
				normalized.deviceId = normalized.kind == WebFrontend::NativeInputKind::Key ||
											  normalized.kind == WebFrontend::NativeInputKind::Character ||
											  normalized.kind == WebFrontend::NativeInputKind::TextComposition || normalized.kind == WebFrontend::NativeInputKind::TextAction
										  ? 1
										  : static_cast<std::uint16_t>(normalized.surface == Host::PointerSurface::Main ? 1 : 2);
			// Bounded native pointer trace: distinguish a missing GTK event from a
			// closed focus gate or an incorrect CEF target without logging each frame.
			const bool tracePointer = normalized.kind == WebFrontend::NativeInputKind::PointerButton ||
									  (normalized.kind == WebFrontend::NativeInputKind::PointerMove && m_webUiPointerOwnership[normalized.surface == Host::PointerSurface::Main ? 0 : 1]);
			static std::atomic_uint32_t pointerTraceCount{};
			const bool logPointer = tracePointer && pointerTraceCount.fetch_add(1, std::memory_order_relaxed) < 48;
			if (logPointer)
				cemuLog_log(LogType::Force, "CEX2-POINTER native kind={} surface={} x={} y={} button={} down={} allowed={}",
							static_cast<unsigned>(normalized.kind), static_cast<unsigned>(normalized.surface),
							normalized.x, normalized.y, normalized.button, normalized.pressed, InputAllowed(normalized.surface));
			if (m_managedInputFocus || m_nativeWindow->GetActiveInputWindow())
			{
				if (!m_managedInputFocus ||
					normalized.kind == WebFrontend::NativeInputKind::FocusLost)
					HandleMetrics(m_nativeWindow->GetMetrics());
				if (normalized.kind == WebFrontend::NativeInputKind::FocusLost)
					return; // Current activation supersedes a delayed loss from another surface.
				if (normalized.kind == WebFrontend::NativeInputKind::Key &&
					m_inputFocus.FilterKey(normalized.deviceId, normalized.usagePage, normalized.usage,
										   normalized.pressed, normalized.repeat,
										   InputAllowed(normalized.surface)))
					return;
				if (!InputAllowed(normalized.surface))
				{
					if (normalized.kind == WebFrontend::NativeInputKind::PointerButton &&
						normalized.button == 1 &&
						m_inputFocus.ResumeClick(SurfaceWindow(normalized.surface), normalized.deviceId,
												 normalized.pressed, normalized.insideContent))
						ApplyManagedInputFocus("resume-click");
					return;
				}
			}
			const bool keyboardStateChanged =
				normalized.kind == WebFrontend::NativeInputKind::Key && normalized.usage &&
				m_nativeKeyboardFocus.SetKey(normalized.usage, normalized.pressed);
			if (normalized.kind == WebFrontend::NativeInputKind::Key)
			{
				// Bounded end-to-end keyboard trace. This stops after the first
				// transitions of a launch process, but includes auto-repeat so a
				// synthetic GTK release can be distinguished from a held level.
				static std::atomic_uint32_t traceCount{};
				const auto trace = traceCount.fetch_add(1, std::memory_order_relaxed);
				if (trace < 128)
					cemuLog_log(LogType::Force,
								"CEX2-KEY frontend n={} key={} usage={} pressed={} repeat={} modifiers={} surface={}",
								trace, normalized.key, normalized.usage, normalized.pressed,
								normalized.repeat, normalized.modifiers,
								static_cast<unsigned>(normalized.surface));
			}

			auto& state = m_pointerStates[normalized.surface == Host::PointerSurface::Main ? 0 : 1];
			if (normalized.kind == WebFrontend::NativeInputKind::RawMouse)
			{
				if (normalized.contentWidth > 0)
					state.width = normalized.contentWidth;
				if (normalized.contentHeight > 0)
					state.height = normalized.contentHeight;
				if (state.width > 0 && state.height > 0)
				{
					if (!state.inside)
					{
						state.x = state.width / 2;
						state.y = state.height / 2;
					}
					state.x = std::clamp(state.x + normalized.deltaX, 0, state.width - 1);
					state.y = std::clamp(state.y + normalized.deltaY, 0, state.height - 1);
					state.inside = true;
					normalized.x = state.x;
					normalized.y = state.y;
					normalized.insideContent = true;
				}
			}
			else if ((normalized.kind == WebFrontend::NativeInputKind::PointerButton ||
					  normalized.kind == WebFrontend::NativeInputKind::PointerWheel) &&
					 PointerBridge(normalized.surface).RawMouseRequested() && state.inside)
			{
				normalized.x = state.x;
				normalized.y = state.y;
				normalized.insideContent = true;
			}
			WebFrontend::CefOverlay::OverlayInputTarget overlayTarget;
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			if (m_cefOverlay)
				overlayTarget = m_cefOverlay->ResolveInput(normalized);
#endif
			if (logPointer)
				cemuLog_log(LogType::Force, "CEX2-POINTER route target={} owner={} x={} y={} raw={}",
							overlayTarget.windowId, static_cast<unsigned>(overlayTarget.ownership.pointer),
							normalized.x, normalized.y, PointerBridge(normalized.surface).RawMouseRequested());
			const auto route = WebFrontend::CefOverlay::ResolveNativeInputRoute(
				normalized.kind, overlayTarget.ownership);
			auto& bridge = PointerBridge(normalized.surface);
			std::int32_t guestWheelY{};

			// Publish the physical stream before an interactive browser consumes the
			// frontend event. This is observation only: Cemu hotkeys and emulated-game
			// injection remain below the overlay gate. Keeping publication in one switch
			// also guarantees that ordinary, non-overlay input is not submitted twice.
			if (route.publishGuestPhysicalInput)
			{
				switch (normalized.kind)
				{
				case WebFrontend::NativeInputKind::PointerMove:
				{
					(void)RefreshPointerPolicy(normalized.surface);
					state = {normalized.x, normalized.y, normalized.contentWidth,
							 normalized.contentHeight, normalized.insideContent};
					const auto motion = bridge.UpdatePosition(
						{normalized.x, normalized.y},
						{normalized.contentWidth / 2, normalized.contentHeight / 2},
						bridge.RawMouseRequested());
					SubmitPointer(normalized, motion.delta.x, motion.delta.y, 0, 0, 0, 0,
								  motion.rawRelative);
					break;
				}
				case WebFrontend::NativeInputKind::PointerButton:
				{
					(void)RefreshPointerPolicy(normalized.surface);
					state = {normalized.x, normalized.y, normalized.contentWidth,
							 normalized.contentHeight, normalized.insideContent};
					const auto mask = PointerButtonMask(normalized.button);
					const auto update = bridge.UpdateButtons(
						normalized.pressed ? Frontend::CemuExtendMouseTransition::Down
										   : Frontend::CemuExtendMouseTransition::Up,
						mask, 0, normalized.deviceId);
					SubmitPointer(normalized, 0, 0, 0, 0, update.changed,
								  update.deviceButtons, false);
					break;
				}
				case WebFrontend::NativeInputKind::PointerWheel:
				{
					const auto wheelX = bridge.NormalizeWheel(normalized.wheelX, 120, true);
					guestWheelY = bridge.NormalizeWheel(normalized.wheelY, 120, false);
					SubmitPointer(normalized, 0, 0, wheelX, guestWheelY, 0, 0, false);
					break;
				}
				case WebFrontend::NativeInputKind::RawMouse:
					(void)RefreshPointerPolicy(normalized.surface);
					if (!bridge.RawMouseRequested())
						break;
					bridge.MarkRawMouseSeen();
					bridge.RecordRawPosition({state.x, state.y});
					SubmitPointer(normalized, normalized.deltaX, normalized.deltaY, 0, 0, 0, 0,
								  true);
					break;
				case WebFrontend::NativeInputKind::Key:
					if (normalized.usage)
					{
						// Receiving a new native key-down proves that the game window
						// owns keyboard input. Publish focus first so the ordered guest
						// stream cannot reject this press using a stale GTK metric.
						if (!m_managedInputFocus && normalized.pressed && keyboardStateChanged)
							ConfirmNativeKeyboardFocus();
						m_controller.SubmitKeyboard(normalized.usagePage, normalized.usage,
													normalized.pressed,
													normalized.modifiers, normalized.deviceId);
					}
					break;
				case WebFrontend::NativeInputKind::FocusLost:
					ReleaseNativeInput(false, false);
					ConfirmNativeFocusLoss();
					break;
				default:
					break;
				}
			}

#if defined(CEMU_OVERLAY_BACKEND_CEF)
			if (route.sendOverlayInput)
			{
				if (WebFrontend::CefOverlay::IsPointerInput(normalized.kind))
				{
					// An interactive OSR layer owns the real host pointer. Do not make
					// cursor release depend on the guest's mapped-injection permission:
					// without this override an overlay opened during gameplay remains in
					// relative/raw mode and never receives an absolute mouse position.
					m_nativeWindow->ApplyPointerPresentation({
						.surface = normalized.surface,
						.ownsPointer = true,
						.showCursor = true,
					});
				}
				(void)m_cefOverlay->SendInput(normalized, overlayTarget.windowId);
				if (!route.processFrontendInput)
					return;
			}
#endif
			if (!route.processFrontendInput)
				return;

			switch (normalized.kind)
			{
			case WebFrontend::NativeInputKind::PointerMove:
				m_hostServices->UpdateMousePosition(normalized.surface,
													{normalized.x, normalized.y});
				break;
			case WebFrontend::NativeInputKind::PointerButton:
				if (normalized.button == 1 || normalized.button == 3)
					m_hostServices->UpdateMouseButton(
						normalized.surface,
						normalized.button == 1 ? Host::PointerButton::Left
											   : Host::PointerButton::Right,
						normalized.pressed, {normalized.x, normalized.y});
				break;
			case WebFrontend::NativeInputKind::PointerWheel:
				m_hostServices->UpdateMouseWheel(
					static_cast<float>(normalized.wheelY) / 120.0f, guestWheelY);
				break;
			case WebFrontend::NativeInputKind::RawMouse:
				break;
			case WebFrontend::NativeInputKind::Touch:
				m_hostServices->UpdateTouch(normalized.surface,
											{normalized.x, normalized.y}, normalized.pressed);
				break;
			case WebFrontend::NativeInputKind::Key:
			{
				m_hostServices->UpdateKey(normalized.key, normalized.pressed);
				const auto usage = normalized.usage;
				if (!normalized.pressed && !normalized.repeat &&
					!m_controller.SoftwareKeyboardActive())
					if (const auto action = MatchKeyboardHotkey(usage, normalized.modifiers))
						ExecuteHotkey(*action);
				break;
			}
			case WebFrontend::NativeInputKind::Character:
				if (!m_textInputSequence)
					for (const auto codepoint : DecodeUtf8(normalized.text))
						if (codepoint >= 0x20 && codepoint != 0x7f)
							m_controller.SubmitText(codepoint, normalized.repeat);
				break;
			case WebFrontend::NativeInputKind::FocusLost:
				break;
			case WebFrontend::NativeInputKind::DeviceChanged:
				ReleaseNativeInput(false);
				m_hostServices->NotifyDeviceChanged();
				break;
			case WebFrontend::NativeInputKind::TextComposition:
				if (normalized.textSequence == m_textInputSequence && m_textInputSequence != 0)
					m_controller.SubmitTextComposition(
						normalized.text, normalized.preedit, normalized.textCursor,
						normalized.selectionLength);
				break;
			case WebFrontend::NativeInputKind::TextAction:
				if (normalized.textSequence == m_textInputSequence && m_textInputSequence != 0)
					m_controller.SubmitTextAction(normalized.textAccepted, normalized.text);
				break;
			}
		}

		void RefreshTextInput()
		{
			if (!InputAllowed(Host::PointerSurface::Main))
			{
				m_textInputSequence = 0;
				m_nativeWindow->UpdateTextInput({});
				return;
			}
			const auto state = m_controller.GetTextInputState();
			m_textInputSequence = state.active ? state.sequence : 0;
			m_nativeWindow->UpdateTextInput({
				.active = state.active,
				.sequence = state.sequence,
				.initialText = state.initialText,
				.maximumLength = state.maximumLength,
				.caretX = state.caretX,
				.caretY = state.caretY,
				.lineHeight = state.lineHeight,
			});
			if (state.active)
				m_controller.KeyboardFocusLost();
		}

		bool RecreateCanvasForHost()
		{
			if (!m_windowState || m_windowState->Snapshot().mode !=
									  WebFrontend::MainWindowContentMode::Playing)
				return false;
			if (!ClosePadRenderRegion())
				return false;
			ReleaseNativeInput(true);
			try
			{
				// Canvas recreation is requested only after the previous Latte thread
				// has stopped. Still abandon any retained renderer before GTK can
				// unrealize the X11 child that owns its VkSurfaceKHR.
				m_rendererHost->AbandonMainInitialization();
				DestroyMainRuntimeOverlay();
				m_nativeWindow->DestroyMainRenderRegion();
				auto& region = m_nativeWindow->CreateMainRenderRegion();
				m_nativeWindow->SetFullscreen(m_fullscreen);
				// A hidden GTK Wayland window has no wl_surface. Map the game window
				// before publishing its native handle to CEF/Vulkan.
				ShowRenderContent();
				CreateMainRuntimeOverlay(region);
				m_hostState->UpdateMetrics(m_nativeWindow->GetMetrics());
				m_rendererHost->InitializeMain(region);
				RefreshTextInput();
				return true;
			} catch (const std::exception& error)
			{
				cemuLog_log(LogType::Force, "Native canvas recreation failed: {}", error.what());
				try
				{
					m_rendererHost->AbandonMainInitialization();
				} catch (...)
				{}
				DestroyMainRuntimeOverlay();
				m_nativeWindow->DestroyMainRenderRegion();
				ShowLibraryContent();
				m_hostState->UpdateMetrics(m_nativeWindow->GetMetrics());
				return false;
			}
		}

		bool PostToUi(std::function<void()> action)
		{
			if (m_stopping.load(std::memory_order_acquire))
				return false;
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			auto stopping = m_eventStopping;
			return WebFrontend::CefNative::PostNativeUi(
				[stopping = std::move(stopping), action = std::move(action)]() mutable {
					if (!stopping->load(std::memory_order_acquire) && action)
						action();
				});
#else
			return false;
#endif
		}

		void Emit(std::string_view type, std::string_view payloadJson,
				  std::function<void()> beforeDispatch = {})
		{
			std::scoped_lock eventLock(m_eventMutex);
			if (m_stopping.load(std::memory_order_acquire))
				return;
			const auto sequence = ++m_eventSequence;
			const std::string eventType(type);
			const std::string payload(payloadJson);
			(void)PostToUi([this, beforeDispatch = std::move(beforeDispatch), eventType,
							payload, sequence] {
				if (beforeDispatch)
					beforeDispatch();
#if defined(CEMU_OVERLAY_BACKEND_CEF)
				if (m_cefOverlay)
				{
					m_cefOverlay->ExecuteWindowEvent(0, eventType, payload, sequence);
					m_cefOverlay->ExecuteWindowEvent(kMainOverlayWindowId, eventType, payload, sequence);
					m_cefOverlay->ExecuteWindowEvent(kPadOverlayWindowId, eventType, payload, sequence);
					for (const auto& [id, window] : m_toolWindows)
					{
						(void)window;
						m_cefOverlay->ExecuteWindowEvent(id, eventType, payload, sequence);
					}
				}
#endif
			});
		}

		void EmitToWindow(std::uint64_t windowId, std::string_view type,
						  std::string_view payloadJson)
		{
			std::scoped_lock eventLock(m_eventMutex);
			if (m_stopping.load(std::memory_order_acquire))
				return;
			const auto sequence = ++m_eventSequence;
			const std::string eventType(type);
			const std::string payload(payloadJson);
			(void)PostToUi([this, windowId, eventType, payload, sequence] {
#if defined(CEMU_OVERLAY_BACKEND_CEF)
				if (m_cefOverlay)
					m_cefOverlay->ExecuteWindowEvent(windowId, eventType, payload, sequence);
#endif
			});
		}

		void ForwardEvent(const Application::Event& event)
		{
			switch (event.type)
			{
			case Application::EventType::LoadingStarted:
				Emit("emulation.loading", "{}");
				break;
			case Application::EventType::GameLoaded:
				Emit("emulation.loaded", "{}", [this] {
					(void)PostToUi([this] { FlushRuntimeOverlay(); });
				});
				break;
			case Application::EventType::GameExited:
			{
				const auto expectedGeneration = m_windowState->Snapshot().generation;
				Emit("emulation.exited", "{}", [this, expectedGeneration] {
					const auto state = m_windowState->Snapshot();
					if (state.mode != WebFrontend::MainWindowContentMode::Playing ||
						state.generation != expectedGeneration)
						return;
					if (!DestroyMainRenderRegion())
						return;
					(void)m_windowState->FinishEmulation();
					if (m_commandLineLaunch)
					{
						m_exitCode = m_controller.ForegroundProcessExitStatus().value_or(EXIT_SUCCESS);
						(void)RequestShutdown();
					}
					else
						FinishGameWindowLifetime();
				});
				break;
			}
			case Application::EventType::PpcProcessExited:
				Emit("emulation.processExited", std::string(R"({"status":)") +
													std::to_string(event.processStatus) + "}");
				break;
			case Application::EventType::PerformanceUpdated:
				Emit("emulation.performance", std::string(R"({"fps":)") +
												  std::to_string(event.framesPerSecond) + "}");
				break;
			case Application::EventType::Diagnostic:
				Emit("system.diagnostic", std::string(R"({"message":)") +
											  JsonString(event.diagnostic) + "}");
				break;
			case Application::EventType::GameListRefreshRequested:
				Emit("titles.changed", "{}");
				break;
			case Application::EventType::TextInputWakeRequested:
				Emit("input.textWakeRequested", "{}", [this] { RefreshTextInput(); });
				break;
			}
		}

		static std::string_view LoggingLevelName(Application::LoggingLevel level)
		{
			switch (level)
			{
			case Application::LoggingLevel::Warning:
				return "warning";
			case Application::LoggingLevel::Error:
				return "error";
			default:
				return "info";
			}
		}

		static std::string LoggingSnapshotJson(const Application::LoggingSnapshot& snapshot)
		{
			rapidjson::StringBuffer buffer;
			JsonWriter writer(buffer);
			writer.StartObject();
			writer.Key("entries");
			writer.StartArray();
			for (const auto& entry : snapshot.entries)
			{
				writer.StartObject();
				writer.Key("sequence");
				writer.String(std::to_string(entry.sequence).c_str());
				writer.Key("level");
				writer.String(LoggingLevelName(entry.level).data());
				writer.Key("category");
				writer.String(entry.category.data(),
							  static_cast<rapidjson::SizeType>(entry.category.size()));
				writer.Key("message");
				writer.String(entry.message.data(),
							  static_cast<rapidjson::SizeType>(entry.message.size()));
				writer.EndObject();
			}
			writer.EndArray();
			writer.Key("firstAvailableSequence");
			writer.String(std::to_string(snapshot.firstAvailableSequence).c_str());
			writer.Key("nextSequence");
			writer.String(std::to_string(snapshot.nextSequence).c_str());
			writer.Key("droppedEntries");
			writer.String(std::to_string(snapshot.droppedEntries).c_str());
			writer.Key("retainedBytes");
			writer.String(std::to_string(snapshot.retainedBytes).c_str());
			writer.Key("truncated");
			writer.Bool(snapshot.truncated);
			writer.EndObject();
			return {buffer.GetString(), buffer.GetSize()};
		}

		void SignalLoggingChanged()
		{
			if (m_loggingFlushPending.exchange(true, std::memory_order_acq_rel))
				return;
			// PendingEvent owns the shutdown token and suppresses this callback once
			// cleanup begins. Do not hold the callback lifetime gate while flushing:
			// a flush may synchronously publish another model change.
			if (!PostToUi([this] { FlushLoggingEvents(); }))
				m_loggingFlushPending.store(false, std::memory_order_release);
		}

		void SignalRuntimeOverlayChanged()
		{
			if (m_overlayFlushPending.exchange(true, std::memory_order_acq_rel))
				return;
			// GetRuntimeOverlaySnapshot() can prune an expired notice and invoke the
			// model change handler synchronously. Keeping m_callbackGate locked here
			// would therefore try to acquire the same non-recursive mutex again on
			// the UI thread and freeze both the launcher and LatteThread.
			if (!PostToUi([this] { FlushRuntimeOverlay(); }))
				m_overlayFlushPending.store(false, std::memory_order_release);
		}

		void SignalFramePresented(bool mainWindow)
		{
			auto& pending = mainWindow ? m_mainFrameRedrawPending : m_padFrameRedrawPending;
			if (pending.exchange(true, std::memory_order_acq_rel))
				return;
			if (!PostToUi([this, mainWindow] {
					auto& pending = mainWindow ? m_mainFrameRedrawPending : m_padFrameRedrawPending;
					pending.store(false, std::memory_order_release);
					m_nativeWindow->RequestRenderRedraw(
						mainWindow ? Host::PointerSurface::Main : Host::PointerSurface::Pad);
				}))
				pending.store(false, std::memory_order_release);
		}

		void FlushRuntimeOverlay()
		{
			m_overlayFlushPending.store(false, std::memory_order_release);
			const auto snapshot = m_controller.GetRuntimeOverlaySnapshot();
			const auto payload = RuntimeOverlayJson(snapshot,
													m_inputFocus.Waiting(WebFrontend::InputWindow::Game), m_inputFocus.Revision());
			const auto padPayload = RuntimeOverlayJson(snapshot,
													   m_inputFocus.Waiting(WebFrontend::InputWindow::GamePad), m_inputFocus.Revision());
			m_overlayInteraction.store(snapshot.interaction, std::memory_order_release);
#if defined(CEMU_OVERLAY_BACKEND_CEF)
			if (m_cefOverlay)
			{
				const bool interactive = snapshot.interaction != RuntimeOverlay::Interaction::Passive;
				m_cefOverlay->SetInteractive(Host::PointerSurface::Main, interactive);
				m_cefOverlay->SetInteractive(Host::PointerSurface::Pad, interactive);
				std::scoped_lock eventLock(m_eventMutex);
				const auto sequence = ++m_eventSequence;
				m_cefOverlay->ExecuteEvent(Host::PointerSurface::Main, "overlay.changed", payload, sequence);
				m_cefOverlay->ExecuteEvent(Host::PointerSurface::Pad, "overlay.changed", padPayload, sequence);
			}
#endif
		}

		void FlushLoggingEvents()
		{
			m_loggingFlushPending.store(false, std::memory_order_release);
			const auto snapshot = m_logging.Snapshot(m_lastForwardedLogSequence, 128);
			std::optional<std::uint64_t> target;
			if (m_mainWorkspaceRole == "logging")
				target = 0;
			else if (const auto loggingWindow = m_windowByRole.find("logging");
					 loggingWindow != m_windowByRole.end())
				target = loggingWindow->second;
			if (!target)
			{
				m_lastForwardedLogSequence = snapshot.nextSequence == 0 ? 0 : snapshot.nextSequence - 1;
				return;
			}
			if (!snapshot.entries.empty())
			{
				m_lastForwardedLogSequence = snapshot.entries.back().sequence;
				EmitToWindow(*target, "logging.entries",
							 LoggingSnapshotJson(snapshot));
			}
			if (snapshot.truncated)
				SignalLoggingChanged();
		}

		std::string DispatchCefRpc(std::uint64_t windowId, std::string_view request)
		{
			const auto previousWindow = std::exchange(m_invokingWindow, windowId);
			std::string response;
			try
			{
				response = m_rpc.Dispatch(request);
			} catch (...)
			{
				m_invokingWindow = previousWindow;
				throw;
			}
			m_invokingWindow = previousWindow;

			// CEF completes Callback::Success after this handler returns. Queue close
			// work on the native loop so the RPC response is delivered first.
			if (windowId == 0 && m_mainReplyPending)
			{
				(void)PostToUi([this] {
					m_mainReplyPending = false;
					MaybeTerminateAfterShutdown();
				});
			}
			else if (windowId != 0)
			{
				const auto found = m_toolWindows.find(windowId);
				if (found != m_toolWindows.end() &&
					std::exchange(found->second->closeRequested, false))
					(void)PostToUi([this, windowId] { RequestToolWindowClose(windowId); });
			}
			return response;
		}

		void HandleCefWindowClosed(std::uint64_t windowId)
		{
			if (windowId == 0)
			{
#if defined(CEMU_OVERLAY_BACKEND_CEF)
				if (!m_stopping.load(std::memory_order_acquire))
					WebFrontend::CefNative::QuitNativeUiLoop();
#endif
				return;
			}
			if (windowId != kMainOverlayWindowId && windowId != kPadOverlayWindowId)
				CloseToolWindow(windowId);
		}

		void RequireRole(std::initializer_list<std::string_view> roles) const
		{
			const auto role = m_invokingWindow == 0
								  ? std::string_view(m_mainWorkspaceRole)
								  : RoleForWindow(m_invokingWindow);
			if (std::ranges::find(roles, role) == roles.end())
				throw std::runtime_error("this RPC method is not available to the current window role");
		}

		std::uint64_t InvokingWindowGeneration() const
		{
			if (m_invokingWindow == 0)
				return 0;
			const auto window = m_toolWindows.find(m_invokingWindow);
			if (window == m_toolWindows.end())
				throw std::runtime_error("the owning window is no longer active");
			return window->second->generation;
		}

		void* InvokingNativeWindow() const
		{
			if (m_invokingWindow == 0)
				return m_nativeWindow->GetNativeWindow();
			const auto window = m_toolWindows.find(m_invokingWindow);
			if (window == m_toolWindows.end() || !window->second->nativeSupport)
				throw std::runtime_error("the owning window is no longer active");
			return window->second->nativeSupport->GetWindow();
		}

		std::string AccountManagerJson() const
		{
			const auto snapshot = m_controller.GetAccountManagerSnapshot();
			rapidjson::StringBuffer buffer;
			JsonWriter writer(buffer);
			writer.StartObject();
			writer.Key("accounts");
			writer.StartArray();
			for (const auto& account : snapshot.accounts)
				WriteAccount(writer, account);
			writer.EndArray();
			writer.Key("countries");
			writer.StartArray();
			for (const auto& country : snapshot.countries)
			{
				writer.StartObject();
				writer.Key("code");
				writer.Uint(country.code);
				writer.Key("name");
				writer.String(country.name.data(),
							  static_cast<rapidjson::SizeType>(country.name.size()));
				writer.EndObject();
			}
			writer.EndArray();
			writer.Key("nextPersistentId");
			writer.Uint(snapshot.nextPersistentId);
			writer.Key("hasFreeSlots");
			writer.Bool(snapshot.hasFreeSlots);
			writer.Key("activePersistentId");
			writer.Uint(snapshot.activePersistentId);
			writer.Key("titleRunning");
			writer.Bool(snapshot.titleRunning);
			writer.Key("networkSettings");
			writer.StartArray();
			for (const auto& setting : snapshot.networkSettings)
			{
				writer.StartObject();
				writer.Key("persistentId");
				writer.Uint(setting.persistentId);
				writer.Key("service");
				writer.String(AccountNetworkServiceName(setting.service).data());
				writer.Key("validation");
				writer.StartObject();
				writer.Key("validAccount");
				writer.Bool(setting.validation.validAccount);
				writer.Key("otp");
				writer.String(AccountFileStateName(setting.validation.otp).data());
				writer.Key("seeprom");
				writer.String(AccountFileStateName(setting.validation.seeprom).data());
				writer.Key("missingFiles");
				writer.StartArray();
				for (const auto& file : setting.validation.missingFiles)
				{
					const auto value = boost::nowide::narrow(file);
					writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
				}
				writer.EndArray();
				writer.Key("accountError");
				writer.String(AccountOnlineErrorName(setting.validation.accountError).data());
				writer.EndObject();
				writer.EndObject();
			}
			writer.EndArray();
			const auto& environment = snapshot.onlineEnvironment;
			writer.Key("onlineEnvironment");
			writer.StartObject();
			writer.Key("requiredFilesAvailable");
			writer.Bool(environment.requiredFilesAvailable);
			writer.Key("otpPresent");
			writer.Bool(environment.otpPresent);
			writer.Key("seepromPresent");
			writer.Bool(environment.seepromPresent);
			writer.Key("consoleCertificateAvailable");
			writer.Bool(environment.consoleCertificateAvailable);
			writer.EndObject();
			writer.EndObject();
			return {buffer.GetString(), buffer.GetSize()};
		}

		std::string GraphicPacksJson() const
		{
			rapidjson::StringBuffer buffer;
			JsonWriter writer(buffer);
			writer.StartArray();
			for (const auto& pack : m_controller.ListGraphicPacks())
				WriteGraphicPack(writer, pack);
			writer.EndArray();
			return {buffer.GetString(), buffer.GetSize()};
		}

		std::string InputSettingsJson() const
		{
			const auto model = m_controller.GetInputSettings();
			rapidjson::StringBuffer buffer;
			JsonWriter writer(buffer);
			auto typeName = [](Application::EmulatedControllerType type) {
				switch (type)
				{
				case Application::EmulatedControllerType::GamePad:
					return "gamePad";
				case Application::EmulatedControllerType::ProController:
					return "proController";
				case Application::EmulatedControllerType::ClassicController:
					return "classicController";
				case Application::EmulatedControllerType::Wiimote:
					return "wiimote";
				default:
					return "disabled";
				}
			};
			auto writeAxis = [&writer](const Application::ControllerAxisSettings& value) {
				writer.StartObject();
				writer.Key("deadzone");
				writer.Double(value.deadzone);
				writer.Key("range");
				writer.Double(value.range);
				writer.EndObject();
			};
			writer.StartObject();
			writer.Key("generation");
			writer.String(std::to_string(model.generation).c_str());
			writer.Key("profiles");
			writer.StartArray();
			for (const auto& profile : model.profiles)
				writer.String(profile.data(), profile.size());
			writer.EndArray();
			writer.Key("availableApis");
			writer.StartArray();
			for (const auto& api : model.availableApis)
				writer.String(api.data(), api.size());
			writer.EndArray();
			writer.Key("players");
			writer.StartArray();
			for (const auto& player : model.players)
			{
				writer.StartObject();
				writer.Key("player");
				writer.Uint(player.player);
				writer.Key("type");
				writer.String(typeName(player.type));
				writer.Key("gameProfileLocked");
				writer.Bool(player.gameProfileLocked);
				writer.Key("profileName");
				writer.String(player.profileName.data(), player.profileName.size());
				writer.Key("controllers");
				writer.StartArray();
				for (const auto& controller : player.controllers)
				{
					writer.StartObject();
					writer.Key("token");
					writer.Uint64(controller.token);
					writer.Key("api");
					writer.String(controller.api.data(), controller.api.size());
					writer.Key("displayName");
					writer.String(controller.displayName.data(), controller.displayName.size());
					writer.Key("connected");
					writer.Bool(controller.connected);
					writer.Key("hasBattery");
					writer.Bool(controller.hasBattery);
					writer.Key("lowBattery");
					writer.Bool(controller.lowBattery);
					writer.Key("hasMotion");
					writer.Bool(controller.hasMotion);
					writer.Key("hasRumble");
					writer.Bool(controller.hasRumble);
					if (controller.wiimoteExtension)
					{
						writer.Key("wiimoteExtension");
						writer.String(controller.wiimoteExtension->data(), controller.wiimoteExtension->size());
					}
					writer.Key("settings");
					writer.StartObject();
					writer.Key("axis");
					writeAxis(controller.settings.axis);
					writer.Key("rotation");
					writeAxis(controller.settings.rotation);
					writer.Key("trigger");
					writeAxis(controller.settings.trigger);
					writer.Key("rumble");
					writer.Double(controller.settings.rumble);
					writer.Key("motion");
					writer.Bool(controller.settings.motion);
					if (controller.settings.packetDelay)
					{
						writer.Key("packetDelay");
						writer.Uint(*controller.settings.packetDelay);
					}
					writer.EndObject();
					writer.EndObject();
				}
				writer.EndArray();
				writer.Key("mappings");
				writer.StartArray();
				for (const auto& mapping : player.mappings)
				{
					writer.StartObject();
					writer.Key("mappingId");
					writer.Uint64(mapping.mappingId);
					writer.Key("label");
					writer.String(mapping.label.data(), mapping.label.size());
					writer.Key("binding");
					writer.String(mapping.binding.data(), mapping.binding.size());
					if (mapping.controllerToken)
					{
						writer.Key("controllerToken");
						writer.Uint64(*mapping.controllerToken);
					}
					writer.EndObject();
				}
				writer.EndArray();
				writer.EndObject();
			}
			writer.EndArray();
			writer.EndObject();
			return {buffer.GetString(), buffer.GetSize()};
		}

		std::string InputMutationJson(const Application::InputSettingsResult& result) const
		{
			if (!result)
				throw std::runtime_error(result.diagnostic.empty() ? "input settings operation failed" : result.diagnostic);
			return InputSettingsJson();
		}

		std::string HotkeySettingsJson(
			const Application::HotkeySettingsModel& model) const
		{
			rapidjson::StringBuffer buffer;
			JsonWriter writer(buffer);
			writer.StartObject();
			writer.Key("revision");
			writer.Uint64(model.revision);
			writer.Key("controllerModifier");
			if (model.controllerModifier)
				writer.Uint(*model.controllerModifier);
			else
				writer.Null();
			writer.Key("controllerModifierLabel");
			writer.String(
				model.controllerModifierLabel.data(),
				static_cast<rapidjson::SizeType>(model.controllerModifierLabel.size()));
			writer.Key("controller");
			if (model.controller)
			{
				writer.StartObject();
				writer.Key("token");
				writer.Uint64(model.controller->token);
				writer.Key("displayName");
				writer.String(model.controller->displayName.data(),
							  static_cast<rapidjson::SizeType>(model.controller->displayName.size()));
				writer.EndObject();
			}
			else
				writer.Null();
			writer.Key("bindings");
			writer.StartArray();
			for (const auto& binding : model.bindings)
			{
				writer.StartObject();
				writer.Key("action");
				writer.String(HotkeyActionName(binding.action).data());
				writer.Key("keyboardUsage");
				writer.Uint(binding.keyboardUsage);
				writer.Key("keyboardModifiers");
				writer.Uint(binding.keyboardModifiers);
				writer.Key("controllerButton");
				if (binding.controllerButton)
					writer.Uint(*binding.controllerButton);
				else
					writer.Null();
				writer.Key("controllerLabel");
				writer.String(binding.controllerLabel.data(),
							  static_cast<rapidjson::SizeType>(binding.controllerLabel.size()));
				writer.EndObject();
			}
			writer.EndArray();
			writer.EndObject();
			return {buffer.GetString(), buffer.GetSize()};
		}

		static void WriteUsbDescriptor(JsonWriter& writer,
									   const Application::UsbDeviceDescriptor& device)
		{
			writer.StartObject();
			writer.Key("id");
			writer.String(device.id.data(), device.id.size());
			writer.Key("vendorId");
			writer.Uint(device.vendorId);
			writer.Key("productId");
			writer.Uint(device.productId);
			writer.Key("interfaceIndex");
			writer.Uint(device.interfaceIndex);
			writer.Key("interfaceSubClass");
			writer.Uint(device.interfaceSubClass);
			writer.Key("protocol");
			writer.Uint(device.protocol);
			writer.Key("maxPacketSizeRx");
			writer.Uint(device.maxPacketSizeRx);
			writer.Key("maxPacketSizeTx");
			writer.Uint(device.maxPacketSizeTx);
			writer.Key("opened");
			writer.Bool(device.opened);
			writer.EndObject();
		}

		std::string UsbModelJson(Application::EmulatedUsbModel model) const
		{
			rapidjson::StringBuffer buffer;
			JsonWriter writer(buffer);
			writer.StartObject();
			writer.Key("generation");
			writer.Uint64(model.generation);
			writer.Key("emulatedDevices");
			writer.StartArray();
			for (const auto& device : model.emulatedDevices)
			{
				writer.StartObject();
				writer.Key("id");
				writer.String(device.id.data(), device.id.size());
				writer.Key("name");
				writer.String(device.name.data(), device.name.size());
				writer.Key("vendorId");
				writer.Uint(device.vendorId);
				writer.Key("productId");
				writer.Uint(device.productId);
				writer.Key("enabled");
				writer.Bool(device.enabled);
				writer.Key("connected");
				writer.Bool(device.connected);
				writer.EndObject();
			}
			writer.EndArray();
			writer.Key("attachedDevices");
			writer.StartArray();
			for (const auto& device : model.attachedDevices)
				WriteUsbDescriptor(writer, device);
			writer.EndArray();
			writer.EndObject();
			return {buffer.GetString(), buffer.GetSize()};
		}

		std::string UsbDeviceChangeJson(const Application::UsbDeviceChange& change) const
		{
			rapidjson::StringBuffer buffer;
			JsonWriter writer(buffer);
			writer.StartObject();
			writer.Key("generation");
			writer.String(std::to_string(change.generation).c_str());
			writer.Key("attached");
			writer.Bool(change.attached);
			writer.Key("device");
			WriteUsbDescriptor(writer, change.device);
			writer.EndObject();
			return {buffer.GetString(), buffer.GetSize()};
		}

		std::string HotkeySettingsResultJson(
			const Application::HotkeySettingsResult& result) const
		{
			return std::string(R"({"ok":)") + (result ? "true" : "false") +
				   R"(,"error":)" + JsonString(HotkeySettingsErrorName(result.error)) +
				   R"(,"snapshot":)" + HotkeySettingsJson(result.snapshot) +
				   R"(,"diagnostic":)" + JsonString(result.diagnostic) + "}";
		}

		std::string TextureDiagnosticJson(const Application::TextureDiagnosticPage& page) const
		{
			rapidjson::StringBuffer buffer;
			JsonWriter writer(buffer);
			writer.StartObject();
			writer.Key("generation");
			writer.String(std::to_string(page.generation).c_str());
			writer.Key("offset");
			writer.Uint64(page.offset);
			writer.Key("total");
			writer.Uint64(page.total);
			writer.Key("truncated");
			writer.Bool(page.truncated);
			writer.Key("available");
			writer.Bool(page.available);
			writer.Key("diagnostic");
			writer.String(page.diagnostic.data(), page.diagnostic.size());
			writer.Key("rows");
			writer.StartArray();
			for (const auto& row : page.rows)
			{
				writer.StartObject();
				writer.Key("id");
				writer.String(row.id.data(), row.id.size());
				if (!row.parentId.empty())
				{
					writer.Key("parentId");
					writer.String(row.parentId.data(), row.parentId.size());
				}
				writer.Key("kind");
				writer.String(row.view ? "view" : "texture");
				writer.Key("active");
				writer.Bool(row.active);
				writer.Key("updatedOnGpu");
				writer.Bool(row.updatedOnGpu);
				writer.Key("depthFormat");
				writer.Bool(row.depthFormat);
				writer.Key("dimension");
				writer.String(row.dimension.data(), row.dimension.size());
				writer.Key("format");
				writer.String(row.format.data(), row.format.size());
				writer.Key("width");
				writer.Uint(row.width);
				writer.Key("height");
				writer.Uint(row.height);
				writer.Key("depth");
				writer.Uint(row.depth);
				writer.Key("pitch");
				writer.Uint(row.pitch);
				writer.Key("tileMode");
				writer.Uint(row.tileMode);
				writer.Key("firstSlice");
				writer.Uint(row.firstSlice);
				writer.Key("sliceCount");
				writer.Uint(row.sliceCount);
				writer.Key("firstMip");
				writer.Uint(row.firstMip);
				writer.Key("mipCount");
				writer.Uint(row.mipCount);
				writer.Key("ageMilliseconds");
				writer.Uint(row.ageMilliseconds);
				writer.Key("alternativeViewCount");
				writer.Uint(row.alternativeViewCount);
				writer.Key("resolutionOverridden");
				writer.Bool(row.resolutionOverridden);
				writer.Key("effectiveWidth");
				writer.Uint(row.effectiveWidth);
				writer.Key("effectiveHeight");
				writer.Uint(row.effectiveHeight);
				writer.Key("effectiveDepth");
				writer.Uint(row.effectiveDepth);
				writer.EndObject();
			}
			writer.EndArray();
			writer.EndObject();
			return {buffer.GetString(), buffer.GetSize()};
		}

		std::string AudioDiagnosticJson(const Application::AudioVoiceDiagnosticPage& page) const
		{
			rapidjson::StringBuffer buffer;
			JsonWriter writer(buffer);
			writer.StartObject();
			writer.Key("generation");
			writer.String(std::to_string(page.generation).c_str());
			writer.Key("offset");
			writer.Uint64(page.offset);
			writer.Key("total");
			writer.Uint64(page.total);
			writer.Key("available");
			writer.Bool(page.available);
			writer.Key("diagnostic");
			writer.String(page.diagnostic.data(), page.diagnostic.size());
			writer.Key("rows");
			writer.StartArray();
			for (const auto& row : page.rows)
			{
				writer.StartObject();
				writer.Key("id");
				writer.String(row.id.data(), row.id.size());
				writer.Key("index");
				writer.Uint(row.index);
				writer.Key("format");
				writer.String(row.format.data(), row.format.size());
				writer.Key("currentOffset");
				writer.Uint(row.currentOffset);
				writer.Key("loopOffset");
				writer.Uint(row.loopOffset);
				writer.Key("endOffset");
				writer.Uint(row.endOffset);
				writer.Key("looping");
				writer.Bool(row.looping);
				writer.Key("volume");
				writer.Uint(row.volume);
				writer.Key("volumeDelta");
				writer.Int(row.volumeDelta);
				writer.Key("sourceRatio");
				writer.Uint(row.sourceRatio);
				writer.Key("lowPassEnabled");
				writer.Bool(row.lowPassEnabled);
				writer.Key("biquadEnabled");
				writer.Bool(row.biquadEnabled);
				writer.Key("deviceMix");
				writer.String(row.deviceMix.data(), row.deviceMix.size());
				writer.EndObject();
			}
			writer.EndArray();
			writer.EndObject();
			return {buffer.GetString(), buffer.GetSize()};
		}

		static Application::EmulatedControllerType ParseInputControllerType(
			std::string_view value)
		{
			if (value == "disabled")
				return Application::EmulatedControllerType::Disabled;
			if (value == "gamePad")
				return Application::EmulatedControllerType::GamePad;
			if (value == "proController")
				return Application::EmulatedControllerType::ProController;
			if (value == "classicController")
				return Application::EmulatedControllerType::ClassicController;
			if (value == "wiimote")
				return Application::EmulatedControllerType::Wiimote;
			throw std::invalid_argument("unknown emulated controller type");
		}

		void RegisterRpc()
		{
			m_rpc.Register("system.bootstrap", [this](const rapidjson::Value&) {
				const auto accountSnapshot = m_controller.GetAccountManagerSnapshot();
				const auto activeAccount = std::ranges::find_if(
					accountSnapshot.accounts, [&accountSnapshot](const auto& account) {
						return account.persistentId == accountSnapshot.activePersistentId;
					});
				const auto activeAccountName =
					activeAccount == accountSnapshot.accounts.end()
						? std::string{}
						: boost::nowide::narrow(activeAccount->miiName);
				auto result = std::string(R"({"windowId":)") +
							  JsonString(std::to_string(m_invokingWindow)) + R"(,"windowRole":)" +
							  JsonString(RoleForWindow(m_invokingWindow)) + R"(,"appVersion":)" +
							  JsonString(BUILD_VERSION_STRING) + R"(,"platform":)" +
							  JsonString(PlatformName()) + R"(,"activeAccountName":)" +
							  JsonString(activeAccountName);
				if (m_invokingWindow != 0)
				{
					const auto found = m_toolWindows.find(m_invokingWindow);
					if (found != m_toolWindows.end() && (found->second->titleContext ||
														 !found->second->packageContext.empty() || found->second->generationContext))
					{
						result += R"(,"context":{)";
						bool separator{};
						if (found->second->titleContext)
						{
							result += R"("titleId":)" + JsonString(TitleIdString(*found->second->titleContext));
							separator = true;
						}
						if (!found->second->packageContext.empty())
						{
							if (separator)
								result += ",";
							result += R"("packageKey":)" + JsonString(found->second->packageContext);
							separator = true;
						}
						if (found->second->generationContext)
						{
							if (separator)
								result += ",";
							result += R"("generation":)" + JsonString(std::to_string(*found->second->generationContext));
						}
						result += "}";
					}
				}
				result += R"(,"theme":)" + JsonString(UiThemeName(m_theme)) +
						  R"(,"themeRevision":)" + JsonString(std::to_string(m_themeRevision)) +
						  R"(,"language":)" + JsonString(m_language) +
						  R"(,"languageRevision":)" + JsonString(std::to_string(m_languageRevision)) +
						  R"(,"shuttingDown":)";
				result += m_rpc.IsShuttingDown() ? "true}" : "false}";
				return result;
			});
			m_rpc.Register("theme.get", [this](const rapidjson::Value&) {
				return std::string(R"({"theme":)") + JsonString(UiThemeName(m_theme)) +
					   R"(,"revision":)" + JsonString(std::to_string(m_themeRevision)) + "}";
			});
			m_rpc.Register("theme.set", [this](const rapidjson::Value& params) {
				const auto requested = RequiredString(params, "theme");
				UiTheme next;
				if (requested == "light")
					next = UiTheme::Light;
				else if (requested == "dark")
					next = UiTheme::Dark;
				else
					throw std::invalid_argument("theme must be light or dark");
				if (m_theme != next)
				{
					m_theme = next;
					++m_themeRevision;
				}
				const auto result = std::string(R"({"theme":)") +
									JsonString(UiThemeName(m_theme)) + R"(,"revision":)" +
									JsonString(std::to_string(m_themeRevision)) + "}";
				Emit("theme.changed", result);
				return result;
			});
			m_rpc.Register("language.get", [this](const rapidjson::Value&) {
				return std::string(R"({"language":)") + JsonString(m_language) +
					   R"(,"revision":)" + JsonString(std::to_string(m_languageRevision)) + "}";
			});
			m_rpc.Register("language.set", [this](const rapidjson::Value& params) {
				const auto requested = RequiredString(params, "language");
				const auto normalized = NormalizeUiLanguage(requested);
				if (normalized != requested)
					throw std::invalid_argument("unsupported interface language");
				if (m_language != normalized)
				{
					auto& config = GetConfig();
					const auto previous = config.frontend.ui_language.GetValue();
					config.frontend.ui_language = normalized;
					if (!GetConfigHandle().Save())
					{
						config.frontend.ui_language = previous;
						throw std::runtime_error("unable to save interface language");
					}
					m_language = normalized;
					++m_languageRevision;
				}
				const auto result = std::string(R"({"language":)") + JsonString(m_language) +
									R"(,"revision":)" + JsonString(std::to_string(m_languageRevision)) + "}";
				Emit("language.changed", result);
				return result;
			});
			m_rpc.Register("system.quit", [this](const rapidjson::Value&) {
				if (m_invokingWindow != 0)
					throw std::runtime_error("only the main window may quit the application");
				if (!RequestShutdown(true))
					throw std::runtime_error("the running title could not be stopped; shutdown was cancelled");
				return std::string("{}");
			});
			m_rpc.Register("overlay.getSnapshot", [this](const rapidjson::Value&) {
				if (m_invokingWindow != 0 && m_invokingWindow != kMainOverlayWindowId &&
					m_invokingWindow != kPadOverlayWindowId)
					throw std::runtime_error("runtime overlay state is available only to a render window");
				return RuntimeOverlayJson(m_controller.GetRuntimeOverlaySnapshot(),
										  m_inputFocus.Waiting(m_invokingWindow == kPadOverlayWindowId
																   ? WebFrontend::InputWindow::GamePad
																   : WebFrontend::InputWindow::Game),
										  m_inputFocus.Revision());
			});
			m_rpc.Register("overlay.getShaderBackground", [this](const rapidjson::Value& params) {
				if (m_invokingWindow != 0 && m_invokingWindow != kMainOverlayWindowId &&
					m_invokingWindow != kPadOverlayWindowId)
					throw std::runtime_error("runtime overlay state is available only to a render window");
				const auto generation = ParseDecimalUint64(RequiredString(params, "generation"), "generation");
				const auto surface = RequiredString(params, "surface");
				const auto snapshot = m_controller.GetRuntimeOverlaySnapshot();
				if (!snapshot.shaderProgress.visible ||
					snapshot.shaderProgress.generation != generation)
					throw std::runtime_error("the shader progress request is no longer active");
				const std::shared_ptr<const std::string>* image{};
				if (surface == "tv")
					image = &snapshot.shaderProgress.backgroundImageTv;
				else if (surface == "pad")
					image = &snapshot.shaderProgress.backgroundImagePad;
				else
					throw std::invalid_argument("unknown runtime overlay surface");
				return std::string(R"({"generation":)") +
					   JsonString(std::to_string(generation)) + R"(,"dataUrl":)" +
					   JsonString(*image ? **image : std::string_view{}) + "}";
			});
			m_rpc.Register("overlay.submitKeyboardKey", [this](const rapidjson::Value& params) {
				if (m_invokingWindow != 0 && m_invokingWindow != kMainOverlayWindowId &&
					m_invokingWindow != kPadOverlayWindowId)
					throw std::runtime_error("runtime overlay input is available only to a render window");
				if (!m_controller.SubmitRuntimeOverlayKeyboardKey(
						ParseDecimalUint64(RequiredString(params, "generation"), "generation"), RequiredUint(params, "keyCode")))
					throw std::runtime_error("the software keyboard request is no longer active");
				return std::string("{}");
			});
			m_rpc.Register("overlay.selectErrorButton", [this](const rapidjson::Value& params) {
				if (m_invokingWindow != 0 && m_invokingWindow != kMainOverlayWindowId &&
					m_invokingWindow != kPadOverlayWindowId)
					throw std::runtime_error("runtime overlay input is available only to a render window");
				if (!m_controller.SelectRuntimeOverlayErrorButton(
						ParseDecimalUint64(RequiredString(params, "generation"), "generation"), RequiredBool(params, "rightButton")))
					throw std::runtime_error("the error dialog request is no longer active");
				return std::string("{}");
			});
			m_rpc.Register("window.close", [this](const rapidjson::Value&) {
				if (m_invokingWindow != 0)
				{
					const auto found = m_toolWindows.find(m_invokingWindow);
					if (found == m_toolWindows.end())
						throw std::runtime_error("the window is no longer active");
					found->second->closeRequested = true;
					return std::string("{}");
				}
				if (!RequestShutdown(true))
					throw std::runtime_error("the running title could not be stopped; window close was cancelled");
				return std::string("{}");
			});
			m_rpc.Register("workspace.activate", [this](const rapidjson::Value& params) {
				if (m_invokingWindow != 0)
					throw std::runtime_error("only the main window owns embedded workspaces");
				const auto role = RequiredString(params, "role");
				if (role != "main-library" &&
					std::ranges::find(WebFrontend::Generated::ImplementedWindowRoles, role) ==
						WebFrontend::Generated::ImplementedWindowRoles.end())
					throw std::invalid_argument("the requested workspace role is not implemented");
				if (role == "cemod-permissions")
					throw std::invalid_argument("permission approval remains an exact-context modal");
				m_mainWorkspaceRole.assign(role);
				RefreshInputConfigurationFocus();
				if (role == "logging")
					SignalLoggingChanged();
				return std::string("{}");
			});
			m_rpc.Register("system.openExternalUrl", [this](const rapidjson::Value& params) {
				const auto url = RequiredString(params, "url");
				constexpr std::array allowedOrigins{
					std::string_view("https://github.com/"),
					std::string_view("https://cemu.info/"),
				};
				if (std::ranges::none_of(allowedOrigins,
										 [url](std::string_view origin) { return url.starts_with(origin); }))
					throw std::invalid_argument("the external URL origin is not allowed");
				if (!m_nativeWindow->OpenExternalUrl(std::string(url)))
					throw std::runtime_error("the operating system could not open the URL");
				return std::string("{}");
			});
			m_rpc.Register("about.get", [this](const rapidjson::Value&) {
				RequireRole({"about"});
				return std::string(R"({"name":"CemuExtend","version":)") +
					   JsonString(BUILD_VERSION_STRING) + R"(,"commit":)" +
					   JsonString(CEMU_EXTEND_COMMIT_HASH) + R"(,"buildDate":)" +
					   JsonString(__DATE__ " " __TIME__) +
					   R"(,"frontend":"cef-react","browserEngine":)" +
					   JsonString("Chromium Embedded Framework") +
					   R"(,"originalAuthors":["Exzap","Petergov"],"libraries":[)"
					   R"({"name":"Chromium Embedded Framework","license":"BSD-3-Clause","url":"https://bitbucket.org/chromiumembedded/cef"},)"
					   R"({"name":"React","license":"MIT","url":"https://github.com/facebook/react"},)"
					   R"({"name":"Bun","license":"MIT","url":"https://github.com/oven-sh/bun"},)"
					   R"({"name":"Vulkan","license":"Apache-2.0","url":"https://github.com/KhronosGroup/Vulkan-Headers"}],"links":[)"
					   R"({"label":"CemuExtend source","url":"https://github.com/CemuExtend/CemuExtend"},)"
					   R"({"label":"Cemu project","url":"https://cemu.info/"}]})";
			});
			m_rpc.Register("settings.getFrontend", [this](const rapidjson::Value&) {
				RequireRole({"getting-started", "general-settings"});
				return FrontendSettingsJson(m_controller.GetFrontendSettings());
			});
			m_rpc.Register("settings.applyFrontend", [this](const rapidjson::Value& params) {
				RequireRole({"getting-started", "general-settings"});
				Application::FrontendSettingsUpdate update;
				update.expectedRevision = RequiredUint64(params, "revision");
				update.startFullscreen = RequiredBool(params, "startFullscreen");
				update.openPad = RequiredBool(params, "openPad");
				update.checkUpdates = RequiredBool(params, "checkUpdates");
				update.saveScreenshots = RequiredBool(params, "saveScreenshots");
				update.completeSetup = RequiredBool(params, "completeSetup");
				update.crashDump = std::string(RequiredString(params, "crashDump"));
				const auto& paths = RequiredMember(params, "gamePaths");
				if (!paths.IsArray())
					throw std::invalid_argument("gamePaths must be an array");
				for (const auto& path : paths.GetArray())
				{
					if (!path.IsString() || path.GetStringLength() == 0 ||
						path.GetStringLength() > 4096)
						throw std::invalid_argument("each game path must be a non-empty path string");
					update.gamePaths.emplace_back(_utf8ToPath(std::string_view(
						path.GetString(), path.GetStringLength())));
				}
				return FrontendSettingsResultJson(m_controller.ApplyFrontendSettings(update));
			});
			m_rpc.Register("accounts.getModel", [this](const rapidjson::Value&) {
				RequireRole({"account-manager", "general-settings"});
				return AccountManagerJson();
			});
			m_rpc.Register("accounts.create", [this](const rapidjson::Value& params) {
				RequireRole({"account-manager", "general-settings"});
				const auto result = m_controller.CreateAccount(RequiredUint(params, "persistentId"),
															   boost::nowide::widen(RequiredString(params, "miiName")));
				if (!result || !result.account)
					throw std::runtime_error(result.diagnostic.empty() ? "account creation failed" : result.diagnostic);
				return AccountJson(*result.account);
			});
			m_rpc.Register("accounts.update", [this](const rapidjson::Value& params) {
				RequireRole({"account-manager", "general-settings"});
				Application::AccountUpdate update;
				update.miiName = boost::nowide::widen(RequiredString(params, "miiName"));
				update.birthYear = static_cast<std::uint16_t>(
					RequiredBoundedUint(params, "birthYear", 0, 2100));
				update.birthMonth = static_cast<std::uint8_t>(
					RequiredBoundedUint(params, "birthMonth", 0, 12));
				update.birthDay = static_cast<std::uint8_t>(
					RequiredBoundedUint(params, "birthDay", 0, 31));
				update.gender = static_cast<std::uint8_t>(
					RequiredBoundedUint(params, "gender", 0, 2));
				update.email = RequiredString(params, "email");
				update.country = RequiredUint(params, "country");
				const auto countries = m_controller.ListAccountCountries();
				if (std::ranges::none_of(countries,
										 [&update](const Application::AccountCountry& country) {
											 return country.code == update.country;
										 }))
					throw std::invalid_argument("country is not supported");
				const auto result = m_controller.UpdateAccount(
					RequiredUint(params, "persistentId"), update);
				if (!result || !result.account)
					throw std::runtime_error(result.diagnostic.empty() ? "account update failed" : result.diagnostic);
				return AccountJson(*result.account);
			});
			m_rpc.Register("accounts.delete", [this](const rapidjson::Value& params) {
				RequireRole({"account-manager", "general-settings"});
				const auto result = m_controller.DeleteAccount(RequiredUint(params, "persistentId"));
				if (!result)
					throw std::runtime_error(result.diagnostic.empty() ? "account deletion failed" : result.diagnostic);
				return std::string("{}");
			});
			m_rpc.Register("accounts.setActive", [this](const rapidjson::Value& params) {
				RequireRole({"account-manager", "general-settings"});
				const auto result = m_controller.SetActiveAccount(
					RequiredUint(params, "persistentId"));
				if (!result)
					throw std::runtime_error(result.diagnostic.empty() ? "active account update failed" : result.diagnostic);
				return std::string("{}");
			});
			m_rpc.Register("accounts.setNetworkService", [this](const rapidjson::Value& params) {
				RequireRole({"account-manager", "general-settings"});
				const auto result = m_controller.SetAccountNetworkService(
					RequiredUint(params, "persistentId"),
					ParseAccountNetworkService(RequiredString(params, "service")));
				if (!result)
					throw std::runtime_error(result.diagnostic.empty() ? "network service update failed" : result.diagnostic);
				return std::string("{}");
			});
			m_rpc.Register("input.getModel", [this](const rapidjson::Value&) {
				RequireRole({"input-settings"});
				return InputSettingsJson();
			});
			m_rpc.Register("input.enumerate", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"});
				const auto result = m_controller.EnumerateInputDevices(RequiredString(params, "api"));
				if (!result)
					throw std::runtime_error(result.diagnostic.empty() ? "input device enumeration failed" : result.diagnostic);
				rapidjson::StringBuffer buffer;
				JsonWriter writer(buffer);
				writer.StartArray();
				for (const auto& device : result.devices)
				{
					writer.StartObject();
					writer.Key("token");
					writer.Uint64(device.token);
					writer.Key("api");
					writer.String(device.api.data(), device.api.size());
					writer.Key("displayName");
					writer.String(device.displayName.data(), device.displayName.size());
					writer.Key("connected");
					writer.Bool(device.connected);
					writer.EndObject();
				}
				writer.EndArray();
				return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("input.setType", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"});
				return InputMutationJson(m_controller.SetEmulatedController(RequiredUint(params, "player"), ParseInputControllerType(RequiredString(params, "type")), RequiredBool(params, "preserveDevices")));
			});
			m_rpc.Register("input.addDevice", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"});
				return InputMutationJson(m_controller.AddInputDevice(RequiredUint(params, "player"), RequiredUint64(params, "token")));
			});
			m_rpc.Register("input.removeDevice", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"});
				return InputMutationJson(m_controller.RemoveInputDevice(RequiredUint(params, "player"), RequiredUint64(params, "token")));
			});
			m_rpc.Register("input.connectDevice", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"});
				return InputMutationJson(m_controller.ConnectInputDevice(RequiredUint64(params, "token")));
			});
			m_rpc.Register("input.captureButton", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings", "hotkey-settings"});
				const auto captured = m_controller.CaptureInputButton(RequiredUint64(params, "token"));
				if (!captured)
					return std::string("null");
				return std::string(R"({"id":)") + std::to_string(captured->id) +
					   R"(,"label":)" + JsonString(captured->label) + "}";
			});
			m_rpc.Register("input.setMapping", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"});
				return InputMutationJson(m_controller.SetInputMapping(RequiredUint(params, "player"), RequiredUint64(params, "mappingId"), RequiredUint64(params, "controllerToken"), RequiredUint64(params, "buttonId")));
			});
			m_rpc.Register("input.clearMapping", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"});
				std::optional<std::uint64_t> mapping;
				if (params.IsObject())
					if (const auto found = params.FindMember("mappingId"); found != params.MemberEnd())
					{
						if (!found->value.IsUint64())
							throw std::invalid_argument("mappingId must be an unsigned integer");
						mapping = found->value.GetUint64();
					}
				return InputMutationJson(m_controller.ClearInputMapping(RequiredUint(params, "player"), mapping));
			});
			m_rpc.Register("input.setDeviceSettings", [this](const rapidjson::Value& params) {
				RequireRole({"input-settings"});
				const auto& value = RequiredMember(params, "settings");
				auto axis = [](const rapidjson::Value& object) {
					return Application::ControllerAxisSettings{static_cast<float>(RequiredDouble(object, "deadzone")), static_cast<float>(RequiredDouble(object, "range"))};
				};
				Application::PhysicalControllerSettings settings{axis(RequiredMember(value, "axis")), axis(RequiredMember(value, "rotation")), axis(RequiredMember(value, "trigger")), static_cast<float>(RequiredDouble(value, "rumble")), RequiredBool(value, "motion")};
				if (const auto found = value.FindMember("packetDelay"); found != value.MemberEnd())
				{
					if (!found->value.IsUint())
						throw std::invalid_argument("packetDelay must be an unsigned integer");
					settings.packetDelay = found->value.GetUint();
				}
				return InputMutationJson(m_controller.SetPhysicalControllerSettings(RequiredUint64(params, "token"), settings));
			});
			m_rpc.Register("input.calibrate", [this](const rapidjson::Value& params) { RequireRole({"input-settings"}); return InputMutationJson(m_controller.CalibrateInputDevice(RequiredUint64(params, "token"))); });
			m_rpc.Register("input.profileLoad", [this](const rapidjson::Value& params) { RequireRole({"input-settings"}); return InputMutationJson(m_controller.LoadInputProfile(RequiredUint(params, "player"), RequiredString(params, "profile"))); });
			m_rpc.Register("input.profileSave", [this](const rapidjson::Value& params) { RequireRole({"input-settings"}); return InputMutationJson(m_controller.SaveInputProfile(RequiredUint(params, "player"), RequiredString(params, "profile"))); });
			m_rpc.Register("input.profileDelete", [this](const rapidjson::Value& params) { RequireRole({"input-settings"}); return InputMutationJson(m_controller.DeleteInputProfile(RequiredString(params, "profile"))); });
			m_rpc.Register("hotkeys.get", [this](const rapidjson::Value&) {
				RequireRole({"hotkey-settings"});
				return HotkeySettingsJson(m_controller.GetHotkeySettings());
			});
			m_rpc.Register("usb.getModel", [this](const rapidjson::Value&) {
				RequireRole({"emulated-usb-devices"});
				return UsbModelJson(m_emulatedUsb.GetModel());
			});
			m_rpc.Register("usb.setEnabled", [this](const rapidjson::Value& params) {
				RequireRole({"emulated-usb-devices"});
				return UsbModelJson(m_emulatedUsb.SetEnabled(RequiredString(params, "deviceId"),
															 static_cast<std::uint16_t>(RequiredBoundedUint(params, "vendorId", 0, 0xffff)),
															 static_cast<std::uint16_t>(RequiredBoundedUint(params, "productId", 0, 0xffff)),
															 RequiredBool(params, "enabled")));
			});
			m_rpc.Register("hotkeys.apply", [this](const rapidjson::Value& params) {
				RequireRole({"hotkey-settings"});
				Application::HotkeySettingsUpdate update;
				update.revision = RequiredUint64(params, "revision");
				const auto& modifier = RequiredMember(params, "controllerModifier");
				if (!modifier.IsNull())
				{
					if (!modifier.IsUint())
						throw std::invalid_argument("controllerModifier must be null or an unsigned integer");
					update.controllerModifier = modifier.GetUint();
				}
				const auto& bindings = RequiredMember(params, "bindings");
				if (!bindings.IsArray())
					throw std::invalid_argument("bindings must be an array");
				for (const auto& value : bindings.GetArray())
				{
					Application::HotkeyBinding binding;
					binding.action = ParseHotkeyAction(RequiredString(value, "action"));
					binding.keyboardUsage = static_cast<std::uint16_t>(
						RequiredBoundedUint(value, "keyboardUsage", 0, 0xffff));
					binding.keyboardModifiers = static_cast<std::uint8_t>(
						RequiredBoundedUint(value, "keyboardModifiers", 0, 0x0f));
					const auto& button = RequiredMember(value, "controllerButton");
					if (!button.IsNull())
					{
						if (!button.IsUint())
							throw std::invalid_argument("controllerButton must be null or an unsigned integer");
						binding.controllerButton = button.GetUint();
					}
					update.bindings.push_back(binding);
				}
				const auto result = m_controller.ApplyHotkeySettings(update);
				if (result)
					RefreshHotkeyBindings(result.snapshot);
				return HotkeySettingsResultJson(result);
			});
			m_rpc.Register("graphicPacks.list", [this](const rapidjson::Value&) {
				RequireRole({"graphic-packs"});
				return GraphicPacksJson();
			});
			m_rpc.Register("graphicPacks.setEnabled", [this](const rapidjson::Value& params) {
				RequireRole({"graphic-packs"});
				const auto result = m_controller.SetGraphicPackEnabled(
					RequiredString(params, "key"), RequiredBool(params, "enabled"));
				if (result)
					m_controller.SaveGraphicPackState();
				return GraphicPackMutationJson(result);
			});
			m_rpc.Register("graphicPacks.setPreset", [this](const rapidjson::Value& params) {
				RequireRole({"graphic-packs"});
				const auto result = m_controller.SetGraphicPackPreset(
					RequiredString(params, "key"), RequiredString(params, "category"),
					RequiredString(params, "preset"));
				if (result)
					m_controller.SaveGraphicPackState();
				return GraphicPackMutationJson(result);
			});
			m_rpc.Register("graphicPacks.reload", [this](const rapidjson::Value& params) {
				RequireRole({"graphic-packs"});
				return GraphicPackMutationJson(
					m_controller.ReloadGraphicPack(RequiredString(params, "key")));
			});
			m_rpc.Register("graphicPacks.refresh", [this](const rapidjson::Value&) {
				RequireRole({"graphic-packs"});
				const auto result = m_controller.RefreshGraphicPacks();
				if (!result)
					throw std::runtime_error(result.diagnostic.empty() ? "graphic pack refresh failed" : result.diagnostic);
				rapidjson::StringBuffer buffer;
				JsonWriter writer(buffer);
				writer.StartObject();
				writer.Key("removedEnabledPaths");
				writer.StartArray();
				for (const auto& path : result.removedEnabledPaths)
					writer.String(path.data(), static_cast<rapidjson::SizeType>(path.size()));
				writer.EndArray();
				writer.Key("diagnostic");
				writer.String(result.diagnostic.data(),
							  static_cast<rapidjson::SizeType>(result.diagnostic.size()));
				writer.EndObject();
				return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("graphicPacks.save", [this](const rapidjson::Value&) {
				RequireRole({"graphic-packs"});
				m_controller.SaveGraphicPackState();
				return std::string("{}");
			});
			m_rpc.Register("graphicPacks.install", [this](const rapidjson::Value& params) {
				RequireRole({"graphic-packs", "update-manager"});
				Application::GraphicPackInstallRequest request;
				const auto kind = RequiredString(params, "kind");
				if (kind == "community")
					request.kind = Application::GraphicPackInstallKind::Community;
				else if (kind == "customUrl")
				{
					request.kind = Application::GraphicPackInstallKind::CustomUrl;
					request.url = RequiredString(params, "url");
				}
				else
					throw std::invalid_argument("unknown graphic-pack install kind");
				request.replaceExisting = RequiredBool(params, "replaceExisting");
				const auto jobId = StartGraphicPackInstallJob(std::move(request));
				return std::string(R"({"jobId":)") + JsonString(std::to_string(jobId)) + "}";
			});
			m_rpc.Register("save.getModel", [this](const rapidjson::Value&) {
				RequireRole({"save-manager"});
				const auto content = m_controller.ListManagedContent();
				const auto accounts = m_controller.ListAccounts();
				rapidjson::StringBuffer buffer;
				JsonWriter writer(buffer);
				writer.StartObject();
				writer.Key("scanning");
				writer.Bool(m_controller.IsTitleScanning());
				writer.Key("accounts");
				writer.StartArray();
				for (const auto& account : accounts)
				{
					writer.StartObject();
					writer.Key("persistentId");
					writer.String(PersistentIdString(account.persistentId).c_str());
					const auto name = boost::nowide::narrow(account.miiName);
					writer.Key("name");
					writer.String(name.data(), name.size());
					writer.EndObject();
				}
				writer.EndArray();
				writer.Key("titles");
				writer.StartArray();
				for (const auto& entry : content)
				{
					if (entry.type != Application::ManagedContentType::Save)
						continue;
					writer.StartObject();
					writer.Key("titleId");
					writer.String(TitleIdString(entry.titleId).c_str());
					writer.Key("name");
					writer.String(entry.name.data(), entry.name.size());
					writer.Key("saves");
					writer.StartArray();
					for (const auto persistentId : m_controller.ListSavePersistentIds(entry.titleId))
					{
						const auto location = m_controller.InspectSaveEntry(entry.titleId, persistentId);
						writer.StartObject();
						writer.Key("persistentId");
						writer.String(PersistentIdString(persistentId).c_str());
						writer.Key("state");
						writer.String(SaveStateName(location.state).data());
						const auto account = std::ranges::find(accounts, persistentId, &Application::AccountInfo::persistentId);
						writer.Key("accountName");
						if (account == accounts.end())
							writer.String("");
						else
						{
							const auto name = boost::nowide::narrow(account->miiName);
							writer.String(name.data(), name.size());
						}
						writer.EndObject();
					}
					writer.EndArray();
					writer.EndObject();
				}
				writer.EndArray();
				writer.EndObject();
				return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("save.inspect", [this](const rapidjson::Value& params) {
				RequireRole({"save-manager"});
				const auto titleId = ParseTitleId(params);
				const auto persistentId = ParsePersistentId(params);
				const auto location = m_controller.InspectSaveEntry(titleId, persistentId);
				return std::string(R"({"state":)") + JsonString(SaveStateName(location.state)) + "}";
			});
			m_rpc.Register("save.delete.prepare", [this](const rapidjson::Value& params) {
				RequireRole({"save-manager"});
				const auto titleId = ParseTitleId(params);
				const auto persistentId = ParsePersistentId(params);
				if (m_controller.InspectSaveEntry(titleId, persistentId).state != Application::SaveEntryState::Directory)
					throw std::invalid_argument("the selected save no longer exists");
				const auto token = IssueSaveTicket({m_invokingWindow, SaveTicketKind::Delete, {}, titleId, persistentId});
				return std::string(R"({"confirmationToken":)") + JsonString(token) + "}";
			});
			m_rpc.Register("save.delete", [this](const rapidjson::Value& params) {
				RequireRole({"save-manager"});
				auto ticket = TakeSaveTicket(RequiredString(params, "confirmationToken"), SaveTicketKind::Delete);
				const auto result = m_controller.DeleteSave(ticket.titleId, ticket.sourcePersistentId);
				if (!result)
					throw std::runtime_error(result.diagnostic.empty() ? std::string(SaveErrorName(result.error)) : result.diagnostic);
				return std::string("{}");
			});
			m_rpc.Register("save.transfer.inspect", [this](const rapidjson::Value& params) {
				RequireRole({"save-manager"});
				const auto titleId = ParseTitleId(params);
				const auto source = ParsePersistentId(params, "sourcePersistentId");
				const auto target = ParsePersistentId(params, "targetPersistentId");
				if (source == target)
					throw std::invalid_argument("source and target persistent ids must differ");
				if (m_controller.InspectSaveEntry(titleId, source).state != Application::SaveEntryState::Directory)
					throw std::invalid_argument("the source save no longer exists");
				const auto targetLocation = m_controller.InspectSaveEntry(titleId, target);
				if (targetLocation.state == Application::SaveEntryState::NonDirectory)
					throw std::invalid_argument("a non-directory entry blocks the target save");
				const bool overwrite = targetLocation.state == Application::SaveEntryState::Directory;
				const auto token = IssueSaveTicket({m_invokingWindow, SaveTicketKind::Transfer, {}, titleId, source, target, overwrite});
				return std::string(R"({"targetState":)") + JsonString(SaveStateName(targetLocation.state)) +
					   R"(,"confirmationToken":)" + JsonString(token) + "}";
			});
			m_rpc.Register("save.transfer", [this](const rapidjson::Value& params) {
				RequireRole({"save-manager"});
				auto ticket = TakeSaveTicket(RequiredString(params, "confirmationToken"), SaveTicketKind::Transfer);
				const auto result = m_controller.TransferSave(ticket.titleId, ticket.sourcePersistentId, ticket.targetPersistentId, ticket.overwrite);
				if (!result)
					throw std::runtime_error(result.diagnostic.empty() ? std::string(SaveErrorName(result.error)) : result.diagnostic);
				return std::string("{}");
			});
			m_rpc.Register("save.import.pick", [this](const rapidjson::Value& params) {
				RequireRole({"save-manager"});
				const auto titleId = ParseTitleId(params);
				const auto selected = WebFrontend::SelectArchiveToOpen(
					InvokingNativeWindow(), "Select a zipped save file");
				if (!selected)
					return std::string(R"({"selected":false})");
				const auto token = IssueSaveTicket({m_invokingWindow, SaveTicketKind::Import, *selected, titleId});
				return std::string(R"({"selected":true,"fileToken":)") + JsonString(token) + R"(,"name":)" + JsonString(_pathToUtf8(selected->filename())) + "}";
			});
			m_rpc.Register("save.import.inspect", [this](const rapidjson::Value& params) {
				RequireRole({"save-manager"});
				const auto token = std::string(RequiredString(params, "fileToken"));
				const auto found = m_saveTickets.find(token);
				if (found == m_saveTickets.end() || found->second.ownerWindow != m_invokingWindow || found->second.kind != SaveTicketKind::Import)
					throw std::invalid_argument("import file token is invalid or expired");
				const auto titleId = ParseTitleId(params);
				const auto persistentId = ParsePersistentId(params);
				if (titleId != found->second.titleId)
					throw std::invalid_argument("titleId does not match the native file selection");
				const auto inspection = m_controller.InspectSaveImport(found->second.archivePath, titleId, persistentId);
				if (!inspection)
					throw std::runtime_error(inspection.diagnostic.empty() ? std::string(SaveErrorName(inspection.error)) : inspection.diagnostic);
				if (inspection.target.state == Application::SaveEntryState::NonDirectory)
					throw std::invalid_argument("a non-directory entry blocks the target save");
				found->second.targetPersistentId = persistentId;
				found->second.overwrite = inspection.target.state == Application::SaveEntryState::Directory;
				const bool mismatch = inspection.sourceTitleId && *inspection.sourceTitleId != 0 && *inspection.sourceTitleId != titleId;
				return std::string(R"({"confirmationToken":)") + JsonString(token) + R"(,"targetState":)" + JsonString(SaveStateName(inspection.target.state)) +
					   R"(,"sourceTitleId":)" + (inspection.sourceTitleId ? JsonString(TitleIdString(*inspection.sourceTitleId)) : "null") +
					   R"(,"titleMismatch":)" + (mismatch ? "true" : "false") + "}";
			});
			m_rpc.Register("save.import.start", [this](const rapidjson::Value& params) {
				RequireRole({"save-manager"});
				auto ticket = TakeSaveTicket(RequiredString(params, "confirmationToken"), SaveTicketKind::Import);
				if (!ticket.targetPersistentId)
					throw std::invalid_argument("import archive has not been inspected");
				return std::string(R"({"jobId":)") + JsonString(std::to_string(StartSaveArchiveJob(std::move(ticket)))) + "}";
			});
			m_rpc.Register("save.export.pick", [this](const rapidjson::Value& params) {
				RequireRole({"save-manager"});
				const auto titleId = ParseTitleId(params);
				const auto persistentId = ParsePersistentId(params);
				if (m_controller.InspectSaveEntry(titleId, persistentId).state != Application::SaveEntryState::Directory)
					throw std::invalid_argument("the selected save no longer exists");
				const auto name = TitleIdString(titleId) + "-" + PersistentIdString(persistentId) + ".zip";
				const auto selected = WebFrontend::SelectArchiveToSave(
					InvokingNativeWindow(), "Export save archive", name);
				if (!selected)
					return std::string(R"({"selected":false})");
				const auto token = IssueSaveTicket({m_invokingWindow, SaveTicketKind::Export, *selected, titleId, persistentId, 0, true});
				return std::string(R"({"selected":true,"confirmationToken":)") + JsonString(token) + R"(,"name":)" + JsonString(_pathToUtf8(selected->filename())) + "}";
			});
			m_rpc.Register("save.export.start", [this](const rapidjson::Value& params) {
				RequireRole({"save-manager"});
				auto ticket = TakeSaveTicket(RequiredString(params, "confirmationToken"), SaveTicketKind::Export);
				return std::string(R"({"jobId":)") + JsonString(std::to_string(StartSaveArchiveJob(std::move(ticket)))) + "}";
			});
			m_rpc.Register("updates.getModel", [this](const rapidjson::Value&) {
				RequireRole({"update-manager"});
				return std::string(R"({"titleRunning":)") +
					   (m_controller.IsTitleRunning() ? "true" : "false") + "}";
			});
			m_rpc.Register("updates.pickTitleSource", [this](const rapidjson::Value&) {
				RequireRole({"update-manager"});
				if (m_controller.IsTitleRunning())
					throw std::runtime_error("titles cannot be installed while a game is running");
				std::optional<fs::path> selected;
				if (m_invokingWindow == 0)
				{
					const auto metadata = m_nativeWindow->PickTitleInstallSource();
					if (metadata)
					{
						std::error_code pathError;
						auto canonical = fs::canonical(_utf8ToPath(*metadata), pathError);
						if (pathError || canonical.filename() != "meta.xml" ||
							canonical.parent_path().filename() != "meta")
							throw std::invalid_argument("the selected file must be meta/meta.xml");
						selected = canonical.parent_path().parent_path();
					}
				}
				else
				{
					const auto owner = m_toolWindows.find(m_invokingWindow);
					if (owner == m_toolWindows.end() || !owner->second->nativeSupport)
						throw std::runtime_error("the update workspace is no longer active");
					selected = owner->second->nativeSupport->PickDirectory(
						"Select the title folder containing code, content, and meta");
				}
				if (!selected)
					return std::string("null");
				const auto planned = m_controller.PlanTitleInstall(*selected);
				if (!planned)
					throw std::runtime_error(planned.diagnostic.empty() ? "the selected folder is not an installable title" : planned.diagnostic);
				const auto& plan = *planned.plan;
				const auto token = m_updatePlans.Issue(
					m_invokingWindow, InvokingWindowGeneration(), plan);
				auto kindName = [](Application::TitleInstallKind kind) {
					switch (kind)
					{
					case Application::TitleInstallKind::Base:
						return "base";
					case Application::TitleInstallKind::Demo:
						return "demo";
					case Application::TitleInstallKind::Update:
						return "update";
					case Application::TitleInstallKind::Dlc:
						return "dlc";
					case Application::TitleInstallKind::SystemTitle:
						return "systemTitle";
					case Application::TitleInstallKind::SystemData:
						return "systemData";
					default:
						return "unknown";
					}
				};
				auto conflictName = [](Application::TitleInstallConflict conflict) {
					switch (conflict)
					{
					case Application::TitleInstallConflict::DifferentType:
						return "differentType";
					case Application::TitleInstallConflict::SameVersion:
						return "sameVersion";
					case Application::TitleInstallConflict::NewerVersionInstalled:
						return "newerVersionInstalled";
					default:
						return "none";
					}
				};
				rapidjson::StringBuffer buffer;
				JsonWriter writer(buffer);
				writer.StartObject();
				writer.Key("planToken");
				writer.String(std::to_string(token).c_str());
				writer.Key("titleId");
				writer.String(TitleIdString(plan.titleId).c_str());
				writer.Key("titleName");
				writer.String(plan.titleName.data(), plan.titleName.size());
				writer.Key("version");
				writer.Uint(plan.version);
				writer.Key("kind");
				writer.String(kindName(plan.kind));
				writer.Key("conflict");
				writer.String(conflictName(plan.conflict));
				writer.Key("installedVersion");
				if (plan.installed.valid)
					writer.Uint(plan.installed.version);
				else
					writer.Null();
				writer.Key("requiredBytes");
				writer.Uint64(plan.requiredBytes);
				writer.Key("availableBytes");
				writer.Uint64(plan.availableBytes);
				writer.EndObject();
				return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("updates.installTitle", [this](const rapidjson::Value& params) {
				RequireRole({"update-manager"});
				if (m_controller.IsTitleRunning())
					throw std::runtime_error("titles cannot be installed while a game is running");
				auto plan = m_updatePlans.Take(
					ParseDecimalUint64(RequiredString(params, "planToken"), "planToken"),
					m_invokingWindow, InvokingWindowGeneration());
				if (!plan)
					throw std::invalid_argument("the title-install plan is missing, stale, or owned by another window");
				const bool acceptConflict = RequiredBool(params, "acceptConflict");
				if (plan->conflict != Application::TitleInstallConflict::None && !acceptConflict)
					throw std::invalid_argument("the existing-title conflict must be accepted explicitly");
				const auto decision = acceptConflict ? Application::TitleInstallDecision::AcceptConflict : Application::TitleInstallDecision::Proceed;
				return std::string(R"({"jobId":)") +
					   JsonString(std::to_string(StartTitleInstallJob(std::move(*plan), decision))) + "}";
			});
			m_rpc.Register("diagnostics.getTextureRelations", [this](const rapidjson::Value& params) {
				RequireRole({"texture-relations"});
				return TextureDiagnosticJson(m_diagnostics.GetTexturePage(
					ParseDecimalUint64(RequiredString(params, "generation"), "generation"), RequiredUint(params, "offset"),
					RequiredBoundedUint(params, "limit", 1, Application::DiagnosticFacade::MaximumPageSize),
					RequiredBool(params, "activeOnly"), RequiredBool(params, "includeViews")));
			});
			m_rpc.Register("diagnostics.getAudioVoices", [this](const rapidjson::Value& params) {
				RequireRole({"audio-debugger"});
				return AudioDiagnosticJson(m_diagnostics.GetAudioVoicePage(
					ParseDecimalUint64(RequiredString(params, "generation"), "generation"), RequiredUint(params, "offset"),
					RequiredBoundedUint(params, "limit", 1, Application::DiagnosticFacade::MaximumPageSize),
					RequiredBool(params, "activeOnly")));
			});
			m_rpc.Register("titleManager.getModel", [this](const rapidjson::Value&) {
				RequireRole({"title-manager"});
				auto typeName = [](Application::ManagedContentType type) {
					switch (type)
					{
					case Application::ManagedContentType::Update:
						return "update";
					case Application::ManagedContentType::Dlc:
						return "dlc";
					case Application::ManagedContentType::System:
						return "system";
					case Application::ManagedContentType::Save:
						return "save";
					default:
						return "base";
					}
				};
				auto formatName = [](Application::ManagedContentFormat format) {
					switch (format)
					{
					case Application::ManagedContentFormat::Wud:
						return "wud";
					case Application::ManagedContentFormat::Nus:
						return "nus";
					case Application::ManagedContentFormat::Wua:
						return "wua";
					case Application::ManagedContentFormat::Wuhb:
						return "wuhb";
					default:
						return "folder";
					}
				};
				rapidjson::StringBuffer buffer;
				JsonWriter writer(buffer);
				writer.StartObject();
				writer.Key("scanning");
				writer.Bool(m_controller.IsTitleScanning());
				writer.Key("entries");
				writer.StartArray();
				for (const auto& entry : m_controller.ListManagedContent())
				{
					writer.StartObject();
					const auto uid = std::to_string(entry.locationUid);
					writer.Key("locationUid");
					writer.String(uid.c_str());
					writer.Key("titleId");
					writer.String(TitleIdString(entry.titleId).c_str());
					writer.Key("name");
					writer.String(entry.name.data(), entry.name.size());
					const auto path = _pathToUtf8(entry.path);
					writer.Key("path");
					writer.String(path.data(), path.size());
					writer.Key("version");
					writer.Uint(entry.version);
					writer.Key("region");
					writer.String(entry.regionName.data(), entry.regionName.size());
					writer.Key("type");
					writer.String(typeName(entry.type));
					writer.Key("format");
					writer.String(formatName(entry.format));
					writer.Key("canLaunch");
					writer.Bool(entry.type == Application::ManagedContentType::Base);
					writer.Key("canVerify");
					writer.Bool(entry.type != Application::ManagedContentType::Save);
					writer.Key("canConvert");
					writer.Bool(entry.type != Application::ManagedContentType::Save && entry.format != Application::ManagedContentFormat::Wua);
					writer.Key("canDelete");
					writer.Bool(Application::IsManagedContentDeletionSupported(entry.type) && !m_controller.IsTitleScanning() && !m_controller.IsTitleRunning());
					writer.EndObject();
				}
				writer.EndArray();
				writer.EndObject();
				return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("titleManager.refresh", [this](const rapidjson::Value&) {
				RequireRole({"title-manager"});
				m_controller.RefreshTitles();
				return std::string("{}");
			});
			m_rpc.Register("titleManager.launch", [this](const rapidjson::Value& params) {
				RequireRole({"title-manager"});
				const auto uid = ParseOpaqueUid(params);
				const auto entries = m_controller.ListManagedContent();
				const auto entry = std::ranges::find(entries, uid, &Application::ManagedContentEntry::locationUid);
				if (entry == entries.end() || entry->type != Application::ManagedContentType::Base)
					throw std::invalid_argument("the selected base installation is no longer available");
				return Launch(entry->path, entry->titleId);
			});
			m_rpc.Register("logging.getSnapshot", [this](const rapidjson::Value&) {
				RequireRole({"logging"});
				return LoggingSnapshotJson(m_logging.Snapshot());
			});
			m_rpc.Register("logging.clear", [this](const rapidjson::Value&) {
				RequireRole({"logging"});
				const auto clearedThroughSequence = m_logging.Clear();
				m_lastForwardedLogSequence = std::max(m_lastForwardedLogSequence,
													  clearedThroughSequence);
				EmitToWindow(m_invokingWindow, "logging.cleared",
							 std::string(R"({"clearedThroughSequence":)") +
								 JsonString(std::to_string(clearedThroughSequence)) + "}");
				return std::string(R"({"clearedThroughSequence":)") +
					   JsonString(std::to_string(clearedThroughSequence)) + "}";
			});
			m_rpc.Register("titleManager.pickInstallSource", [this](const rapidjson::Value&) {
				RequireRole({"title-manager"});
				const auto selected = m_nativeWindow->PickTitleInstallSource();
				if (!selected)
					return std::string(R"({"cancelled":true})");
				std::error_code pathError;
				auto metadata = fs::canonical(_utf8ToPath(*selected), pathError);
				if (pathError || metadata.filename() != "meta.xml" ||
					metadata.parent_path().filename() != "meta" || !fs::is_regular_file(metadata, pathError) || pathError)
					throw std::invalid_argument("the selected file must be meta/meta.xml");
				auto source = metadata.parent_path().parent_path();
				const auto token = ++m_nextOperationToken;
				m_installSources.emplace(token, NativePathRecord{m_invokingWindow, std::move(source)});
				return std::string(R"({"cancelled":false,"sourceToken":)") + JsonString(std::to_string(token)) +
					   R"(,"displayName":)" + JsonString(_pathToUtf8(metadata.parent_path().parent_path().filename())) + "}";
			});
			m_rpc.Register("titleManager.planInstall", [this](const rapidjson::Value& params) {
				RequireRole({"title-manager"});
				const auto sourceToken = ParseDecimalUint64(
					RequiredString(params, "sourceToken"), "sourceToken");
				const auto source = m_installSources.find(sourceToken);
				if (source == m_installSources.end() || source->second.owner != m_invokingWindow)
					throw std::invalid_argument("install source token is invalid or expired");
				const auto path = source->second.path;
				m_installSources.erase(source);
				const auto result = m_controller.PlanTitleInstall(path);
				if (!result)
					throw std::runtime_error(result.diagnostic.empty() ? "unable to plan title installation" : result.diagnostic);
				const auto token = ++m_nextOperationToken;
				m_installPlans.emplace(token, InstallPlanRecord{m_invokingWindow, *result.plan});
				auto kindName = [](Application::TitleInstallKind kind) {
					switch (kind)
					{
					case Application::TitleInstallKind::Base:
						return "base";
					case Application::TitleInstallKind::Demo:
						return "demo";
					case Application::TitleInstallKind::Update:
						return "update";
					case Application::TitleInstallKind::Dlc:
						return "dlc";
					case Application::TitleInstallKind::SystemTitle:
						return "systemTitle";
					case Application::TitleInstallKind::SystemData:
						return "systemData";
					default:
						return "unknown";
					}
				};
				auto conflictName = [](Application::TitleInstallConflict conflict) {
					switch (conflict)
					{
					case Application::TitleInstallConflict::DifferentType:
						return "differentType";
					case Application::TitleInstallConflict::SameVersion:
						return "sameVersion";
					case Application::TitleInstallConflict::NewerVersionInstalled:
						return "newerVersionInstalled";
					default:
						return "none";
					}
				};
				const auto& plan = *result.plan;
				rapidjson::StringBuffer buffer;
				JsonWriter writer(buffer);
				writer.StartObject();
				writer.Key("planToken");
				writer.String(std::to_string(token).c_str());
				writer.Key("titleId");
				writer.String(TitleIdString(plan.titleId).c_str());
				writer.Key("titleName");
				writer.String(plan.titleName.data(), plan.titleName.size());
				writer.Key("version");
				writer.Uint(plan.version);
				writer.Key("kind");
				writer.String(kindName(plan.kind));
				writer.Key("conflict");
				writer.String(conflictName(plan.conflict));
				writer.Key("requiredBytes");
				writer.Uint64(plan.requiredBytes);
				writer.Key("availableBytes");
				writer.Uint64(plan.availableBytes);
				writer.EndObject();
				return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("titleManager.startInstall", [this](const rapidjson::Value& params) {
				RequireRole({"title-manager"});
				const auto token = ParseDecimalUint64(
					RequiredString(params, "planToken"), "planToken");
				const auto found = m_installPlans.find(token);
				if (found == m_installPlans.end() || found->second.owner != m_invokingWindow)
					throw std::invalid_argument("install plan token is invalid or expired");
				auto plan = std::move(found->second.plan);
				m_installPlans.erase(found);
				const auto decisionText = RequiredString(params, "decision");
				const auto decision = decisionText == "acceptConflict" ? Application::TitleInstallDecision::AcceptConflict : Application::TitleInstallDecision::Proceed;
				if (decisionText != "proceed" && decisionText != "acceptConflict")
					throw std::invalid_argument("unknown install decision");
				return std::string(R"({"jobId":)") + JsonString(std::to_string(StartTitleInstallJob(std::move(plan), decision))) + "}";
			});
			m_rpc.Register("titleManager.planWua", [this](const rapidjson::Value& params) {
				RequireRole({"title-manager"});
				const auto uid = ParseOpaqueUid(params);
				const auto entries = m_controller.ListManagedContent();
				const auto entry = std::ranges::find(entries, uid, &Application::ManagedContentEntry::locationUid);
				if (entry == entries.end() || entry->type == Application::ManagedContentType::Save)
					throw std::invalid_argument("the selected installation is no longer convertible");
				auto plan = m_controller.PlanWuaConversion(entry->titleId, uid);
				if (!plan || plan->items.empty())
					throw std::runtime_error("no installed content was found for conversion");
				const auto token = ++m_nextOperationToken;
				const auto suggested = plan->suggestedFileName;
				m_wuaPlans.emplace(token, WuaPlanRecord{m_invokingWindow, entry->titleId, uid, *plan});
				rapidjson::StringBuffer buffer;
				JsonWriter writer(buffer);
				writer.StartObject();
				writer.Key("planToken");
				writer.String(std::to_string(token).c_str());
				writer.Key("suggestedFileName");
				writer.String(suggested.data(), suggested.size());
				writer.Key("items");
				writer.StartArray();
				for (const auto& item : plan->items)
				{
					writer.StartObject();
					writer.Key("titleId");
					writer.String(TitleIdString(item.titleId).c_str());
					writer.Key("version");
					writer.Uint(item.version);
					writer.Key("role");
					writer.String(item.role == Application::ContentRole::Update ? "update" : item.role == Application::ContentRole::Dlc ? "dlc"
																																		: "base");
					writer.Key("displayPath");
					writer.String(item.displayPath.data(), item.displayPath.size());
					writer.EndObject();
				}
				writer.EndArray();
				writer.EndObject();
				return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("titleManager.pickWuaDestination", [this](const rapidjson::Value& params) {
				RequireRole({"title-manager"});
				auto suggested = RequiredString(params, "suggestedFileName");
				const auto selected = m_nativeWindow->PickWuaDestination(std::string(suggested));
				if (!selected)
					return std::string(R"({"cancelled":true})");
				auto path = _utf8ToPath(*selected);
				if (path.extension() != ".wua")
					path += ".wua";
				const auto token = ++m_nextOperationToken;
				m_wuaDestinations.emplace(token, NativePathRecord{m_invokingWindow, std::move(path)});
				return std::string(R"({"cancelled":false,"destinationToken":)") + JsonString(std::to_string(token)) + "}";
			});
			m_rpc.Register("titleManager.startWua", [this](const rapidjson::Value& params) {
				RequireRole({"title-manager"});
				const auto planToken = ParseDecimalUint64(RequiredString(params, "planToken"), "planToken");
				const auto destinationToken = ParseDecimalUint64(RequiredString(params, "destinationToken"), "destinationToken");
				const auto stored = m_wuaPlans.find(planToken);
				const auto destination = m_wuaDestinations.find(destinationToken);
				if (stored == m_wuaPlans.end() || destination == m_wuaDestinations.end() || stored->second.owner != m_invokingWindow || destination->second.owner != m_invokingWindow)
					throw std::invalid_argument("WUA plan or destination token is invalid or expired");
				auto record = std::move(stored->second);
				auto output = std::move(destination->second.path);
				m_wuaPlans.erase(stored);
				m_wuaDestinations.erase(destination);
				const auto current = m_controller.PlanWuaConversion(record.titleId, record.preferredLocationUid);
				auto same = [](const Application::WuaConversionPlan& left, const Application::WuaConversionPlan& right) {
					if (left.items.size() != right.items.size())
						return false;
					for (std::size_t i = 0; i < left.items.size(); ++i)
					{
						const auto& a = left.items[i];
						const auto& b = right.items[i];
						if (a.locationUid != b.locationUid || a.titleId != b.titleId || a.version != b.version || a.fingerprint != b.fingerprint || a.role != b.role || a.displayPath != b.displayPath)
							return false;
					}
					return true;
				};
				if (!current || !same(record.plan, *current))
					throw std::runtime_error("WUA conversion plan is stale; review the current installed content again");
				return std::string(R"({"jobId":)") + JsonString(std::to_string(StartWuaConversionJob(std::move(*current), std::move(output)))) + "}";
			});
			m_rpc.Register("titleManager.planDelete", [this](const rapidjson::Value& params) {
				RequireRole({"title-manager"});
				const auto result = m_controller.PlanManagedContentDelete(ParseOpaqueUid(params));
				if (!result)
					throw std::runtime_error(result.diagnostic.empty() ? "unable to plan managed-content deletion" : result.diagnostic);
				const auto token = ++m_nextOperationToken;
				m_deletePlans.emplace(token, DeletePlanRecord{m_invokingWindow, *result.plan});
				const auto& plan = *result.plan;
				rapidjson::StringBuffer buffer;
				JsonWriter writer(buffer);
				writer.StartObject();
				writer.Key("planToken");
				writer.String(std::to_string(token).c_str());
				writer.Key("titleId");
				writer.String(TitleIdString(plan.titleId).c_str());
				writer.Key("name");
				writer.String(plan.name.data(), plan.name.size());
				writer.Key("displayPath");
				writer.String(plan.displayPath.data(), plan.displayPath.size());
				writer.EndObject();
				return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("titleManager.delete", [this](const rapidjson::Value& params) {
				RequireRole({"title-manager"});
				const auto token = ParseDecimalUint64(
					RequiredString(params, "planToken"), "planToken");
				const auto found = m_deletePlans.find(token);
				if (found == m_deletePlans.end() || found->second.owner != m_invokingWindow)
					throw std::invalid_argument("delete plan token is invalid or expired");
				auto plan = std::move(found->second.plan);
				m_deletePlans.erase(found);
				const auto result = m_controller.DeleteManagedContent(plan);
				if (!result)
					throw std::runtime_error(result.diagnostic.empty() ? "managed-content deletion failed" : result.diagnostic);
				return std::string("{}");
			});
			m_rpc.Register("checksum.getModel", [this](const rapidjson::Value&) {
				RequireRole({"checksum-tool", "title-manager"});
				auto typeName = [](Application::ManagedContentType type) {
					switch (type)
					{
					case Application::ManagedContentType::Update:
						return "update";
					case Application::ManagedContentType::Dlc:
						return "dlc";
					case Application::ManagedContentType::System:
						return "system";
					default:
						return "base";
					}
				};
				auto formatName = [](Application::ManagedContentFormat format) {
					switch (format)
					{
					case Application::ManagedContentFormat::Wud:
						return "wud";
					case Application::ManagedContentFormat::Nus:
						return "nus";
					case Application::ManagedContentFormat::Wua:
						return "wua";
					case Application::ManagedContentFormat::Wuhb:
						return "wuhb";
					default:
						return "folder";
					}
				};
				rapidjson::StringBuffer buffer;
				JsonWriter writer(buffer);
				writer.StartObject();
				writer.Key("entries");
				writer.StartArray();
				for (const auto& entry : m_controller.ListManagedContent())
				{
					writer.StartObject();
					writer.Key("locationUid");
					const auto uid = std::to_string(entry.locationUid);
					writer.String(uid.c_str());
					writer.Key("titleId");
					writer.String(TitleIdString(entry.titleId).c_str());
					writer.Key("name");
					writer.String(entry.name.data(), entry.name.size());
					writer.Key("version");
					writer.Uint(entry.version);
					writer.Key("region");
					writer.String(entry.regionName.data(), entry.regionName.size());
					writer.Key("type");
					writer.String(typeName(entry.type));
					writer.Key("format");
					writer.String(formatName(entry.format));
					writer.EndObject();
				}
				writer.EndArray();
				writer.EndObject();
				return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("checksum.start", [this](const rapidjson::Value& params) {
				RequireRole({"checksum-tool", "title-manager"});
				const auto uid = ParseOpaqueUid(params);
				const auto entries = m_controller.ListManagedContent();
				if (std::ranges::none_of(entries, [uid](const auto& entry) { return entry.locationUid == uid; }))
					throw std::invalid_argument("locationUid is not present in the managed-content catalog");
				return std::string(R"({"jobId":)") + JsonString(std::to_string(StartChecksumJob(uid))) + "}";
			});
			m_rpc.Register("cemod.discover", [this](const rapidjson::Value& params) {
				RequireRole({"cemod-manager", "cemod-permissions"});
				std::optional<std::uint64_t> titleId;
				if (params.IsObject() && params.HasMember("titleId"))
					titleId = ParseTitleId(params);
				return std::string(R"({"jobId":)") + JsonString(std::to_string(StartCemodSnapshotJob(titleId))) + "}";
			});
			m_rpc.Register("cemod.openPermissions", [this](const rapidjson::Value& params) {
				RequireRole({"cemod-manager"});
				const auto requestId = RequiredString(params, "requestId");
				if (requestId.empty() || requestId.size() > 128)
					throw std::invalid_argument("requestId must contain between 1 and 128 characters");
				const auto titleId = ParseTitleId(params);
				const auto packageKey = RequiredString(params, "packageKey");
				if (packageKey.empty() || packageKey.size() > 4096)
					throw std::invalid_argument("packageKey is invalid");
				const auto generation = ParseDecimalUint64(RequiredString(params, "generation"), "generation");
				const auto id = QueueToolWindow("cemod-permissions", std::string(requestId), titleId, std::string(packageKey), generation);
				return std::string(R"({"windowId":)") + JsonString(std::to_string(id)) + "}";
			});
			m_rpc.Register("cemod.setEnabled", [this](const rapidjson::Value& params) {
				RequireRole({"cemod-manager"});
				Application::CemodEnableUpdate update;
				update.packageKey = std::string(RequiredString(params, "packageKey"));
				if (update.packageKey.empty() || update.packageKey.size() > 4096)
					throw std::invalid_argument("packageKey is invalid");
				update.enabled = RequiredBool(params, "enabled");
				const auto result = m_controller.SetCemodEnabled(update);
				if (result)
					Emit("cemod.changed", R"({"reason":"enablement"})");
				return CemodResultJson(result);
			});
			m_rpc.Register("cemod.saveApproval", [this](const rapidjson::Value& params) {
				RequireRole({"cemod-permissions"});
				const auto found = m_toolWindows.find(m_invokingWindow);
				if (found == m_toolWindows.end() || !found->second->titleContext || !found->second->generationContext)
					throw std::runtime_error("approval context is unavailable");
				Application::CemodApprovalUpdate update;
				update.generation = ParseDecimalUint64(RequiredString(params, "generation"), "generation");
				update.titleId = ParseTitleId(params);
				update.packageKey = std::string(RequiredString(params, "packageKey"));
				update.grantedPermissions = ParseDecimalUint64(RequiredString(params, "grantedPermissions"), "grantedPermissions");
				update.approved = RequiredBool(params, "approved");
				update.trustUpdates = RequiredBool(params, "trustUpdates");
				if (update.titleId != *found->second->titleContext || update.generation != *found->second->generationContext || update.packageKey != found->second->packageContext)
					throw std::runtime_error("approval target does not match the exact modal context");
				const auto result = m_controller.SaveCemodApproval(update);
				if (result)
				{
					Emit("cemod.changed", R"({"reason":"approval"})");
					if (m_pendingLaunch && m_pendingLaunch->permissionWindow == m_invokingWindow &&
						m_pendingLaunch->titleId == update.titleId &&
						m_pendingLaunch->generation == update.generation &&
						m_pendingLaunch->packageKey == update.packageKey)
					{
						m_pendingLaunch->decisionSaved = true;
						m_pendingLaunch->approved = update.approved;
					}
				}
				return CemodResultJson(result);
			});
			m_rpc.Register("cemod.importLegacy", [this](const rapidjson::Value& params) {
				RequireRole({"cemod-manager"});
				if (!RequiredBool(params, "confirmed"))
					throw std::invalid_argument("legacy import requires explicit confirmation");
				const auto result = m_controller.ImportLegacyCemodPackageData(
					ParseDecimalUint64(RequiredString(params, "generation"), "generation"),
					ParseTitleId(params), RequiredString(params, "packageKey"));
				if (result)
					Emit("cemod.changed", R"({"reason":"legacyImport"})");
				return CemodResultJson(result);
			});
			m_rpc.Register("diagnostics.ppcThreadsSnapshot", [this](const rapidjson::Value&) {
				RequireRole({"ppc-threads"});
				const auto snapshot = m_controller.CapturePpcThreads();
				auto stateName = [](Application::PpcThreadState state) -> const char* {
					switch (state)
					{
					case Application::PpcThreadState::None:
						return "none";
					case Application::PpcThreadState::Ready:
						return "ready";
					case Application::PpcThreadState::Running:
						return "running";
					case Application::PpcThreadState::Waiting:
						return "waiting";
					case Application::PpcThreadState::Moribund:
						return "moribund";
					case Application::PpcThreadState::Suspended:
						return "suspended";
					default:
						return "unknown";
					}
				};
				auto hex = [](std::uint32_t value) {
					return fmt::format("{:08X}", value);
				};
				rapidjson::StringBuffer buffer;
				JsonWriter writer(buffer);
				writer.StartObject();
				writer.Key("generation");
				writer.String(std::to_string(snapshot.generation).c_str());
				writer.Key("available");
				writer.Bool(snapshot.available);
				writer.Key("diagnostic");
				writer.String(snapshot.diagnostic.data(), snapshot.diagnostic.size());
				writer.Key("threads");
				writer.StartArray();
				for (const auto& thread : snapshot.threads)
				{
					writer.StartObject();
					const auto address = hex(thread.address);
					writer.Key("address");
					writer.String(address.c_str());
					const auto identity = fmt::format("{:016X}", thread.identity);
					writer.Key("identity");
					writer.String(identity.c_str());
					const auto entry = hex(thread.entryPoint);
					writer.Key("entryPoint");
					writer.String(entry.c_str());
					const auto stackLow = hex(thread.stackLow);
					writer.Key("stackLow");
					writer.String(stackLow.c_str());
					const auto stackHigh = hex(thread.stackHigh);
					writer.Key("stackHigh");
					writer.String(stackHigh.c_str());
					const auto pc = hex(thread.instructionPointer);
					writer.Key("instructionPointer");
					writer.String(pc.c_str());
					const auto lr = hex(thread.linkRegister);
					writer.Key("linkRegister");
					writer.String(lr.c_str());
					writer.Key("state");
					writer.String(stateName(thread.state));
					writer.Key("requestedAffinity");
					writer.Uint(thread.requestedAffinity);
					writer.Key("effectiveAffinity");
					writer.Uint(thread.effectiveAffinity);
					writer.Key("basePriority");
					writer.Int(thread.basePriority);
					writer.Key("effectivePriority");
					writer.Int(thread.effectivePriority);
					const auto wake = std::to_string(thread.wakeUpTime);
					writer.Key("wakeUpTime");
					writer.String(wake.c_str());
					const auto cycles = std::to_string(thread.totalCycles);
					writer.Key("totalCycles");
					writer.String(cycles.c_str());
					writer.Key("name");
					writer.String(thread.name.data(), thread.name.size());
					writer.Key("gpr");
					writer.StartArray();
					for (const auto value : {thread.gpr3, thread.gpr4, thread.gpr5, thread.gpr6, thread.gpr7})
					{
						const auto formatted = hex(value);
						writer.String(formatted.c_str());
					}
					writer.EndArray();
					writer.Key("cancelRequested");
					writer.Bool(thread.cancelRequested);
					writer.Key("suspensionOwnedByFacade");
					writer.Bool(thread.suspensionOwnedByFacade);
					if (thread.waitingMutex != 0)
					{
						writer.Key("waitingMutex");
						writer.StartObject();
						const auto mutex = hex(thread.waitingMutex);
						writer.Key("address");
						writer.String(mutex.c_str());
						const auto owner = hex(thread.mutexOwner);
						writer.Key("owner");
						writer.String(owner.c_str());
						writer.Key("lockCount");
						writer.Uint(thread.mutexLockCount);
						writer.EndObject();
					}
					writer.EndObject();
				}
				writer.EndArray();
				writer.EndObject();
				return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("diagnostics.ppcThreadCommand", [this](const rapidjson::Value& params) {
				RequireRole({"ppc-threads"});
				Application::PpcThreadCommandRequest request;
				request.generation = ParseDecimalUint64(
					RequiredString(params, "generation"), "generation");
				request.threadAddress = RequiredHexAddress(params, "threadAddress");
				request.threadIdentity = RequiredHexIdentity(params, "threadIdentity");
				const auto command = RequiredString(params, "command");
				if (command == "suspend")
					request.command = Application::PpcThreadCommand::Suspend;
				else if (command == "resume")
					request.command = Application::PpcThreadCommand::Resume;
				else
				{
					request.command = Application::PpcThreadCommand::AdjustPriority;
					if (command == "boost1")
						request.priorityDelta = -1;
					else if (command == "boost5")
						request.priorityDelta = -5;
					else if (command == "decrease1")
						request.priorityDelta = 1;
					else if (command == "decrease5")
						request.priorityDelta = 5;
					else
						throw std::invalid_argument("unknown PPC thread command");
				}
				const auto result = m_controller.ExecutePpcThreadCommand(request);
				return std::string(R"({"applied":)") + (result.applied ? "true" : "false") +
					   R"(,"diagnostic":)" + JsonString(result.diagnostic) + "}";
			});
			m_rpc.Register("memorySearch.start", [this](const rapidjson::Value& params) {
				RequireRole({"memory-searcher"});
				const auto& value = RequiredMember(params, "value");
				return MemorySessionJson(m_memorySearch.Start(m_invokingWindow,
															  {ParseMemoryValue(value), RequiredBoundedUint(params, "maximumBytes", 1,
																											static_cast<std::uint32_t>(Application::MemorySearchFacade::MaximumScanBytes))}));
			});
			m_rpc.Register("memorySearch.filter", [this](const rapidjson::Value& params) {
				RequireRole({"memory-searcher"});
				return MemorySessionJson(m_memorySearch.Filter(m_invokingWindow,
															   RequiredString(params, "sessionToken"),
															   ParseDecimalUint64(RequiredString(params, "generation"), "generation"),
															   ParseMemoryValue(RequiredMember(params, "value"))));
			});
			m_rpc.Register("memorySearch.status", [this](const rapidjson::Value& params) {
				RequireRole({"memory-searcher"});
				return MemoryStatusJson(m_memorySearch.Status(m_invokingWindow,
															  RequiredString(params, "sessionToken")));
			});
			m_rpc.Register("memorySearch.page", [this](const rapidjson::Value& params) {
				RequireRole({"memory-searcher"});
				return MemoryPageJson(m_memorySearch.Page(m_invokingWindow,
														  RequiredString(params, "sessionToken"),
														  ParseDecimalUint64(RequiredString(params, "generation"), "generation"),
														  RequiredUint(params, "offset"), RequiredBoundedUint(params, "limit", 1, Application::MemorySearchFacade::MaximumPageSize)));
			});
			m_rpc.Register("memorySearch.cancel", [this](const rapidjson::Value& params) {
				RequireRole({"memory-searcher"});
				m_memorySearch.Cancel(m_invokingWindow, RequiredString(params, "sessionToken"),
									  ParseDecimalUint64(RequiredString(params, "generation"), "generation"));
				return std::string("{}");
			});
			m_rpc.Register("ppcDebugger.snapshot", [this](const rapidjson::Value& params) {
				RequireRole({"ppc-debugger"});
				return PpcDebuggerSnapshotJson(m_ppcDebugger.Capture(m_invokingWindow,
																	 {RequiredHexAddress(params, "center")}, RequiredBoundedUint(params, "instructionCount", 1, Application::PpcDebuggerFacade::MaximumInstructionCount)));
			});
			m_rpc.Register("ppcDebugger.toggleBreakpoint", [this](const rapidjson::Value& params) {
				RequireRole({"ppc-debugger"});
				m_ppcDebugger.ToggleExecuteBreakpoint(m_invokingWindow,
													  ParseDecimalUint64(RequiredString(params, "generation"), "generation"),
													  {RequiredHexAddress(params, "address")});
				return std::string("{}");
			});
			m_rpc.Register("ppcDebugger.setBreakpointEnabled", [this](const rapidjson::Value& params) {
				RequireRole({"ppc-debugger"});
				m_ppcDebugger.SetBreakpointEnabled(m_invokingWindow,
												   ParseDecimalUint64(RequiredString(params, "generation"), "generation"),
												   RequiredString(params, "identity"), RequiredBool(params, "enabled"));
				return std::string("{}");
			});
			m_rpc.Register("ppcDebugger.deleteBreakpoint", [this](const rapidjson::Value& params) {
				RequireRole({"ppc-debugger"});
				m_ppcDebugger.DeleteBreakpoint(m_invokingWindow,
											   ParseDecimalUint64(RequiredString(params, "generation"), "generation"),
											   RequiredString(params, "identity"));
				return std::string("{}");
			});
			m_rpc.Register("ppcDebugger.control", [this](const rapidjson::Value& params) {
				RequireRole({"ppc-debugger"});
				const auto command = RequiredString(params, "command");
				const auto parsed = command == "break" ? Application::PpcDebuggerControl::Break : command == "run"	  ? Application::PpcDebuggerControl::Run
																							  : command == "stepInto" ? Application::PpcDebuggerControl::StepInto
																							  : command == "stepOver" ? Application::PpcDebuggerControl::StepOver
																													  : throw std::invalid_argument("unknown PPC debugger control command");
				m_ppcDebugger.Control(m_invokingWindow,
									  ParseDecimalUint64(RequiredString(params, "generation"), "generation"), parsed);
				return std::string("{}");
			});
			m_rpc.Register("jobs.cancel", [this](const rapidjson::Value& params) {
				const auto jobId = ParseDecimalUint64(RequiredString(params, "jobId"), "jobId");
				const auto job = m_backgroundJobs.find(jobId);
				if (job == m_backgroundJobs.end() || job->second->ownerWindow != m_invokingWindow)
					throw std::runtime_error("background job was not found for this window");
				job->second->cancelled->store(true, std::memory_order_release);
				job->second->worker.request_stop();
				return std::string("{}");
			});
			m_rpc.Register("titles.list", [this](const rapidjson::Value&) {
				rapidjson::Document document(rapidjson::kArrayType);
				auto& allocator = document.GetAllocator();
				for (const auto& game : m_controller.ListGames())
				{
					rapidjson::Value item(rapidjson::kObjectType);
					const auto titleId = TitleIdString(game.titleId);
					item.AddMember("titleId", rapidjson::Value(titleId.c_str(), allocator), allocator);
					item.AddMember("name", rapidjson::Value(game.name.c_str(), allocator), allocator);
					const auto path = _pathToUtf8(game.basePath);
					item.AddMember("path", rapidjson::Value(path.c_str(), allocator), allocator);
					item.AddMember("region", rapidjson::Value(game.regionName.c_str(), allocator), allocator);
					item.AddMember("version", game.version, allocator);
					item.AddMember("playTimeMinutes", game.playStats.minutesPlayed, allocator);
					if (game.playStats.available)
					{
						std::array<char, 11> date{};
						std::snprintf(date.data(), date.size(), "%04u-%02u-%02u",
									  game.playStats.lastPlayedYear, game.playStats.lastPlayedMonth,
									  game.playStats.lastPlayedDay);
						item.AddMember("lastPlayed", rapidjson::Value(date.data(), allocator), allocator);
					}
					else
						item.AddMember("lastPlayed", rapidjson::Value(rapidjson::kNullType), allocator);
					document.PushBack(item, allocator);
				}
				rapidjson::StringBuffer buffer;
				rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
				document.Accept(writer);
				return std::string(buffer.GetString(), buffer.GetSize());
			});
			m_rpc.Register("titles.icon", [this](const rapidjson::Value& params) {
				RequireRole({"main-library"});
				const auto titleId = ParseTitleId(params);
				const auto [cached, inserted] =
					m_titleIconCache.try_emplace(titleId, std::string{});
				if (inserted)
				{
					if (const auto icon = m_controller.LoadTitleIcon(titleId))
						if (const auto dataUrl = TgaDataUrl(*icon))
							cached->second = *dataUrl;
				}
				const auto titleIdText = TitleIdString(titleId);
				return std::string(R"({"titleId":)") + JsonString(titleIdText) +
					   R"(,"iconDataUrl":)" +
					   (cached->second.empty() ? "null" : JsonString(cached->second)) + "}";
			});
			m_rpc.Register("titles.refresh", [this](const rapidjson::Value&) {
				m_titleIconCache.clear();
				m_controller.RefreshTitles();
				return "{}";
			});
			m_rpc.Register("titles.launch", [this](const rapidjson::Value& params) {
				RequireRole({"main-library"});
				const auto game = m_controller.GetGame(ParseTitleId(params));
				if (!game)
					throw std::invalid_argument("titleId is not present in the game library");
				return Launch(game->basePath, game->titleId);
			});
			m_rpc.Register("emulation.stop", [this](const rapidjson::Value&) {
				const auto result = m_controller.Stop();
				if (!result.stopped)
					throw std::runtime_error(result.diagnostic);
				if (!DestroyMainRenderRegion())
					throw std::runtime_error("native renderer surfaces could not be detached safely");
				(void)m_windowState->FinishEmulation();
				FinishGameWindowLifetime();
				return "{}";
			});
			m_rpc.VerifyMethods(WebFrontend::Generated::RpcMethods);
		}

		std::string Launch(const fs::path& path, std::uint64_t titleId)
		{
			if (m_pendingLaunch)
				throw std::runtime_error(
					"another title launch is awaiting an exact package approval");
			std::uint64_t ownerGeneration{};
			if (m_invokingWindow != 0)
			{
				const auto owner = m_toolWindows.find(m_invokingWindow);
				if (owner == m_toolWindows.end())
					throw std::runtime_error("the launch owner is no longer active");
				ownerGeneration = owner->second->generation;
			}
			if (!m_windowState->BeginLaunch())
				throw std::runtime_error("main window is not ready to launch a title");
			m_pendingLaunch = PendingLaunch{path, titleId, m_invokingWindow, ownerGeneration};
			try
			{
				return ContinuePendingLaunch();
			} catch (...)
			{
				CancelPendingLaunch("failed", "Title launch preflight failed.");
				throw;
			}
		}

		std::string ContinuePendingLaunch()
		{
			if (!m_pendingLaunch || m_rpc.IsShuttingDown())
				return R"({"status":"cancelled"})";
			if (m_pendingLaunch->ownerWindow != 0)
			{
				const auto owner = m_toolWindows.find(m_pendingLaunch->ownerWindow);
				if (owner == m_toolWindows.end() ||
					owner->second->generation != m_pendingLaunch->ownerGeneration)
					throw std::runtime_error("the launch owner is no longer active");
			}
			const auto preflight = m_controller.GetCemodLaunchPreflight(
				m_pendingLaunch->titleId);
			if (!preflight.pendingApprovals.empty())
			{
				const auto& approval = preflight.pendingApprovals.front();
				m_pendingLaunch->generation = approval.generation;
				m_pendingLaunch->packageKey = approval.packageKey;
				m_pendingLaunch->decisionSaved = false;
				m_pendingLaunch->approved = false;
				(void)QueueToolWindow("cemod-permissions", {}, approval.titleId,
									  approval.packageKey, approval.generation, true);
				Emit("titles.launchState", std::string(
											   R"({"status":"awaitingPermission","titleId":)") +
											   JsonString(TitleIdString(approval.titleId)) + R"(,"packageKey":)" +
											   JsonString(approval.packageKey) + "}");
				return std::string(R"({"status":"awaitingPermission","titleId":)") +
					   JsonString(TitleIdString(approval.titleId)) + "}";
			}

			const auto path = m_pendingLaunch->path;
			const auto expectedTitleId = m_pendingLaunch->titleId;
			const auto frontendSettings = m_controller.GetFrontendSettings();
			const bool launchFullscreen = frontendSettings.fullscreenOverride.value_or(
				frontendSettings.startFullscreen);
			const bool previousFullscreen = m_fullscreen;
			const auto result = m_controller.Launch({path}, [this, launchFullscreen](const Application::LaunchResult&) {
					m_fullscreen = launchFullscreen;
					auto& region = m_nativeWindow->CreateMainRenderRegion();
					m_nativeWindow->SetFullscreen(m_fullscreen);
					// GTK destroys the Wayland wl_surface while the temporary game
					// window is hidden. Show/map it before RendererHost publishes the
					// handle and creates the VkSurfaceKHR.
					ShowRenderContent();
					CreateMainRuntimeOverlay(region);
					m_hostState->UpdateMetrics(m_nativeWindow->GetMetrics());
					m_rendererHost->InitializeMain(region);
					if (!m_windowState->CommitLaunch())
						throw std::runtime_error("main window content transition failed"); }, [this, previousFullscreen] {
					m_rendererHost->AbandonMainInitialization();
					DestroyMainRenderRegion();
					ShowLibraryContent();
					(void)m_windowState->RollbackLaunch();
					if (m_fullscreen != previousFullscreen)
					{
						m_fullscreen = previousFullscreen;
						m_nativeWindow->SetFullscreen(previousFullscreen);
					} });
			if (!result)
			{
				m_pendingLaunch.reset();
				if (m_controller.State() == Application::EmulationState::Running)
				{
					auto& region = m_nativeWindow->CreateMainRenderRegion();
					CreateMainRuntimeOverlay(region);
					(void)m_windowState->CommitLaunch();
					ShowRenderContent();
				}
				else
				{
					DestroyMainRenderRegion();
					(void)m_windowState->RollbackLaunch();
					if (m_launcherClosed)
						(void)RequestShutdown();
					else
						ShowLibraryContent();
				}
				const auto diagnostic = result.diagnostic.empty() ? "title launch failed" : result.diagnostic;
				Emit("titles.launchState", std::string(R"({"status":"failed","titleId":)") +
											   JsonString(TitleIdString(expectedTitleId)) + R"(,"diagnostic":)" +
											   JsonString(diagnostic) + "}");
				throw std::runtime_error(diagnostic);
			}
			m_pendingLaunch.reset();
			if (frontendSettings.openPad && !m_nativeWindow->IsPadRenderRegionOpen())
				TogglePadRenderRegion();
			Emit("titles.launchState", std::string(R"({"status":"started","titleId":)") +
										   JsonString(TitleIdString(result.titleId)) + "}");
			return std::string(R"({"status":"started","titleId":)") +
				   JsonString(TitleIdString(result.titleId)) + "}";
		}

		void LaunchFromCommandLine() noexcept
		{
			try
			{
				std::optional<fs::path> path;
				std::optional<std::uint64_t> titleId;
				if (const auto requestedPath = LaunchSettings::GetLoadFile())
				{
					path = *requestedPath;
					titleId = m_controller.ResolveLaunchTitleId(*requestedPath);
				}
				else if (const auto requestedTitleId = LaunchSettings::GetLoadTitleID())
				{
					if (const auto title = m_controller.ResolveBaseTitle(*requestedTitleId))
					{
						path = title->path;
						titleId = title->titleId;
					}
				}

				if (!path || !titleId)
					throw std::runtime_error("command-line title could not be resolved");
				const auto preflight = m_controller.GetCemodLaunchPreflight(*titleId);
				if (!preflight.pendingApprovals.empty())
					throw std::runtime_error(
						"command-line title requires CemuMod permission approval in the GUI first");
				(void)Launch(*path, *titleId);
			} catch (const std::exception& error)
			{
				m_exitCode = EXIT_FAILURE;
				cemuLog_log(LogType::Force, "Command-line title launch failed: {}", error.what());
				(void)RequestShutdown();
			} catch (...)
			{
				m_exitCode = EXIT_FAILURE;
				cemuLog_log(LogType::Force,
							"Command-line title launch failed with an unknown error");
				(void)RequestShutdown();
			}
		}

		void CancelPendingLaunch(std::string_view status,
								 std::string_view diagnostic) noexcept
		{
			if (!m_pendingLaunch)
				return;
			const auto titleId = m_pendingLaunch->titleId;
			m_pendingLaunch.reset();
			if (m_windowState && m_windowState->Snapshot().mode ==
									 WebFrontend::MainWindowContentMode::LaunchPending)
				(void)m_windowState->RollbackLaunch();
			try
			{
				if (!m_rpc.IsShuttingDown())
					Emit("titles.launchState", std::string(R"({"status":)") +
												   JsonString(status) + R"(,"titleId":)" +
												   JsonString(TitleIdString(titleId)) + R"(,"diagnostic":)" +
												   JsonString(diagnostic) + "}");
			} catch (...)
			{}
		}

		void HandleLaunchPermissionOpenFailure(std::uint64_t windowId,
											   std::string_view diagnostic) noexcept
		{
			if (m_pendingLaunch && m_pendingLaunch->permissionWindow == windowId)
				CancelPendingLaunch("failed", diagnostic);
		}

		void ResumePendingLaunchNoexcept() noexcept
		{
			try
			{
				(void)ContinuePendingLaunch();
			} catch (const std::exception& error)
			{
				CancelPendingLaunch("failed", error.what());
			} catch (...)
			{
				CancelPendingLaunch("failed", "Unknown title launch continuation failure.");
			}
		}

		std::unique_ptr<INativeWindowHost> m_nativeWindow;
#if defined(CEMU_OVERLAY_BACKEND_CEF)
		std::shared_ptr<WebFrontend::CefOverlay::BrowserRuntime> m_cefOverlay;
		std::shared_ptr<WebFrontend::CemodWebUiFrontend> m_cemodWebUi;
#else
		std::shared_ptr<Host::IOverlayFrameSource> m_cefOverlay;
#endif
		RpcDispatcher m_rpc;
		std::unordered_map<std::uint64_t, std::unique_ptr<ToolWindow>> m_toolWindows;
		std::unordered_map<std::string, std::uint64_t> m_windowByRole;
		std::unordered_map<std::string, std::uint64_t> m_pendingWindowRoles;
		std::unordered_map<std::string, std::vector<std::string>> m_pendingWindowRequests;
		std::unordered_map<std::string, std::optional<std::uint64_t>> m_pendingWindowContexts;
		std::unordered_map<std::string, std::string> m_pendingPackageContexts;
		std::unordered_map<std::string, std::optional<std::uint64_t>> m_pendingGenerationContexts;
		std::unordered_map<std::uint64_t, std::unique_ptr<BackgroundJob>> m_backgroundJobs;
		std::unordered_map<std::string, SaveTicket> m_saveTickets;
		std::unordered_map<std::uint64_t, WuaPlanRecord> m_wuaPlans;
		std::unordered_map<std::uint64_t, InstallPlanRecord> m_installPlans;
		std::unordered_map<std::uint64_t, DeletePlanRecord> m_deletePlans;
		std::unordered_map<std::uint64_t, NativePathRecord> m_installSources;
		std::unordered_map<std::uint64_t, NativePathRecord> m_wuaDestinations;
		std::uint64_t m_nextWindowId{};
		std::uint64_t m_nextWindowGeneration{};
		UiTheme m_theme{UiTheme::Light};
		std::uint64_t m_themeRevision{1};
		std::string m_language{"system"};
		std::uint64_t m_languageRevision{1};
		std::uint64_t m_nextBackgroundJobId{};
		std::uint64_t m_nextSaveTicketId{};
		WebFrontend::UpdatePlanRegistry m_updatePlans;
		std::uint64_t m_nextOperationToken{};
		std::optional<PendingLaunch> m_pendingLaunch;
		std::uint64_t m_invokingWindow{};
		std::shared_ptr<WebHostState> m_hostState{std::make_shared<WebHostState>()};
		std::shared_ptr<WebHostServices> m_hostServices;
		std::unique_ptr<IRendererHost> m_rendererHost;
		Application::EmulationController m_controller;
		Application::LoggingFacade m_logging;
		Application::EmulatedUsbFacade m_emulatedUsb{Application::CreateEmulatedUsbBackend()};
		Application::DiagnosticFacade m_diagnostics;
		Application::MemorySearchFacade m_memorySearch{
			Application::CreateCafeMemoryDiagnosticBackend()};
		Application::PpcDebuggerFacade m_ppcDebugger{
			Application::CreateCafePpcDebuggerBackend()};
		std::unique_ptr<MainWindowState> m_windowState;
		std::shared_ptr<RuntimeCallbackGate> m_callbackGate{std::make_shared<RuntimeCallbackGate>()};
		Application::EventSubscription m_applicationEvents;
		Application::TitleCatalogSubscription m_titleEvents;
		Application::LoggingSubscription m_loggingEvents;
		std::atomic_bool m_loggingFlushPending{};
		std::atomic_bool m_overlayFlushPending{};
		std::atomic_bool m_mainFrameRedrawPending{};
		std::atomic_bool m_padFrameRedrawPending{};
		std::atomic_bool m_gameClosePending{};
		std::atomic<RuntimeOverlay::Interaction> m_overlayInteraction{
			RuntimeOverlay::Interaction::Passive};
		std::array<bool, 7> m_overlayNavigationButtons{};
		std::uint64_t m_lastForwardedLogSequence{};
		std::atomic_bool m_stopping{};
		std::shared_ptr<std::atomic_bool> m_eventStopping{std::make_shared<std::atomic_bool>()};
		std::mutex m_eventMutex;
		std::uint64_t m_eventSequence{};
		std::uint64_t m_padGeneration{};
		struct PointerState
		{
			std::int32_t x{};
			std::int32_t y{};
			std::int32_t width{};
			std::int32_t height{};
			bool inside{};
		};
		std::array<PointerState, 2> m_pointerStates;
		std::array<Frontend::CemuExtendFrontendBridge, 2> m_pointerBridges;
		std::array<bool, 2> m_webUiPointerOwnership{};
		WebFrontend::NativeKeyboardFocusTracker m_nativeKeyboardFocus;
		WebFrontend::InputFocusState m_inputFocus;
		bool m_managedInputFocus{};
		std::array<WebFrontend::CefOverlay::InputOwnership, 2> m_surfaceInputOwnership{};
		mutable std::mutex m_hotkeyMutex;
		Application::HotkeySettingsModel m_hotkeySettings;
		std::atomic_bool m_hotkeyEditing{};
		std::uint64_t m_textInputSequence{};
		std::string m_mainWorkspaceRole{"main-library"};
		std::unordered_map<std::uint64_t, std::string> m_titleIconCache;
		bool m_fullscreen{};
		bool m_launcherClosed{};
		bool m_nativeUiLoopInitialized{};
		bool m_cleanedUp{};
		bool m_applicationShutdown{};
		bool m_hostConnected{};
		bool m_terminateWhenToolsClosed{};
		bool m_mainReplyPending{};
		bool m_commandLineLaunch{};
		int m_exitCode{EXIT_SUCCESS};
		Host::NativeSurfacePublication m_mainWindowPublication{};
	};
} // namespace

void Frontend::Run()
{
#if BOOST_OS_LINUX
	// CEF hosts the launcher and the tool windows as windowed X11 children that
	// the GTK host reparents by XID, so the frontend needs the X11 GDK backend
	// even on a Wayland desktop, where XWayland provides it. This has to happen
	// before CefInitialize: Chromium initializes GTK itself and would otherwise
	// open a Wayland display. CefApp::OnBeforeCommandLineProcessing pins the
	// matching Ozone platform.
	if (std::getenv("DISPLAY"))
		setenv("GDK_BACKEND", "x11", 1);
	// Most distributions set yama/ptrace_scope=1, which only lets a debugger
	// attach to its own descendants. A frontend hang has to be inspected while it
	// is happening, so allow an explicit opt-in to be attached from outside.
	if (const char* allowPtrace = std::getenv("CEMU_ALLOW_PTRACE");
		allowPtrace && std::string_view(allowPtrace) == "1")
	{
		prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
	}
#endif
	Application::InitializePaths();
	CemuCommonInit();
#if BOOST_OS_WINDOWS
	std::exception_ptr uiFailure;
	int exitCode{EXIT_SUCCESS};
	std::thread uiThread([&uiFailure, &exitCode] {
		SetThreadName("cemu-web-ui");
		const auto initialized = CoInitializeEx(nullptr,
												COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		if (FAILED(initialized))
		{
			uiFailure = std::make_exception_ptr(
				std::runtime_error("failed to initialize the CEF STA UI thread"));
			return;
		}
		try
		{
			Runtime runtime;
			runtime.Run();
			exitCode = runtime.ExitCode();
		} catch (...)
		{
			uiFailure = std::current_exception();
		}
		CoUninitialize();
	});
	uiThread.join();
	if (uiFailure)
		std::rethrow_exception(uiFailure);
	if (LaunchSettings::GetLoadFile() || LaunchSettings::GetLoadTitleID())
		ExitProcess(static_cast<UINT>(exitCode));
#else
	SetThreadName("cemu-web-ui");
	int exitCode{EXIT_SUCCESS};
	{
		Runtime runtime;
		runtime.Run();
		exitCode = runtime.ExitCode();
	}
	// Cemu owns process-lifetime worker objects whose static destruction is not
	// safe after the frontend has shut the application down. Runtime cleanup has
	// already released emulation and native UI resources at this point.
	std::_Exit(exitCode);
#endif
}
