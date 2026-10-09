#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C"
{
#endif
    /* TreeNew's SDK structures stay on this side of the C/C++ boundary. Text
     * returned by callbacks is borrowed until the next callback or model update. */
    typedef struct WSL_TREE WSL_TREE;
    typedef struct WSL_TREE_COLUMN
    {
        PCWSTR text;
        int width;
        BOOL visible;
        BOOL numeric;
        int order;
    } WSL_TREE_COLUMN;
    typedef struct WSL_TREE_CALLBACKS
    {
        PCWSTR (*text)(void *, int, int);
        void (*colors)(void *, int, COLORREF *, COLORREF *);
        HFONT (*font)(void *, int);
        PCWSTR (*tooltip)(void *, int, int);
        int (*find)(void *, int, PCWSTR, size_t);
        void (*event)(void *, int, int, int, POINT);
    } WSL_TREE_CALLBACKS;
    enum
    {
        WslTreeSelection,
        WslTreeSort,
        WslTreeDoubleClick,
        WslTreeContext,
        WslTreeHeaderContext,
        WslTreeLayout,
        WslTreeDestroy
    };
    WSL_TREE *WslTreeCreate(HWND parent, int id, HINSTANCE instance, void *context,
                            const WSL_TREE_CALLBACKS *callbacks);
    HWND WslTreeWindow(WSL_TREE *tree);
    void WslTreeFree(WSL_TREE *tree);
    void WslTreeSetCount(WSL_TREE *tree, size_t count);
    void WslTreeColumns(WSL_TREE *tree, const WSL_TREE_COLUMN *columns, size_t count);
    int WslTreeColumnWidth(WSL_TREE *tree, int id);
    void WslTreeSetColumnWidth(WSL_TREE *tree, int id, int width);
    int WslTreeVisibleColumns(WSL_TREE *tree, int *ids, size_t capacity);
    void WslTreeSetSort(WSL_TREE *tree, int id, BOOL descending);
    int WslTreeSelected(WSL_TREE *tree, int after);
    void WslTreeSelect(WSL_TREE *tree, int index);
    void WslTreeRestoreSelection(WSL_TREE *tree, const int *indices, size_t count);
    void WslTreeEnsureVisible(WSL_TREE *tree, int index);
    BOOL WslTreeRowRect(WSL_TREE *tree, int index, RECT *rect);
    void WslTreeCenter(WSL_TREE *tree, int index);
    void WslTreeInvalidate(WSL_TREE *tree, int first, int last);
    void WslTreeAutoSize(WSL_TREE *tree, int id);
    void WslTreeSetFont(WSL_TREE *tree, HFONT font);
#ifdef __cplusplus
}
#endif
