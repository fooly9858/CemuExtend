#pragma once

#include <array>
#include <cstdint>
#include <unordered_set>

namespace WebFrontend
{
	// OS activation is independent of the overlay's requested input owner.
	enum class InputWindow : std::uint8_t
	{
		Outside,
		Launcher,
		Game,
		GamePad,
		Tool
	};

	class InputFocusState
	{
	  public:
		bool Observe(InputWindow window, bool playing)
		{
			const auto previous = m_window;
			const bool changed = previous != window || m_playing != playing;
			if (!changed)
				return false;
			if (!playing)
				m_waiting = {};
			else if (m_playing && IsRender(previous) && previous != window)
				m_waiting[Index(previous)] = true;
			if (IsRender(previous) && previous != window)
				m_blockedKeys.insert(m_pressedKeys.begin(), m_pressedKeys.end());
			if (!playing)
			{
				m_pressedKeys.clear();
				m_blockedKeys.clear();
			}
			m_window = window;
			m_playing = playing;
			m_resumeDevice = 0;
			m_resumePressed = false;
			++m_revision;
			return true;
		}

		// A full left click is consumed, including its release. Key repeat, wheel,
		// raw motion and a release without a preceding press cannot resume input.
		bool ResumeClick(InputWindow window, std::uint16_t device, bool pressed, bool inside)
		{
			if (!Waiting(window))
				return false;
			if (pressed && inside && !m_resumePressed)
			{
				m_resumePressed = true;
				m_resumeDevice = device;
			}
			else if (!pressed && m_resumePressed && device == m_resumeDevice)
			{
				m_resumePressed = false;
				if (!inside)
					return false;
				m_waiting[Index(window)] = false;
				++m_revision;
				return true;
			}
			return false;
		}

		// Keep physical holds across activation changes, while releasing their
		// forwarded state. Repeats cannot revive a key held during click-to-resume.
		bool FilterKey(std::uint16_t device, std::uint16_t page, std::uint16_t usage,
					   bool pressed, bool repeat, bool allowed)
		{
			const auto key = (std::uint64_t(device) << 32) | (std::uint64_t(page) << 16) | usage;
			if (!pressed)
			{
				m_pressedKeys.erase(key);
				return m_blockedKeys.erase(key) != 0 || !allowed;
			}
			if (!allowed || (repeat && !m_pressedKeys.contains(key)))
				m_blockedKeys.insert(key);
			else if (!repeat)
				m_blockedKeys.erase(key); // A new physical press after an unobserved release.
			m_pressedKeys.insert(key);
			return m_blockedKeys.contains(key);
		}

		[[nodiscard]] bool Waiting(InputWindow window) const
		{
			return m_playing && IsRender(window) && m_window == window && m_waiting[Index(window)];
		}
		[[nodiscard]] bool Allows(InputWindow window) const
		{
			return m_playing && IsRender(window) && m_window == window && !Waiting(window);
		}
		[[nodiscard]] InputWindow Window() const
		{
			return m_window;
		}
		[[nodiscard]] bool RenderActive() const
		{
			return m_playing && IsRender(m_window);
		}
		[[nodiscard]] std::uint64_t Revision() const
		{
			return m_revision;
		}
		[[nodiscard]] static bool IsRender(InputWindow window)
		{
			return window == InputWindow::Game || window == InputWindow::GamePad;
		}

	  private:
		static unsigned Index(InputWindow window)
		{
			return window == InputWindow::GamePad ? 1 : 0;
		}
		InputWindow m_window{InputWindow::Outside};
		bool m_playing{};
		std::array<bool, 2> m_waiting{};
		bool m_resumePressed{};
		std::uint16_t m_resumeDevice{};
		std::uint64_t m_revision{};
		std::unordered_set<std::uint64_t> m_pressedKeys;
		std::unordered_set<std::uint64_t> m_blockedKeys;
	};
} // namespace WebFrontend
