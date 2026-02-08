#ifndef GUI_THEME_H
#define GUI_THEME_H

#include "imgui.h"

static inline void ApplyXboxTheme()
{
    ImGuiStyle &style = ImGui::GetStyle();

    style.FrameRounding    = 8.0f;
    style.ChildRounding    = 12.0f;
    style.GrabRounding     = 10.0f;
    style.GrabMinSize      = 20.0f;
    style.FramePadding     = ImVec2(10, 8);
    style.ItemSpacing      = ImVec2(10, 10);
    style.WindowPadding    = ImVec2(20, 20);
    style.ChildBorderSize  = 1.0f;
    style.ScrollbarSize    = 12.0f;

    ImVec4 *c = style.Colors;

    c[ImGuiCol_WindowBg]         = ImVec4(0.071f, 0.071f, 0.094f, 1.0f);   /* (18,18,24)    */
    c[ImGuiCol_ChildBg]          = ImVec4(0.094f, 0.094f, 0.125f, 1.0f);   /* (24,24,32)    */
    c[ImGuiCol_Text]             = ImVec4(0.902f, 0.902f, 0.922f, 1.0f);   /* (230,230,235) */
    c[ImGuiCol_TextDisabled]     = ImVec4(0.549f, 0.549f, 0.588f, 1.0f);   /* (140,140,150) */
    c[ImGuiCol_Border]           = ImVec4(0.196f, 0.196f, 0.255f, 1.0f);   /* (50,50,65)    */

    c[ImGuiCol_FrameBg]          = ImVec4(0.157f, 0.157f, 0.196f, 1.0f);   /* (40,40,50)    */
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.196f, 0.196f, 0.255f, 1.0f);   /* (50,50,65)    */
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.216f, 0.216f, 0.275f, 1.0f);   /* (55,55,70)    */

    c[ImGuiCol_SliderGrab]       = ImVec4(0.063f, 0.486f, 0.063f, 1.0f);   /* (16,124,16)   */
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.078f, 0.627f, 0.078f, 1.0f);   /* (20,160,20)   */

    c[ImGuiCol_Button]           = ImVec4(0.118f, 0.118f, 0.157f, 1.0f);   /* (30,30,40)    */
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.176f, 0.176f, 0.227f, 1.0f);   /* (45,45,58)    */
    c[ImGuiCol_ButtonActive]     = ImVec4(0.216f, 0.216f, 0.275f, 1.0f);   /* (55,55,70)    */

    c[ImGuiCol_ScrollbarBg]      = ImVec4(0.071f, 0.071f, 0.094f, 1.0f);   /* (18,18,24)    */
    c[ImGuiCol_ScrollbarGrab]    = ImVec4(0.196f, 0.196f, 0.255f, 1.0f);   /* (50,50,65)    */

    c[ImGuiCol_Header]           = ImVec4(0.118f, 0.118f, 0.157f, 1.0f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.176f, 0.176f, 0.227f, 1.0f);
    c[ImGuiCol_HeaderActive]     = ImVec4(0.216f, 0.216f, 0.275f, 1.0f);

    c[ImGuiCol_Separator]        = ImVec4(0.196f, 0.196f, 0.255f, 1.0f);
    c[ImGuiCol_PopupBg]          = ImVec4(0.094f, 0.094f, 0.125f, 1.0f);
}

#endif /* GUI_THEME_H */
