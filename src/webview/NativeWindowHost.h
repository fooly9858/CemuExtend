#pragma once

#include "host/contracts/HostContracts.h"
#include "webview/InputFocusState.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace WebFrontend
{
	enum class NativeInputKind : std::uint8_t
	{
		PointerMove,
		PointerButton,
		PointerWheel,
		Touch,
		Key,
		Character,
		FocusLost,
		DeviceChanged,
		RawMouse,
		TextComposition,
		TextAction,
	};

	struct NativeInputEvent
	{
		NativeInputKind kind{};
		Host::PointerSurface surface{Host::PointerSurface::Main};
		std::int32_t x{};
		std::int32_t y{};
		std::int32_t deltaX{};
		std::int32_t deltaY{};
		std::int32_t wheelX{};
		std::int32_t wheelY{};
		std::int32_t contentWidth{};
		std::int32_t contentHeight{};
		std::uint32_t key{};
		std::uint16_t usagePage{0x0007};
		std::uint16_t usage{};
		// Stable physical source when the native backend can identify it. Zero means
		// unknown and is replaced by a documented aggregate fallback upstream.
		std::uint16_t deviceId{};
		std::uint8_t modifiers{};
		std::uint32_t button{};
		std::uint32_t touchId{};
		bool pressed{};
		bool repeat{};
		bool insideContent{};
		// Whether the window this event came from was active when it happened. Mouse
		// events handed to a CEMod are tagged with it, and a mod that ignores input
		// while unfocused sees nothing at all when it is wrong. Reading it from the
		// producing window beats a cached snapshot that only refreshes on some events.
		bool windowActive{true};
		std::string text;
		std::string preedit;
		std::uint32_t textCursor{};
		std::uint32_t selectionLength{};
		std::uint64_t textSequence{};
		bool textAccepted{};
	};

	struct NativePointerPresentation
	{
		Host::PointerSurface surface{Host::PointerSurface::Main};
		bool ownsPointer{};
		bool showCursor{true};
		bool confine{};
		bool enteringCapture{};
		bool leavingPolicy{};
		bool requestRawMouse{};
		bool rawMouseEnabled{};
		std::uint8_t cursor{};
	};

	struct NativeTextInputRequest
	{
		bool active{};
		std::uint64_t sequence{};
		std::string initialText;
		std::uint32_t maximumLength{};
		std::int32_t caretX{};
		std::int32_t caretY{};
		std::int32_t lineHeight{};
	};

	class INativeWindowHost
	{
	  public:
		using CloseHandler = std::function<void()>;
		using MetricsHandler = std::function<void(Host::WindowMetricsSnapshot)>;
		using PadCloseHandler = std::function<void()>;
		using GameCloseHandler = std::function<void()>;
		using InputHandler = std::function<void(const NativeInputEvent&)>;
		virtual ~INativeWindowHost() = default;

		// Backends opt into explicit activation tracking. Other platforms retain
		// their existing input behavior until they implement this contract.
		[[nodiscard]] virtual std::optional<InputWindow> GetActiveInputWindow() const
		{
			return {};
		}

		[[nodiscard]] virtual void* GetNativeWindow() const = 0;
		[[nodiscard]] virtual Host::NativeWindowHandle GetMainWindowHandle() const = 0;
		[[nodiscard]] virtual Host::WindowMetricsSnapshot GetMetrics() const = 0;
		// Native child-browser container used by the CEF frontend. Handles are
		// intentionally opaque so CEF platform types do not leak into host contracts.
		[[nodiscard]] virtual void* GetBrowserParentWindow() const = 0;
		[[nodiscard]] virtual Host::RenderRegionBounds GetBrowserBounds() const = 0;
		[[nodiscard]] virtual double GetBrowserDpiScale() const = 0;
		virtual void AttachBrowser(void* browserWindow) = 0;
		virtual void ResizeBrowser() = 0;
		virtual void FocusBrowser() = 0;
		virtual void DetachBrowser(void* browserWindow) = 0;
		virtual void Show() = 0;
		virtual void HideLauncher() = 0;
		[[nodiscard]] virtual bool IsLauncherVisible() const = 0;
		virtual void ShowLibrary() = 0;
		[[nodiscard]] virtual Host::IRenderRegion& CreateMainRenderRegion() = 0;
		virtual void DestroyMainRenderRegion() = 0;
		virtual void ShowRenderRegion() = 0;
		virtual void RequestRenderRedraw(Host::PointerSurface) {}
		[[nodiscard]] virtual Host::IRenderRegion& CreatePadRenderRegion() = 0;
		virtual void DestroyPadRenderRegion() = 0;
		[[nodiscard]] virtual bool IsPadRenderRegionOpen() const = 0;
		virtual void SetFullscreen(bool fullscreen) = 0;
		virtual void SetCloseHandler(CloseHandler handler) = 0;
		virtual void SetGameCloseHandler(GameCloseHandler handler) = 0;
		virtual void SetMetricsHandler(MetricsHandler handler) = 0;
		virtual void SetPadCloseHandler(PadCloseHandler handler) = 0;
		virtual void SetPadMetricsEnabled(bool enabled) = 0;
		virtual void SetInputHandler(InputHandler handler) = 0;
		virtual void ApplyPointerPresentation(const NativePointerPresentation& presentation) = 0;
		virtual void UpdateTextInput(const NativeTextInputRequest& request) = 0;
		[[nodiscard]] virtual std::string GetKeyName(std::uint32_t key) const = 0;
		[[nodiscard]] virtual std::pair<bool, std::string> GetClipboardText() = 0;
		[[nodiscard]] virtual bool SetClipboardText(std::string text) = 0;
		[[nodiscard]] virtual bool SetClipboardImage(std::span<const std::uint8_t> rgb,
													 std::int32_t width, std::int32_t height) = 0;
		[[nodiscard]] virtual bool OpenExternalUrl(std::string url) = 0;
		// File-system paths remain native-only. Callers expose short-lived opaque
		// tokens to web content instead of returning either path across the bridge.
		[[nodiscard]] virtual std::optional<std::string> PickTitleInstallSource() = 0;
		[[nodiscard]] virtual std::optional<std::string> PickWuaDestination(
			std::string suggestedFileName) = 0;
	};

	[[nodiscard]] std::unique_ptr<INativeWindowHost> CreateNativeWindowHost();
} // namespace WebFrontend
