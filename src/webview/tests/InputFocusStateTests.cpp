#include "webview/InputFocusState.h"
#include "webview/cef/CefOverlayInput.h"
#undef NDEBUG
#include <cassert>

using namespace WebFrontend;

int main()
{
	InputFocusState state;
	state.Observe(InputWindow::Launcher, false);
	assert(!state.Allows(InputWindow::Game));
	state.Observe(InputWindow::Game, true);
	assert(state.Allows(InputWindow::Game));
	assert(!state.Allows(InputWindow::GamePad));
	const auto revision = state.Revision();
	// A child focus change or a delayed callback reading current activation
	// leaves the input owner alone, without releasing held keys.
	assert(!state.Observe(InputWindow::Game, true));
	assert(state.Revision() == revision);
	for (auto destination : {InputWindow::Outside, InputWindow::Launcher, InputWindow::Tool,
							 InputWindow::GamePad})
	{
		state.Observe(destination, true);
		assert(!state.Allows(InputWindow::Game));
		assert(!state.ResumeClick(InputWindow::Game, 1, true, true));
		state.Observe(InputWindow::Game, true);
		assert(state.RenderActive()); // Waiting never means emulation must pause.
		assert(state.Waiting(InputWindow::Game));
		assert(!state.Allows(InputWindow::Game));
		assert(!state.ResumeClick(InputWindow::Game, 1, false, true));
		assert(!state.ResumeClick(InputWindow::Game, 1, true, false));
		assert(!state.ResumeClick(InputWindow::Game, 1, false, true));
		assert(!state.ResumeClick(InputWindow::Game, 1, true, true));
		assert(!state.ResumeClick(InputWindow::Game, 2, false, true));
		assert(!state.Allows(InputWindow::Game));
		assert(state.ResumeClick(InputWindow::Game, 1, false, true));
		assert(state.Allows(InputWindow::Game));
	}
	state.Observe(InputWindow::GamePad, true);
	assert(state.Waiting(InputWindow::GamePad));
	state.ResumeClick(InputWindow::GamePad, 1, true, true);
	// Losing focus halfway through a resume click invalidates its release.
	state.Observe(InputWindow::Outside, true);
	state.Observe(InputWindow::GamePad, true);
	assert(!state.ResumeClick(InputWindow::GamePad, 1, false, true));
	assert(state.Waiting(InputWindow::GamePad));
	state.ResumeClick(InputWindow::GamePad, 1, true, true);
	assert(state.ResumeClick(InputWindow::GamePad, 1, false, true));
	assert(state.Allows(InputWindow::GamePad));
	assert(!state.Allows(InputWindow::Game));
	state.Observe(InputWindow::Launcher, false);
	state.Observe(InputWindow::Game, true);
	assert(state.Allows(InputWindow::Game)); // A new title has no stale wait.
	assert(!state.FilterKey(1, 7, 26, true, false, true));
	state.Observe(InputWindow::Outside, true);
	state.Observe(InputWindow::Game, true);
	assert(state.FilterKey(1, 7, 26, true, true, false));
	state.ResumeClick(InputWindow::Game, 1, true, true);
	assert(state.ResumeClick(InputWindow::Game, 1, false, true));
	assert(state.FilterKey(1, 7, 26, true, true, true));
	assert(state.FilterKey(1, 7, 26, false, false, true));
	assert(!state.FilterKey(1, 7, 26, true, false, true));
	assert(!state.FilterKey(1, 7, 26, false, false, true));
	// An unknown repeat on reactivation also requires a fresh physical press.
	assert(state.FilterKey(2, 7, 26, true, true, true));
	using namespace WebFrontend::CefOverlay;
	const InputOwnership suspended{InputOwner::None, InputOwner::None, InputOwner::None};
	const InputOwnership webUi{InputOwner::WebUi, InputOwner::WebUi, InputOwner::WebUi, true};
	const auto padKeyboard = MainStreamOwnership(InputWindow::GamePad, suspended, webUi);
	assert(padKeyboard.keyboard == InputOwner::WebUi);
	assert(padKeyboard.text == InputOwner::WebUi && padKeyboard.webUiTextFocused);
	assert(padKeyboard.pointer == InputOwner::None);
	assert(MainStreamOwnership(InputWindow::Game, {}, suspended).keyboard == InputOwner::Title);
	assert(MainStreamOwnership(InputWindow::Tool, {}, webUi).keyboard == InputOwner::None);
}
