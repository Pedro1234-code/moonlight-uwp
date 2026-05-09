//
// DirectXPage.xaml.cpp
// Implementation of the DirectXPage class.
//

#include "pch.h"
#include "StreamPage.xaml.h"
#include "../Streaming/FFMpegDecoder.h"
#include <Utils.hpp>
#include <KeyboardControl.xaml.h>
#include "../Common/ModalDialog.xaml.h"

using namespace moonlight_xbox_dx;

using namespace Platform;
using namespace Platform::Collections;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Devices::Input;
using namespace Windows::Gaming::Input;
using namespace Windows::Graphics::Display;
using namespace Windows::System::Threading;
using namespace Windows::UI::Core;
using namespace Windows::UI::Input;
using namespace Windows::UI::ViewManagement;
using namespace Windows::UI::ViewManagement::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Data;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;
using namespace concurrency;

namespace {
	const unsigned int MouseButtonLeftMask = 1u << 0;
	const unsigned int MouseButtonMiddleMask = 1u << 1;
	const unsigned int MouseButtonRightMask = 1u << 2;
	const unsigned int MouseButtonX1Mask = 1u << 3;
	const unsigned int MouseButtonX2Mask = 1u << 4;
}

StreamPage::StreamPage():
	m_windowVisible(true),
	m_coreInput(nullptr),
	m_mouseInputRegistered(false),
	m_mouseMovedRegistered(false),
	m_corePointerHandlersRegistered(false),
	m_panelPointerHandlersRegistered(false),
	m_mouseCaptureActive(false),
	m_mouseCursorHidden(false),
	m_mouseCaptureSuspended(false),
	m_mouseButtons(0)
{
	InitializeComponent();

	DisplayInformation^ currentDisplayInformation = DisplayInformation::GetForCurrentView();
	NavigationCacheMode = Windows::UI::Xaml::Navigation::NavigationCacheMode::Enabled;
	swapChainPanel->SizeChanged +=
		ref new SizeChangedEventHandler(this, &StreamPage::OnSwapChainPanelSizeChanged);
	m_deviceResources = std::make_shared<DX::DeviceResources>();
}



void StreamPage::OnBackRequested(Platform::Object^ e,Windows::UI::Core::BackRequestedEventArgs^ args)
{
	// UWP on Xbox One triggers a back request whenever the B
	// button is pressed which can result in the app being
	// suspended if unhandled
	args->Handled = true;
}

void StreamPage::Page_Loaded(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^ e) {

	this->m_progressView->Visibility = Windows::UI::Xaml::Visibility::Visible;
	this->m_progressRing->IsActive = true;

	auto navigation = Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
	m_back_cookie = navigation->BackRequested += ref new EventHandler<BackRequestedEventArgs ^>(this, &StreamPage::OnBackRequested);

	Windows::UI::ViewManagement::ApplicationView::GetForCurrentView()->SetDesiredBoundsMode(Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);

	keyDownHandler = (Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyDown += ref new Windows::Foundation::TypedEventHandler<Windows::UI::Core::CoreWindow ^, Windows::UI::Core::KeyEventArgs ^>(this, &StreamPage::OnKeyDown));
	keyUpHandler = (Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyUp += ref new Windows::Foundation::TypedEventHandler<Windows::UI::Core::CoreWindow ^, Windows::UI::Core::KeyEventArgs ^>(this, &StreamPage::OnKeyUp));
	RegisterMouseInput();

	// Detect gamepad connection and disconnection events
	gamepadAddedHandler = Gamepad::GamepadAdded += ref new EventHandler<Gamepad^>(this, &StreamPage::OnGamepadAdded);
	gamepadRemovedHandler = Gamepad::GamepadRemoved += ref new EventHandler<Gamepad ^>(this, &StreamPage::OnGamepadRemoved);

	try {
		m_deviceResources->SetSwapChainPanel(swapChainPanel);
	} catch (...) {
		Utils::Log("StreamPage::Page_Loaded: SetSwapChainPanel failed\n");
	}

	Platform::WeakReference weakThis(this);
	DISPATCH_UI([weakThis] {
		auto that = weakThis.Resolve<StreamPage>();
		if (that == nullptr) return;
		try {
			that->m_main = std::unique_ptr<moonlight_xbox_dxMain>(new moonlight_xbox_dxMain(that->m_deviceResources, that, new MoonlightClient(), that->configuration));
			that->m_main->CreateDeviceDependentResources();
			that->m_main->CreateWindowSizeDependentResources();
			that->m_main->StartRenderLoop();
			that->CaptureMouseInput();
        } catch (const std::exception &ex) {
			Utils::Logf("StreamPage::Page_Loaded: Exception when starting stream. Exception: %s", ex.what());
        } catch (const std::string &string) {
			Utils::Logf("StreamPage::Page_Loaded: Exception when starting stream. Exception: %s", string);
        } catch (Platform::Exception ^ e) {
            Platform::String ^ errorMsg = ref new Platform::String();
            errorMsg = errorMsg->Concat(L"Exception: ", e->Message);
            errorMsg = errorMsg->Concat(errorMsg, Utils::StringPrintf("%x", e->HResult));
			Utils::Logf("StreamPage::Page_Loaded: Exception when starting stream. Exception: %s", Utils::PlatformStringToStdString(errorMsg));
        } catch (...) {
            Utils::Log("StreamPage::Page_Loaded: Exception when starting stream. Exception: Generic Exception");
        }
	});
}

void StreamPage::Page_Unloaded(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^ e) {
	auto navigation = Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
	navigation->BackRequested -= m_back_cookie;

	Gamepad::GamepadAdded -= gamepadAddedHandler;
	Gamepad::GamepadRemoved -= gamepadRemovedHandler;
	UnregisterMouseInput();

	if (this->m_main) {

		Utils::Log("StreamPage::Page_Unloaded stopping m_main render loop\n");

		try {
			this->m_main->StopRenderLoop();
			this->m_main.reset();
		} catch (std::exception &ex) {
			Utils::Logf("StreamPage::Page_Unloaded m_main threw an exception: %s\n", ex.what());
		} catch (...) {
			Utils::Log("StreamPage::Page_Unloaded m_main threw an exception\n");
		}

		Utils::Log("StreamPage::Page_Unloaded m_main reset\n");
	}

	Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyDown -= keyDownHandler;
	Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyUp -= keyUpHandler;
}

StreamPage::~StreamPage()
{
}

void StreamPage::OnSwapChainPanelSizeChanged(Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ e)
{
	if (m_main == nullptr || m_deviceResources == nullptr)return;
	Utils::Logf("StreamPage::OnSwapChainPanelSizeChanged( NewSize: %f x %f )\n", e->NewSize.Width, e->NewSize.Height);
	critical_section::scoped_lock lock(m_main->GetCriticalSection());
	m_deviceResources->SetLogicalSize(e->NewSize);
	m_main->CreateDeviceDependentResources();
	m_main->CreateWindowSizeDependentResources();
}

void StreamPage::RegisterMouseInput()
{
	if (m_mouseInputRegistered) return;

	auto mouseDevice = MouseDevice::GetForCurrentView();
	if (mouseDevice != nullptr) {
		mouseMovedHandler = mouseDevice->MouseMoved +=
			ref new Windows::Foundation::TypedEventHandler<MouseDevice^, MouseEventArgs^>(this, &StreamPage::OnMouseMoved);
		m_mouseMovedRegistered = true;
	}

	auto window = CoreWindow::GetForCurrentThread();
	if (window != nullptr) {
		pointerMovedHandler = window->PointerMoved +=
			ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>([this](CoreWindow^ sender, PointerEventArgs^ args) {
				if (args != nullptr && HandlePointerMoved(args->CurrentPoint)) args->Handled = true;
			});
		pointerPressedHandler = window->PointerPressed +=
			ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>([this](CoreWindow^ sender, PointerEventArgs^ args) {
				if (args != nullptr && HandlePointerPressed(args->CurrentPoint)) args->Handled = true;
			});
		pointerReleasedHandler = window->PointerReleased +=
			ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>([this](CoreWindow^ sender, PointerEventArgs^ args) {
				if (args != nullptr && HandlePointerReleased(args->CurrentPoint)) args->Handled = true;
			});
		pointerExitedHandler = window->PointerExited +=
			ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>([this](CoreWindow^ sender, PointerEventArgs^ args) {
				if (args != nullptr && HandlePointerExited(args->CurrentPoint)) args->Handled = true;
			});
		pointerCaptureLostHandler = window->PointerCaptureLost +=
			ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>([this](CoreWindow^ sender, PointerEventArgs^ args) {
				if (args != nullptr && HandlePointerCaptureLost(args->CurrentPoint)) args->Handled = true;
			});
		pointerWheelChangedHandler = window->PointerWheelChanged +=
			ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>([this](CoreWindow^ sender, PointerEventArgs^ args) {
				if (args != nullptr && HandlePointerWheelChanged(args->CurrentPoint)) args->Handled = true;
			});
		m_corePointerHandlersRegistered = true;
	}


	if (swapChainPanel != nullptr) {
		panelPointerMovedHandler = swapChainPanel->PointerMoved +=
			ref new TypedEventHandler<Platform::Object^, PointerRoutedEventArgs^>([this](Platform::Object^ sender, PointerRoutedEventArgs^ args) {
				if (args != nullptr && HandlePointerMoved(args->GetCurrentPoint(swapChainPanel))) args->Handled = true;
			});
		panelPointerPressedHandler = swapChainPanel->PointerPressed +=
			ref new TypedEventHandler<Platform::Object^, PointerRoutedEventArgs^>([this](Platform::Object^ sender, PointerRoutedEventArgs^ args) {
				if (args != nullptr && HandlePointerPressed(args->GetCurrentPoint(swapChainPanel))) args->Handled = true;
			});
		panelPointerReleasedHandler = swapChainPanel->PointerReleased +=
			ref new TypedEventHandler<Platform::Object^, PointerRoutedEventArgs^>([this](Platform::Object^ sender, PointerRoutedEventArgs^ args) {
				if (args != nullptr && HandlePointerReleased(args->GetCurrentPoint(swapChainPanel))) args->Handled = true;
			});
		panelPointerExitedHandler = swapChainPanel->PointerExited +=
			ref new TypedEventHandler<Platform::Object^, PointerRoutedEventArgs^>([this](Platform::Object^ sender, PointerRoutedEventArgs^ args) {
				if (args != nullptr && HandlePointerExited(args->GetCurrentPoint(swapChainPanel))) args->Handled = true;
			});
		panelPointerCaptureLostHandler = swapChainPanel->PointerCaptureLost +=
			ref new TypedEventHandler<Platform::Object^, PointerRoutedEventArgs^>([this](Platform::Object^ sender, PointerRoutedEventArgs^ args) {
				if (args != nullptr && HandlePointerCaptureLost(args->GetCurrentPoint(swapChainPanel))) args->Handled = true;
			});
		panelPointerWheelChangedHandler = swapChainPanel->PointerWheelChanged +=
			ref new TypedEventHandler<Platform::Object^, PointerRoutedEventArgs^>([this](Platform::Object^ sender, PointerRoutedEventArgs^ args) {
				if (args != nullptr && HandlePointerWheelChanged(args->GetCurrentPoint(swapChainPanel))) args->Handled = true;
			});
		m_panelPointerHandlersRegistered = true;
	}

	m_mouseInputRegistered = true;
}

void StreamPage::UnregisterMouseInput()
{
	if (!m_mouseInputRegistered) return;

	ReleaseMouseInputCapture();

	auto mouseDevice = MouseDevice::GetForCurrentView();
	if (m_mouseMovedRegistered && mouseDevice != nullptr) {
		mouseDevice->MouseMoved -= mouseMovedHandler;
	}
	m_mouseMovedRegistered = false;

	auto window = CoreWindow::GetForCurrentThread();
	if (m_corePointerHandlersRegistered && window != nullptr) {
		window->PointerMoved -= pointerMovedHandler;
		window->PointerPressed -= pointerPressedHandler;
		window->PointerReleased -= pointerReleasedHandler;
		window->PointerExited -= pointerExitedHandler;
		window->PointerCaptureLost -= pointerCaptureLostHandler;
		window->PointerWheelChanged -= pointerWheelChangedHandler;
	}
	m_corePointerHandlersRegistered = false;
	if (m_panelPointerHandlersRegistered && swapChainPanel != nullptr) {
		swapChainPanel->PointerMoved -= panelPointerMovedHandler;
		swapChainPanel->PointerPressed -= panelPointerPressedHandler;
		swapChainPanel->PointerReleased -= panelPointerReleasedHandler;
		swapChainPanel->PointerExited -= panelPointerExitedHandler;
		swapChainPanel->PointerCaptureLost -= panelPointerCaptureLostHandler;
		swapChainPanel->PointerWheelChanged -= panelPointerWheelChangedHandler;
	}
	m_panelPointerHandlersRegistered = false;

	m_mouseInputRegistered = false;
	m_mouseCaptureSuspended = false;
}

void StreamPage::CaptureMouseInput()
{
	if (m_mouseCaptureSuspended) return;

	auto window = CoreWindow::GetForCurrentThread();
	if (window == nullptr) return;

	if (!m_mouseCursorHidden) {
		window->PointerCursor = nullptr;
		m_mouseCursorHidden = true;
	}

	if (!m_mouseCaptureActive) {
		window->SetPointerCapture();
		m_mouseCaptureActive = true;
	}
}

void StreamPage::ReleaseMouseInputCapture()
{
	ReleaseMouseButtons();

	auto window = CoreWindow::GetForCurrentThread();
	if (window == nullptr) {
		m_mouseCaptureActive = false;
		m_mouseCursorHidden = false;
		return;
	}

	if (m_mouseCaptureActive) {
		window->ReleasePointerCapture();
		m_mouseCaptureActive = false;
	}

	if (m_mouseCursorHidden) {
		window->PointerCursor = ref new CoreCursor(CoreCursorType::Arrow, 0);
		m_mouseCursorHidden = false;
	}
}

bool StreamPage::IsMousePointer(PointerPoint^ point)
{
	return point != nullptr &&
	       point->PointerDevice != nullptr &&
	       point->PointerDevice->PointerDeviceType == Windows::Devices::Input::PointerDeviceType::Mouse;
}

unsigned int StreamPage::GetMouseButtonMask(PointerPoint^ point)
{
	if (point == nullptr || point->Properties == nullptr) return 0;

	unsigned int mask = 0;
	auto properties = point->Properties;
	if (properties->IsLeftButtonPressed) mask |= MouseButtonLeftMask;
	if (properties->IsMiddleButtonPressed) mask |= MouseButtonMiddleMask;
	if (properties->IsRightButtonPressed) mask |= MouseButtonRightMask;
	if (properties->IsXButton1Pressed) mask |= MouseButtonX1Mask;
	if (properties->IsXButton2Pressed) mask |= MouseButtonX2Mask;
	return mask;
}

void StreamPage::UpdateMouseButtonState(PointerPoint^ point)
{
	unsigned int nextButtons = GetMouseButtonMask(point);
	unsigned int changedButtons = m_mouseButtons ^ nextButtons;

	if (m_main != nullptr) {
		if ((changedButtons & MouseButtonLeftMask) != 0) {
			(nextButtons & MouseButtonLeftMask) != 0 ? m_main->OnMouseButtonDown(BUTTON_LEFT) : m_main->OnMouseButtonUp(BUTTON_LEFT);
		}
		if ((changedButtons & MouseButtonMiddleMask) != 0) {
			(nextButtons & MouseButtonMiddleMask) != 0 ? m_main->OnMouseButtonDown(BUTTON_MIDDLE) : m_main->OnMouseButtonUp(BUTTON_MIDDLE);
		}
		if ((changedButtons & MouseButtonRightMask) != 0) {
			(nextButtons & MouseButtonRightMask) != 0 ? m_main->OnMouseButtonDown(BUTTON_RIGHT) : m_main->OnMouseButtonUp(BUTTON_RIGHT);
		}
		if ((changedButtons & MouseButtonX1Mask) != 0) {
			(nextButtons & MouseButtonX1Mask) != 0 ? m_main->OnMouseButtonDown(BUTTON_X1) : m_main->OnMouseButtonUp(BUTTON_X1);
		}
		if ((changedButtons & MouseButtonX2Mask) != 0) {
			(nextButtons & MouseButtonX2Mask) != 0 ? m_main->OnMouseButtonDown(BUTTON_X2) : m_main->OnMouseButtonUp(BUTTON_X2);
		}
	}

	m_mouseButtons = nextButtons;
}

void StreamPage::ReleaseMouseButtons()
{
	if (m_main != nullptr) {
		if ((m_mouseButtons & MouseButtonLeftMask) != 0) m_main->OnMouseButtonUp(BUTTON_LEFT);
		if ((m_mouseButtons & MouseButtonMiddleMask) != 0) m_main->OnMouseButtonUp(BUTTON_MIDDLE);
		if ((m_mouseButtons & MouseButtonRightMask) != 0) m_main->OnMouseButtonUp(BUTTON_RIGHT);
		if ((m_mouseButtons & MouseButtonX1Mask) != 0) m_main->OnMouseButtonUp(BUTTON_X1);
		if ((m_mouseButtons & MouseButtonX2Mask) != 0) m_main->OnMouseButtonUp(BUTTON_X2);
	}

	m_mouseButtons = 0;
}

void StreamPage::OnMouseMoved(MouseDevice^ sender, MouseEventArgs^ args)
{
	if (m_main == nullptr || args == nullptr || m_mouseCaptureSuspended) return;

	auto delta = args->MouseDelta;
	if (delta.X == 0 && delta.Y == 0) return;

	CaptureMouseInput();
	m_main->OnMouseMove(delta.X, delta.Y);
}

bool StreamPage::HandlePointerMoved(PointerPoint^ point)
{
	if (m_main == nullptr || m_mouseCaptureSuspended || !IsMousePointer(point)) return false;

	CaptureMouseInput();
	UpdateMouseButtonState(point);
	return true;
}

bool StreamPage::HandlePointerPressed(PointerPoint^ point)
{
	if (m_main == nullptr || m_mouseCaptureSuspended || !IsMousePointer(point)) return false;

	CaptureMouseInput();
	UpdateMouseButtonState(point);
	return true;
}

bool StreamPage::HandlePointerReleased(PointerPoint^ point)
{
	if (m_main == nullptr || m_mouseCaptureSuspended || !IsMousePointer(point)) return false;

	CaptureMouseInput();
	UpdateMouseButtonState(point);
	return true;
}

bool StreamPage::HandlePointerExited(PointerPoint^ point)
{
	if (m_main == nullptr || m_mouseCaptureSuspended || !IsMousePointer(point)) return false;

	CaptureMouseInput();
	return true;
}

bool StreamPage::HandlePointerCaptureLost(PointerPoint^ point)
{
	if (m_mouseCaptureSuspended || !IsMousePointer(point)) return false;

	m_mouseCaptureActive = false;
	ReleaseMouseButtons();
	if (m_mouseCursorHidden) {
		auto window = CoreWindow::GetForCurrentThread();
		if (window != nullptr) {
			window->PointerCursor = ref new CoreCursor(CoreCursorType::Arrow, 0);
		}
		m_mouseCursorHidden = false;
	}
	return true;
}

bool StreamPage::HandlePointerWheelChanged(PointerPoint^ point)
{
	if (m_mouseCaptureSuspended || !IsMousePointer(point) || m_main == nullptr) return false;

	int delta = point->Properties->MouseWheelDelta;
	if (delta == 0) return false;

	CaptureMouseInput();
	m_main->OnMouseWheel(delta, point->Properties->IsHorizontalMouseWheel);
	return true;
}

void StreamPage::flyoutButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	m_mouseCaptureSuspended = true;
	ReleaseMouseInputCapture();
	Windows::UI::Xaml::Controls::Flyout::ShowAttachedFlyout((FrameworkElement^)sender);
	if (m_main != nullptr) m_main->SetFlyoutOpened(true);
}


void StreamPage::ActionsFlyout_Closed(Platform::Object^ sender, Platform::Object^ e)
{
	if(m_main != nullptr) m_main->SetFlyoutOpened(false);
	m_mouseCaptureSuspended = false;
	CaptureMouseInput();
}

void StreamPage::toggleMouseButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	SetMouseMode(!this->m_mouseMode);
}

void StreamPage::SetMouseMode(bool enabled)
{
	this->MouseMode = enabled;
	if (m_main) m_main->mouseMode = this->MouseMode;
}

void StreamPage::showKeyboardButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	if (!m_main) return;
	if (GetApplicationState()->EnableKeyboard) {
		m_main->keyboardMode = true;

		this->Dispatcher->RunAsync(
			Windows::UI::Core::CoreDispatcherPriority::Normal,
			ref new Windows::UI::Core::DispatchedHandler([this]() {
				this->m_keyboardView->Visibility = Windows::UI::Xaml::Visibility::Visible;
			}));
	} else {
		CoreInputView::GetForCurrentView()->TryShow(CoreInputViewKind::Keyboard);
	}
}

void StreamPage::toggleLogsButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	bool isVisible = m_main->ToggleLogs();
	SetShowLogs(isVisible);
}

void StreamPage::SetShowLogs(bool enabled) {
	this->ShowLogs = enabled;
}

void StreamPage::toggleStatsButton_Click(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^ e) {
	bool isVisible = m_main->ToggleStats();
	SetShowStats(isVisible);
}

void StreamPage::SetShowStats(bool enabled) {
	this->ShowStats = enabled;
}

void StreamPage::OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) {

	configuration = dynamic_cast<StreamConfiguration^>(e->Parameter);
	SetStreamConfig(configuration);

	if (configuration == nullptr)return;

	SetMouseMode(false);
	SetShowLogs(false);
	SetShowStats(configuration->enableStats);

}

void StreamPage::disonnectButton_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	ReleaseMouseInputCapture();
	Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyDown -= keyDownHandler;
	Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyUp -= keyUpHandler;

	// trigger the server disconnected flow
	this->m_main->moonlightClient->SetConnectionTerminated();
}

void StreamPage::OnKeyDown(Windows::UI::Core::CoreWindow^ sender, Windows::UI::Core::KeyEventArgs^ e)
{
	//Ignore Gamepad input
	if (e->VirtualKey >= Windows::System::VirtualKey::GamepadA && e->VirtualKey <= Windows::System::VirtualKey::GamepadRightThumbstickLeft) {
		return;
	}
	char modifiers = 0;
	modifiers |= CoreWindow::GetForCurrentThread()->GetKeyState(Windows::System::VirtualKey::Control) == (CoreVirtualKeyStates::Down) ? MODIFIER_CTRL : 0;
	modifiers |= CoreWindow::GetForCurrentThread()->GetKeyState(Windows::System::VirtualKey::Menu) == (CoreVirtualKeyStates::Down) ? MODIFIER_ALT : 0;
	modifiers |= CoreWindow::GetForCurrentThread()->GetKeyState(Windows::System::VirtualKey::Shift) == (CoreVirtualKeyStates::Down) ? MODIFIER_SHIFT : 0;
	this->m_main->OnKeyDown((unsigned short)e->VirtualKey,modifiers);
}


void StreamPage::OnKeyUp(Windows::UI::Core::CoreWindow^ sender, Windows::UI::Core::KeyEventArgs^ e)
{
	//Ignore Gamepad input
	if (e->VirtualKey >= Windows::System::VirtualKey::GamepadA && e->VirtualKey <= Windows::System::VirtualKey::GamepadRightThumbstickLeft) {
		return;
	}
	char modifiers = 0;
	modifiers |= CoreWindow::GetForCurrentThread()->GetKeyState(Windows::System::VirtualKey::Control) == (CoreVirtualKeyStates::Down) ? MODIFIER_CTRL : 0;
	modifiers |= CoreWindow::GetForCurrentThread()->GetKeyState(Windows::System::VirtualKey::Menu) == (CoreVirtualKeyStates::Down) ? MODIFIER_ALT : 0;
	modifiers |= CoreWindow::GetForCurrentThread()->GetKeyState(Windows::System::VirtualKey::Shift) == (CoreVirtualKeyStates::Down) ? MODIFIER_SHIFT : 0;
	this->m_main->OnKeyUp((unsigned short) e->VirtualKey, modifiers);

}

void StreamPage::disconnectAndCloseButton_Click(Platform::Object ^ sender, Windows::UI::Xaml::RoutedEventArgs ^ e) {
	ReleaseMouseInputCapture();
	Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyDown -= keyDownHandler;
	Windows::UI::Core::CoreWindow::GetForCurrentThread()->KeyUp -= keyUpHandler;
	if (this->m_main) {
		// trigger the server disconnected flow which will cleanly exit the loop and call StopRenderLoop()
		this->m_main->moonlightClient->SetConnectionTerminated();
	}

	auto that = this;

	auto progressToken = ::moonlight_xbox_dx::ModalDialog::ShowProgressDialogToken(nullptr, Utils::StringFromStdString("Closing..."));

	concurrency::create_task(concurrency::create_async([that, progressToken]() {
		try {
			if (that->m_main) {
				that->m_main->CloseApp();
			}
		} catch (...) {
		}
	})).then([that, progressToken](concurrency::task<void> t) {
		try {
			t.get();

			// UI is sent back to HostSelectorPage in StartRenderLoop(), after the loop exits
			// All we need to do is close the progress dialog

			DISPATCH_UI([progressToken] {
				::moonlight_xbox_dx::ModalDialog::HideDialogByToken(progressToken);
			});
		} catch (...) {
		}
	});
}

void StreamPage::Keyboard_OnKeyDown(KeyboardControl^ sender, KeyEvent^ e)
{
	this->m_main->OnKeyDown(e->VirtualKey, e->Modifiers);
}


void StreamPage::Keyboard_OnKeyUp(KeyboardControl^ sender, KeyEvent^ e)
{
	this->m_main->OnKeyUp(e->VirtualKey, e->Modifiers);
}


void StreamPage::guideButtonShort_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	this->m_main->SendGuideButton(500);
}


void StreamPage::guideButtonLong_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	this->m_main->SendGuideButton(3000);
}


void StreamPage::toggleHDR_WinAltB_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	this->m_main->SendWinAltB();
}

void StreamPage::resetDecoder_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	LiRequestIdrFrame();
}

void StreamPage::toggleFramePacing_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e)
{
	// thread safe atomic bool
	bool isImmediate = Pacer::instance().getPacingImmediate();
	Pacer::instance().setPacingImmediate(isImmediate ? false : true);
}

void StreamPage::OnPropertyChanged(Platform::String^ propertyName)
{
	PropertyChanged(this, ref new Windows::UI::Xaml::Data::PropertyChangedEventArgs(propertyName));
}

void StreamPage::OnGamepadAdded(Platform::Object^ sender, Gamepad^ gamepad)
{
	m_refreshGamepads.store(true, std::memory_order_release);
}

void StreamPage::OnGamepadRemoved(Platform::Object^ sender, Gamepad^ gamepad)
{
	m_refreshGamepads.store(true, std::memory_order_release);
}

bool StreamPage::ShouldRefreshGamepads() {
	return m_refreshGamepads.exchange(false);
}

void StreamPage::RequestRefreshGamepads() {
	m_refreshGamepads.store(true, std::memory_order_release);
}

