#include "webview/NativeWindowHost.h"
#include "webview/NativeKeyboardInput.h"

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>
#ifdef HAS_WAYLAND
#include <gdk/gdkwayland.h>
#endif

namespace WebFrontend
{
	namespace
	{
		std::optional<::Window> ActiveToplevelXid(GdkDisplay* gdkDisplay)
		{
			std::optional<::Window> active;
			if (gdkDisplay && GDK_IS_X11_DISPLAY(gdkDisplay))
			{
				auto* display = gdk_x11_display_get_xdisplay(gdkDisplay);
				const auto atom = XInternAtom(display, "_NET_ACTIVE_WINDOW", True);
				if (atom != None)
				{
					Atom type{};
					int format{};
					unsigned long count{}, remaining{};
					unsigned char* data{};
					gdk_x11_display_error_trap_push(gdkDisplay);
					const auto status = XGetWindowProperty(display, DefaultRootWindow(display),
														   atom, 0, 1, False, XA_WINDOW, &type, &format, &count, &remaining, &data);
					const auto error = gdk_x11_display_error_trap_pop(gdkDisplay);
					if (!error && status == Success && type == XA_WINDOW && format == 32 && count == 1 && data)
						active = *reinterpret_cast<unsigned long*>(data);
					if (data)
						XFree(data);
				}
			}
			return active;
		}

		struct GtkInputBinding;

		class XInput2RawMouse
		{
		  public:
			bool Activate(GtkInputBinding* binding, GdkDisplay* display);
			void Deactivate(GtkInputBinding* binding);
			[[nodiscard]] bool IsActive(const GtkInputBinding* binding) const
			{
				return m_active == binding;
			}

		  private:
			struct FractionalMotion
			{
				double x{};
				double y{};
			};

			static GdkFilterReturn Filter(GdkXEvent* event, GdkEvent*, gpointer data);
			GdkFilterReturn Handle(XEvent* event);
			bool Initialize(GdkDisplay* display);

			GdkDisplay* m_gdkDisplay{};
			Display* m_xDisplay{};
			GtkInputBinding* m_active{};
			std::unordered_map<int, FractionalMotion> m_fractionalMotion;
			int m_opcode{};
			bool m_initialized{};
		};

		XInput2RawMouse& RawMouse()
		{
			// GDK owns the display for the process lifetime. Keeping the filter alive
			// avoids removing it after GDK has already begun tearing the display down.
			static auto* rawMouse = new XInput2RawMouse;
			return *rawMouse;
		}

		std::uint16_t GtkUsbHidUsage(GtkWidget* source, const GdkEventKey& event)
		{
			guint baseKeyval{};
			GdkModifierType consumed{};
			auto* keymap = gdk_keymap_get_for_display(gtk_widget_get_display(source));
			if (keymap && gdk_keymap_translate_keyboard_state(
							  keymap, event.hardware_keycode, static_cast<GdkModifierType>(0),
							  event.group, &baseKeyval, nullptr, nullptr, &consumed))
			{
				if (const auto usage = XkbBaseKeyvalUsbHidUsage(baseKeyval))
					return usage;
			}
			return XkbBaseKeyvalUsbHidUsage(event.keyval);
		}

		struct GtkInputBinding
		{
			~GtkInputBinding()
			{
				RawMouse().Deactivate(this);
			}

			Host::PointerSurface surface{Host::PointerSurface::Main};
			INativeWindowHost::InputHandler* handler{};
			GtkWidget* contentWidget{};
			std::unordered_set<std::uint32_t> pressedKeys;
			std::uint64_t focusGeneration{};
			bool detectableAutoRepeat{};
			bool captured{};
			bool rawMouseEnabled{};
			bool warpCapture{};
			bool positionValid{};
			double lastX{};
			double lastY{};
		};

		void DispatchGtkInput(GtkWidget* source, GtkInputBinding* binding,
							  NativeInputEvent event)
		{
			if (!binding || !binding->handler || !*binding->handler)
				return;
			event.surface = binding->surface;
			auto* content = binding->contentWidget ? binding->contentWidget : source;
			auto* toplevel = gtk_widget_get_toplevel(source);
			event.windowActive = toplevel && GTK_IS_WINDOW(toplevel) &&
								 gtk_window_is_active(GTK_WINDOW(toplevel)) != FALSE;
			GtkAllocation allocation{};
			gtk_widget_get_allocation(content, &allocation);
			const auto scale = gtk_widget_get_scale_factor(content);
			event.contentWidth = allocation.width * scale;
			event.contentHeight = allocation.height * scale;
			if (event.kind == NativeInputKind::PointerButton)
				event.insideContent = event.x >= 0 && event.y >= 0 &&
									  event.x < event.contentWidth && event.y < event.contentHeight;
			(*binding->handler)(event);
		}

		bool XInput2RawMouse::Initialize(GdkDisplay* display)
		{
			if (m_initialized)
				return display == m_gdkDisplay && m_xDisplay;
			m_initialized = true;
			if (!display || !GDK_IS_X11_DISPLAY(display))
				return false;

			m_gdkDisplay = display;
			m_xDisplay = gdk_x11_display_get_xdisplay(display);
			int eventBase{};
			int errorBase{};
			if (!m_xDisplay ||
				!XQueryExtension(m_xDisplay, "XInputExtension", &m_opcode,
								 &eventBase, &errorBase))
			{
				m_xDisplay = nullptr;
				return false;
			}

			int major = 2;
			int minor = 0;
			if (XIQueryVersion(m_xDisplay, &major, &minor) != Success)
			{
				m_xDisplay = nullptr;
				return false;
			}

			unsigned char bits[XIMaskLen(XI_RawMotion)]{};
			XIEventMask mask{XIAllMasterDevices, static_cast<int>(sizeof(bits)), bits};
			XISetMask(bits, XI_RawMotion);
			if (XISelectEvents(m_xDisplay, DefaultRootWindow(m_xDisplay), &mask, 1) != Success)
			{
				m_xDisplay = nullptr;
				return false;
			}
			XFlush(m_xDisplay);
			gdk_window_add_filter(nullptr, &XInput2RawMouse::Filter, this);
			return true;
		}

		bool XInput2RawMouse::Activate(GtkInputBinding* binding, GdkDisplay* display)
		{
			if (!binding || !Initialize(display))
				return false;
			if (m_active != binding)
				m_fractionalMotion.clear();
			m_active = binding;
			return true;
		}

		void XInput2RawMouse::Deactivate(GtkInputBinding* binding)
		{
			if (m_active != binding)
				return;
			m_active = nullptr;
			m_fractionalMotion.clear();
		}

		GdkFilterReturn XInput2RawMouse::Filter(GdkXEvent* event, GdkEvent*, gpointer data)
		{
			return static_cast<XInput2RawMouse*>(data)->Handle(
				static_cast<XEvent*>(event));
		}

		GdkFilterReturn XInput2RawMouse::Handle(XEvent* event)
		{
			if (!m_active || !event || event->type != GenericEvent ||
				event->xcookie.extension != m_opcode ||
				event->xcookie.evtype != XI_RawMotion ||
				!XGetEventData(m_xDisplay, &event->xcookie))
				return GDK_FILTER_CONTINUE;

			auto* raw = static_cast<XIRawEvent*>(event->xcookie.data);
			double deltaX{};
			double deltaY{};
			if (raw && raw->raw_values)
			{
				int valueIndex{};
				for (int axis = 0; axis < raw->valuators.mask_len * 8; ++axis)
				{
					if (!XIMaskIsSet(raw->valuators.mask, axis))
						continue;
					const auto value = raw->raw_values[valueIndex++];
					if (axis == 0)
						deltaX = value;
					else if (axis == 1)
						deltaY = value;
				}
			}

			const auto sourceId = raw ? raw->sourceid : 0;
			auto& fractional = m_fractionalMotion[sourceId];
			double integralX{};
			double integralY{};
			fractional.x = std::modf(fractional.x + deltaX, &integralX);
			fractional.y = std::modf(fractional.y + deltaY, &integralY);
			const auto clampDelta = [](double value) {
				return static_cast<std::int32_t>(std::clamp(
					value,
					static_cast<double>(std::numeric_limits<std::int32_t>::min()),
					static_cast<double>(std::numeric_limits<std::int32_t>::max())));
			};
			const auto x = clampDelta(integralX);
			const auto y = clampDelta(integralY);
			if (x || y)
			{
				const auto deviceId = sourceId > 0 &&
											  sourceId <= std::numeric_limits<std::uint16_t>::max()
										  ? static_cast<std::uint16_t>(sourceId)
										  : std::uint16_t{};
				DispatchGtkInput(m_active->contentWidget, m_active,
								 {.kind = NativeInputKind::RawMouse,
								  .deltaX = x,
								  .deltaY = y,
								  .deviceId = deviceId});
			}
			XFreeEventData(m_xDisplay, &event->xcookie);
			return GDK_FILTER_CONTINUE;
		}

		void ConnectInput(GtkWidget* widget, Host::PointerSurface surface,
						  INativeWindowHost::InputHandler* handler,
						  GtkWidget* keySource = nullptr)
		{
			if (!keySource)
				keySource = widget;
			gtk_widget_add_events(widget, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK |
											  GDK_BUTTON_RELEASE_MASK | GDK_SCROLL_MASK | GDK_TOUCH_MASK);
			gtk_widget_add_events(keySource,
								  GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | GDK_FOCUS_CHANGE_MASK);
			auto* binding = new GtkInputBinding{surface, handler, widget};
			if (auto* display = gtk_widget_get_display(keySource);
				display && GDK_IS_X11_DISPLAY(display))
			{
				Bool supported{};
				XkbSetDetectableAutoRepeat(gdk_x11_display_get_xdisplay(display), True,
										   &supported);
				binding->detectableAutoRepeat = supported != False;
			}
			g_object_set_data_full(G_OBJECT(keySource), "cemu-input-binding", binding, +[](gpointer data) { delete static_cast<GtkInputBinding*>(data); });
			g_signal_connect(widget, "motion-notify-event", G_CALLBACK((+[](GtkWidget* source, GdkEventMotion* event, gpointer data) -> gboolean {
								 auto* binding = static_cast<GtkInputBinding*>(data);
								 const auto scale = gtk_widget_get_scale_factor(source);
								 if (binding->captured)
								 {
									 if (RawMouse().IsActive(binding))
										 return TRUE;
									 if (binding->warpCapture)
									 {
										 GtkAllocation allocation{};
										 gtk_widget_get_allocation(source, &allocation);
										 const auto centerX = allocation.width / 2;
										 const auto centerY = allocation.height / 2;
										 const auto deltaX = static_cast<std::int32_t>((event->x - centerX) * scale);
										 const auto deltaY = static_cast<std::int32_t>((event->y - centerY) * scale);
										 if (deltaX || deltaY)
										 {
											 if (binding->rawMouseEnabled)
												 DispatchGtkInput(source, binding, {.kind = NativeInputKind::RawMouse, .deltaX = deltaX, .deltaY = deltaY});
											 else
												 DispatchGtkInput(source, binding, {.kind = NativeInputKind::PointerMove, .x = static_cast<std::int32_t>(event->x * scale), .y = static_cast<std::int32_t>(event->y * scale), .insideContent = true});
										 }
										 auto* window = gtk_widget_get_window(source);
										 auto* xdisplay = gdk_x11_display_get_xdisplay(gtk_widget_get_display(source));
										 XWarpPointer(xdisplay, None, gdk_x11_window_get_xid(window),
													  0, 0, 0, 0, centerX * scale, centerY * scale);
										 XFlush(xdisplay);
										 return TRUE;
									 }
									 if (binding->positionValid)
									 {
										 if (binding->rawMouseEnabled)
											 DispatchGtkInput(source, binding, {.kind = NativeInputKind::RawMouse, .deltaX = static_cast<std::int32_t>((event->x - binding->lastX) * scale), .deltaY = static_cast<std::int32_t>((event->y - binding->lastY) * scale)});
										 else
											 DispatchGtkInput(source, binding, {.kind = NativeInputKind::PointerMove, .x = static_cast<std::int32_t>(event->x * scale), .y = static_cast<std::int32_t>(event->y * scale), .insideContent = true});
									 }
									 binding->lastX = event->x;
									 binding->lastY = event->y;
									 binding->positionValid = true;
									 return TRUE;
								 }
								 NativeInputEvent input{.kind = NativeInputKind::PointerMove,
														.x = static_cast<std::int32_t>(event->x * scale),
														.y = static_cast<std::int32_t>(event->y * scale),
														.insideContent = true};
								 DispatchGtkInput(source, binding, std::move(input));
								 return TRUE;
							 })),
							 binding);
			g_signal_connect(widget, "button-press-event", G_CALLBACK((+[](GtkWidget* source, GdkEventButton* event, gpointer data) -> gboolean {
								 const auto scale = gtk_widget_get_scale_factor(source);
								 DispatchGtkInput(source, static_cast<GtkInputBinding*>(data), {.kind = NativeInputKind::PointerButton, .x = static_cast<std::int32_t>(event->x * scale), .y = static_cast<std::int32_t>(event->y * scale), .button = event->button, .pressed = true, .insideContent = true});
								 gtk_widget_grab_focus(source);
								 return TRUE;
							 })),
							 binding);
			g_signal_connect(widget, "button-release-event", G_CALLBACK((+[](GtkWidget* source, GdkEventButton* event, gpointer data) -> gboolean {
								 const auto scale = gtk_widget_get_scale_factor(source);
								 DispatchGtkInput(source, static_cast<GtkInputBinding*>(data), {.kind = NativeInputKind::PointerButton, .x = static_cast<std::int32_t>(event->x * scale), .y = static_cast<std::int32_t>(event->y * scale), .button = event->button, .pressed = false, .insideContent = true});
								 return TRUE;
							 })),
							 binding);
			g_signal_connect(widget, "scroll-event", G_CALLBACK((+[](GtkWidget* source, GdkEventScroll* event, gpointer data) -> gboolean {
								 const auto scale = gtk_widget_get_scale_factor(source);
								 double dx{}, dy{};
								 if (!gdk_event_get_scroll_deltas(reinterpret_cast<GdkEvent*>(event), &dx, &dy))
								 {
									 dx = event->direction == GDK_SCROLL_LEFT ? -1 : event->direction == GDK_SCROLL_RIGHT ? 1
																														  : 0;
									 dy = event->direction == GDK_SCROLL_UP ? -1 : event->direction == GDK_SCROLL_DOWN ? 1
																													   : 0;
								 }
								 DispatchGtkInput(source, static_cast<GtkInputBinding*>(data), {.kind = NativeInputKind::PointerWheel, .x = static_cast<std::int32_t>(event->x * scale), .y = static_cast<std::int32_t>(event->y * scale), .wheelX = static_cast<std::int32_t>(-dx * 120.0), .wheelY = static_cast<std::int32_t>(-dy * 120.0), .insideContent = true});
								 return TRUE;
							 })),
							 binding);
			g_signal_connect(keySource, "key-press-event", G_CALLBACK((+[](GtkWidget* source, GdkEventKey* event, gpointer data) -> gboolean {
								 auto* binding = static_cast<GtkInputBinding*>(data);
								 const auto modifiers = static_cast<std::uint8_t>(
									 ((event->state & GDK_CONTROL_MASK) ? 1U : 0U) |
									 ((event->state & GDK_SHIFT_MASK) ? 2U : 0U) |
									 ((event->state & GDK_MOD1_MASK) ? 4U : 0U) |
									 ((event->state & GDK_META_MASK) ? 8U : 0U));
								 const bool repeat =
									 !binding->pressedKeys.insert(event->hardware_keycode).second;
								 const auto consumerUsage = XkbConsumerUsbHidUsage(event->keyval);
								 DispatchGtkInput(source, binding, {.kind = NativeInputKind::Key, .key = event->keyval, .usagePage = consumerUsage ? ConsumerUsagePage : KeyboardUsagePage, .usage = consumerUsage ? consumerUsage : GtkUsbHidUsage(source, *event), .modifiers = modifiers, .pressed = true, .repeat = repeat});
								 if (event->string && event->length > 0)
									 DispatchGtkInput(source, binding, {.kind = NativeInputKind::Character, .repeat = repeat, .text = std::string(event->string, event->length)});
								 return TRUE;
							 })),
							 binding);
			g_signal_connect(keySource, "key-release-event", G_CALLBACK((+[](GtkWidget* source, GdkEventKey* event, gpointer data) -> gboolean {
								 auto* binding = static_cast<GtkInputBinding*>(data);
								 bool autoRepeatRelease = false;
								 if (!binding->detectableAutoRepeat)
								 {
									 if (auto* next = gdk_event_peek())
									 {
										 autoRepeatRelease = next->type == GDK_KEY_PRESS &&
															 next->key.hardware_keycode == event->hardware_keycode &&
															 next->key.time == event->time;
										 gdk_event_free(next);
									 }
								 }
								 if (autoRepeatRelease)
									 return TRUE;
								 binding->pressedKeys.erase(event->hardware_keycode);
								 const auto modifiers = static_cast<std::uint8_t>(
									 ((event->state & GDK_CONTROL_MASK) ? 1U : 0U) |
									 ((event->state & GDK_SHIFT_MASK) ? 2U : 0U) |
									 ((event->state & GDK_MOD1_MASK) ? 4U : 0U) |
									 ((event->state & GDK_META_MASK) ? 8U : 0U));
								 const auto consumerUsage = XkbConsumerUsbHidUsage(event->keyval);
								 DispatchGtkInput(source, binding, {.kind = NativeInputKind::Key, .key = event->keyval, .usagePage = consumerUsage ? ConsumerUsagePage : KeyboardUsagePage, .usage = consumerUsage ? consumerUsage : GtkUsbHidUsage(source, *event), .modifiers = modifiers, .pressed = false});
								 return TRUE;
							 })),
							 binding);
			g_signal_connect(keySource, "focus-in-event", G_CALLBACK((+[](GtkWidget* source, GdkEventFocus*, gpointer data) -> gboolean {
								 auto* binding = static_cast<GtkInputBinding*>(data);
								 ++binding->focusGeneration;
								 // Keep held keys identifiable as repeats after reactivation,
								 // but discard releases that happened in another application.
								 auto* display = gtk_widget_get_display(source);
								 if (display && GDK_IS_X11_DISPLAY(display))
								 {
									 char keys[32]{};
									 XQueryKeymap(gdk_x11_display_get_xdisplay(display), keys);
									 std::erase_if(binding->pressedKeys, [&keys](std::uint32_t code) {
										 return code >= 256 || !(keys[code / 8] & (1U << (code % 8)));
									 });
								 }
								 return FALSE;
							 })),
							 binding);
			g_signal_connect(keySource, "focus-out-event", G_CALLBACK((+[](GtkWidget* source, GdkEventFocus*, gpointer data) -> gboolean {
								 struct DeferredFocusLoss
								 {
									 GtkWidget* source{};
									 std::uint64_t generation{};
								 };
								 auto* binding = static_cast<GtkInputBinding*>(data);
								 auto* deferred = new DeferredFocusLoss{
									 GTK_WIDGET(g_object_ref(source)), ++binding->focusGeneration};
								 g_idle_add_full(
									 G_PRIORITY_DEFAULT_IDLE,
									 +[](gpointer value) -> gboolean {
										 auto* deferred = static_cast<DeferredFocusLoss*>(value);
										 if (gtk_widget_in_destruction(deferred->source))
											 return G_SOURCE_REMOVE;
										 auto* binding = static_cast<GtkInputBinding*>(g_object_get_data(
											 G_OBJECT(deferred->source), "cemu-input-binding"));
										 if (!binding || binding->focusGeneration != deferred->generation)
											 return G_SOURCE_REMOVE;
										 auto* toplevel = gtk_widget_get_toplevel(deferred->source);
										 if (toplevel && GTK_IS_WINDOW(toplevel) &&
											 gtk_window_is_active(GTK_WINDOW(toplevel)))
											 return G_SOURCE_REMOVE;
										 DispatchGtkInput(deferred->source, binding,
														  {.kind = NativeInputKind::FocusLost});
										 return G_SOURCE_REMOVE;
									 },
									 deferred,
									 +[](gpointer value) {
										 auto* deferred = static_cast<DeferredFocusLoss*>(value);
										 g_object_unref(deferred->source);
										 delete deferred;
									 });
								 return FALSE;
							 })),
							 binding);
			g_signal_connect(widget, "touch-event", G_CALLBACK((+[](GtkWidget* source, GdkEventTouch* event, gpointer data) -> gboolean {
								 const auto scale = gtk_widget_get_scale_factor(source);
								 const bool pressed = event->type != GDK_TOUCH_END && event->type != GDK_TOUCH_CANCEL;
								 DispatchGtkInput(source, static_cast<GtkInputBinding*>(data), {.kind = NativeInputKind::Touch, .x = static_cast<std::int32_t>(event->x * scale), .y = static_cast<std::int32_t>(event->y * scale), .touchId = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(event->sequence)), .pressed = pressed, .insideContent = true});
								 return TRUE;
							 })),
							 binding);
		}

		bool IsWaylandDisplay(GdkDisplay* display)
		{
			const auto waylandType = g_type_from_name("GdkWaylandDisplay");
			return display && waylandType != G_TYPE_INVALID &&
				   g_type_is_a(G_OBJECT_TYPE(display), waylandType);
		}

		Host::NativeWindowHandle NativeHandle(GtkWidget* widget)
		{
			if (!widget)
				return {};
			gtk_widget_realize(widget);
			auto* window = gtk_widget_get_window(widget);
			if (!window)
				return {};
			auto* display = gdk_window_get_display(window);
			Host::NativeWindowHandle result;
			if (GDK_IS_X11_WINDOW(window))
			{
				result.backend = Host::NativeWindowBackend::X11;
				result.display = gdk_x11_display_get_xdisplay(display);
				result.surface = reinterpret_cast<void*>(gdk_x11_window_get_xid(window));
			}
#ifdef HAS_WAYLAND
			else if (GDK_IS_WAYLAND_WINDOW(window))
			{
				result.backend = Host::NativeWindowBackend::Wayland;
				result.display = gdk_wayland_display_get_wl_display(display);
				result.surface = gdk_wayland_window_get_wl_surface(window);
			}
#endif
			return result;
		}

		class GtkRenderRegion final : public Host::IRenderRegion
		{
		  public:
			explicit GtkRenderRegion(GtkWidget* stack, INativeWindowHost::InputHandler* inputHandler)
				: m_stack(stack), m_widget(gtk_drawing_area_new())
			{
				gtk_widget_set_hexpand(m_widget, TRUE);
				gtk_widget_set_vexpand(m_widget, TRUE);
				gtk_widget_set_can_focus(m_widget, TRUE);
				gtk_stack_add_named(GTK_STACK(m_stack), m_widget, "render");
				// Install native input masks before GTK realizes the drawing area.
				ConnectInput(m_widget, Host::PointerSurface::Main, inputHandler);
				gtk_widget_realize(m_widget);
			}

			~GtkRenderRegion() override
			{
				PrepareForDestroy();
			}

			Host::NativeWindowHandle GetWindowHandle() const override
			{
				return NativeHandle(gtk_widget_get_toplevel(m_widget));
			}

			Host::NativeWindowHandle GetSurfaceHandle() const override
			{
				return NativeHandle(m_widget);
			}

			Host::RenderRegionBounds GetBounds() const override
			{
				GtkAllocation allocation{};
				gtk_widget_get_allocation(m_widget, &allocation);
				return {allocation.x, allocation.y, allocation.width, allocation.height};
			}

			void SetBounds(Host::RenderRegionBounds bounds) override
			{
				gtk_widget_set_size_request(m_widget,
											std::max(1, bounds.width), std::max(1, bounds.height));
				gtk_widget_set_margin_start(m_widget, std::max(0, bounds.x));
				gtk_widget_set_margin_top(m_widget, std::max(0, bounds.y));
			}

			void SetVisible(bool visible) override
			{
				if (visible)
					gtk_widget_show(m_widget);
				else
					gtk_widget_hide(m_widget);
			}

			void RequestFocus() override
			{
				gtk_widget_grab_focus(m_widget);
			}
			GtkWidget* Widget() const
			{
				return m_widget;
			}
			void PrepareForDestroy() override
			{
				if (std::exchange(m_prepared, true) || !m_widget)
					return;
				gtk_widget_hide(m_widget);
				gtk_container_remove(GTK_CONTAINER(m_stack), m_widget);
				m_widget = nullptr;
			}

		  private:
			GtkWidget* m_stack{};
			GtkWidget* m_widget{};
			bool m_prepared{};
		};

		class GtkPadRenderRegion final : public Host::IRenderRegion
		{
		  public:
			explicit GtkPadRenderRegion(std::string title, Host::PointerSurface surface,
										std::function<void()> closeHandler,
										std::function<void()> metricsHandler, INativeWindowHost::InputHandler* inputHandler)
				: m_closeHandler(std::move(closeHandler)),
				  m_metricsHandler(std::move(metricsHandler))
			{
				m_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
				try
				{
					gtk_window_set_title(GTK_WINDOW(m_window), title.c_str());
					gtk_window_set_default_size(GTK_WINDOW(m_window),
												surface == Host::PointerSurface::Main ? 1280 : 854,
												surface == Host::PointerSurface::Main ? 720 : 480);
					m_widget = gtk_drawing_area_new();
					gtk_widget_set_hexpand(m_widget, TRUE);
					gtk_widget_set_vexpand(m_widget, TRUE);
					gtk_widget_set_can_focus(m_widget, TRUE);
					gtk_container_add(GTK_CONTAINER(m_window), m_widget);
					g_signal_connect(m_window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
										 auto& self = *static_cast<GtkPadRenderRegion*>(data);
										 if (self.m_closeHandler)
											 self.m_closeHandler();
										 return TRUE;
									 }),
									 this);
					g_signal_connect(m_widget, "size-allocate", G_CALLBACK(+[](GtkWidget*, GtkAllocation*, gpointer data) {
										 auto& self = *static_cast<GtkPadRenderRegion*>(data);
										 if (self.m_metricsHandler)
											 self.m_metricsHandler();
									 }),
									 this);
					// Key focus can move between GTK-owned children while the game window
					// remains active. Listen at the toplevel so the physical keyboard
					// stream does not disappear when an overlay changes browser focus.
					// Event masks must be present before show_all realizes both widgets.
					ConnectInput(m_widget, surface, inputHandler, m_window);
					// The window metrics carry whether the application is active, and
					// every mouse event a CEMod receives is tagged with it. Without a
					// refresh when this window gains or loses focus the flag stays at
					// whatever it was when the window was last resized, so a mod that
					// ignores unfocused input - as any sane one does - sees no mouse at
					// all while a title is running.
					gtk_widget_add_events(m_window, GDK_FOCUS_CHANGE_MASK);
					g_signal_connect(m_window, "focus-in-event", G_CALLBACK(+[](GtkWidget*, GdkEventFocus*, gpointer data) -> gboolean {
										 auto& self = *static_cast<GtkPadRenderRegion*>(data);
										 self.ClaimInputFocus();
										 if (self.m_metricsHandler)
											 self.m_metricsHandler();
										 return FALSE;
									 }),
									 this);
					// ConnectInput defers focus loss until GTK settles child focus changes.
					// Publishing metrics here would bypass that generation-checked decision.
					gtk_widget_show_all(m_window);
					gtk_widget_realize(m_widget);
					gtk_widget_hide(m_window);
				} catch (...)
				{
					if (m_window)
						gtk_widget_destroy(m_window);
					m_window = nullptr;
					m_widget = nullptr;
					throw;
				}
			}

			~GtkPadRenderRegion() override
			{
				PrepareForDestroy();
			}
			Host::NativeWindowHandle GetWindowHandle() const override
			{
				return NativeHandle(m_window);
			}
			Host::NativeWindowHandle GetSurfaceHandle() const override
			{
				return NativeHandle(m_widget);
			}
			int GetScaleFactor() const
			{
				return gtk_widget_get_scale_factor(m_widget);
			}
			GtkWidget* Widget() const
			{
				return m_widget;
			}
			GtkWidget* WindowWidget() const
			{
				return m_window;
			}
			Host::RenderRegionBounds GetBounds() const override
			{
				GtkAllocation allocation{};
				gtk_widget_get_allocation(m_widget, &allocation);
				return {0, 0, allocation.width, allocation.height};
			}
			void SetBounds(Host::RenderRegionBounds bounds) override
			{
				gtk_window_resize(GTK_WINDOW(m_window), std::max(1, bounds.width),
								  std::max(1, bounds.height));
			}
			void SetVisible(bool visible) override
			{
				if (visible)
					gtk_widget_show(m_window);
				else
					gtk_widget_hide(m_window);
			}
			void RequestFocus() override
			{
				gtk_window_present(GTK_WINDOW(m_window));
				gtk_window_set_focus(GTK_WINDOW(m_window), m_widget);
				gtk_widget_grab_focus(m_widget);
				ClaimInputFocus();
			}
			void SetFullscreen(bool fullscreen)
			{
				if (fullscreen)
					gtk_window_fullscreen(GTK_WINDOW(m_window));
				else
					gtk_window_unfullscreen(GTK_WINDOW(m_window));
			}
			bool IsActive() const
			{
				return m_window && gtk_window_is_active(GTK_WINDOW(m_window)) != FALSE;
			}

			// The launcher hands X input focus to CEF's child window so the browser can
			// receive keys, and an X child keeps that focus until something takes it.
			// The compositor making this window active does not: keystrokes would still
			// be delivered to the launcher's browser while the user looks at the game.
			// Claiming focus here only ever agrees with the window the compositor just
			// activated, so it cannot fight the user's own focus choice.
			void ClaimInputFocus()
			{
				if (!m_window || !gtk_window_is_active(GTK_WINDOW(m_window)))
					return;
				auto* gdkWindow = gtk_widget_get_window(m_window);
				if (!gdkWindow || !GDK_IS_X11_WINDOW(gdkWindow))
					return;
				auto* gdkDisplay = gtk_widget_get_display(m_window);
				auto* display = gdk_x11_display_get_xdisplay(gdk_window_get_display(gdkWindow));
				const auto self = gdk_x11_window_get_xid(gdkWindow);
				if (const auto active = ActiveToplevelXid(gdkDisplay); active && *active != self)
					return; // Never answer a child focus event by fighting the compositor.
				gdk_x11_display_error_trap_push(gdkDisplay);
				::Window focused{};
				int revert{};
				if (XGetInputFocus(display, &focused, &revert) && focused != self)
				{
					XSetInputFocus(display, self, RevertToParent, CurrentTime);
					XSync(display, False);
				}
				[[maybe_unused]] const auto xError =
					gdk_x11_display_error_trap_pop(gdkDisplay);
			}
			void RequestRedraw()
			{
				if (m_widget)
					gtk_widget_queue_draw(m_widget);
			}
			void PrepareForDestroy() override
			{
				if (std::exchange(m_prepared, true) || !m_window)
					return;
				m_closeHandler = {};
				m_metricsHandler = {};
				gtk_widget_destroy(m_window);
				m_window = nullptr;
				m_widget = nullptr;
			}

		  private:
			GtkWidget* m_window{};
			GtkWidget* m_widget{};
			std::function<void()> m_closeHandler;
			std::function<void()> m_metricsHandler;
			bool m_prepared{};
		};

		class GtkWindowHost final : public INativeWindowHost
		{
		  public:
			GtkWindowHost()
			{
#if defined(CEMU_OVERLAY_BACKEND_CEF)
				if (const char* display = std::getenv("DISPLAY"); !display || !*display)
					throw std::runtime_error(
						"the CEF frontend requires X11 or XWayland, but DISPLAY is not set");
				// A windowed CEF child on Linux uses an XID. Select the X11 GDK
				// backend before GTK opens its default display, even in a Wayland
				// desktop session where XWayland is available.
				g_setenv("GDK_BACKEND", "x11", TRUE);
#endif
				if (!gtk_init_check(nullptr, nullptr))
					throw std::runtime_error("GTK initialization failed");
#ifndef HAS_WAYLAND
				if (IsWaylandDisplay(gdk_display_get_default()))
					throw std::runtime_error(
						"the current GTK session uses Wayland, but Cemu was built with ENABLE_WAYLAND=OFF");
#endif
				m_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
				gtk_window_set_title(GTK_WINDOW(m_window), "CemuExtend");
				gtk_window_set_default_size(GTK_WINDOW(m_window), 1100, 720);
				g_signal_connect(m_window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
									 auto& self = *static_cast<GtkWindowHost*>(data);
									 if (self.m_closeHandler)
										 self.m_closeHandler();
									 return TRUE;
								 }),
								 this);
				g_signal_connect(m_window, "size-allocate", G_CALLBACK(+[](GtkWidget*, GtkAllocation*, gpointer data) {
									 static_cast<GtkWindowHost*>(data)->NotifyMetrics();
								 }),
								 this);
				g_signal_connect(m_window, "window-state-event", G_CALLBACK(+[](GtkWidget*, GdkEventWindowState*, gpointer data) -> gboolean {
									 static_cast<GtkWindowHost*>(data)->NotifyMetrics();
									 return FALSE;
								 }),
								 this);
				g_signal_connect(m_window, "focus-in-event", G_CALLBACK(+[](GtkWidget*, GdkEventFocus*, gpointer data) -> gboolean {
									 static_cast<GtkWindowHost*>(data)->NotifyMetrics();
									 return FALSE;
								 }),
								 this);
				g_signal_connect(m_window, "focus-out-event", G_CALLBACK(+[](GtkWidget*, GdkEventFocus*, gpointer data) -> gboolean {
									 static_cast<GtkWindowHost*>(data)->NotifyMetrics();
									 return FALSE;
								 }),
								 this);
			}

			~GtkWindowHost() override
			{
				ReleasePointerGrab();
				DestroyPadRenderRegion();
				DestroyMainRenderRegion();
				if (m_window)
					gtk_widget_destroy(m_window);
			}

			void* GetNativeWindow() const override
			{
				return m_window;
			}
			Host::NativeWindowHandle GetMainWindowHandle() const override
			{
				return NativeHandle(m_window);
			}

			// The CEF windowed browser is parented to a small, window-owning GTK
			// drawing area.  Keep the CEF/X11 details behind INativeWindowHost so
			// the browser runtime does not need to include GTK headers.
			void* GetBrowserParentWindow() const override
			{
				auto& self = *const_cast<GtkWindowHost*>(this);
				self.EnsureBrowserContainer();
				auto* display = self.BrowserXDisplay();
				if (!display)
					throw std::runtime_error(
						"CEF native browser requires an X11 or XWayland GTK session");
				// Chromium creates its first child with the default X11 visual. GTK may
				// give the drawing area a different (for example GL-capable) visual,
				// which makes XCreateWindow fail with BadMatch. Create against the root
				// window first; OnAfterCreated immediately reparents the browser into
				// the GTK-owned container in AttachBrowser().
				return reinterpret_cast<void*>(
					static_cast<std::uintptr_t>(DefaultRootWindow(display)));
			}

			Host::RenderRegionBounds GetBrowserBounds() const override
			{
				if (!m_browserContainer)
					return {};
				GtkAllocation allocation{};
				gtk_widget_get_allocation(m_browserContainer, &allocation);
				if (auto* window = gtk_widget_get_window(m_browserContainer);
					window && GDK_IS_X11_WINDOW(window))
				{
					XWindowAttributes attributes{};
					auto* display = gdk_x11_display_get_xdisplay(
						gdk_window_get_display(window));
					if (XGetWindowAttributes(display, gdk_x11_window_get_xid(window),
											 &attributes))
						return {0, 0, std::max(1, attributes.width),
								std::max(1, attributes.height)};
				}
				return {0, 0, std::max(1, allocation.width),
						std::max(1, allocation.height)};
			}

			double GetBrowserDpiScale() const override
			{
				return m_browserContainer
						   ? static_cast<double>(gtk_widget_get_scale_factor(m_browserContainer))
						   : 1.0;
			}

			void AttachBrowser(void* widget) override
			{
				EnsureBrowserContainer();
				if (!widget)
					throw std::invalid_argument("CEF browser window must not be null");
				const auto child = static_cast<::Window>(
					reinterpret_cast<std::uintptr_t>(widget));
				auto* display = BrowserXDisplay();
				const auto parent = BrowserParentXWindow();
				if (!display || !parent)
					throw std::runtime_error("CEF browser parent is not a valid X11 window");

				auto* gdkDisplay = gtk_widget_get_display(m_browserContainer);
				gdk_x11_display_error_trap_push(gdkDisplay);
				::Window root{}, currentParent{}, *children{};
				unsigned childCount{};
				const bool queried = XQueryTree(display, child, &root, &currentParent,
												&children, &childCount) != 0;
				if (children)
					XFree(children);
				if (queried)
				{
					if (currentParent != parent)
						XReparentWindow(display, child, parent, 0, 0);
					const auto bounds = GetBrowserBounds();
					XMoveResizeWindow(display, child, 0, 0,
									  static_cast<unsigned>(std::max(1, bounds.width)),
									  static_cast<unsigned>(std::max(1, bounds.height)));
					XMapWindow(display, child);
				}
				XSync(display, False);
				const auto xError = gdk_x11_display_error_trap_pop(gdkDisplay);
				if (!queried || xError)
					throw std::invalid_argument("CEF browser window is not a valid X11 child");
				m_browserChild = child;
			}

			void ResizeBrowser() override
			{
				if (!m_browserChild)
					return;
				auto* display = BrowserXDisplay();
				if (!display)
					return;
				const auto bounds = GetBrowserBounds();
				auto* gdkDisplay = gtk_widget_get_display(m_browserContainer);
				gdk_x11_display_error_trap_push(gdkDisplay);
				XMoveResizeWindow(display, m_browserChild, 0, 0,
								  static_cast<unsigned>(std::max(1, bounds.width)),
								  static_cast<unsigned>(std::max(1, bounds.height)));
				XSync(display, False);
				if (gdk_x11_display_error_trap_pop(gdkDisplay))
					m_browserChild = None;
			}

			// Chromium activates its own X11 window whenever its web contents take
			// focus, and GTK answers that by emitting focus-in on this container even
			// while the user is working in a different one of our windows. Taking X
			// input focus from there starts a tug of war: the main window and a tool
			// window trade focus several times a second, the UI thread spins on the
			// exchange, and the frontend stops responding. Only the active toplevel may
			// direct input focus. Deliberate calls to FocusBrowser() - showing the
			// library, presenting a tool window - stay unconditional.
			// A focus-follows-mouse compositor re-focuses whatever sits under the
			// pointer, so an assertion made here can be reversed before it settles and
			// answered by another focus-in. Neither side is wrong, so neither yields:
			// the window under the pointer and the window that wants focus trade it
			// several times a second while the UI thread spins on the exchange. Bound
			// how often this window may take input focus - a burst means something else
			// is competing for it, and the compositor is allowed to win.
			bool FocusAssertionAllowed()
			{
				constexpr int kMaximumAssertions = 3;
				constexpr auto kWindow = std::chrono::seconds(1);
				constexpr auto kCooldown = std::chrono::seconds(2);
				const auto now = std::chrono::steady_clock::now();
				if (now < m_focusAssertionBlockedUntil)
					return false;
				if (now - m_focusAssertionWindowStart > kWindow)
				{
					m_focusAssertionWindowStart = now;
					m_focusAssertionCount = 0;
				}
				if (++m_focusAssertionCount > kMaximumAssertions)
				{
					m_focusAssertionBlockedUntil = now + kCooldown;
					m_focusAssertionWindowStart = now;
					m_focusAssertionCount = 0;
					return false;
				}
				return true;
			}

			void FocusBrowserFromFocusEvent()
			{
				if (!m_window || !GTK_IS_WINDOW(m_window) ||
					!gtk_window_is_active(GTK_WINDOW(m_window)))
					return;
				FocusBrowser();
			}

			void FocusBrowser() override
			{
				if (m_browserContainer)
					gtk_widget_grab_focus(m_browserContainer);
				if (m_browserChild)
				{
					if (auto* display = BrowserXDisplay())
					{
						auto* gdkDisplay = gtk_widget_get_display(m_browserContainer);
						gdk_x11_display_error_trap_push(gdkDisplay);
						if (!BrowserOwnsInputFocus(display) && FocusAssertionAllowed())
						{
							XRaiseWindow(display, m_browserChild);
							XSetInputFocus(display, m_browserChild, RevertToParent, CurrentTime);
						}
						XSync(display, False);
						if (gdk_x11_display_error_trap_pop(gdkDisplay))
							m_browserChild = None;
					}
				}
			}

			void DetachBrowser(void* widget) override
			{
				const auto child = static_cast<::Window>(
					reinterpret_cast<std::uintptr_t>(widget));
				if (!widget || child == m_browserChild)
					m_browserChild = None;
			}

			std::optional<InputWindow> GetActiveInputWindow() const override
			{
				// The WM's active toplevel is authoritative on XWayland. GTK can
				// transiently report inactive when CEF moves focus to a native child.
				const auto active = ActiveToplevelXid(gtk_widget_get_display(m_window));
				const auto isActive = [&active](GtkWidget* widget) {
					if (!widget)
						return false;
					if (!active)
						return gtk_window_is_active(GTK_WINDOW(widget)) != FALSE;
					auto* window = gtk_widget_get_window(widget);
					return window && GDK_IS_X11_WINDOW(window) && gdk_x11_window_get_xid(window) == *active;
				};
				if (m_renderRegion && isActive(m_renderRegion->WindowWidget()))
					return InputWindow::Game;
				if (m_padRenderRegion && isActive(m_padRenderRegion->WindowWidget()))
					return InputWindow::GamePad;
				if (isActive(m_window))
					return InputWindow::Launcher;
				InputWindow result = InputWindow::Outside;
				auto* windows = gtk_window_list_toplevels();
				for (auto* item = windows; item; item = item->next)
					if (isActive(GTK_WIDGET(item->data)))
						result = InputWindow::Tool;
				g_list_free(windows);
				return result;
			}

			Host::WindowMetricsSnapshot GetMetrics() const override
			{
				GtkAllocation allocation{};
				int scale = gtk_widget_get_scale_factor(m_window);
				bool appActive = gtk_window_is_active(GTK_WINDOW(m_window)) != FALSE;
				if (m_renderRegion)
				{
					const auto bounds = m_renderRegion->GetBounds();
					allocation.width = bounds.width;
					allocation.height = bounds.height;
					scale = m_renderRegion->GetScaleFactor();
					appActive = m_renderRegion->IsActive();
				}
				else
					gtk_widget_get_allocation(m_stack ? m_stack : m_window, &allocation);
				const auto active = GetActiveInputWindow();
				appActive = m_renderRegion
								? active == InputWindow::Game || active == InputWindow::GamePad
								: active == InputWindow::Launcher;
				auto metrics = Host::WindowMetricsSnapshot{
					.appActive = appActive,
					.fullscreen = m_fullscreen,
					.width = allocation.width,
					.height = allocation.height,
					.physicalWidth = allocation.width * scale,
					.physicalHeight = allocation.height * scale,
					.dpiScale = static_cast<double>(scale),
				};
				if (m_padRenderRegion)
				{
					const auto pad = m_padRenderRegion->GetBounds();
					const auto padScale = m_padRenderRegion->GetScaleFactor();
					metrics.padOpen = true;
					metrics.padWidth = pad.width;
					metrics.padHeight = pad.height;
					metrics.physicalPadWidth = pad.width * padScale;
					metrics.physicalPadHeight = pad.height * padScale;
					metrics.padDpiScale = static_cast<double>(padScale);
				}
				return metrics;
			}

			void Show() override
			{
				EnsureBrowserContainer();
				gtk_widget_show_all(m_window);
				ShowLibrary();
			}

			void HideLauncher() override
			{
				gtk_widget_hide(m_window);
			}

			bool IsLauncherVisible() const override
			{
				return m_window && gtk_widget_get_visible(m_window) != FALSE;
			}

			void ShowLibrary() override
			{
				if (!m_stack || !m_browserContainer)
					return;
				gtk_widget_set_sensitive(m_browserContainer, TRUE);
				gtk_stack_set_visible_child(GTK_STACK(m_stack), m_browserContainer);
				gtk_widget_grab_focus(m_browserContainer);
				if (m_browserChild)
					FocusBrowser();
			}

			Host::IRenderRegion& CreateMainRenderRegion() override
			{
				if (!m_renderRegion)
					m_renderRegion = std::make_unique<GtkPadRenderRegion>(
						"CemuExtend Game", Host::PointerSurface::Main,
						[this] { if (m_gameCloseHandler) m_gameCloseHandler(); },
						[this] { NotifyMetrics(); }, &m_inputHandler);
				return *m_renderRegion;
			}

			void DestroyMainRenderRegion() override
			{
				if (m_renderRegion)
					m_renderRegion->PrepareForDestroy();
				m_renderRegion.reset();
			}

			void ShowRenderRegion() override
			{
				auto& region = CreateMainRenderRegion();
				region.SetVisible(true);
				gtk_widget_set_sensitive(m_browserContainer, FALSE);
				region.RequestFocus();
			}

			void RequestRenderRedraw(Host::PointerSurface surface) override
			{
				auto* region = surface == Host::PointerSurface::Main
								   ? m_renderRegion.get()
								   : m_padRenderRegion.get();
				if (region)
					region->RequestRedraw();
			}

			Host::IRenderRegion& CreatePadRenderRegion() override
			{
				if (!m_padRenderRegion)
				{
					m_padRenderRegion = std::make_unique<GtkPadRenderRegion>(
						"CemuExtend GamePad", Host::PointerSurface::Pad,
						[this] {
							if (m_padCloseHandler)
								m_padCloseHandler();
						},
						[this] { if (m_padMetricsEnabled) NotifyMetrics(); }, &m_inputHandler);
				}
				return *m_padRenderRegion;
			}

			void DestroyPadRenderRegion() override
			{
				m_padMetricsEnabled = false;
				m_padRenderRegion.reset();
				NotifyMetrics();
			}

			bool IsPadRenderRegionOpen() const override
			{
				return m_padRenderRegion != nullptr;
			}

			void SetFullscreen(bool fullscreen) override
			{
				m_fullscreen = fullscreen;
				if (m_renderRegion)
					m_renderRegion->SetFullscreen(fullscreen);
			}

			void SetCloseHandler(CloseHandler handler) override
			{
				m_closeHandler = std::move(handler);
			}

			void SetGameCloseHandler(GameCloseHandler handler) override
			{
				m_gameCloseHandler = std::move(handler);
			}

			void SetMetricsHandler(MetricsHandler handler) override
			{
				m_metricsHandler = std::move(handler);
				NotifyMetrics();
			}

			void SetPadCloseHandler(PadCloseHandler handler) override
			{
				m_padCloseHandler = std::move(handler);
			}
			void SetPadMetricsEnabled(bool enabled) override
			{
				m_padMetricsEnabled = enabled;
			}
			void SetInputHandler(InputHandler handler) override
			{
				m_inputHandler = std::move(handler);
			}
			void ApplyPointerPresentation(const NativePointerPresentation& presentation) override
			{
				auto* widget = presentation.surface == Host::PointerSurface::Main
								   ? (m_renderRegion ? m_renderRegion->Widget() : nullptr)
								   : (m_padRenderRegion ? m_padRenderRegion->Widget() : nullptr);
				if (!widget || !gtk_widget_get_window(widget))
					return;
				auto* binding = static_cast<GtkInputBinding*>(
					g_object_get_data(G_OBJECT(widget), "cemu-input-binding"));
				if (!binding)
				{
					auto* toplevel = gtk_widget_get_toplevel(widget);
					if (toplevel)
						binding = static_cast<GtkInputBinding*>(
							g_object_get_data(G_OBJECT(toplevel), "cemu-input-binding"));
				}
				const bool wantsCapture = presentation.ownsPointer && !presentation.showCursor;
				if (binding && binding->captured != wantsCapture)
					binding->positionValid = false;
				auto* display = gtk_widget_get_display(widget);
				auto* cursor = presentation.showCursor
								   ? gdk_cursor_new_from_name(display, presentation.cursor == 1 ? "text" : presentation.cursor == 7 ? "pointer"
																																	: "default")
								   : gdk_cursor_new_for_display(display, GDK_BLANK_CURSOR);
				gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
				if (cursor)
					g_object_unref(cursor);
				auto* seat = gdk_display_get_default_seat(display);
				const bool grabPointer = presentation.confine ||
										 (presentation.ownsPointer && !presentation.showCursor);
				bool grabbed = grabPointer && m_grabbedWidget == widget;
				if (grabPointer && !grabbed)
				{
					ReleasePointerGrab();
					if (GDK_IS_X11_DISPLAY(display))
					{
						auto* xdisplay = gdk_x11_display_get_xdisplay(display);
						const auto xid = gdk_x11_window_get_xid(gtk_widget_get_window(widget));
						grabbed = XGrabPointer(xdisplay, xid, True,
											   PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
											   GrabModeAsync, GrabModeAsync, xid, None, CurrentTime) == GrabSuccess;
						m_x11Grabbed = grabbed;
					}
					else
					{
						grabbed = gdk_seat_grab(seat, gtk_widget_get_window(widget),
												GDK_SEAT_CAPABILITY_POINTER, FALSE, nullptr, nullptr, nullptr, nullptr) == GDK_GRAB_SUCCESS;
						if (grabbed)
							m_grabbedSeat = seat;
					}
					if (grabbed)
						m_grabbedWidget = widget;
				}
				else if (!grabPointer && m_grabbedWidget == widget)
				{
					ReleasePointerGrab();
				}
				if (binding)
				{
					binding->captured = wantsCapture && grabbed;
					binding->rawMouseEnabled = presentation.rawMouseEnabled;
					const bool usesRawMouse = binding->captured &&
											  binding->rawMouseEnabled && RawMouse().Activate(binding, display);
					if (!usesRawMouse)
						RawMouse().Deactivate(binding);
					binding->warpCapture = binding->captured &&
										   GDK_IS_X11_DISPLAY(display) && !usesRawMouse;
					if (binding->warpCapture && presentation.enteringCapture)
					{
						GtkAllocation allocation{};
						gtk_widget_get_allocation(widget, &allocation);
						auto* xdisplay = gdk_x11_display_get_xdisplay(display);
						const auto scale = gtk_widget_get_scale_factor(widget);
						XWarpPointer(xdisplay, None, gdk_x11_window_get_xid(gtk_widget_get_window(widget)),
									 0, 0, 0, 0, allocation.width * scale / 2, allocation.height * scale / 2);
						XFlush(xdisplay);
					}
				}
			}
			void UpdateTextInput(const NativeTextInputRequest& request) override
			{
				if (!m_textInput)
					return;
				if (!request.active)
				{
					gtk_entry_reset_im_context(GTK_ENTRY(m_textInput));
					gtk_widget_hide(m_textInput);
					m_textInputSequence = 0;
					m_textPreedit.clear();
					// Cancelling IME is also used on OS focus loss and overlay changes.
					// It must not activate the game or steal focus from the chosen UI.
					return;
				}
				if (!m_renderRegion)
					return;
				const auto bounds = m_renderRegion->GetBounds();
				const auto x = std::clamp(request.caretX * bounds.width / 1280, 0,
										  std::max(0, bounds.width - 1));
				const auto y = std::clamp(request.caretY * bounds.height / 720, 0,
										  std::max(0, bounds.height - 1));
				if (m_textPreedit.empty())
				{
					gtk_widget_set_margin_start(m_textInput, x);
					gtk_widget_set_margin_top(m_textInput, y);
				}
				gtk_widget_set_size_request(m_textInput, 2,
											std::clamp(request.lineHeight * bounds.height / 720, 20, 64));
				if (m_textInputSequence != request.sequence)
				{
					m_textInputUpdating = true;
					m_textInputSequence = request.sequence;
					m_textPreedit.clear();
					gtk_entry_set_text(GTK_ENTRY(m_textInput), request.initialText.c_str());
					gtk_entry_set_max_length(GTK_ENTRY(m_textInput),
											 static_cast<gint>(std::min<std::uint32_t>(request.maximumLength, G_MAXINT)));
					gtk_editable_set_position(GTK_EDITABLE(m_textInput), -1);
					m_textInputUpdating = false;
				}
				gtk_widget_show(m_textInput);
				gtk_widget_grab_focus(m_textInput);
			}
			std::string GetKeyName(std::uint32_t key) const override
			{
				const char* name = gdk_keyval_name(key);
				return name ? name : std::string{};
			}
			std::pair<bool, std::string> GetClipboardText() override
			{
				auto* text = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
				if (!text)
					return {false, {}};
				std::string result(text);
				g_free(text);
				return {true, std::move(result)};
			}
			bool SetClipboardText(std::string text) override
			{
				auto* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
				gtk_clipboard_set_text(clipboard, text.data(), static_cast<gint>(text.size()));
				gtk_clipboard_store(clipboard);
				return true;
			}
			bool SetClipboardImage(std::span<const std::uint8_t> rgb,
								   std::int32_t width, std::int32_t height) override
			{
				if (width <= 0 || height <= 0 || rgb.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3)
					return false;
				auto* image = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
				if (!image)
					return false;
				const auto stride = gdk_pixbuf_get_rowstride(image);
				auto* target = gdk_pixbuf_get_pixels(image);
				for (std::int32_t row = 0; row < height; ++row)
					std::memcpy(target + row * stride, rgb.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 3,
								static_cast<std::size_t>(width) * 3);
				auto* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
				gtk_clipboard_set_image(clipboard, image);
				gtk_clipboard_store(clipboard);
				g_object_unref(image);
				return true;
			}
			bool OpenExternalUrl(std::string url) override
			{
				GError* error{};
				const bool success = gtk_show_uri_on_window(GTK_WINDOW(m_window), url.c_str(),
															GDK_CURRENT_TIME, &error) != FALSE;
				if (error)
					g_error_free(error);
				return success;
			}

			std::optional<std::string> PickTitleInstallSource() override
			{
				GtkWidget* dialog = gtk_file_chooser_dialog_new("Select title to install",
																GTK_WINDOW(m_window), GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel",
																GTK_RESPONSE_CANCEL, "Open", GTK_RESPONSE_ACCEPT, nullptr);
				auto* filter = gtk_file_filter_new();
				gtk_file_filter_set_name(filter, "Wii U title metadata (meta.xml)");
				gtk_file_filter_add_pattern(filter, "meta.xml");
				gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
				std::optional<std::string> result;
				if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
				{
					char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
					if (path)
					{
						result = path;
						g_free(path);
					}
				}
				gtk_widget_destroy(dialog);
				return result;
			}

			std::optional<std::string> PickWuaDestination(
				std::string suggestedFileName) override
			{
				GtkWidget* dialog = gtk_file_chooser_dialog_new("Save Wii U game archive",
																GTK_WINDOW(m_window), GTK_FILE_CHOOSER_ACTION_SAVE, "Cancel",
																GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, nullptr);
				gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
				gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), suggestedFileName.c_str());
				auto* filter = gtk_file_filter_new();
				gtk_file_filter_set_name(filter, "Wii U archives (*.wua)");
				gtk_file_filter_add_pattern(filter, "*.wua");
				gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
				std::optional<std::string> result;
				if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
				{
					char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
					if (path)
					{
						result = path;
						g_free(path);
					}
				}
				gtk_widget_destroy(dialog);
				return result;
			}

		  private:
			void EnsureBrowserContainer()
			{
				if (m_browserContainer)
					return;
				m_browserContainer = gtk_drawing_area_new();
				gtk_widget_set_hexpand(m_browserContainer, TRUE);
				gtk_widget_set_vexpand(m_browserContainer, TRUE);
				gtk_widget_set_can_focus(m_browserContainer, TRUE);
				gtk_widget_add_events(m_browserContainer, GDK_FOCUS_CHANGE_MASK);
				g_signal_connect(m_browserContainer, "size-allocate",
								 G_CALLBACK(+[](GtkWidget*, GtkAllocation*, gpointer data) {
									 static_cast<GtkWindowHost*>(data)->ResizeBrowser();
								 }),
								 this);
				g_signal_connect(m_browserContainer, "focus-in-event",
								 G_CALLBACK(+[](GtkWidget*, GdkEventFocus*, gpointer data) -> gboolean {
									 static_cast<GtkWindowHost*>(data)->FocusBrowserFromFocusEvent();
									 return FALSE;
								 }),
								 this);

				m_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
				m_stack = gtk_stack_new();
				gtk_stack_set_transition_type(GTK_STACK(m_stack), GTK_STACK_TRANSITION_TYPE_NONE);
				gtk_widget_set_hexpand(m_stack, TRUE);
				gtk_widget_set_vexpand(m_stack, TRUE);
				gtk_stack_add_named(GTK_STACK(m_stack), m_browserContainer, "library");
				m_overlay = gtk_overlay_new();
				gtk_container_add(GTK_CONTAINER(m_overlay), m_stack);

				m_textInput = gtk_entry_new();
				gtk_entry_set_has_frame(GTK_ENTRY(m_textInput), FALSE);
				gtk_entry_set_input_purpose(GTK_ENTRY(m_textInput), GTK_INPUT_PURPOSE_FREE_FORM);
				gtk_widget_set_opacity(m_textInput, 0.0);
				gtk_widget_set_halign(m_textInput, GTK_ALIGN_START);
				gtk_widget_set_valign(m_textInput, GTK_ALIGN_START);
				gtk_widget_set_size_request(m_textInput, 2, 24);
				gtk_widget_set_no_show_all(m_textInput, TRUE);
				gtk_overlay_add_overlay(GTK_OVERLAY(m_overlay), m_textInput);
				gtk_widget_hide(m_textInput);
				g_signal_connect(m_textInput, "changed", G_CALLBACK(+[](GtkEditable* editable, gpointer data) {
									 auto& self = *static_cast<GtkWindowHost*>(data);
									 if (self.m_textInputUpdating || !self.m_inputHandler)
										 return;
									 const char* text = gtk_entry_get_text(GTK_ENTRY(editable));
									 const auto characterOffset = gtk_editable_get_position(editable);
									 const char* cursor = text ? g_utf8_offset_to_pointer(text, characterOffset) : nullptr;
									 self.m_inputHandler({.kind = NativeInputKind::TextComposition,
														  .text = text ? text : "",
														  .preedit = self.m_textPreedit,
														  .textCursor = text && cursor ? static_cast<std::uint32_t>(cursor - text) : 0,
														  .selectionLength = static_cast<std::uint32_t>(self.m_textPreedit.size()),
														  .textSequence = self.m_textInputSequence});
								 }),
								 this);
				g_signal_connect(m_textInput, "preedit-changed", G_CALLBACK(+[](GtkEntry*, gchar* preedit, gpointer data) {
									 auto& self = *static_cast<GtkWindowHost*>(data);
									 self.m_textPreedit = preedit ? preedit : "";
									 if (!self.m_textInputUpdating && self.m_inputHandler)
									 {
										 const char* text = gtk_entry_get_text(GTK_ENTRY(self.m_textInput));
										 const auto characterOffset = gtk_editable_get_position(GTK_EDITABLE(self.m_textInput));
										 const char* cursor = text ? g_utf8_offset_to_pointer(text, characterOffset) : nullptr;
										 self.m_inputHandler({.kind = NativeInputKind::TextComposition,
															  .text = text ? text : "",
															  .preedit = self.m_textPreedit,
															  .textCursor = text && cursor ? static_cast<std::uint32_t>(cursor - text) : 0,
															  .selectionLength = static_cast<std::uint32_t>(self.m_textPreedit.size()),
															  .textSequence = self.m_textInputSequence});
									 }
								 }),
								 this);
				g_signal_connect(m_textInput, "activate", G_CALLBACK(+[](GtkEntry* entry, gpointer data) {
									 auto& self = *static_cast<GtkWindowHost*>(data);
									 if (!self.m_inputHandler || !self.m_textInputSequence || !self.m_textPreedit.empty())
										 return;
									 const char* text = gtk_entry_get_text(entry);
									 self.m_inputHandler({.kind = NativeInputKind::TextAction,
														  .text = text ? text : "",
														  .textSequence = self.m_textInputSequence,
														  .textAccepted = true});
								 }),
								 this);
				g_signal_connect_after(m_textInput, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer data) -> gboolean {
										   auto& self = *static_cast<GtkWindowHost*>(data);
										   if (event->keyval != GDK_KEY_Escape || !self.m_textPreedit.empty() ||
											   !self.m_inputHandler || !self.m_textInputSequence)
											   return FALSE;
										   const char* text = gtk_entry_get_text(GTK_ENTRY(self.m_textInput));
										   self.m_inputHandler({.kind = NativeInputKind::TextAction,
																.text = text ? text : "",
																.textSequence = self.m_textInputSequence,
																.textAccepted = false});
										   return TRUE;
									   }),
									   this);
				gtk_box_pack_start(GTK_BOX(m_root), m_overlay, TRUE, TRUE, 0);
				gtk_container_add(GTK_CONTAINER(m_window), m_root);
				gtk_widget_realize(m_window);
				gtk_widget_realize(m_browserContainer);
			}

			// Chromium answers XSetInputFocus by activating its own X11 window, which
			// hands focus back to this container and re-enters the focus-in handler.
			// Re-asserting focus every time turns that exchange into a loop that pins
			// the UI thread at full speed, so focus is only taken when the browser
			// does not already hold it. Chromium focuses a descendant of the window it
			// handed us, so the whole chain up to the child counts as focused.
			bool BrowserOwnsInputFocus(Display* display) const
			{
				if (!display || !m_browserChild)
					return false;
				::Window focused{};
				int revert{};
				if (!XGetInputFocus(display, &focused, &revert) || focused == None ||
					focused == PointerRoot)
					return false;
				for (auto window = focused; window != None;)
				{
					if (window == m_browserChild)
						return true;
					::Window root{}, parent{}, *children{};
					unsigned count{};
					if (!XQueryTree(display, window, &root, &parent, &children, &count))
						return false;
					if (children)
						XFree(children);
					if (parent == None || parent == root)
						return false;
					window = parent;
				}
				return false;
			}

			Display* BrowserXDisplay() const
			{
				if (!m_browserContainer)
					return nullptr;
				auto* window = gtk_widget_get_window(m_browserContainer);
				if (!window || !GDK_IS_X11_WINDOW(window))
					return nullptr;
				return gdk_x11_display_get_xdisplay(gdk_window_get_display(window));
			}

			::Window BrowserParentXWindow() const
			{
				if (!m_browserContainer)
					return None;
				auto* window = gtk_widget_get_window(m_browserContainer);
				return window && GDK_IS_X11_WINDOW(window)
						   ? gdk_x11_window_get_xid(window)
						   : None;
			}

			void ReleasePointerGrab()
			{
				if (m_x11Grabbed)
				{
					auto* display = gdk_display_get_default();
					if (display && GDK_IS_X11_DISPLAY(display))
						XUngrabPointer(gdk_x11_display_get_xdisplay(display), CurrentTime);
				}
				if (m_grabbedSeat)
					gdk_seat_ungrab(m_grabbedSeat);
				m_x11Grabbed = false;
				m_grabbedSeat = nullptr;
				m_grabbedWidget = nullptr;
			}

			void NotifyMetrics()
			{
				if (m_metricsHandler && m_window)
					m_metricsHandler(GetMetrics());
			}

			GtkWidget* m_window{};
			GtkWidget* m_root{};
			GtkWidget* m_overlay{};
			GtkWidget* m_stack{};
			GtkWidget* m_browserContainer{};
			::Window m_browserChild{None};
			std::chrono::steady_clock::time_point m_focusAssertionWindowStart{};
			std::chrono::steady_clock::time_point m_focusAssertionBlockedUntil{};
			int m_focusAssertionCount{};

			GtkWidget* m_textInput{};
			std::unique_ptr<GtkPadRenderRegion> m_renderRegion;
			std::unique_ptr<GtkPadRenderRegion> m_padRenderRegion;
			CloseHandler m_closeHandler;
			GameCloseHandler m_gameCloseHandler;
			MetricsHandler m_metricsHandler;
			PadCloseHandler m_padCloseHandler;
			InputHandler m_inputHandler;
			std::uint64_t m_textInputSequence{};
			std::string m_textPreedit;
			bool m_textInputUpdating{};
			GdkSeat* m_grabbedSeat{};
			GtkWidget* m_grabbedWidget{};
			bool m_x11Grabbed{};
			bool m_padMetricsEnabled{};
			bool m_fullscreen{};
		};
	} // namespace

	std::unique_ptr<INativeWindowHost> CreateNativeWindowHost()
	{
		return std::make_unique<GtkWindowHost>();
	}
} // namespace WebFrontend

#endif
