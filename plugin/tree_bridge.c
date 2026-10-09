#include <phdk.h>
#include "tree_bridge.h"
#include "host_bridge.h"
#include <stdlib.h>

struct WSL_TREE
{
    HWND window;
    void *context;
    WSL_TREE_CALLBACKS callbacks;
    PPH_TREENEW_NODE nodes;
    PPH_TREENEW_NODE *children;
    size_t count;
    BOOL settingSort;
    BOOL settingColumns;
};

static BOOLEAN NTAPI TreeCallback(HWND window, PH_TREENEW_MESSAGE message, PVOID parameter1, PVOID parameter2,
                                  PVOID context)
{
    WSL_TREE *tree = context;
    POINT point = {0};
    switch (message)
    {
    case TreeNewGetDialogCode:
        *(PULONG)parameter2 = DLGC_WANTARROWS | DLGC_WANTCHARS;
        if (PtrToUlong(parameter1) == VK_RETURN)
            *(PULONG)parameter2 |= DLGC_WANTMESSAGE;
        return TRUE;
    case TreeNewGetChildren: {
        PPH_TREENEW_GET_CHILDREN request = parameter1;
        request->Children = request->Node ? NULL : tree->children;
        request->NumberOfChildren = request->Node ? 0 : (ULONG)tree->count;
        return TRUE;
    }
    case TreeNewIsLeaf:
        ((PPH_TREENEW_IS_LEAF)parameter1)->IsLeaf = TRUE;
        return TRUE;
    case TreeNewGetCellText: {
        PPH_TREENEW_GET_CELL_TEXT request = parameter1;
        PhInitializeStringRef(&request->Text,
                              tree->callbacks.text(tree->context, request->Node->Index, request->Id));
        return TRUE;
    }
    case TreeNewGetNodeColor: {
        PPH_TREENEW_GET_NODE_COLOR request = parameter1;
        tree->callbacks.colors(tree->context, request->Node->Index, &request->BackColor, &request->ForeColor);
        return TRUE;
    }
    case TreeNewGetNodeFont: {
        PPH_TREENEW_GET_NODE_FONT request = parameter1;
        request->Font = tree->callbacks.font(tree->context, request->Node->Index);
        return TRUE;
    }
    case TreeNewGetCellTooltip: {
        PPH_TREENEW_GET_CELL_TOOLTIP request = parameter1;
        PCWSTR text = tree->callbacks.tooltip(tree->context, request->Node->Index, request->Column->Id);
        request->Font = (HFONT)SendMessage(window, WM_GETFONT, 0, 0);
        if (text)
        {
            request->Unfolding = FALSE;
            request->MaximumWidth = MulDiv(550, GetDpiForWindow(window), 96);
            PhInitializeStringRef(&request->Text, text);
        }
        return TRUE;
    }
    case TreeNewIncrementalSearch: {
        PPH_TREENEW_SEARCH_EVENT request = parameter1;
        request->FoundIndex = tree->callbacks.find(tree->context, request->StartIndex, request->String.Buffer,
                                                   request->String.Length / sizeof(WCHAR));
        return TRUE;
    }
    case TreeNewSelectionChanged:
        tree->callbacks.event(tree->context, WslTreeSelection, 0, 0, point);
        return TRUE;
    case TreeNewSortChanged: {
        PPH_TREENEW_SORT_CHANGED_EVENT request = parameter1;
        if (!tree->settingSort)
            tree->callbacks.event(tree->context, WslTreeSort,
                                  request->SortOrder == NoSortOrder ? -1 : (int)request->SortColumn,
                                  request->SortOrder == DescendingSortOrder, point);
        return TRUE;
    }
    case TreeNewLeftDoubleClick:
        if (((PPH_TREENEW_MOUSE_EVENT)parameter1)->Node)
            tree->callbacks.event(tree->context, WslTreeDoubleClick, 0, 0, point);
        return TRUE;
    case TreeNewContextMenu:
        tree->callbacks.event(tree->context, WslTreeContext, 0, 0,
                              ((PPH_TREENEW_CONTEXT_MENU)parameter1)->Location);
        return TRUE;
    case TreeNewHeaderRightClick: {
        PPH_TREENEW_HEADER_MOUSE_EVENT request = parameter1;
        tree->callbacks.event(tree->context, WslTreeHeaderContext,
                              request->Column ? (int)request->Column->Id : -1, 0, request->ScreenLocation);
        return TRUE;
    }
    case TreeNewColumnResized:
    case TreeNewColumnReordered:
        if (!tree->settingColumns)
            tree->callbacks.event(tree->context, WslTreeLayout, 0, 0, point);
        return TRUE;
    }
    return FALSE;
}

WSL_TREE *WslTreeCreate(HWND parent, int id, HINSTANCE instance, void *context,
                        const WSL_TREE_CALLBACKS *callbacks)
{
    WSL_TREE *tree = calloc(1, sizeof(*tree));
    if (!tree)
        return NULL;
    tree->context = context;
    tree->callbacks = *callbacks;
    tree->window = CreateWindowEx(0, PH_TREENEW_CLASSNAME, L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS |
                                      TN_STYLE_DOUBLE_BUFFERED | TN_STYLE_ANIMATE_DIVIDER |
                                      TN_STYLE_ALWAYS_SHOW_SELECTION,
                                  0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, instance, NULL);
    if (!tree->window)
    {
        free(tree);
        return NULL;
    }
    TreeNew_SetCallback(tree->window, TreeCallback, tree);
    TreeNew_SetTriState(tree->window, FALSE);
    TreeNew_SetExtendedFlags(tree->window, TN_FLAG_ITEM_DRAG_SELECT, TN_FLAG_ITEM_DRAG_SELECT);
    // Explorer supplies the normal selection/hover theme. ThemeSupport is the
    // host's dark renderer switch, not a general request for themed controls.
    PhSetControlTheme(tree->window, L"explorer");
    TreeNew_ThemeSupport(tree->window, !!WslHostIntegerSetting(L"EnableThemeSupport"));
    return tree;
}
HWND WslTreeWindow(WSL_TREE *tree)
{
    return tree ? tree->window : NULL;
}
void WslTreeFree(WSL_TREE *tree)
{
    if (!tree)
        return;
    free(tree->children);
    free(tree->nodes);
    free(tree);
}
void WslTreeSetCount(WSL_TREE *tree, size_t count)
{
    PPH_TREENEW_NODE previous = tree->nodes;
    PPH_TREENEW_NODE *previousChildren = tree->children;
    size_t i;
    if (count == tree->count)
        return;
    tree->nodes = count ? calloc(count, sizeof(PH_TREENEW_NODE)) : NULL;
    tree->children = count ? calloc(count, sizeof(PPH_TREENEW_NODE)) : NULL;
    if (count && (!tree->nodes || !tree->children))
    {
        free(tree->nodes);
        free(tree->children);
        tree->nodes = previous;
        tree->children = previousChildren;
        return;
    }
    tree->count = count;
    for (i = 0; i < count; i++)
    {
        PhInitializeTreeNewNode(&tree->nodes[i]);
        tree->children[i] = &tree->nodes[i];
    }
    // Structure rebuild may inspect the former focused/hot nodes. Retain the
    // old allocation until the synchronous rebuild has fully completed.
    TreeNew_SetFocusNode(tree->window, NULL);
    TreeNew_SetHotNode(tree->window, NULL);
    TreeNew_NodesStructured(tree->window);
    free(previousChildren);
    free(previous);
}
void WslTreeColumns(WSL_TREE *tree, const WSL_TREE_COLUMN *columns, size_t count)
{
    size_t i, position;
    BOOL fixed = FALSE;
    ULONG display = 0;
    tree->settingColumns = TRUE;
    // Fixed is immutable in TreeNew_SetColumn: remove/re-add when visibility
    // or order changes, so the first visible column is genuinely fixed.
    for (i = 0; i < count; i++)
        TreeNew_RemoveColumn(tree->window, (ULONG)i);
    for (position = 0; position < count; position++)
    {
        for (i = 0; i < count; i++)
            if (columns[i].order == (int)position)
            {
                PH_TREENEW_COLUMN column = {0};
                column.Id = (ULONG)i;
                column.Text = columns[i].text;
                column.Width = columns[i].width;
                column.Visible = !!columns[i].visible;
                column.Fixed = column.Visible && !fixed;
                column.SortDescending = !!columns[i].numeric;
                column.Alignment = columns[i].numeric ? PH_ALIGN_RIGHT : PH_ALIGN_LEFT;
                column.TextFlags =
                    DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | (columns[i].numeric ? DT_RIGHT : DT_LEFT);
                column.DisplayIndex = column.Fixed ? (ULONG)-1 : display;
                if (column.Visible)
                {
                    if (column.Fixed)
                        fixed = TRUE;
                    else
                        display++;
                }
                TreeNew_AddColumn(tree->window, &column);
                break;
            }
    }
    tree->settingColumns = FALSE;
    // TreeNew column changes update the headers/layout without invalidating
    // populated rows. Repaint every cell after visibility or order changes.
    InvalidateRect(tree->window, NULL, FALSE);
}
int WslTreeColumnWidth(WSL_TREE *tree, int id)
{
    PH_TREENEW_COLUMN column;
    return TreeNew_GetColumn(tree->window, id, &column) && column.Visible ? column.Width : 0;
}
void WslTreeSetColumnWidth(WSL_TREE *tree, int id, int width)
{
    PH_TREENEW_COLUMN column = {0};
    column.Id = id;
    column.Width = width;
    TreeNew_SetColumn(tree->window, TN_COLUMN_WIDTH, &column);
}
int WslTreeVisibleColumns(WSL_TREE *tree, int *ids, size_t capacity)
{
    ULONG i, count = TreeNew_GetVisibleColumnCount(tree->window);
    for (i = 0; i < count && i < capacity; i++)
    {
        PH_TREENEW_COLUMN column;
        if (TreeNew_GetVisibleColumn(tree->window, i, &column))
            ids[i] = column.Id;
    }
    return (int)count;
}
void WslTreeSetSort(WSL_TREE *tree, int id, BOOL descending)
{
    tree->settingSort = TRUE;
    TreeNew_SetSort(tree->window, id < 0 ? 0 : id,
                    id < 0       ? NoSortOrder
                    : descending ? DescendingSortOrder
                                 : AscendingSortOrder);
    tree->settingSort = FALSE;
}
int WslTreeSelected(WSL_TREE *tree, int after)
{
    size_t i;
    for (i = (size_t)(after + 1); i < tree->count; i++)
        if (tree->nodes[i].Selected)
            return (int)i;
    return -1;
}
void WslTreeSelect(WSL_TREE *tree, int index)
{
    TreeNew_DeselectRange(tree->window, 0, (ULONG)-1);
    if (index >= 0 && (size_t)index < tree->count)
    {
        TreeNew_SetFocusNode(tree->window, &tree->nodes[index]);
        TreeNew_SetMarkNode(tree->window, &tree->nodes[index]);
        TreeNew_SelectRange(tree->window, index, index);
    }
}
void WslTreeRestoreSelection(WSL_TREE *tree, const int *indices, size_t count)
{
    size_t i, selected = 0;
    BOOL changed = FALSE;
    // Indices arrive sorted by current display order. Avoid a deselect/reselect
    // cycle on every snapshot: it would disturb selection and keyboard focus.
    for (i = 0; i < tree->count; i++)
    {
        ULONG wanted = selected < count && indices[selected] == (int)i ? 1u : 0u;
        if (wanted)
            selected++;
        if (tree->nodes[i].Selected != wanted)
        {
            tree->nodes[i].Selected = wanted;
            TreeNew_InvalidateNode(tree->window, &tree->nodes[i]);
            changed = TRUE;
        }
    }
    if (changed)
    {
        TreeNew_SetFocusNode(tree->window, count ? &tree->nodes[indices[0]] : NULL);
        if (count)
            TreeNew_SetMarkNode(tree->window, &tree->nodes[indices[0]]);
    }
}
void WslTreeEnsureVisible(WSL_TREE *tree, int index)
{
    if (index >= 0 && (size_t)index < tree->count)
        TreeNew_EnsureVisible(tree->window, &tree->nodes[index]);
}
BOOL WslTreeRowRect(WSL_TREE *tree, int index, RECT *rect)
{
    PH_TREENEW_VIEW_PARTS parts;
    if (index < 0 || (size_t)index >= tree->count)
        return FALSE;
    TreeNew_GetViewParts(tree->window, &parts);
    *rect = parts.ClientRect;
    rect->top = parts.HeaderHeight + (index - parts.VScrollPosition) * parts.RowHeight;
    rect->bottom = rect->top + parts.RowHeight;
    return TRUE;
}
void WslTreeCenter(WSL_TREE *tree, int index)
{
    PH_TREENEW_VIEW_PARTS parts;
    TreeNew_GetViewParts(tree->window, &parts);
    if (parts.RowHeight > 0)
        TreeNew_Scroll(tree->window,
                       index - parts.VScrollPosition -
                           (parts.ClientRect.bottom - parts.HeaderHeight - (LONG)parts.HScrollHeight) /
                               parts.RowHeight / 2,
                       0);
}
void WslTreeInvalidate(WSL_TREE *tree, int first, int last)
{
    TreeNew_InvalidateNodes(tree->window, first, last);
}
void WslTreeAutoSize(WSL_TREE *tree, int id)
{
    TreeNew_AutoSizeColumn(tree->window, id, 0);
}
void WslTreeSetFont(WSL_TREE *tree, HFONT font)
{
    TreeNew_ThemeSupport(tree->window, !!WslHostIntegerSetting(L"EnableThemeSupport"));
    HWND tooltip = TreeNew_GetTooltips(tree->window);
    SendMessage(TreeNew_GetHeader(tree->window), WM_SETFONT, (WPARAM)font, TRUE);
    SendMessage(TreeNew_GetFixedHeader(tree->window), WM_SETFONT, (WPARAM)font, TRUE);
    SendMessage(tooltip, WM_SETFONT, (WPARAM)font, FALSE);
    SendMessage(tooltip, TTM_SETMAXTIPWIDTH, 0, MulDiv(550, GetDpiForWindow(tree->window), 96));
    SendMessage(tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, MAXSHORT);
    SendMessage(tooltip, TTM_SETDELAYTIME, TTDT_INITIAL,
                WslHostIntegerSetting(L"EnableInstantTooltips") ? 0 : -1);
}
