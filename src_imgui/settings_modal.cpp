// ============================================================================
// DiskMate ImGui 前端 —— 设置模态 / 重命名 / 新建文件夹对话框
//   布局与字段逐项对照 web/index.html 的 buildSettingsForm / saveSettings：
//     · 居中模态 + rgba(0,0,0,.5) 遮罩，点遮罩关闭、✕ 关闭、无 Esc
//     · 四张并排卡片（扫描 / 树图 / 界面 / 文件颜色）+ 整行宽的右键菜单卡片
//     · 标签列固定 132px；数字框 64px；"0=∞" 徽标
//     · 文件颜色卡片改动即时生效；"保存"写 config.json 后显示提示 700ms 再关闭
// ============================================================================
#include "dm.h"
#include "imgui_internal.h"

#include <array>

using namespace dm;

// ---------------------------------------------------------------- 字段 ----
namespace {

enum FieldType { FT_NUM, FT_BOOL };
struct SetField {
    const char* group;
    const char* key;
    FieldType type;
    const char* label;
    bool inf;
    const char* hint;
};

const SetField kFields[] = {
    { "\xe6\x89\xab\xe6\x8f\x8f", "threads", FT_NUM, "\xe6\x89\xab\xe6\x8f\x8f\xe7\xba\xbf\xe7\xa8\x8b\xe6\x95\xb0", false, "1-16" },
    { "\xe6\x89\xab\xe6\x8f\x8f", "skipHidden", FT_BOOL, "\xe8\xb7\xb3\xe8\xbf\x87\xe9\x9a\x90\xe8\x97\x8f/\xe7\xb3\xbb\xe7\xbb\x9f\xe9\xa1\xb9", false, nullptr },
    { "\xe6\x89\xab\xe6\x8f\x8f", "followReparse", FT_BOOL, "\xe8\xb7\x9f\xe9\x9a\x8f\xe9\x87\x8d\xe8\xa7\xa3\xe6\x9e\x90\xe7\x82\xb9\xef\xbc\x88\xe8\x87\xaa\xe5\x8a\xa8\xe9\x98\xb2\xe7\x8e\xaf\xef\xbc\x89", false, nullptr },
    { "\xe6\xa0\x91\xe5\x9b\xbe", "maxDepth", FT_NUM, "\xe9\x80\x92\xe5\xbd\x92\xe5\xb1\x82\xe7\xba\xa7", true, "0 = \xe6\x97\xa0\xe9\x99\x90" },
    { "\xe6\xa0\x91\xe5\x9b\xbe", "minSizeMB", FT_NUM, "\xe9\x80\x92\xe5\xbd\x92\xe6\x9c\x80\xe5\xb0\x8f\xe5\xb0\xba\xe5\xaf\xb8 (MB)", false, "0 = \xe5\x85\xa8\xe9\x83\xa8" },
    { "\xe6\xa0\x91\xe5\x9b\xbe", "maxRects", FT_NUM, "\xe6\x9c\x80\xe5\xa4\xa7\xe7\x9f\xa9\xe5\xbd\xa2\xe6\x95\xb0", true, "0 = \xe6\x97\xa0\xe9\x99\x90" },
    { "\xe6\xa0\x91\xe5\x9b\xbe", "kidsCap", FT_NUM, "\xe6\xaf\x8f\xe5\xb1\x82\xe5\xb1\x95\xe5\xbc\x80\xe4\xb8\x8a\xe9\x99\x90", true, "0 = \xe6\x97\xa0\xe9\x99\x90" },
    { "\xe6\xa0\x91\xe5\x9b\xbe", "lazyDepth", FT_NUM, "\xe6\x87\x92\xe5\x8a\xa0\xe8\xbd\xbd\xe5\xb1\x82\xe7\xba\xa7", true, "0 = \xe5\x85\xa8\xe9\x87\x8f\xe4\xbc\xa0\xe8\xbe\x93\xef\xbc\x9b\xe8\xaf\xa5\xe5\xb1\x82\xe7\xba\xa7\xe4\xb9\x8b\xe4\xb8\x8b\xe7\x9a\x84\xe5\xad\x90\xe7\x9b\xae\xe5\xbd\x95\xe6\x8c\x89\xe9\x9c\x80\xe5\x8a\xa0\xe8\xbd\xbd" },
    { "\xe6\xa0\x91\xe5\x9b\xbe", "showLabels", FT_BOOL, "\xe5\xa4\xa7\xe7\x9f\xa9\xe5\xbd\xa2\xe6\x98\xbe\xe7\xa4\xba\xe5\x90\x8d\xe7\xa7\xb0", false, nullptr },
    { "\xe6\xa0\x91\xe5\x9b\xbe", "colorScheme", FT_NUM, "\xe9\x85\x8d\xe8\x89\xb2\xe6\x96\xb9\xe6\xa1\x88", false, "0=\xe6\x89\xa9\xe5\xb1\x95\xe5\x90\x8d 1=\xe7\xb1\xbb\xe5\x9e\x8b 2=\xe5\x8d\x95\xe8\x89\xb2" },
    { "\xe7\x95\x8c\xe9\x9d\xa2", "defaultSort", FT_NUM, "\xe9\xbb\x98\xe8\xae\xa4\xe6\x8e\x92\xe5\xba\x8f", false, "0=\xe5\xa4\xa7\xe5\xb0\x8f 1=\xe5\x90\x8d\xe7\xa7\xb0" },
    { "\xe7\x95\x8c\xe9\x9d\xa2", "rowExtraPx", FT_NUM, "\xe8\xa1\x8c\xe9\x99\x84\xe5\x8a\xa0\xe9\xab\x98\xe5\xba\xa6 (px)", false, nullptr },
    { "\xe7\x95\x8c\xe9\x9d\xa2", "confirmDelete", FT_BOOL, "\xe5\x88\xa0\xe9\x99\xa4\xe5\x89\x8d\xe7\xb3\xbb\xe7\xbb\x9f\xe7\xa1\xae\xe8\xae\xa4", false, nullptr },
};

struct MenuField { const char* key; const char* label; };
const MenuField kMenuFields[] = {
    { "open", "\xe6\x89\x93\xe5\xbc\x80" },
    { "explore", "\xe5\x9c\xa8\xe8\xb5\x84\xe6\xba\x90\xe7\xae\xa1\xe7\x90\x86\xe5\x99\xa8\xe4\xb8\xad\xe6\x89\x93\xe5\xbc\x80" },
    { "terminal", "\xe6\x89\x93\xe5\xbc\x80\xe7\xbb\x88\xe7\xab\xaf" },
    { "props", "\xe5\xb1\x9e\xe6\x80\xa7" },
    { "copyPath", "\xe5\xa4\x8d\xe5\x88\xb6\xe5\xae\x8c\xe6\x95\xb4\xe8\xb7\xaf\xe5\xbe\x84" },
    { "copyName", "\xe5\xa4\x8d\xe5\x88\xb6\xe6\x96\x87\xe4\xbb\xb6\xe5\x90\x8d" },
    { "copySize", "\xe5\xa4\x8d\xe5\x88\xb6\xe5\xa4\xa7\xe5\xb0\x8f" },
    { "enter", "\xe8\xbf\x9b\xe5\x85\xa5\xe6\xad\xa4\xe7\x9b\xae\xe5\xbd\x95" },
    { "locateList", "\xe5\x9c\xa8\xe5\x88\x97\xe8\xa1\xa8\xe4\xb8\xad\xe5\xae\x9a\xe4\xbd\x8d" },
    { "locateTreemap", "\xe5\x9c\xa8\xe6\xa0\x91\xe5\x9b\xbe\xe4\xb8\xad\xe5\xae\x9a\xe4\xbd\x8d" },
    { "selectParent", "\xe9\x80\x89\xe6\x8b\xa9\xe7\x88\xb6\xe7\xba\xa7" },
    { "goUp", "\xe5\x90\x91\xe4\xb8\x8a" },
    { "back", "\xe5\x90\x8e\xe9\x80\x80" },
    { "forward", "\xe5\x89\x8d\xe8\xbf\x9b" },
    { "rescan", "\xe9\x87\x8d\xe6\x96\xb0\xe6\x89\xab\xe6\x8f\x8f\xe6\xad\xa4\xe9\xa1\xb9" },
    { "deleteRecycle", "\xe5\x88\xa0\xe9\x99\xa4\xe5\x88\xb0\xe5\x9b\x9e\xe6\x94\xb6\xe7\xab\x99" },
    { "deleteForever", "\xe6\xb0\xb8\xe4\xb9\x85\xe5\x88\xa0\xe9\x99\xa4" },
    { "shellMenu", "\xe5\x8c\x85\xe5\x90\xab\xe8\xb5\x84\xe6\xba\x90\xe7\xae\xa1\xe7\x90\x86\xe5\x99\xa8\xe6\x89\xa9\xe5\xb1\x95\xe8\x8f\x9c\xe5\x8d\x95" },
};

int* IntField(AppSettings& s, const char* key) {
    if (!strcmp(key, "threads")) return &s.threads;
    if (!strcmp(key, "maxDepth")) return &s.maxDepth;
    if (!strcmp(key, "minSizeMB")) return &s.minSizeMB;
    if (!strcmp(key, "maxRects")) return &s.maxRects;
    if (!strcmp(key, "kidsCap")) return &s.kidsCap;
    if (!strcmp(key, "lazyDepth")) return &s.lazyDepth;
    if (!strcmp(key, "colorScheme")) return &s.colorScheme;
    if (!strcmp(key, "defaultSort")) return &s.defaultSort;
    if (!strcmp(key, "rowExtraPx")) return &s.rowExtraPx;
    return nullptr;
}
bool* BoolField(AppSettings& s, const char* key) {
    if (!strcmp(key, "skipHidden")) return &s.skipHidden;
    if (!strcmp(key, "followReparse")) return &s.followReparse;
    if (!strcmp(key, "showLabels")) return &s.showLabels;
    if (!strcmp(key, "confirmDelete")) return &s.confirmDelete;
    return nullptr;
}
bool* MenuBool(MenuConfig& m, const char* key) {
    if (!strcmp(key, "open")) return &m.open;
    if (!strcmp(key, "explore")) return &m.explore;
    if (!strcmp(key, "terminal")) return &m.terminal;
    if (!strcmp(key, "props")) return &m.props;
    if (!strcmp(key, "copyPath")) return &m.copyPath;
    if (!strcmp(key, "copyName")) return &m.copyName;
    if (!strcmp(key, "copySize")) return &m.copySize;
    if (!strcmp(key, "enter")) return &m.enter;
    if (!strcmp(key, "locateList")) return &m.locateList;
    if (!strcmp(key, "locateTreemap")) return &m.locateTreemap;
    if (!strcmp(key, "selectParent")) return &m.selectParent;
    if (!strcmp(key, "goUp")) return &m.goUp;
    if (!strcmp(key, "back")) return &m.back;
    if (!strcmp(key, "forward")) return &m.forward;
    if (!strcmp(key, "rescan")) return &m.rescan;
    if (!strcmp(key, "deleteRecycle")) return &m.deleteRecycle;
    if (!strcmp(key, "deleteForever")) return &m.deleteForever;
    if (!strcmp(key, "shellMenu")) return &m.shellMenu;
    return nullptr;
}

// 每分组一个后缀编辑缓冲（未激活时从模型同步）
std::vector<std::array<char, 512>> s_extBuf;
char s_newGroupName[128] = "";
int  s_newGroupColor = 0x5a6472;

const float kLabelW = 132.0f;
const float kNumW = 64.0f;
const float kCardGap = 14.0f;
const float kCardMinW = 230.0f;

}  // namespace

// ------------------------------------------------------ 输入对话框 ----
void DrawInputDialogs() {
    if (g_renameOpen && g_renameNode) ImGui::OpenPopup("##rename");
    if (g_newFolderOpen) ImGui::OpenPopup("##newfolder");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 center(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);

    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("##rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("\xe9\x87\x8d\xe5\x91\xbd\xe5\x90\x8d");   // 重命名
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 5));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##rn", g_renameBuf, sizeof(g_renameBuf));
        ImGui::PopStyleVar();
        ImGui::Spacing();
        bool ok = ImGui::Button("\xe7\xa1\xae\xe5\xae\x9a", ImVec2(90, 28));
        ImGui::SameLine();
        bool cancel = ImGui::Button("\xe5\x8f\x96\xe6\xb6\x88", ImVec2(90, 28)) ||
                      ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (ok) {
            ScanNode* n = g_renameNode;
            if (n) {
                std::wstring oldPath = FullPathOf(n);
                std::wstring newName = Wide(std::string(g_renameBuf));
                size_t sp = oldPath.find_last_of(L"\\/");
                std::wstring dir = (sp == std::wstring::npos) ? L"" : oldPath.substr(0, sp + 1);
                if (!newName.empty() && !dir.empty() && newName != n->name) {
                    if (MoveFileW(oldPath.c_str(), (dir + newName).c_str())) {
                        n->name = newName;
                        g_st0 = L"已重命名：" + newName;
                        g_layoutStamp++;
                        BuildRows();
                    } else {
                        g_st0 = L"重命名失败：" + newName;
                    }
                }
            }
            g_renameOpen = false;
            g_renameNode = nullptr;
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            g_renameOpen = false;
            g_renameNode = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("##newfolder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("\xe6\x96\xb0\xe5\xbb\xba\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9");  // 新建文件夹
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 5));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##nf", g_newFolderBuf, sizeof(g_newFolderBuf));
        ImGui::PopStyleVar();
        ImGui::Spacing();
        bool ok = ImGui::Button("\xe7\xa1\xae\xe5\xae\x9a", ImVec2(90, 28));
        ImGui::SameLine();
        bool cancel = ImGui::Button("\xe5\x8f\x96\xe6\xb6\x88", ImVec2(90, 28)) ||
                      ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (ok) {
            if (g_current) {
                std::wstring dir = FullPathOf(g_current);
                std::wstring name = Wide(std::string(g_newFolderBuf));
                if (!name.empty() && !dir.empty()) {
                    std::wstring full = dir + (dir.back() == L'\\' ? L"" : L"\\") + name;
                    if (CreateDirectoryW(full.c_str(), nullptr)) {
                        // 本地补节点（免重扫）
                        auto node = std::make_unique<ScanNode>();
                        node->name = name;
                        node->isDir = true;
                        node->parent = g_current;
                        ScanNode* raw = node.get();
                        if (g_tree) g_tree->arena.push_back(std::move(node));
                        g_current->children.push_back(raw);
                        g_expanded.insert(g_current);
                        g_st0 = L"已新建文件夹：" + name;
                        RefreshSubtree(g_current);
                        g_selNode = nullptr;
                        for (ScanNode* c : g_current->children)
                            if (c->name == name) { g_selNode = c; break; }
                        g_layoutStamp++;
                        BuildRows();
                    } else {
                        g_st0 = L"新建文件夹失败：" + name;
                    }
                }
            }
            g_newFolderOpen = false;
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            g_newFolderOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------- 设置模态 ----
// 单窗口实现：窗口本身铺满视口、WindowBg 用 rgba(0,0,0,.5) 当遮罩，
// 面板画在窗口内部 —— 避免"遮罩窗口 + 模态窗口"两层窗口的层级问题。
void DrawSettingsModal() {
    if (!g_showSettings) return;
    if (!g_setHint.empty() && g_setHintAt > 0) {
        if (ImGui::GetTime() * 1000.0 - g_setHintAt > HINT_MS) {
            g_showSettings = false;
            g_setHint.clear();
            g_setHintAt = 0;
            return;
        }
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 origin = vp->Pos;
    ImVec2 vsize = vp->Size;
    float mw = std::min(1000.0f, std::max(560.0f, vsize.x * 0.78f));   // clamp(560px,78vw,1000px)
    float mh = vsize.y * 0.86f;                                        // max-height:86vh
    ImVec2 mp(origin.x + (vsize.x - mw) * 0.5f, origin.y + (vsize.y - mh) * 0.5f);
    ImVec2 mp1(mp.x + mw, mp.y + mh);

    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(vsize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0.5f));
    ImGui::Begin("##settings", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* f = g_fontUi ? g_fontUi : ImGui::GetFont();

    // 点遮罩空白处关闭（HTML: #setMask 的 click 判定）
    bool panelHover = ImGui::IsMouseHoveringRect(mp, mp1, false);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !panelHover) {
        g_showSettings = false;
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return;
    }

    // ---- 面板 ----
    dl->AddRectFilled(mp, mp1, ColPanel(), 10.0f);
    dl->AddRect(mp, mp1, ColLine(), 10.0f, 0, 1.0f);

    const float headH = 44.0f, footH = 46.0f;
    dl->AddText(f, 14.0f, ImVec2(mp.x + 16.0f, mp.y + (headH - 14.0f) / 2.0f), ColText(),
                "\xe8\xae\xbe\xe7\xbd\xae");   // 设置
    dl->AddLine(ImVec2(mp.x, mp.y + headH), ImVec2(mp1.x, mp.y + headH), ColLine());
    // ✕ 关闭
    {
        ImVec2 a(mp1.x - 40.0f, mp.y + 8.0f), b(mp1.x - 8.0f, mp.y + 36.0f);
        bool hov = ImGui::IsMouseHoveringRect(a, b, false);
        if (hov) {
            dl->AddRectFilled(a, b, ColBtnHover(), 4.0f);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                g_showSettings = false;
                ImGui::End();
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
                return;
            }
        }
        dl->AddText(f, 15.0f, ImVec2(a.x + 11.0f, a.y + 4.0f), ColDim(), "\xe2\x9c\x95");
    }

    // ---- 正文（可滚动）----
    ImGui::SetCursorScreenPos(ImVec2(mp.x, mp.y + headH));
    ImGui::BeginChild("##setbody", ImVec2(mw, mh - headH - footH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoMove);
    ImVec2 bodyPos = ImGui::GetWindowPos();
    float bodyW = ImGui::GetWindowWidth() - 32.0f;
    ImVec2 cur(bodyPos.x + 16.0f, bodyPos.y + 14.0f);
    ImDrawList* bdl = ImGui::GetWindowDrawList();

    auto drawCardBg = [&](ImVec2 a, ImVec2 b) {
        bdl->AddRectFilled(a, b, BlendText(ColPanel(), ColText(), 0.03f), 8.0f);
        bdl->AddRect(a, b, ColLine(), 8.0f, 0, 1.0f);
    };
    auto drawCardTitle = [&](ImVec2 a, const char* title) {
        bdl->AddText(f, 12.0f, ImVec2(a.x + 12.0f, a.y + 10.0f), ColAccent(), title);
    };

    const char* groupNames[4] = { kFields[0].group, kFields[3].group, kFields[10].group,
                                  "\xe6\x96\x87\xe4\xbb\xb6\xe9\xa2\x9c\xe8\x89\xb2" };  // 文件颜色
    int counts[4] = { 0, 0, 0, 0 };
    for (const auto& fl : kFields) {
        if (!strcmp(fl.group, groupNames[0])) counts[0]++;
        else if (!strcmp(fl.group, groupNames[1])) counts[1]++;
        else if (!strcmp(fl.group, groupNames[2])) counts[2]++;
    }
    counts[3] = (int)g_extMap.size() + 2;   // 其他 + 新分类

    // 卡片列数：minmax(230px,1fr) + 14px gap 的 flex-wrap
    int perRow = 4;
    while (perRow > 1 && (bodyW - (float)(perRow - 1) * kCardGap) / (float)perRow < kCardMinW) perRow--;
    float cardW = (bodyW - (float)(perRow - 1) * kCardGap) / (float)perRow;

    const float rowPat = 3.0f, rowH = 26.0f;
    float maxCardH = 0;
    for (int i = 0; i < 4; i++) maxCardH = std::max(maxCardH, 30.0f + (float)counts[i] * rowH + 12.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
    float colX0 = cur.x;
    for (int gi = 0; gi < 4; gi++) {
        int col = gi % perRow, row = gi / perRow;
        ImVec2 a(colX0 + (float)col * (cardW + kCardGap), cur.y + (float)row * (maxCardH + kCardGap));
        ImVec2 b(a.x + cardW, a.y + maxCardH);
        drawCardBg(a, b);
        drawCardTitle(a, groupNames[gi]);
        float ix = a.x + 12.0f, iy = a.y + 32.0f, iw = cardW - 24.0f;

        if (gi < 3) {
            for (const auto& fl : kFields) {
                if (strcmp(fl.group, groupNames[gi])) continue;
                ImGui::SetCursorScreenPos(ImVec2(ix, iy + rowPat));
                ImGui::TextUnformatted(fl.label);
                ImGui::SetCursorScreenPos(ImVec2(ix + kLabelW, iy + rowPat));
                if (fl.type == FT_BOOL) {
                    bool* bv = BoolField(g_stEdit, fl.key);
                    if (bv && ImGui::Checkbox((std::string("##") + fl.key).c_str(), bv))
                        g_settingsDirty = true;
                    if (ImGui::IsItemHovered() && fl.hint) ImGui::SetTooltip("%s", fl.hint);
                } else {
                    int* iv = IntField(g_stEdit, fl.key);
                    if (iv) {
                        ImGui::SetNextItemWidth(kNumW);
                        if (ImGui::InputInt((std::string("##") + fl.key).c_str(), iv, 0, 0))
                            g_settingsDirty = true;
                        if (ImGui::IsItemHovered() && fl.hint) ImGui::SetTooltip("%s", fl.hint);
                        // HTML：inf 字段输入框右侧显示 "0=∞" 徽标，其余 hint 只作 title 提示
                        if (fl.inf)
                            bdl->AddText(f, 10.0f, ImVec2(ix + kLabelW + kNumW + 8.0f, iy + rowPat + 4.0f),
                                         ColDim(), "0=\xe2\x88\x9e");
                    }
                }
                iy += rowH;
            }
        } else {
            // ---- 文件颜色卡片（改动即时生效）----
            if (s_extBuf.size() != g_extMap.size()) {
                s_extBuf.assign(g_extMap.size(), {});
                for (size_t k = 0; k < g_extMap.size(); k++) {
                    std::string joined;
                    for (size_t e = 0; e < g_extMap[k].exts.size(); e++) {
                        if (e) joined += ",";
                        joined += Utf8(g_extMap[k].exts[e]);
                    }
                    strncpy_s(s_extBuf[k].data(), s_extBuf[k].size(), joined.c_str(), _TRUNCATE);
                }
            }
            size_t k = 0;
            for (; k < g_extMap.size(); k++) {
                ExtGroup& grp = g_extMap[k];
                ImGui::SetCursorScreenPos(ImVec2(ix, iy + rowPat + 3.0f));
                ImGui::TextUnformatted(Utf8(grp.name).c_str());
                float c[4] = { ((grp.color >> IM_COL32_R_SHIFT) & 255) / 255.0f,
                               ((grp.color >> IM_COL32_G_SHIFT) & 255) / 255.0f,
                               ((grp.color >> IM_COL32_B_SHIFT) & 255) / 255.0f, 1.0f };
                ImGui::SetCursorScreenPos(ImVec2(ix + 78.0f, iy + rowPat));
                ImGui::SetNextItemWidth(44);
                if (ImGui::ColorEdit4((std::string("##ec") + std::to_string(k)).c_str(), c,
                                      ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
                                          ImGuiColorEditFlags_NoAlpha)) {
                    grp.color = IM_COL32((int)(c[0] * 255), (int)(c[1] * 255), (int)(c[2] * 255), 255);
                    prefs::SaveUiPrefs();
                    g_layoutStamp++;
                }
                ImGui::SetCursorScreenPos(ImVec2(ix + 126.0f, iy + rowPat));
                ImGui::SetNextItemWidth(std::max(50.0f, iw - 126.0f - 26.0f));
                if (ImGui::InputText((std::string("##ex") + std::to_string(k)).c_str(),
                                     s_extBuf[k].data(), s_extBuf[k].size())) {
                    grp.exts.clear();
                    std::string s = s_extBuf[k].data();
                    size_t p = 0;
                    while (p < s.size()) {
                        size_t c2 = s.find(',', p);
                        if (c2 == std::string::npos) c2 = s.size();
                        std::string tok = s.substr(p, c2 - p);
                        while (!tok.empty() && isspace((unsigned char)tok.front())) tok.erase(tok.begin());
                        while (!tok.empty() && isspace((unsigned char)tok.back())) tok.pop_back();
                        std::transform(tok.begin(), tok.end(), tok.begin(), ::tolower);
                        if (!tok.empty()) grp.exts.push_back(Wide(tok));
                        p = c2 + 1;
                    }
                    prefs::SaveUiPrefs();
                    g_layoutStamp++;
                }
                ImGui::SameLine(0, 4);
                if (ImGui::SmallButton((std::string("\xe2\x9c\x95##d") + std::to_string(k)).c_str())) {
                    g_extMap.erase(g_extMap.begin() + (ptrdiff_t)k);
                    s_extBuf.clear();
                    prefs::SaveUiPrefs();
                    g_layoutStamp++;
                    break;
                }
                iy += rowH;
            }
            if (k >= g_extMap.size()) {
                // 其他（兜底色）
                ImGui::SetCursorScreenPos(ImVec2(ix, iy + rowPat + 3.0f));
                ImGui::TextUnformatted("\xe5\x85\xb6\xe4\xbb\x96");   // 其他
                float c[4] = { ((g_extFallback >> IM_COL32_R_SHIFT) & 255) / 255.0f,
                               ((g_extFallback >> IM_COL32_G_SHIFT) & 255) / 255.0f,
                               ((g_extFallback >> IM_COL32_B_SHIFT) & 255) / 255.0f, 1.0f };
                ImGui::SetCursorScreenPos(ImVec2(ix + 78.0f, iy + rowPat));
                ImGui::SetNextItemWidth(44);
                if (ImGui::ColorEdit4("##ecfb", c,
                                      ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
                                          ImGuiColorEditFlags_NoAlpha)) {
                    g_extFallback = IM_COL32((int)(c[0] * 255), (int)(c[1] * 255),
                                             (int)(c[2] * 255), 255);
                    prefs::SaveUiPrefs();
                    g_layoutStamp++;
                }
                iy += rowH;
                // 新分类
                ImGui::SetCursorScreenPos(ImVec2(ix, iy + rowPat));
                ImGui::SetNextItemWidth(std::max(50.0f, iw - 76.0f));
                ImGui::InputTextWithHint("##newname", "\xe5\x88\x86\xe7\xb1\xbb\xe5\x90\x8d",
                                         s_newGroupName, sizeof(s_newGroupName));
                ImGui::SameLine(0, 4);
                if (ImGui::SmallButton("\xe6\xb7\xbb\xe5\x8a\xa0")) {   // 添加
                    std::string nm = s_newGroupName;
                    if (!nm.empty()) {
                        bool exists = false;
                        for (const auto& gg : g_extMap)
                            if (Utf8(gg.name) == nm) { exists = true; break; }
                        if (!exists) {
                            ExtGroup ng;
                            ng.name = Wide(nm);
                            ng.key = Wide(nm);   // 自定义分组：键名就用分组名（Web 侧显示键名）
                            ng.color = IM_COL32(0x5a, 0x64, 0x72, 255);
                            g_extMap.push_back(ng);
                            s_extBuf.clear();
                            s_newGroupName[0] = 0;
                            prefs::SaveUiPrefs();
                            g_layoutStamp++;
                        }
                    }
                }
            }
        }
    }
    int rowsUsed = (4 + perRow - 1) / perRow;
    cur.y += (float)rowsUsed * (maxCardH + kCardGap);

    // ---- 右键菜单卡片（整行宽、多列密集排）----
    {
        int menuCols = (int)floorf((bodyW + 18.0f) / 228.0f + 1e-3f);
        if (menuCols < 1) menuCols = 1;
        if (menuCols > 5) menuCols = 5;
        int menuRows = (18 + menuCols - 1) / menuCols;
        float menuH = 34.0f + (float)menuRows * 24.0f + 10.0f;
        ImVec2 a(cur.x, cur.y), b(cur.x + bodyW, cur.y + menuH);
        drawCardBg(a, b);
        drawCardTitle(a, "\xe5\x8f\xb3\xe9\x94\xae\xe8\x8f\x9c\xe5\x8d\x95\xef\xbc\x88\xe6\x98\xbe\xe7\xa4\xba\xe5\x93\xaa\xe4\xba\x9b\xe9\xa1\xb9\xef\xbc\x89");
        float colW = (bodyW - 24.0f) / (float)menuCols;
        for (int mi = 0; mi < 18; mi++) {
            int cx = mi / menuRows, cy = mi % menuRows;
            float x = a.x + 12.0f + (float)cx * colW;
            float y = a.y + 34.0f + (float)cy * 24.0f;
            bool* bv = MenuBool(g_st.menu, kMenuFields[mi].key);
            ImGui::SetCursorScreenPos(ImVec2(x, y + 4.0f));
            ImGui::TextUnformatted(kMenuFields[mi].label);
            ImGui::SetCursorScreenPos(ImVec2(x + colW - 26.0f, y + 2.0f));
            if (bv && ImGui::Checkbox((std::string("##mk") + kMenuFields[mi].key).c_str(), bv))
                g_settingsDirty = true;
        }
        cur.y += menuH;
    }
    ImGui::Dummy(ImVec2(1, 4));
    ImGui::PopStyleVar(2);
    ImGui::EndChild();

    // ---- 底栏 ----
    dl->AddLine(ImVec2(mp.x, mp1.y - footH), ImVec2(mp1.x, mp1.y - footH), ColLine());
    if (!g_setHint.empty())
        dl->AddText(f, 12.0f, ImVec2(mp.x + 16.0f, mp1.y - footH + (footH - 12.0f) / 2.0f),
                    ColDim(), Utf8(g_setHint).c_str());
    {
        const char* save = "\xe4\xbf\x9d\xe5\xad\x98";   // 保存
        float bw = f->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, save).x + 28.0f;
        ImVec2 a(mp1.x - 16.0f - bw, mp1.y - footH + 8.0f);
        ImVec2 b(a.x + bw, a.y + 30.0f);
        bool hov = ImGui::IsMouseHoveringRect(a, b, false);
        dl->AddRectFilled(a, b, hov ? AlphaOf(ColAccent(), 230) : ColAccent(), 6.0f);
        ImVec2 tsz = f->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, save);
        dl->AddText(f, 13.0f, ImVec2(a.x + (bw - tsz.x) * 0.5f, a.y + (30.0f - 13.0f) / 2.0f),
                    IM_COL32(255, 255, 255, 255), save);
        if (hov) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                g_st = g_stEdit;
                SaveSettings(g_st);
                prefs::SaveUiPrefs();
                ApplyImGuiTheme();
                g_layoutStamp++;
                TreemapInvalidate();
                BuildRows();
                g_setHint = L"\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98\xef\xbc\x88\xe6\x89\xab\xe6\x8f\x8f\xe7\xb1\xbb\xe8\xae\xbe\xe7\xbd\xae\xe4\xb8\x8b\xe6\xac\xa1\xe6\x89\xab\xe6\x8f\x8f\xe7\x94\x9f\xe6\x95\x88\xef\xbc\x89";
                g_setHintAt = ImGui::GetTime() * 1000.0;
                LogLine(L"settings saved");
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}
