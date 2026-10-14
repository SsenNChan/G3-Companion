#include "pch.h"
#include "../localization.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include <winrt/Microsoft.UI.Windowing.h>
#include <microsoft.ui.xaml.window.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.Globalization.NumberFormatting.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.System.Threading.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cwchar>
#include <memory>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Shapes;
using namespace winrt::Windows::Foundation;

namespace
{
    LRESULT CALLBACK HideNativeToolTip(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR subclassId,DWORD_PTR)
    {
        if(message==WM_WINDOWPOSCHANGING){
            auto position=reinterpret_cast<WINDOWPOS*>(lparam);
            position->flags=(position->flags&~SWP_SHOWWINDOW)|SWP_HIDEWINDOW;
        }
        if(message==WM_NCDESTROY)RemoveWindowSubclass(hwnd,HideNativeToolTip,subclassId);
        return DefSubclassProc(hwnd,message,wparam,lparam);
    }
    BOOL CALLBACK SuppressNativeToolTip(HWND hwnd,LPARAM)
    {
        wchar_t className[128]{};
        if(GetClassNameW(hwnd,className,ARRAYSIZE(className))&&wcscmp(className,TOOLTIPS_CLASSW)==0)
            SetWindowSubclass(hwnd,HideNativeToolTip,1,0);
        return TRUE;
    }
    LRESULT CALLBACK NativeToolTipCreationHook(int code,WPARAM wparam,LPARAM lparam)
    {
        if(code==HCBT_CREATEWND)SuppressNativeToolTip(reinterpret_cast<HWND>(wparam),0);
        return CallNextHookEx(nullptr,code,wparam,lparam);
    }
    winrt::Windows::UI::Color StageColor(std::uint32_t value)
    {
        return { 255, static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value >> 16) };
    }
    std::uint32_t StageValue(winrt::Windows::UI::Color color)
    {
        return color.R | (static_cast<std::uint32_t>(color.G) << 8) | (static_cast<std::uint32_t>(color.B) << 16);
    }
    GridLength AutoLength() { return { 1, GridUnitType::Auto }; }
    GridLength StarLength() { return { 1, GridUnitType::Star }; }
    std::wstring UsagePercent(double value)
    {
        value=std::round(value);
        wchar_t text[40]{};
        std::swprintf(text,ARRAYSIZE(text),L"%.0f%%",value);
        return text;
    }
    bool SameUsage(BatteryUsage::Report const& left,BatteryUsage::Report const& right)
    {
        if(left.error!=right.error || left.average!=right.average || left.averageDays!=right.averageDays)return false;
        for(size_t index=0;index<BatteryUsage::DisplayDays;++index){
            auto const& a=left.days[index];auto const& b=right.days[index];
            if(a.date.wYear!=b.date.wYear || a.date.wMonth!=b.date.wMonth || a.date.wDay!=b.date.wDay ||
                a.hasReading!=b.hasReading || a.consumed!=b.consumed)return false;
        }
        return true;
    }
    bool ReadNumber(NumberBox const& box, int& value)
    {
        std::wstring text(box.Text().c_str());
        wchar_t* end = nullptr;
        double number = std::wcstod(text.c_str(), &end);
        if (text.empty() || !end || end == text.c_str() || *end || !std::isfinite(number) ||
            number < 100 || number > 12000 || std::floor(number) != number) return false;
        value = static_cast<int>(number);
        return true;
    }
}

namespace winrt::G3Companion::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
        RootGrid().Language(Localization::LanguageTag());
        ScaleTransform cursorGeometryScale;
        cursorGeometryScale.ScaleX(1.2);cursorGeometryScale.ScaleY(1.2);
        cursorGeometryScale.CenterX(10);cursorGeometryScale.CenterY(10);
        ButtonsCursorIcon().Data().Transform(cursorGeometryScale);
        Title(L"G3 Companion");
        ExtendsContentIntoTitleBar(true);
        AppWindow().TitleBar().PreferredHeightOption(Microsoft::UI::Windowing::TitleBarHeightOption::Standard);
        SetTitleBar(ManagerTitleBar());
        check_hresult(get_strong().as<::IWindowNative>()->get_WindowHandle(&m_windowHandle));
        const auto module=GetModuleHandleW(nullptr);
        const auto windowDpi=GetDpiForWindow(m_windowHandle);
        const auto largeIcon=LoadImageW(module,MAKEINTRESOURCEW(1),IMAGE_ICON,
            GetSystemMetricsForDpi(SM_CXICON,windowDpi),GetSystemMetricsForDpi(SM_CYICON,windowDpi),LR_SHARED);
        const auto smallIcon=LoadImageW(module,MAKEINTRESOURCEW(1),IMAGE_ICON,
            GetSystemMetricsForDpi(SM_CXSMICON,windowDpi),GetSystemMetricsForDpi(SM_CYSMICON,windowDpi),LR_SHARED);
        SendMessageW(m_windowHandle,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(largeIcon));
        SendMessageW(m_windowHandle,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(smallIcon));
        // Windows caption tooltips are native HWNDs outside the XAML resource tree.
        // The hook only observes this manager UI thread's tooltip windows.
        m_tooltipCreationHook=SetWindowsHookExW(WH_CBT,NativeToolTipCreationHook,nullptr,GetCurrentThreadId());
        check_bool(m_tooltipCreationHook!=nullptr);
        EnumThreadWindows(GetCurrentThreadId(),SuppressNativeToolTip,0);
        UpdateBackdropTheme();
        m_completionStatusTimer = DispatcherQueue().CreateTimer();
        m_completionStatusTimer.Interval(std::chrono::seconds(3));
        m_completionStatusTimer.IsRepeating(false);
        auto weak = get_weak();
        m_completionStatusTimer.Tick([weak](auto const&, auto const&)
        {
            if (auto self = weak.get(); self && !self->m_closed && (self->m_completionStatus == Localization::Get(Localization::T001) || self->m_completionStatus == Localization::Get(Localization::T002)))
            {
                self->m_completionStatus.clear(); self->UpdateStatus();
            }
        });
        CreateStageRows();
        CreateButtonRows();
        InitializeMacroEditor();
        InitializeClockCalibration();
        SYSTEMTIME usageToday{};GetLocalTime(&usageToday);m_usage=BatteryUsage::Empty(usageToday);
        CreateUsageChart();
        RenderUsage();
        m_usageTimer=DispatcherQueue().CreateTimer();m_usageTimer.Interval(std::chrono::minutes(1));
        m_usageTimer.Tick([weak](auto const&,auto const&){if(auto self=weak.get();self && !self->m_closed)self->RefreshUsage();});
        Navigation().SelectedItem(GeneralItem());
        Populate();
        UpdateBattery();
        UpdateStatusPresentation();
        RootGrid().ActualThemeChanged([this](FrameworkElement const&, IInspectable const&) { UpdateBackdropTheme(); UpdateBattery(); UpdateStatusPresentation(); UpdateButtonLabels(true); RenderMacroSteps(); });
        AppWindow().Closing([this](Microsoft::UI::Windowing::AppWindow const&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args)
        {
            if (m_allowClose) return;
            if(m_clockCalibrating||m_clockRecoveryPending){args.Cancel(true);if(m_clockCalibrating)CancelClockCalibration();else m_clockDialogTask=ShowClockCalibration(false,true);return;}
            if(m_buttonResetDialog){CancelButtonReset();args.Cancel(true);return;}
            if (m_applying)
            {
                args.Cancel(true);
                UpdateStatus();
            }
            else if (m_dirty || MacroHasChanges() || m_macroRecording)
            {
                args.Cancel(true);
                if (!m_closePending) ConfirmClose();
            }
        });
        Closed([this](IInspectable const&, WindowEventArgs const&)
        {
            m_closed = true;
            m_usageReader->canceled=true;
            *m_macroLoadCanceled=true;
            StopMacroRecording();
            StopClockInput();
            CancelButtonReset();
            if(m_parameterReadFailureDialog)m_parameterReadFailureDialog.Hide();
            if (m_statusAnimation) m_statusAnimation.Stop();
            if (m_completionStatusTimer) m_completionStatusTimer.Stop();
            if (m_usageTimer) m_usageTimer.Stop();
            if(m_tooltipCreationHook){UnhookWindowsHookEx(m_tooltipCreationHook);m_tooltipCreationHook=nullptr;}
            if (m_closeHandler) m_closeHandler();
        });
        Activated([this](auto const&, WindowActivatedEventArgs const& args){
            if(args.WindowActivationState()!=WindowActivationState::Deactivated && m_monitoringConfigured)UpdateMonitoringControls();
        });
        check_bool(SetWindowSubclass(m_windowHandle,BackdropWindowProcedure,1,reinterpret_cast<DWORD_PTR>(this)));
        InvalidateRect(m_windowHandle,nullptr,TRUE);
    }

    MainWindow::~MainWindow()
    {
        if(m_macroKeyboardHook)UnhookWindowsHookEx(m_macroKeyboardHook);
        if(m_macroMouseHook)UnhookWindowsHookEx(m_macroMouseHook);
        if(s_macroRecorder==this)s_macroRecorder=nullptr;
        if(m_tooltipCreationHook)UnhookWindowsHookEx(m_tooltipCreationHook);
        StopClockInput();
        if(m_windowHandle)RemoveWindowSubclass(m_windowHandle,BackdropWindowProcedure,1);
    }

    LRESULT CALLBACK MainWindow::BackdropWindowProcedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR subclassId,DWORD_PTR reference)
    {
        auto self=reinterpret_cast<MainWindow*>(reference);
        if(message==WM_INPUT)self->HandleClockInput(lparam);
        if(message==WM_SIZE){
            const bool minimized=wparam==SIZE_MINIMIZED;
            if(self->m_minimized!=minimized){self->m_minimized=minimized;self->UpdateUsageTimer();}
        }
        if(message==WM_ERASEBKGND && self->m_dwmBackdropEnabled){
            // WinUI's default erase fills the HWND with an opaque theme color.
            // Zero RGB/alpha keeps the extended DWM frame visible beneath XAML.
            RECT client{};
            if(GetClientRect(hwnd,&client) && FillRect(reinterpret_cast<HDC>(wparam),&client,
                static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))))return 1;
        }
        if(message==WM_NCDESTROY)RemoveWindowSubclass(hwnd,BackdropWindowProcedure,subclassId);
        return DefSubclassProc(hwnd,message,wparam,lparam);
    }

    void MainWindow::SetApplyHandler(std::function<void(ManagerBackend::Settings const&, ManagerBackend::Settings const&, ManagerBackend::ApplyMode)> handler)
    {
        m_applyHandler = std::move(handler);
        UpdateActions();
    }
    void MainWindow::SetRefreshHandler(std::function<void()> handler) { m_refreshHandler = std::move(handler); }
    void MainWindow::SetCloseHandler(std::function<void()> handler) { m_closeHandler = std::move(handler); }
    void MainWindow::SetStatusMessage(hstring const& text)
    {
        if (text != m_pendingStatus && m_completionStatusTimer) m_completionStatusTimer.Stop();
        m_pendingStatus = text;
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetHelpText(StatusPanel(),m_statusDetail);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(StatusPanel(), text);
        if (!m_statusAnimating && ResultText().Text() != text) AnimateStatus();
    }

    void MainWindow::UpdateBackdropTheme()
    {
        const BOOL dark=RootGrid().ActualTheme()==ElementTheme::Dark;
        DwmSetWindowAttribute(m_windowHandle,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
        DWM_SYSTEMBACKDROP_TYPE type=DWMSBT_MAINWINDOW;
        m_dwmBackdropEnabled=SUCCEEDED(DwmSetWindowAttribute(m_windowHandle,DWMWA_SYSTEMBACKDROP_TYPE,&type,sizeof(type)));
        if(m_dwmBackdropEnabled){
            MARGINS margins{-1,-1,-1,-1};
            m_dwmBackdropEnabled=SUCCEEDED(DwmExtendFrameIntoClientArea(m_windowHandle,&margins));
        }
        auto backdropTarget=get_strong().as<Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>();
        if(m_dwmBackdropEnabled){
            if(!m_systemCompositor){
                DispatcherQueue().EnsureSystemDispatcherQueue();
                m_systemCompositor=Windows::UI::Composition::Compositor();
                m_transparentBackdropBrush=m_systemCompositor.CreateColorBrush({0,0,0,0});
            }
            // A non-null system brush removes WinUI's opaque island background.
            // The native erase handler also clears the underlying HWND surface.
            backdropTarget.SystemBackdrop(m_transparentBackdropBrush);
            RootGrid().Background(nullptr);
        }
        else{
            backdropTarget.SystemBackdrop(nullptr);
            type=DWMSBT_NONE;
            DwmSetWindowAttribute(m_windowHandle,DWMWA_SYSTEMBACKDROP_TYPE,&type,sizeof(type));
            RootGrid().Background(RootGrid().Resources().Lookup(box_value(L"PageBackgroundBrush")).as<Brush>());
        }
        InvalidateRect(m_windowHandle,nullptr,TRUE);
    }
    void MainWindow::AnimateStatus()
    {
        using namespace Microsoft::UI::Xaml::Media::Animation;
        m_statusAnimating = true;
        DoubleAnimation fadeOut;
        fadeOut.From(StatusContent().Opacity()); fadeOut.To(0.0);
        fadeOut.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(50)));
        Storyboard::SetTarget(fadeOut, StatusContent()); Storyboard::SetTargetProperty(fadeOut, L"Opacity");
        m_statusAnimation = Storyboard(); m_statusAnimation.Children().Append(fadeOut);
        auto weak = get_weak();
        m_statusAnimation.Completed([weak](IInspectable const&, IInspectable const&)
        {
            if (auto self = weak.get(); self && !self->m_closed)
            {
                // Swap text only after the outgoing text is completely transparent.
                self->StatusContent().Opacity(0); self->m_statusAnimation.Stop();
                self->ResultText().Text(self->m_pendingStatus);
                self->UpdateStatusPresentation();
                DoubleAnimation fadeIn; fadeIn.From(0.0); fadeIn.To(1.0);
                fadeIn.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(50)));
                Storyboard::SetTarget(fadeIn, self->StatusContent()); Storyboard::SetTargetProperty(fadeIn, L"Opacity");
                self->m_statusAnimation = Storyboard(); self->m_statusAnimation.Children().Append(fadeIn);
                self->m_statusAnimation.Completed([weak](IInspectable const&, IInspectable const&)
                {
                    if (auto window = weak.get(); window && !window->m_closed)
                    {
                        window->StatusContent().Opacity(1); window->m_statusAnimation.Stop();
                        window->m_statusAnimating = false;
                        if ((window->m_pendingStatus == Localization::Get(Localization::T001) || window->m_pendingStatus == Localization::Get(Localization::T002)) && window->ResultText().Text() == window->m_pendingStatus)
                            window->m_completionStatusTimer.Start();
                        if (window->ResultText().Text() != window->m_pendingStatus) window->AnimateStatus();
                    }
                });
                self->m_statusAnimation.Begin();
            }
        });
        m_statusAnimation.Begin();
    }

    void MainWindow::UpdateStatus()
    {
        if (m_applying) SetStatusMessage(Localization::Get(Localization::T003));
        else if (m_dirty) SetStatusMessage(Localization::Get(Localization::T004));
        else if (!m_completionStatus.empty()) SetStatusMessage(m_completionStatus);
        else
        {
            using MouseState::Mode;
            switch (m_battery.state)
            {
            case Mode::Wired: SetStatusMessage(Localization::Get(Localization::T005)); break;
            case Mode::Missing: case Mode::Offline: case Mode::Error: SetStatusMessage(Localization::Get(Localization::T006)); break;
            case Mode::Wireless: case Mode::Charging: SetStatusMessage(m_reading ? Localization::Get(Localization::T007) : Localization::Get(Localization::T008)); break;
            case Mode::AwaitingBattery: SetStatusMessage(Localization::Get(Localization::T009)); break;
            default: SetStatusMessage(Localization::Get(Localization::T007)); break;
            }
        }
    }

    void MainWindow::UpdateStatusPresentation()
    {
        const auto text=ResultText().Text();
        const bool general=text==Localization::Get(Localization::T007)||text==Localization::Get(Localization::T009)||text==Localization::Get(Localization::T008)||text==Localization::Get(Localization::T005)||text==Localization::Get(Localization::T006);
        auto foreground=RootGrid().Resources().Lookup(box_value(
            general?L"CompactBatteryTextBrush":L"ExpandedBatteryTextBrush")).as<Brush>();
        ResultText().Foreground(foreground); StatusProgressRing().Foreground(foreground);
        for(auto icon : {WirelessStatusIcon(),WiredStatusIcon(),AppliedStatusIcon(),RestoredStatusIcon(),MissingStatusIcon(),UnappliedStatusIcon()}){
            icon.Foreground(foreground); icon.Visibility(Visibility::Collapsed);
        }
        const bool busy=text==Localization::Get(Localization::T007)||text==Localization::Get(Localization::T009)||text==Localization::Get(Localization::T003);
        StatusProgressRing().IsActive(busy);
        StatusProgressRing().Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
        PathIcon selected{nullptr};
        if(text==Localization::Get(Localization::T008))selected=WirelessStatusIcon();
        else if(text==Localization::Get(Localization::T005))selected=WiredStatusIcon();
        else if(text==Localization::Get(Localization::T001))selected=AppliedStatusIcon();
        else if(text==Localization::Get(Localization::T002))selected=RestoredStatusIcon();
        else if(text==Localization::Get(Localization::T006))selected=MissingStatusIcon();
        else if(text==Localization::Get(Localization::T004))selected=UnappliedStatusIcon();
        if(selected)selected.Visibility(Visibility::Visible);
    }

    void MainWindow::CreateStageRows()
    {
        auto rowStyle = RootGrid().Resources().Lookup(box_value(L"SettingsRowStyle")).as<Style>();
        auto separatorStyle = RootGrid().Resources().Lookup(box_value(L"SettingsSeparatorStyle")).as<Style>();
        auto indicatorStyle = RootGrid().Resources().Lookup(box_value(L"DpiStageIndicatorStyle")).as<Style>();
        auto colorButtonResources = RootGrid().Resources().Lookup(box_value(L"DpiColorButtonResources")).as<ResourceDictionary>();
        for (std::size_t i = 0; i < m_stages.size(); ++i)
        {
            auto& row = m_stages[i];
            row.separator = Border(); row.separator.Style(separatorStyle);
            DpiRows().Children().Append(row.separator);
            row.row = Border(); row.row.Style(rowStyle);
            row.row.Padding({0,0,0,0});
            Grid grid;
            ColumnDefinition labelColumn; labelColumn.Width(StarLength()); grid.ColumnDefinitions().Append(labelColumn);
            ColumnDefinition controlsColumn; controlsColumn.Width(AutoLength()); grid.ColumnDefinitions().Append(controlsColumn);
            grid.ColumnSpacing(20);
            row.label = TextBlock(); row.label.FontSize(14);
            row.label.TextWrapping(TextWrapping::Wrap);
            row.label.VerticalAlignment(VerticalAlignment::Center); grid.Children().Append(row.label);
            StackPanel controls; controls.Orientation(Orientation::Horizontal); controls.Spacing(10); controls.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(controls, 1);
            auto number = [this, i](wchar_t const* axis)
            {
                NumberBox box;
                box.VerticalAlignment(VerticalAlignment::Center);
                box.MinWidth(100); box.Width(134); box.Minimum(100); box.Maximum(12000); box.SmallChange(100); box.LargeChange(1000);
                box.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Inline);
                box.ValidationMode(NumberBoxValidationMode::Disabled);
                Windows::Globalization::NumberFormatting::DecimalFormatter formatter;
                formatter.IsGrouped(false); formatter.FractionDigits(0); box.NumberFormatter(formatter);
                Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(box,
                    Localization::Format(Localization::T010,{{L"stage",std::to_wstring(i+1)},{L"axis",axis}}));
                box.ValueChanged([this](NumberBox const&, NumberBoxValueChangedEventArgs const&) { Edited(); });
                box.RegisterPropertyChangedCallback(NumberBox::TextProperty(), [this](DependencyObject const&, DependencyProperty const&) { Edited(); });
                return box;
            };
            row.x = number(L"X"); row.y = number(L"Y");
            row.xLabel = TextBlock(); row.xLabel.Text(L"X"); row.xLabel.VerticalAlignment(VerticalAlignment::Center);
            row.yLabel = TextBlock(); row.yLabel.Text(L"Y"); row.yLabel.VerticalAlignment(VerticalAlignment::Center);
            controls.Children().Append(row.xLabel); controls.Children().Append(row.x);
            controls.Children().Append(row.yLabel); controls.Children().Append(row.y);
            row.colorButton = Button(); row.colorButton.Width(40); row.colorButton.Height(34);
            // Dictionaries have one owner; copy the theme maps for each button.
            ResourceDictionary buttonResources;
            for(auto const& theme : colorButtonResources.ThemeDictionaries()){
                ResourceDictionary themeResources;
                for(auto const& entry : theme.Value().as<ResourceDictionary>())
                    themeResources.Insert(entry.Key(),entry.Value());
                buttonResources.ThemeDictionaries().Insert(theme.Key(),themeResources);
            }
            row.colorButton.Resources().MergedDictionaries().Append(buttonResources);
            row.colorButton.BorderThickness({2,2,2,2});
            row.colorButton.VerticalAlignment(VerticalAlignment::Center);
            row.colorButton.Padding({ 0,0,0,0 });
            row.colorButton.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            row.colorButton.VerticalContentAlignment(VerticalAlignment::Stretch);
            row.colorSwatch = Border(); row.colorSwatch.CornerRadius({2,2,2,2});
            row.colorSwatchBrush = SolidColorBrush(StageColor(m_draft.stages[i].color));
            row.colorSwatch.Background(row.colorSwatchBrush);
            row.colorButton.Content(row.colorSwatch);
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(row.colorButton,
                Localization::Format(Localization::T011,{{L"stage",std::to_wstring(i+1)}}));
            row.colorButton.Click([this, i](IInspectable const&, RoutedEventArgs const&) { ShowColorPicker(i); });
            controls.Children().Append(row.colorButton); grid.Children().Append(controls);
            Border content; content.Padding({20,8,20,8}); content.Child(grid);
            Grid container; container.Children().Append(content);
            row.currentIndicator = Border(); row.currentIndicator.Style(indicatorStyle);
            row.currentIndicator.Visibility(Visibility::Collapsed);
            container.Children().Append(row.currentIndicator);
            row.row.Child(container); DpiRows().Children().Append(row.row);
        }
    }

    void MainWindow::Root_SizeChanged(IInspectable const&, SizeChangedEventArgs const& args)
    {
        bool expanded = args.NewSize().Width >= 900;
        // NavigationView owns its adaptive pane state, including initial template loading.
        UpdateBatteryLayout(expanded);
        if (m_stages.front().row) UpdateStageRows();
    }
    void MainWindow::Root_KeyDown(IInspectable const&, Input::KeyRoutedEventArgs const& args)
    {
        if(m_buttonResetDialog||m_clockCalibrating)return;
        if (args.Key() == Windows::System::VirtualKey::F5 && !m_applying && m_refreshHandler)
        {
            RetryReadParameters(); args.Handled(true);
        }
    }
    void MainWindow::Navigation_SelectionChanged(NavigationView const&, NavigationViewSelectionChangedEventArgs const& args)
    {
        auto item = args.SelectedItem().try_as<NavigationViewItem>();
        if (!item) return;
        if((m_macroRecording||m_clockCalibrating)&&item!=ButtonsItem()){Navigation().SelectedItem(ButtonsItem());return;}
        bool general = item == GeneralItem();
        bool battery = item == BatteryItem();
        bool about = item == AboutItem();
        PageTitle().Text(general ? Localization::Get(Localization::T012) : battery ? Localization::Get(Localization::T013) : about ? Localization::Get(Localization::T014) : Localization::Get(Localization::T015));
        if(item==ButtonsItem()&&m_monitoringConfigured)LoadMacroLibrary();
        AboutPage().Visibility(about ? Visibility::Visible : Visibility::Collapsed);
        GeneralPage().Visibility(general ? Visibility::Visible : Visibility::Collapsed);
        ButtonsPage().Visibility(item == ButtonsItem() ? Visibility::Visible : Visibility::Collapsed);
        BatteryPage().Visibility(battery ? Visibility::Visible : Visibility::Collapsed);
        if((battery||about) && m_monitoringConfigured)UpdateMonitoringControls();
        else if(m_usageTimer)m_usageTimer.Stop();
    }
    void MainWindow::ConfigureMonitoring(Monitoring::Paths const& paths)
    {
        m_monitoringPaths=paths;m_monitoringConfigured=true;
        if(!m_loaded){const auto clock=ClockCompensation::Read(paths.settings);m_baseline.clockCompensationEnabled=m_draft.clockCompensationEnabled=clock.enabled;m_baseline.clockCompensationFactor=m_draft.clockCompensationFactor=clock.factor;UpdateClockControls();}
        UpdateMonitoringControls();
    }
    void MainWindow::UpdateMonitoringControls()
    {
        m_monitoringPopulating=true;
        if(ProgramLoggingToggle()){ProgramLoggingToggle().IsOn(ProgramLog::Enabled(m_monitoringPaths.settings));ProgramLoggingToggle().IsEnabled(true);}
        if(!BatteryPage()){m_monitoringPopulating=false;return;}
        auto state=Monitoring::Read(m_monitoringPaths);
        MonitoringToggle().IsOn(state.enabled);
        StartupToggle().IsOn(state.startup);
        NotificationsToggle().IsOn(state.lowBatteryNotifications);
        MonitoringToggle().IsEnabled(true);
        StartupToggle().IsEnabled(state.enabled && SUCCEEDED(state.error));
        NotificationsToggle().IsEnabled(state.enabled);
        TestNotificationButton().IsEnabled(state.enabled);
        MonitoringOptions().Visibility(state.enabled?Visibility::Visible:Visibility::Collapsed);
        m_monitoringPopulating=false;
        UpdateUsageTimer();
    }
    bool MainWindow::CanRefreshUsage()
    {
        return !m_closed && !m_minimized && m_monitoringConfigured && BatteryPage() &&
            BatteryPage().Visibility()==Visibility::Visible && MonitoringToggle().IsOn();
    }
    void MainWindow::UpdateUsageTimer()
    {
        if(CanRefreshUsage()){
            if(m_usageTimer && !m_usageTimer.IsRunning())m_usageTimer.Start();
            RefreshUsage();
        }else if(m_usageTimer)m_usageTimer.Stop();
    }
    void MainWindow::CreateUsageChart()
    {
        for(size_t index=0;index<BatteryUsage::DisplayDays;++index){
            ColumnDefinition barColumn,dateColumn;barColumn.Width(StarLength());dateColumn.Width(StarLength());
            UsageBars().ColumnDefinitions().Append(barColumn);UsageDates().ColumnDefinitions().Append(dateColumn);
            auto bar=m_usageBars[index];bar.Width(18);bar.CornerRadius(CornerRadius{3,3,3,3});bar.UseLayoutRounding(true);
            bar.HorizontalAlignment(HorizontalAlignment::Center);bar.VerticalAlignment(VerticalAlignment::Bottom);
            bar.Style(RootGrid().Resources().Lookup(box_value(index+1==BatteryUsage::DisplayDays?L"UsageTodayBarStyle":L"UsagePastBarStyle")).as<Style>());
            Grid::SetColumn(bar,static_cast<int>(index));UsageBars().Children().Append(bar);
            auto label=m_usageDates[index];label.FontSize(11);label.HorizontalAlignment(HorizontalAlignment::Center);
            label.Style(RootGrid().Resources().Lookup(box_value(index+1==BatteryUsage::DisplayDays?L"UsageTodayDateStyle":L"UsagePastDateStyle")).as<Style>());
            Grid::SetColumn(label,static_cast<int>(index));UsageDates().Children().Append(label);
        }
    }
    void MainWindow::UsageChart_SizeChanged(IInspectable const&,SizeChangedEventArgs const&)
    {
        RenderUsage();
    }
    void MainWindow::RenderUsage()
    {
        if(!BatteryPage() || m_minimized)return;
        const auto& today=m_usage.days.back();bool ok=m_usage.error==ERROR_SUCCESS;
        UsageAverageValue().Text(ok && m_usage.averageDays?UsagePercent(m_usage.average):L"--");
        UsageTodayValue().Text(ok && today.hasReading?UsagePercent(today.consumed):L"--");
        const wchar_t* description=Localization::Get(Localization::T239);
        if(!ok)description=Localization::Get(Localization::T240);
        else if(m_usage.averageDays && today.hasReading){
            double difference=std::round(today.consumed)-std::round(m_usage.average);
            description=difference<0?Localization::Get(Localization::T241):
                difference>0?Localization::Get(Localization::T242):
                Localization::Get(Localization::T243);
        }
        UsageDescription().Text(description);
        double average=std::round(m_usage.average),maximum=average;
        for(const auto& day:m_usage.days)if(day.hasReading)maximum=std::max(maximum,day.consumed);
        double scale=std::max(1.0,maximum*1.25);
        double chartWidth=UsageBars().ActualWidth(),slotWidth=chartWidth/BatteryUsage::DisplayDays;
        double barWidth=chartWidth>0?std::max(1.0,std::floor(std::min({22.0,slotWidth*0.55,slotWidth-4}))):18;
        for(size_t index=0;index<BatteryUsage::DisplayDays;++index){
            const auto& day=m_usage.days[index];auto bar=m_usageBars[index];auto label=m_usageDates[index];
            bar.Width(barWidth);bar.Height(std::max(2.0,160*day.consumed/scale));
            bar.Visibility(ok && day.hasReading?Visibility::Visible:Visibility::Collapsed);
            wchar_t date[20]{};std::swprintf(date,ARRAYSIZE(date),L"%u/%u",static_cast<unsigned>(day.date.wMonth),static_cast<unsigned>(day.date.wDay));
            label.Text(date);label.Opacity(day.hasReading && ok?1.0:0.5);
        }
        auto visibility=ok && m_usage.averageDays?Visibility::Visible:Visibility::Collapsed;
        UsageAverageLine().Visibility(visibility);UsageAverageLabel().Visibility(visibility);
        double averageTop=std::clamp(160-160*average/scale-1.5,0.0,157.0);
        UsageAverageLine().Margin(Thickness{0,averageTop,0,0});
        UsageAverageLabel().Margin(Thickness{0,std::max(0.0,averageTop-24),0,0});
        UsageAverageLabel().Text(Localization::Get(Localization::T244));
    }
    void MainWindow::RefreshUsage()
    {
        if(!CanRefreshUsage())return;
        if(m_usageLoading){m_usageReloadRequested=true;return;}
        m_usageLoading=true;
        auto queue=DispatcherQueue();auto weak=get_weak();auto logs=m_monitoringPaths.directory+L"\\batteryhistory";auto reader=m_usageReader;
        try{
            Windows::System::Threading::ThreadPool::RunAsync([queue,weak,logs,reader](Windows::Foundation::IAsyncAction const&){
                BatteryUsage::Report usage;
                try{usage=reader->Read(logs);}
                catch(...){SYSTEMTIME today{};GetLocalTime(&today);usage=BatteryUsage::Empty(today);usage.error=ERROR_READ_FAULT;}
                queue.TryEnqueue([weak,usage]{
                    if(auto self=weak.get();self && !self->m_closed){
                        self->m_usageLoading=false;
                        if(self->CanRefreshUsage() && !SameUsage(self->m_usage,usage)){self->m_usage=usage;self->RenderUsage();}
                        if(self->m_usageReloadRequested){self->m_usageReloadRequested=false;self->RefreshUsage();}
                    }
                });
            });
        }catch(...){m_usageLoading=false;m_usage.error=ERROR_READ_FAULT;RenderUsage();}
    }
    void MainWindow::ProgramLogging_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        if(!m_monitoringConfigured||m_monitoringPopulating)return;
        const bool enabled=ProgramLoggingToggle().IsOn();
        if(!enabled)ProgramLog::Write("program_logging_disabled");
        if(!ProgramLog::SetEnabled(m_monitoringPaths.settings,enabled)){
            const DWORD error=GetLastError();UpdateMonitoringControls();
            MessageBoxW(m_windowHandle,(Localization::Format(Localization::T016,{{L"errorCode",std::to_wstring(error)}})).c_str(),Localization::Get(Localization::T017),MB_OK|MB_ICONERROR);
            return;
        }
        if(enabled)ProgramLog::Write("program_logging_enabled");
    }
    void MainWindow::Monitoring_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        if(!m_monitoringConfigured || m_monitoringPopulating)return;
        HRESULT result=Monitoring::SetEnabled(m_monitoringPaths,MonitoringToggle().IsOn());
        UpdateMonitoringControls();
        if(FAILED(result)){
            std::wstring text=Localization::Format(Localization::T018,{{L"errorCode",std::to_wstring(static_cast<unsigned>(result))}});
            MessageBoxW(m_windowHandle,text.c_str(),L"G3 Companion",MB_OK|MB_ICONERROR);
        }
    }
    void MainWindow::Startup_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        if(!m_monitoringConfigured || m_monitoringPopulating)return;
        HRESULT result=Monitoring::SetStartup(m_monitoringPaths,StartupToggle().IsOn());
        UpdateMonitoringControls();
        if(FAILED(result))MessageBoxW(m_windowHandle,Localization::Get(Localization::T245),L"G3 Companion",MB_OK|MB_ICONERROR);
    }
    void MainWindow::Notifications_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        if(!m_monitoringConfigured || m_monitoringPopulating)return;
        HRESULT result=Monitoring::SetLowBatteryNotifications(m_monitoringPaths,NotificationsToggle().IsOn());
        UpdateMonitoringControls();
        if(FAILED(result))MessageBoxW(m_windowHandle,Localization::Get(Localization::T246),L"G3 Companion",MB_OK|MB_ICONERROR);
    }
    void MainWindow::TestNotification_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if(!m_monitoringConfigured)return;
        HRESULT result=Monitoring::TestNotification(m_monitoringPaths);
        if(FAILED(result))MessageBoxW(m_windowHandle,Localization::Get(Localization::T247),L"G3 Companion",MB_OK|MB_ICONERROR);
    }
    void MainWindow::Slider_ValueChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const&) { Edited(); }
    void MainWindow::Toggle_Toggled(IInspectable const&, RoutedEventArgs const&) { Edited(); }
    void MainWindow::Combo_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) { Edited(); }

    void MainWindow::Populate()
    {
        m_populating = true;
        SleepSlider().Value(m_draft.sleepMinutes); DebounceSlider().Value(m_draft.debounceMs);
        AngleToggle().IsOn(m_draft.angleSnap); RippleToggle().IsOn(m_draft.ripple);
        const int rates[] = { 125,250,500,1000 };
        for (int i = 0; i < 4; ++i) if (rates[i] == m_draft.reportRate) RateCombo().SelectedIndex(i);
        CountCombo().SelectedIndex(std::clamp(m_draft.stageCount, 1, 6) - 1);
        for (std::size_t i = 0; i < m_stages.size(); ++i)
        {
            auto& row = m_stages[i]; auto const& stage = m_draft.stages[i];
            row.splitAxes = stage.dpiX != stage.dpiY;
            row.x.Value(stage.dpiX); row.y.Value(stage.dpiY);
            // Reserved inactive slots remain visible as their original text when enabled.
            row.x.Text(to_hstring(stage.dpiX)); row.y.Text(to_hstring(stage.dpiY));
            row.colorSwatchBrush.Color(StageColor(stage.color));
            if (row.picker) row.picker.Color(StageColor(stage.color));
        }
        m_populating = false; m_editorChangedSincePopulate = false;
        UpdateStageRows(); UpdateButtonLabels(); UpdateClockControls(); UpdateActions();
    }

    bool MainWindow::ReadDraft(std::wstring& message)
    {
        m_draft.sleepMinutes = static_cast<int>(std::lround(SleepSlider().Value()));
        m_draft.debounceMs = static_cast<int>(std::lround(DebounceSlider().Value()));
        m_draft.angleSnap = AngleToggle().IsOn(); m_draft.ripple = RippleToggle().IsOn();
        const int rates[] = { 125,250,500,1000 };
        int rate = RateCombo().SelectedIndex(); if (rate >= 0 && rate < 4) m_draft.reportRate = rates[rate];
        int count = CountCombo().SelectedIndex(); if (count >= 0 && count < 6) m_draft.stageCount = count + 1;
        m_draft.currentStage = std::clamp(m_baseline.currentStage, 1, m_draft.stageCount);
        bool valid = true;
        for (int i = 0; i < m_draft.stageCount; ++i)
        {
            auto index = static_cast<std::size_t>(i); int x = 0, y = 0;
            if (!ReadNumber(m_stages[index].x, x)) valid = false;
            else { m_draft.stages[index].dpiX = x; if (!m_stages[index].splitAxes) m_draft.stages[index].dpiY = x; }
            if (m_stages[index].splitAxes)
            {
                if (!ReadNumber(m_stages[index].y, y)) valid = false;
                else m_draft.stages[index].dpiY = y;
            }
        }
        if (!valid) message = Localization::Get(Localization::T019);
        else message.clear();
        return valid;
    }
    void MainWindow::Edited()
    {
        if (m_populating) return;
        m_editorChangedSincePopulate = true;
        if (!m_loaded || m_applying || m_macroRecording) return;
        std::wstring error; bool valid = ReadDraft(error);
        m_dirty = !valid || !ManagerBackend::Equals(m_baseline, m_draft);
        m_completionStatus.clear(); UpdateStatus();
        m_statusDetail=error;
        UpdateStageRows(); UpdateActions();
    }
    void MainWindow::UpdateStageRows()
    {
        SleepValue().Text(Localization::Format(Localization::T020,{{L"minutes",std::to_wstring(m_draft.sleepMinutes)}}));
        DebounceValue().Text(to_hstring(m_draft.debounceMs) + L" ms");
        for (std::size_t i = 0; i < m_stages.size(); ++i)
        {
            auto& row = m_stages[i];
            bool narrow = RootGrid().ActualWidth() < 760;
            row.x.Width(row.splitAxes ? (narrow ? 112 : 134) : (narrow ? 144 : 164));
            row.y.Width(narrow ? 112 : 134);
            row.row.Visibility(i < static_cast<std::size_t>(std::clamp(m_draft.stageCount, 1, 6)) ? Visibility::Visible : Visibility::Collapsed);
            row.separator.Visibility(row.row.Visibility());
            row.y.Visibility(row.splitAxes ? Visibility::Visible : Visibility::Collapsed);
            row.xLabel.Visibility(row.splitAxes ? Visibility::Visible : Visibility::Collapsed);
            row.yLabel.Visibility(row.y.Visibility());
            const bool current=m_liveCurrentStage == static_cast<int>(i + 1);
            row.label.Text(Localization::Format(Localization::T021,{{L"stage",std::to_wstring(i+1)}}));
            row.currentIndicator.Visibility(current ? Visibility::Visible : Visibility::Collapsed);
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetHelpText(row.label,current ? Localization::Get(Localization::T343) : L"");
        }
    }
    void MainWindow::UpdateActions()
    {
        hstring windowTitle=L"G3 Companion";
        if(m_dirty)windowTitle=windowTitle+L" "+Localization::Get(Localization::T022);
        if(Title()!=windowTitle)Title(windowTitle);
        const hstring editSubtitle=m_dirty ? Localization::Get(Localization::T022) : L"";
        if(ManagerTitleBar().Subtitle()!=editSubtitle)ManagerTitleBar().Subtitle(editSubtitle);
        const bool clockLocked=m_clockCalibrating||m_clockRecoveryPending;
        SettingsEditor().IsEnabled(m_loaded && !m_applying && !m_macroRecording && !clockLocked);
        ButtonsEditor().IsEnabled(m_loaded && m_buttonsLoaded && !m_applying && !m_macroRecording && !clockLocked);
        MacroControls().IsEnabled(!m_applying && !clockLocked);
        const bool clockAvailable=m_loaded && m_buttonsLoaded && !m_applying && !m_macroRecording && !clockLocked;
        ClockCalibrateButton().IsEnabled(clockAvailable);ClockCompensationToggle().IsEnabled(clockAvailable);
        if(m_applying && m_buttonFlyout)m_buttonFlyout.Hide();
        ApplyButton().IsEnabled(m_dirty && m_loaded && !m_applying && !m_macroRecording && !clockLocked &&
            ((m_draft.buttons==m_baseline.buttons&&std::none_of(m_draft.macroAssignments.begin(),m_draft.macroAssignments.end(),[](auto const& assignment){return assignment.has_value();})) || m_buttonsLoaded) && static_cast<bool>(m_applyHandler));
        RestoreButton().IsEnabled(m_dirty && m_loaded && !m_applying && !m_macroRecording && !clockLocked);
    }
    void MainWindow::Apply_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (!m_loaded || m_applying || m_macroRecording || m_clockCalibrating || m_clockRecoveryPending || !m_applyHandler) return;
        std::wstring message;
        if (!ReadDraft(message) || !ManagerBackend::Validate(m_draft, message) || !CompileBoundMacros(message))
        {
            m_statusDetail=message; UpdateStatus(); return;
        }
        m_applying = true; m_completionStatus.clear(); UpdateStatus(); UpdateActions();
        try { m_applyHandler(m_draft, m_baseline, ManagerBackend::ApplyMode::EditedSettings); }
        catch (...) { m_applying = false; m_statusDetail=Localization::Get(Localization::T023); UpdateStatus(); UpdateActions(); }
    }
    void MainWindow::Restore_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_applying || m_macroRecording || m_clockCalibrating || m_clockRecoveryPending) return;
        m_draft = m_baseline; m_dirty = false; Populate(); m_completionStatus = Localization::Get(Localization::T002);
        m_statusDetail.clear(); UpdateStatus();
    }
    void MainWindow::ShowColorPicker(std::size_t index)
    {
        auto& row = m_stages[index];
        if (!row.picker)
        {
            row.picker = ColorPicker(); row.picker.IsAlphaEnabled(false);
            row.selectedColorBrush = SolidColorBrush(StageColor(m_draft.stages[index].color));
            row.picker.Resources().Insert(box_value(L"CompactPickerSelectedColorBrush"),row.selectedColorBrush);
            row.picker.Style(Application::Current().Resources().Lookup(box_value(L"CompactColorPickerStyle")).as<Style>());
            row.picker.IsAlphaSliderVisible(false); row.picker.IsAlphaTextInputVisible(false);
            row.picker.IsColorPreviewVisible(false);
            row.picker.IsColorChannelTextInputVisible(false);
            row.picker.IsColorSliderVisible(true);
            row.picker.IsHexInputVisible(true); row.picker.IsMoreButtonVisible(false);
            row.picker.Width(224);
            row.picker.ColorChanged([this, index](ColorPicker const&, ColorChangedEventArgs const& args)
            {
                m_stages[index].selectedColorBrush.Color(args.NewColor());
                if (m_populating || !m_loaded || m_applying || m_macroRecording) return;
                m_draft.stages[index].color = StageValue(args.NewColor());
                m_stages[index].colorSwatchBrush.Color(args.NewColor()); Edited();
            });
            row.flyout = Flyout(); row.flyout.Content(row.picker);
            auto weak=get_weak();
            row.flyout.Closed([weak,index](auto const& sender,auto const&){
                if(auto self=weak.get()){
                    sender.template as<Flyout>().Content(nullptr);
                    auto& stage=self->m_stages[index];stage.picker=nullptr;stage.selectedColorBrush=nullptr;stage.flyout=nullptr;
                }
            });
        }
        m_populating = true; row.picker.Color(StageColor(m_draft.stages[index].color)); m_populating = false;
        row.flyout.ShowAt(row.colorButton);
    }

    void MainWindow::UpdateSnapshot(ManagerBackend::Snapshot const& snapshot)
    {
        if (m_closed) return;
        if (!DispatcherQueue().HasThreadAccess())
        {
            auto copy = std::make_shared<ManagerBackend::Snapshot>(snapshot); auto weak = get_weak();
            DispatcherQueue().TryEnqueue([weak, copy] { if (auto self = weak.get()) self->UpdateSnapshot(*copy); });
            return;
        }
        if(HandleClockSnapshot(snapshot))return;
        if (m_battery.state != snapshot.status.state) m_completionStatus.clear();
        const bool usageChanged=m_battery.sampledAtUtc!=snapshot.status.sampledAtUtc;
        m_battery = snapshot.status; UpdateBattery();if(usageChanged)RefreshUsage();
        UpdateButtonSnapshot(snapshot);
        m_statusDetail=snapshot.message;
        if (snapshot.configurationValid || (snapshot.status.state != MouseState::Mode::Unknown && snapshot.status.state != MouseState::Mode::AwaitingBattery)) m_reading = false;
        if (snapshot.configurationValid) m_liveCurrentStage = snapshot.settings.currentStage;
        if(snapshot.configurationValid || snapshot.status.state==MouseState::Mode::Offline || snapshot.status.state==MouseState::Mode::Missing){
            m_parameterReadFailureNotified=false;
            if(m_parameterReadFailureDialog)m_parameterReadFailureDialog.Hide();
        }
        if (snapshot.operationFinished)
        {
            const bool resettingButtons=m_resetButtonApply;m_resetButtonApply=false;
            m_applying = false;
            if (snapshot.configurationValid)
            {
                const auto previousButtons=m_baseline.buttons;
                m_baseline = snapshot.settings; m_loaded = true;
                if (snapshot.applied) { m_draft = m_baseline; m_dirty = false; Populate(); }
                else if(resettingButtons){
                    m_draft=m_baseline;m_draft.buttons=ManagerBackend::DefaultButtonMappings;
                    m_dirty=!ManagerBackend::Equals(m_baseline,m_draft);Populate();
                }
                else {
                    for(std::size_t index=0;index<m_draft.buttons.size();++index)
                        if(m_draft.buttons[index]==previousButtons[index])m_draft.buttons[index]=m_baseline.buttons[index];
                    std::wstring error; bool valid = ReadDraft(error); m_dirty = !valid || !ManagerBackend::Equals(m_baseline, m_draft);
                }
            }
            m_completionStatus = snapshot.applied ? hstring(Localization::Get(Localization::T001)) : hstring();
        }
        else if (snapshot.configurationValid && !m_dirty && !m_applying)
        {
            // Equal numeric values can still have edited NumberBox text that needs normalization.
            const bool populate = !m_loaded || m_editorChangedSincePopulate || !ManagerBackend::Equals(m_draft, snapshot.settings);
            m_baseline = snapshot.settings; m_draft = m_baseline; m_loaded = true;
            if (populate) Populate();
        }
        UpdateStatus(); UpdateStageRows(); UpdateButtonLabels(); UpdateActions();
        if(ShouldPromptParameterReadFailure(snapshot))PromptParameterReadFailure();
    }

    bool MainWindow::ShouldPromptParameterReadFailure(ManagerBackend::Snapshot const& snapshot) const
    {
        using MouseState::Mode;
        const bool connected=snapshot.status.state==Mode::Wireless || snapshot.status.state==Mode::Charging || snapshot.status.state==Mode::Wired;
        return connected && snapshot.parameterReadFailed && !snapshot.configurationValid && !snapshot.operationFinished &&
            !m_loaded && !m_applying && !m_closePending && !m_closed && !m_parameterReadFailureNotified && !m_parameterReadFailureDialog;
    }

    Grid MainWindow::CreatePowerSwitchIllustration(ContentDialog const& dialog)
    {
        auto illustration=Application::Current().Resources().Lookup(box_value(L"PowerSwitchIllustrationTemplate")).as<DataTemplate>().LoadContent().as<Grid>();
        auto animation=illustration.Resources().Lookup(box_value(L"PowerSwitchAnimation")).as<Media::Animation::Storyboard>();
        Media::Animation::Storyboard::SetTarget(animation.Children().GetAt(0),illustration.FindName(L"PowerSwitchThumb").as<DependencyObject>());
        Media::Animation::Storyboard::SetTarget(animation.Children().GetAt(1),illustration.FindName(L"PowerSwitchFill").as<DependencyObject>());
        auto weak=make_weak(illustration);
        dialog.Opened([animation,weak](auto const&,auto const&){if(auto view=weak.get();view&&view.Visibility()==Visibility::Visible)animation.Begin();});
        dialog.Closed([animation](auto const&,auto const&){animation.Stop();});
        illustration.RegisterPropertyChangedCallback(UIElement::VisibilityProperty(),[animation](DependencyObject const& sender,DependencyProperty const&){
            auto view=sender.as<Grid>();
            if(view.IsLoaded()&&view.Visibility()==Visibility::Visible)animation.Begin();else animation.Stop();
        });
        return illustration;
    }

    ContentDialog MainWindow::CreateParameterReadFailureDialog()
    {
        ContentDialog dialog;
        dialog.Title(box_value(Localization::Get(Localization::T024)));
        StackPanel content;content.Spacing(20);
        TextBlock body;body.Text(Localization::Get(Localization::T025));
        body.TextWrapping(TextWrapping::Wrap);content.Children().Append(body);
        content.Children().Append(CreatePowerSwitchIllustration(dialog));dialog.Content(content);
        dialog.CloseButtonText(Localization::Get(Localization::T026));
        dialog.DefaultButton(ContentDialogButton::Close);
        return dialog;
    }

    void MainWindow::RetryReadParameters()
    {
        if(m_closed || m_applying || !m_refreshHandler)return;
        m_parameterReadFailureNotified=false;m_completionStatus.clear();m_statusDetail.clear();m_reading=true;
        UpdateStatus();m_refreshHandler();
    }

    void MainWindow::PromptParameterReadFailure()
    {
        if(!m_refreshHandler || !RootGrid().XamlRoot())return;
        auto dialog=CreateParameterReadFailureDialog();dialog.XamlRoot(RootGrid().XamlRoot());
        try{
            auto operation=dialog.ShowAsync();
            m_parameterReadFailureDialog=dialog;m_parameterReadFailureNotified=true;
            auto weak=get_weak();
            operation.Completed([weak,dialog](IAsyncOperation<ContentDialogResult> const&,AsyncStatus){
                if(auto self=weak.get())self->DispatcherQueue().TryEnqueue([weak,dialog]{
                    if(auto window=weak.get()){
                        if(window->m_parameterReadFailureDialog!=dialog)return;
                        window->m_parameterReadFailureDialog=nullptr;
                    }
                });
            });
        }catch(hresult_error const& error){
            m_statusDetail=hstring(Localization::Format(Localization::T027,{{L"details",error.message().c_str()}}));UpdateStatus();
        }
    }

    void MainWindow::UpdateBatteryLayout(bool expanded)
    {
        // Anchor the battery to the full pane width outside NavigationView's footer clipping.
        const bool compactCharging=!expanded && m_battery.state==MouseState::Mode::Charging;
        // Expanded text height: title 18 + number 36 - spacing adjustment 2 = 52 DIP.
        const double contentHeight=expanded ? 52 : 32;
        BatteryHost().Width(expanded ? 180 : 48);
        BatteryHost().Height(contentHeight+(expanded ? 32 : 16));
        SidebarFooterHost().Width(BatteryHost().Width());
        BatteryFooterSpacer().Height(BatteryHost().Height()+StatusPanel().Height());
        StatusPanel().Margin(expanded ? Thickness{8,0,8,0} : Thickness{0,0,0,0});
        StatusContent().HorizontalAlignment(expanded ? HorizontalAlignment::Stretch : HorizontalAlignment::Center);
        StatusContent().ColumnSpacing(expanded ? 4 : 0);
        ResultText().Visibility(expanded ? Visibility::Visible : Visibility::Collapsed);
        StatusContent().ColumnDefinitions().GetAt(1).Width(expanded ? StarLength() : GridLength{0,GridUnitType::Pixel});
        BatteryPanel().Width(expanded ? 148 : 32);
        BatteryPanel().Height(contentHeight);
        BatteryPanel().Margin(expanded ? Thickness{16,16,16,16} : Thickness{8,8,8,8});
        BatteryPanel().ColumnSpacing(expanded ? 8 : 0);
        BatteryRingColumn().Width(expanded ? GridLength{56,GridUnitType::Pixel} : GridLength{32,GridUnitType::Pixel});
        BatteryRing().Width(expanded ? 52 : 32);
        BatteryRing().Height(expanded ? 52 : 32);
        BatteryProgress().Width(BatteryRing().Width());
        BatteryProgress().Height(BatteryRing().Height());
        Grid::SetColumn(BatteryRing(), 0);
        Grid::SetColumn(BatteryTextStack(), expanded ? 1 : 0);
        BatteryTextStack().HorizontalAlignment(expanded ? HorizontalAlignment::Right : HorizontalAlignment::Center);
        BatteryRing().HorizontalAlignment(expanded ? HorizontalAlignment::Left : HorizontalAlignment::Center);
        Grid::SetRow(BatteryTextStack(), 0);
        Grid::SetRow(BatteryRing(), 0);
        BatteryPanel().RowDefinitions().GetAt(0).Height(StarLength());
        BatteryPanel().RowDefinitions().GetAt(1).Height(AutoLength());
        BatteryTextStack().Margin({0,0,0,0});
        BatteryTextStack().Visibility(compactCharging ? Visibility::Collapsed : Visibility::Visible);
        BatteryTitle().Visibility(expanded ? Visibility::Visible : Visibility::Collapsed);
        BatteryNumber().FontSize(expanded ? 32 : 12);
        BatteryNumber().LineHeight(expanded ? 36 : 16);
        BatteryNumber().Margin(expanded ? Thickness{0,-2,0,0} : Thickness{0,0,0,0});
        BatteryNumber().HorizontalAlignment(expanded ? HorizontalAlignment::Right : HorizontalAlignment::Center);
        BatteryNumber().TextAlignment(expanded ? TextAlignment::Right : TextAlignment::Center);
        BatteryNumber().Foreground(RootGrid().Resources().Lookup(box_value(expanded ? L"ExpandedBatteryTextBrush" : L"CompactBatteryTextBrush")).as<Brush>());
        const int percent = MouseState::RelativePercent(m_battery.rawPercent);
        const bool visible = MouseState::ShowPercent(m_battery.state) && percent >= 0;
        BatteryNumber().Text(visible ? to_hstring(percent) + (expanded ? L"%" : L"") : hstring(expanded ? L"--%" : L"--"));
        UpdateBatteryGeometry();
    }

    void MainWindow::UpdateBatteryGeometry()
    {
        // Only the charging glyph uses custom geometry; ProgressRing draws the battery level.
        const float size=static_cast<float>(BatteryRing().Width());
        if(size==m_batteryGeometrySize)return;
        PathGeometry lightning; PathFigure figure;
        const float scale=size/72;
        figure.StartPoint({39*scale,14*scale}); figure.IsClosed(true); figure.IsFilled(true);
        const Windows::Foundation::Point points[]={{22,37},{32,37},{29,56},{47,31},{36,31}};
        for(auto const& point:points)
        {
            LineSegment line; line.Point({point.X*scale,point.Y*scale}); figure.Segments().Append(line);
        }
        lightning.Figures().Append(figure); BatteryLightning().Data(lightning); m_batteryGeometrySize=size;
    }

    void MainWindow::UpdateBattery()
    {
        int percent = MouseState::RelativePercent(m_battery.rawPercent);
        bool visible = MouseState::ShowPercent(m_battery.state) && percent >= 0;
        bool charging = m_battery.state == MouseState::Mode::Charging;
        bool dark = RootGrid().ActualTheme() == ElementTheme::Dark;
        Windows::UI::Color gray = dark ? Windows::UI::Color{ 255,170,173,181 } : Windows::UI::Color{ 255,100,105,113 };
        Windows::UI::Color green = dark ? Windows::UI::Color{ 255,66,212,124 } : Windows::UI::Color{ 255,22,143,87 };
        Windows::UI::Color color = gray;
        if (m_battery.state == MouseState::Mode::Wireless)
            color = percent <= 10 ? Windows::UI::Color{ 255,228,92,92 } : percent <= 20 ? Windows::UI::Color{ 255,216,149,36 } : green;
        UpdateBatteryLayout(RootGrid().ActualWidth() >= 900);
        BatteryLightning().Visibility(charging ? Visibility::Visible : Visibility::Collapsed);
        if(!m_batteryLightningBrush){
            m_batteryLightningBrush=SolidColorBrush(green);BatteryLightning().Fill(m_batteryLightningBrush);
        }else m_batteryLightningBrush.Color(green);
        if(!m_batteryProgressBrush){
            m_batteryProgressBrush=SolidColorBrush(color);BatteryProgress().Foreground(m_batteryProgressBrush);
        }else m_batteryProgressBrush.Color(color);
        BatteryProgress().Value(visible ? percent : 0);
        std::wstring status;
        using MouseState::Mode;
        switch (m_battery.state)
        {
        case Mode::Wireless: status = Localization::Get(Localization::T008); break;
        case Mode::Wired: status = Localization::Get(Localization::T005); break;
        case Mode::Offline:
            if (m_battery.rawPercent >= 0 && m_battery.lastRead.wYear)
            {
                wchar_t reading[40]{};
                swprintf_s(reading,L"%02u:%02u",m_battery.lastRead.wHour,m_battery.lastRead.wMinute);
                status = Localization::Format(Localization::T248,{{L"time",reading}});
            }
            else status = Localization::Get(Localization::T249);
            break;
        case Mode::Missing: status = Localization::Get(Localization::T006); break;
        case Mode::Charging: status = Localization::Get(Localization::T250); break;
        case Mode::AwaitingBattery: status = Localization::Get(Localization::T251); break;
        case Mode::Error: status = Localization::Get(Localization::T046); break;
        default: status = Localization::Get(Localization::T252); break;
        }
        if (m_battery.rawPercent >= 0 && m_battery.lastRead.wYear)
        {
            wchar_t time[80]{};
            swprintf_s(time, L"%04u-%02u-%02u %02u:%02u", m_battery.lastRead.wYear,
                m_battery.lastRead.wMonth, m_battery.lastRead.wDay, m_battery.lastRead.wHour, m_battery.lastRead.wMinute);
            status += Localization::Format(Localization::T253,{{L"dateTime",time}});
        }
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(BatteryRing(), status);
    }

    Windows::Foundation::IAsyncAction MainWindow::ConfirmClose()
    {
        auto lifetime=get_strong();m_closePending=true;
        try {
            StopMacroRecording();
            if(!co_await ConfirmMacroSwitch()){m_closePending=false;co_return;}
            if(m_dirty){
                ContentDialog dialog;dialog.XamlRoot(RootGrid().XamlRoot());dialog.Title(box_value(Localization::Get(Localization::T028)));
                dialog.Content(box_value(Localization::Get(Localization::T029)));dialog.PrimaryButtonText(Localization::Get(Localization::T026));dialog.CloseButtonText(Localization::Get(Localization::T030));
                dialog.DefaultButton(ContentDialogButton::Close);
                if(co_await dialog.ShowAsync()!=ContentDialogResult::Primary){m_closePending=false;co_return;}
            }
            m_closePending=false;m_allowClose=true;Close();
        } catch(...) {m_closePending=false;}
    }
}
