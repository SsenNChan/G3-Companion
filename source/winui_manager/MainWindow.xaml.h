#pragma once
#include "../localization.h"
#include "MainWindow.g.h"
#include "../manager_backend.h"
#include "../monitor_settings.h"
#include "../battery_usage.h"
#include "../macro_recording_log.h"
#include "../macro_layout.h"
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Windows.UI.Composition.h>
#include <array>
#include <functional>
#include <memory>
#include <map>

namespace winrt::G3Companion::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        void UpdateSnapshot(ManagerBackend::Snapshot const& snapshot);
        void SetApplyHandler(std::function<void(ManagerBackend::Settings const&, ManagerBackend::Settings const&, ManagerBackend::ApplyMode)> handler);
        void SetRefreshHandler(std::function<void()> handler);
        void SetCloseHandler(std::function<void()> handler);

        void Root_SizeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void Root_KeyDown(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
        void Navigation_SelectionChanged(Microsoft::UI::Xaml::Controls::NavigationView const&, Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
        void Slider_ValueChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void Toggle_Toggled(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Combo_SelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void Apply_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Restore_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ProgramLogging_Toggled(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Monitoring_Toggled(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Startup_Toggled(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Notifications_Toggled(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void TestNotification_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ConfigureMonitoring(Monitoring::Paths const& paths);
        void UsageChart_SizeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);

    private:
        struct StageControls
        {
            Microsoft::UI::Xaml::Controls::Border row{ nullptr };
            Microsoft::UI::Xaml::Controls::Border separator{ nullptr };
            Microsoft::UI::Xaml::Controls::Border currentIndicator{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBlock label{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBlock xLabel{ nullptr }, yLabel{ nullptr };
            Microsoft::UI::Xaml::Controls::NumberBox x{ nullptr }, y{ nullptr };
            Microsoft::UI::Xaml::Controls::Button colorButton{ nullptr };
            Microsoft::UI::Xaml::Controls::Border colorSwatch{ nullptr };
            Microsoft::UI::Xaml::Media::SolidColorBrush colorSwatchBrush{ nullptr };
            Microsoft::UI::Xaml::Controls::ColorPicker picker{ nullptr };
            Microsoft::UI::Xaml::Media::SolidColorBrush selectedColorBrush{ nullptr };
            Microsoft::UI::Xaml::Controls::Flyout flyout{ nullptr };
            bool splitAxes = false;
        };
        std::array<StageControls, 6> m_stages;
        std::array<Microsoft::UI::Xaml::Controls::DropDownButton,6> m_buttonSelectors;
        std::array<Microsoft::UI::Xaml::Controls::Grid,6> m_buttonPreviewHosts;
        struct ButtonLabelState {
            std::wstring draftName,currentName;
            bool draftLogo=false,currentLogo=false,initialized=false;
        };
        std::array<ButtonLabelState,6> m_buttonLabels;
        ManagerBackend::ButtonMappings m_liveButtons=ManagerBackend::DefaultButtonMappings;
        std::shared_ptr<const std::vector<ManagerBackend::MacroConfiguration>> m_macroSnapshot=ManagerBackend::EmptyMacros;
        std::wstring m_macroDirectory;
        std::vector<MacroEditor::Profile> m_macroProfiles;
        struct MacroListView {
            std::wstring id;
            Microsoft::UI::Xaml::Controls::Button row{nullptr};
            Microsoft::UI::Xaml::Controls::TextBlock label{nullptr};
            Microsoft::UI::Xaml::Controls::Border indicator{nullptr};
            bool renaming=false;
        };
        std::vector<MacroListView> m_macroListViews;
        struct MacroReadbackLink {
            std::wstring profileId;
            ManagerBackend::ButtonCode binding{};
            std::vector<std::uint8_t> data;
        };
        std::array<std::optional<MacroReadbackLink>,6> m_macroReadbackLinks;
        std::optional<std::size_t> m_selectedMacro;
        MacroEditor::Profile m_macroDraft;
        bool m_macroRenderQueued=false,m_macroRepeatInvalid=false;
        bool m_macroPopulating=false,m_macroRecording=false,m_macroErasing=false,m_macroRendering=false,m_macroTimeInvalid=false;
        std::size_t m_macroCursor=0;
        struct MacroStepView {
            Microsoft::UI::Xaml::FrameworkElement element{nullptr};
            MacroEditor::Step step{};
            bool invalid=false,erasing=false;
            Microsoft::UI::Xaml::ElementTheme theme{};
        };
        std::map<std::size_t,MacroStepView> m_macroStepViews;
        std::vector<Windows::Foundation::Rect> m_macroStepLayout;
        std::vector<MacroEditor::Step> m_macroMeasuredSteps;
        std::vector<double> m_macroStepWidths;
        MacroEditor::LayoutState m_macroLayout;
        Microsoft::UI::Xaml::Controls::Button m_macroMeasureToken{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock m_macroMeasureText{nullptr};
        std::shared_ptr<std::atomic<bool>> m_macroLoadCanceled=std::make_shared<std::atomic<bool>>(false);
        bool m_macroLoading=false;
        std::optional<std::size_t> m_macroDrag;
        Windows::Foundation::Point m_macroDragPoint{},m_macroDragOffset{};
        std::vector<std::size_t> m_macroInvalidSteps;
        Microsoft::UI::Xaml::Controls::TextBox m_macroNameEditor{nullptr};
        Microsoft::UI::Xaml::Controls::TextBox m_macroTimeEditor{nullptr};
        std::optional<std::size_t> m_macroEditingTime;
        HHOOK m_macroKeyboardHook=nullptr,m_macroMouseHook=nullptr;
        std::chrono::steady_clock::time_point m_macroLastInput;
        bool m_macroHasInput=false;
        std::uint32_t m_macroLastInterval=0;
        std::unique_ptr<MacroRecordingLog::Recorder> m_macroRecordingLog;
        std::array<bool,256> m_macroHeldKeys{};
        bool m_macroSkipMouseUp=false;
        Microsoft::UI::Xaml::Controls::ContentDialog m_macroSwitchDialog{nullptr};
        Microsoft::UI::Xaml::Controls::ContentDialog m_macroDeleteDialog{nullptr};
        inline static MainWindow* s_macroRecorder=nullptr;
        bool m_buttonsLoaded=false,m_hasButtonSnapshot=false,m_buttonsCurrentValid=false;
        winrt::hstring m_buttonsReadMessage;
        Microsoft::UI::Xaml::Controls::MenuFlyout m_buttonFlyout{nullptr};
        std::size_t m_menuButton=0;
        Microsoft::UI::Xaml::Controls::ContentDialog m_buttonResetDialog{nullptr};
        Microsoft::UI::Xaml::Controls::ProgressBar m_buttonResetProgress{nullptr};
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_buttonResetTimer{nullptr};
        ULONGLONG m_buttonResetDeadline=0;
        bool m_buttonResetConfirmed=false,m_buttonResetSpacePressed=false,m_resetButtonApply=false;
        ManagerBackend::Settings m_baseline{}, m_draft{};
        Device::Result m_battery{};
        bool m_populating = true, m_loaded = false, m_dirty = false, m_applying = false;
        bool m_editorChangedSincePopulate = false;
        bool m_closePending = false, m_allowClose = false, m_closed = false;
        bool m_reading = true, m_statusAnimating = false;
        bool m_parameterReadFailureNotified=false;
        Microsoft::UI::Xaml::Controls::ContentDialog m_parameterReadFailureDialog{nullptr};
        Monitoring::Paths m_monitoringPaths;
        bool m_monitoringConfigured=false,m_monitoringPopulating=false;
        BatteryUsage::Report m_usage;
        std::shared_ptr<BatteryUsage::Reader> m_usageReader=std::make_shared<BatteryUsage::Reader>();
        bool m_usageLoading=false,m_usageReloadRequested=false;
        std::array<Microsoft::UI::Xaml::Controls::Border,BatteryUsage::DisplayDays> m_usageBars;
        std::array<Microsoft::UI::Xaml::Controls::TextBlock,BatteryUsage::DisplayDays> m_usageDates;
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_usageTimer{nullptr};
        winrt::hstring m_completionStatus, m_pendingStatus{ Localization::Get(Localization::T007) }, m_statusDetail;
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_statusAnimation{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_completionStatusTimer{ nullptr };
        HWND m_windowHandle = nullptr;
        HHOOK m_tooltipCreationHook=nullptr;
        bool m_dwmBackdropEnabled = false;
        Windows::UI::Composition::Compositor m_systemCompositor{nullptr};
        Windows::UI::Composition::CompositionColorBrush m_transparentBackdropBrush{nullptr};
        Microsoft::UI::Xaml::Media::SolidColorBrush m_batteryLightningBrush{nullptr}, m_batteryProgressBrush{nullptr};
        float m_batteryGeometrySize = 0;
        int m_liveCurrentStage = 0;
        std::function<void(ManagerBackend::Settings const&, ManagerBackend::Settings const&, ManagerBackend::ApplyMode)> m_applyHandler;
        std::function<void()> m_refreshHandler, m_closeHandler;

        enum class CalibrationPage { Intro, Preparing, PrepareFailed, Armed, Measuring, MeasureFailed, Restoring, RestoreFailed, CancelConfirm };
        CalibrationPage m_clockPage=CalibrationPage::Intro,m_clockPreviousPage=CalibrationPage::Intro;
        bool m_clockCalibrating=false,m_clockPopulating=false,m_clockFirstEnable=false,m_clockCanceling=false,m_clockAllowClose=false,m_clockRecoveryPending=false;
        bool m_clockInputRegistered=false,m_clockHeld=false,m_clockDialogOpened=false;
        HANDLE m_clockInputDevice=nullptr;
        ClockCalibration::Measurement m_clockMeasurement;
        Microsoft::UI::Xaml::Controls::ContentDialog m_clockDialog{nullptr};
        Windows::Foundation::IAsyncAction m_clockDialogTask{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock m_clockBody{nullptr};
        Microsoft::UI::Xaml::Controls::Grid m_clockReconnectIllustration{nullptr};
        Microsoft::UI::Xaml::Controls::Border m_clockInputBox{nullptr};
        Microsoft::UI::Xaml::FrameworkElement m_clockDialogSurface{nullptr};
        double m_clockDialogMinimumHeight=0;
        Microsoft::UI::Xaml::Controls::ProgressBar m_clockProgress{nullptr};
        Microsoft::UI::Xaml::Controls::Border m_clockFooter{nullptr};
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_clockTimer{nullptr};
        void InitializeClockCalibration();
        void UpdateClockControls();
        Microsoft::UI::Xaml::Controls::Border CreateDialogProgressFooter(Microsoft::UI::Xaml::Controls::ContentDialog const& dialog,Microsoft::UI::Xaml::Controls::ProgressBar const& progress);
        Windows::Foundation::IAsyncAction ShowClockCalibration(bool firstEnable=false,bool recovery=false);
        void SetClockPage(CalibrationPage page);
        void ClockDialogAction(bool primary);
        void CancelClockCalibration();
        void SubmitClockCalibration(ManagerBackend::ApplyMode mode);
        bool HandleClockSnapshot(ManagerBackend::Snapshot const& snapshot);
        bool StartClockInput();
        void StopClockInput();
        bool ClockPointerInside();
        void HandleClockInput(LPARAM input);
        void HandleClockRawInput(RAWINPUT const& input,bool targetDevice,bool inside,std::int64_t counter);
        void UpdateClockMeasurement();
        void CompleteClockCalibration();
        bool EncodeMacroForDevice(MacroEditor::Profile const& profile,std::vector<std::uint8_t>& data,std::wstring& error);
        bool CompileBoundMacros(std::wstring& error);

        void CreateStageRows();
        void InitializeMacroEditor();
        void LoadMacroLibrary();
        bool EnsureMacroProfile(MacroEditor::Profile& profile);
        void ReleaseMacroCache();
        bool m_macroLibraryRequested=false;
        void RefreshMacroReadbackLinks();
        void RefreshMacroMenus();
        std::array<bool,6> BoundMacroSlots(MacroEditor::Profile const& profile);
        void SyncSavedMacro(MacroEditor::Profile const& profile,std::array<bool,6> const& slots);
        void RenderMacroList(bool rename=false);
        void RenderMacroEditor();
        void RenderMacroSteps();
        void LayoutMacroSteps();
        void UpdateVisibleMacroSteps();
        void FocusMacroStep(std::size_t index);
        bool BeginMacroDrag(std::size_t index, Windows::Foundation::Point point, Windows::Foundation::Point offset);
        void UpdateMacroDrag(Windows::Foundation::Point point);
        void FinishMacroDrag(bool apply);
        bool CommitMacroTime();
        void UpdateMacroHeight(double contentHeight);
        void MacroErrorMessage(std::wstring_view text);
        bool MacroHasChanges();
        void UpdateMacroActions();
        bool SaveMacroDraft();
        Windows::Foundation::IAsyncOperation<bool> ConfirmMacroSwitch();
        Windows::Foundation::IAsyncAction SelectMacro(std::size_t index);
        Windows::Foundation::IAsyncAction ManageMacro(int operation);
        void PopulateMacroAddMenu();
        void InsertMacroStep(MacroEditor::Step step);
        void StopMacroRecording();
        void ToggleMacroRecording();
        void CaptureMacroStep(MacroEditor::Step step);
        void SelectMacroForButton(std::size_t index);
        void SelectMacroForButton(std::wstring const& profileId);
        static LRESULT CALLBACK MacroKeyboardHook(int code,WPARAM message,LPARAM data);
        static LRESULT CALLBACK MacroMouseHook(int code,WPARAM message,LPARAM data);
        static Microsoft::UI::Xaml::Controls::RichTextBlock FunctionLabel(std::wstring_view name,bool alignRight=false,bool logos=true);
        static void SetFunctionLabel(Microsoft::UI::Xaml::Controls::RichTextBlock const& label,std::wstring_view name,bool logos=true);
        static std::wstring MenuFunctionName(std::wstring_view name,bool logos=true);
        void CreateButtonRows();
        void UpdateButtonSnapshot(ManagerBackend::Snapshot const& snapshot);
        void UpdateButtonLabels(bool force=false);
        void PopulateButtonMenu();
        void SelectButtonFunction(ManagerBackend::ButtonCode const& code);
        Microsoft::UI::Xaml::Controls::ContentDialog CreateButtonResetDialog();
        void ShowButtonResetDialog();
        void UpdateButtonResetCountdown(ULONGLONG now);
        bool HandleButtonResetKey(Windows::System::VirtualKey key,bool released);
        void ConfirmButtonReset();
        void CancelButtonReset();
        void ResetButtonMappings();
        void UpdateMonitoringControls();
        void CreateUsageChart();
        void RefreshUsage();
        void RenderUsage();
        void SetStatusMessage(winrt::hstring const& text);
        void AnimateStatus();
        void UpdateStatus();
        void UpdateStatusPresentation();
        void UpdateBackdropTheme();
        static LRESULT CALLBACK BackdropWindowProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
        void Populate();
        void Edited();
        bool ReadDraft(std::wstring& message);
        void UpdateActions();
        void UpdateStageRows();
        void UpdateBattery();
        void UpdateBatteryLayout(bool expanded);
        void UpdateBatteryGeometry();
        void ShowColorPicker(std::size_t index);
        Windows::Foundation::IAsyncAction ConfirmClose();
        void RetryReadParameters();
        bool ShouldPromptParameterReadFailure(ManagerBackend::Snapshot const& snapshot) const;
        Microsoft::UI::Xaml::Controls::ContentDialog CreateParameterReadFailureDialog();
        Microsoft::UI::Xaml::Controls::Grid CreatePowerSwitchIllustration(Microsoft::UI::Xaml::Controls::ContentDialog const& dialog);
        void PromptParameterReadFailure();
    };
}

namespace winrt::G3Companion::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
