#include "pch.h"
#include "../localization.h"
#include "MainWindow.xaml.h"
#include <cwctype>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Windows::Foundation;

namespace winrt::G3Companion::implementation {
    namespace {
        FrameworkElement ClockDialogPart(DependencyObject const& parent,wchar_t const* name){
            if(auto element=parent.try_as<FrameworkElement>();element&&element.Name()==name)return element;
            for(int i=0;i<VisualTreeHelper::GetChildrenCount(parent);++i)
                if(auto found=ClockDialogPart(VisualTreeHelper::GetChild(parent,i),name))return found;
            return nullptr;
        }
    }
    Border MainWindow::CreateDialogProgressFooter(ContentDialog const& dialog,ProgressBar const& progress){
        auto commands=ClockDialogPart(dialog,L"CommandSpace").as<Grid>();
        auto space=ClockDialogPart(dialog,L"DialogSpace").as<Grid>();
        Border footer;const auto padding=commands.Padding();footer.Padding(padding);footer.Background(commands.Background());
        // Keep the native options row height when the buttons are replaced by progress.
        footer.MinHeight(32+padding.Top+padding.Bottom);footer.Child(progress);progress.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetRow(footer,1);footer.Visibility(progress.Visibility());space.Children().Append(footer);return footer;
    }
    void MainWindow::InitializeClockCalibration(){
        ClockCalibrateButton().Click([this](auto const&,auto const&){m_clockDialogTask=ShowClockCalibration();});
        ClockCompensationToggle().Toggled([this](auto const&,auto const&){
            if(m_clockPopulating||m_clockCalibrating||m_populating)return;
            m_draft.clockCompensationEnabled=ClockCompensationToggle().IsOn();
            if(m_draft.clockCompensationEnabled&&!ClockCompensation::Valid(m_draft.clockCompensationFactor)){m_clockDialogTask=ShowClockCalibration(true);return;}
            Edited();UpdateClockControls();
        });
        m_clockTimer=DispatcherQueue().CreateTimer();m_clockTimer.Interval(std::chrono::milliseconds(50));
        auto weak=get_weak();m_clockTimer.Tick([weak](auto const&,auto const&){if(auto self=weak.get()){
            LARGE_INTEGER counter{};QueryPerformanceCounter(&counter);
            if(self->m_clockMeasurement.status==ClockCalibration::Measurement::Status::Running){
                if(!self->ClockPointerInside())self->m_clockMeasurement.Fail(ClockCalibration::Measurement::Status::PointerOutside);
                self->m_clockMeasurement.Tick(counter.QuadPart);self->UpdateClockMeasurement();
            }
            // Wait for the test macro's release before enabling a retry or restoring its data.
            if(self->m_clockHeld&&(counter.QuadPart-self->m_clockMeasurement.start)*1000.0/self->m_clockMeasurement.frequency>=ClockCalibration::TimeoutMs){self->m_clockHeld=false;self->m_clockDialog.IsSecondaryButtonEnabled(true);}
        }});
        UpdateClockControls();
    }
    void MainWindow::UpdateClockControls(){
        m_clockPopulating=true;ClockCompensationToggle().IsOn(m_draft.clockCompensationEnabled);m_clockPopulating=false;
        wchar_t value[64]{};if(ClockCompensation::Valid(m_draft.clockCompensationFactor))swprintf_s(value,L"%.2f%%",(m_draft.clockCompensationFactor-1)*100);
        ClockFactorText().Text(value);
    }
    bool MainWindow::EncodeMacroForDevice(MacroEditor::Profile const& profile,std::vector<std::uint8_t>& data,std::wstring& error){
        return MacroEditor::Encode(profile,to_string(profile.name),data,error)&&ClockCompensation::ScaleData(data,ClockCompensation::Effective({m_draft.clockCompensationEnabled,m_draft.clockCompensationFactor}),error);
    }
    bool MainWindow::CompileBoundMacros(std::wstring& error){
        const bool clockChanged=m_draft.clockCompensationEnabled!=m_baseline.clockCompensationEnabled||m_draft.clockCompensationFactor!=m_baseline.clockCompensationFactor;
        auto assignments=m_draft.macroAssignments;
        for(auto& profile:m_macroProfiles){auto slots=BoundMacroSlots(profile);bool needed=false;
            for(std::size_t i=0;i<6;++i)needed|=slots[i]&&(clockChanged||assignments[i].has_value());
            if(!needed)continue;if(!EnsureMacroProfile(profile)){error=Localization::Get(Localization::T185);return false;}MacroEditor::Assignment assignment;
            if(!EncodeMacroForDevice(profile,assignment.data,error))return false;
            assignment.profileId=profile.id;assignment.name=profile.name;assignment.executionMode=static_cast<std::uint8_t>(profile.executionMode);assignment.repeatCount=static_cast<std::uint8_t>(profile.repeatCount);
            for(std::size_t i=0;i<6;++i)if(slots[i])assignments[i]=assignment;
        }
        ReleaseMacroCache();
        for(auto& assignment:assignments)if(assignment&&!assignment->clockSourceData.empty()){
            assignment->data=assignment->clockSourceData;
            if(!ClockCompensation::ScaleData(assignment->data,ClockCompensation::Effective({m_draft.clockCompensationEnabled,m_draft.clockCompensationFactor})/assignment->clockSourceFactor,error))return false;
        }
        if(clockChanged){
            const auto ratio=ClockCompensation::Effective({m_draft.clockCompensationEnabled,m_draft.clockCompensationFactor})/ClockCompensation::Effective({m_baseline.clockCompensationEnabled,m_baseline.clockCompensationFactor});
            for(std::size_t i=0;i<6;++i)if(m_draft.buttons[i][0]==3&&!assignments[i]){
                const auto found=std::find_if(m_macroSnapshot->begin(),m_macroSnapshot->end(),[&](auto const& macro){return macro.binding==m_draft.buttons[i];});
                if(found==m_macroSnapshot->end()||found->data.empty()){error=Localization::Get(Localization::T216);return false;}
                MacroEditor::Assignment assignment;assignment.profileId=L"device-macro-"+std::to_wstring(m_draft.buttons[i][3]);assignment.name=found->name;assignment.data=found->data;
                assignment.clockSourceData=found->data;assignment.clockSourceFactor=ClockCompensation::Effective({m_baseline.clockCompensationEnabled,m_baseline.clockCompensationFactor});
                assignment.executionMode=m_draft.buttons[i][1];assignment.repeatCount=m_draft.buttons[i][2];
                if(!ClockCompensation::ScaleData(assignment.data,ratio,error))return false;assignments[i]=std::move(assignment);
            }
        }
        m_draft.macroAssignments=std::move(assignments);return true;
    }
    IAsyncAction MainWindow::ShowClockCalibration(bool firstEnable,bool recovery){
        auto lifetime=get_strong();
        if(m_clockCalibrating||m_applying||m_macroRecording||m_buttonResetDialog||m_parameterReadFailureDialog||m_closePending||(!recovery&&(!m_loaded||!m_buttonsLoaded||!m_applyHandler)))co_return;
        m_clockCalibrating=true;m_clockFirstEnable=firstEnable;m_clockCanceling=recovery;m_clockAllowClose=false;m_clockRecoveryPending=recovery;
        m_clockHeld=false;m_clockInputDevice=nullptr;m_clockDialogOpened=false;Navigation().SelectedItem(ButtonsItem());UpdateActions();
        try{
        ContentDialog dialog;dialog.XamlRoot(RootGrid().XamlRoot());dialog.DefaultButton(ContentDialogButton::None);
        dialog.HorizontalContentAlignment(HorizontalAlignment::Stretch);dialog.VerticalContentAlignment(VerticalAlignment::Top);
        dialog.Style(Application::Current().Resources().Lookup(box_value(L"DefaultContentDialogStyle")).as<Style>());
        StackPanel content;content.Spacing(16);content.MinWidth(320);
        m_clockBody=TextBlock();m_clockBody.TextWrapping(TextWrapping::Wrap);content.Children().Append(m_clockBody);
        m_clockReconnectIllustration=CreatePowerSwitchIllustration(dialog);
        m_clockReconnectIllustration.Visibility(Visibility::Collapsed);content.Children().Append(m_clockReconnectIllustration);
        m_clockInputBox=Border();m_clockInputBox.Height(160);m_clockInputBox.CornerRadius(CornerRadius{8,8,8,8});
        m_clockInputBox.PointerExited([this](auto const&,auto const&){if(m_clockPage==CalibrationPage::Measuring&&m_clockMeasurement.status==ClockCalibration::Measurement::Status::Running){m_clockMeasurement.Fail(ClockCalibration::Measurement::Status::PointerOutside);UpdateClockMeasurement();}});
        content.Children().Append(m_clockInputBox);
        m_clockProgress=ProgressBar();m_clockProgress.IsIndeterminate(true);dialog.Content(content);
        m_clockDialog=dialog;auto weak=get_weak();
        dialog.Opened([weak](auto const&,auto const&){if(auto self=weak.get()){
            auto commands=ClockDialogPart(self->m_clockDialog,L"CommandSpace").as<Grid>();
            self->m_clockFooter=self->CreateDialogProgressFooter(self->m_clockDialog,self->m_clockProgress);
            const auto padding=commands.Padding();
            auto viewport=ClockDialogPart(self->m_clockDialog,L"ContentScrollViewer").as<ScrollViewer>();
            viewport.HorizontalContentAlignment(HorizontalAlignment::Stretch);viewport.VerticalContentAlignment(VerticalAlignment::Top);
            // Disabled horizontal scrolling still measures content beyond the viewport.
            // Constrain the page to the padded viewport to keep all frame corners inside it.
            auto page=self->m_clockDialog.Content().as<StackPanel>();
            const auto fit=[page,padding](FrameworkElement const& element){
                const auto width=std::max(0.0,element.ActualWidth()-padding.Left-padding.Right);
                if(width>0&&page.Width()!=width)page.Width(width);
            };
            fit(viewport);viewport.SizeChanged([fit](auto const& sender,auto const&){fit(sender.template as<FrameworkElement>());});
            self->m_clockDialog.UpdateLayout();self->m_clockDialogOpened=true;
        }});
        dialog.PreviewKeyDown([weak](auto const&,Input::KeyRoutedEventArgs const& args){if(args.Key()==Windows::System::VirtualKey::Escape){args.Handled(true);if(auto self=weak.get())self->CancelClockCalibration();}});
        dialog.PrimaryButtonClick([weak](auto const&,ContentDialogButtonClickEventArgs const& args){args.Cancel(true);if(auto self=weak.get())self->ClockDialogAction(false);});
        dialog.SecondaryButtonClick([weak](auto const&,ContentDialogButtonClickEventArgs const& args){args.Cancel(true);if(auto self=weak.get())self->ClockDialogAction(true);});
        dialog.Closing([weak](auto const&,ContentDialogClosingEventArgs const& args){if(auto self=weak.get();self&&!self->m_clockAllowClose){args.Cancel(true);self->CancelClockCalibration();}});
        SetClockPage(recovery?CalibrationPage::Restoring:CalibrationPage::Intro);
        auto shown=dialog.ShowAsync();if(recovery)SubmitClockCalibration(ManagerBackend::ApplyMode::RestoreClockCalibration);
        co_await shown;
        }catch(hresult_error const& error){m_clockCanceling=true;m_statusDetail=Localization::Format(Localization::T217,{{L"details",error.message().c_str()}});UpdateStatus();}
        StopClockInput();m_clockDialog=nullptr;m_clockBody=nullptr;m_clockReconnectIllustration=nullptr;m_clockInputBox=nullptr;m_clockProgress=nullptr;m_clockFooter=nullptr;m_clockDialogSurface=nullptr;
        m_clockCalibrating=false;
        if(firstEnable&&m_clockCanceling)m_draft.clockCompensationEnabled=false;
        UpdateClockControls();Edited();UpdateActions();
    }
    void MainWindow::SetClockPage(CalibrationPage page){
        if(m_clockDialogSurface&&page!=CalibrationPage::Measuring){m_clockDialogSurface.MinHeight(m_clockDialogMinimumHeight);m_clockDialogSurface=nullptr;}
        if(page==CalibrationPage::Measuring&&!m_clockDialogSurface){
            const auto findSurface=[](auto const& recurse,DependencyObject const& parent)->FrameworkElement{
                if(auto element=parent.try_as<FrameworkElement>();element&&element.Name()==L"BackgroundElement")return element;
                for(int i=0;i<VisualTreeHelper::GetChildrenCount(parent);++i)if(auto found=recurse(recurse,VisualTreeHelper::GetChild(parent,i)))return found;return nullptr;
            };
            m_clockDialogSurface=findSurface(findSurface,m_clockDialog);
            if(m_clockDialogSurface){m_clockDialogMinimumHeight=m_clockDialogSurface.MinHeight();m_clockDialogSurface.MinHeight(m_clockDialogSurface.ActualHeight());}
        }
        m_clockPage=page;m_clockDialog.Title(box_value(Localization::Get(Localization::T218)));m_clockDialog.PrimaryButtonText(L"");m_clockDialog.SecondaryButtonText(L"");m_clockDialog.CloseButtonText(L"");m_clockDialog.IsSecondaryButtonEnabled(!m_clockHeld);
        m_clockInputBox.Visibility(page==CalibrationPage::Armed||page==CalibrationPage::Measuring?Visibility::Visible:Visibility::Collapsed);m_clockProgress.Visibility(Visibility::Collapsed);
        m_clockReconnectIllustration.Visibility(page==CalibrationPage::PrepareFailed||page==CalibrationPage::RestoreFailed?Visibility::Visible:Visibility::Collapsed);
        switch(page){
        case CalibrationPage::Intro:
            m_clockBody.Text(Localization::Get(Localization::T219));m_clockDialog.PrimaryButtonText(Localization::Get(Localization::T198));m_clockDialog.SecondaryButtonText(Localization::Get(Localization::T220));break;
        case CalibrationPage::Preparing:m_clockBody.Text(Localization::Get(Localization::T221));m_clockProgress.Visibility(Visibility::Visible);break;
        case CalibrationPage::PrepareFailed:m_clockDialog.Title(box_value(Localization::Get(Localization::T222)));m_clockBody.Text(Localization::Get(Localization::T223));m_clockDialog.PrimaryButtonText(Localization::Get(Localization::T198));m_clockDialog.SecondaryButtonText(Localization::Get(Localization::T224));break;
        case CalibrationPage::Armed:case CalibrationPage::Measuring:
            m_clockDialog.Title(box_value(Localization::Get(Localization::T225)));m_clockBody.Text(Localization::Get(Localization::T226));m_clockInputBox.Visibility(Visibility::Visible);
            m_clockInputBox.Background(page==CalibrationPage::Measuring?
                Application::Current().Resources().Lookup(box_value(L"AccentFillColorDefaultBrush")).as<Brush>():
                RootGrid().Resources().Lookup(box_value(L"ClockCalibrationIdleFillBrush")).as<Brush>());
            if(page==CalibrationPage::Armed)m_clockDialog.PrimaryButtonText(Localization::Get(Localization::T198));else m_clockProgress.Visibility(Visibility::Visible);break;
        case CalibrationPage::MeasureFailed:
            m_clockDialog.Title(box_value(Localization::Get(Localization::T227)));
            switch(m_clockMeasurement.status){
            case ClockCalibration::Measurement::Status::TimedOut:m_clockBody.Text(Localization::Get(Localization::T228));break;
            case ClockCalibration::Measurement::Status::PointerOutside:m_clockBody.Text(Localization::Get(Localization::T229));break;
            default:m_clockBody.Text(Localization::Get(Localization::T230));break;
            }
            m_clockDialog.PrimaryButtonText(Localization::Get(Localization::T198));m_clockDialog.SecondaryButtonText(Localization::Get(Localization::T224));break;
        case CalibrationPage::Restoring:m_clockDialog.Title(box_value(m_clockCanceling?Localization::Get(Localization::T231):Localization::Get(Localization::T232)));m_clockBody.Text(Localization::Get(Localization::T233));m_clockProgress.Visibility(Visibility::Visible);break;
        case CalibrationPage::RestoreFailed:m_clockDialog.Title(box_value(Localization::Get(Localization::T234)));m_clockBody.Text(Localization::Get(Localization::T235));m_clockDialog.SecondaryButtonText(Localization::Get(Localization::T224));break;
        case CalibrationPage::CancelConfirm:m_clockDialog.Title(box_value(Localization::Get(Localization::T231)));m_clockBody.Text(Localization::Get(Localization::T236));m_clockDialog.PrimaryButtonText(Localization::Get(Localization::T237));m_clockDialog.SecondaryButtonText(Localization::Get(Localization::T238));break;
        }
        if(m_clockFooter)m_clockFooter.Visibility(m_clockProgress.Visibility());
    }
    void MainWindow::ClockDialogAction(bool primary){
        if(!m_clockDialog)return;
        switch(m_clockPage){
        case CalibrationPage::Intro:if(primary)SubmitClockCalibration(ManagerBackend::ApplyMode::PrepareClockCalibration);else{m_clockCanceling=true;CompleteClockCalibration();}break;
        case CalibrationPage::PrepareFailed:if(primary)SubmitClockCalibration(ManagerBackend::ApplyMode::PrepareClockCalibration);else CancelClockCalibration();break;
        case CalibrationPage::Armed:if(!primary)CancelClockCalibration();break;
        case CalibrationPage::MeasureFailed:
            if(primary&&!m_clockHeld){if(!StartClockInput()){m_clockMeasurement.Fail();SetClockPage(CalibrationPage::MeasureFailed);}else SetClockPage(CalibrationPage::Armed);}else if(!primary)CancelClockCalibration();break;
        case CalibrationPage::RestoreFailed:if(primary)SubmitClockCalibration(ManagerBackend::ApplyMode::RestoreClockCalibration);break;
        case CalibrationPage::CancelConfirm:
            if(!primary){SetClockPage(m_clockPreviousPage);break;}
            if(m_clockHeld)return;m_clockCanceling=true;
            if(m_clockRecoveryPending)SubmitClockCalibration(ManagerBackend::ApplyMode::RestoreClockCalibration);else CompleteClockCalibration();break;
        default:break;
        }
    }
    void MainWindow::CancelClockCalibration(){
        if(!m_clockCalibrating||!m_clockDialog)return;
        if(m_clockPage==CalibrationPage::Intro){m_clockCanceling=true;CompleteClockCalibration();return;}
        if(m_clockPage!=CalibrationPage::Armed&&m_clockPage!=CalibrationPage::MeasureFailed&&m_clockPage!=CalibrationPage::PrepareFailed)return;
        m_clockPreviousPage=m_clockPage;SetClockPage(CalibrationPage::CancelConfirm);
    }
    void MainWindow::SubmitClockCalibration(ManagerBackend::ApplyMode mode){
        StopClockInput();SetClockPage(mode==ManagerBackend::ApplyMode::PrepareClockCalibration?CalibrationPage::Preparing:CalibrationPage::Restoring);
        try{if(!m_applyHandler)throw hresult_error(E_FAIL);m_applyHandler(m_draft,m_baseline,mode);}
        catch(...){SetClockPage(mode==ManagerBackend::ApplyMode::PrepareClockCalibration?CalibrationPage::PrepareFailed:CalibrationPage::RestoreFailed);}
    }
    bool MainWindow::HandleClockSnapshot(ManagerBackend::Snapshot const& snapshot){
        const bool operation=snapshot.calibrationOperation==ManagerBackend::ApplyMode::PrepareClockCalibration||snapshot.calibrationOperation==ManagerBackend::ApplyMode::RestoreClockCalibration;
        if(operation){m_clockRecoveryPending=snapshot.calibrationRecoveryPending;
            if(!m_clockDialog)return true;
            if(snapshot.calibrationOperation==ManagerBackend::ApplyMode::PrepareClockCalibration){
                if(!snapshot.calibrationSucceeded)SetClockPage(CalibrationPage::PrepareFailed);
                else if(!StartClockInput()){m_clockMeasurement.Fail();SetClockPage(CalibrationPage::MeasureFailed);}
                else SetClockPage(CalibrationPage::Armed);
            }else if(!snapshot.calibrationSucceeded)SetClockPage(CalibrationPage::RestoreFailed);else CompleteClockCalibration();
            return true;
        }
        if(m_clockCalibrating){
            if(snapshot.status.state==MouseState::Mode::Offline||snapshot.status.state==MouseState::Mode::Missing){
                if(m_clockPage==CalibrationPage::Armed||m_clockPage==CalibrationPage::Measuring){m_clockMeasurement.Fail();UpdateClockMeasurement();}
            }
            return true;
        }
        m_clockRecoveryPending=snapshot.calibrationRecoveryPending;
        if(m_clockRecoveryPending){m_clockDialogTask=ShowClockCalibration(false,true);return true;}
        return false;
    }
    void MainWindow::CompleteClockCalibration(){
        StopClockInput();
        if(!m_clockCanceling&&m_clockMeasurement.status==ClockCalibration::Measurement::Status::Complete){m_draft.clockCompensationFactor=m_clockMeasurement.factor;}
        m_clockAllowClose=true;
        // Dismiss after the native button event returns; Hide inside that event is reentrant.
        auto dialog=m_clockDialog;DispatcherQueue().TryEnqueue([dialog]{dialog.Hide();});
    }
    bool MainWindow::StartClockInput(){
        LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);m_clockMeasurement.Reset(frequency.QuadPart);m_clockInputDevice=nullptr;m_clockHeld=false;
        {RAWINPUTDEVICE devices[]={{1,2,RIDEV_INPUTSINK,m_windowHandle},{1,6,RIDEV_INPUTSINK,m_windowHandle}};
            if(!RegisterRawInputDevices(devices,2,sizeof(RAWINPUTDEVICE)))return false;m_clockInputRegistered=true;m_clockTimer.Start();}
        return true;
    }
    void MainWindow::StopClockInput(){
        if(m_clockTimer)m_clockTimer.Stop();
        if(m_clockInputRegistered){RAWINPUTDEVICE devices[]={{1,2,RIDEV_REMOVE,nullptr},{1,6,RIDEV_REMOVE,nullptr}};RegisterRawInputDevices(devices,2,sizeof(RAWINPUTDEVICE));m_clockInputRegistered=false;}
    }
    bool MainWindow::ClockPointerInside(){
        if(!m_clockInputBox||m_clockInputBox.Visibility()!=Visibility::Visible||GetForegroundWindow()!=m_windowHandle||IsIconic(m_windowHandle))return false;
        POINT point{};if(!GetCursorPos(&point)||!ScreenToClient(m_windowHandle,&point))return false;
        const auto origin=m_clockInputBox.TransformToVisual(RootGrid()).TransformPoint({0,0});const auto scale=RootGrid().XamlRoot().RasterizationScale();
        return point.x>=origin.X*scale&&point.y>=origin.Y*scale&&point.x<(origin.X+m_clockInputBox.ActualWidth())*scale&&point.y<(origin.Y+m_clockInputBox.ActualHeight())*scale;
    }
    void MainWindow::HandleClockInput(LPARAM input){
        if(!m_clockInputRegistered)return;LARGE_INTEGER counter{};QueryPerformanceCounter(&counter);
        if(m_clockPage!=CalibrationPage::Armed&&m_clockPage!=CalibrationPage::Measuring&&!m_clockHeld)return;
        RAWINPUT raw{};UINT size=sizeof(raw);
        if(GetRawInputData(reinterpret_cast<HRAWINPUT>(input),RID_INPUT,&raw,&size,sizeof(RAWINPUTHEADER))==UINT(-1))return;
        if(raw.header.dwType==RIM_TYPEMOUSE){
            HandleClockRawInput(raw,false,ClockPointerInside(),counter.QuadPart);return;
        }
        if(raw.header.dwType!=RIM_TYPEKEYBOARD)return;
        UINT count=0;GetRawInputDeviceInfoW(raw.header.hDevice,RIDI_DEVICENAME,nullptr,&count);std::wstring path(count,L'\0');
        if(count&&GetRawInputDeviceInfoW(raw.header.hDevice,RIDI_DEVICENAME,path.data(),&count)!=UINT(-1)){for(auto& c:path)c=static_cast<wchar_t>(towlower(c));}else path.clear();
        const auto product=m_battery.state==MouseState::Mode::Wired?L"vid_258a&pid_0136":L"vid_258a&pid_0138";
        HandleClockRawInput(raw,path.find(product)!=std::wstring::npos,ClockPointerInside(),counter.QuadPart);
    }
    void MainWindow::HandleClockRawInput(RAWINPUT const& raw,bool targetDevice,bool inside,std::int64_t counter){
        const bool keyboard=raw.header.dwType==RIM_TYPEKEYBOARD;
        const bool insert=keyboard&&raw.data.keyboard.VKey==VK_INSERT&&(raw.data.keyboard.Flags&RI_KEY_E0);
        const bool down=insert&&!(raw.data.keyboard.Flags&RI_KEY_BREAK),up=insert&&!down;
        const bool target=targetDevice&&(!m_clockInputDevice||m_clockInputDevice==raw.header.hDevice);
        // A failed sample may still have Insert held; consume its release before retrying.
        if(m_clockHeld&&target&&up&&m_clockPage!=CalibrationPage::Measuring){m_clockHeld=false;m_clockDialog.IsSecondaryButtonEnabled(true);return;}
        if(m_clockPage!=CalibrationPage::Armed&&m_clockPage!=CalibrationPage::Measuring)return;
        // Validate other input only after the measurement macro starts.
        if(m_clockPage==CalibrationPage::Armed&&!(target&&inside&&down))return;
        if(raw.header.dwType==RIM_TYPEMOUSE){
            if(!inside){m_clockMeasurement.Fail(ClockCalibration::Measurement::Status::PointerOutside);UpdateClockMeasurement();}
            else if(raw.data.mouse.usButtonFlags){m_clockMeasurement.Fail();UpdateClockMeasurement();}
            return;
        }
        if(!keyboard)return;
        // Holding a keyboard key can produce typematic make events; they do not restart timing.
        if(target&&inside&&down&&m_clockHeld)return;
        const auto old=m_clockMeasurement.status;m_clockMeasurement.Button(target,inside,down,up,!insert,counter);
        if(target&&down&&!m_clockHeld){
            m_clockInputDevice=raw.header.hDevice;m_clockHeld=true;
            if(old==ClockCalibration::Measurement::Status::Armed)m_clockMeasurement.start=counter;
        }
        if(target&&up)m_clockHeld=false;
        UpdateClockMeasurement();
    }
    void MainWindow::UpdateClockMeasurement(){
        using Status=ClockCalibration::Measurement::Status;
        switch(m_clockMeasurement.status){
        case Status::Running:if(m_clockPage==CalibrationPage::Armed)SetClockPage(CalibrationPage::Measuring);break;
        case Status::Complete:m_clockHeld=false;if(ProgramLog::Enabled())ProgramLog::Write("clock_calibration_measured","elapsed_ms="+std::to_string(m_clockMeasurement.elapsedMs)+";factor="+std::to_string(m_clockMeasurement.factor));SubmitClockCalibration(ManagerBackend::ApplyMode::RestoreClockCalibration);break;
        case Status::UnexpectedInput:case Status::PointerOutside:case Status::TimedOut:if(m_clockPage==CalibrationPage::Armed||m_clockPage==CalibrationPage::Measuring)SetClockPage(CalibrationPage::MeasureFailed);break;
        default:break;
        }
    }
}
