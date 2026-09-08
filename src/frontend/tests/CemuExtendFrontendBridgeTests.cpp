#include "frontend/CemuExtendFrontendBridge.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

int main()
{
	static_assert(static_cast<std::uint8_t>(Frontend::CemuExtendPointerMode::Default) == 0);
	static_assert(static_cast<std::uint8_t>(
					  Frontend::CemuExtendPointerMode::VisibleAbsolute) == 1);
	static_assert(static_cast<std::uint8_t>(
					  Frontend::CemuExtendPointerMode::HiddenAbsolute) == 2);
	static_assert(static_cast<std::uint8_t>(
					  Frontend::CemuExtendPointerMode::CapturedRelative) == 3);
	static_assert(static_cast<std::uint8_t>(
					  Frontend::CemuExtendPointerCursor::Arrow) == 0);
	static_assert(static_cast<std::uint8_t>(
					  Frontend::CemuExtendPointerCursor::TextInput) == 1);
	static_assert(static_cast<std::uint8_t>(
					  Frontend::CemuExtendPointerCursor::ResizeAll) == 2);
	static_assert(static_cast<std::uint8_t>(
					  Frontend::CemuExtendPointerCursor::ResizeNS) == 3);
	static_assert(static_cast<std::uint8_t>(
					  Frontend::CemuExtendPointerCursor::ResizeEW) == 4);
	static_assert(static_cast<std::uint8_t>(
					  Frontend::CemuExtendPointerCursor::ResizeNESW) == 5);
	static_assert(static_cast<std::uint8_t>(
					  Frontend::CemuExtendPointerCursor::ResizeNWSE) == 6);
	static_assert(static_cast<std::uint8_t>(
					  Frontend::CemuExtendPointerCursor::Hand) == 7);
	static_assert(static_cast<std::uint8_t>(
					  Frontend::CemuExtendPointerCursor::NotAllowed) == 8);
	static_assert(static_cast<std::uint32_t>(
					  Frontend::CemuExtendMouseButton::Left) == 1U << 0U);
	static_assert(static_cast<std::uint32_t>(
					  Frontend::CemuExtendMouseButton::Right) == 1U << 1U);
	static_assert(static_cast<std::uint32_t>(
					  Frontend::CemuExtendMouseButton::Middle) == 1U << 2U);
	static_assert(static_cast<std::uint32_t>(
					  Frontend::CemuExtendMouseButton::X1) == 1U << 3U);
	static_assert(static_cast<std::uint32_t>(
					  Frontend::CemuExtendMouseButton::X2) == 1U << 4U);
	static_assert(static_cast<std::uint8_t>(
					  Frontend::CemuExtendMouseEventFlag::None) == 0);
	static_assert(static_cast<std::uint8_t>(
					  Frontend::CemuExtendMouseEventFlag::RawRelative) == 1U << 0U);

	Frontend::CemuExtendFrontendBridge bridge;
	const auto visible = bridge.ApplyPointerPolicy(1, 7, 1U << 2U, true, true);
	assert(visible.ownsPointer && visible.showCursor && visible.confine);
	assert(!bridge.RawMouseRequested());
	const auto inactive = bridge.ApplyPointerPolicy(1, 7, 1U << 2U, false, true);
	assert(inactive.ownsPointer && !inactive.confine);
	const auto noCanvas = bridge.ApplyPointerPolicy(3, 0, 0, true, false);
	assert(!noCanvas.ownsPointer && noCanvas.showCursor &&
		   !noCanvas.enteringCapture && !noCanvas.requestRawMouse);
	const auto captured = bridge.ApplyPointerPolicy(3, 0, 0, true, true);
	assert(captured.enteringCapture && captured.requestRawMouse && !captured.showCursor);
	// The synthetic event that caused capture is suppressed.
	auto motion = bridge.UpdatePosition({100, 100}, {50, 50}, false);
	assert(motion.delta == Frontend::CemuExtendPoint{});
	motion = bridge.UpdatePosition({60, 45}, {50, 50}, false);
	assert((motion.delta == Frontend::CemuExtendPoint{10, -5}));
	bridge.MarkRawMouseSeen();
	motion = bridge.UpdatePosition({61, 45}, {50, 50}, true);
	assert(motion.rawRelative && motion.delta == Frontend::CemuExtendPoint{});

	auto buttons = bridge.UpdateButtons(Frontend::CemuExtendMouseTransition::Down, 1);
	assert(buttons.buttons == 1 && buttons.changed == 1);
	buttons = bridge.UpdateButtons(Frontend::CemuExtendMouseTransition::Aggregate, 1, 0);
	assert(buttons.buttons == 0 && buttons.changed == 1);
	buttons = bridge.UpdateButtons(Frontend::CemuExtendMouseTransition::Down, 1);
	assert(buttons.buttons == 1 && buttons.changed == 1);
	buttons = bridge.UpdateButtons(Frontend::CemuExtendMouseTransition::Down, 1);
	assert(buttons.buttons == 1 && buttons.changed == 0);
	buttons = bridge.UpdateButtons(Frontend::CemuExtendMouseTransition::Up, 1);
	assert(buttons.buttons == 0 && buttons.changed == 1);
	buttons = bridge.UpdateButtons(Frontend::CemuExtendMouseTransition::Down, 1, 0, 11);
	assert(buttons.buttons == 1 && buttons.deviceButtons == 1 && buttons.changed == 1);
	buttons = bridge.UpdateButtons(Frontend::CemuExtendMouseTransition::Down, 1, 0, 22);
	assert(buttons.buttons == 1 && buttons.deviceButtons == 1 && buttons.changed == 1);
	buttons = bridge.UpdateButtons(Frontend::CemuExtendMouseTransition::Up, 1, 0, 11);
	assert(buttons.buttons == 1 && buttons.deviceButtons == 0 && buttons.changed == 1);
	buttons = bridge.UpdateButtons(Frontend::CemuExtendMouseTransition::Up, 1, 0, 22);
	assert(buttons.buttons == 0 && buttons.deviceButtons == 0 && buttons.changed == 1);
	assert(bridge.NormalizeWheel(60, 120, false) == 0);
	assert(bridge.NormalizeWheel(60, 120, false) == 1);
	assert(bridge.NormalizeWheel(-60, 120, true) == 0);
	assert(bridge.NormalizeWheel(-60, 120, true) == -1);

	const auto released = bridge.ApplyPointerPolicy(0, 0, 0, true, true);
	assert(released.leavingPolicy && !released.ownsPointer && released.showCursor);
	// UI hit testing must use absolute native coordinates after releasing capture.
	assert(!bridge.RawMouseRequested());
	const auto recaptured = bridge.ApplyPointerPolicy(1, 0, 0, true, true);
	assert(recaptured.ownsPointer);
	motion = bridge.UpdatePosition({10, 10}, {}, false);
	assert(motion.delta == Frontend::CemuExtendPoint{});
	motion = bridge.UpdatePosition({13, 8}, {}, false);
	assert((motion.delta == Frontend::CemuExtendPoint{3, -2}));

	assert(bridge.BeginTextInput(7));
	assert(!bridge.BeginTextInput(7));
	bridge.SetPreedit("abc");
	assert(!bridge.CanSubmitText());
	const auto composition = bridge.ComposeText("committed", 4);
	assert(composition.preedit == "abc" && composition.selectionLength == 3);
	assert(bridge.BeginTextInput(8));
	assert(bridge.CanSubmitText() && !bridge.HasPreedit());
	bridge.EndTextInput();
	assert(bridge.CanSubmitText() && bridge.TextInputSequence() == 0);
}
