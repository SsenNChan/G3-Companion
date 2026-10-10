#include "pch.h"
#include "../localization.h"
#include "MainWindow.xaml.h"
#include "MacroLibrary.h"
#include "../button_catalog.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Input.h>
#include <cmath>
#include <limits>
#include <winrt/Windows.System.Threading.h>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Windows::Foundation;
using MacroEditor::Step;
using MacroEditor::StepKind;

namespace {
    struct MacroPointerDrag {
        std::optional<std::uint32_t> pointer;
        Point origin{},offset{};
        bool dragging=false,suppressClick=false;
    };
    bool ReadMacroInteger(std::wstring const& text,std::uint32_t maximum,std::uint32_t& value){
        wchar_t* end=nullptr;const auto parsed=std::wcstoull(text.c_str(),&end,10);
        if(text.empty()||end==text.c_str()||*end||parsed<1||parsed>maximum)return false;
        value=static_cast<std::uint32_t>(parsed);return true;
    }
    std::wstring StepLabel(const Step& step){
        if(step.kind==StepKind::Delay)return std::to_wstring(step.value)+L" ms";
        if(step.kind==StepKind::Wheel)return step.value==0x0a?Localization::Get(Localization::T129):Localization::Get(Localization::T130);
        if(step.kind==StepKind::Mouse){switch(step.value){case 1:return Localization::Get(Localization::T075);case 2:return Localization::Get(Localization::T076);case 4:return Localization::Get(Localization::T077);case 8:return Localization::Get(Localization::T127);default:return Localization::Get(Localization::T128);}}
        for(const auto& definition:ButtonCatalog::Entries)if(definition.category==1&&definition.code[3]==step.value)return std::wstring(definition.label);
        const wchar_t* modifiers[]={Localization::Get(Localization::T123),Localization::Get(Localization::T125),Localization::Get(Localization::T121),Localization::Get(Localization::T119),Localization::Get(Localization::T124),Localization::Get(Localization::T126),Localization::Get(Localization::T122),Localization::Get(Localization::T120)};
        if(step.value>=0xe0&&step.value<=0xe7)return modifiers[step.value-0xe0];
        return Localization::Format(Localization::T188,{{L"keyCode",std::to_wstring(step.value)}});
    }
    std::uint32_t KeyboardUsage(DWORD key,DWORD scan,DWORD flags){
        if(key>=L'A'&&key<=L'Z')return key-L'A'+4;
        if(key>=L'1'&&key<=L'9')return key-L'1'+0x1e;if(key==L'0')return 0x27;
        if(key>=VK_F1&&key<=VK_F12)return key-VK_F1+0x3a;if(key>=VK_F13&&key<=VK_F24)return key-VK_F13+0x68;
        if(key>=VK_NUMPAD1&&key<=VK_NUMPAD9)return key-VK_NUMPAD1+0x59;
        switch(key){
        case VK_LCONTROL:return 0xe0;case VK_RCONTROL:return 0xe4;case VK_CONTROL:return flags&LLKHF_EXTENDED?0xe4:0xe0;
        case VK_LSHIFT:return 0xe1;case VK_RSHIFT:return 0xe5;case VK_SHIFT:return scan==0x36?0xe5:0xe1;
        case VK_LMENU:return 0xe2;case VK_RMENU:return 0xe6;case VK_MENU:return flags&LLKHF_EXTENDED?0xe6:0xe2;
        case VK_LWIN:return 0xe3;case VK_RWIN:return 0xe7;
        case VK_RETURN:return flags&LLKHF_EXTENDED?0x58:0x28;case VK_ESCAPE:return 0x29;case VK_BACK:return 0x2a;case VK_TAB:return 0x2b;case VK_SPACE:return 0x2c;
        case VK_OEM_MINUS:return 0x2d;case VK_OEM_PLUS:return 0x2e;case VK_OEM_4:return 0x2f;case VK_OEM_6:return 0x30;case VK_OEM_5:return 0x31;
        case VK_OEM_1:return 0x33;case VK_OEM_7:return 0x34;case VK_OEM_3:return 0x35;case VK_OEM_COMMA:return 0x36;case VK_OEM_PERIOD:return 0x37;case VK_OEM_2:return 0x38;
        case VK_CAPITAL:return 0x39;case VK_SNAPSHOT:return 0x46;case VK_SCROLL:return 0x47;case VK_PAUSE:return 0x48;case VK_INSERT:return 0x49;case VK_HOME:return 0x4a;
        case VK_PRIOR:return 0x4b;case VK_DELETE:return 0x4c;case VK_END:return 0x4d;case VK_NEXT:return 0x4e;case VK_RIGHT:return 0x4f;case VK_LEFT:return 0x50;
        case VK_DOWN:return 0x51;case VK_UP:return 0x52;case VK_NUMLOCK:return 0x53;case VK_DIVIDE:return 0x54;case VK_MULTIPLY:return 0x55;case VK_SUBTRACT:return 0x56;
        case VK_ADD:return 0x57;case VK_NUMPAD0:return 0x62;case VK_DECIMAL:return 0x63;case VK_OEM_102:return 0x64;case VK_APPS:return 0x65;
        default:return 0;
        }
    }
}

namespace winrt::G3Companion::implementation {
    void MainWindow::InitializeMacroEditor(){
        MacroNewButton().Click([this](auto const&,auto const&){ManageMacro(0);});
        MacroRemoveButton().Click([this](auto const&,auto const&){ManageMacro(1);});
        MacroRenameButton().Click([this](auto const&,auto const&){ManageMacro(2);});
        MacroCopyButton().Click([this](auto const&,auto const&){ManageMacro(3);});
        MacroSaveButton().Click([this](auto const&,auto const&){SaveMacroDraft();});
        MacroCancelButton().Click([this](auto const&,auto const&){if(m_selectedMacro&&!m_macroRecording){m_macroDraft=m_macroProfiles[*m_selectedMacro];m_macroLayout.Invalidate();m_macroCursor=0;m_macroErasing=false;m_macroTimeEditor=nullptr;m_macroEditingTime.reset();m_macroTimeInvalid=false;m_macroRepeatInvalid=false;m_macroInvalidSteps.clear();MacroErrorMessage(L"");RenderMacroEditor();}});
        MacroEraseButton().Click([this](auto const&,auto const&){if(!m_macroRecording&&CommitMacroTime()){m_macroErasing=!m_macroErasing;RenderMacroEditor();}});
        MacroRecordButton().Click([this](auto const&,auto const&){ToggleMacroRecording();});
        MacroMode().SelectionChanged([this](auto const&,auto const&){if(m_macroPopulating||!m_selectedMacro||m_macroRecording)return;if(!CommitMacroTime()){m_macroPopulating=true;MacroMode().SelectedIndex(m_macroDraft.executionMode==1?0:m_macroDraft.executionMode==4?1:2);m_macroPopulating=false;return;}const int modes[]={1,4,2};auto index=MacroMode().SelectedIndex();if(index>=0){m_macroDraft.executionMode=modes[index];if(m_macroDraft.executionMode!=1)m_macroRepeatInvalid=false;}RenderMacroEditor();});
        MacroRepeatCount().ValueChanged([this](auto const&,auto const&){if(m_macroPopulating||!m_selectedMacro||m_macroRecording)return;double value=MacroRepeatCount().Value();m_macroRepeatInvalid=!std::isfinite(value)||value<1||value>255||std::floor(value)!=value;if(!m_macroRepeatInvalid)m_macroDraft.repeatCount=static_cast<int>(value);UpdateMacroActions();});
        MacroRepeatCount().RegisterPropertyChangedCallback(NumberBox::TextProperty(),[this](auto const&,auto const&){if(!m_macroPopulating)UpdateMacroActions();});
        MenuFlyout addMenu;addMenu.Opening([this](auto const&,auto const&){PopulateMacroAddMenu();});MacroAddButton().Flyout(addMenu);
        MacroStepCanvas().SizeChanged([this](auto const&,SizeChangedEventArgs const& args){if(!m_macroRendering&&std::abs(args.NewSize().Width-args.PreviousSize().Width)>0.1)LayoutMacroSteps();});
        MacroStepsScroll().ViewChanged([this](auto const&,auto const&){UpdateVisibleMacroSteps();});
        RenderMacroList();RenderMacroEditor();
    }
    void MainWindow::MacroErrorMessage(std::wstring_view text){MacroError().Text(hstring(text));MacroError().Visibility(text.empty()?Visibility::Collapsed:Visibility::Visible);}
    void MainWindow::LoadMacroLibrary(){
        if(m_macroLibraryRequested)return;m_macroLibraryRequested=true;
        m_macroDirectory=m_monitoringPaths.directory+L"\\Macros";m_macroLoading=true;RenderMacroEditor();
        auto weak=get_weak();auto queue=DispatcherQueue();auto directory=m_macroDirectory;auto canceled=m_macroLoadCanceled;
        try{Windows::System::Threading::ThreadPool::RunAsync([weak,queue,directory,canceled](auto const&){
            std::vector<MacroEditor::Profile> profiles;std::wstring errors;
            try{profiles=MacroLibrary::Load(directory,errors,canceled.get());}
            catch(winrt::hresult_error const& error){errors=Localization::Format(Localization::T189,{{L"details",error.message().c_str()}});}
            catch(std::exception const&){errors=Localization::Get(Localization::T190);}
            if(canceled->load())return;
            queue.TryEnqueue([weak,profiles=std::move(profiles),errors=std::move(errors)]() mutable {
                if(auto self=weak.get();self&&!self->m_closed){self->m_macroLoading=false;self->m_macroProfiles=std::move(profiles);self->MacroErrorMessage(errors);
                    self->RefreshMacroReadbackLinks();self->RefreshMacroMenus();self->RenderMacroList();self->RenderMacroEditor();}
            });
        });}catch(...){m_macroLoading=false;MacroErrorMessage(Localization::Get(Localization::T191));RenderMacroEditor();}
    }
    bool MainWindow::EnsureMacroProfile(MacroEditor::Profile& profile){
        if(profile.stepsLoaded)return true;
        try{
            auto loaded=MacroLibrary::Parse(MacroLibrary::Read(MacroLibrary::File(m_macroDirectory,profile.name)));
            if(!MacroLibrary::SameName(loaded.name,profile.name))throw hresult_invalid_argument();
            loaded.id=profile.id;profile=std::move(loaded);return true;
        }catch(...){MacroErrorMessage(Localization::Get(Localization::T185));return false;}
    }
    void MainWindow::ReleaseMacroCache(){
        for(std::size_t index=0;index<m_macroProfiles.size();++index){
            if(m_selectedMacro&&index==*m_selectedMacro)continue;
            auto& profile=m_macroProfiles[index];std::vector<Step>().swap(profile.steps);profile.stepsLoaded=false;
        }
    }
    void MainWindow::RefreshMacroReadbackLinks(){
        for(std::size_t slot=0;slot<6;++slot){const auto code=m_liveButtons[slot];auto macro=std::find_if(m_macroSnapshot->begin(),m_macroSnapshot->end(),[&](auto const& value){return value.binding==code;});
            auto& link=m_macroReadbackLinks[slot];if(code[0]!=3||macro==m_macroSnapshot->end()||macro->data.empty()){link.reset();continue;}
            if(link&&link->binding==code&&link->data==macro->data&&std::any_of(m_macroProfiles.begin(),m_macroProfiles.end(),[&](auto const& profile){return profile.id==link->profileId;}))continue;
            auto profile=std::find_if(m_macroProfiles.begin(),m_macroProfiles.end(),[&](auto const& value){return MacroLibrary::SameName(value.name,macro->name);});
            if(profile==m_macroProfiles.end())link.reset();else link=MacroReadbackLink{profile->id,code,macro->data};
        }
    }
    void MainWindow::RefreshMacroMenus(){
        if(m_buttonFlyout)m_buttonFlyout.Hide();for(auto const& selector:m_buttonSelectors)if(selector)if(auto menu=selector.Flyout().try_as<MenuFlyout>())menu.Items().Clear();
    }
    std::array<bool,6> MainWindow::BoundMacroSlots(MacroEditor::Profile const& profile){
        RefreshMacroReadbackLinks();std::array<bool,6> slots{};
        for(std::size_t slot=0;slot<6;++slot)slots[slot]=m_draft.macroAssignments[slot]?m_draft.macroAssignments[slot]->profileId==profile.id:
            m_macroReadbackLinks[slot]&&m_macroReadbackLinks[slot]->profileId==profile.id&&m_draft.buttons[slot]==m_liveButtons[slot];
        return slots;
    }
    void MainWindow::SyncSavedMacro(MacroEditor::Profile const& profile,std::array<bool,6> const& slots){
        if(std::none_of(slots.begin(),slots.end(),[](bool bound){return bound;})){RefreshMacroMenus();return;}
        MacroEditor::Assignment assignment;std::wstring error;if(!EncodeMacroForDevice(profile,assignment.data,error)){MacroErrorMessage(error);return;}
        assignment.profileId=profile.id;assignment.name=profile.name;assignment.executionMode=static_cast<std::uint8_t>(profile.executionMode);assignment.repeatCount=static_cast<std::uint8_t>(profile.repeatCount);
        for(std::size_t slot=0;slot<6;++slot)if(slots[slot]){
            const auto baseline=m_baseline.buttons[slot];const bool saved=std::any_of(m_macroSnapshot->begin(),m_macroSnapshot->end(),[&](auto const& macro){return macro.binding==baseline&&macro.data==assignment.data&&baseline[1]==assignment.executionMode&&baseline[2]==assignment.repeatCount;});
            if(saved){m_draft.macroAssignments[slot].reset();m_draft.buttons[slot]=baseline;}
            else{m_draft.macroAssignments[slot]=assignment;m_draft.buttons[slot]={3,assignment.executionMode,assignment.repeatCount,m_draft.buttons[slot][3]};}
        }
        RefreshMacroMenus();UpdateButtonLabels();Edited();
    }
    bool MainWindow::MacroHasChanges(){
        if(!m_selectedMacro)return false;const auto& saved=m_macroProfiles[*m_selectedMacro];
        if(!MacroEditor::SameContent(m_macroDraft,saved))return true;
        std::uint32_t value=0;
        if(m_macroDraft.executionMode==1&&(!ReadMacroInteger(std::wstring(MacroRepeatCount().Text()),255,value)||value!=static_cast<std::uint32_t>(saved.repeatCount)))return true;
        if(m_macroTimeEditor&&m_macroEditingTime&&(!ReadMacroInteger(std::wstring(m_macroTimeEditor.Text()),MacroEditor::MaximumDelay,value)||*m_macroEditingTime>=saved.steps.size()||value!=saved.steps[*m_macroEditingTime].value))return true;
        return false;
    }
    void MainWindow::UpdateMacroActions(){
        const bool changed=!m_macroRecording&&!m_applying&&MacroHasChanges();
        MacroSaveButton().IsEnabled(changed);MacroCancelButton().IsEnabled(changed);
    }
    bool MainWindow::CommitMacroTime(){
        if(!m_macroTimeEditor||!m_macroEditingTime)return !m_macroTimeInvalid;
        auto text=std::wstring(m_macroTimeEditor.Text());wchar_t* end=nullptr;auto value=std::wcstoull(text.c_str(),&end,10);
        if(text.empty()||end==text.c_str()||*end||value<1||value>MacroEditor::MaximumDelay){m_macroTimeInvalid=true;MacroErrorMessage(Localization::Get(Localization::T192));return false;}
        m_macroLayout.Invalidate(*m_macroEditingTime);m_macroDraft.steps[*m_macroEditingTime].value=static_cast<std::uint32_t>(value);m_macroTimeEditor=nullptr;m_macroEditingTime.reset();m_macroTimeInvalid=false;UpdateMacroActions();return true;
    }
    bool MainWindow::SaveMacroDraft(){
        if(!m_selectedMacro||m_macroRecording||m_applying||!CommitMacroTime())return false;
        MacroEditor::Validation validation;
        std::uint32_t repeats=0;
        if(m_macroDraft.executionMode==1&&!ReadMacroInteger(std::wstring(MacroRepeatCount().Text()),255,repeats)){MacroErrorMessage(Localization::Get(Localization::T193));return false;}
        if(m_macroDraft.executionMode==1)m_macroDraft.repeatCount=static_cast<int>(repeats);m_macroRepeatInvalid=false;
        if(!MacroEditor::Validate(m_macroDraft,validation)){m_macroInvalidSteps=validation.invalid;MacroErrorMessage(validation.message);RenderMacroSteps();return false;}
        const auto bound=BoundMacroSlots(m_macroProfiles[*m_selectedMacro]);std::vector<std::uint8_t> encoded;std::wstring error;
        if(std::any_of(bound.begin(),bound.end(),[](bool value){return value;})&&!EncodeMacroForDevice(m_macroDraft,encoded,error)){MacroErrorMessage(error);return false;}
        auto saved=m_macroDraft;saved.steps.clear();saved.steps.reserve(m_macroDraft.steps.size());std::size_t savedCursor=0;
        for(std::size_t i=0;i<m_macroDraft.steps.size();++i){auto const& step=m_macroDraft.steps[i];
            if(step.kind==MacroEditor::StepKind::Delay&&!saved.steps.empty()&&saved.steps.back().kind==MacroEditor::StepKind::Delay)saved.steps.back().value+=step.value;
            else saved.steps.push_back(step);
            if(i<m_macroCursor)savedCursor=saved.steps.size();
        }
        try{MacroLibrary::Save(m_macroDirectory,saved);m_macroDraft=std::move(saved);m_macroLayout.Invalidate();m_macroCursor=savedCursor;m_macroProfiles[*m_selectedMacro]=m_macroDraft;m_macroInvalidSteps.clear();m_macroErasing=false;MacroErrorMessage(L"");SyncSavedMacro(m_macroDraft,bound);RenderMacroList();RenderMacroEditor();return true;}
        catch(...){MacroErrorMessage(Localization::Get(Localization::T194));return false;}
    }
    IAsyncOperation<bool> MainWindow::ConfirmMacroSwitch(){
        auto lifetime=get_strong();if(m_macroRecording)co_return false;CommitMacroTime();
        if(!MacroHasChanges())co_return true;
        ContentDialog dialog;dialog.XamlRoot(RootGrid().XamlRoot());dialog.Title(box_value(Localization::Get(Localization::T195)));dialog.Content(box_value(Localization::Get(Localization::T196)));
        dialog.PrimaryButtonText(Localization::Get(Localization::T155));dialog.SecondaryButtonText(Localization::Get(Localization::T197));dialog.CloseButtonText(Localization::Get(Localization::T198));
        m_macroSwitchDialog=dialog;auto choice=co_await dialog.ShowAsync();m_macroSwitchDialog=nullptr;if(choice==ContentDialogResult::Primary)co_return SaveMacroDraft();
        if(choice==ContentDialogResult::Secondary){m_macroDraft=m_macroProfiles[*m_selectedMacro];m_macroLayout.Invalidate();m_macroInvalidSteps.clear();m_macroTimeInvalid=false;m_macroRepeatInvalid=false;m_macroTimeEditor=nullptr;m_macroEditingTime.reset();co_return true;}
        co_return false;
    }
    IAsyncAction MainWindow::SelectMacro(std::size_t index){
        auto lifetime=get_strong();if(m_selectedMacro&&*m_selectedMacro==index)co_return;
        if(!co_await ConfirmMacroSwitch())co_return;
        if(index>=m_macroProfiles.size()||m_closed||!EnsureMacroProfile(m_macroProfiles[index]))co_return;m_selectedMacro=index;ReleaseMacroCache();m_macroDraft=MacroEditor::Profile(m_macroProfiles[index]);m_macroLayout.Invalidate();m_macroRepeatInvalid=false;m_macroCursor=0;m_macroErasing=false;
        MacroErrorMessage(L"");RenderMacroList();RenderMacroEditor();
    }
    IAsyncAction MainWindow::ManageMacro(int operation){
        auto lifetime=get_strong();if(m_macroLoading||m_macroRecording||m_applying||(operation&& !m_selectedMacro))co_return;
        if(operation==2){RenderMacroList(true);co_return;}
        if(!co_await ConfirmMacroSwitch()||m_closed)co_return;
        try{
            if(operation==1){ContentDialog dialog;dialog.XamlRoot(RootGrid().XamlRoot());dialog.Title(box_value(Localization::Get(Localization::T199)));dialog.Content(box_value(hstring(Localization::Format(Localization::T200,{{L"name",m_macroProfiles[*m_selectedMacro].name}}))));
                dialog.PrimaryButtonText(Localization::Get(Localization::T201));dialog.CloseButtonText(Localization::Get(Localization::T198));m_macroDeleteDialog=dialog;const auto choice=co_await dialog.ShowAsync();m_macroDeleteDialog=nullptr;if(choice!=ContentDialogResult::Primary)co_return;
                const auto id=m_macroProfiles[*m_selectedMacro].id;
                std::filesystem::remove(MacroLibrary::File(m_macroDirectory,m_macroProfiles[*m_selectedMacro].name));
                for(std::size_t slot=0;slot<6;++slot)if(m_draft.macroAssignments[slot]&&m_draft.macroAssignments[slot]->profileId==id){m_draft.macroAssignments[slot].reset();m_draft.buttons[slot]=m_baseline.buttons[slot];}
                m_macroProfiles.erase(m_macroProfiles.begin()+*m_selectedMacro);m_selectedMacro.reset();m_macroDraft={};m_macroLayout.Invalidate();
                RefreshMacroReadbackLinks();UpdateButtonLabels();Edited();
            }else{
                MacroEditor::Profile profile;if(operation==3){if(!EnsureMacroProfile(m_macroProfiles[*m_selectedMacro]))co_return;profile=m_macroProfiles[*m_selectedMacro];profile.name=Localization::Format(Localization::T202,{{L"name",profile.name}});}
                profile.id=MacroLibrary::NewId();profile.created=MacroLibrary::Now();
                auto base=profile.name;for(unsigned n=2;std::any_of(m_macroProfiles.begin(),m_macroProfiles.end(),[&](auto const& value){return MacroLibrary::SameName(profile.name,value.name);})||(std::filesystem::exists(MacroLibrary::File(m_macroDirectory,profile.name)));++n)profile.name=base+L" ("+std::to_wstring(n)+L")";
                if(to_string(profile.name).size()>255){MacroErrorMessage(Localization::Get(Localization::T203));co_return;}
                MacroLibrary::Save(m_macroDirectory,profile);m_macroProfiles.push_back(profile);m_selectedMacro=m_macroProfiles.size()-1;ReleaseMacroCache();m_macroDraft=profile;m_macroLayout.Invalidate();m_macroRepeatInvalid=false;
            }
            m_macroCursor=0;m_macroErasing=false;m_macroInvalidSteps.clear();MacroErrorMessage(L"");RefreshMacroMenus();RenderMacroList(operation==0||operation==3);RenderMacroEditor();
        }catch(...){MacroErrorMessage(Localization::Get(Localization::T204));}
    }
    void MainWindow::RenderMacroList(bool rename){
        m_macroPopulating=true;m_macroNameEditor=nullptr;
        auto children=MacroList().Children();
        while(children.Size()>m_macroProfiles.size())children.RemoveAtEnd();
        m_macroListViews.resize(m_macroProfiles.size());
        MacroListEmpty().Visibility(m_macroProfiles.empty()?Visibility::Visible:Visibility::Collapsed);
        for(std::size_t index=0;index<m_macroProfiles.size();++index){
            const bool selected=m_selectedMacro&&*m_selectedMacro==index;
            auto& view=m_macroListViews[index];
            if(view.row&&view.id==m_macroProfiles[index].id&&!view.renaming&&!(rename&&selected)){
                view.row.IsEnabled(!m_macroRecording);view.label.Text(m_macroProfiles[index].name);
                view.indicator.Visibility(selected?Visibility::Visible:Visibility::Collapsed);
                Automation::AutomationProperties::SetName(view.row,m_macroProfiles[index].name);continue;
            }
            view={};view.id=m_macroProfiles[index].id;view.renaming=rename&&selected;
            Button row;row.Height(50);row.Padding({8,0,8,0});row.HorizontalContentAlignment(HorizontalAlignment::Stretch);row.IsEnabled(!m_macroRecording);
            row.Style(Application::Current().Resources().Lookup(box_value(L"SubtleButtonStyle")).as<Style>());row.HorizontalAlignment(HorizontalAlignment::Stretch);
            Grid grid;
            if(rename&&selected){TextBox editor;editor.Text(m_macroProfiles[index].name);editor.MinWidth(0);editor.Height(32);editor.Padding({5,4,5,4});editor.Margin({8,0,0,0});grid.Children().Append(editor);m_macroNameEditor=editor;
                auto weakEditor=make_weak(editor);
                const auto commit=[this,index,weakEditor](){auto editor=weakEditor.get();if(!editor||m_macroPopulating||m_applying||index>=m_macroProfiles.size())return;auto name=std::wstring(editor.Text());
                    if(!MacroLibrary::ValidName(name)){MacroErrorMessage(Localization::Get(Localization::T205));return;}
                    for(std::size_t other=0;other<m_macroProfiles.size();++other)if(other!=index&&MacroLibrary::SameName(m_macroProfiles[other].name,name)){MacroErrorMessage(Localization::Get(Localization::T184));return;}
                    const auto previous=m_macroProfiles[index];const auto bound=BoundMacroSlots(previous);auto profile=previous;profile.name=name;
                    try{MacroLibrary::Rename(m_macroDirectory,profile,previous.name);m_macroProfiles[index].name=name;m_macroDraft.name=name;MacroErrorMessage(L"");SyncSavedMacro(profile,bound);RenderMacroList();}
                    catch(...){MacroErrorMessage(Localization::Get(Localization::T206));}
                };
                editor.LostFocus([commit](auto const&,auto const&){commit();});editor.KeyDown([this,commit](auto const&,Input::KeyRoutedEventArgs const& args){if(args.Key()==Windows::System::VirtualKey::Enter){args.Handled(true);commit();}else if(args.Key()==Windows::System::VirtualKey::Escape){args.Handled(true);RenderMacroList();}});
                editor.Loaded([weakEditor](auto const&,auto const&){if(auto editor=weakEditor.get()){editor.Focus(FocusState::Programmatic);editor.SelectAll();}});
            }else{TextBlock label;view.label=label;label.Text(m_macroProfiles[index].name);label.TextTrimming(TextTrimming::CharacterEllipsis);label.VerticalAlignment(VerticalAlignment::Center);label.Margin({8,0,0,0});grid.Children().Append(label);}
            {Border indicator;view.indicator=indicator;indicator.Visibility(selected?Visibility::Visible:Visibility::Collapsed);indicator.Style(RootGrid().Resources().Lookup(box_value(L"DpiStageIndicatorStyle")).as<Style>());indicator.Margin({-6,0,0,0});grid.Children().Append(indicator);}
            row.Content(grid);Automation::AutomationProperties::SetName(row,m_macroProfiles[index].name);row.Click([this,index](auto const&,auto const&){SelectMacro(index);});view.row=row;if(index<children.Size())children.SetAt(static_cast<std::uint32_t>(index),row);else children.Append(row);
        }
        m_macroPopulating=false;UpdateMacroHeight(MacroStepCanvas().Height());
    }
    void MainWindow::RenderMacroEditor(){
        const bool selected=m_selectedMacro.has_value();m_macroPopulating=true;
        MacroEditorHeaderFill().Visibility(selected?Visibility::Visible:Visibility::Collapsed);MacroEditorFooterFill().Visibility(selected?Visibility::Visible:Visibility::Collapsed);
        MacroEditorPanel().Visibility(selected?Visibility::Visible:Visibility::Collapsed);MacroEditorEmpty().Visibility(selected?Visibility::Collapsed:Visibility::Visible);
        MacroRemoveButton().IsEnabled(selected&&!m_macroRecording);MacroRenameButton().IsEnabled(selected&&!m_macroRecording);MacroCopyButton().IsEnabled(selected&&!m_macroRecording);MacroNewButton().IsEnabled(!m_macroLoading&&!m_macroRecording);
        MacroAddButton().IsEnabled(selected&&!m_macroRecording&&!m_macroErasing);MacroMode().IsEnabled(!m_macroRecording);MacroRepeatCount().IsEnabled(!m_macroRecording);
        MacroEraseButton().IsEnabled(selected&&!m_macroRecording);
        auto critical=RootGrid().Resources().Lookup(box_value(L"MacroCriticalBrush")).as<Brush>();
        MacroRecordButton().Foreground(m_macroRecording?critical:RootGrid().Resources().Lookup(box_value(L"MacroSecondaryBrush")).as<Brush>());
        MacroEraseButton().Foreground(m_macroErasing?critical:RootGrid().Resources().Lookup(box_value(L"MacroSecondaryBrush")).as<Brush>());
        Automation::AutomationProperties::SetName(MacroRecordButton(),m_macroRecording?Localization::Get(Localization::T339):Localization::Get(Localization::T340));
        if(selected){MacroMode().SelectedIndex(m_macroDraft.executionMode==1?0:m_macroDraft.executionMode==4?1:2);if(!m_macroRepeatInvalid)MacroRepeatCount().Value(m_macroDraft.repeatCount);MacroRepeatCount().Visibility(m_macroDraft.executionMode==1?Visibility::Visible:Visibility::Collapsed);
            }
        m_macroPopulating=false;UpdateMacroActions();RenderMacroSteps();
    }
    void MainWindow::UpdateMacroHeight(double contentHeight){
        const auto rows=1+std::max<std::size_t>(3,std::min<std::size_t>(5,m_macroProfiles.size()));
        const double required=std::isfinite(contentHeight)?100+contentHeight+10:200;
        MacroSplit().Height(std::clamp(std::max(rows*50.0,std::ceil(required/50.0)*50.0),200.0,300.0));
    }
    bool MainWindow::BeginMacroDrag(std::size_t index,Point point,Point offset){
        if(m_macroDrag||!m_selectedMacro||m_macroRecording||m_macroErasing||m_macroTimeEditor||index>=m_macroDraft.steps.size())return false;
        m_macroDrag=index;m_macroDragPoint=point;m_macroDragOffset=offset;UpdateMacroDrag(point);return true;
    }
    void MainWindow::UpdateMacroDrag(Point point){
        if(!m_macroDrag)return;m_macroDragPoint=point;double best=std::numeric_limits<double>::max();std::size_t destination=m_macroCursor;
        const auto first=std::lower_bound(m_macroStepLayout.begin(),m_macroStepLayout.end(),point.Y-54,[](auto const& rect,float y){return rect.Y<y;});
        for(auto it=first;it!=m_macroStepLayout.end()&&it->Y<=point.Y+54;++it){
            const auto key=static_cast<std::size_t>(it-m_macroStepLayout.begin());if(key%2)continue;
            const double dx=point.X-it->X-it->Width/2,dy=point.Y-it->Y-16,distance=dx*dx+dy*dy;
            if(distance<best){best=distance;destination=key/2;}
        }
        m_macroCursor=destination;LayoutMacroSteps();
    }
    void MainWindow::FinishMacroDrag(bool apply){
        if(!m_macroDrag)return;const auto source=*m_macroDrag;m_macroDrag.reset();
        if(apply){m_macroLayout.Invalidate(std::min(source,m_macroCursor));MacroEditor::Move(m_macroDraft.steps,source,m_macroCursor);m_macroInvalidSteps.clear();}
        m_macroCursor=std::min(m_macroCursor,m_macroDraft.steps.size());RenderMacroEditor();
    }
    void MainWindow::LayoutMacroSteps(){
        if(m_macroRendering||m_macroTimeEditor)return;
        m_macroRendering=true;
        const double width=std::max(40.0,MacroStepsScroll().ActualWidth()>0?MacroStepsScroll().ActualWidth()-16:310.0);
        if(!m_macroMeasureToken){
            m_macroMeasureToken=Button();m_macroMeasureToken.Padding({0,0,0,0});m_macroMeasureToken.MinWidth(0);m_macroMeasureToken.Height(32);
            m_macroMeasureToken.Style(Application::Current().Resources().Lookup(box_value(L"SubtleButtonStyle")).as<Style>());
            m_macroMeasureToken.ApplyTemplate();
            Border body;body.BorderThickness({1,1,1,1});body.Padding({9,4,9,4});m_macroMeasureText=TextBlock();m_macroMeasureText.FontSize(13);
            m_macroMeasureText.FontFamily(FontFamily(L"Segoe UI, Segoe UI Symbol"));m_macroMeasureText.TextWrapping(TextWrapping::NoWrap);
            body.Child(m_macroMeasureText);m_macroMeasureToken.Content(body);
        }
        static const std::vector<Step> empty;
        const auto height=MacroEditor::UpdateLayout(m_selectedMacro?m_macroDraft.steps:empty,m_macroCursor,m_macroDrag.has_value(),width,
            m_macroLayout,m_macroMeasuredSteps,m_macroStepWidths,m_macroStepLayout,[&](const Step& step){
                m_macroMeasureText.Text(MenuFunctionName(StepLabel(step)));m_macroMeasureToken.Measure({std::numeric_limits<float>::infinity(),32});
                return m_macroMeasureToken.DesiredSize().Width;
            });
        MacroStepCanvas().Height(height);m_macroRendering=false;UpdateMacroHeight(height);UpdateVisibleMacroSteps();
    }

    void MainWindow::FocusMacroStep(std::size_t index){
        if(index>=m_macroDraft.steps.size())return;
        const auto rect=m_macroStepLayout[index*2+1];const auto top=MacroStepsScroll().VerticalOffset(),height=MacroStepsScroll().ViewportHeight();
        if(rect.Y<top||rect.Y+32>top+height)MacroStepsScroll().ChangeView(nullptr,box_value(static_cast<double>(rect.Y)).as<IReference<double>>(),nullptr,true);
        UpdateVisibleMacroSteps();
        if(auto found=m_macroStepViews.find(index*2+1);found!=m_macroStepViews.end())found->second.element.as<Control>().Focus(FocusState::Keyboard);
    }
    void MainWindow::RenderMacroSteps(){
        LayoutMacroSteps();
        if(m_selectedMacro){MacroStepsText().Text(L": "+to_hstring(m_macroLayout.actions));MacroDurationText().Text(L": "+to_hstring(m_macroLayout.duration+m_macroDraft.completionDelayMs)+L" ms");}
    }
    void MainWindow::UpdateVisibleMacroSteps(){
        if(m_macroRendering)return;m_macroRendering=true;
        const double top=MacroStepsScroll().VerticalOffset()-38,bottom=top+std::max(200.0,MacroStepsScroll().ViewportHeight())+76;
        auto children=MacroStepCanvas().Children();
        const auto theme=RootGrid().ActualTheme();
        for(auto it=m_macroStepViews.begin();it!=m_macroStepViews.end();){
            const auto key=it->first,index=key/2;const bool token=key%2!=0;
            const bool pinned=token&&((m_macroDrag&&*m_macroDrag==index)||(m_macroEditingTime&&*m_macroEditingTime==index)||it->second.element.as<Control>().FocusState()!=FocusState::Unfocused);
            const bool inRange=key<m_macroStepLayout.size()&&m_selectedMacro;
            const bool invalid=token&&std::binary_search(m_macroInvalidSteps.begin(),m_macroInvalidSteps.end(),index);
            const bool editorClosed=token&&!m_macroTimeEditor&&it->second.element.as<Button>().Content().as<Border>().Child().try_as<TextBox>()!=nullptr;
            const bool changed=token&&inRange&&(editorClosed||!(it->second.step==m_macroDraft.steps[index])||it->second.invalid!=invalid||it->second.erasing!=m_macroErasing);
            if(!inRange||(!pinned&&(m_macroStepLayout[key].Y+32<top||m_macroStepLayout[key].Y>bottom))||changed||it->second.theme!=theme){
                std::uint32_t position=0;if(children.IndexOf(it->second.element,position))children.RemoveAt(position);it=m_macroStepViews.erase(it);
            }else ++it;
        }
        if(!m_selectedMacro){m_macroRendering=false;return;}
        m_macroCursor=std::min(m_macroCursor,m_macroDraft.steps.size());auto accent=RootGrid().Resources().Lookup(box_value(L"MacroAccentBrush")).as<Brush>();
        auto secondary=RootGrid().Resources().Lookup(box_value(L"MacroSecondaryBrush")).as<Brush>();auto inverted=RootGrid().Resources().Lookup(box_value(L"MacroInvertedBrush")).as<Brush>();
        auto critical=RootGrid().Resources().Lookup(box_value(L"MacroCriticalBrush")).as<Brush>();
        auto first=std::lower_bound(m_macroStepLayout.begin(),m_macroStepLayout.end(),static_cast<float>(top-32),[](auto const& rect,float y){return rect.Y<y;});
        const auto firstIndex=static_cast<std::size_t>(first-m_macroStepLayout.begin())/2;
        for(std::size_t index=firstIndex;index<=m_macroDraft.steps.size();++index){
            if(m_macroStepLayout[index*2].Y>bottom)break;
            if(!m_macroStepViews.contains(index*2)){
            Grid gap;gap.Height(32);gap.Tag(box_value(-static_cast<int>(index)-1));gap.Background(SolidColorBrush(Windows::UI::Colors::Transparent()));TextBlock dash;dash.VerticalAlignment(VerticalAlignment::Center);dash.HorizontalAlignment(HorizontalAlignment::Center);dash.Foreground(secondary);gap.Children().Append(dash);
            Border cursor;cursor.Width(2);cursor.Height(24);cursor.VerticalAlignment(VerticalAlignment::Center);cursor.HorizontalAlignment(HorizontalAlignment::Center);cursor.Background(accent);gap.Children().Append(cursor);
            TextBlock rightDash;rightDash.VerticalAlignment(VerticalAlignment::Center);rightDash.HorizontalAlignment(HorizontalAlignment::Right);rightDash.Foreground(secondary);gap.Children().Append(rightDash);
            gap.PointerPressed([this,index](auto const&,auto const&){if(!m_macroRecording&&!m_macroErasing&&CommitMacroTime()){m_macroCursor=index;LayoutMacroSteps();}});
            m_macroStepViews.emplace(index*2,MacroStepView{gap,{},false,false,theme});children.Append(gap);
            }
            if(index==m_macroDraft.steps.size())break;
            if(m_macroStepViews.contains(index*2+1))continue;
            const auto step=m_macroDraft.steps[index];Button token;token.Padding({0,0,0,0});token.MinWidth(0);token.Height(32);token.Tag(box_value(static_cast<int>(index)));token.TabIndex(static_cast<int>(index));
            token.Style(Application::Current().Resources().Lookup(box_value(L"SubtleButtonStyle")).as<Style>());
            Border body;body.CornerRadius({6,6,6,6});body.BorderThickness({1,1,1,1});body.Padding({9,4,9,4});TextBlock text;text.Text(MenuFunctionName(StepLabel(step)));text.FontSize(13);
            text.FontFamily(FontFamily(L"Segoe UI, Segoe UI Symbol"));text.TextWrapping(TextWrapping::NoWrap);text.VerticalAlignment(VerticalAlignment::Center);body.Child(text);token.Content(body);
            const bool invalid=std::find(m_macroInvalidSteps.begin(),m_macroInvalidSteps.end(),index)!=m_macroInvalidSteps.end();
            auto weakToken=make_weak(token);
            const auto paint=[this,index,step,weakToken,body,text,accent,secondary,inverted,critical,invalid](bool hover){
                const bool red=invalid||(m_macroErasing&&hover);const bool filled=red||((step.kind==StepKind::Keyboard||step.kind==StepKind::Mouse)&&step.pressed);
                auto color=red?critical:step.kind==StepKind::Delay?secondary:accent;body.BorderBrush(color);body.Background(filled?color:SolidColorBrush(Windows::UI::Colors::Transparent()));text.Foreground(filled?inverted:color);if(auto token=weakToken.get())token.Opacity(m_macroDrag&&*m_macroDrag==index?0.65:m_macroErasing&&!hover?0.4:1);
            };paint(false);
            token.PointerEntered([paint](auto const&,auto const&){paint(true);});token.PointerExited([paint](auto const&,auto const&){paint(false);});
            auto pointerDrag=std::make_shared<MacroPointerDrag>();
            // ButtonBase handles and captures pointer input. Observe those events too, so dragging and clicking share the same source.
            token.AddHandler(UIElement::PointerPressedEvent(),box_value(Input::PointerEventHandler([this,pointerDrag](IInspectable const& sender,Input::PointerRoutedEventArgs const& args){
                if(m_macroRecording||m_macroErasing||m_macroTimeEditor)return;const auto point=args.GetCurrentPoint(MacroStepCanvas());if(!point.Properties().IsLeftButtonPressed())return;
                pointerDrag->pointer=args.Pointer().PointerId();pointerDrag->origin=point.Position();pointerDrag->offset=args.GetCurrentPoint(sender.as<UIElement>()).Position();pointerDrag->dragging=false;pointerDrag->suppressClick=false;
            })),true);
            token.AddHandler(UIElement::PointerMovedEvent(),box_value(Input::PointerEventHandler([this,index,pointerDrag](auto const&,Input::PointerRoutedEventArgs const& args){
                if(!pointerDrag->pointer||*pointerDrag->pointer!=args.Pointer().PointerId())return;const auto point=args.GetCurrentPoint(MacroStepCanvas());if(!point.Properties().IsLeftButtonPressed())return;
                if(!pointerDrag->dragging){const auto position=point.Position();if(std::abs(position.X-pointerDrag->origin.X)<6&&std::abs(position.Y-pointerDrag->origin.Y)<6)return;
                    if(!BeginMacroDrag(index,position,pointerDrag->offset))return;pointerDrag->dragging=true;pointerDrag->suppressClick=true;
                }else UpdateMacroDrag(point.Position());args.Handled(true);
            })),true);
            token.AddHandler(UIElement::PointerReleasedEvent(),box_value(Input::PointerEventHandler([this,pointerDrag](auto const&,Input::PointerRoutedEventArgs const& args){
                if(!pointerDrag->pointer||*pointerDrag->pointer!=args.Pointer().PointerId())return;pointerDrag->pointer.reset();if(!pointerDrag->dragging)return;
                UpdateMacroDrag(args.GetCurrentPoint(MacroStepCanvas()).Position());const auto point=args.GetCurrentPoint(MacroStepsScroll()).Position();pointerDrag->dragging=false;
                FinishMacroDrag(point.X>=0&&point.Y>=0&&point.X<MacroStepsScroll().ActualWidth()&&point.Y<MacroStepsScroll().ActualHeight());args.Handled(true);
            })),true);
            auto weakSelf=get_weak();token.AddHandler(UIElement::PointerCaptureLostEvent(),box_value(Input::PointerEventHandler([weakSelf,pointerDrag](auto const&,Input::PointerRoutedEventArgs const&){
                if(auto self=weakSelf.get();self&&pointerDrag->dragging)self->DispatcherQueue().TryEnqueue([weakSelf,pointerDrag](){
                    if(auto self=weakSelf.get();self&&pointerDrag->dragging){pointerDrag->dragging=false;pointerDrag->pointer.reset();self->FinishMacroDrag(false);}
                });
            })),true);
            token.AddHandler(UIElement::PointerCanceledEvent(),box_value(Input::PointerEventHandler([this,pointerDrag](auto const&,Input::PointerRoutedEventArgs const&){pointerDrag->pointer.reset();if(pointerDrag->dragging){pointerDrag->dragging=false;FinishMacroDrag(false);}})),true);
            token.Click([this,index,body,pointerDrag](auto const&,auto const&){
                if(m_macroRecording||m_macroDrag||pointerDrag->suppressClick)return;if(m_macroEditingTime&&*m_macroEditingTime==index)return;if(!CommitMacroTime())return;
                if(m_macroErasing){m_macroLayout.Invalidate(index);m_macroDraft.steps.erase(m_macroDraft.steps.begin()+index);if(m_macroCursor>index)--m_macroCursor;m_macroInvalidSteps.clear();RenderMacroEditor();return;}
                auto& step=m_macroDraft.steps[index];
                if(step.kind==StepKind::Delay){TextBox editor;editor.Text(to_hstring(step.value));editor.Width(78);editor.Height(30);editor.MinWidth(0);editor.Padding({3,3,3,3});body.Padding({0,0,0,0});body.Child(editor);m_macroTimeEditor=editor;m_macroEditingTime=index;
                    editor.RegisterPropertyChangedCallback(TextBox::TextProperty(),[this](auto const&,auto const&){m_macroTimeInvalid=false;UpdateMacroActions();});UpdateMacroActions();
                    editor.KeyDown([this](auto const&,Input::KeyRoutedEventArgs const& args){if(args.Key()==Windows::System::VirtualKey::Enter){args.Handled(true);if(CommitMacroTime()){m_macroInvalidSteps.clear();MacroErrorMessage(L"");RenderMacroEditor();}}else if(args.Key()==Windows::System::VirtualKey::Escape){args.Handled(true);m_macroTimeEditor=nullptr;m_macroEditingTime.reset();m_macroTimeInvalid=false;RenderMacroEditor();}});
                    editor.LostFocus([this](auto const&,auto const&){if(!m_macroRendering&&CommitMacroTime())RenderMacroEditor();});editor.Focus(FocusState::Programmatic);editor.SelectAll();
                }else if(step.kind!=StepKind::Wheel){m_macroLayout.Invalidate(index);step.pressed=!step.pressed;m_macroInvalidSteps.clear();RenderMacroEditor();}
            });
            token.KeyDown([this,index](auto const&,Input::KeyRoutedEventArgs const& args){
                if(args.Key()!=Windows::System::VirtualKey::Tab)return;
                const bool backwards=(GetKeyState(VK_SHIFT)&0x8000)!=0;
                if((backwards&&index>0)||(!backwards&&index+1<m_macroDraft.steps.size())){args.Handled(true);FocusMacroStep(backwards?index-1:index+1);}
            });
            Automation::AutomationProperties::SetName(token,step.kind==StepKind::Keyboard||step.kind==StepKind::Mouse?Localization::Format(step.pressed?Localization::T341:Localization::T342,{{L"key",StepLabel(step)}}):StepLabel(step));
            m_macroStepViews.emplace(index*2+1,MacroStepView{token,step,invalid,m_macroErasing,theme});children.Append(token);
        }
        for(auto const& [key,view]:m_macroStepViews){
            auto const rect=m_macroStepLayout[key];auto element=view.element;element.Width(rect.Width);Canvas::SetLeft(element,rect.X);Canvas::SetTop(element,rect.Y);Canvas::SetZIndex(element,0);if(key%2)element.Opacity(m_macroErasing?0.4:1);
            if(key%2==0){
                const auto index=key/2;const bool active=index==m_macroCursor,left=index>0,right=index<m_macroDraft.steps.size();auto grid=element.as<Grid>();
                auto dash=grid.Children().GetAt(0).as<TextBlock>();dash.Text((active?left:left&&right)?L"-":L"");dash.HorizontalAlignment(active?HorizontalAlignment::Left:HorizontalAlignment::Center);
                grid.Children().GetAt(1).Visibility(active?Visibility::Visible:Visibility::Collapsed);grid.Children().GetAt(2).as<TextBlock>().Text(active&&right?L"-":L"");
            }else if(m_macroDrag&&key/2==*m_macroDrag){Canvas::SetLeft(element,m_macroDragPoint.X-m_macroDragOffset.X);Canvas::SetTop(element,m_macroDragPoint.Y-m_macroDragOffset.Y);Canvas::SetZIndex(element,1);element.Opacity(0.65);}
        }
        m_macroRendering=false;
    }
    void MainWindow::PopulateMacroAddMenu(){
        auto menu=MacroAddButton().Flyout().as<MenuFlyout>();menu.Items().Clear();
        const auto append=[this](auto const& collection,std::wstring_view name,Step step){MenuFlyoutItem item;item.Text(MenuFunctionName(name));
            if(name.find(L"Windows")!=std::wstring_view::npos)item.FontFamily(FontFamily(L"Segoe UI, Segoe UI Symbol"));
            Automation::AutomationProperties::SetName(item,hstring(name));item.Click([this,step](auto const&,auto const&){InsertMacroStep(step);});collection.Append(item);};
        MenuFlyoutSubItem operation;operation.Text(Localization::Get(Localization::T207));MenuFlyoutSubItem mouse;mouse.Text(Localization::Get(Localization::T208));
        for(unsigned value:{1u,2u,4u,8u,16u}){Step step{StepKind::Mouse,value,true};append(mouse.Items(),StepLabel(step),step);}
        append(mouse.Items(),Localization::Get(Localization::T129),{StepKind::Wheel,0x0a,true});append(mouse.Items(),Localization::Get(Localization::T130),{StepKind::Wheel,0xff,true});operation.Items().Append(mouse);
        MenuFlyoutSubItem keyboard;keyboard.Text(Localization::Get(Localization::T209));const wchar_t* groups[]={Localization::Get(Localization::T070),Localization::Get(Localization::T071),Localization::Get(Localization::T072),Localization::Get(Localization::T068),Localization::Get(Localization::T210),Localization::Get(Localization::T074)};
        for(int group=0;group<6;++group){MenuFlyoutSubItem section;section.Text(groups[group]);
            if(group==3){for(unsigned key=0xe0;key<=0xe7;++key){Step step{StepKind::Keyboard,key,true};append(section.Items(),StepLabel(step),step);}}
            std::vector<std::uint32_t> seen;
            for(const auto& definition:ButtonCatalog::Entries){if(definition.category!=1)continue;const auto key=definition.code[3];const bool fn=key>=0x3a&&key<=0x45;
                const bool belongs=group==4?fn:group==3?((definition.keyboardGroup==3||definition.keyboardGroup==4)&&!fn):definition.keyboardGroup==group;
                if(belongs&&key>0&&key<0xe0&&std::find(seen.begin(),seen.end(),key)==seen.end()){seen.push_back(key);append(section.Items(),definition.label,{StepKind::Keyboard,key,true});}
            }keyboard.Items().Append(section);
        }operation.Items().Append(keyboard);menu.Items().Append(operation);append(menu.Items(),Localization::Get(Localization::T211),{StepKind::Delay,1000,true});
    }
    void MainWindow::InsertMacroStep(Step step){if(!m_selectedMacro||m_macroRecording||m_macroErasing||!CommitMacroTime())return;m_macroLayout.Invalidate(m_macroCursor);MacroEditor::Insert(m_macroDraft.steps,m_macroCursor,step,false);m_macroInvalidSteps.clear();MacroErrorMessage(L"");RenderMacroEditor();}
    void MainWindow::SelectMacroForButton(std::size_t index){
        if(index>=m_macroProfiles.size()||!m_buttonsLoaded||m_applying||m_macroRecording)return;if(!EnsureMacroProfile(m_macroProfiles[index]))return;const auto& profile=m_macroProfiles[index];MacroEditor::Assignment assignment;
        std::wstring message;if(!EncodeMacroForDevice(profile,assignment.data,message)){MacroErrorMessage(message);return;}
        assignment.profileId=profile.id;assignment.name=profile.name;assignment.executionMode=static_cast<std::uint8_t>(profile.executionMode);assignment.repeatCount=static_cast<std::uint8_t>(profile.repeatCount);
        m_draft.macroAssignments[m_menuButton]=assignment;m_draft.buttons[m_menuButton]={3,assignment.executionMode,assignment.repeatCount,0};ReleaseMacroCache();UpdateButtonLabels();Edited();
    }
    void MainWindow::SelectMacroForButton(std::wstring const& profileId){
        auto profile=std::find_if(m_macroProfiles.begin(),m_macroProfiles.end(),[&](auto const& value){return value.id==profileId;});
        if(profile!=m_macroProfiles.end())SelectMacroForButton(static_cast<std::size_t>(profile-m_macroProfiles.begin()));
    }
    void MainWindow::CaptureMacroStep(Step step){
        if(!m_macroRecording||!m_selectedMacro)return;const auto time=std::chrono::steady_clock::now();m_macroLastInterval=0;m_macroLayout.Invalidate(m_macroCursor);
        if(m_macroHasInput){const auto elapsed=std::max<std::int64_t>(1,std::llround(std::chrono::duration<double,std::milli>(time-m_macroLastInput).count()));
            m_macroLastInterval=static_cast<std::uint32_t>(std::min<std::int64_t>(elapsed,UINT32_MAX));
            MacroEditor::Insert(m_macroDraft.steps,m_macroCursor,{StepKind::Delay,m_macroLastInterval,true},true);}
        MacroEditor::Insert(m_macroDraft.steps,m_macroCursor,step,true);
        m_macroLastInput=time;m_macroHasInput=true;
        if(!m_macroRenderQueued){m_macroRenderQueued=true;auto weak=get_weak();DispatcherQueue().TryEnqueue([weak](){if(auto self=weak.get()){self->m_macroRenderQueued=false;if(!self->m_closed)self->RenderMacroEditor();}});}
    }
    void MainWindow::ToggleMacroRecording(){
        if(m_macroRecording){StopMacroRecording();return;}if(!m_selectedMacro||m_applying||!CommitMacroTime())return;
        CancelButtonReset();if(m_buttonFlyout)m_buttonFlyout.Hide();for(auto const& stage:m_stages)if(stage.flyout)stage.flyout.Hide();
        m_macroErasing=false;m_macroHeldKeys.fill(false);m_macroSkipMouseUp=false;m_macroHasInput=false;m_macroLastInterval=0;
        MacroErrorMessage(L"");
        try{m_macroRecordingLog=std::make_unique<MacroRecordingLog::Recorder>(m_monitoringPaths.directory,m_monitoringPaths.settings,to_string(MacroLibrary::NewId()),to_string(m_macroDraft.id),to_string(m_macroDraft.name),m_macroCursor);}
        catch(...){ProgramLog::Write("macro_recording_log_failed","phase=start",ERROR_WRITE_FAULT);MacroErrorMessage(Localization::Get(Localization::T212));}
        s_macroRecorder=this;
        m_macroKeyboardHook=SetWindowsHookExW(WH_KEYBOARD_LL,MacroKeyboardHook,GetModuleHandleW(nullptr),0);
        m_macroMouseHook=SetWindowsHookExW(WH_MOUSE_LL,MacroMouseHook,GetModuleHandleW(nullptr),0);
        if(!m_macroKeyboardHook||!m_macroMouseHook){StopMacroRecording();MacroErrorMessage(Localization::Get(Localization::T213));return;}
        m_macroRecording=true;MacroStepsScroll().Focus(FocusState::Programmatic);m_macroInvalidSteps.clear();RenderMacroList();RenderMacroEditor();UpdateActions();
    }
    void MainWindow::StopMacroRecording(){
        if(m_macroKeyboardHook){UnhookWindowsHookEx(m_macroKeyboardHook);m_macroKeyboardHook=nullptr;}if(m_macroMouseHook){UnhookWindowsHookEx(m_macroMouseHook);m_macroMouseHook=nullptr;}
        if(s_macroRecorder==this)s_macroRecorder=nullptr;const bool active=m_macroRecording;m_macroRecording=false;
        m_macroHasInput=false;
        if(m_macroRecordingLog){
            auto recorder=std::shared_ptr<MacroRecordingLog::Recorder>(std::move(m_macroRecordingLog));
            auto weak=get_weak();auto queue=DispatcherQueue();const auto cursor=m_macroCursor;
            try{MacroRecordingLog::Finish(recorder,cursor,[weak,queue](DWORD error){
                ProgramLog::Write("macro_recording_log_failed","phase=write",error);
                queue.TryEnqueue([weak]{if(auto self=weak.get();self&&!self->m_closed)self->MacroErrorMessage(Localization::Get(Localization::T214));});
            });}catch(...){recorder->Stop(cursor);}
        }
        if(active&&!m_closed){RenderMacroList();RenderMacroEditor();UpdateActions();}
    }
    LRESULT CALLBACK MainWindow::MacroKeyboardHook(int code,WPARAM message,LPARAM data){
        auto self=s_macroRecorder;if(code==HC_ACTION&&self&&self->m_macroRecording){const auto& input=*reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
            MacroRecordingLog::Event event;if(self->m_macroRecordingLog&&self->m_macroRecordingLog->Enabled())event=MacroRecordingLog::Timestamp("keyboard");event.message=static_cast<DWORD>(message);event.virtualKey=input.vkCode;event.scanCode=input.scanCode;event.flags=input.flags;event.hookTime=input.time;event.extraInfo=input.dwExtraInfo;
            event.usage=KeyboardUsage(input.vkCode,input.scanCode,input.flags);event.cursorBefore=event.cursorAfter=self->m_macroCursor;
            const bool pressed=message==WM_KEYDOWN||message==WM_SYSKEYDOWN;bool failed=false;
            if(input.flags&LLKHF_INJECTED)event.reason="injected";
            else if(event.usage&&(pressed||message==WM_KEYUP||message==WM_SYSKEYUP)&&event.usage<self->m_macroHeldKeys.size()){
                if(self->m_macroHeldKeys[event.usage]==pressed)event.reason=pressed?"repeat":"unpaired_release";
                else try{self->CaptureMacroStep({StepKind::Keyboard,event.usage,pressed});self->m_macroHeldKeys[event.usage]=pressed;event.accepted=true;event.reason="accepted";event.intervalMs=self->m_macroLastInterval;}
                catch(...){event.reason="capture_failed";failed=true;}
            }
            event.cursorAfter=self->m_macroCursor;if(self->m_macroRecordingLog)self->m_macroRecordingLog->Push(event);
            if(failed){self->StopMacroRecording();self->MacroErrorMessage(Localization::Get(Localization::T215));}
        }return CallNextHookEx(nullptr,code,message,data);
    }
    LRESULT CALLBACK MainWindow::MacroMouseHook(int code,WPARAM message,LPARAM data){
        auto self=s_macroRecorder;if(code==HC_ACTION&&self&&self->m_macroRecording){const auto& input=*reinterpret_cast<MSLLHOOKSTRUCT*>(data);
            if(message==WM_MOUSEMOVE&&(!self->m_macroRecordingLog||!self->m_macroRecordingLog->Enabled()))return CallNextHookEx(nullptr,code,message,data);
            MacroRecordingLog::Event event;if(self->m_macroRecordingLog&&self->m_macroRecordingLog->Enabled())event=MacroRecordingLog::Timestamp("mouse");event.message=static_cast<DWORD>(message);event.flags=input.flags;event.hookTime=input.time;event.extraInfo=input.dwExtraInfo;event.x=input.pt.x;event.y=input.pt.y;event.mouseData=input.mouseData;
            event.cursorBefore=event.cursorAfter=self->m_macroCursor;event.reason="non_macro_event";bool failed=false,skip=false;
            const auto capture=[&](Step step){self->CaptureMacroStep(step);if(!event.accepted)event.intervalMs=self->m_macroLastInterval;event.usage=step.value;event.accepted=true;event.reason="accepted";};
            if(input.flags&LLMHF_INJECTED)event.reason="injected";
            else try{
                if(message==WM_LBUTTONDOWN){const auto point=self->MacroRecordButton().TransformToVisual(self->RootGrid()).TransformPoint({0,0});POINT origin{};ClientToScreen(self->m_windowHandle,&origin);const double scale=self->RootGrid().XamlRoot().RasterizationScale();
                    self->m_macroSkipMouseUp=GetAncestor(WindowFromPoint(input.pt),GA_ROOT)==self->m_windowHandle&&input.pt.x>=origin.x+point.X*scale&&input.pt.x<origin.x+(point.X+self->MacroRecordButton().ActualWidth())*scale&&
                        input.pt.y>=origin.y+point.Y*scale&&input.pt.y<origin.y+(point.Y+self->MacroRecordButton().ActualHeight())*scale;
                    skip=self->m_macroSkipMouseUp;
                }
                if(message==WM_LBUTTONUP&&self->m_macroSkipMouseUp){self->m_macroSkipMouseUp=false;skip=true;}
                if(skip)event.reason="record_button";
                else switch(message){
                case WM_LBUTTONDOWN:case WM_LBUTTONUP:capture({StepKind::Mouse,1,message==WM_LBUTTONDOWN});break;
                case WM_RBUTTONDOWN:case WM_RBUTTONUP:capture({StepKind::Mouse,2,message==WM_RBUTTONDOWN});break;
                case WM_MBUTTONDOWN:case WM_MBUTTONUP:capture({StepKind::Mouse,4,message==WM_MBUTTONDOWN});break;
                case WM_XBUTTONDOWN:case WM_XBUTTONUP:capture({StepKind::Mouse,HIWORD(input.mouseData)==XBUTTON1?8u:16u,message==WM_XBUTTONDOWN});break;
                case WM_MOUSEWHEEL:{const auto delta=static_cast<short>(HIWORD(input.mouseData));for(int count=0;count<std::max(1,std::abs(static_cast<int>(delta))/WHEEL_DELTA);++count)capture({StepKind::Wheel,delta>0?0x0au:0xffu,true});break;}
                default:break;
                }
            }catch(...){event.reason="capture_failed";failed=true;}
            event.cursorAfter=self->m_macroCursor;if(self->m_macroRecordingLog)self->m_macroRecordingLog->Push(event);
            if(failed){self->StopMacroRecording();self->MacroErrorMessage(Localization::Get(Localization::T215));}
        }return CallNextHookEx(nullptr,code,message,data);
    }
}
