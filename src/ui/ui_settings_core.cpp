#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "bloom_accent_picker.h"
#include "../common/localization.h"

namespace luling::ui {
void MainRenderer::DrawSettingsCore(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    using I = l10n::StringId;
    using H = HitTestResult;
    auto* dc=compositor_->Dc();
    const auto lay=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_);
    auto text=[&](const std::wstring& value,D2D1_RECT_F r,bool is_small=false) {
        painter_.DrawText(value,r,is_small ? compositor_->SmallFormat() : compositor_->TextFormat(),is_small ? theme.text_secondary : theme.text);
    };
    auto draw_card=[&](D2D1_RECT_F r) {
        if(r.bottom<=r.top) return;
        MakeBrush(dc,WithAlpha(theme.surface_card,card_alpha_),brFillInput_); MakeBrush(dc,theme.stroke_card,brStrokeCard_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r,8*scale_,8*scale_),brFillInput_.get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r,8*scale_,8*scale_),brStrokeCard_.get(),1);
    };
    auto divider=[&](D2D1_RECT_F r) {
        MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
        FillRect(dc,brStrokeDivider_.get(),r.left+16*scale_,r.bottom,r.right-r.left-32*scale_,1);
    };
    auto label=[&](D2D1_RECT_F r,const std::wstring& title,const std::wstring& desc,const wchar_t* icon,float text_right=0.0f,float description_height=21.0f) {
        const float right=text_right>0 ? text_right : r.right-72*scale_;
        DrawIconText(r.left+16*scale_,r.top+17*scale_,24*scale_,24*scale_,icon,L"",theme.text_secondary,0.85f);
        text(title,D2D1::RectF(r.left+54*scale_,r.top+10*scale_,right,r.top+34*scale_));
        if(!desc.empty()) {
            const auto bounds=D2D1::RectF(r.left+54*scale_,r.top+35*scale_,right,r.top+(35+description_height)*scale_);
            if(description_height<=21.0f) text(desc,bounds,true);
            else {
                ComPtr<IDWriteTextLayout> description;
                if(SUCCEEDED(compositor_->DwriteFactory()->CreateTextLayout(desc.c_str(),static_cast<UINT32>(desc.size()),
                    compositor_->SmallFormat(),bounds.right-bounds.left,bounds.bottom-bounds.top,&description))) {
                    description->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                    description->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                    MakeBrush(dc,theme.text_secondary,brTextSecondary_);
                    dc->DrawTextLayout(D2D1::Point2F(bounds.left,bounds.top),description.get(),brTextSecondary_.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
    };
    auto toggle=[&](D2D1_RECT_F r,I title,I desc,const wchar_t* icon,bool on,int hit) {
        if(IsHovered(vm,H::SettingsToggle,hit)) {
            MakeBrush(dc,theme.fill_hover,brFillHover_);
            FillRoundedRect(dc,brFillHover_.get(),r.left+2*scale_,r.top+2*scale_,r.right-r.left-4*scale_,r.bottom-r.top-4*scale_,6*scale_);
        }
        label(r,l10n::Get(title),l10n::Get(desc),icon,0.0f,hit==15 ? 43.0f : 21.0f);
        fluent::ControlState state{}; state.checked=on; state.hovered=IsHovered(vm,H::SettingsToggle,hit);
        painter_.DrawSwitch(D2D1::RectF(r.right-60*scale_,r.top+16*scale_,r.right-16*scale_,r.top+48*scale_),L"",state);
    };
    auto button=[&](D2D1_RECT_F r,const std::wstring& value,H::Region region,int index,bool primary=false,bool enabled=true,const wchar_t* glyph=L"") {
        fluent::ControlState st{}; st.enabled=enabled; st.hovered=enabled && IsHovered(vm,region,index);
        painter_.DrawButton({r,value,glyph,primary ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,st});
    };
    auto disclosure=[&](D2D1_RECT_F r,I title,I desc,const wchar_t* icon,int id,bool own_card) {
        if(own_card) draw_card(r);
        if(IsHovered(vm,H::SettingsDisclosure,id)) {
            MakeBrush(dc,theme.fill_hover,brFillHover_);
            FillRoundedRect(dc,brFillHover_.get(),r.left+2*scale_,r.top+2*scale_,r.right-r.left-4*scale_,r.bottom-r.top-4*scale_,6*scale_);
        }
        label(r,l10n::Get(title),l10n::Get(desc),icon);
        DrawIconText(r.right-38*scale_,r.top+22*scale_,18*scale_,18*scale_,
            (vm.settings_expanded&(1u<<id)) ? L"\xE70D" : L"\xE76C",L"",theme.text_secondary,0.75f);
    };
    auto section=[&](int i,I title) { text(l10n::Get(title),lay.section[i]); };
    auto segmented=[&](D2D1_RECT_F card,D2D1_RECT_F const* choices,const I* labels,const int* values,int current,H::Region hit,I title,I desc,int count=3,const wchar_t* icon=L"\xE8A4") {
        const bool stacked=choices[0].left<card.left+100*scale_;
        label(card,l10n::Get(title),l10n::Get(desc),icon,stacked ? card.right-16*scale_ : choices[0].left-12*scale_);
        painter_.DrawSegmentedTrack(D2D1::RectF(choices[0].left,choices[0].top,choices[count-1].right,choices[count-1].bottom));
        for(int i=0;i<count;++i) {
            fluent::SegmentedItemSpec item{};item.bounds=choices[i];item.text=l10n::Get(labels[i]);
            item.state.checked=current==values[i];item.state.hovered=IsHovered(vm,hit,i);
            item.shared_track=true; item.position=i==0 ? fluent::SegmentPosition::First : i==count-1 ? fluent::SegmentPosition::Last : fluent::SegmentPosition::Middle;
            painter_.DrawSegmentedItem(item);
        }
    };
    // Continuous slider in the segment cells: accent fill, Fluent thumb, preset
    // ticks labelled with the former level names, value at the right.
    auto slider=[&](D2D1_RECT_F card,D2D1_RECT_F const* cells,int value,int max_value,const int* ticks,const I* tick_labels,
                    H::Region hit,I title,I desc,const wchar_t* unit) {
        const bool stacked=cells[0].left<card.left+100*scale_;
        label(card,l10n::Get(title),l10n::Get(desc),L"\xE8A4",stacked ? card.right-16*scale_ : cells[0].left-12*scale_);
        const auto g=SettingsSlider(cells,scale_);
        const bool hot=vm.hover_region==static_cast<int>(hit);
        const float span=(std::max)(1.0f,g.right-g.left);
        const auto x_of=[&](int v){return g.left+span*std::clamp(static_cast<float>(v)/static_cast<float>(max_value),0.0f,1.0f);};
        const float x=x_of(value);
        const float th=4*scale_;
        MakeBrush(dc,WithAlpha(theme.text,0.16f),brFillHover_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(g.left,g.cy-th/2,g.right,g.cy+th/2),th/2,th/2),brFillHover_.get());
        MakeBrush(dc,theme.accent,brAccent_);
        if(x>g.left) dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(g.left,g.cy-th/2,x,g.cy+th/2),th/2,th/2),brAccent_.get());
        for(int i=0;i<3;++i) {
            if(ticks[i]<0) continue;
            const float tx=x_of(ticks[i]);
            MakeBrush(dc,WithAlpha(theme.text_secondary,value==ticks[i] ? 0.9f : 0.55f),brTextSecondary_);
            if(ticks[i]>0) dc->FillRectangle(D2D1::RectF(tx-0.5f*scale_,g.cy+5*scale_,tx+0.5f*scale_,g.cy+8*scale_),brTextSecondary_.get());
            const auto lr=ticks[i]==0 ? D2D1::RectF(g.left-9*scale_,g.cy+8*scale_,g.left+60*scale_,g.cy+22*scale_)
                                      : D2D1::RectF(tx-34*scale_,g.cy+8*scale_,tx+34*scale_,g.cy+22*scale_);
            painter_.DrawText(l10n::Get(tick_labels[i]),lr,compositor_->SmallFormat(),
                value==ticks[i] ? theme.text : theme.text_secondary,
                ticks[i]==0 ? fluent::HorizontalAlignment::Left : fluent::HorizontalAlignment::Center);
        }
        MakeBrush(dc,WithAlpha(theme.surface_card,1.0f),brFillInput_);
        MakeBrush(dc,theme.stroke_card,brStrokeCard_);
        const float outer=9*scale_;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,g.cy),outer,outer),brFillInput_.get());
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x,g.cy),outer,outer),brStrokeCard_.get(),1);
        const float inner=(hot ? 6.0f : 5.0f)*scale_;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,g.cy),inner,inner),brAccent_.get());
        painter_.DrawText(std::to_wstring(value)+unit,D2D1::RectF(g.right+10*scale_,g.cy-10*scale_,cells[2].right,g.cy+10*scale_),
            compositor_->TextFormat(),theme.text,fluent::HorizontalAlignment::Right);
    };
        section(0,I::SearchModeName);section(1,I::SearchModeContent);
        draw_card(lay.group[0]);draw_card(lay.group[1]);
        toggle(lay.global_search_row,I::GlobalSearch,I::GlobalSearchDesc,L"\xE721",vm.settings_global_search_enabled,15);divider(lay.global_search_row);
        label(lay.global_search_hotkey_row,l10n::Get(I::GlobalSearchHotkey),
            vm.settings_global_search_error.empty() ? l10n::Get(I::GlobalSearchHotkeyDesc) : vm.settings_global_search_error,
            L"\xE765",lay.global_search_hotkey_row.right-16*scale_);
        button(lay.global_search_hotkey_button,vm.settings_global_search_capturing ? l10n::Get(I::GlobalSearchRecording) : vm.settings_global_search_hotkey,H::SettingsGlobalSearchHotkey,0);
        divider(lay.global_search_hotkey_row);
        toggle(lay.search_pinyin_row,I::SearchPinyin,I::SearchPinyinDesc,L"\xE721",vm.settings_search_pinyin,9);divider(lay.search_pinyin_row);
        label(lay.filename_status,l10n::Get(I::SettingsFilenameIndex),vm.settings_index_status,L"\xE8A5",lay.filename_status.right-16*scale_);divider(lay.filename_status);
        disclosure(lay.disclosure[1],I::SettingsMaintenance,I::SettingsMaintenanceDesc,L"\xE713",1,false);
        label(lay.content_header,l10n::Get(I::SettingsContentFolders),vm.settings_content_summary,L"\xE8B7",
            lay.content_header.right-16*scale_);
        divider(lay.content_header);
        label(lay.content_types,l10n::Get(I::SettingsContentTypes),l10n::Get(I::SettingsContentTypesDesc),L"\xE8A5",
            lay.content_types.right-16*scale_,(lay.content_types.bottom-lay.content_types.top)/scale_-45);
        divider(lay.content_types);
        if(vm.settings_content_folders.empty()) {
            label(lay.content_empty,l10n::Get(I::ContentIndexEmpty),l10n::Get(I::SettingsContentEmptyHelp),L"\xE8B7",lay.content_empty.right-16*scale_);
        }
        if(!vm.settings_content_instant && !vm.settings_content_folders.empty()) button(lay.content_pause,l10n::Get(vm.settings_content_paused ? I::ContentIndexResume : I::ContentIndexPause),H::SettingsContentAction,1);
        label(lay.content_options,l10n::Get(I::SettingsContentOptions),l10n::Get(I::SettingsContentOptionsDesc),L"\xE8A5");
        DrawIconText(lay.content_options.right-38*scale_,lay.content_options.top+22*scale_,18*scale_,18*scale_,L"\xE76C",L"",theme.text_secondary,0.75f);
        text(l10n::Get(vm.settings_content_instant ? I::ContentInstantReady : I::SettingsContentHelp),lay.footer,true);
        if(!vm.settings_content_instant && !vm.settings_content_folders.empty()) {
        divider(lay.content_options);label(lay.content_rebuild,l10n::Get(I::ContentIndexRebuild),l10n::Get(I::SettingsContentRebuildHelp),L"\xE72C");
        DrawIconText(lay.content_rebuild.right-38*scale_,lay.content_rebuild.top+22*scale_,18*scale_,18*scale_,L"\xE76C",L"",theme.text_secondary,0.75f);
        }
}
} // namespace luling::ui
