#include "Common/precompiled.h"

#include "webview/cef/CefOverlayRuntime.h"
#include "webview/cef/CefOverlayFrameMailbox.h"
#include "webview/cef/CefOverlayInput.h"
#include "webview/cef/CefNativeUiLoop.h"
#include "webview/CemodNetworkPolicy.h"
#include "webview/CemodNetworkProxy.h"
#include "webview/generated/WebAssets.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_dialog_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_download_handler.h"
#include "include/cef_parser.h"
#include "include/cef_permission_handler.h"
#include "include/cef_request.h"
#include "include/cef_request_context.h"
#include "include/cef_resource_handler.h"
#include "include/cef_scheme.h"
#include "include/cef_stream.h"
#include "include/cef_values.h"
#include "include/cef_v8.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_message_router.h"
#include "include/wrapper/cef_resource_manager.h"
#include "include/wrapper/cef_stream_resource_handler.h"
#if defined(OS_MAC)
#include "include/wrapper/cef_library_loader.h"
#endif

#if defined(OS_WIN)
#include <windows.h>
#elif defined(OS_MAC)
#include <mach-o/dyld.h>
#endif

#include <condition_variable>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <unordered_set>
#include <unordered_map>

namespace WebFrontend::CefOverlay
{
	namespace
	{
		constexpr char kScheme[] = "cemu";
		constexpr char kDomain[] = "ui";
		constexpr char kCemodScheme[] = "cemod-ui";

		std::string JsonString(std::string_view value)
		{
			std::string result{"\""};
			for (const unsigned char character : value)
			{
				switch (character)
				{
				case '\\':
					result += "\\\\";
					break;
				case '"':
					result += "\\\"";
					break;
				case '\b':
					result += "\\b";
					break;
				case '\f':
					result += "\\f";
					break;
				case '\n':
					result += "\\n";
					break;
				case '\r':
					result += "\\r";
					break;
				case '\t':
					result += "\\t";
					break;
				default:
					if (character < 0x20)
						result += fmt::format("\\u{:04x}", character);
					else
						result += static_cast<char>(character);
				}
			}
			result += '"';
			return result;
		}

		std::string PlatformName()
		{
#if defined(OS_WIN)
			return "windows";
#elif defined(OS_MAC)
			return "macos";
#else
			return "linux";
#endif
		}

		std::optional<std::string> LoopbackOrigin(std::string_view url)
		{
			CefURLParts parts;
			if (!CefParseURL(std::string(url), parts))
				return std::nullopt;
			const std::string scheme = CefString(&parts.scheme);
			const std::string host = CefString(&parts.host);
			const std::string port = CefString(&parts.port);
			if (scheme != "http" || (host != "127.0.0.1" && host != "localhost") || port.empty())
				return std::nullopt;
			return scheme + "://" + host + ':' + port;
		}

		bool IsAllowedUrl(std::string_view candidate, std::string_view initialUrl)
		{
			if (candidate.starts_with("cemu://ui/"))
				return true;
			CefURLParts initialParts;
			CefURLParts candidateParts;
			if (CefParseURL(std::string(initialUrl), initialParts) &&
				CefParseURL(std::string(candidate), candidateParts) &&
				CefString(&initialParts.scheme) == kCemodScheme &&
				CefString(&candidateParts.scheme) == kCemodScheme)
			{
				return CefString(&initialParts.host) == CefString(&candidateParts.host);
			}
			const auto initialOrigin = LoopbackOrigin(initialUrl);
			const auto candidateOrigin = LoopbackOrigin(candidate);
			return initialOrigin && candidateOrigin && *initialOrigin == *candidateOrigin;
		}

		std::string MakeBootstrap(const BrowserDescriptor& descriptor)
		{
			if (!descriptor.bootstrapJson.empty())
				return descriptor.bootstrapJson;
			std::string context = descriptor.contextJson.empty() ? "{}" : descriptor.contextJson;
			std::string result = "{windowId:" + JsonString(std::to_string(descriptor.windowId)) +
								 ",windowRole:" + JsonString(descriptor.role) +
								 ",platform:" + JsonString(PlatformName()) +
								 ",language:'system',context:" + context;
			if (descriptor.overlaySurface)
			{
				const char* surface = *descriptor.overlaySurface == Host::PointerSurface::Main ? "tv" : "pad";
				result += ",surface:" + JsonString(surface) + ",overlaySurface:" + JsonString(surface);
			}
			result += '}';
			return result;
		}

		void* NativeHandlePointer(CefWindowHandle handle)
		{
#if defined(OS_LINUX)
			return reinterpret_cast<void*>(static_cast<std::uintptr_t>(handle));
#else
			return static_cast<void*>(handle);
#endif
		}

		CefMessageRouterConfig RouterConfig()
		{
			CefMessageRouterConfig config;
			config.js_query_function = "cemuCefQuery";
			config.js_cancel_function = "cemuCefQueryCancel";
			return config;
		}

		std::mutex g_contextTasksMutex;
		bool g_contextInitialized{};
		std::vector<std::function<void()>> g_contextTasks;

		class App final : public CefApp,
						  public CefBrowserProcessHandler,
						  public CefRenderProcessHandler
		{
		  public:
			CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override
			{
				return this;
			}
			CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override
			{
				return this;
			}
			void OnContextInitialized() override;

			void OnScheduleMessagePumpWork(std::int64_t delayMs) override
			{
				CefNative::ScheduleCefMessagePump(
					std::chrono::milliseconds(std::max<std::int64_t>(delayMs, 0)),
					[] { DoProcessMessageLoopWork(); });
			}

			void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override
			{
				registrar->AddCustomScheme(kScheme,
										   CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
											   CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED);
				registrar->AddCustomScheme(kCemodScheme,
										   CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
											   CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED);
			}

			void OnBeforeCommandLineProcessing(const CefString& processType,
											   CefRefPtr<CefCommandLine> commandLine) override
			{
				if (processType.empty())
				{
					commandLine->AppendSwitch("no-first-run");
					commandLine->AppendSwitch("disable-gpu-compositing");
					// Keep OSR compositing on the CPU while allowing Three.js-based Cemod
					// views to create hardware WebGL contexts. On Linux, avoid SwANGLE's
					// Vulkan backend: the portable launcher deliberately selects the host
					// Vulkan ICD for Cemu, whereas Chromium's SwiftShader needs its private
					// ICD. ANGLE's desktop-GL backend keeps those loader paths independent.
					commandLine->AppendSwitchWithValue("use-gl", "angle");
#if defined(OS_LINUX)
					commandLine->AppendSwitchWithValue("use-angle", "gl");
#else
					commandLine->AppendSwitch("enable-unsafe-swiftshader");
#endif
					commandLine->AppendSwitch("disable-features=CalculateNativeWinOcclusion");
					const char* headless = std::getenv("CEMU_CEF_HEADLESS");
					const bool runHeadless = headless && std::string_view(headless) == "1";
					if (runHeadless)
					{
						commandLine->AppendSwitch("headless");
						commandLine->AppendSwitchWithValue("ozone-platform", "headless");
					}
#if defined(OS_LINUX)
					// The launcher and the tool windows host a windowed CEF child that
					// Cemu reparents into its GTK container by XID, and Chromium picks
					// GTK's GDK backend to match the Ozone platform it selected. In a
					// Wayland session that pairing hands out wl_surfaces the reparenting
					// path cannot use, so pin Ozone to X11 and let XWayland bridge the
					// desktop. An explicit switch on the command line still wins.
					else if (std::getenv("DISPLAY") &&
							 !commandLine->HasSwitch("ozone-platform") &&
							 !commandLine->HasSwitch("ozone-platform-hint"))
					{
						commandLine->AppendSwitchWithValue("ozone-platform", "x11");
					}
#endif
				}
			}

			void OnWebKitInitialized() override
			{
				m_rendererRouter = CefMessageRouterRendererSide::Create(RouterConfig());
			}

			void OnBrowserCreated(CefRefPtr<CefBrowser> browser,
								  CefRefPtr<CefDictionaryValue> extraInfo) override
			{
				if (extraInfo && extraInfo->HasKey("bootstrap"))
					m_bootstrap.insert_or_assign(browser->GetIdentifier(), extraInfo->GetString("bootstrap"));
				if (extraInfo && extraInfo->GetBool("cemodBridge"))
					m_cemodOrigins.insert_or_assign(browser->GetIdentifier(),
													extraInfo->GetString("cemodOrigin"));
			}

			void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override
			{
				m_bootstrap.erase(browser->GetIdentifier());
				m_cemodOrigins.erase(browser->GetIdentifier());
			}

			void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
								  CefRefPtr<CefV8Context> context) override
			{
				m_rendererRouter->OnContextCreated(browser, frame, context);
				if (!frame->IsMain())
					return;
				const auto entry = m_bootstrap.find(browser->GetIdentifier());
				if (entry == m_bootstrap.end())
					return;
				std::string script;
				const auto cemodOrigin = m_cemodOrigins.find(browser->GetIdentifier());
				CefURLParts frameUrl;
				const bool isCemodFrame = cemodOrigin != m_cemodOrigins.end() &&
										  CefParseURL(frame->GetURL(), frameUrl) &&
										  CefString(&frameUrl.scheme) == kCemodScheme &&
										  CefString(&frameUrl.host) == cemodOrigin->second;
				if (cemodOrigin != m_cemodOrigins.end() && !isCemodFrame)
					return;
				if (isCemodFrame)
				{
					script = "(()=>{const bootstrap=" + entry->second + ";"
																		"const listeners=new Map();"
																		"const parseFailure=(code,message)=>{let value;try{value=JSON.parse(message)}catch{}"
																		"const error=new Error(value?.message??message);error.code=value?.code??String(code);"
																		"error.details=value?.details??null;return error};"
																		"const invoke=(kind,value,options={})=>new Promise((resolve,reject)=>{"
																		"let settled=false,timer=0,abort;const finish=(fn,value)=>{if(settled)return;"
																		"settled=true;if(timer)clearTimeout(timer);if(abort&&options.signal)"
																		"options.signal.removeEventListener('abort',abort);fn(value)};"
																		"let request;try{request=JSON.stringify({bridge:1,kind,value})}catch(error){reject(error);return}"
																		"const queryId=window.cemuCefQuery({request,onSuccess:(raw)=>{try{finish(resolve,JSON.parse(raw))}"
																		"catch(error){finish(reject,error)}},onFailure:(code,message)=>"
																		"finish(reject,parseFailure(code,message))});"
																		"const cancel=(code,message)=>{if(settled)return;window.cemuCefQueryCancel(queryId);"
																		"finish(reject,Object.assign(new Error(message),{code,details:null}))};"
																		"if(options.signal){abort=()=>cancel('ABORTED','Cemod call was aborted');"
																		"if(options.signal.aborted){abort();return}options.signal.addEventListener('abort',abort,{once:true})}"
																		"if(options.timeoutMs)timer=setTimeout(()=>cancel('TIMED_OUT','Cemod call timed out'),options.timeoutMs)"
																		"});"
																		"const data=(value)=>value===undefined?null:value;"
																		"const api=Object.freeze({"
																		"info:Object.freeze(bootstrap),"
																		"ready:(value)=>invoke('ready',data(value)),"
																		"call:(name,value,options={})=>{const timeoutMs=options.timeoutMs===undefined?10000:Number(options.timeoutMs);"
																		"if(!Number.isInteger(timeoutMs)||timeoutMs<1000||timeoutMs>60000)"
																		"return Promise.reject(Object.assign(new RangeError('timeoutMs must be 1000..60000'),{code:'INVALID_ARGUMENT'}));"
																		"return invoke('call',{name,data:data(value)},{signal:options.signal,timeoutMs})},"
																		"send:(name,value)=>invoke('send',{name,data:data(value)}),"
																		"on:(name,handler)=>{if(typeof name!=='string'||typeof handler!=='function')"
																		"throw new TypeError('invalid Cemod event listener');"
																		"let set=listeners.get(name);if(!set)listeners.set(name,set=new Set());"
																		"set.add(handler);return()=>set.delete(handler)},"
																		"close:(reason)=>invoke('close',{reason:reason===undefined?null:String(reason)})});"
																		"Object.defineProperty(window,'cemod',{value:api,writable:false,configurable:false});"
																		"Object.defineProperty(window,'__cemodDispatchEvent',{value:(name,payload)=>{"
																		"const set=listeners.get(name);if(set)for(const handler of [...set])"
																		"try{handler(payload)}catch(error){console.error(error)}},writable:false,configurable:false});"
																		"})();";
				}
				else
				{
					script = "window.__CEMU_BOOTSTRAP__=" + entry->second + ";"
																			"window.cemuInvoke=(request)=>new Promise((resolve,reject)=>"
																			"window.cemuCefQuery({request,onSuccess:resolve,onFailure:(code,message)=>"
																			"reject(new Error('CEF RPC '+code+': '+message))}));"
																			"(()=>{const mark=()=>{if(document.documentElement&&"
																			"window.__CEMU_BOOTSTRAP__.overlaySurface)"
																			"document.documentElement.dataset.runtimeOverlay='active'};mark();"
																			"document.addEventListener('DOMContentLoaded',mark,{once:true})})();";
				}
				CefRefPtr<CefV8Value> result;
				CefRefPtr<CefV8Exception> exception;
				context->Eval(script, frame->GetURL(), 0, result, exception);
			}

			void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
								   CefRefPtr<CefV8Context> context) override
			{
				m_rendererRouter->OnContextReleased(browser, frame, context);
			}

			bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
										  CefRefPtr<CefFrame> frame, CefProcessId sourceProcess,
										  CefRefPtr<CefProcessMessage> message) override
			{
				return m_rendererRouter->OnProcessMessageReceived(
					browser, frame, sourceProcess, message);
			}

		  private:
			CefRefPtr<CefMessageRouterRendererSide> m_rendererRouter;
			std::unordered_map<int, std::string> m_bootstrap;
			std::unordered_map<int, std::string> m_cemodOrigins;
			IMPLEMENT_REFCOUNTING(App);
		};

		CefRefPtr<App> g_app;
		bool g_initialized{};
		int g_argc{};
		char** g_argv{};
		std::vector<std::string> g_arguments;
		std::vector<char*> g_argumentPointers;
		std::mutex g_runtimesMutex;
		std::vector<std::weak_ptr<BrowserRuntime>> g_runtimes;
#if defined(OS_MAC)
		std::unique_ptr<CefScopedLibraryLoader> g_libraryLoader;
#endif

		void RestoreArgumentPointers()
		{
			g_argumentPointers.clear();
			g_argumentPointers.reserve(g_arguments.size() + 1);
			for (auto& argument : g_arguments)
				g_argumentPointers.push_back(argument.data());
			g_argumentPointers.push_back(nullptr);
			g_argc = static_cast<int>(g_arguments.size());
			g_argv = g_argumentPointers.data();
		}

		std::filesystem::path ExecutablePath(std::error_code& error)
		{
#if defined(OS_WIN)
			std::wstring buffer(32768, L'\0');
			const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
													static_cast<DWORD>(buffer.size()));
			if (!length || length >= buffer.size())
			{
				error = std::make_error_code(std::errc::no_such_file_or_directory);
				return {};
			}
			buffer.resize(length);
			return std::filesystem::path(buffer);
#elif defined(OS_MAC)
			std::uint32_t size{};
			_NSGetExecutablePath(nullptr, &size);
			std::string buffer(size, '\0');
			if (_NSGetExecutablePath(buffer.data(), &size) != 0)
			{
				error = std::make_error_code(std::errc::no_such_file_or_directory);
				return {};
			}
			buffer.resize(std::char_traits<char>::length(buffer.c_str()));
			return std::filesystem::weakly_canonical(buffer, error);
#else
			return std::filesystem::read_symlink("/proc/self/exe", error);
#endif
		}

		std::filesystem::path CefResourceDirectory(const std::filesystem::path& executable)
		{
			if (const char* overridePath = std::getenv("CEMU_CEF_RESOURCES_DIR");
				overridePath && *overridePath)
				return overridePath;
#if defined(OS_MAC)
			const auto contents = executable.parent_path().parent_path();
			return contents / "Frameworks" / "Chromium Embedded Framework.framework" / "Resources";
#else
			return executable.parent_path();
#endif
		}

		std::filesystem::path CefSubprocessPath(const std::filesystem::path& executable)
		{
			if (const char* overridePath = std::getenv("CEMU_CEF_SUBPROCESS_PATH");
				overridePath && *overridePath)
				return overridePath;
#if defined(OS_MAC)
			const auto helper = executable.parent_path().parent_path() / "Frameworks" /
								"Cemu Helper.app" / "Contents" / "MacOS" / "Cemu Helper";
			std::error_code ignored;
			return std::filesystem::exists(helper, ignored) ? helper : executable;
#else
			return executable;
#endif
		}

		std::filesystem::path CefCacheRoot()
		{
#if defined(OS_WIN)
			if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData && *localAppData)
				return std::filesystem::path(localAppData) / "CemuExtend" / "cef";
#elif defined(OS_MAC)
			if (const char* home = std::getenv("HOME"); home && *home)
				return std::filesystem::path(home) / "Library" / "Caches" / "CemuExtend" / "cef";
#else
			if (const char* xdgCache = std::getenv("XDG_CACHE_HOME"); xdgCache && *xdgCache)
				return std::filesystem::path(xdgCache) / "CemuExtend" / "cef";
			if (const char* home = std::getenv("HOME"); home && *home)
				return std::filesystem::path(home) / ".cache" / "CemuExtend" / "cef";
#endif
			return std::filesystem::temp_directory_path() / "cemu-cef-cache";
		}
		class MemoryReadHandler final : public CefReadHandler
		{
		  public:
			explicit MemoryReadHandler(std::string data) : m_data(std::move(data)) {}
			size_t Read(void* output, size_t size, size_t count) override
			{
				if (!size || !count)
					return 0;
				const auto available = m_data.size() - std::min(m_position, m_data.size());
				const auto elements = std::min(count, available / size);
				const auto bytes = elements * size;
				std::memcpy(output, m_data.data() + m_position, bytes);
				m_position += bytes;
				return elements;
			}
			int Seek(std::int64_t offset, int whence) override
			{
				std::int64_t origin = whence == SEEK_CUR   ? static_cast<std::int64_t>(m_position)
									  : whence == SEEK_END ? static_cast<std::int64_t>(m_data.size())
														   : 0;
				const auto next = origin + offset;
				if (next < 0 || static_cast<std::size_t>(next) > m_data.size())
					return -1;
				m_position = static_cast<std::size_t>(next);
				return 0;
			}
			std::int64_t Tell() override
			{
				return static_cast<std::int64_t>(m_position);
			}
			int Eof() override
			{
				return m_position >= m_data.size();
			}
			bool MayBlock() override
			{
				return false;
			}

		  private:
			std::string m_data;
			std::size_t m_position{};
			IMPLEMENT_REFCOUNTING(MemoryReadHandler);
		};

		class BuiltinAssetFactory final : public CefSchemeHandlerFactory
		{
		  public:
			CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
												 const CefString&, CefRefPtr<CefRequest> request) override
			{
				CEF_REQUIRE_IO_THREAD();
				const CefURLParts parts = [](const CefString& url) {
					CefURLParts value;
					CefParseURL(url, value);
					return value;
				}(request->GetURL());
				const std::string path = CefString(&parts.path);
				if (path != "/" && path != "/index.html")
					return nullptr;
				const bool smoke = [] {
					const char* value = std::getenv("CEMU_CEF_OSR_SMOKE");
					return value && std::string_view(value) == "1";
				}();
				std::string html;
				if (smoke)
				{
					// Keep the CI OSR probe independent of the production React bundle's
					// parse time. The painted element is created only after a successful
					// round trip through the same CEF message-router RPC bridge.
					html = R"(<!doctype html><html><head><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="script-src 'nonce-cemu-cef-bootstrap'; style-src 'unsafe-inline'"><style>html,body{margin:0;width:100%;height:100%;background:transparent}#smoke{display:none;width:96px;height:48px;background:#25a7f0}</style></head><body><div id="smoke"></div><script nonce="cemu-cef-bootstrap">window.cemuInvoke(JSON.stringify({id:'cef-osr-smoke',method:'system.bootstrap',params:{}})).then(()=>document.getElementById('smoke').style.display='block');</script></body></html>)";
				}
				else
				{
					html.assign(reinterpret_cast<const char*>(WebAssets::html), WebAssets::htmlSize);
				}
				auto stream = CefStreamReader::CreateForHandler(new MemoryReadHandler(std::move(html)));
				return new CefStreamResourceHandler(200, "OK", "text/html", {}, stream);
			}

			IMPLEMENT_REFCOUNTING(BuiltinAssetFactory);
		};

		std::optional<std::string> DecodeCemodPath(std::string_view encoded)
		{
			if (encoded.empty() || encoded.front() != '/')
				return std::nullopt;
			std::string result;
			result.reserve(encoded.size());
			auto hex = [](char value) -> int {
				if (value >= '0' && value <= '9')
					return value - '0';
				if (value >= 'a' && value <= 'f')
					return value - 'a' + 10;
				if (value >= 'A' && value <= 'F')
					return value - 'A' + 10;
				return -1;
			};
			for (std::size_t index = 0; index < encoded.size(); ++index)
			{
				unsigned char value = static_cast<unsigned char>(encoded[index]);
				if (value == '%')
				{
					if (index + 2 >= encoded.size())
						return std::nullopt;
					const int high = hex(encoded[index + 1]);
					const int low = hex(encoded[index + 2]);
					if (high < 0 || low < 0)
						return std::nullopt;
					value = static_cast<unsigned char>((high << 4) | low);
					index += 2;
					if (value == '/' || value == '\\')
						return std::nullopt;
				}
				if (value == '\\' || value == 0 || value < 0x20 || value > 0x7e)
					return std::nullopt;
				result += static_cast<char>(value);
			}
			return result;
		}

		std::string CemodMimeType(std::string_view path)
		{
			const auto extension = std::filesystem::path(path).extension().string();
			if (extension == ".html" || extension == ".htm")
				return "text/html";
			if (extension == ".css")
				return "text/css";
			if (extension == ".js" || extension == ".mjs")
				return "text/javascript";
			if (extension == ".json" || extension == ".map")
				return "application/json";
			if (extension == ".svg")
				return "image/svg+xml";
			if (extension == ".png")
				return "image/png";
			if (extension == ".jpg" || extension == ".jpeg")
				return "image/jpeg";
			if (extension == ".gif")
				return "image/gif";
			if (extension == ".webp")
				return "image/webp";
			if (extension == ".woff")
				return "font/woff";
			if (extension == ".woff2")
				return "font/woff2";
			if (extension == ".txt")
				return "text/plain";
			return "application/octet-stream";
		}

		std::string CemodContentSecurityPolicy(const BrowserAssetBundle& bundle)
		{
			std::string connect = "'self'";
			for (const auto& origin : bundle.connectOrigins)
			{
				connect += ' ';
				connect += origin;
			}
			std::string resources = "'self' data:";
			for (const auto& origin : bundle.resourceOrigins)
			{
				resources += ' ';
				resources += origin;
			}
			return "default-src 'none'; base-uri 'none'; object-src 'none'; frame-src 'none'; "
				   "worker-src 'none'; navigate-to 'none'; "
				   "form-action 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
				   "connect-src " +
				   connect + "; img-src " + resources + "; font-src " +
				   resources + "; media-src " + resources;
		}

		bool IsCemodAssetUrl(std::string_view url, const BrowserAssetBundle& bundle)
		{
			CefURLParts parts;
			return CefParseURL(std::string(url), parts) &&
				   CefString(&parts.scheme) == kCemodScheme &&
				   CefString(&parts.host) == bundle.originId;
		}

		std::optional<CemodNetworkRequestKind> NetworkRequestKind(
			std::string_view url, cef_resource_type_t resourceType)
		{
			if (url.starts_with("wss://"))
				return CemodNetworkRequestKind::Connect;
			switch (resourceType)
			{
			case RT_XHR:
			case RT_PING:
				return CemodNetworkRequestKind::Connect;
			case RT_IMAGE:
			case RT_FONT_RESOURCE:
			case RT_MEDIA:
			case RT_FAVICON:
				return CemodNetworkRequestKind::Resource;
			default:
				return std::nullopt;
			}
		}

		bool IsCemodInitiator(std::string_view initiator, const BrowserAssetBundle& bundle)
		{
			if (initiator.empty())
				return false;
			CefURLParts parts;
			return CefParseURL(std::string(initiator), parts) &&
				   CefString(&parts.scheme) == kCemodScheme &&
				   CefString(&parts.host) == bundle.originId;
		}

		bool IsAllowedCemodNetworkRequest(std::string_view url, cef_resource_type_t resourceType,
										  const BrowserAssetBundle& bundle)
		{
			if (IsCemodAssetUrl(url, bundle))
				return true;
			const auto kind = NetworkRequestKind(url, resourceType);
			if (!kind || !IsCemodNetworkUrlAllowed(url, *kind,
												   bundle.connectOrigins, bundle.resourceOrigins))
				return false;
			if (bundle.allowPrivateNetwork)
				return true;
			const auto origin = ParseCemodNetworkOrigin(url);
			return origin && !IsLocalNetworkHostname(origin->host) &&
				   (!origin->addressLiteral || IsPublicNetworkAddress(origin->host));
		}

		class CemodAssetFactory final : public CefSchemeHandlerFactory
		{
		  public:
			explicit CemodAssetFactory(std::shared_ptr<const BrowserAssetBundle> bundle)
				: m_bundle(std::move(bundle)) {}

			CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
												 const CefString&, CefRefPtr<CefRequest> request) override
			{
				CEF_REQUIRE_IO_THREAD();
				const std::string method = request->GetMethod();
				if (method != "GET" && method != "HEAD")
					return nullptr;
				CefURLParts parts;
				if (!CefParseURL(request->GetURL(), parts) ||
					CefString(&parts.scheme) != kCemodScheme ||
					CefString(&parts.host) != m_bundle->originId)
					return nullptr;
				const std::string encodedPath = CefString(&parts.path);
				const auto decoded = DecodeCemodPath(encodedPath);
				if (!decoded)
					return nullptr;
				std::string_view path = *decoded;
				path.remove_prefix(1);
				const auto separator = path.find('/');
				const std::string viewId(path.substr(0, separator));
				const auto view = m_bundle->viewEntries.find(viewId);
				if (view == m_bundle->viewEntries.end())
					return nullptr;
				std::string assetPath;
				if (separator == std::string_view::npos || separator + 1 == path.size())
					assetPath = view->second;
				else
				{
					const std::string_view relative = path.substr(separator + 1);
					std::size_t cursor{};
					while (cursor <= relative.size())
					{
						const auto end = relative.find('/', cursor);
						const auto component = relative.substr(cursor,
															   end == std::string_view::npos ? relative.size() - cursor : end - cursor);
						if (component.empty() || component == "." || component == "..")
							return nullptr;
						if (end == std::string_view::npos)
							break;
						cursor = end + 1;
					}
					const auto root = view->second.rfind('/');
					assetPath = root == std::string::npos ? std::string(relative)
														  : view->second.substr(0, root + 1) + std::string(relative);
				}
				const auto asset = m_bundle->assets.find(assetPath);
				if (asset == m_bundle->assets.end())
					return nullptr;
				std::string data;
				if (method == "GET")
					data.assign(reinterpret_cast<const char*>(asset->second.data()), asset->second.size());
				CefResponse::HeaderMap headers;
				headers.emplace("X-Content-Type-Options", "nosniff");
				headers.emplace("Cache-Control", "no-store");
				headers.emplace("Content-Length", std::to_string(asset->second.size()));
				const auto mime = CemodMimeType(assetPath);
				if (mime == "text/html")
					headers.emplace("Content-Security-Policy", CemodContentSecurityPolicy(*m_bundle));
				auto stream = CefStreamReader::CreateForHandler(new MemoryReadHandler(std::move(data)));
				return new CefStreamResourceHandler(200, "OK", mime, headers, stream);
			}

		  private:
			std::shared_ptr<const BrowserAssetBundle> m_bundle;
			IMPLEMENT_REFCOUNTING(CemodAssetFactory);
		};

		class RuntimeImpl;

		class Client final : public CefClient,
							 public CefLifeSpanHandler,
							 public CefDisplayHandler,
							 public CefRenderHandler,
							 public CefLoadHandler,
							 public CefRequestHandler,
							 public CefResourceRequestHandler,
							 public CefDialogHandler,
							 public CefDownloadHandler,
							 public CefPermissionHandler,
							 public CefMessageRouterBrowserSide::Handler
		{
		  public:
			Client(RuntimeImpl& owner, BrowserDescriptor descriptor);
			CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override
			{
				return this;
			}
			CefRefPtr<CefRenderHandler> GetRenderHandler() override
			{
				return m_descriptor.presentation == BrowserPresentation::OverlayOsr ? this : nullptr;
			}
			CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
			bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t,
				const CefString& message, const CefString&, int) override
			{
				const auto text = message.ToString();
				if (!text.starts_with("__CEMU_POINTER_PROBE__"))
					return false;
				cemuLog_log(LogType::Force, "CEX2-POINTER dom window={} {}",
					WindowId(), text.substr(22, 1024));
				return true;
			}
			CefRefPtr<CefLoadHandler> GetLoadHandler() override
			{
				return this;
			}
			CefRefPtr<CefRequestHandler> GetRequestHandler() override
			{
				return this;
			}
			CefRefPtr<CefDialogHandler> GetDialogHandler() override
			{
				return m_descriptor.cemodAssets ? this : nullptr;
			}
			CefRefPtr<CefDownloadHandler> GetDownloadHandler() override
			{
				return m_descriptor.cemodAssets ? this : nullptr;
			}
			CefRefPtr<CefPermissionHandler> GetPermissionHandler() override
			{
				return m_descriptor.cemodAssets ? this : nullptr;
			}

			void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override;
			bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& screenInfo) override;
			void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
						 const RectList& dirtyRects, const void* buffer, int width, int height) override;
			void OnPopupShow(CefRefPtr<CefBrowser>, bool show) override;
			void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) override;
			void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
			bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int,
							   const CefString&, const CefString&, cef_window_open_disposition_t, bool,
							   const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&,
							   CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) override;
			void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
			void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
							 ErrorCode errorCode, const CefString& errorText, const CefString&) override;
			void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override;
			bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
								CefRefPtr<CefRequest> request, bool userGesture, bool isRedirect) override;
			bool OnOpenURLFromTab(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
								  const CefString&, cef_window_open_disposition_t, bool) override;
			CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
				CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>, bool,
				bool isDownload, const CefString& requestInitiator,
				bool& disableDefaultHandling) override;
			ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
											 CefRefPtr<CefRequest>, CefRefPtr<CefCallback>) override;
			void OnResourceRedirect(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
									CefRefPtr<CefRequest>, CefRefPtr<CefResponse>, CefString& newUrl) override;
			void OnProtocolExecution(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
									 CefRefPtr<CefRequest>, bool& allowOsExecution) override;
			bool GetAuthCredentials(CefRefPtr<CefBrowser>, const CefString&, bool,
									const CefString&, int, const CefString&, const CefString&,
									CefRefPtr<CefAuthCallback>) override;
			bool OnSelectClientCertificate(CefRefPtr<CefBrowser>, bool, const CefString&, int,
										   const X509CertificateList&, CefRefPtr<CefSelectClientCertificateCallback>) override;
			bool OnFileDialog(CefRefPtr<CefBrowser>, FileDialogMode, const CefString&,
							  const CefString&, const std::vector<CefString>&, const std::vector<CefString>&,
							  const std::vector<CefString>&, CefRefPtr<CefFileDialogCallback>) override;
			bool CanDownload(CefRefPtr<CefBrowser>, const CefString&, const CefString&) override;
			void OnDownloadUpdated(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem>,
								   CefRefPtr<CefDownloadItemCallback>) override;
			bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
												const CefString&, std::uint32_t, CefRefPtr<CefMediaAccessCallback>) override;
			bool OnShowPermissionPrompt(CefRefPtr<CefBrowser>, std::uint64_t, const CefString&,
										std::uint32_t, CefRefPtr<CefPermissionPromptCallback>) override;
			void OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus,
										   int, const CefString&) override;
			bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
										  CefProcessId sourceProcess, CefRefPtr<CefProcessMessage> message) override;
			bool OnQuery(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, std::int64_t,
						 const CefString& request, bool, CefRefPtr<Callback> callback) override;
			void OnQueryCanceled(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, std::int64_t) override;

			void SetSize(int width, int height, double scale);
			std::pair<int, int> Size() const;
			double Scale() const;
			void RequestClose();
			void CompleteQuery(std::int64_t queryId, std::shared_ptr<std::atomic_bool> cancelled,
							   CefRefPtr<Callback> callback, bool success, std::string response);
			CefRefPtr<CefBrowser> Browser() const
			{
				return m_browser;
			}
			std::uint64_t WindowId() const
			{
				return m_descriptor.windowId;
			}
			const BrowserDescriptor& Descriptor() const
			{
				return m_descriptor;
			}

		  private:
			RuntimeImpl& m_owner;
			BrowserDescriptor m_descriptor;
			mutable std::mutex m_sizeMutex;
			int m_width{1};
			int m_height{1};
			double m_scale{1.0};
			CefRefPtr<CefBrowser> m_browser;
			CefRefPtr<CefMessageRouterBrowserSide> m_router;
			std::mutex m_queryMutex;
			std::unordered_map<std::int64_t, std::shared_ptr<std::atomic_bool>> m_queries;
			bool m_seenCemodMainNavigation{};
			bool m_reportedRendererTermination{};
			bool m_closeRequested{};
			IMPLEMENT_REFCOUNTING(Client);
		};

		class RuntimeImpl final : public BrowserRuntime,
								  public std::enable_shared_from_this<RuntimeImpl>
		{
		  public:
			RuntimeImpl(RpcHandler rpc, std::function<void(Host::PointerSurface)> redraw,
						ClosedHandler closed, WindowClosedHandler windowClosed,
						InputOwnershipHandler inputOwnershipChanged)
				: m_rpc(std::move(rpc)), m_mailbox(std::move(redraw)), m_closed(std::move(closed)),
				  m_windowClosed(std::move(windowClosed)),
				  m_inputOwnershipChanged(std::move(inputOwnershipChanged)) {}

			~RuntimeImpl() override
			{
				CloseAll();
			}
			bool Create(Host::PointerSurface surface, std::uint64_t windowId,
						int physicalWidth, int physicalHeight, double dpiScale) override;
			void Close(Host::PointerSurface surface) override;
			void CloseAll() override;
			void Resize(Host::PointerSurface surface, int physicalWidth,
						int physicalHeight, double dpiScale) override;
			void SetInteractive(Host::PointerSurface surface, bool interactive) override;
			bool CapturesInput(Host::PointerSurface surface) const override;
			OverlayInputTarget ResolveInput(const NativeInputEvent& event) override;
			bool SendInput(const NativeInputEvent& event, std::uint64_t windowId) override;
			void ExecuteEvent(Host::PointerSurface surface, std::string_view name,
							  std::string_view jsonPayload, std::uint64_t sequence) override;
			void ExecuteScript(Host::PointerSurface surface, std::string_view script) override;
			bool CreateBrowser(const BrowserDescriptor& descriptor) override;
			bool CloseWindow(std::uint64_t windowId) override;
			void ResizeWindow(std::uint64_t windowId, int width, int height, double scale) override;
			void SetWindowFocus(std::uint64_t windowId, bool focused) override;
			void SetOverlayVisible(std::uint64_t windowId, bool visible) override;
			void SetOverlayInteractive(std::uint64_t windowId, bool interactive) override;
			bool UpdateInputIntent(std::uint64_t windowId, InputIntent intent) override;
			void ResetInputIntent(std::uint64_t windowId, std::uint64_t generation) override;
			void SetInputSuspended(Host::PointerSurface surface, bool suspended) override;
			void ReleaseInput(Host::PointerSurface surface) override;
			void ExecuteWindowEvent(std::uint64_t windowId, std::string_view name,
									std::string_view payload, std::uint64_t sequence) override;
			void ExecuteCemodEvent(std::uint64_t windowId, std::string_view name,
								   std::string_view payload) override;
			void ExecuteWindowScript(std::uint64_t windowId, std::string_view script) override;
			bool HasWindow(std::uint64_t windowId) const override;
			std::optional<Host::OverlayFrameSnapshot> AcquireLatestOverlayFrame(
				Host::PointerSurface surface, std::uint64_t afterSequence) override
			{
				return m_mailbox.AcquireLatestOverlayFrame(surface, afterSequence);
			}

			std::string Dispatch(std::uint64_t windowId, std::string_view request)
			{
				try
				{
					return m_rpc(windowId, request);
				} catch (const std::exception&)
				{
					return R"({"id":"","ok":false,"error":{"code":"cef_bridge_failure","message":"native RPC exception"}})";
				} catch (...)
				{
					return R"({"id":"","ok":false,"error":{"code":"cef_bridge_failure","message":"unknown native exception"}})";
				}
			}
			void Created(std::uint64_t windowId, CefRefPtr<CefBrowser> browser);
			void Closed(std::uint64_t windowId);
			void LaunchBrowser(std::uint64_t windowId);
			CefRefPtr<CefRequestContext> RequestContext(
				const std::shared_ptr<const BrowserAssetBundle>& bundle);
			void PaintView(std::uint64_t windowId, int width, int height, const void* bgra,
						   int sourceStride, std::span<const Host::OverlayDirtyRect> dirtyRects);
			void PaintPopup(std::uint64_t windowId, int width, int height, const void* bgra,
							int sourceStride, std::span<const Host::OverlayDirtyRect> dirtyRects);
			void SetPopupVisible(std::uint64_t windowId, bool visible);
			void SetPopupRect(std::uint64_t windowId, Host::OverlayDirtyRect rect);

		  private:
			struct LayerBitmap
			{
				std::uint64_t windowId{};
				Host::PointerSurface surface{Host::PointerSurface::Main};
				bool visible{true};
				bool interactive{};
				bool hasInputIntent{};
				InputIntent inputIntent{};
				std::optional<CemodOverlayOrder> cemodOrder;
				int width{};
				int height{};
				std::vector<std::uint8_t> view;
				bool popupVisible{};
				Host::OverlayDirtyRect popupRect{};
				int popupWidth{};
				int popupHeight{};
				std::vector<std::uint8_t> popup;
			};

			struct SurfaceLayers
			{
				int width{};
				int height{};
				double scale{1.0};
				std::optional<std::uint64_t> builtin;
				std::optional<std::uint64_t> cemod;
				double pointerX{};
				double pointerY{};
				std::vector<std::uint8_t> composite;
			};

			struct ResolvedSurfaceInput
			{
				InputOwnership ownership{};
				std::uint64_t keyboardWindow{};
				std::uint64_t pointerWindow{};
				std::uint64_t textWindow{};
			};

			static std::size_t Index(Host::PointerSurface surface)
			{
				return surface == Host::PointerSurface::Main ? 0 : 1;
			}
			CefRefPtr<Client> Get(Host::PointerSurface surface) const;
			CefRefPtr<Client> Get(std::uint64_t windowId) const;
			LayerBitmap* FindLayerLocked(std::uint64_t windowId);
			const LayerBitmap* FindLayerLocked(std::uint64_t windowId) const;
			std::array<std::optional<std::uint64_t>, 2> BottomToTopLocked(
				const SurfaceLayers& layers) const;
			std::vector<Host::OverlayDirtyRect> NormalizeDamageLocked(
				Host::PointerSurface surface,
				std::span<const Host::OverlayDirtyRect> dirtyRects) const;
			void RecomposeLocked(Host::PointerSurface surface,
								 std::span<const Host::OverlayDirtyRect> dirtyRects);
			ResolvedSurfaceInput ResolveSurfaceInputLocked(Host::PointerSurface surface,
														   double x, double y) const;
			void NotifyInputOwnership(Host::PointerSurface surface,
									  const InputOwnership& ownership);
			void PublishComposite(Host::PointerSurface surface,
								  std::span<const Host::OverlayDirtyRect> dirtyRects = {});
			void UpdateLayerFocus(Host::PointerSurface surface);
			RpcHandler m_rpc;
			FrameMailbox m_mailbox;
			ClosedHandler m_closed;
			WindowClosedHandler m_windowClosed;
			InputOwnershipHandler m_inputOwnershipChanged;
			mutable std::mutex m_mutex;
			std::unordered_map<std::uint64_t, CefRefPtr<Client>> m_clients;
			std::unordered_map<std::string, CefRefPtr<CefRequestContext>> m_requestContexts;
			std::unordered_map<std::string, std::string> m_networkPolicyKeys;
			std::unordered_map<std::string, std::shared_ptr<CemodNetworkProxy>> m_networkProxies;
			std::unordered_map<std::uint64_t, LayerBitmap> m_overlayLayers;
			std::array<SurfaceLayers, 2> m_surfaces;
			std::array<bool, 2> m_inputSuspended{};
			std::mutex m_inputOwnershipMutex;
			std::array<InputOwnership, 2> m_notifiedOwnership{};
			std::condition_variable m_closeCondition;
		};

		Client::Client(RuntimeImpl& owner, BrowserDescriptor descriptor)
			: m_owner(owner), m_descriptor(std::move(descriptor)),
			  m_router(CefMessageRouterBrowserSide::Create(RouterConfig()))
		{
			m_router->AddHandler(this, false);
		}

		void Client::GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect)
		{
			const auto [width, height] = Size();
			rect = CefRect(0, 0, width, height);
		}

		bool Client::GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info)
		{
			const auto [width, height] = Size();
			info.device_scale_factor = static_cast<float>(Scale());
			info.rect = info.available_rect = CefRect(0, 0, width, height);
			return true;
		}

		void Client::OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
							 const RectList& dirty, const void* buffer, int width, int height)
		{
			if (!m_descriptor.overlaySurface)
				return;
			std::vector<Host::OverlayDirtyRect> rects;
			rects.reserve(dirty.size());
			for (const auto& rect : dirty)
				rects.push_back({rect.x, rect.y, rect.width, rect.height});
			if (type == PET_VIEW)
				m_owner.PaintView(m_descriptor.windowId, width, height, buffer, width * 4, rects);
			else if (type == PET_POPUP)
				m_owner.PaintPopup(m_descriptor.windowId, width, height, buffer, width * 4, rects);
		}

		void Client::OnPopupShow(CefRefPtr<CefBrowser> browser, bool show)
		{
			if (!m_descriptor.overlaySurface)
				return;
			m_owner.SetPopupVisible(m_descriptor.windowId, show);
			if (!show)
				browser->GetHost()->Invalidate(PET_VIEW);
		}

		void Client::OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect)
		{
			if (!m_descriptor.overlaySurface)
				return;
			const double scale = Scale();
			m_owner.SetPopupRect(m_descriptor.windowId, {static_cast<int>(std::lround(rect.x * scale)),
														 static_cast<int>(std::lround(rect.y * scale)),
														 static_cast<int>(std::lround(rect.width * scale)),
														 static_cast<int>(std::lround(rect.height * scale))});
		}

		void Client::OnAfterCreated(CefRefPtr<CefBrowser> browser)
		{
			m_browser = browser;
			m_owner.Created(m_descriptor.windowId, browser);
			if (m_descriptor.presentation == BrowserPresentation::NativeChild &&
				m_descriptor.nativeBrowserCreated)
			{
				try
				{
					m_descriptor.nativeBrowserCreated(
						NativeHandlePointer(browser->GetHost()->GetWindowHandle()));
				} catch (...)
				{
					cemuLog_log(LogType::Force, "CEF native browser attach callback failed for window {}",
								m_descriptor.windowId);
				}
			}
			if (m_closeRequested)
			{
				browser->GetHost()->CloseBrowser(true);
				return;
			}
		}

		bool Client::OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int,
								   const CefString&, const CefString&, cef_window_open_disposition_t, bool,
								   const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&,
								   CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*)
		{
			return m_descriptor.cemodAssets != nullptr;
		}

		void Client::OnBeforeClose(CefRefPtr<CefBrowser> browser)
		{
			std::vector<std::int64_t> cancelledQueries;
			{
				std::scoped_lock lock(m_queryMutex);
				cancelledQueries.reserve(m_queries.size());
				for (const auto& [queryId, cancelled] : m_queries)
				{
					cancelled->store(true, std::memory_order_release);
					cancelledQueries.push_back(queryId);
				}
				m_queries.clear();
			}
			if (m_descriptor.cemodQueryCancelled)
			{
				for (const auto queryId : cancelledQueries)
				{
					try
					{
						m_descriptor.cemodQueryCancelled(queryId);
					} catch (...)
					{
						cemuLog_log(LogType::Force,
									"Cemod Web UI cancel callback failed for window {}",
									m_descriptor.windowId);
					}
				}
			}
			if (m_descriptor.presentation == BrowserPresentation::NativeChild &&
				m_descriptor.nativeBrowserClosing)
			{
				try
				{
					m_descriptor.nativeBrowserClosing(
						NativeHandlePointer(browser->GetHost()->GetWindowHandle()));
				} catch (...)
				{
					cemuLog_log(LogType::Force, "CEF native browser detach callback failed for window {}",
								m_descriptor.windowId);
				}
			}
			m_router->OnBeforeClose(browser);
			m_router->RemoveHandler(this);
			m_browser = nullptr;
			if (m_descriptor.closed)
			{
				try
				{
					m_descriptor.closed();
				} catch (...)
				{
					cemuLog_log(LogType::Force,
								"CEF browser close callback failed for window {}", m_descriptor.windowId);
				}
			}
			m_owner.Closed(m_descriptor.windowId);
		}

		void Client::OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
								 ErrorCode code, const CefString& text, const CefString&)
		{
			if (code != ERR_ABORTED)
				cemuLog_log(LogType::Force, "CEF overlay load failed: {} ({})", text.ToString(), static_cast<int>(code));
		}

		void Client::OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int)
		{
			if (frame->IsMain() && m_descriptor.loadReady)
			{
				try
				{
					m_descriptor.loadReady();
				} catch (...)
				{
					cemuLog_log(LogType::Force,
								"Cemod Web UI load callback failed for window {}", m_descriptor.windowId);
				}
			}
		}

		bool Client::OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
									CefRefPtr<CefRequest> request, bool, bool isRedirect)
		{
			if (m_descriptor.cemodAssets && !frame->IsMain())
				return true;
			const std::string url = request->GetURL();
			if (m_descriptor.cemodAssets)
			{
				CefURLParts parts;
				if (!CefParseURL(url, parts) || CefString(&parts.scheme) != kCemodScheme ||
					CefString(&parts.host) != m_descriptor.cemodAssets->originId)
					return true;
			}
			if (!IsAllowedUrl(url, m_descriptor.initialUrl))
				return true;
			const bool mainCemodNavigation = m_descriptor.cemodAssets && frame->IsMain();
			const bool reloaded = mainCemodNavigation && !isRedirect &&
								  std::exchange(m_seenCemodMainNavigation, true);
			m_router->OnBeforeBrowse(browser, frame);
			if (reloaded && m_descriptor.cemodMainFrameReloaded)
			{
				try
				{
					m_descriptor.cemodMainFrameReloaded();
				} catch (...)
				{
					cemuLog_log(LogType::Force,
								"Cemod Web UI reload callback failed for window {}", m_descriptor.windowId);
				}
			}
			return false;
		}

		bool Client::OnOpenURLFromTab(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
									  const CefString&, cef_window_open_disposition_t, bool)
		{
			return m_descriptor.cemodAssets != nullptr;
		}

		CefRefPtr<CefResourceRequestHandler> Client::GetResourceRequestHandler(
			CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request,
			bool, bool isDownload, const CefString& requestInitiator,
			bool& disableDefaultHandling)
		{
			CEF_REQUIRE_IO_THREAD();
			if (!m_descriptor.cemodAssets)
				return nullptr;
			const auto& bundle = *m_descriptor.cemodAssets;
			if (isDownload)
			{
				disableDefaultHandling = true;
				return nullptr;
			}
			if (!IsCemodAssetUrl(request->GetURL().ToString(), bundle) &&
				!IsCemodInitiator(requestInitiator.ToString(), bundle))
			{
				disableDefaultHandling = true;
				return nullptr;
			}
			return this;
		}

		CefResourceRequestHandler::ReturnValue Client::OnBeforeResourceLoad(
			CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request,
			CefRefPtr<CefCallback>)
		{
			CEF_REQUIRE_IO_THREAD();
			if (!m_descriptor.cemodAssets)
				return RV_CONTINUE;
			const auto& bundle = *m_descriptor.cemodAssets;
			const auto url = request->GetURL().ToString();
			if (!IsAllowedCemodNetworkRequest(url, request->GetResourceType(), bundle))
				return RV_CANCEL;
			return RV_CONTINUE;
		}

		void Client::OnResourceRedirect(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
										CefRefPtr<CefRequest> request, CefRefPtr<CefResponse>, CefString& newUrl)
		{
			CEF_REQUIRE_IO_THREAD();
			if (m_descriptor.cemodAssets &&
				!IsAllowedCemodNetworkRequest(newUrl.ToString(), request->GetResourceType(),
											  *m_descriptor.cemodAssets))
				newUrl = "cemod-blocked://invalid/";
		}

		void Client::OnProtocolExecution(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
										 CefRefPtr<CefRequest>, bool& allowOsExecution)
		{
			if (m_descriptor.cemodAssets)
				allowOsExecution = false;
		}

		bool Client::GetAuthCredentials(CefRefPtr<CefBrowser>, const CefString&, bool,
										const CefString&, int, const CefString&, const CefString&, CefRefPtr<CefAuthCallback>)
		{
			return false;
		}

		bool Client::OnSelectClientCertificate(CefRefPtr<CefBrowser>, bool, const CefString&, int,
											   const X509CertificateList&, CefRefPtr<CefSelectClientCertificateCallback> callback)
		{
			if (!m_descriptor.cemodAssets)
				return false;
			if (callback)
				callback->Select(nullptr);
			return true;
		}

		bool Client::OnFileDialog(CefRefPtr<CefBrowser>, FileDialogMode, const CefString&,
								  const CefString&, const std::vector<CefString>&, const std::vector<CefString>&,
								  const std::vector<CefString>&, CefRefPtr<CefFileDialogCallback> callback)
		{
			if (callback)
				callback->Cancel();
			return true;
		}

		bool Client::CanDownload(CefRefPtr<CefBrowser>, const CefString&, const CefString&)
		{
			return false;
		}

		void Client::OnDownloadUpdated(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem>,
									   CefRefPtr<CefDownloadItemCallback> callback)
		{
			if (callback)
				callback->Cancel();
		}

		bool Client::OnRequestMediaAccessPermission(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
													const CefString&, std::uint32_t, CefRefPtr<CefMediaAccessCallback> callback)
		{
			if (callback)
				callback->Cancel();
			return true;
		}

		bool Client::OnShowPermissionPrompt(CefRefPtr<CefBrowser>, std::uint64_t,
											const CefString&, std::uint32_t, CefRefPtr<CefPermissionPromptCallback> callback)
		{
			if (callback)
				callback->Continue(CEF_PERMISSION_RESULT_DENY);
			return true;
		}

		void Client::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
											   TerminationStatus, int, const CefString&)
		{
			m_router->OnRenderProcessTerminated(browser);
			if (!m_descriptor.cemodAssets ||
				std::exchange(m_reportedRendererTermination, true) ||
				!m_descriptor.cemodRendererTerminated)
				return;
			try
			{
				m_descriptor.cemodRendererTerminated();
			} catch (...)
			{
				cemuLog_log(LogType::Force,
							"Cemod Web UI renderer callback failed for window {}", m_descriptor.windowId);
			}
		}

		bool Client::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
											  CefRefPtr<CefFrame> frame, CefProcessId source, CefRefPtr<CefProcessMessage> message)
		{
			return m_router->OnProcessMessageReceived(browser, frame, source, message);
		}

		bool Client::OnQuery(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, std::int64_t queryId,
							 const CefString& request, bool, CefRefPtr<Callback> callback)
		{
			if (m_descriptor.cemodQuery)
			{
				if (!frame->IsMain())
				{
					callback->Failure(-1, "Cemod bridge is only available to the main frame");
					return true;
				}
				auto cancelled = std::make_shared<std::atomic_bool>(false);
				{
					std::scoped_lock lock(m_queryMutex);
					m_queries.insert_or_assign(queryId, cancelled);
				}
				CefRefPtr<Client> self(this);
				try
				{
					m_descriptor.cemodQuery(queryId, request.ToString(),
											[self, queryId, cancelled, callback](bool success, std::string response) mutable {
												CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(
																		&Client::CompleteQuery, self, queryId, std::move(cancelled),
																		std::move(callback), success, std::move(response))));
											});
				} catch (...)
				{
					CompleteQuery(queryId, std::move(cancelled), std::move(callback), false,
								  "Cemod bridge dispatch failed");
				}
				return true;
			}
			callback->Success(m_owner.Dispatch(m_descriptor.windowId, request.ToString()));
			return true;
		}

		void Client::OnQueryCanceled(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, std::int64_t queryId)
		{
			std::shared_ptr<std::atomic_bool> cancelled;
			{
				std::scoped_lock lock(m_queryMutex);
				const auto query = m_queries.find(queryId);
				if (query == m_queries.end())
					return;
				cancelled = query->second;
				m_queries.erase(query);
			}
			cancelled->store(true, std::memory_order_release);
			if (m_descriptor.cemodQueryCancelled)
			{
				try
				{
					m_descriptor.cemodQueryCancelled(queryId);
				} catch (...)
				{
					cemuLog_log(LogType::Force,
								"Cemod Web UI cancel callback failed for window {}", m_descriptor.windowId);
				}
			}
		}

		void Client::CompleteQuery(std::int64_t queryId, std::shared_ptr<std::atomic_bool> cancelled,
								   CefRefPtr<Callback> callback, bool success, std::string response)
		{
			CEF_REQUIRE_UI_THREAD();
			{
				std::scoped_lock lock(m_queryMutex);
				const auto query = m_queries.find(queryId);
				if (query == m_queries.end() || query->second != cancelled)
					return;
				m_queries.erase(query);
			}
			if (cancelled->exchange(true, std::memory_order_acq_rel))
				return;
			if (success)
				callback->Success(response);
			else
				callback->Failure(-1, response);
		}

		void Client::SetSize(int width, int height, double scale)
		{
			std::scoped_lock lock(m_sizeMutex);
			m_scale = std::clamp(scale, 0.5, 8.0);
			// CEF's view rectangle and input coordinates are device-independent;
			// OnPaint and popup pixel buffers are scaled to physical pixels.
			m_width = std::max(static_cast<int>(std::lround(width / m_scale)), 1);
			m_height = std::max(static_cast<int>(std::lround(height / m_scale)), 1);
		}

		std::pair<int, int> Client::Size() const
		{
			std::scoped_lock lock(m_sizeMutex);
			return {m_width, m_height};
		}

		double Client::Scale() const
		{
			std::scoped_lock lock(m_sizeMutex);
			return m_scale;
		}

		void Client::RequestClose()
		{
			if (m_closeRequested)
				return;
			m_closeRequested = true;
			if (m_browser)
				m_browser->GetHost()->CloseBrowser(true);
		}

		RuntimeImpl::LayerBitmap* RuntimeImpl::FindLayerLocked(std::uint64_t windowId)
		{
			const auto found = m_overlayLayers.find(windowId);
			return found == m_overlayLayers.end() ? nullptr : &found->second;
		}

		const RuntimeImpl::LayerBitmap* RuntimeImpl::FindLayerLocked(std::uint64_t windowId) const
		{
			const auto found = m_overlayLayers.find(windowId);
			return found == m_overlayLayers.end() ? nullptr : &found->second;
		}

		std::array<std::optional<std::uint64_t>, 2> RuntimeImpl::BottomToTopLocked(
			const SurfaceLayers& layers) const
		{
			CemodOverlayOrder order = CemodOverlayOrder::AboveBuiltin;
			if (layers.cemod)
				if (const auto* layer = FindLayerLocked(*layers.cemod);
					layer && layer->cemodOrder == CemodOverlayOrder::BelowBuiltin)
					order = CemodOverlayOrder::BelowBuiltin;
			// The passive resume prompt must remain visible above a mod's Dock.
			if ((&layers == &m_surfaces[0] && m_inputSuspended[0]) ||
				(&layers == &m_surfaces[1] && m_inputSuspended[1]))
				order = CemodOverlayOrder::BelowBuiltin;
			std::array<std::optional<std::uint64_t>, 2> result;
			const auto layerOrder = OverlayLayersBottomToTop(order);
			for (std::size_t index = 0; index < layerOrder.size(); ++index)
				result[index] = layerOrder[index] == OverlayLayer::Builtin ? layers.builtin : layers.cemod;
			return result;
		}

		std::vector<Host::OverlayDirtyRect> RuntimeImpl::NormalizeDamageLocked(
			Host::PointerSurface surface,
			std::span<const Host::OverlayDirtyRect> dirtyRects) const
		{
			const auto& layers = m_surfaces[Index(surface)];
			if (layers.width <= 0 || layers.height <= 0 || layers.width > 16384 ||
				layers.height > 16384)
				return {};
			const Host::OverlayDirtyRect full{0, 0, layers.width, layers.height};
			if (dirtyRects.empty())
				return {full};
			std::vector<Host::OverlayDirtyRect> result;
			result.reserve(dirtyRects.size());
			for (auto rect : dirtyRects)
			{
				if (rect.width <= 0 || rect.height <= 0)
					continue;
				const auto right = std::min<std::int64_t>(
					layers.width, static_cast<std::int64_t>(rect.x) + rect.width);
				const auto bottom = std::min<std::int64_t>(
					layers.height, static_cast<std::int64_t>(rect.y) + rect.height);
				rect.x = std::max(rect.x, 0);
				rect.y = std::max(rect.y, 0);
				rect.width = static_cast<int>(right - rect.x);
				rect.height = static_cast<int>(bottom - rect.y);
				if (rect.width > 0 && rect.height > 0)
					result.push_back(rect);
			}
			return result;
		}

		void RuntimeImpl::RecomposeLocked(
			Host::PointerSurface surface,
			std::span<const Host::OverlayDirtyRect> dirtyRects)
		{
			auto& layers = m_surfaces[Index(surface)];
			const auto layerOrder = BottomToTopLocked(layers);
			auto blendPixel = [](std::uint8_t* destination, const std::uint8_t* source) {
				if (source[3] == 0)
					return;
				if (source[3] == 255)
				{
					std::memcpy(destination, source, 4U);
					return;
				}
				const unsigned inverseAlpha = 255U - source[3];
				for (unsigned channel = 0; channel != 4; ++channel)
					destination[channel] = static_cast<std::uint8_t>(std::min(
						255U, static_cast<unsigned>(source[channel]) +
								  (static_cast<unsigned>(destination[channel]) * inverseAlpha + 127U) / 255U));
			};
			std::array<const LayerBitmap*, 2> activeLayers{};
			std::size_t activeLayerCount{};
			for (const auto id : layerOrder)
			{
				if (!id)
					continue;
				const auto* layer = FindLayerLocked(*id);
				if (layer && layer->visible && layer->width == layers.width &&
					layer->height == layers.height &&
					layer->view.size() == layers.composite.size())
					activeLayers[activeLayerCount++] = layer;
			}
			const bool hasPopup = std::any_of(
				activeLayers.begin(), activeLayers.begin() + activeLayerCount,
				[](const LayerBitmap* layer) {
					return layer->popupVisible && !layer->popup.empty() &&
						   layer->popupWidth > 0 && layer->popupHeight > 0;
				});
			for (const auto& rect : dirtyRects)
			{
				if (activeLayerCount == 1 && !hasPopup)
				{
					for (int row = 0; row < rect.height; ++row)
					{
						const auto offset =
							(static_cast<std::size_t>(rect.y + row) * layers.width + rect.x) * 4U;
						std::memcpy(layers.composite.data() + offset,
									activeLayers[0]->view.data() + offset,
									static_cast<std::size_t>(rect.width) * 4U);
					}
					continue;
				}
				for (int row = 0; row < rect.height; ++row)
				{
					const auto offset =
						(static_cast<std::size_t>(rect.y + row) * layers.width + rect.x) * 4U;
					std::memset(layers.composite.data() + offset, 0,
								static_cast<std::size_t>(rect.width) * 4U);
				}
				for (std::size_t layerIndex = 0; layerIndex < activeLayerCount; ++layerIndex)
				{
					const auto& layer = *activeLayers[layerIndex];
					for (int y = rect.y; y < rect.y + rect.height; ++y)
						for (int x = rect.x; x < rect.x + rect.width; ++x)
						{
							const auto offset =
								(static_cast<std::size_t>(y) * layers.width + x) * 4U;
							blendPixel(layers.composite.data() + offset,
									   layer.view.data() + offset);
						}
					if (!layer.popupVisible || layer.popup.empty() ||
						layer.popupWidth <= 0 || layer.popupHeight <= 0)
						continue;
					const int left = std::max(rect.x, layer.popupRect.x);
					const int top = std::max(rect.y, layer.popupRect.y);
					const int right = std::min(rect.x + rect.width,
											   layer.popupRect.x + layer.popupWidth);
					const int bottom = std::min(rect.y + rect.height,
												layer.popupRect.y + layer.popupHeight);
					for (int y = top; y < bottom; ++y)
						for (int x = left; x < right; ++x)
						{
							const auto destinationOffset =
								(static_cast<std::size_t>(y) * layers.width + x) * 4U;
							const auto popupOffset =
								(static_cast<std::size_t>(y - layer.popupRect.y) * layer.popupWidth +
								 (x - layer.popupRect.x)) *
								4U;
							if (popupOffset + 4U <= layer.popup.size())
								blendPixel(layers.composite.data() + destinationOffset,
										   layer.popup.data() + popupOffset);
						}
				}
			}
		}

		void RuntimeImpl::PublishComposite(
			Host::PointerSurface surface,
			std::span<const Host::OverlayDirtyRect> dirtyRects)
		{
			std::scoped_lock lock(m_mutex);
			auto& layers = m_surfaces[Index(surface)];
			auto damage = NormalizeDamageLocked(surface, dirtyRects);
			if (damage.empty())
				return;
			const auto size = static_cast<std::size_t>(layers.width) * layers.height * 4U;
			if (layers.composite.size() != size)
			{
				layers.composite.assign(size, 0);
				damage = NormalizeDamageLocked(surface, {});
			}
			RecomposeLocked(surface, damage);
			m_mailbox.Reopen(surface);
			m_mailbox.PublishView(surface, layers.width, layers.height,
								  layers.composite.data(), layers.width * 4, damage);
		}

		void RuntimeImpl::PaintView(std::uint64_t windowId, int width, int height,
									const void* bgra, int sourceStride,
									std::span<const Host::OverlayDirtyRect> dirtyRects)
		{
			if (!bgra || width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
				sourceStride < width * 4)
				return;
			Host::PointerSurface surface;
			std::vector<Host::OverlayDirtyRect> damage;
			{
				std::scoped_lock lock(m_mutex);
				auto* layer = FindLayerLocked(windowId);
				if (!layer)
					return;
				surface = layer->surface;
				const auto& target = m_surfaces[Index(surface)];
				if (width != target.width || height != target.height)
					return;
				const auto size = static_cast<std::size_t>(width) * height * 4U;
				const bool resized = layer->width != width || layer->height != height ||
									 layer->view.size() != size;
				layer->width = width;
				layer->height = height;
				if (resized)
					layer->view.assign(size, 0);
				damage = NormalizeDamageLocked(surface, resized
															? std::span<const Host::OverlayDirtyRect>{}
															: dirtyRects);
				for (const auto& rect : damage)
					for (int row = 0; row < rect.height; ++row)
					{
						const auto offset =
							(static_cast<std::size_t>(rect.y + row) * width + rect.x) * 4U;
						std::memcpy(layer->view.data() + offset,
									static_cast<const std::uint8_t*>(bgra) +
										static_cast<std::size_t>(rect.y + row) * sourceStride +
										static_cast<std::size_t>(rect.x) * 4U,
									static_cast<std::size_t>(rect.width) * 4U);
					}
			}
			if (!damage.empty())
				PublishComposite(surface, damage);
		}

		void RuntimeImpl::PaintPopup(std::uint64_t windowId, int width, int height,
									 const void* bgra, int sourceStride,
									 std::span<const Host::OverlayDirtyRect> dirtyRects)
		{
			if (!bgra || width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
				sourceStride < width * 4)
				return;
			Host::PointerSurface surface;
			std::vector<Host::OverlayDirtyRect> damage;
			{
				std::scoped_lock lock(m_mutex);
				auto* layer = FindLayerLocked(windowId);
				if (!layer || !layer->popupVisible)
					return;
				surface = layer->surface;
				const bool resized = layer->popupWidth != width || layer->popupHeight != height ||
									 layer->popup.size() != static_cast<std::size_t>(width) * height * 4U;
				layer->popupWidth = width;
				layer->popupHeight = height;
				if (resized)
					layer->popup.assign(static_cast<std::size_t>(width) * height * 4U, 0);
				const Host::OverlayDirtyRect full{0, 0, width, height};
				const auto popupDamage = resized || dirtyRects.empty()
											 ? std::span<const Host::OverlayDirtyRect>{&full, 1U}
											 : dirtyRects;
				for (auto rect : popupDamage)
				{
					const auto right = std::min<std::int64_t>(
						width, static_cast<std::int64_t>(rect.x) + rect.width);
					const auto bottom = std::min<std::int64_t>(
						height, static_cast<std::int64_t>(rect.y) + rect.height);
					rect.x = std::max(rect.x, 0);
					rect.y = std::max(rect.y, 0);
					rect.width = static_cast<int>(right - rect.x);
					rect.height = static_cast<int>(bottom - rect.y);
					if (rect.width <= 0 || rect.height <= 0)
						continue;
					for (int row = 0; row < rect.height; ++row)
					{
						const auto popupOffset =
							(static_cast<std::size_t>(rect.y + row) * width + rect.x) * 4U;
						std::memcpy(layer->popup.data() + popupOffset,
									static_cast<const std::uint8_t*>(bgra) +
										static_cast<std::size_t>(rect.y + row) * sourceStride +
										static_cast<std::size_t>(rect.x) * 4U,
									static_cast<std::size_t>(rect.width) * 4U);
					}
					damage.push_back({layer->popupRect.x + rect.x,
									  layer->popupRect.y + rect.y, rect.width, rect.height});
				}
			}
			if (!damage.empty())
				PublishComposite(surface, damage);
		}

		void RuntimeImpl::SetPopupVisible(std::uint64_t windowId, bool visible)
		{
			Host::PointerSurface surface;
			{
				std::scoped_lock lock(m_mutex);
				auto* layer = FindLayerLocked(windowId);
				if (!layer)
					return;
				surface = layer->surface;
				layer->popupVisible = visible;
				if (!visible)
				{
					layer->popup.clear();
					layer->popupWidth = layer->popupHeight = 0;
				}
			}
			PublishComposite(surface);
		}

		void RuntimeImpl::SetPopupRect(std::uint64_t windowId, Host::OverlayDirtyRect rect)
		{
			Host::PointerSurface surface;
			{
				std::scoped_lock lock(m_mutex);
				auto* layer = FindLayerLocked(windowId);
				if (!layer)
					return;
				surface = layer->surface;
				layer->popupRect = rect;
			}
			PublishComposite(surface);
		}

		CefRefPtr<Client> RuntimeImpl::Get(Host::PointerSurface surface) const
		{
			std::scoped_lock lock(m_mutex);
			const auto window = m_surfaces[Index(surface)].builtin;
			if (!window)
				return nullptr;
			const auto client = m_clients.find(*window);
			return client == m_clients.end() ? nullptr : client->second;
		}

		CefRefPtr<Client> RuntimeImpl::Get(std::uint64_t windowId) const
		{
			std::scoped_lock lock(m_mutex);
			const auto client = m_clients.find(windowId);
			return client == m_clients.end() ? nullptr : client->second;
		}

		bool RuntimeImpl::Create(Host::PointerSurface surface, std::uint64_t windowId,
								 int width, int height, double scale)
		{
			BrowserDescriptor descriptor;
			descriptor.windowId = windowId;
			descriptor.role = "runtime-overlay";
			descriptor.presentation = BrowserPresentation::OverlayOsr;
			descriptor.overlaySurface = surface;
			descriptor.overlayLayer = OverlayLayer::Builtin;
			descriptor.bounds.width = width;
			descriptor.bounds.height = height;
			descriptor.dpiScale = scale;
			return CreateBrowser(descriptor);
		}

		bool RuntimeImpl::CreateBrowser(const BrowserDescriptor& descriptor)
		{
			CEF_REQUIRE_UI_THREAD();
			if (descriptor.bounds.width <= 0 || descriptor.bounds.height <= 0 ||
				descriptor.dpiScale <= 0.0 || descriptor.initialUrl.empty())
				return false;
			if (descriptor.cemodAssets)
			{
				const auto& originId = descriptor.cemodAssets->originId;
				if (originId.empty() || originId.size() > 63 ||
					!std::all_of(originId.begin(), originId.end(), [](unsigned char value) {
						return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
							   value == '-';
					}))
					return false;
				CefURLParts parts;
				if (!CefParseURL(descriptor.initialUrl, parts) ||
					CefString(&parts.scheme) != kCemodScheme ||
					CefString(&parts.host) != originId)
					return false;
			}
			else if (!descriptor.initialUrl.starts_with("cemu://ui/") &&
					 !LoopbackOrigin(descriptor.initialUrl))
				return false;
			if (descriptor.presentation == BrowserPresentation::OverlayOsr &&
				(!descriptor.overlaySurface || !descriptor.overlayLayer ||
				 (*descriptor.overlayLayer == OverlayLayer::Cemod && !descriptor.cemodOverlayOrder) ||
				 (*descriptor.overlayLayer == OverlayLayer::Builtin && descriptor.cemodOverlayOrder)))
				return false;
			if (descriptor.presentation == BrowserPresentation::NativeChild &&
				!descriptor.nativeParent)
				return false;

			BrowserDescriptor normalized = descriptor;
			CefRefPtr<Client> client;
			{
				std::scoped_lock lock(m_mutex);
				if (m_clients.contains(descriptor.windowId))
					return true;
				if (descriptor.overlaySurface)
				{
					auto& surface = m_surfaces[Index(*descriptor.overlaySurface)];
					auto& reservation = *descriptor.overlayLayer == OverlayLayer::Builtin
											? surface.builtin
											: surface.cemod;
					if (reservation)
						return false;
					if (*descriptor.overlayLayer == OverlayLayer::Builtin)
					{
						surface.width = descriptor.bounds.width;
						surface.height = descriptor.bounds.height;
						surface.scale = descriptor.dpiScale;
					}
					else
					{
						if (!surface.builtin || surface.width <= 0 || surface.height <= 0)
							return false;
						normalized.bounds.width = surface.width;
						normalized.bounds.height = surface.height;
						normalized.dpiScale = surface.scale;
					}
					reservation = descriptor.windowId;
					LayerBitmap layer;
					layer.windowId = descriptor.windowId;
					layer.surface = *descriptor.overlaySurface;
					layer.visible = descriptor.overlayVisible;
					layer.interactive = descriptor.overlayInteractive;
					layer.cemodOrder = descriptor.cemodOverlayOrder;
					m_overlayLayers.emplace(descriptor.windowId, std::move(layer));
				}
				client = new Client(*this, normalized);
				client->SetSize(normalized.bounds.width, normalized.bounds.height, normalized.dpiScale);
				m_clients.emplace(descriptor.windowId, client);
			}
			if (descriptor.overlaySurface)
			{
				m_mailbox.Reopen(*descriptor.overlaySurface);
				PublishComposite(*descriptor.overlaySurface);
			}

			auto launch = [weak = weak_from_this(), windowId = descriptor.windowId] {
				if (const auto runtime = weak.lock())
					runtime->LaunchBrowser(windowId);
			};
			bool launchNow{};
			{
				std::scoped_lock lock(g_contextTasksMutex);
				launchNow = g_contextInitialized;
				if (!launchNow)
					g_contextTasks.emplace_back(launch);
			}
			if (launchNow)
				launch();
			return true;
		}

		CefRefPtr<CefRequestContext> RuntimeImpl::RequestContext(
			const std::shared_ptr<const BrowserAssetBundle>& bundle)
		{
			CEF_REQUIRE_UI_THREAD();
			if (!bundle)
				return nullptr;
			if (const auto context = m_requestContexts.find(bundle->originId);
				context != m_requestContexts.end())
			{
				const auto policy = m_networkPolicyKeys.find(bundle->originId);
				if (policy == m_networkPolicyKeys.end() || policy->second != bundle->networkPolicyKey)
				{
					cemuLog_log(LogType::Force,
								"Refusing concurrent Cemod Web UI network policy change for origin {}",
								bundle->originId);
					return nullptr;
				}
				if (!context->second->RegisterSchemeHandlerFactory(kCemodScheme, bundle->originId,
																   new CemodAssetFactory(bundle)))
					return nullptr;
				return context->second;
			}

			CefRequestContextSettings settings;
			if (bundle->persistentStorage)
			{
				const auto cachePath = CefCacheRoot() / "cemod" / bundle->originId;
				std::error_code error;
				std::filesystem::create_directories(cachePath, error);
				if (error)
				{
					cemuLog_log(LogType::Force, "Unable to create Cemod Web UI cache directory: {}",
								error.message());
					return nullptr;
				}
#if defined(OS_WIN)
				CefString(&settings.cache_path) = cachePath.wstring();
#else
				CefString(&settings.cache_path) = cachePath.string();
#endif
			}
			if (!bundle->credentials)
				settings.cookieable_schemes_exclude_defaults = true;
			auto context = CefRequestContext::CreateContext(settings, nullptr);
			if (!context || !context->RegisterSchemeHandlerFactory(kCemodScheme, bundle->originId,
																   new CemodAssetFactory(bundle)))
			{
				cemuLog_log(LogType::Force, "Unable to register Cemod Web UI origin {}", bundle->originId);
				return nullptr;
			}
			std::shared_ptr<CemodNetworkProxy> proxy;
			if (!bundle->connectOrigins.empty() || !bundle->resourceOrigins.empty())
			{
				proxy = CemodNetworkProxy::Create(bundle->connectOrigins,
												  bundle->resourceOrigins, bundle->allowPrivateNetwork);
				if (!proxy)
				{
					cemuLog_log(LogType::Force, "Unable to create Cemod Web UI network proxy");
					return nullptr;
				}
				auto proxyDictionary = CefDictionaryValue::Create();
				proxyDictionary->SetString("mode", "fixed_servers");
				proxyDictionary->SetString("server",
										   "http://127.0.0.1:" + std::to_string(proxy->Port()));
				proxyDictionary->SetString("bypass_list", "<-loopback>");
				auto proxyValue = CefValue::Create();
				proxyValue->SetDictionary(proxyDictionary);
				CefString error;
				if (!context->SetPreference("proxy", proxyValue, error))
				{
					cemuLog_log(LogType::Force,
								"Unable to configure Cemod Web UI network proxy: {}", error.ToString());
					return nullptr;
				}
			}
			m_requestContexts.emplace(bundle->originId, context);
			m_networkPolicyKeys.emplace(bundle->originId, bundle->networkPolicyKey);
			if (proxy)
				m_networkProxies.emplace(bundle->originId, std::move(proxy));
			return context;
		}

		void RuntimeImpl::LaunchBrowser(std::uint64_t windowId)
		{
			CEF_REQUIRE_UI_THREAD();
			const auto client = Get(windowId);
			if (!client || client->Browser())
				return;
			const auto& descriptor = client->Descriptor();

			CefWindowInfo windowInfo;
			CefWindowHandle parent = kNullWindowHandle;
#if defined(OS_LINUX)
			if (descriptor.nativeParent)
				parent = static_cast<CefWindowHandle>(reinterpret_cast<std::uintptr_t>(descriptor.nativeParent));
#else
			if (descriptor.nativeParent)
				parent = static_cast<CefWindowHandle>(descriptor.nativeParent);
#endif
			if (descriptor.presentation == BrowserPresentation::OverlayOsr)
			{
				windowInfo.SetAsWindowless(parent);
				windowInfo.shared_texture_enabled = false;
			}
			else
			{
				windowInfo.SetAsChild(parent, CefRect(descriptor.bounds.x, descriptor.bounds.y,
													  descriptor.bounds.width, descriptor.bounds.height));
			}
			CefBrowserSettings settings;
			settings.background_color = descriptor.presentation == BrowserPresentation::OverlayOsr
											? (descriptor.overlayTransparent ? CefColorSetARGB(0, 0, 0, 0)
																			 : CefColorSetARGB(255, 0, 0, 0))
											: CefColorSetARGB(255, 32, 32, 32);
			if (descriptor.presentation == BrowserPresentation::OverlayOsr)
			{
				// Blink must keep its own monotonic animation clock. The guest can
				// present at 30 Hz, pause while changing menus, or stop presenting
				// altogether; tying external begin frames to those presents stalls CSS
				// transitions and requestAnimationFrame. The renderer's latest-only
				// mailbox samples this 60 Hz timeline without queueing stale frames.
				int frameRate = 60;
				if (const char* value = std::getenv("CEMU_CEF_OVERLAY_FPS"))
					frameRate = std::clamp(std::atoi(value), 30, 60);
				settings.windowless_frame_rate = frameRate;
			}
			auto extraInfo = CefDictionaryValue::Create();
			extraInfo->SetString("bootstrap", MakeBootstrap(descriptor));
			if (descriptor.cemodAssets)
			{
				extraInfo->SetBool("cemodBridge", true);
				extraInfo->SetString("cemodOrigin", descriptor.cemodAssets->originId);
			}
			auto requestContext = descriptor.cemodAssets
									  ? RequestContext(descriptor.cemodAssets)
									  : nullptr;
			if (descriptor.cemodAssets && !requestContext)
			{
				Closed(descriptor.windowId);
				return;
			}
			if (!CefBrowserHost::CreateBrowser(windowInfo, client, descriptor.initialUrl,
											   settings, extraInfo, requestContext))
			{
				cemuLog_log(LogType::Force, "CEF browser creation failed for role {} window {}",
							descriptor.role, descriptor.windowId);
				Closed(descriptor.windowId);
			}
		}

		void App::OnContextInitialized()
		{
			CEF_REQUIRE_UI_THREAD();
			std::vector<std::function<void()>> tasks;
			{
				std::scoped_lock lock(g_contextTasksMutex);
				g_contextInitialized = true;
				tasks.swap(g_contextTasks);
			}
			for (auto& task : tasks)
				task();
		}

		void RuntimeImpl::Created(std::uint64_t windowId, CefRefPtr<CefBrowser>)
		{
			if (auto client = Get(windowId); client && client->Descriptor().overlaySurface)
				UpdateLayerFocus(*client->Descriptor().overlaySurface);
		}

		void RuntimeImpl::Closed(std::uint64_t windowId)
		{
			std::optional<Host::PointerSurface> surface;
			std::optional<OverlayLayer> overlayLayer;
			InputOwnership ownership;
			std::string cemodOrigin;
			bool releaseCemodOrigin{};
			{
				std::scoped_lock lock(m_mutex);
				const auto client = m_clients.find(windowId);
				if (client == m_clients.end())
					return;
				surface = client->second->Descriptor().overlaySurface;
				overlayLayer = client->second->Descriptor().overlayLayer;
				if (client->second->Descriptor().cemodAssets)
					cemodOrigin = client->second->Descriptor().cemodAssets->originId;
				m_clients.erase(client);
				if (surface)
				{
					auto& layers = m_surfaces[Index(*surface)];
					if (overlayLayer == OverlayLayer::Cemod)
						layers.cemod.reset();
					else
						layers.builtin.reset();
					m_overlayLayers.erase(windowId);
					const auto& state = m_surfaces[Index(*surface)];
					ownership = ResolveSurfaceInputLocked(*surface, state.pointerX,
														  state.pointerY)
									.ownership;
				}
				if (!cemodOrigin.empty())
					releaseCemodOrigin = std::ranges::none_of(m_clients, [&](const auto& item) {
						const auto& assets = item.second->Descriptor().cemodAssets;
						return assets && assets->originId == cemodOrigin;
					});
			}
			if (releaseCemodOrigin)
			{
				m_requestContexts.erase(cemodOrigin);
				m_networkPolicyKeys.erase(cemodOrigin);
				m_networkProxies.erase(cemodOrigin);
			}
			if (surface)
			{
				NotifyInputOwnership(*surface, ownership);
				PublishComposite(*surface);
			}
			m_closeCondition.notify_all();
			try
			{
				if (surface && m_closed)
					m_closed(*surface);
			} catch (...)
			{
				cemuLog_log(LogType::Force, "CEF overlay close callback failed for window {}", windowId);
			}
			try
			{
				if (m_windowClosed)
					m_windowClosed(windowId);
			} catch (...)
			{
				cemuLog_log(LogType::Force, "CEF window close callback failed for window {}", windowId);
			}
		}

		void RuntimeImpl::Close(Host::PointerSurface surface)
		{
			std::array<std::optional<std::uint64_t>, 2> windows;
			{
				std::scoped_lock lock(m_mutex);
				const auto& layers = m_surfaces[Index(surface)];
				windows = {layers.builtin, layers.cemod};
			}
			for (const auto window : windows)
				if (window)
					CloseWindow(*window);
		}

		bool RuntimeImpl::CloseWindow(std::uint64_t windowId)
		{
			CEF_REQUIRE_UI_THREAD();
			if (auto client = Get(windowId))
			{
				const auto surface = client->Descriptor().overlaySurface;
				InputOwnership ownership;
				if (surface)
				{
					std::scoped_lock lock(m_mutex);
					if (auto* layer = FindLayerLocked(windowId))
					{
						layer->visible = false;
						layer->interactive = false;
						layer->view.clear();
						layer->popup.clear();
						layer->popupVisible = false;
					}
					const auto& state = m_surfaces[Index(*surface)];
					ownership = ResolveSurfaceInputLocked(*surface, state.pointerX,
														  state.pointerY)
									.ownership;
				}
				if (surface)
				{
					NotifyInputOwnership(*surface, ownership);
					PublishComposite(*surface);
					UpdateLayerFocus(*surface);
				}
				if (client->Browser())
					client->RequestClose();
				else
					Closed(windowId);
				return true;
			}
			return false;
		}

		void RuntimeImpl::CloseAll()
		{
			if (!g_initialized)
				return;
			for (;;)
			{
				std::vector<CefRefPtr<Client>> clients;
				{
					std::scoped_lock lock(m_mutex);
					if (m_clients.empty())
						break;
					clients.reserve(m_clients.size());
					for (const auto& [windowId, client] : m_clients)
						clients.push_back(client);
				}
				for (const auto& client : clients)
				{
					if (client->Browser())
						client->RequestClose();
					else
						Closed(client->WindowId());
				}
				CefDoMessageLoopWork();
				std::unique_lock lock(m_mutex);
				m_closeCondition.wait_for(lock, std::chrono::milliseconds(5));
			}
		}

		void RuntimeImpl::Resize(Host::PointerSurface surface, int width, int height, double scale)
		{
			if (width <= 0 || height <= 0 || scale <= 0.0)
				return;
			std::array<std::optional<std::uint64_t>, 2> windows;
			{
				std::scoped_lock lock(m_mutex);
				auto& layers = m_surfaces[Index(surface)];
				// Window focus and activation notifications also refresh metrics. Keep
				// those size-neutral updates from discarding both retained OSR layers
				// and publishing a transparent frame before CEF repaints them.
				if (layers.width == width && layers.height == height && layers.scale == scale)
					return;
				layers.width = width;
				layers.height = height;
				layers.scale = scale;
				layers.composite.clear();
				windows = {layers.builtin, layers.cemod};
				for (const auto window : windows)
					if (window)
						if (auto* layer = FindLayerLocked(*window))
						{
							layer->width = layer->height = 0;
							layer->view.clear();
							layer->popup.clear();
							layer->popupVisible = false;
						}
			}
			PublishComposite(surface);
			for (const auto window : windows)
				if (window)
					ResizeWindow(*window, width, height, scale);
		}

		void RuntimeImpl::ResizeWindow(std::uint64_t windowId, int width, int height, double scale)
		{
			if (width <= 0 || height <= 0 || scale <= 0.0)
				return;
			if (auto client = Get(windowId))
			{
				client->SetSize(width, height, scale);
				if (auto browser = client->Browser())
				{
					if (client->Descriptor().presentation == BrowserPresentation::OverlayOsr)
					{
						browser->GetHost()->NotifyScreenInfoChanged();
						browser->GetHost()->WasResized();
					}
					else
						browser->GetHost()->NotifyMoveOrResizeStarted();
				}
			}
		}

		void RuntimeImpl::SetInteractive(Host::PointerSurface surface, bool interactive)
		{
			InputOwnership ownership;
			{
				std::scoped_lock lock(m_mutex);
				const auto window = m_surfaces[Index(surface)].builtin;
				if (window)
					if (auto* layer = FindLayerLocked(*window))
						layer->interactive = interactive;
				const auto& state = m_surfaces[Index(surface)];
				ownership = ResolveSurfaceInputLocked(surface, state.pointerX,
													  state.pointerY)
								.ownership;
			}
			NotifyInputOwnership(surface, ownership);
			UpdateLayerFocus(surface);
		}

		bool RuntimeImpl::CapturesInput(Host::PointerSurface surface) const
		{
			std::scoped_lock lock(m_mutex);
			const auto& layers = m_surfaces[Index(surface)];
			const auto order = BottomToTopLocked(layers);
			return std::ranges::any_of(order | std::views::reverse,
									   [this](const std::optional<std::uint64_t>& window) {
										   if (!window)
											   return false;
										   const auto* candidate = FindLayerLocked(*window);
										   return candidate && candidate->visible && candidate->interactive;
									   });
		}

		RuntimeImpl::ResolvedSurfaceInput RuntimeImpl::ResolveSurfaceInputLocked(
			Host::PointerSurface surface, double x, double y) const
		{
			ResolvedSurfaceInput result;
			if (m_inputSuspended[Index(surface)])
			{
				result.ownership = {InputOwner::None, InputOwner::None, InputOwner::None};
				return result;
			}
			const auto& layers = m_surfaces[Index(surface)];
			const auto order = BottomToTopLocked(layers);
			for (auto layer = order.rbegin(); layer != order.rend(); ++layer)
			{
				if (!*layer)
					continue;
				const auto* candidate = FindLayerLocked(**layer);
				if (!candidate || !candidate->visible || !candidate->interactive)
					continue;
				const bool legacyCapture = !candidate->hasInputIntent;
				const auto& intent = candidate->inputIntent;
				const bool intentVisible = legacyCapture || intent.visible;
				if (!intentVisible)
					continue;
				double candidateX = x;
				double candidateY = y;
				if (const auto client = m_clients.find(**layer); client != m_clients.end())
				{
					const auto scale = client->second->Scale();
					if (scale > 0.0)
					{
						candidateX /= scale;
						candidateY /= scale;
					}
				}
				constexpr auto webUiOwner = InputOwner::WebUi;
				if (!result.keyboardWindow &&
					(legacyCapture || intent.bindingCapture || intent.popup ||
					 intent.keyboardFocus != KeyboardFocus::None))
				{
					result.keyboardWindow = **layer;
					result.ownership.keyboard = webUiOwner;
				}
				const bool pointerHit = legacyCapture || intent.pointerCaptured ||
										std::ranges::any_of(intent.interactiveRects,
															[candidateX, candidateY](const InteractiveRect& rect) {
																return rect.Contains(candidateX, candidateY);
															});
				if (!result.pointerWindow && (intent.bindingCapture || pointerHit))
				{
					result.pointerWindow = **layer;
					result.ownership.pointer = webUiOwner;
				}
				if (!result.textWindow &&
					(legacyCapture || intent.keyboardFocus == KeyboardFocus::Text || intent.ime))
				{
					result.textWindow = **layer;
					result.ownership.text = InputOwner::WebUi;
				}
				result.ownership.webUiTextFocused =
					result.ownership.webUiTextFocused ||
					(!legacyCapture && intent.keyboardFocus == KeyboardFocus::Text);
				result.ownership.webUiPointerCaptured =
					result.ownership.webUiPointerCaptured ||
					(!legacyCapture && intent.pointerCaptured);
			}
			return result;
		}

		void RuntimeImpl::NotifyInputOwnership(Host::PointerSurface surface,
											   const InputOwnership& ownership)
		{
			const auto index = Index(surface);
			{
				std::scoped_lock lock(m_inputOwnershipMutex);
				if (m_notifiedOwnership[index] == ownership)
					return;
				m_notifiedOwnership[index] = ownership;
			}
			if (m_inputOwnershipChanged)
				m_inputOwnershipChanged(surface, ownership);
		}

		OverlayInputTarget RuntimeImpl::ResolveInput(const NativeInputEvent& event)
		{
			ResolvedSurfaceInput resolved;
			{
				std::scoped_lock lock(m_mutex);
				auto& surface = m_surfaces[Index(event.surface)];
				if (event.kind == NativeInputKind::PointerMove ||
					event.kind == NativeInputKind::PointerButton ||
					event.kind == NativeInputKind::PointerWheel ||
					event.kind == NativeInputKind::Touch ||
					event.kind == NativeInputKind::RawMouse)
				{
					surface.pointerX = event.x;
					surface.pointerY = event.y;
				}
				resolved = ResolveSurfaceInputLocked(event.surface, surface.pointerX,
													 surface.pointerY);
			}
			NotifyInputOwnership(event.surface, resolved.ownership);
			std::uint64_t target{};
			if (event.kind == NativeInputKind::Key)
				target = resolved.keyboardWindow;
			else if (event.kind == NativeInputKind::Character ||
					 event.kind == NativeInputKind::TextComposition ||
					 event.kind == NativeInputKind::TextAction)
				target = resolved.textWindow;
			else if (IsPointerInput(event.kind))
				target = resolved.pointerWindow;
			else if (event.kind == NativeInputKind::FocusLost)
				target = resolved.keyboardWindow  ? resolved.keyboardWindow
						 : resolved.pointerWindow ? resolved.pointerWindow
												  : resolved.textWindow;
			return {target, resolved.ownership};
		}

		bool RuntimeImpl::UpdateInputIntent(std::uint64_t windowId, InputIntent intent)
		{
			std::optional<Host::PointerSurface> surface;
			InputOwnership ownership;
			{
				std::scoped_lock lock(m_mutex);
				auto* layer = FindLayerLocked(windowId);
				if (!layer)
					return false;
				if (layer->hasInputIntent &&
					(intent.generation < layer->inputIntent.generation ||
					 (intent.generation == layer->inputIntent.generation &&
					  intent.revision <= layer->inputIntent.revision)))
					return true;
				if (m_inputSuspended[Index(layer->surface)])
				{
					intent.ime = false;
					intent.pointerCaptured = false;
					intent.pointerId.reset();
				}
				layer->hasInputIntent = true;
				layer->inputIntent = std::move(intent);
				surface = layer->surface;
				const auto& state = m_surfaces[Index(*surface)];
				ownership = ResolveSurfaceInputLocked(*surface, state.pointerX,
													  state.pointerY)
								.ownership;
			}
			NotifyInputOwnership(*surface, ownership);
			UpdateLayerFocus(*surface);
			return true;
		}

		void RuntimeImpl::ResetInputIntent(std::uint64_t windowId, std::uint64_t generation)
		{
			InputIntent intent;
			intent.generation = generation;
			intent.visible = false;
			(void)UpdateInputIntent(windowId, std::move(intent));
		}

		void RuntimeImpl::SetInputSuspended(Host::PointerSurface surface, bool suspended)
		{
			InputOwnership ownership;
			std::vector<CefRefPtr<Client>> clients;
			{
				std::scoped_lock lock(m_mutex);
				if (m_inputSuspended[Index(surface)] == suspended)
					return;
				m_inputSuspended[Index(surface)] = suspended;
				if (suspended)
				{
					const auto& layers = m_surfaces[Index(surface)];
					for (const auto window : {layers.builtin, layers.cemod})
						if (window)
							if (const auto found = m_clients.find(*window); found != m_clients.end())
								clients.push_back(found->second);
					for (const auto window : {layers.builtin, layers.cemod})
						if (window)
							if (auto* layer = FindLayerLocked(*window))
							{
								// Preserve visibility and the requested keyboard owner for
								// click-to-resume, but never resurrect a drag or composition.
								layer->inputIntent.ime = false;
								layer->inputIntent.pointerCaptured = false;
								layer->inputIntent.pointerId.reset();
							}
				}
				const auto& state = m_surfaces[Index(surface)];
				ownership = ResolveSurfaceInputLocked(surface, state.pointerX,
													  state.pointerY)
								.ownership;
			}
			NotifyInputOwnership(surface, ownership);
			UpdateLayerFocus(surface);
			for (const auto& client : clients)
				if (client->Browser())
				{
					client->Browser()->GetHost()->SendCaptureLostEvent();
					client->Browser()->GetHost()->ImeCancelComposition();
				}
			PublishComposite(surface);
		}

		void RuntimeImpl::ReleaseInput(Host::PointerSurface surface)
		{
			std::vector<CefRefPtr<Client>> clients;
			InputOwnership ownership;
			{
				std::scoped_lock lock(m_mutex);
				const auto& state = m_surfaces[Index(surface)];
				for (const auto window : {state.builtin, state.cemod})
				{
					if (!window)
						continue;
					if (auto* layer = FindLayerLocked(*window); layer && layer->hasInputIntent)
					{
						layer->inputIntent.visible = false;
						layer->inputIntent.keyboardFocus = KeyboardFocus::None;
						layer->inputIntent.ime = false;
						layer->inputIntent.pointerCaptured = false;
						layer->inputIntent.pointerId.reset();
						layer->inputIntent.popup = false;
						layer->inputIntent.bindingCapture = false;
					}
					if (const auto client = m_clients.find(*window); client != m_clients.end())
						clients.push_back(client->second);
				}
				ownership = ResolveSurfaceInputLocked(surface, state.pointerX, state.pointerY).ownership;
			}
			NotifyInputOwnership(surface, ownership);
			for (const auto& client : clients)
				if (client->Browser())
					client->Browser()->GetHost()->SetFocus(false);
		}

		void RuntimeImpl::SetOverlayVisible(std::uint64_t windowId, bool visible)
		{
			std::optional<Host::PointerSurface> surface;
			InputOwnership ownership;
			{
				std::scoped_lock lock(m_mutex);
				if (auto* layer = FindLayerLocked(windowId))
				{
					layer->visible = visible;
					surface = layer->surface;
					const auto& state = m_surfaces[Index(*surface)];
					ownership = ResolveSurfaceInputLocked(*surface, state.pointerX,
														  state.pointerY)
									.ownership;
				}
			}
			if (surface)
			{
				NotifyInputOwnership(*surface, ownership);
				PublishComposite(*surface);
				UpdateLayerFocus(*surface);
			}
		}

		void RuntimeImpl::SetOverlayInteractive(std::uint64_t windowId, bool interactive)
		{
			std::optional<Host::PointerSurface> surface;
			InputOwnership ownership;
			{
				std::scoped_lock lock(m_mutex);
				if (auto* layer = FindLayerLocked(windowId))
				{
					layer->interactive = interactive;
					surface = layer->surface;
					const auto& state = m_surfaces[Index(*surface)];
					ownership = ResolveSurfaceInputLocked(*surface, state.pointerX,
														  state.pointerY)
									.ownership;
				}
			}
			if (surface)
			{
				NotifyInputOwnership(*surface, ownership);
				UpdateLayerFocus(*surface);
			}
		}

		void RuntimeImpl::UpdateLayerFocus(Host::PointerSurface surface)
		{
			std::vector<std::pair<CefRefPtr<Client>, bool>> updates;
			{
				std::scoped_lock lock(m_mutex);
				const auto& layers = m_surfaces[Index(surface)];
				const auto resolved = ResolveSurfaceInputLocked(
					surface, layers.pointerX, layers.pointerY);
				const auto target = resolved.keyboardWindow;
				for (const auto window : {layers.builtin, layers.cemod})
					if (window)
						if (const auto found = m_clients.find(*window); found != m_clients.end())
							updates.emplace_back(found->second, target && target == *window);
			}
			for (const auto& [client, focused] : updates)
				if (client->Browser())
					client->Browser()->GetHost()->SetFocus(focused);
		}

		void RuntimeImpl::SetWindowFocus(std::uint64_t windowId, bool focused)
		{
			if (auto client = Get(windowId); client && client->Browser())
				client->Browser()->GetHost()->SetFocus(focused);
		}

		bool RuntimeImpl::SendInput(const NativeInputEvent& event, std::uint64_t windowId)
		{
			CefRefPtr<Client> client;
			{
				std::scoped_lock lock(m_mutex);
				if (const auto found = m_clients.find(windowId); found != m_clients.end())
					client = found->second;
			}
			if (!client || !client->Browser())
				return false;
			auto host = client->Browser()->GetHost();
			const double scale = client->Scale();
			// Bound diagnostics per browser, independently for motion and buttons.
			// Do not record text or values from the page.
			if (client->Descriptor().cemodAssets &&
				(event.kind == NativeInputKind::PointerButton || event.kind == NativeInputKind::PointerMove))
			{
				static std::unordered_map<std::uint64_t, std::array<unsigned, 2>> traceCounts;
				auto& count = traceCounts[windowId][event.kind == NativeInputKind::PointerButton ? 1 : 0];
				if (count++ < 12)
				{
					const auto [width, height] = client->Size();
					cemuLog_log(LogType::Force,
						"CEX2-POINTER cef window={} kind={} x={} y={} inside={} view={}x{} scale={}",
						windowId, static_cast<unsigned>(event.kind), event.x, event.y,
						event.insideContent, width, height, scale);
					if (auto frame = client->Browser()->GetMainFrame())
					{
						const auto script = std::string(R"JS((() => {
							const describe = e => e ? {tag:e.tagName, rect:{x:e.getBoundingClientRect().x,
								y:e.getBoundingClientRect().y,width:e.getBoundingClientRect().width,
								height:e.getBoundingClientRect().height}} : null;
							const report = data => console.debug('__CEMU_POINTER_PROBE__' + JSON.stringify(data));
							if (!window.__cemuPointerProbe) {
								window.__cemuPointerProbe = true;
								let remaining = 24;
								for (const type of ['pointermove','pointerdown','pointerup','click'])
									window.addEventListener(type, e => { if (remaining-- > 0)
										report({event:type,x:e.clientX,y:e.clientY,target:describe(e.target)}); }, true);
							}
							report({probe:true,focused:document.hasFocus(),width:innerWidth,height:innerHeight,
								target:describe(document.elementFromPoint()JS") +
							std::to_string(static_cast<int>(std::lround(event.x / scale))) + "," +
							std::to_string(static_cast<int>(std::lround(event.y / scale))) + "))});})()";
						frame->ExecuteJavaScript(script, frame->GetURL(), 0);
					}
				}
			}
			CefMouseEvent mouse;
			mouse.x = static_cast<int>(std::lround(event.x / scale));
			mouse.y = static_cast<int>(std::lround(event.y / scale));
			mouse.modifiers =
				((event.modifiers & 1U) ? EVENTFLAG_CONTROL_DOWN : 0) |
				((event.modifiers & 2U) ? EVENTFLAG_SHIFT_DOWN : 0) |
				((event.modifiers & 4U) ? EVENTFLAG_ALT_DOWN : 0) |
				((event.modifiers & 8U) ? EVENTFLAG_COMMAND_DOWN : 0);
			switch (event.kind)
			{
			case NativeInputKind::PointerMove:
				host->SendMouseMoveEvent(mouse, !event.insideContent);
				return true;
			case NativeInputKind::RawMouse:
				host->SendMouseMoveEvent(mouse, !event.insideContent);
				return true;
			case NativeInputKind::PointerButton:
				host->SendMouseClickEvent(mouse,
										  event.button == 3 ? MBT_RIGHT : event.button == 2 ? MBT_MIDDLE
																							: MBT_LEFT,
										  !event.pressed, 1);
				return true;
			case NativeInputKind::PointerWheel:
				host->SendMouseWheelEvent(mouse, event.wheelX, event.wheelY);
				return true;
			case NativeInputKind::Key:
			{
				CefKeyEvent key;
				key.type = event.pressed ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
				key.native_key_code = static_cast<int>(event.key);
				const auto normalized = WindowsKeyCodeFromUsbHid(event.usage);
				key.windows_key_code = static_cast<int>(normalized ? normalized : event.key);
				key.modifiers = mouse.modifiers;
				key.is_system_key = (event.modifiers & 4U) != 0;
				host->SendKeyEvent(key);
				return true;
			}
			case NativeInputKind::Character:
				for (const auto value : CefString(event.text).ToString16())
				{
					CefKeyEvent key;
					key.type = KEYEVENT_CHAR;
					key.character = value;
					key.unmodified_character = value;
					host->SendKeyEvent(key);
				}
				return true;
			case NativeInputKind::Touch:
			{
				CefTouchEvent touch;
				touch.id = static_cast<int>(event.touchId);
				touch.x = static_cast<float>(event.x / scale);
				touch.y = static_cast<float>(event.y / scale);
				touch.type = event.pressed ? CEF_TET_PRESSED : CEF_TET_RELEASED;
				touch.pointer_type = CEF_POINTER_TYPE_TOUCH;
				host->SendTouchEvent(touch);
				return true;
			}
			case NativeInputKind::FocusLost:
				host->SetFocus(false);
				return true;
			default:
				return false;
			}
		}

		void RuntimeImpl::ExecuteEvent(Host::PointerSurface surface, std::string_view name,
									   std::string_view payload, std::uint64_t sequence)
		{
			// Use Runtime's shared sequence. Mixing an independent counter with
			// broadcast events would make bridge/events.ts discard valid updates.
			if (auto client = Get(surface))
				ExecuteWindowEvent(client->WindowId(), name, payload, sequence);
		}

		void RuntimeImpl::ExecuteScript(Host::PointerSurface surface, std::string_view script)
		{
			if (auto client = Get(surface))
				ExecuteWindowScript(client->WindowId(), script);
		}

		void RuntimeImpl::ExecuteWindowEvent(std::uint64_t windowId, std::string_view name,
											 std::string_view payload, std::uint64_t sequence)
		{
			ExecuteWindowScript(windowId, "window.__cemuDispatchEvent?.({type:" + JsonString(name) +
											  ",sequence:" + JsonString(std::to_string(sequence)) + ",payload:" +
											  std::string(payload) + "})");
		}

		void RuntimeImpl::ExecuteCemodEvent(std::uint64_t windowId, std::string_view name,
											std::string_view payload)
		{
			ExecuteWindowScript(windowId, "window.__cemodDispatchEvent?.(" + JsonString(name) +
											  "," + std::string(payload) + ")");
		}

		void RuntimeImpl::ExecuteWindowScript(std::uint64_t windowId, std::string_view script)
		{
			if (auto client = Get(windowId); client && client->Browser())
				if (auto frame = client->Browser()->GetMainFrame())
					frame->ExecuteJavaScript(std::string(script), frame->GetURL(), 0);
		}

		bool RuntimeImpl::HasWindow(std::uint64_t windowId) const
		{
			return static_cast<bool>(Get(windowId));
		}
	} // namespace

	int ExecuteSubprocess(int argc, char* argv[])
	{
#if defined(OS_MAC)
		if (!g_libraryLoader)
		{
			auto loader = std::make_unique<CefScopedLibraryLoader>();
			if (!loader->LoadInMain())
				return 1;
			g_libraryLoader = std::move(loader);
		}
#endif
		// CefExecuteProcess may reorder argv while Chromium normalizes switches.
		// Cemu parses its own value-taking options afterwards, so passing the
		// caller-owned array can separate e.g. --title-id from its value. Keep an
		// owned copy for both CEF calls and rebuild its pointer order afterwards.
		g_arguments.clear();
		g_arguments.reserve(static_cast<std::size_t>(std::max(argc, 0)));
		for (int index = 0; index < argc; ++index)
			g_arguments.emplace_back(argv[index] ? argv[index] : "");
		RestoreArgumentPointers();
		g_app = new App;
#if defined(OS_WIN)
		CefMainArgs args(GetModuleHandleW(nullptr));
#else
		CefMainArgs args(g_argc, g_argv);
#endif
		const int result = CefExecuteProcess(args, g_app, nullptr);
		RestoreArgumentPointers();
#if defined(OS_MAC)
		if (result >= 0)
			g_libraryLoader.reset();
#endif
		return result;
	}

	int ExecuteHelperSubprocess(int argc, char* argv[])
	{
#if defined(OS_MAC)
		CefScopedLibraryLoader loader;
		if (!loader.LoadInHelper())
			return 1;
		g_arguments.clear();
		g_arguments.reserve(static_cast<std::size_t>(std::max(argc, 0)));
		for (int index = 0; index < argc; ++index)
			g_arguments.emplace_back(argv[index] ? argv[index] : "");
		RestoreArgumentPointers();
		g_app = new App;
		CefMainArgs args(g_argc, g_argv);
		const int result = CefExecuteProcess(args, g_app, nullptr);
		RestoreArgumentPointers();
		g_app = nullptr;
		return result;
#else
		return ExecuteSubprocess(argc, argv);
#endif
	}

	bool InitializeProcessRuntime()
	{
		if (g_initialized)
			return true;
		{
			std::scoped_lock lock(g_contextTasksMutex);
			g_contextInitialized = false;
			g_contextTasks.clear();
		}
		if (!CefNative::InitializeNativeUiLoop())
		{
			cemuLog_log(LogType::Force, "Unable to initialize the native CEF UI loop");
			return false;
		}
		if (!g_app)
			g_app = new App;
		std::error_code executableError;
		const auto executable = ExecutablePath(executableError);
		if (executableError || executable.empty())
		{
			cemuLog_log(LogType::Force, "Unable to resolve the executable path for CEF: {}",
						executableError.message());
			CefNative::ShutdownNativeUiLoop();
			return false;
		}
		const auto resources = CefResourceDirectory(executable);
		const auto subprocess = CefSubprocessPath(executable);
		if (!g_argv || g_argc <= 0)
		{
			cemuLog_log(LogType::Force, "CEF overlay initialization is missing process arguments");
			CefNative::ShutdownNativeUiLoop();
			return false;
		}
#if defined(OS_WIN)
		CefMainArgs args(GetModuleHandleW(nullptr));
#else
		CefMainArgs args(g_argc, g_argv);
#endif
		CefSettings settings;
		settings.windowless_rendering_enabled = true;
		settings.multi_threaded_message_loop = false;
		// CefNativeUiLoop translates CEF's requested deadlines into GLib, Win32,
		// or CFRunLoop work on the frontend UI thread and supplies a bounded
		// watchdog when Chromium does not schedule the next cycle itself.
		settings.external_message_pump = true;
		settings.background_color = CefColorSetARGB(0, 0, 0, 0);
#if defined(OS_WIN)
		CefString(&settings.resources_dir_path) = resources.wstring();
		CefString(&settings.locales_dir_path) = (resources / "locales").wstring();
		CefString(&settings.browser_subprocess_path) = subprocess.wstring();
#else
		CefString(&settings.resources_dir_path) = resources.string();
		CefString(&settings.locales_dir_path) = (resources / "locales").string();
		CefString(&settings.browser_subprocess_path) = subprocess.string();
#endif
		const auto cacheRoot = CefCacheRoot();
		std::error_code cacheError;
		std::filesystem::create_directories(cacheRoot, cacheError);
		if (cacheError)
		{
			cemuLog_log(LogType::Force, "Unable to create the CEF cache directory: {}", cacheError.message());
			CefNative::ShutdownNativeUiLoop();
			return false;
		}
#if defined(OS_WIN)
		CefString(&settings.root_cache_path) = cacheRoot.wstring();
#else
		CefString(&settings.root_cache_path) = cacheRoot.string();
#endif
#if defined(OS_WIN)
		// Cemu's Windows CEF distribution is built without cef_sandbox.lib.
		settings.no_sandbox = true;
#endif
		if (const char* noSandbox = std::getenv("CEMU_CEF_NO_SANDBOX"); noSandbox && std::string_view(noSandbox) == "1")
			settings.no_sandbox = true;
		if (!CefInitialize(args, settings, g_app, nullptr))
		{
			cemuLog_log(LogType::Force, "CefInitialize failed for the Runtime Overlay");
			CefNative::ShutdownNativeUiLoop();
			return false;
		}
		if (!CefRegisterSchemeHandlerFactory(kScheme, kDomain, new BuiltinAssetFactory))
		{
			CefShutdown();
			CefNative::ShutdownNativeUiLoop();
			cemuLog_log(LogType::Force, "CEF overlay scheme registration failed");
			return false;
		}
		g_initialized = true;
		return true;
	}

	void DoProcessMessageLoopWork()
	{
		if (g_initialized)
			CefDoMessageLoopWork();
	}

	void ShutdownProcessRuntime()
	{
		if (!g_initialized)
			return;
		std::vector<std::shared_ptr<BrowserRuntime>> runtimes;
		{
			std::scoped_lock lock(g_runtimesMutex);
			for (auto& runtime : g_runtimes)
				if (auto active = runtime.lock())
					runtimes.push_back(std::move(active));
			g_runtimes.clear();
		}
		for (const auto& runtime : runtimes)
			runtime->CloseAll();
		g_initialized = false;
		CefClearSchemeHandlerFactories();
		CefShutdown();
		{
			std::scoped_lock lock(g_contextTasksMutex);
			g_contextInitialized = false;
			g_contextTasks.clear();
		}
		CefNative::ShutdownNativeUiLoop();
		g_app = nullptr;
#if defined(OS_MAC)
		g_libraryLoader.reset();
#endif
	}

	bool IsProcessRuntimeInitialized()
	{
		return g_initialized;
	}

	std::shared_ptr<BrowserRuntime> CreateBrowserRuntime(BrowserRuntime::RpcHandler rpc,
														 std::function<void(Host::PointerSurface)> redraw, BrowserRuntime::ClosedHandler closed,
														 BrowserRuntime::WindowClosedHandler windowClosed,
														 BrowserRuntime::InputOwnershipHandler inputOwnershipChanged)
	{
		if (!g_initialized)
			return {};
		auto runtime = std::make_shared<RuntimeImpl>(std::move(rpc), std::move(redraw),
													 std::move(closed), std::move(windowClosed),
													 std::move(inputOwnershipChanged));
		{
			std::scoped_lock lock(g_runtimesMutex);
			std::erase_if(g_runtimes, [](const auto& value) { return value.expired(); });
			g_runtimes.emplace_back(runtime);
		}
		return runtime;
	}
} // namespace WebFrontend::CefOverlay
