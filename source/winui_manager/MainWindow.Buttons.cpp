#include "pch.h"
#include "../localization.h"
#include "MainWindow.xaml.h"
#include "../button_catalog.h"
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <chrono>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Documents;
using namespace winrt::Microsoft::UI::Xaml::Media;

namespace winrt::G3Companion::implementation {
    RichTextBlock MainWindow::FunctionLabel(std::wstring_view name,bool alignRight,bool logos) {
        RichTextBlock label;label.FontSize(14);label.TextWrapping(TextWrapping::NoWrap);label.IsTextSelectionEnabled(false);
        label.TextTrimming(TextTrimming::CharacterEllipsis);label.VerticalAlignment(VerticalAlignment::Center);
        label.TextAlignment(alignRight?TextAlignment::Right:TextAlignment::Left);
        SetFunctionLabel(label,name,logos);return label;
    }
    void MainWindow::SetFunctionLabel(RichTextBlock const& label,std::wstring_view name,bool logos) {
        label.Blocks().Clear();Paragraph paragraph;std::size_t offset=0;
        while(offset<name.size()){
            const auto logo=logos?name.find(L"Windows",offset):std::wstring_view::npos;
            const auto end=logo==std::wstring_view::npos?name.size():logo;
            if(end>offset){Run text;text.Text(hstring(name.substr(offset,end-offset)));paragraph.Inlines().Append(text);}
            if(logo==std::wstring_view::npos)break;
            Shapes::Path symbol;symbol.Width(12);symbol.Height(12);symbol.Stretch(Stretch::Uniform);
            GeometryGroup windowsLogo;
            for(float y:{0.0f,6.0f})for(float x:{0.0f,6.0f}){
                RectangleGeometry square;square.Rect({x,y,5,5});windowsLogo.Children().Append(square);
            }
            symbol.Data(windowsLogo);
            Data::Binding foreground;foreground.Source(label);foreground.Path(PropertyPath(L"Foreground"));
            symbol.SetBinding(Shapes::Shape::FillProperty(),foreground);
            InlineUIContainer inlineLogo;inlineLogo.Child(symbol);paragraph.Inlines().Append(inlineLogo);
            offset=logo+7;
        }
        label.Blocks().Append(paragraph);
    }
    std::wstring MainWindow::MenuFunctionName(std::wstring_view name,bool logos) {
        std::wstring text(name);
        if(logos){
            std::size_t offset=0;
            while((offset=text.find(L"Windows",offset))!=std::wstring::npos){
                text.replace(offset,7,L"\uE138");++offset;
            }
        }
        return text;
    }
}

namespace winrt::G3Companion::implementation {
    void MainWindow::CreateButtonRows() {
        m_buttonPreviewHosts={LeftButtonPreview(),RightButtonPreview(),MiddleButtonPreview(),ForwardButtonPreview(),BackButtonPreview(),CenterButtonPreview()};
        for(std::size_t index=0;index<m_buttonSelectors.size();++index){
            if(index){Border separator;separator.Style(RootGrid().Resources().Lookup(box_value(L"SettingsSeparatorStyle")).as<Style>());ButtonRows().Children().Append(separator);}
            Border row;row.Style(RootGrid().Resources().Lookup(box_value(L"SettingsRowStyle")).as<Style>());
            Grid grid;grid.ColumnSpacing(16);
            ColumnDefinition titleColumn,selectorColumn;titleColumn.Width({1,GridUnitType::Star});selectorColumn.Width({1,GridUnitType::Auto});
            grid.ColumnDefinitions().Append(titleColumn);grid.ColumnDefinitions().Append(selectorColumn);
            TextBlock title;title.Text(hstring(ButtonCatalog::PhysicalButtons[index]));
            title.Style(RootGrid().Resources().Lookup(box_value(L"SettingLabelStyle")).as<Style>());grid.Children().Append(title);
            auto selector=m_buttonSelectors[index];selector.Width(260);selector.MinHeight(32);selector.Padding({12,4,10,4});
            selector.HorizontalAlignment(HorizontalAlignment::Right);selector.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            selector.VerticalAlignment(VerticalAlignment::Center);
            Automation::AutomationProperties::SetName(selector,Localization::Format(Localization::T337,{{L"button",ButtonCatalog::PhysicalButtons[index]}}));
            MenuFlyout menu;
            menu.Opening([this,index](auto const& sender,auto const&){
                m_menuButton=index;m_buttonFlyout=sender.template as<MenuFlyout>();PopulateButtonMenu();
            });selector.Flyout(menu);
            Grid::SetColumn(selector,1);grid.Children().Append(selector);row.Child(grid);ButtonRows().Children().Append(row);
        }
        Input::KeyboardAccelerator resetShortcut;resetShortcut.Key(Windows::System::VirtualKey::R);
        resetShortcut.Modifiers(Windows::System::VirtualKeyModifiers::Control|Windows::System::VirtualKeyModifiers::Shift|Windows::System::VirtualKeyModifiers::Menu);
        resetShortcut.Invoked([this](auto const&,Input::KeyboardAcceleratorInvokedEventArgs const& args){
            if(ButtonsPage().Visibility()!=Visibility::Visible||GetForegroundWindow()!=m_windowHandle)return;
            args.Handled(true);ShowButtonResetDialog();
        });ButtonsPage().KeyboardAcceleratorPlacementMode(Input::KeyboardAcceleratorPlacementMode::Hidden);
        ButtonsPage().KeyboardAccelerators().Append(resetShortcut);
        m_buttonResetTimer=DispatcherQueue().CreateTimer();m_buttonResetTimer.Interval(std::chrono::milliseconds(16));
        auto weak=get_weak();m_buttonResetTimer.Tick([weak](auto const&,auto const&){
            if(auto self=weak.get();self&&!self->m_closed)self->UpdateButtonResetCountdown(GetTickCount64());
        });
        UpdateButtonLabels();
    }
    void MainWindow::UpdateButtonSnapshot(ManagerBackend::Snapshot const& snapshot) {
        using MouseState::Mode;
        const auto state=snapshot.status.state;
        const bool connected=state==Mode::Wireless||state==Mode::Charging||state==Mode::Wired||state==Mode::AwaitingBattery;
        m_buttonsCurrentValid=snapshot.buttonsValid && connected;
        m_buttonsLoaded=m_buttonsCurrentValid;
        m_buttonsReadMessage.clear();
        if(m_buttonsCurrentValid){const bool changed=m_liveButtons!=snapshot.settings.buttons||m_macroSnapshot!=snapshot.macros;
            m_liveButtons=snapshot.settings.buttons;
            if(m_macroSnapshot!=snapshot.macros){m_macroSnapshot=snapshot.macros;}
            m_hasButtonSnapshot=true;if(changed)RefreshMacroReadbackLinks();}
        else if(connected && snapshot.configurationValid)m_buttonsReadMessage=Localization::Get(Localization::T044);
        if(!m_buttonsLoaded && m_buttonFlyout)m_buttonFlyout.Hide();
        if(!m_buttonsLoaded)CancelButtonReset();
    }
    void MainWindow::UpdateButtonLabels(bool force) {
        for(std::size_t index=0;index<m_buttonSelectors.size();++index){
            if(!m_buttonSelectors[index] || !m_buttonPreviewHosts[index])continue;
            const auto draftName=m_hasButtonSnapshot?(m_draft.macroAssignments[index]?m_draft.macroAssignments[index]->name:ButtonCatalog::FunctionName(m_draft.buttons[index],*m_macroSnapshot)):Localization::Get(Localization::T045);
            auto& previous=m_buttonLabels[index];const bool draftLogo=m_draft.buttons[index][0]!=3;
            if(force||!previous.initialized||previous.draftName!=draftName||previous.draftLogo!=draftLogo){
                m_buttonSelectors[index].Content(FunctionLabel(draftName,false,draftLogo));
                Automation::AutomationProperties::SetName(m_buttonSelectors[index],hstring(ButtonCatalog::PhysicalButtons[index])+L"："+draftName);
                previous.draftName=draftName;previous.draftLogo=draftLogo;
            }
            std::wstring currentName;
            if(m_buttonsCurrentValid)currentName=ButtonCatalog::FunctionName(m_liveButtons[index],*m_macroSnapshot);
            else if(m_battery.state==MouseState::Mode::Offline||m_battery.state==MouseState::Mode::Missing)currentName=Localization::Get(Localization::T006);
            else if(!m_buttonsReadMessage.empty())currentName=Localization::Get(Localization::T046);
            else currentName=m_reading?Localization::Get(Localization::T007):Localization::Get(Localization::T045);
            const bool left=index==0||index==3||index==4;auto host=m_buttonPreviewHosts[index];
            const bool currentLogo=m_liveButtons[index][0]!=3;
            if(force||!previous.initialized||previous.currentName!=currentName||previous.currentLogo!=currentLogo){
                host.Children().Clear();host.Children().Append(FunctionLabel(currentName,left,currentLogo));
                Automation::AutomationProperties::SetName(host,hstring(ButtonCatalog::PhysicalButtons[index])+L"："+currentName);
                previous.currentName=currentName;previous.currentLogo=currentLogo;
            }
            previous.initialized=true;
        }
        ButtonsReadStatus().Text(m_buttonsReadMessage);
        ButtonsReadStatus().Visibility(m_buttonsReadMessage.empty()?Visibility::Collapsed:Visibility::Visible);
    }
    void MainWindow::PopulateButtonMenu() {
        // Native menus own cascading, focus, keyboard navigation and scrolling.
        m_buttonFlyout.Items().Clear();
        const auto appendMapping=[this](auto const& items,std::wstring_view name,ManagerBackend::ButtonCode const& code,int tag,bool logos=true){
            ToggleMenuFlyoutItem item;item.Text(MenuFunctionName(name,logos));item.Tag(box_value(tag));
            // Keep the native text template; font fallback supplies the inline Windows glyph.
            if(logos && name.find(L"Windows")!=std::wstring_view::npos)
                item.FontFamily(FontFamily(L"Segoe UI, Segoe UI Symbol"));
            item.IsChecked(!m_draft.macroAssignments[m_menuButton]&&code==m_draft.buttons[m_menuButton]);
            Automation::AutomationProperties::SetName(item,hstring(name));
            item.Click([this,code](auto const&,auto const&){SelectButtonFunction(code);});items.Append(item);
        };
        for(int category=0;category<static_cast<int>(std::size(ButtonCatalog::Categories));++category){
            MenuFlyoutSubItem section;section.Text(hstring(ButtonCatalog::Categories[category]));
            if(category==1){
                for(int group=0;group<static_cast<int>(std::size(ButtonCatalog::KeyboardGroups));++group){
                    MenuFlyoutSubItem subgroup;subgroup.Text(hstring(ButtonCatalog::KeyboardGroups[group]));
                    std::vector<std::size_t> entries;
                    for(std::size_t entry=0;entry<std::size(ButtonCatalog::Entries);++entry){
                        const auto& definition=ButtonCatalog::Entries[entry];
                        if(definition.category==category&&definition.keyboardGroup==group)entries.push_back(entry);
                    }
                    if(group==4)std::stable_sort(entries.begin(),entries.end(),[](auto a,auto b){return ButtonCatalog::Entries[a].code[3]<ButtonCatalog::Entries[b].code[3];});
                    for(auto entry:entries){const auto& definition=ButtonCatalog::Entries[entry];
                        appendMapping(subgroup.Items(),definition.label,definition.code,static_cast<int>(entry));}
                    section.Items().Append(subgroup);
                }
            }else if(category==4){
                RefreshMacroReadbackLinks();
                for(std::size_t index=0;index<m_macroProfiles.size();++index){
                    ToggleMenuFlyoutItem item;item.Text(m_macroProfiles[index].name);
                    const auto& selected=m_draft.buttons[m_menuButton];
                    item.IsChecked(m_draft.macroAssignments[m_menuButton]?m_draft.macroAssignments[m_menuButton]->profileId==m_macroProfiles[index].id:
                        m_macroReadbackLinks[m_menuButton]&&m_macroReadbackLinks[m_menuButton]->profileId==m_macroProfiles[index].id&&selected==m_liveButtons[m_menuButton]);
                    const auto id=m_macroProfiles[index].id;const auto button=m_menuButton;item.Click([this,id,button](auto const&,auto const&){m_menuButton=button;SelectMacroForButton(id);});section.Items().Append(item);
                }
                if(m_macroProfiles.empty()){MenuFlyoutItem empty;empty.Text(Localization::Get(Localization::T047));empty.IsEnabled(false);section.Items().Append(empty);}
            }else{
                for(std::size_t entry=0;entry<std::size(ButtonCatalog::Entries);++entry){const auto& definition=ButtonCatalog::Entries[entry];
                    if(definition.category==category)appendMapping(section.Items(),definition.label,definition.code,static_cast<int>(entry));}
            }
            m_buttonFlyout.Items().Append(section);
        }
    }
    void MainWindow::SelectButtonFunction(ManagerBackend::ButtonCode const& code) {
        if(m_closed||!m_buttonsLoaded||m_applying||m_macroRecording)return;
        m_draft.macroAssignments[m_menuButton].reset();m_draft.buttons[m_menuButton]=code;UpdateButtonLabels();Edited();
    }
    ContentDialog MainWindow::CreateButtonResetDialog() {
        ContentDialog dialog;dialog.Title(box_value(Localization::Get(Localization::T048)));
        dialog.Style(Application::Current().Resources().Lookup(box_value(L"DefaultContentDialogStyle")).as<Style>());
        dialog.IsTabStop(true);
        dialog.Resources().Insert(box_value(L"ContentDialogMinHeight"),box_value(0.0));
        StackPanel content;content.MinWidth(320);content.Spacing(16);
        TextBlock body;body.Text(Localization::Get(Localization::T049));body.TextWrapping(TextWrapping::Wrap);content.Children().Append(body);
        m_buttonResetProgress=ProgressBar();m_buttonResetProgress.IsIndeterminate(false);
        m_buttonResetProgress.Style(RootGrid().Resources().Lookup(box_value(L"ButtonResetCountdownStyle")).as<Style>());
        m_buttonResetProgress.Minimum(0);m_buttonResetProgress.Maximum(10);
        m_buttonResetProgress.Value(10);
        Automation::AutomationProperties::SetName(m_buttonResetProgress,Localization::Get(Localization::T338));
        dialog.Content(content);
        auto weak=get_weak();
        dialog.Opened([weak](auto const& sender,auto const&){
            if(auto self=weak.get();self&&!self->m_closed){
                self->CreateDialogProgressFooter(sender,self->m_buttonResetProgress);sender.UpdateLayout();
                if(!sender.Focus(FocusState::Programmatic)){
                    self->CancelButtonReset();self->m_statusDetail=Localization::Get(Localization::T050);self->UpdateStatus();return;
                }
                self->m_buttonResetSpacePressed=false;
                self->m_buttonResetDeadline=GetTickCount64()+10000;self->m_buttonResetTimer.Start();
            }
        });
        // Keep focus in the dialog through both halves of the Space press.
        // Dismissing on KeyDown would return focus before the matching KeyUp.
        dialog.PreviewKeyDown([weak](auto const&,Input::KeyRoutedEventArgs const& args){
            if(auto self=weak.get();self&&(args.Key()==Windows::System::VirtualKey::Space||args.Key()==Windows::System::VirtualKey::Escape)){
                args.Handled(true);self->HandleButtonResetKey(args.Key(),false);
            }
        });
        dialog.PreviewKeyUp([weak](auto const&,Input::KeyRoutedEventArgs const& args){
            if(auto self=weak.get();self&&(args.Key()==Windows::System::VirtualKey::Space||args.Key()==Windows::System::VirtualKey::Escape)){
                args.Handled(true);self->HandleButtonResetKey(args.Key(),true);
            }
        });
        dialog.Closed([weak](auto const& sender,auto const&){
            if(auto self=weak.get();self&&self->m_buttonResetDialog==sender){
                const bool confirmed=self->m_buttonResetConfirmed;
                self->m_buttonResetTimer.Stop();self->m_buttonResetDeadline=0;self->m_buttonResetConfirmed=false;
                self->m_buttonResetSpacePressed=false;
                self->m_buttonResetDialog=nullptr;self->m_buttonResetProgress=nullptr;
                if(confirmed&&!self->m_closed)self->ResetButtonMappings();
            }
        });
        return dialog;
    }
    void MainWindow::ShowButtonResetDialog() {
        if(m_closed||ButtonsPage().Visibility()!=Visibility::Visible||GetForegroundWindow()!=m_windowHandle||
           !m_loaded||!m_buttonsLoaded||m_applying||m_macroRecording||m_clockCalibrating||!m_applyHandler||m_buttonResetDialog||
           m_parameterReadFailureDialog||m_closePending)return;
        if(m_buttonFlyout)m_buttonFlyout.Hide();
        auto dialog=CreateButtonResetDialog();dialog.XamlRoot(RootGrid().XamlRoot());
        m_buttonResetConfirmed=false;m_buttonResetDialog=dialog;
        try{dialog.ShowAsync();}
        catch(hresult_error const& error){
            m_buttonResetDialog=nullptr;m_buttonResetProgress=nullptr;m_buttonResetDeadline=0;
            m_statusDetail=Localization::Format(Localization::T051,{{L"details",error.message().c_str()}});UpdateStatus();
        }
    }
    void MainWindow::UpdateButtonResetCountdown(ULONGLONG now) {
        if(!m_buttonResetDialog||m_buttonResetConfirmed||!m_buttonResetDeadline)return;
        if(now>=m_buttonResetDeadline){CancelButtonReset();return;}
        m_buttonResetProgress.Value(static_cast<double>(m_buttonResetDeadline-now)/1000.0);
    }
    void MainWindow::ConfirmButtonReset() {
        if(!m_buttonResetDialog||m_buttonResetConfirmed||!m_buttonResetDeadline)return;
        if(GetTickCount64()>=m_buttonResetDeadline||!m_loaded||!m_buttonsLoaded||m_applying||m_macroRecording||!m_applyHandler){CancelButtonReset();return;}
        m_buttonResetConfirmed=true;m_buttonResetDeadline=0;m_buttonResetTimer.Stop();m_buttonResetDialog.Hide();
    }
    bool MainWindow::HandleButtonResetKey(Windows::System::VirtualKey key,bool released) {
        if(!m_buttonResetDialog)return false;
        if(key==Windows::System::VirtualKey::Space){
            if(!released)m_buttonResetSpacePressed=true;
            else if(m_buttonResetSpacePressed){m_buttonResetSpacePressed=false;ConfirmButtonReset();}
            return true;
        }
        if(key==Windows::System::VirtualKey::Escape){if(!released)CancelButtonReset();return true;}
        return false;
    }
    void MainWindow::CancelButtonReset() {
        m_buttonResetConfirmed=false;m_buttonResetSpacePressed=false;m_buttonResetDeadline=0;
        if(m_buttonResetTimer)m_buttonResetTimer.Stop();
        if(m_buttonResetDialog)m_buttonResetDialog.Hide();
    }
    void MainWindow::ResetButtonMappings() {
        if(m_closed||!m_loaded||!m_buttonsLoaded||m_applying||m_macroRecording||!m_applyHandler)return;
        for(auto const& stage:m_stages)if(stage.flyout)stage.flyout.Hide();
        m_draft=m_baseline;m_draft.buttons=ManagerBackend::DefaultButtonMappings;
        m_dirty=!ManagerBackend::Equals(m_baseline,m_draft);m_resetButtonApply=true;m_applying=true;
        m_completionStatus.clear();m_statusDetail.clear();Populate();UpdateStatus();
        try{m_applyHandler(m_draft,m_baseline,ManagerBackend::ApplyMode::ResetButtonMappings);}
        catch(...){m_resetButtonApply=false;m_applying=false;m_statusDetail=Localization::Get(Localization::T052);UpdateStatus();UpdateActions();}
    }
}
