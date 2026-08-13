;DropDown.au3
#include-once

#include <GuiListView.au3>
;#include <_WindowCompozitionAttribute.au3>

;~ Global $aLVStyles[19][2] = [['$LVS_ALIGNLEFT', '0x' & Hex($LVS_ALIGNLEFT, 8)], ['$LVS_ALIGNTOP', '0x' & Hex($LVS_ALIGNTOP, 8)], ['$LVS_AUTOARRANGE', '0x' & Hex($LVS_AUTOARRANGE, 8)], ['$LVS_EDITLABELS', '0x' & Hex($LVS_EDITLABELS, 8)], ['$LVS_ICON', '0x' & Hex($LVS_ICON, 8)], _
;~ 		['$LVS_LIST', '0x' & Hex($LVS_LIST, 8)], ['$LVS_NOCOLUMNHEADER', '0x' & Hex($LVS_NOCOLUMNHEADER, 8)], ['$LVS_NOLABELWRAP', '0x' & Hex($LVS_NOLABELWRAP, 8)], ['$LVS_NOSCROLL', '0x' & Hex($LVS_NOSCROLL, 8)], ['$LVS_NOSORTHEADER', '0x' & Hex($LVS_NOSORTHEADER, 8)], ['$LVS_OWNERDATA', '0x' & Hex($LVS_OWNERDATA, 8)], _
;~ 		['$LVS_OWNERDRAWFIXED', '0x' & Hex($LVS_OWNERDRAWFIXED, 8)], ['$LVS_REPORT', '0x' & Hex($LVS_REPORT, 8)], ['$LVS_SHAREIMAGELISTS', '0x' & Hex($LVS_SHAREIMAGELISTS, 8)], ['$LVS_SHOWSELALWAYS', '0x' & Hex($LVS_SHOWSELALWAYS, 8)], ['$LVS_SINGLESEL', '0x' & Hex($LVS_SINGLESEL, 8)], _
;~ 		['$LVS_SMALLICON', '0x' & Hex($LVS_SMALLICON, 8)], ['$LVS_SORTASCENDING', '0x' & Hex($LVS_SORTASCENDING, 8)], ['$LVS_SORTDESCENDING', '0x' & Hex($LVS_SORTDESCENDING, 8)]]

Global Enum $id_DropMenuInfo, $id_MenuInfo, $id_HeaderMenuInfo, $id_MenuCreate, $id_HeaderCreate, $id_Max
Global $_a__DropDownData[0][$id_Max]
Global Const $DM_ALPHABK = 150
Global $DD_hImage

Global Const $DMNS_MODELESS = 0x00100000
Global Const $DMNS_NOCHECK = 0x00200000
Global Const $DMNS_NOTIFYBYPOS = 0x00400000
Global Const $DMNS_ROUNDSMALLFRAME = 0x01000000
Global Const $DMNS_ROUNDFRAME = 0x02000000
Global Const $DMNS_BORDERFRAME = 0x04000000
Global Const $DMNS_BLURBACKGROUND = 0x10000000
Global Const $DMNS_TRANSPARENTBACKGROUND = 0x20000000

Global Const $DMT_TYPE = 0x0F00
Global Const $DMT_BITMAP = 0x0100
Global Const $DMT_RADIOCHECK = 0x0200
Global Const $DMT_RIGHTJUSTIFY = 0x0400
Global Const $DMT_SEPARATOR = 0x0800

Global Const $DMT_STATE = 0xF0FF
Global Const $DMS_DISABLED = 0x0004
Global Const $DMS_CHECKED = 0x2000
Global Const $DMS_UNCHECKED = 0x1000
Global Const $DMS_SELECTED = $LVIS_SELECTED
Global Const $DMS_FOCUSEDED = $LVIS_FOCUSED

Global Const $DMM_GETINFOMENU = _WinAPI_RegisterWindowMessage("DMM_GETINFOMENU")
Global Const $DMM_INITMENU = _WinAPI_RegisterWindowMessage("DMM_INITMENU")
Global Const $DMM_MENUSELECT = _WinAPI_RegisterWindowMessage("DMM_MENUSELECT")
Global Const $DMM_UNINITMENU = _WinAPI_RegisterWindowMessage("DMM_UNINITMENU")
Global Const $DMM_SETHEIGHTMENU = _WinAPI_RegisterWindowMessage("DMM_SETHEIGHTMENU")
Global Const $DMM_SETWIDTHMENU = _WinAPI_RegisterWindowMessage("DMM_SETWIDTHMENU")
Global Const $DMM_GETVIEWRECT = _WinAPI_RegisterWindowMessage("DMM_GETVIEWRECT")
Global Const $DMM_GETMINHEIGHT = _WinAPI_RegisterWindowMessage("DMM_GETMINHEIGHT")
Global Const $DMM_GETWIDTHSCROLLBAR = _WinAPI_RegisterWindowMessage("DMM_GETWIDTHSCROLLBAR")
Global Const $DMM_GETSIZE = _WinAPI_RegisterWindowMessage("DMM_GETSIZE")
Global Const $DMM_SETITEMHEIGHT = _WinAPI_RegisterWindowMessage("DMM_SETITEMHEIGHT")
Global Const $DMM_SETAUTOSIZE = _WinAPI_RegisterWindowMessage("DMM_SETAUTOSIZE")

OnAutoItExitRegister("_OnDropDownExit")

Global Const $tagDRAWITEM = _
		"uint   CtlType;" & _
		"uint   CtlID;" & _
		"uint   itemID;" & _
		"uint   itemAction;" & _
		"uint   itemState;" & _
		"hwnd   hwndItem;" & _
		"handle hDC;" & _
		$tagRECT & ";" & _
		"ulong_ptr itemData"

Global Const $tagDROPMENUINFO = _
		'uint  Index;' & _
		'uint  GuiStyle;' & _
		'HWND  hwndParent;' & _
		'int   ItemHeight;' & _
		'uint  Style;' & _
		'ptr   hImage;' & _
		'byte  Font[92];' & _
		'uint  Data;' & _
		$tagSIZE & ';' & _
		'HWND  hDropDown;' & _
		'HWND  hPopUp;' & _
		'HWND  hHeaderView;' & _
		'HWND  hMenuView;' & _
		'HWND  hParent;' & _
		'ptr     hSubMenu;' & _
		'uint    SelectMenu;' & _
		'int     Level;' & _
		'boolean Init;' & _
		'boolean Notify'
		
Global Const $tagDROPMENUITEM = _
		'wchar Text[96];' & _
		'int   itemID;' & _
		'int   Image;' & _
		'int   State'

Global Const $tagLVINFOMENU = _
		'handle  hBitmap;' & _
		'int64   SizeCheck;' & _
		'int64   SizeSubMenu;' & _
		'HWND    hNext;' & _
		'ptr     hMenu;' & _
		'boolean MouseHot'

Global Const $tagDROPMENUITEMINFO = _
		"uint Type;" & _
		"uint State;" & _
		"uint ID;" & _
		"ptr  SubMenu;" & _
		"uint Param;" & _
		"int  BmpItem"

Global Const $tagCREATESTRUCT = _
		"lparam    lpCreateParams;" & _
		"ptr       hInstance;" & _
		"handle    hMenu;" & _
		"HWND      hwndParent;" & _
		"int       cy;" & _
		"int       cx;" & _
		"int       y;" & _
		"int       x;" & _
		"LONG      style;" & _
		"ptr       lpszName;" & _
		"ptr       lpszClass;" & _
		"DWORD     dwExStyle"

Global Const $h__instance_gui = _WinAPI_GetModuleHandle(0)
Global Const $p__dropdown_class = _GuiRegisterClass($h__instance_gui, 'DropDown', 0, _WinAPI_GetSysColor($COLOR_WINDOW), '_DropProc')
Global Const $p__listviewpopup_class = _GuiRegisterClass($h__instance_gui, 'ListviewPopup', 0, _WinAPI_GetSysColor($COLOR_WINDOW), '_PopupProc')

Global Const $__dm_maxwidth = 264
Global Const $__dm_maxcount = 18
Global Const $__dm_mincount = 8

#Region Functions

Func _GuiCtrlDropMenu_CreatePopup($hWnd, $iStyle = 0, $iWidth = 0, $iHeight = 0, $iItemHeight = 21)
	Local $iGuiStyle = 0x8480009F
	If BitAND($iStyle, $DMNS_BORDERFRAME) Then $iGuiStyle = BitOR($iGuiStyle, $WS_CAPTION)
	Local $iIndex = UBound($_a__DropDownData)
	ReDim $_a__DropDownData[$iIndex + 1][$id_Max]
	$_a__DropDownData[$iIndex][$id_DropMenuInfo] = DllStructCreate($tagDROPMENUINFO)
	DllStructSetData($_a__DropDownData[$iIndex][$id_DropMenuInfo], 'Index', $iIndex)
	DllStructSetData($_a__DropDownData[$iIndex][$id_DropMenuInfo], 'GuiStyle', $iGuiStyle)
	DllStructSetData($_a__DropDownData[$iIndex][$id_DropMenuInfo], 'hwndParent', $hWnd)
	DllStructSetData($_a__DropDownData[$iIndex][$id_DropMenuInfo], 'Style', $iStyle)
	DllStructSetData($_a__DropDownData[$iIndex][$id_DropMenuInfo], 'X', $iWidth)
	DllStructSetData($_a__DropDownData[$iIndex][$id_DropMenuInfo], 'Y', $iHeight)
	DllStructSetData($_a__DropDownData[$iIndex][$id_DropMenuInfo], 'ItemHeight', $iItemHeight)
	
	Local $tFont = _WinAPI_GetThemeSysFont(0, $TMT_MENUFONT)
	DllStructSetData($_a__DropDownData[$iIndex][$id_DropMenuInfo], "Font", __StructureToBinary($tFont, $tagLOGFONTW))
	
	Local $aData[0]
	$_a__DropDownData[$iIndex][$id_HeaderCreate] = $aData
	$_a__DropDownData[$iIndex][$id_MenuCreate] = $aData
	
	Return DllStructGetPtr($_a__DropDownData[$iIndex][$id_DropMenuInfo])
EndFunc   ;==>_GuiCtrlDropMenu_CreatePopup

Func _GuiCtrlDropMenu_TrackPopupMenu($hMenu, $iX = -1, $iY = -1, $iAlignX = 1, $iAlignY = 1, $iNotify = 0, $iButtons = 0)
	If Not _GuiCtrlDropMenu_IsMenu($hMenu) Then Return
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hwndParent')
	Return __TrackPopupMenu($hMenu, $hWnd, $iX, $iY, $iAlignX, $iAlignY, $iNotify, $iButtons)
EndFunc   ;==>_GuiCtrlDropMenu_TrackPopupMenu

Func _GuiCtrlDropMenu_SetImageList($hMenu, $hImage)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	DllStructSetData($tDROPMENUINFO, 'hImage', _GUIImageList_Duplicate($hImage))
	$hImage = DllStructGetData($tDROPMENUINFO, 'hImage')
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If _WinAPI_IsWindow($hWnd) Then _GUICtrlListView_SetImageList($hWnd, $hImage, 1)
	$hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If _WinAPI_IsWindow($hWnd) Then _GUICtrlListView_SetImageList($hWnd, $hImage, 1)
	Return 1
EndFunc

Func _GuiCtrlDropMenu_GetImageList($hMenu)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Return DllStructGetData($tDROPMENUINFO, 'hImage')
EndFunc

Func _GuiCtrlDropMenu_AddHeaderMenuItem($hMenu, $sText = '', $iIndex = -1, $hSubMenu = 0)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $i = DllStructGetData($tDROPMENUINFO, 'Index')
	Local $aData = $_a__DropDownData[$i][$id_HeaderCreate]
	Local $j = UBound($aData)
	ReDim $aData[$j + 1]
	Local $iState = 0
	If $iIndex > -1 Then $iState = $DMT_BITMAP
	If Not $sText Then $iState = BitOR($DMT_SEPARATOR, $DMS_DISABLED)
	$aData[$j] = DllStructCreate($tagDROPMENUITEM)
	DllStructSetData($aData[$j], "Text", $sText)
	DllStructSetData($aData[$j], "itemID", $hSubMenu)
	DllStructSetData($aData[$j], "Image", $iIndex)
	DllStructSetData($aData[$j], "State", $iState)
	$_a__DropDownData[$i][$id_HeaderCreate] = $aData
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If _WinAPI_IsWindow($hWnd) Then Return __DM_AddMenuItem($hWnd, $sText, $iIndex, $hSubMenu)
	Return $j
EndFunc   ;==>_GuiCtrlDropMenu_AddHeaderMenuItem

Func _GuiCtrlDropMenu_AddMenuItem($hMenu, $sText = '', $iIndex = -1, $hSubMenu = 0)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $i = DllStructGetData($tDROPMENUINFO, 'Index')
	Local $aData = $_a__DropDownData[$i][$id_MenuCreate]
	Local $j = UBound($aData)
	ReDim $aData[$j + 1]
	Local $iState = 0
	If $iIndex > -1 Then $iState = $DMT_BITMAP
	If Not $sText Then $iState = BitOR($DMT_SEPARATOR, $DMS_DISABLED)
	$aData[$j] = DllStructCreate($tagDROPMENUITEM)
	DllStructSetData($aData[$j], "Text", $sText)
	DllStructSetData($aData[$j], "itemID", $hSubMenu)
	DllStructSetData($aData[$j], "Image", $iIndex)
	DllStructSetData($aData[$j], "State", $iState)
	$_a__DropDownData[$i][$id_MenuCreate] = $aData
	
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If _WinAPI_IsWindow($hWnd) Then Return __DM_AddMenuItem($hWnd, $sText, $iIndex, $hSubMenu)
	Return $j
EndFunc   ;==>_GuiCtrlDropMenu_AddMenuItem

Func _GuiCtrlDropMenu_DeleteHeaderMenu($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return False
	If @extended Then _GUICtrlListView_DeleteItem($hWnd, $iItem)
	_ArrayDelete($aData, $iItem)
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate] = $aData
	Return True
EndFunc   ;==>_GuiCtrlDropMenu_DeleteHeaderMenu

Func _GuiCtrlDropMenu_DeleteMenu($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return False
	If @extended Then _GUICtrlListView_DeleteItem($hWnd, $iItem)
	_ArrayDelete($aData, $iItem)
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate] = $aData
	Return True
EndFunc   ;==>_GuiCtrlDropMenu_DeleteMenu

;    0 - Enabled
;    1 - Disabled
Func _GuiCtrlDropMenu_EnableHeaderMenuItem($hMenu, $iItem, $iState = 0, $bByPos = True)
	Return _GuiCtrlDropMenu_SetHeaderItemState($hMenu, $iItem, $DMS_DISABLED, Not $iState, $bByPos)
EndFunc   ;==>_GuiCtrlDropMenu_EnableHeaderMenuItem

Func _GuiCtrlDropMenu_EnableMenuItem($hMenu, $iItem, $iState = 0, $bByPos = True)
	Return _GuiCtrlDropMenu_SetItemState($hMenu, $iItem, $DMS_DISABLED, Not $iState, $bByPos)
EndFunc   ;==>_GuiCtrlDropMenu_EnableMenuItem

Func _GuiCtrlDropMenu_FindHeaderItem($hMenu, $sText, $fInStr = False, $iStart = 0)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return -1
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If _WinAPI_IsWindow($hWnd) Then Return _GUICtrlListView_FindText($hWnd, $sText, $iStart, $fInStr, False)
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	For $iIndex = $iStart To UBound($aData) -1
		If Not $fInStr Then 
			If $sText == DllStructGetData($aData[$iIndex], "Text") Then Return $iIndex
		Else 
			If StringInStr(DllStructGetData($aData[$iIndex], "Text"), $sText) Then Return $iIndex
		EndIf
	Next
	Return -1
EndFunc   ;==>_GuiCtrlDropMenu_FindHeaderItem

Func _GuiCtrlDropMenu_FindItem($hMenu, $sText, $fInStr = False, $iStart = 0)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return -1
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If _WinAPI_IsWindow($hWnd) Then Return _GUICtrlListView_FindText($hWnd, $sText, $iStart, $fInStr, False)
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	For $iIndex = $iStart To UBound($aData) -1
		If Not $fInStr Then 
			If $sText == DllStructGetData($aData[$iIndex], "Text") Then Return $iIndex
		Else 
			If StringInStr(DllStructGetData($aData[$iIndex], "Text"), $sText) Then Return $iIndex
		EndIf
	Next
	Return -1
EndFunc   ;==>_GuiCtrlDropMenu_FindItem

Func _GuiCtrlDropMenu_GetHeaderItemChecked($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return False
	If @extended Then Return _GUICtrlListView_GetItemChecked($hWnd, $iItem)
	Return (BitAND(DllStructGetData($aData[$iItem], "State"), 0xF000) == $DMS_CHECKED)
	
EndFunc   ;==>_GuiCtrlDropMenu_GetHeaderItemChecked

Func _GuiCtrlDropMenu_GetItemChecked($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return False
	If @extended Then Return _GUICtrlListView_GetItemChecked($hWnd, $iItem)
	Return (BitAND(DllStructGetData($aData[$iItem], "State"), 0xF000) == $DMS_CHECKED)
EndFunc   ;==>_GuiCtrlDropMenu_GetItemChecked

Func _GuiCtrlDropMenu_GetHeaderItemCount($hMenu)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If _WinAPI_IsWindow($hWnd) Then Return _GUICtrlListView_GetItemCount($hWnd)
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	Return UBound($aData)
EndFunc   ;==>_GuiCtrlDropMenu_GetHeaderItemCount

Func _GuiCtrlDropMenu_GetItemCount($hMenu)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If _WinAPI_IsWindow($hWnd) Then Return _GUICtrlListView_GetItemCount($hWnd)
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	Return UBound($aData)
EndFunc   ;==>_GuiCtrlDropMenu_GetItemCount

Func _GuiCtrlDropMenu_GetHeaderItemDisabled($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	Local $iState
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return False
	If @extended Then
		$iState = _GUICtrlListView_GetItemState($hWnd, $iItem, 0x000F)
	Else 
		$iState = DllStructGetData($aData[$iItem], "State")
	EndIf
	Return (BitAND($iState, 0x000F) == $DMS_DISABLED)
EndFunc   ;==>_GuiCtrlDropMenu_GetHeaderItemDisabled

Func _GuiCtrlDropMenu_GetItemDisabled($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	Local $iState
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return False
	If @extended Then
		$iState = _GUICtrlListView_GetItemState($hWnd, $iItem, 0x000F)
	Else 
		$iState = DllStructGetData($aData[$iItem], "State")
	EndIf
	Return (BitAND($iState, 0x000F) == $DMS_DISABLED)
EndFunc   ;==>_GuiCtrlDropMenu_GetItemDisabled

Func _GuiCtrlDropMenu_GetHeaderItemEnabled($hMenu, $iItem, $bByPos = True)
	Return Not _GuiCtrlDropMenu_GetHeaderItemDisabled($hMenu, $iItem, $bByPos)
EndFunc   ;==>_GuiCtrlDropMenu_GetHeaderItemEnabled

Func _GuiCtrlDropMenu_GetItemEnabled($hMenu, $iItem, $bByPos = True)
	Return Not _GuiCtrlDropMenu_GetItemDisabled($hMenu, $iItem, $bByPos)
EndFunc   ;==>_GuiCtrlDropMenu_GetItemEnabled

Func _GuiCtrlDropMenu_GetHeaderItemHighlighted($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If Not $bByPos Then $iItem = __DM_GetItemByID($hWnd, $iItem)
	Return _GUICtrlListView_GetItemSelected($hWnd, $iItem)
EndFunc   ;==>_GuiCtrlDropMenu_GetHeaderItemHighlighted

Func _GuiCtrlDropMenu_GetItemHighlighted($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If Not $bByPos Then $iItem = __DM_GetItemByID($hWnd, $iItem)
	Return _GUICtrlListView_GetItemSelected($hWnd, $iItem)
EndFunc   ;==>_GuiCtrlDropMenu_GetItemHighlighted

Func _GuiCtrlDropMenu_GetHeaderItemID($hMenu, $iItem)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return -1
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	Local $lParam
	If _WinAPI_IsWindow($hWnd) Then
		$lParam = _GUICtrlListView_MapIndexToID($hWnd, $iItem)
	Else 
		Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
		If $iItem > 0 Or $iItem > UBound($aData) -1 Then Return -1
		$lParam = DllStructGetData($aData[$iItem], "itemID")
	EndIf
	
	Return _GuiCtrlDropMenu_IsMenu($lParam) ? -1 : $lParam
EndFunc   ;==>_GuiCtrlDropMenu_GetHeaderItemID

Func _GuiCtrlDropMenu_GetItemID($hMenu, $iItem)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return -1
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	Local $lParam
	If _WinAPI_IsWindow($hWnd) Then
		$lParam = _GUICtrlListView_MapIndexToID($hWnd, $iItem)
	Else 
		Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
		If $iItem > 0 Or $iItem > UBound($aData) -1 Then Return -1
		$lParam = DllStructGetData($aData[$iItem], "itemID")
	EndIf
	
	Return _GuiCtrlDropMenu_IsMenu($lParam) ? -1 : $lParam
EndFunc   ;==>_GuiCtrlDropMenu_GetItemID

Func _GuiCtrlDropMenu_GetHeaderItemInfo($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If Not $bByPos Then $iItem = __DM_GetItemByID($hWnd, $iItem)
	Return __DM_GetInfoMenu($hWnd, $iItem)
EndFunc   ;==>_GuiCtrlDropMenu_GetHeaderItemInfo

Func _GuiCtrlDropMenu_GetItemInfo($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If Not $bByPos Then $iItem = __DM_GetItemByID($hWnd, $iItem)
	Return __DM_GetInfoMenu($hWnd, $iItem)
EndFunc   ;==>_GuiCtrlDropMenu_GetItemInfo

Func _GuiCtrlDropMenu_GetHeaderItemRectEx($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If Not $bByPos Then $iItem = __DM_GetItemByID($hWnd, $iItem)
	Return _GUICtrlListView_GetItemRectEx($hWnd, $iItem, $LVIR_BOUNDS)
EndFunc   ;==>_GuiCtrlDropMenu_GetHeaderItemRectEx

Func _GuiCtrlDropMenu_GetItemRectEx($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If Not $bByPos Then $iItem = __DM_GetItemByID($hWnd, $iItem)
	Return _GUICtrlListView_GetItemRectEx($hWnd, $iItem, $LVIR_BOUNDS)
EndFunc   ;==>_GuiCtrlDropMenu_GetItemRectEx

Func _GuiCtrlDropMenu_GetHeaderItemState($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then Return _GUICtrlListView_GetItemState($hWnd, $iItem, $DMT_STATE)
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	Return BitAND(DllStructGetData($aData[$iItem], "State"), $DMT_STATE)
EndFunc   ;==>_GuiCtrlDropMenu_GetHeaderItemState

Func _GuiCtrlDropMenu_GetItemState($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then Return _GUICtrlListView_GetItemState($hWnd, $iItem, $DMT_STATE)
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	Return BitAND(DllStructGetData($aData[$iItem], "State"), $DMT_STATE)
EndFunc   ;==>_GuiCtrlDropMenu_GetItemState

Func _GuiCtrlDropMenu_GetHeaderItemSubMenu($hMenu, $iItem)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	Local $iParam
	If _WinAPI_IsWindow($hWnd) Then 
		$iParam = _GUICtrlListView_GetItemParam($hWnd, $iItem)
	Else 
		Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
		If $iItem < 0 Or $iItem > UBound($aData) -1 Then Return 0
		$iParam = DllStructGetData($aData[$iItem], "itemID")
	EndIf
	
	Return _GuiCtrlDropMenu_IsMenu($iParam) ? Ptr($iParam) : 0
EndFunc   ;==>_GuiCtrlDropMenu_GetHeaderItemSubMenu

Func _GuiCtrlDropMenu_GetItemSubMenu($hMenu, $iItem)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	Local $iParam
	If _WinAPI_IsWindow($hWnd) Then 
		$iParam = _GUICtrlListView_GetItemParam($hWnd, $iItem)
	Else 
		Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
		If $iItem < 0 Or $iItem > UBound($aData) -1 Then Return 0
		$iParam = DllStructGetData($aData[$iItem], "itemID")
	EndIf
	Return _GuiCtrlDropMenu_IsMenu($iParam) ? Ptr($iParam) : 0
EndFunc   ;==>_GuiCtrlDropMenu_GetItemSubMenu

Func _GuiCtrlDropMenu_GetHeaderItemText($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return ''
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return ''
	If @extended Then Return _GUICtrlListView_GetItemText($hWnd, $iItem)
	Return DllStructGetData($aData[$iItem], "Text")
EndFunc   ;==>_GuiCtrlDropMenu_GetHeaderItemText

Func _GuiCtrlDropMenu_GetItemText($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return ''
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return ''
	If @extended Then Return _GUICtrlListView_GetItemText($hWnd, $iItem)
	Return DllStructGetData($aData[$iItem], "Text")
EndFunc   ;==>_GuiCtrlDropMenu_GetItemText

Func _GuiCtrlDropMenu_GetHeaderItemType($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then Return _GUICtrlListView_GetItemState($hWnd, $iItem, $DMT_TYPE)
	Return BitAND(DllStructGetData($aData[$iItem], "State"), $DMT_TYPE)
EndFunc   ;==>_GuiCtrlDropMenu_GetHeaderItemType

Func _GuiCtrlDropMenu_GetItemType($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then Return _GUICtrlListView_GetItemState($hWnd, $iItem, $DMT_TYPE)
	Return BitAND(DllStructGetData($aData[$iItem], "State"), $DMT_TYPE)
EndFunc   ;==>_GuiCtrlDropMenu_GetItemType

Func _GuiCtrlDropMenu_GetMenuHeight($hMenu)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	Return DllStructGetData($tDROPMENUINFO, 'Y')
EndFunc   ;==>_GuiCtrlDropMenu_GetMenuHeight

Func _GuiCtrlDropMenu_GetMenuWidth($hMenu)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	Return DllStructGetData($tDROPMENUINFO, 'X')
EndFunc   ;==>_GuiCtrlDropMenu_GetMenuWidth

Func _GuiCtrlDropMenu_GetMenuInfo($hMenu)
	Return DllStructCreate($tagDROPMENUINFO, $hMenu)
EndFunc   ;==>_GuiCtrlDropMenu_GetMenuInfo

Func _GuiCtrlDropMenu_GetMenuStyle($hMenu)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	Return DllStructGetData($tDROPMENUINFO, 'Style')
EndFunc   ;==>_GuiCtrlDropMenu_GetMenuStyle

Func _GuiCtrlDropMenu_GetMenuData($hMenu)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	Return DllStructGetData($tDROPMENUINFO, 'Data')
EndFunc   ;==>_GuiCtrlDropMenu_GetMenuData

Func _GuiCtrlDropMenu_InsertHeaderMenuItem($hMenu, $iIndex = -1, $sText = '', $iImage = -1, $hSubMenu = 0)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If _WinAPI_IsWindow($hWnd) Then __DM_InsertMenuItem($hWnd, $iIndex, $sText, $iImage, $hSubMenu)
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	Local $iType = 0
	If $iImage > -1 Then $iType = $DMT_BITMAP
	If Not $sText Then $iType = BitOR($DMT_SEPARATOR, $DMS_DISABLED)
	If $iIndex == -1 Then 
		$iIndex = _ArrayAdd($aData, '')
	Else 
		_ArrayInsert($aData, $iIndex)
	EndIf
	$aData[$iIndex] = DllStructCreate($tagDROPMENUITEM)
	DllStructSetData($aData[$iIndex], "Text", $sText)
	DllStructSetData($aData[$iIndex], "itemID", $hSubMenu)
	DllStructSetData($aData[$iIndex], "Image", $iImage)
	DllStructSetData($aData[$iIndex], "State", $iType)
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate] = $aData
	Return $iIndex
EndFunc   ;==>_GuiCtrlDropMenu_InsertHeaderMenuItem

Func _GuiCtrlDropMenu_InsertMenuItem($hMenu, $iIndex = -1, $sText = '', $iImage = -1, $hSubMenu = 0)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If _WinAPI_IsWindow($hWnd) Then __DM_InsertMenuItem($hWnd, $iIndex, $sText, $iImage, $hSubMenu)
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	Local $iType = 0
	If $iImage > -1 Then $iType = $DMT_BITMAP
	If Not $sText Then $iType = BitOR($DMT_SEPARATOR, $DMS_DISABLED)
	If $iIndex == -1 Then 
		$iIndex = _ArrayAdd($aData, '')
	Else 
		_ArrayInsert($aData, $iIndex)
	EndIf
	$aData[$iIndex] = DllStructCreate($tagDROPMENUITEM)
	DllStructSetData($aData[$iIndex], "Text", $sText)
	DllStructSetData($aData[$iIndex], "itemID", $hSubMenu)
	DllStructSetData($aData[$iIndex], "Image", $iImage)
	DllStructSetData($aData[$iIndex], "State", $iType)
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate] = $aData
	Return $iIndex
EndFunc   ;==>_GuiCtrlDropMenu_InsertMenuItem

Func _GuiCtrlDropMenu_MenuItemFromPoint($hMenu, $iX = -1, $iY = -1)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return -1
	Local $tPOINT = _WinAPI_GetMousePos()
	If $iX > -1 Then DllStructSetData($tPOINT, 1, $iX)
	If $iY > -1 Then DllStructSetData($tPOINT, 2, $iY)
	Local $hWnd = _WinAPI_WindowFromPoint($tPOINT)
	If $hWnd == DllStructGetData($tDROPMENUINFO, 'hMenuView') Or $hWnd == DllStructGetData($tDROPMENUINFO, 'hHeaderView') Then
		_WinAPI_ScreenToClient($hWnd, $tPOINT)
		Return _GUICtrlListView_HitTest($hWnd, DllStructGetData($tPOINT, 1), DllStructGetData($tPOINT, 2))[0]
	EndIf
	Return -1
EndFunc   ;==>_GuiCtrlDropMenu_MenuItemFromPoint

Func _GuiCtrlDropMenu_RemoveHeaderMenu($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then _GUICtrlListView_DeleteItem($hWnd, $iItem)
	_ArrayDelete($aData, $iItem)
	Local $Ret = (Not @error)
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate] = $aData
	Return $Ret
EndFunc   ;==>_GuiCtrlDropMenu_RemoveHeaderMenu

Func _GuiCtrlDropMenu_RemoveMenu($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then _GUICtrlListView_DeleteItem($hWnd, $iItem)
	_ArrayDelete($aData, $iItem)
	Local $Ret = (Not @error)
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate] = $aData
	Return $Ret
EndFunc   ;==>_GuiCtrlDropMenu_RemoveMenu

Func _GuiCtrlDropMenu_SetHeaderItemBmp($hMenu, $iItem, $iIndex = -1, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	Local $iCurType
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then
		$iCurType = _GUICtrlListView_GetItemState($hWnd, $iItem, $DMT_TYPE)
		If $iIndex == -1 Then _GUICtrlListView_SetItemState($hWnd, $iItem, BitXOR($iCurType, BitAND($iCurType, $DMT_BITMAP)), $DMT_TYPE)
		If $iIndex > -1 Then _GUICtrlListView_SetItemState($hWnd, $iItem, BitOR($iCurType, $DMT_BITMAP), $DMT_TYPE)
		_GUICtrlListView_SetItemImage($hWnd, $iItem, $iIndex)
	EndIf
	$iCurType = DllStructGetData($aData[$iItem], "State")
	If $iIndex == -1 Then DllStructSetData($aData[$iItem], "State", BitXOR($iCurType, BitAND($iCurType, $DMT_BITMAP)))
	If $iIndex > -1 Then DllStructSetData($aData[$iItem], "State", BitOR($iCurType, $DMT_BITMAP))
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate] = $aData
	Return 1
EndFunc   ;==>_GuiCtrlDropMenu_SetHeaderItemBmp

Func _GuiCtrlDropMenu_SetItemBmp($hMenu, $iItem, $iIndex = -1, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	Local $iCurType
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then
		$iCurType = _GUICtrlListView_GetItemState($hWnd, $iItem, $DMT_TYPE)
		If $iIndex == -1 Then _GUICtrlListView_SetItemState($hWnd, $iItem, BitXOR($iCurType, BitAND($iCurType, $DMT_BITMAP)), $DMT_TYPE)
		If $iIndex > -1 Then _GUICtrlListView_SetItemState($hWnd, $iItem, BitOR($iCurType, $DMT_BITMAP), $DMT_TYPE)
		_GUICtrlListView_SetItemImage($hWnd, $iItem, $iIndex)
	EndIf
	$iCurType = DllStructGetData($aData[$iItem], "State")
	If $iIndex == -1 Then DllStructSetData($aData[$iItem], "State", BitXOR($iCurType, BitAND($iCurType, $DMT_BITMAP)))
	If $iIndex > -1 Then DllStructSetData($aData[$iItem], "State", BitOR($iCurType, $DMT_BITMAP))
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate] = $aData
	Return 1
EndFunc   ;==>_GuiCtrlDropMenu_SetItemBmp

Func _GuiCtrlDropMenu_GetHeaderItemBmp($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return -1
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	If Not $bByPos Then $iItem = __GetItem(0, $aData, $iItem)
	If @error Then Return -1
	If $iItem < 0 Or $iItem > UBound($aData) -1 Then Return -1
	Return DllStructGetData($aData[$iItem], "Image")
EndFunc

Func _GuiCtrlDropMenu_GetItemBmp($hMenu, $iItem, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return -1
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	If Not $bByPos Then $iItem = __GetItem(0, $aData, $iItem)
	If @error Then Return -1
	If $iItem < 0 Or $iItem > UBound($aData) -1 Then Return -1
	Return DllStructGetData($aData[$iItem], "Image")
EndFunc

Func _GuiCtrlDropMenu_SetHeaderItemChecked($hMenu, $iItem, $bState = True, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then
		If _GUICtrlListView_SetItemChecked($hWnd, $iItem, $bState) Then _WinAPI_InvalidateRect($hWnd)
	EndIf
	Local $iCurType = DllStructGetData($aData[$iItem], "State")
	DllStructSetData($aData[$iItem], "State", BitOR(($bState ? $DMS_CHECKED : $DMS_UNCHECKED), BitAND($iCurType, 0x0FFF)))
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate] = $aData
	Return 1
EndFunc   ;==>_GuiCtrlDropMenu_SetHeaderItemChecked

Func _GuiCtrlDropMenu_SetItemChecked($hMenu, $iItem, $bState = True, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then
		If _GUICtrlListView_SetItemChecked($hWnd, $iItem, $bState) Then _WinAPI_InvalidateRect($hWnd)
	EndIf
	Local $iCurType = DllStructGetData($aData[$iItem], "State")
	DllStructSetData($aData[$iItem], "State", BitOR(($bState ? $DMS_CHECKED : $DMS_UNCHECKED), BitAND($iCurType, 0x0FFF)))
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate] = $aData
	Return 1
EndFunc   ;==>_GuiCtrlDropMenu_SetItemChecked

Func _GuiCtrlDropMenu_SetHeaderItemSubMenu($hMenu, $iItem, $hSubMenu, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then _GUICtrlListView_SetItemParam($hWnd, $iItem, $hSubMenu)
	DllStructSetData($aData[$iItem], "itemID", $hSubMenu)
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate] = $aData
	Return 1
EndFunc   ;==>_GuiCtrlDropMenu_SetHeaderItemSubMenu

Func _GuiCtrlDropMenu_SetItemSubMenu($hMenu, $iItem, $hSubMenu, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then _GUICtrlListView_SetItemParam($hWnd, $iItem, $hSubMenu)
	DllStructSetData($aData[$iItem], "itemID", $hSubMenu)
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate] = $aData
	Return 1
EndFunc   ;==>_GuiCtrlDropMenu_SetItemSubMenu

Func _GuiCtrlDropMenu_SetHeaderItemText($hMenu, $iItem, $sText, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	Local $iType
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then
		$iType = _GUICtrlListView_GetItemState($hWnd, $iItem, $DMT_TYPE)
		If $sText Then $iType = BitXOR($iType, BitAND($iType, $DMT_SEPARATOR))
		If Not $sText Then $iType = $DMT_SEPARATOR
		_GUICtrlListView_SetItemState($hWnd, $iItem, $iType, $DMT_TYPE)
		_GUICtrlListView_SetItemText($hWnd, $iItem, $sText)
	EndIf
	$iType = DllStructGetData($aData[$iItem], "State")
	If $sText Then $iType = BitXOR($iType, BitAND($iType, $DMT_SEPARATOR))
	If Not $sText Then $iType = $DMT_SEPARATOR
	DllStructSetData($aData[$iItem], "State", $iType)
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate] = $aData
	Return 1
EndFunc   ;==>_GuiCtrlDropMenu_SetHeaderItemText

Func _GuiCtrlDropMenu_SetItemText($hMenu, $iItem, $sText, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	Local $iType
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then
		$iType = _GUICtrlListView_GetItemState($hWnd, $iItem, $DMT_TYPE)
		If $sText Then $iType = BitXOR($iType, BitAND($iType, $DMT_SEPARATOR))
		If Not $sText Then $iType = $DMT_SEPARATOR
		_GUICtrlListView_SetItemState($hWnd, $iItem, $iType, $DMT_TYPE)
		_GUICtrlListView_SetItemText($hWnd, $iItem, $sText)
	EndIf
	$iType = DllStructGetData($aData[$iItem], "State")
	If $sText Then $iType = BitXOR($iType, BitAND($iType, $DMT_SEPARATOR))
	If Not $sText Then $iType = $DMT_SEPARATOR
	DllStructSetData($aData[$iItem], "State", $iType)
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate] = $aData
	Return 1
EndFunc   ;==>_GuiCtrlDropMenu_SetItemText

Func _GuiCtrlDropMenu_SetHeaderItemType($hMenu, $iItem, $iType, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then _GUICtrlListView_SetItemState($hWnd, $iItem, $iType, $DMT_TYPE)
	DllStructSetData($aData[$iItem], "State", $iType)
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate] = $aData
	Return 1
EndFunc   ;==>_GuiCtrlDropMenu_SetHeaderItemType

Func _GuiCtrlDropMenu_SetItemType($hMenu, $iItem, $iType, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then _GUICtrlListView_SetItemState($hWnd, $iItem, $iType, $DMT_TYPE)
	DllStructSetData($aData[$iItem], "State", $iType)
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate] = $aData
	Return 1
EndFunc   ;==>_GuiCtrlDropMenu_SetItemType

Func _GuiCtrlDropMenu_SetMenuData($hMenu, $iData)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	DllStructSetData($tDROPMENUINFO, 'Data', $iData)
	Return Not @error
EndFunc   ;==>_GuiCtrlDropMenu_SetMenuData

Func _GuiCtrlDropMenu_SetMenuHeight($hMenu, $iHeight)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If _WinAPI_IsWindow($hWnd) Then _SendMessage($hWnd, $DMM_SETHEIGHTMENU, 0, $iHeight)
	DllStructSetData($tDROPMENUINFO, 'Y', $iHeight)
EndFunc   ;==>_GuiCtrlDropMenu_SetMenuHeight

Func _GuiCtrlDropMenu_SetMenuWidth($hMenu, $iWidth)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	If _WinAPI_IsWindow($hWnd) Then _SendMessage($hWnd, $DMM_SETWIDTHMENU, 0, $iWidth)
	DllStructSetData($tDROPMENUINFO, 'X', $iWidth)
EndFunc   ;==>_GuiCtrlDropMenu_SetMenuWidth

Func _GuiCtrlDropMenu_SetMenuInfo($hMenu, ByRef $tInfo)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	For $i = 1 To 10
		DllStructSetData($tDROPMENUINFO, $i, DllStructGetData($tInfo, $i))
	Next
EndFunc   ;==>_GuiCtrlDropMenu_SetMenuInfo

Func _GuiCtrlDropMenu_SetMenuStyle($hMenu, $iStyle)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	DllStructSetData($tDROPMENUINFO, 'Style', $iStyle)
	If _WinAPI_IsWindow(DllStructGetData($tDROPMENUINFO, 'hDropDown')) Then _SendMessage(DllStructGetData($tDROPMENUINFO, 'hDropDown'), $DMM_INITMENU, DllStructGetData($tDROPMENUINFO, 'hHeaderView'), 0, 0, 'hwnd')
EndFunc   ;==>_GuiCtrlDropMenu_SetMenuStyle

Func _GuiCtrlDropMenu_Redraw($hMenu)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	If _WinAPI_IsWindowVisible(DllStructGetData($tDROPMENUINFO, 'hDropDown')) Then
		_WinAPI_RedrawWindow(DllStructGetData($tDROPMENUINFO, 'hDropDown'), 0, 0, BitOR($RDW_INVALIDATE, $RDW_ALLCHILDREN))
		Return 1
	EndIf
	Return 0
EndFunc   ;==>_GuiCtrlDropMenu_Redraw

Func _GuiCtrlDropMenu_SetHeaderItemState($hMenu, $iItem, $iState, $fState = True, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
	Local $iCurState
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then
		$iCurState = _GUICtrlListView_GetItemState($hWnd, $iItem, $DMT_STATE)
		If $fState Then
			_GUICtrlListView_SetItemState($hWnd, $iItem, $iState, $DMT_STATE)
		Else
			_GUICtrlListView_SetItemState($hWnd, $iItem, BitXOR($iCurState, BitAND($iCurState, $iState)), $DMT_STATE)
		EndIf
	EndIf
	If $fState Then DllStructSetData($aData[$iItem], "State", BitOR(BitOR(DllStructGetData($aData[$iItem], "State"), 0x0F00), BitAND($iState, $DMT_STATE)))
	$iCurState = BitAND(DllStructGetData($aData[$iItem], "State"), $DMT_STATE)
	If Not $fState Then DllStructSetData($aData[$iItem], "State", BitOR(BitOR(DllStructGetData($aData[$iItem], "State"), 0x0F00), BitXOR($iCurState, BitAND($iCurState, $iState)))))
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate] = $aData
	Return 1
EndFunc   ;==>_GuiCtrlDropMenu_SetHeaderItemState

Func _GuiCtrlDropMenu_SetItemState($hMenu, $iItem, $iState, $fState = True, $bByPos = True)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
	Local $hWnd = DllStructGetData($tDROPMENUINFO, 'hMenuView')
	Local $iCurState
	If Not $bByPos Then $iItem = __GetItem($hWnd, $aData, $iItem)
	If @error Then Return 0
	If @extended Then
		$iCurState = _GUICtrlListView_GetItemState($hWnd, $iItem, $DMT_STATE)
		If $fState Then
			_GUICtrlListView_SetItemState($hWnd, $iItem, $iState, $DMT_STATE)
		Else
			_GUICtrlListView_SetItemState($hWnd, $iItem, BitXOR($iCurState, BitAND($iCurState, $iState)), $DMT_STATE)
		EndIf
	EndIf
	If $fState Then DllStructSetData($aData[$iItem], "State", BitOR(BitAND(DllStructGetData($aData[$iItem], "State"), 0x0F00), BitAND($iState, $DMT_STATE)))
	$iCurState = BitAND(DllStructGetData($aData[$iItem], "State"), $DMT_STATE)
	If Not $fState Then DllStructSetData($aData[$iItem], "State", BitOR(BitAND(DllStructGetData($aData[$iItem], "State"), 0x0F00), BitXOR($iCurState, BitAND($iCurState, $iState))))
	$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate] = $aData
	Return 1
EndFunc   ;==>_GuiCtrlDropMenu_SetItemState

Func _GuiCtrlDropMenu_DestroyMenu($hMenu)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If @error Then Return 0
	If DllStructGetData($tDROPMENUINFO, 'hImage') Then _GUIImageList_Destroy(DllStructGetData($tDROPMENUINFO, 'hImage'))
	_ArrayDelete($_a__DropDownData, DllStructGetData($tDROPMENUINFO, 'Index'))
	For $iIndex = 0 To UBound($_a__DropDownData) -1
		$tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $_a__DropDownData[$iIndex][0])
		DllStructSetData($tDROPMENUINFO, 'Index', $iIndex)
	Next
EndFunc   ;==>_GuiCtrlDropMenu_DestroyMenu

Func _GuiCtrlDropMenu_FindParent($hMenu)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	Return DllStructGetData($tDROPMENUINFO, 'hParent')
EndFunc   ;==>_GuiCtrlDropMenu_FindParent

Func _GuiCtrlDropMenu_IsMenu($hMenu)
	If Not $hMenu Then Return 0
	For $i = 0 To UBound($_a__DropDownData) - 1
		If DllStructGetPtr($_a__DropDownData[$i][0]) == Ptr($hMenu) Then Return 1
	Next
	Return 0
EndFunc   ;==>_GuiCtrlDropMenu_IsMenu

Func _GuiCtrlDropMenu_SetFont($hMenu, $iSize, $iWeight = Default, $iAttribute = Default, $sFontname = Default, $iQuality = Default)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	Local $tFont = __BinaryToStructure(DllStructGetData($tDROPMENUINFO, "Font"), $tagLOGFONTW)
	DllStructSetData($tFont, "Height", __SizeToHeight($iSize))
	If $iWeight <> Default Then DllStructSetData($tFont, "Weight", $iWeight)
	If $iAttribute <> Default And $iAttribute <> 0 Then
		DllStructSetData($tFont, "Italic", BitAND($iAttribute, 2) / 2)
		DllStructSetData($tFont, "Underline", BitAND($iAttribute, 4) / 4)
		DllStructSetData($tFont, "Strikeout", BitAND($iAttribute, 8) / 8)
	EndIf
	If $sFontname <> Default Then DllStructSetData($tFont, "FaceName", $sFontname)
	If $iQuality <> Default Then DllStructSetData($tFont, "Quality", $iQuality)
	
	DllStructSetData($tDROPMENUINFO, "Font", __StructureToBinary($tFont, $tagLOGFONTW))
EndFunc

#EndRegion Functions

#Region Process Functions
Func _PopupProc($hWnd, $iMsg, $wParam, $lParam)
	Local Static $dpi
	Switch $iMsg
		Case $WM_CREATE
			$dpi = _WinAPI_GetDpiForPrimaryMonitor() / 96
			
			Local $tCREATESTRUCT = DllStructCreate($tagCREATESTRUCT, $lParam)
			Local $pInfo = Ptr(DllStructGetData($tCREATESTRUCT, 'lpCreateParams'))
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $pInfo)
			_WinAPI_SetWindowLong($hWnd, $GWL_USERDATA, $pInfo)
			_WinAPI_DeleteObject(_WinAPI_SetClassLongEx($hWnd, $GCL_HBRBACKGROUND, _WinAPI_CreateSolidBrush(_WinAPI_GetSysColor($COLOR_WINDOW))))
			DllStructSetData($tDROPMENUINFO, 'hPopUp', $hWnd)
			Local $hHeaderListView = _GUICtrlListView_Create($hWnd, '', 0, 0, 0, 0, BitOR(0x56006405, $LVS_SHAREIMAGELISTS)) ;0x56006405
			_GUICtrlListView_InsertColumn($hHeaderListView, 0, '', 1)
			_GUICtrlListView_EnableGroupView($hHeaderListView)
			_GUICtrlListView_InsertGroup($hHeaderListView, -1, 1, '')
			_GUICtrlListView_SetGroupInfo($hHeaderListView, 1, '', 0, BitOR($LVGS_NOHEADER, $LVGS_HIDDEN))
			
			;GUICtrlSetFont(_WinAPI_GetDlgCtrlID($hHeaderListView), 8.5 * $dpi2 * $dpi)
			
			Local $hListView = _GUICtrlListView_Create($hWnd, '', 0, 0, 0, 0, BitOR(0x56204405, $LVS_SHAREIMAGELISTS), $LVS_EX_CHECKBOXES) ;0x56204405
			_GUICtrlListView_InsertColumn($hListView, 0, '', 1)
			_GUICtrlListView_EnableGroupView($hListView)
			_GUICtrlListView_InsertGroup($hListView, -1, 1, '')
			_GUICtrlListView_SetGroupInfo($hListView, 1, '', 0, BitOR($LVGS_NOHEADER, $LVGS_HIDDEN))
			;GUICtrlSetFont(_WinAPI_GetDlgCtrlID($hListView), 8.5 * $dpi2 * $dpi)
			
			If BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_TRANSPARENTBACKGROUND) Or BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_BLURBACKGROUND) Then 
				_GUICtrlListView_SetBkColor($hHeaderListView, 0)
				_GUICtrlListView_SetBkColor($hListView, 0)
			EndIf
			
			DllStructSetData($tDROPMENUINFO, 'hHeaderView', $hHeaderListView)
			DllStructSetData($tDROPMENUINFO, 'hMenuView', $hListView)
			
			$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderMenuInfo] = DllStructCreate($tagLVINFOMENU)
			$_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuInfo] = DllStructCreate($tagLVINFOMENU)
			
			_WinAPI_SetWindowLong($hHeaderListView, $GWL_USERDATA, DllStructGetPtr($_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderMenuInfo]))
			_WinAPI_SetWindowLong($hListView, $GWL_USERDATA, DllStructGetPtr($_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuInfo]))
			
			Local $hDC = _WinAPI_GetWindowDC($hWnd)
			Local $hTheme = _WinAPI_OpenThemeData(0, 'Menu')
			Local $tSIZE = _WinAPI_GetThemePartSize($hTheme, 11, 0, $hDC, 0, $TS_DRAW)
			
			DllStructSetData($_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderMenuInfo], 'SizeCheck', _WinAPI_MakeLong(DllStructGetData($tSIZE, 1), DllStructGetData($tSIZE, 2)))
			DllStructSetData($_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuInfo], 'SizeCheck', _WinAPI_MakeLong(DllStructGetData($tSIZE, 1), DllStructGetData($tSIZE, 2)))
			
			$tSIZE = _WinAPI_GetThemePartSize($hTheme, 16, 0, $hDC, 0, $TS_DRAW)
			_WinAPI_CloseThemeData($hTheme)
			_WinAPI_ReleaseDC($hWnd, $hDC)
			
			DllStructSetData($_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderMenuInfo], 'hMenu', $pInfo)
			DllStructSetData($_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuInfo], 'hMenu', $pInfo)
			
			DllStructSetData($_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderMenuInfo], 'SizeSubMenu', _WinAPI_MakeLong(DllStructGetData($tSIZE, 1), DllStructGetData($tSIZE, 2)))
			DllStructSetData($_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuInfo], 'SizeSubMenu', _WinAPI_MakeLong(DllStructGetData($tSIZE, 1), DllStructGetData($tSIZE, 2)))
			
			DllStructSetData($_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderMenuInfo], 'hNext', $hListView)
			DllStructSetData($_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuInfo], 'hNext', $hHeaderListView)
			
			Local $aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_HeaderCreate]
			For $i = 0 To UBound($aData) -1
				__DM_AddMenuItem($hHeaderListView, DllStructGetData($aData[$i], "Text"), DllStructGetData($aData[$i], "Image"), DllStructGetData($aData[$i], "itemID"))
				If DllStructGetData($aData[$i], "State") Then _GUICtrlListView_SetItemState($hHeaderListView, $i, DllStructGetData($aData[$i], "State"), 0xFFFF)
			Next
			$aData = $_a__DropDownData[DllStructGetData($tDROPMENUINFO, 'Index')][$id_MenuCreate]
			For $i = 0 To UBound($aData) -1
				__DM_AddMenuItem($hListView, DllStructGetData($aData[$i], "Text"), DllStructGetData($aData[$i], "Image"), DllStructGetData($aData[$i], "itemID"))
				If DllStructGetData($aData[$i], "State") Then _GUICtrlListView_SetItemState($hListView, $i, DllStructGetData($aData[$i], "State"), 0xFFFF)
				;_GUICtrlListView_SetItemChecked($hListView, $i, False)
				;ConsoleWrite('ITEM ' & DllStructGetData($aData[$i], "Text") & ' 0x' & Hex(_GUICtrlListView_GetItemState($hListView, $i, $DMT_TYPE), 4) & @CR)
			Next
		Case $WM_DESTROY
			Local $hMenu = __DM_GetID($hWnd)
			If @error Then Return 0
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
			Local $hHeaderListView = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
			Local $hListView = DllStructGetData($tDROPMENUINFO, 'hMenuView')
			_GUICtrlListView_Destroy($hHeaderListView)
			_GUICtrlListView_Destroy($hListView)
			DllStructSetData($tDROPMENUINFO, 'hHeaderView', 0)
			DllStructSetData($tDROPMENUINFO, 'hMenuView', 0)
		Case $WM_MOUSEACTIVATE
			Return 1
		Case $WM_NOTIFY
			Local $tNMHDR, $hWndFrom, $iCode
			$tNMHDR = DllStructCreate($tagNMHDR, $lParam)
			$hWndFrom = HWnd(DllStructGetData($tNMHDR, "hWndFrom"))
			$iCode = DllStructGetData($tNMHDR, "Code")
			Local $tLVINFOMENU = DllStructCreate($tagLVINFOMENU, __DM_GetID($hWndFrom))
			Local Static $iHotItem = -1, $iKeyActivate = 0
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, __DM_GetID($hWnd))
			Local $hHeaderListView = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
			Local $hListView = DllStructGetData($tDROPMENUINFO, 'hMenuView')
			Switch $iCode
				Case $NM_CLICK
					ContinueCase
				Case $LVN_ITEMACTIVATE
					If Not DllStructGetData($tDROPMENUINFO, 'Init') Then Return 1
					Local $tInfo = DllStructCreate($tagNMITEMACTIVATE, $lParam)
					Local $iIndex = DllStructGetData($tInfo, "Index")
					Local $iState = _GUICtrlListView_GetItemState($hWndFrom, $iIndex, 0xffff)
					If $iIndex == -1 Or BitAND($iState, $DMS_DISABLED) Or BitAND($iState, $DMT_SEPARATOR) Then Return 1
					Local $iParam = _GUICtrlListView_GetItemParam($hWndFrom, $iIndex)
					If _GuiCtrlDropMenu_IsMenu($iParam) Then
						If Not _WinAPI_IsWindowVisible(DllStructGetData(DllStructCreate($tagDROPMENUINFO, $iParam), 'hDropDown')) Then __DM_SubMenuActivate($hWndFrom, $iIndex, 1)
						Return 1
					EndIf
					If $hWndFrom == DllStructGetData($tDROPMENUINFO, 'hMenuView') Then $iIndex += _GUICtrlListView_GetItemCount($hHeaderListView)
					_SendMessage(DllStructGetData($tDROPMENUINFO, 'hDropDown'), $DMM_MENUSELECT, _WinAPI_MakeLong($iIndex, $iParam), DllStructGetData($tLVINFOMENU, "hMenu"), 0, 'int', 'ptr')
					
				Case $NM_KILLFOCUS
					If DllStructGetData($tDROPMENUINFO, 'hSubMenu') Then Return $GUI_RUNDEFMSG
					;ConsoleWrite('-$NM_KILLFOCUS ' & ($hWndFrom == $hHeaderListView ? 'HEADER' : $hWndFrom == $hListView ? 'MENU' : '') & @TAB & Ptr(__DM_GetID($hWnd)) & @TAB & DllStructGetData($tDROPMENUINFO, 'hSubMenu') & @CR)
					_GUICtrlListView_SetItemFocused($hWndFrom, __DM_GetSelectItem($hWndFrom), False)
					_GUICtrlListView_SetItemSelected($hWndFrom, __DM_GetSelectItem($hWndFrom), False, False)
					Local $hWndFocus = _WinAPI_GetFocus()
					If $hWndFocus == $hHeaderListView Or $hWndFocus == $hListView Then Return 0
					
					Return 0
				Case $NM_SETFOCUS
					;ConsoleWrite('+$NM_SETFOCUS ' & ($hWndFrom == $hHeaderListView ? 'HEADER' : $hWndFrom == $hListView ? 'MENU' : '') & @TAB & Ptr(__DM_GetID($hWnd)) & @CR)
					$iHotItem = -1
				Case $LVN_HOTTRACK
					If Not DllStructGetData($tDROPMENUINFO, 'Init') Then Return 1
					
					Local $tInfo = DllStructCreate($tagNMLISTVIEW, $lParam)
					$iHotItem = _GUICtrlListView_HitTest($hWndFrom, DllStructGetData($tInfo, "ActionX"), DllStructGetData($tInfo, "ActionY"))[0]
					Local $iState = _GUICtrlListView_GetItemState($hWndFrom, $iHotItem, 0xFFFF)
					
					If BitAND($iState, $LVNI_SELECTED) == $LVNI_SELECTED Then Return 0
					
					If _WinAPI_GetFocus() <> $hWndFrom Then _WinAPI_SetFocus($hWndFrom)
					_GUICtrlListView_SetItemSelected($hWndFrom, $iHotItem, True, True)
					
					Return 0
				Case $LVN_ITEMCHANGING
					If Not DllStructGetData($tDROPMENUINFO, 'Init') Then Return $GUI_RUNDEFMSG
					Local $iFlag = 0
					Local $tInfo = DllStructCreate($tagNMLISTVIEW, $lParam)
					Local $iIndex = DllStructGetData($tInfo, "Item")
					
					If Not (BitAND(DllStructGetData($tInfo, "NewState"), $LVNI_SELECTED) == $LVNI_SELECTED And Not BitAND(DllStructGetData($tInfo, "OldState"), $LVNI_SELECTED)) Then Return $GUI_RUNDEFMSG
					
					Local $iState = _GUICtrlListView_GetItemState($hWndFrom, $iIndex, 0xFFFF)
					
					If $iKeyActivate And (BitAND($iState, $DMT_SEPARATOR) == $DMT_SEPARATOR Or BitAND($iState, $DMS_DISABLED) == $DMS_DISABLED) Then
						Switch $iKeyActivate
							Case $VK_UP
								$iIndex -= 1
							Case $VK_DOWN
								$iIndex += 1
						EndSwitch
						
						Switch $iIndex
							Case -1
								$hWndFrom = DllStructGetData($tLVINFOMENU, "hNext")
								$iIndex = _GUICtrlListView_GetItemCount($hWndFrom) - 1
							Case _GUICtrlListView_GetItemCount($hWndFrom)
								$hWndFrom = DllStructGetData($tLVINFOMENU, "hNext")
								$iIndex = 0
						EndSwitch
						
						If _WinAPI_GetFocus() <> $hWndFrom Then _WinAPI_SetFocus($hWndFrom)
						_GUICtrlListView_SetItemSelected($hWndFrom, $iIndex, True, True)
						Return 1
					EndIf
					$iKeyActivate = 0
					
					Local $bIsMenu = _GuiCtrlDropMenu_IsMenu(_GUICtrlListView_GetItemParam($hWndFrom, $iIndex))
					If $bIsMenu Then __DM_SubMenuActivate($hWndFrom, $iHotItem)
					
					If Not DllStructGetData($tDROPMENUINFO, 'Notify') Then Return $GUI_RUNDEFMSG
					
					If $bIsMenu Then $iFlag = BitOR($iFlag, $MF_POPUP)
					If $iHotItem == DllStructGetData($tInfo, "Item") Then $iFlag = BitOR($iFlag, $MF_MOUSESELECT)
					If BitAND($iState, $DMT_SEPARATOR) == $DMT_SEPARATOR Then $iFlag = BitOR($iFlag, $MF_SEPARATOR)
					If BitAND($iState, $DMT_BITMAP) == $DMT_BITMAP Then $iFlag = BitOR($iFlag, $MF_BITMAP)
					If BitAND($iState, $DMS_DISABLED) == $DMS_DISABLED Then $iFlag = BitOR($iFlag, $MF_DISABLED)
					If Not BitAND($iState, $DMS_DISABLED) Then $iFlag = BitOR($iFlag, $MF_HILITE)
					If BitAND($iState, $DMS_CHECKED) == $DMS_CHECKED Then $iFlag = BitOR($iFlag, $MF_CHECKED)
					If $hWndFrom == DllStructGetData($tDROPMENUINFO, 'hMenuView') Then $iIndex += _GUICtrlListView_GetItemCount(DllStructGetData($tDROPMENUINFO, 'hHeaderView'))
					Local $LoParam = DllStructGetData($tInfo, "Param") ? $bIsMenu ? $iIndex : DllStructGetData($tInfo, "Param") : $iIndex
					_WinAPI_PostMessage(DllStructGetData($tDROPMENUINFO, 'hParent'), $WM_MENUSELECT, _WinAPI_MakeLong($LoParam, $iFlag), __DM_GetID($hWnd))
					$iHotItem = -1
					
				Case $LVN_KEYDOWN
					Local $tInfo = DllStructCreate($tagNMLVKEYDOWN, $lParam)
					Local Static $iSelItem
					$iKeyActivate = DllStructGetData($tInfo, "VKey")
					$iSelItem = __DM_GetSelectItem($hWndFrom)
					Switch DllStructGetData($tInfo, "VKey")
						Case $VK_UP
							If $iSelItem == 0 Then
								If _GUICtrlListView_GetItemCount(DllStructGetData($tLVINFOMENU, "hNext")) Then $hWndFrom = DllStructGetData($tLVINFOMENU, "hNext")
								If _WinAPI_GetFocus() <> $hWndFrom Then _WinAPI_SetFocus($hWndFrom)
								_GUICtrlListView_SetItemSelected($hWndFrom, _GUICtrlListView_GetItemCount($hWndFrom) - 1, True, True)
								Return 1
							EndIf
						Case $VK_DOWN
							If $iSelItem == _GUICtrlListView_GetItemCount($hWndFrom) - 1 Then
								If _GUICtrlListView_GetItemCount(DllStructGetData($tLVINFOMENU, "hNext")) Then $hWndFrom = DllStructGetData($tLVINFOMENU, "hNext")
								If _WinAPI_GetFocus() <> $hWndFrom Then _WinAPI_SetFocus($hWndFrom)
								_GUICtrlListView_SetItemSelected($hWndFrom, 0, True, True)
								Return 1
							EndIf
						Case $VK_LEFT
							$iKeyActivate = 0
							Local $hParent = _WinAPI_GetParent(DllStructGetData($tDROPMENUINFO, 'hDropDown'))
							If $hParent <> DllStructGetData($tDROPMENUINFO, 'hParent') Then _WinAPI_SetFocus($hParent)
						Case $VK_RIGHT
							Local $hMenu = _GUICtrlListView_GetItemParam($hWndFrom, $iSelItem)
							If _GuiCtrlDropMenu_IsMenu($hMenu) Then
								$iKeyActivate = 0
								$tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
								Select
									Case _GUICtrlListView_GetItemCount(DllStructGetData($tDROPMENUINFO, 'hHeaderView'))
										_GUICtrlListView_SetItemSelected(DllStructGetData($tDROPMENUINFO, 'hHeaderView'), 0, True, True)
									Case _GUICtrlListView_GetItemCount(DllStructGetData($tDROPMENUINFO, 'hMenuView'))
										_GUICtrlListView_SetItemSelected(DllStructGetData($tDROPMENUINFO, 'hMenuView'), 0, True, True)
								EndSelect
								
								__DM_SubMenuActivate($hWndFrom, $iSelItem, 1)
							EndIf
						Case $VK_ESCAPE
							_WinAPI_ShowWindow(DllStructGetData($tDROPMENUINFO, 'hDropDown'), @SW_HIDE)
					EndSwitch
				Case $NM_RETURN
					_GUICtrlListView_ClickItem($hWndFrom, __DM_GetSelectItem($hWndFrom))
					
				Case -165
				Case Else
					;ConsoleWrite('NOTIFY MSG ' & $iCode & @CR)
			EndSwitch
			;ConsoleWrite('$WM_NOTIFY ' & $iCode & @CR)
			Return $GUI_RUNDEFMSG
		Case $WM_DRAWITEM
			Local $DRAWITEMSTRUCT = DllStructCreate($tagDRAWITEM, $lParam)
			If DllStructGetData($DRAWITEMSTRUCT, "CtlType") <> $ODT_LISTVIEW Then Return $GUI_RUNDEFMSG
			Local $CtlID = DllStructGetData($DRAWITEMSTRUCT, "CtlID")
			Local $itemID = DllStructGetData($DRAWITEMSTRUCT, "itemID")
			Local $itmAction = DllStructGetData($DRAWITEMSTRUCT, "itemAction")
			Local $hWndFrom = DllStructGetData($DRAWITEMSTRUCT, "hwndItem")
			Local $iState = _GUICtrlListView_GetItemState($hWndFrom, $itemID, 0xffff) ;DllStructGetData($DRAWITEMSTRUCT, "itemState")
			Local $hDC = DllStructGetData($DRAWITEMSTRUCT, "hDC")
			Local $itemData = DllStructGetData($DRAWITEMSTRUCT, "itemData")
			Local $tLVINFOMENU = DllStructCreate($tagLVINFOMENU, __DM_GetID($hWndFrom))
			Local $tInfo = DllStructCreate($tagDROPMENUINFO, DllStructGetData($tLVINFOMENU, 'hMenu'))
			
			Switch $itmAction
				Case $ODA_DRAWENTIRE
					Local $iCount = _GUICtrlListView_GetItemCount($hWndFrom)
					Local $iFix = ($itemID == $iCount - 1 ? 6 : 1) * $dpi
					Local $sItemText = _GUICtrlListView_GetItemText($hWndFrom, $itemID)
					Local $tRECT = DllStructCreate($tagRECT, DllStructGetPtr($DRAWITEMSTRUCT, "Left"))
					DllStructSetData($tRECT, 'Left', DllStructGetData($tRECT, "Left") * $dpi)
					DllStructSetData($tRECT, 'Right', DllStructGetData($tRECT, "Right") * $dpi)
					Local $iWidth = DllStructGetData($tRECT, 'Right') - DllStructGetData($tRECT, "Left")
					Local $iHeight = DllStructGetData($tRECT, 'Bottom') - DllStructGetData($tRECT, 'Top')
					
					Local $iY = DllStructGetData($tRECT, 'Top')
					DllStructSetData($tRECT, 'Top', 0)
					DllStructSetData($tRECT, 'Bottom', $iHeight)
					Local $hDestDC = _WinAPI_CreateCompatibleDC($hDC)
					Local $hDIB = _WinAPI_CreateDIB($iWidth, -$iHeight)
					Local $hDestSv = _WinAPI_SelectObject($hDestDC, $hDIB)
					
					Local $tRECT1 = _WinAPI_CreateRect(0, 0, $iWidth, $iHeight + $iFix)
					Local $tRECT2 = _WinAPI_CreateRect(2, 0, $iWidth - 4, $iHeight)
					Local $tRECT3 = __DM_GetItemTextRect($tLVINFOMENU, $tRECT, $dpi)
					
					If BitAND(DllStructGetData($tInfo, 'Style'), $DMNS_TRANSPARENTBACKGROUND) Or BitAND(DllStructGetData($tInfo, 'Style'), $DMNS_BLURBACKGROUND) Then 
						_WinAPI_PatBlt($hDestDC, 0, 0, $iWidth, $iHeight + $iFix, $BLACKNESS)
						_WinAPI_SetBkMode($hDestDC, $TRANSPARENT)
					Else 
						_SendMessage($hWndFrom, $WM_ERASEBKGND, $hDestDC)
					EndIf
					
					;_WinAPI_SetTextColor($hDestDC, 0xFF00FF)
					
					Local $hFont = _SendMessage($hWndFrom, $WM_GETFONT)
					_WinAPI_SelectObject($hDestDC, $hFont)
					
					Local $iStateID = 1
					Select
						Case BitAND($iState, $DMS_DISABLED) And BitAND($iState, $DMS_SELECTED)
							$iStateID = 4
						Case BitAND($iState, $DMS_DISABLED)
							$iStateID = 3
						Case BitAND($iState, $DMS_SELECTED)
							$iStateID = 2
					EndSelect
					;ConsoleWrite('INDEX ' & $itemID & @TAB & 'STATE ' & $iStateID & @CR)
					Local $iImage = _GUICtrlListView_GetItemImage($hWndFrom, $itemID)
					Local $hTheme = _WinAPI_OpenThemeData(0, 'Menu')
					
					If BitAND($iState, $DMT_SEPARATOR) Then
						DllStructSetData($tRECT2, 'Top', DllStructGetData($tRECT2, "Bottom") - (DllStructGetData($tRECT2, "Bottom") - DllStructGetData($tRECT2, "Top")) / 2 - 2)
						_WinAPI_DrawThemeBackground($hTheme, 15, 0, $hDestDC, $tRECT2)
					Else
						_WinAPI_DrawThemeBackground($hTheme, 14, $iStateID, $hDestDC, $tRECT2)
						Local $tDTTOPTS = DllStructCreate($tagDTTOPTS)
						DllStructSetData($tDTTOPTS, 'Size', DllStructGetSize($tDTTOPTS))
						DllStructSetData($tDTTOPTS, 'Flags', $DTT_COMPOSITED)
						_WinAPI_DrawThemeTextEx($hTheme, 14, $iStateID, $hDestDC, $sItemText, $tRECT3, BitOR(BitAND($iState, $DMT_RIGHTJUSTIFY) ? BitOR($DT_RTLREADING, $DT_RIGHT) : $DT_LEFT, $DT_VCENTER, $DT_SINGLELINE, $DT_WORD_ELLIPSIS), $tDTTOPTS)
						
						If _GuiCtrlDropMenu_IsMenu($itemData) Then
							_WinAPI_DrawThemeBackground($hTheme, 16, BitAND($iState, $DMS_DISABLED) ? 2 : 1, $hDestDC, __DM_GetItemRightImageRect($tLVINFOMENU, $tRECT))
						EndIf
						
						If Not BitAND(DllStructGetData($tInfo, 'Style'), $DMNS_NOCHECK) Then
							If BitAND($iState, 0xF000) Then
								If BitAND($iState, $DMS_CHECKED) Then _WinAPI_DrawThemeBackground($hTheme, 11, BitAND($iState, $DMT_RADIOCHECK) ? BitAND($iState, $DMS_DISABLED) ? 4 : 3 : BitAND($iState, $DMS_DISABLED) ? 2 : 1, $hDestDC, __DM_GetItemLeftImageRect($tLVINFOMENU, $tRECT))
							Else
								If BitAND($iState, $DMT_BITMAP) Then
									Local $hImage = _GUICtrlListView_GetImageList($hWndFrom, 1)
									Local $H = _GUIImageList_GetIconHeight($hImage)
									_GUIImageList_Draw($hImage, $iImage, $hDestDC, DllStructGetData($tRECT1, "Left") + 8 * $dpi, ($iHeight - $H) / 2, 1)
								EndIf
							EndIf
						EndIf
						
					EndIf
					
					_WinAPI_CloseThemeData($hTheme)
					
					_WinAPI_BitBlt($hDC, 0, $iY, $iWidth, $iHeight, $hDestDC, 0, 0, $SRCCOPY)
					_WinAPI_SelectObject($hDestDC, $hDestSv)
					_WinAPI_DeleteDC($hDestDC)
					_WinAPI_DeleteObject($hDIB)
					
					Return 1
			EndSwitch
		Case $WM_NCHITTEST, $WM_SETCURSOR
			Return 1
		Case $WM_WINDOWPOSCHANGED
			Local $hMenu = __DM_GetID($hWnd)
			If @error Then Return 0
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
			Local $tPOS = DllStructCreate($tagWINDOWPOS, $lParam)
			If Not DllStructGetData($tPOS, 'CX') Or Not DllStructGetData($tPOS, 'CY') Then Return 0
			_WinAPI_SetWindowPos(DllStructGetData($tDROPMENUINFO, 'hDropDown'), $HWND_TOPMOST, 0, 0, DllStructGetData($tPOS, 'CX') + 2 * $dpi, DllStructGetData($tPOS, 'CY') + 2 * $dpi, BitOR($SWP_NOOWNERZORDER, $SWP_NOMOVE, $SWP_NOACTIVATE))
			Return 0
		Case $WM_ERASEBKGND
			Local $hMenu = __DM_GetID($hWnd)
			If @error Then Return _WinAPI_DefWindowProcW($hWnd, $iMsg, $wParam, $lParam)
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
			If Not BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_TRANSPARENTBACKGROUND) And Not BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_BLURBACKGROUND) Then 
				Return _WinAPI_DefWindowProcW($hWnd, $iMsg, $wParam, $lParam)
			EndIf
			Local $tRECT = _WinAPI_GetClientRect($hWnd)
			_WinAPI_PatBlt($wParam, 0, 0, DllStructGetData($tRECT, 'Right'), DllStructGetData($tRECT, 'Bottom'), $BLACKNESS) ;$BLACKNESS
			Return 1
		Case $DMM_SETITEMHEIGHT
			Local $hMenu = __DM_GetID($hWnd)
			If @error Then Return 0
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
			Local $hHeaderListView = DllStructGetData($tDROPMENUINFO, 'hHeaderView')
			Local $hListView = DllStructGetData($tDROPMENUINFO, 'hMenuView')
			Local $dpi2 = 96 / _WinAPI_GetDpiForSystem()
			Local $tFont = _WinAPI_GetThemeSysFont(0, $TMT_MENUFONT)
			DllStructSetData($tFont, 'Height', __SizeToHeight(($wParam / 2.03) * $dpi * $dpi2))
			_WinAPI_SetFont($hHeaderListView, _WinAPI_CreateFontIndirect($tFont))
			_WinAPI_SetFont($hListView, _WinAPI_CreateFontIndirect($tFont))
			
			Local $hImage = _GUIImageList_Create(16 * $dpi, 16 * $dpi, 5, 1)
			_GUICtrlListView_SetImageList($hHeaderListView, $hImage, 1)
			_GUICtrlListView_SetImageList($hListView, $hImage, 1)
			
			_GUICtrlListView_SetImageList($hHeaderListView, 0)
			_GUICtrlListView_SetImageList($hListView, 0)
			
			_GUIImageList_Destroy($hImage)
			
			Local $tFont = __BinaryToStructure(DllStructGetData($tDROPMENUINFO, "Font"), $tagLOGFONTW)
			Local $iHeightFont = __HeightToSize(DllStructGetData($tFont, "Height")) * $dpi * $dpi2
			DllStructSetData($tFont, "Height", __SizeToHeight($iHeightFont))
			
			_WinAPI_SetFont($hHeaderListView, _WinAPI_CreateFontIndirect($tFont))
			_WinAPI_SetFont($hListView, _WinAPI_CreateFontIndirect($tFont))
			
			If DllStructGetData($tDROPMENUINFO, 'hImage') Then 
				_GUICtrlListView_SetImageList($hHeaderListView, DllStructGetData($tDROPMENUINFO, 'hImage'), 1)
				_GUICtrlListView_SetImageList($hListView, DllStructGetData($tDROPMENUINFO, 'hImage'), 1)
			EndIf
			Return 0
	EndSwitch
	Return _WinAPI_DefWindowProcW($hWnd, $iMsg, $wParam, $lParam)
EndFunc   ;==>_PopupProc

Func _DropProc($hWnd, $iMsg, $wParam, $lParam)
	#forceref $hWnd, $iMsg, $wParam
	Local Static $iSelectItem, $iSelectItemParam, $hMenuActivate
	Local Static $dpi
	Switch $iMsg
		Case $WM_CREATE
			$dpi = _WinAPI_GetDpiForPrimaryMonitor() / 96
			Local $tCREATESTRUCT = DllStructCreate($tagCREATESTRUCT, $lParam)
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, Ptr(DllStructGetData($tCREATESTRUCT, 'lpCreateParams')))
			
			DllStructSetData($tDROPMENUINFO, 'hDropDown', $hWnd)
			DllStructSetData($tDROPMENUINFO, 'hParent', _WinAPI_GetParent($hWnd))
			DllStructSetData($tDROPMENUINFO, 'Level', __GetLevel(DllStructGetData($tDROPMENUINFO, 'hParent')) + 1)
			
			_WinAPI_SetWindowLong($hWnd, $GWL_USERDATA, DllStructGetData($tCREATESTRUCT, 'lpCreateParams'))
			_WinAPI_CreateWindowEx(0, 'ListviewPopup', '', 0x52000000, 0, 0, 0, 0, $hWnd, 0, $h__instance_gui, DllStructGetData($tCREATESTRUCT, 'lpCreateParams'))
			
			_WinAPI_DeleteObject(_WinAPI_SetClassLongEx($hWnd, $GCL_HBRBACKGROUND, _WinAPI_CreateSolidBrush(_WinAPI_GetSysColor($COLOR_WINDOW))))
			
			Select
				Case BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_ROUNDFRAME)
					_WinAPI_DwmSetWindowAttribute($hWnd, $DWMWA_WINDOW_CORNER_PREFERENCE, $DWMWCP_ROUND)
					_WinAPI_DwmSetWindowAttribute($hWnd, $DWMWA_BORDER_COLOR, $DWMWA_COLOR_NONE)
				Case BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_ROUNDSMALLFRAME)
					_WinAPI_DwmSetWindowAttribute($hWnd, $DWMWA_WINDOW_CORNER_PREFERENCE, $DWMWCP_ROUNDSMALL)
					_WinAPI_DwmSetWindowAttribute($hWnd, $DWMWA_BORDER_COLOR, $DWMWA_COLOR_NONE)
			EndSelect
			
			_SendMessage(DllStructGetData($tDROPMENUINFO, 'hPopUp'), $DMM_SETITEMHEIGHT, DllStructGetData($tDROPMENUINFO, 'ItemHeight'))
			If BitAND(DllStructGetData($tCREATESTRUCT, 'Style'), $WS_CAPTION) Then _WinAPI_DwmExtendFrameIntoClientArea($hWnd, _WinAPI_CreateMargins(0, 0, 1, 0))
			Return 0
		Case $WM_CLOSE
			Local $hMenu = __DM_GetID($hWnd)
			If @error Then Return 0
			_WinAPI_ShowWindow($hWnd, @SW_HIDE)
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
			_WinAPI_SetWindowLong($hWnd, $GWL_USERDATA, 0)
			Local $hParent = _WinAPI_GetParent($hWnd)
			If $hParent <> DllStructGetData($tDROPMENUINFO, 'hwndParent') Then DllStructSetData(DllStructCreate($tagDROPMENUINFO, __DM_GetID($hParent)), 'hSubMenu', 0)
			
			; Уведомление окна, содержащего меню
			
			If $hParent == DllStructGetData($tDROPMENUINFO, 'hwndParent') Then
				If DllStructGetData($tDROPMENUINFO, 'Notify') Then _SendMessage(DllStructGetData($tDROPMENUINFO, 'hwndParent'), $WM_UNINITMENUPOPUP, $hMenu, 0)
				_SendMessage(DllStructGetData($tDROPMENUINFO, 'hwndParent'), $WM_MENUSELECT, _WinAPI_MakeLong(0, 0xFFFF), 0)
				If $iSelectItem > -1 And DllStructGetData($tDROPMENUINFO, 'Notify') And Not BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_NOTIFYBYPOS) Then _SendMessage(DllStructGetData($tDROPMENUINFO, 'hwndParent'), $WM_COMMAND, ($iSelectItemParam ? $iSelectItemParam : $iSelectItem), 0)
				If $iSelectItem > -1 And DllStructGetData($tDROPMENUINFO, 'Notify') And BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_NOTIFYBYPOS) Then _SendMessage(DllStructGetData($tDROPMENUINFO, 'hwndParent'), $WM_MENUCOMMAND, $iSelectItem, $hMenuActivate)
				DllStructSetData($tDROPMENUINFO, 'SelectMenu', $iSelectItem)
			EndIf
			
			_WinAPI_DestroyWindow(DllStructGetData($tDROPMENUINFO, 'hPopUp'))
			DllStructSetData($tDROPMENUINFO, 'hPopUp', 0)
			DllStructSetData($tDROPMENUINFO, 'hDropDown', 0)
			DllStructSetData($tDROPMENUINFO, 'Init', 0)
			DllStructSetData($tDROPMENUINFO, 'Level', 0)
			
			_WinAPI_DestroyWindow($hWnd)
			
			If $hParent <> DllStructGetData($tDROPMENUINFO, 'hwndParent') Then _SendMessage($hParent, $WM_CLOSE)
			
		Case $WM_DESTROY
		Case $WM_ACTIVATEAPP
			Return 0
		Case $WM_MOUSEACTIVATE
			Return 1
		Case $WM_SHOWWINDOW
			Local $hMenu = __DM_GetID($hWnd)
			If @error Then Return 0
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
			DllStructSetData($tDROPMENUINFO, 'Init', $wParam)
			Switch $wParam
				Case True
					Select
						Case BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_TRANSPARENTBACKGROUND)
							__DM_SetBlur($hWnd, BitOR(BitAND(0xFF000000, _WinAPI_DwmGetColorizationColor()), _WinAPI_SwitchColor(_WinAPI_GetSysColor($COLOR_MENU))), $BACKDROP_TRANSPARENT)
						Case BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_BLURBACKGROUND)
							__DM_SetBlur($hWnd, BitOR(BitAND(0xFF000000, _WinAPI_DwmGetColorizationColor()), _WinAPI_SwitchColor(_WinAPI_GetSysColor($COLOR_MENU))), $BACKDROP_MODERN_ACRYLIC_BLUR)
					EndSelect
					
					DllStructSetData($tDROPMENUINFO, 'SelectMenu', 0)
					$iSelectItem = -1
					$iSelectItemParam = 0
					$hMenuActivate = 0
				Case False
					
			EndSwitch
			Return 0
		Case $WM_ACTIVATE
			Local $hMenu = __DM_GetID($hWnd)
			If @error Then Return 0
			
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
			Local $HwndFrom = HWnd($lParam)
			Switch _WinAPI_LoWord($wParam)
				Case True
					If Not @Compiled Then ConsoleWrite('+ Активировалось окно [' & _WinAPI_GetClassName($hWnd) & '] [' & _WinAPI_GetWindowText($hWnd) & '] и деактивировалось [' & _WinAPI_GetClassName($HwndFrom) & '] [' & _WinAPI_GetWindowText($HwndFrom) & ']' & @CR)
					_WinAPI_DefWindowProcW($HwndFrom, $WM_NCACTIVATE, True, 0)
					Return 0
				Case False
					If Not @Compiled Then ConsoleWrite('- Деактивировалось окно [' & _WinAPI_GetClassName($hWnd) & '] [' & _WinAPI_GetWindowText($hWnd) & '] и активировалось [' & _WinAPI_GetClassName($HwndFrom) & '] [' & _WinAPI_GetWindowText($HwndFrom) & ']' & @CR)
					Local $hParent = _WinAPI_GetParent($hWnd)
					Switch $HwndFrom
						Case DllStructGetData($tDROPMENUINFO, 'hwndParent')
							ContinueCase
						Case 0
							If Not $HwndFrom Then _WinAPI_DefWindowProcW(DllStructGetData($tDROPMENUINFO, 'hwndParent'), $WM_NCACTIVATE, False, 0)
							_SendMessage($hWnd, $WM_CLOSE)
						Case DllStructGetData(DllStructCreate($tagDROPMENUINFO, Ptr(DllStructGetData($tDROPMENUINFO, 'hSubMenu'))), 'hDropDown') ; Если активируется дочернее окно SubMenu
						Case Else
							_SendMessage($hWnd, $WM_CLOSE)
					EndSwitch
					Return 0
			EndSwitch
		Case $DMM_MENUSELECT
			Local $hMenu = __DM_GetID($hWnd)
			If @error Then Return 0
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
			$iSelectItem = _WinAPI_LoWord($wParam)
			$iSelectItemParam = _WinAPI_HiWord($wParam)
			$hMenuActivate = $lParam
			_WinAPI_SetFocus(DllStructGetData($tDROPMENUINFO, 'hwndParent'))
		Case $WM_NCCALCSIZE
			If Not $wParam Then Return False
			_WinAPI_SetWindowLong($hWnd, 0, 0)
			Return True
		Case $WM_NCPAINT
			;Local $hDC = _WinAPI_GetDCEx($hWnd, $wParam, BitOR($DCX_WINDOW, $DCX_INTERSECTRGN))
			
			;_WinAPI_ReleaseDC($hWnd, $hDC)
			;Return $GUI_RUNDEFMSG
		Case $WM_SETFOCUS
			Local $hMenu = __DM_GetID($hWnd)
			If @error Then Return 0
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
			;ConsoleWrite('$WM_SETFOCUS ' & Ptr(__DM_GetID($hWnd)) & @CR)
			Select
				Case __DM_GetSelectItem(DllStructGetData($tDROPMENUINFO, 'hMenuView')) > -1
					_WinAPI_SetFocus(DllStructGetData($tDROPMENUINFO, 'hMenuView'))
				Case Else
					_WinAPI_SetFocus(DllStructGetData($tDROPMENUINFO, 'hHeaderView'))
			EndSelect
			Return 0
		Case $WM_NCHITTEST, $WM_SETCURSOR
			Return 1
		Case $WM_SIZE
			Local $hMenu = __DM_GetID($hWnd)
			If @error Then Return 0
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
			If _WinAPI_LoWord($lParam) Then DllStructSetData($tDROPMENUINFO, 'X', _WinAPI_LoWord($lParam))
			If _WinAPI_HiWord($lParam) Then DllStructSetData($tDROPMENUINFO, 'Y', _WinAPI_HiWord($lParam))
			;ConsoleWrite('$WM_SIZE ' & $hWnd & @TAB & DllStructGetData($tDROPMENUINFO, 'X') & '/' & DllStructGetData($tDROPMENUINFO, 'Y') & @CR)
			Return 0
		Case $WM_ERASEBKGND
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, __DM_GetID($hWnd))
			If Not BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_TRANSPARENTBACKGROUND) And Not BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_BLURBACKGROUND) Then 
				Return _WinAPI_DefWindowProcW($hWnd, $iMsg, $wParam, $lParam)
			EndIf
			Local $tRECT = _WinAPI_GetClientRect($hWnd)
			_WinAPI_PatBlt($wParam, 0, 0, DllStructGetData($tRECT, 'Right'), DllStructGetData($tRECT, 'Bottom'), $BLACKNESS) ;$BLACKNESS
			Return 1
		Case $DMM_INITMENU
			Local $hMenu = __DM_GetID($hWnd)
			If @error Then Return 0
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
			Local $HwndFrom = HWnd($wParam)
			
			Local $tRECT = DllStructCreate($tagRECT)
			Local $iCount = 0, $iWidth = 0, $iHeight = 0, $iMinHeight = 0, $iSize = 0
			Local $iHotItem
			Switch $HwndFrom
				Case DllStructGetData($tDROPMENUINFO, 'hHeaderView')
					_GUIScrollBars_ShowScrollBar($HwndFrom, $SB_VERT, False)
					_SendMessage($hWnd, $DMM_SETAUTOSIZE, $HwndFrom, 0, 0, 'hwnd')
					Local $iCount = _SendMessage($hWnd, $DMM_GETVIEWRECT, $HwndFrom, $tRECT, 0, 'hwnd', 'struct*')
					_WinAPI_MoveWindow($HwndFrom, 1 * $dpi, 2 * $dpi, DllStructGetData($tRECT, 'Right'), DllStructGetData($tRECT, 'Bottom'))
					$iSize = _WinAPI_MakeLong(DllStructGetData($tRECT, 'Right'), DllStructGetData($tRECT, 'Bottom'))
					$HwndFrom = DllStructGetData($tDROPMENUINFO, 'hMenuView')
					ContinueCase
				Case DllStructGetData($tDROPMENUINFO, 'hMenuView')
					
					_SendMessage($hWnd, $DMM_SETAUTOSIZE, $HwndFrom, $iSize, 0, 'hwnd')
					
					If DllStructGetData($tDROPMENUINFO, 'Y') Then
						$iHeight = DllStructGetData($tDROPMENUINFO, 'Y') - _WinAPI_HiWord($iSize) - 6
						$iMinHeight = _SendMessage($hWnd, $DMM_GETMINHEIGHT, $HwndFrom, 0, 0, 'hwnd')
						If $iHeight < $iMinHeight Then $iHeight = $iMinHeight
						$iCount = 0
					Else
						$iCount = $__dm_maxcount - $iCount
						If $iCount < $__dm_mincount Then $iCount = $__dm_mincount
					EndIf
					
					Local $tSCROLLINFO = _GUIScrollBars_GetScrollInfoEx($HwndFrom, $SB_VERT)
					If DllStructGetData($tSCROLLINFO, "nPos") Then _GUICtrlListView_EnsureVisible($HwndFrom, 0)
					
					DllStructSetData($tRECT, 'Left', $iCount)
					DllStructSetData($tRECT, 'Top', $iHeight)
					
					Local $iRet = _SendMessage($hWnd, $DMM_GETVIEWRECT, $HwndFrom, $tRECT, 0, 'hwnd', 'struct*')
					DllStructSetData($tRECT, 'Bottom', DllStructGetData($tRECT, 'Bottom') + 4 * $dpi) ; При меньшем значении активируется скролл бар
					
					_GUICtrlListView_BeginUpdate($HwndFrom)
					_WinAPI_MoveWindow($HwndFrom, 0, 0, DllStructGetData($tRECT, 'Right'), DllStructGetData($tRECT, 'Bottom'))
					_GUICtrlListView_EndUpdate($HwndFrom)
					
					DllStructSetData($tRECT, 'Right', DllStructGetData($tRECT, 'Right') + _SendMessage($hWnd, $DMM_GETWIDTHSCROLLBAR, $HwndFrom, 0, 0, 'hwnd'))
					
					If DllStructGetData($tRECT, 'Right') > _WinAPI_LoWord($iSize) Then
						_GUICtrlListView_SetColumnWidth(DllStructGetData($tDROPMENUINFO, 'hHeaderView'), 0, DllStructGetData($tRECT, 'Right'))
						_WinAPI_SetWindowPos(DllStructGetData($tDROPMENUINFO, 'hHeaderView'), $HWND_TOP, 1 * $dpi, 2 * $dpi, DllStructGetData($tRECT, 'Right'), _WinAPI_HiWord($iSize), BitOR($SWP_NOMOVE, $SWP_NOACTIVATE, $SWP_NOREDRAW, $SWP_NOSENDCHANGING))
					Else
						_GUICtrlListView_SetColumnWidth($HwndFrom, 0, _WinAPI_LoWord($iSize) - _SendMessage($hWnd, $DMM_GETWIDTHSCROLLBAR, $HwndFrom, 0, 0, 'hwnd'))
						DllStructSetData($tRECT, 'Right', _WinAPI_LoWord($iSize))
					EndIf
					
					_WinAPI_MoveWindow($HwndFrom, 1 * $dpi, 2 * $dpi + _WinAPI_HiWord($iSize), DllStructGetData($tRECT, 'Right'), DllStructGetData($tRECT, 'Bottom'))
					_WinAPI_SetWindowPos(DllStructGetData($tDROPMENUINFO, 'hPopUp'), $HWND_TOP, 1 * $dpi, 1 * $dpi, DllStructGetData($tRECT, 'Right') + 2 * $dpi, 4 * $dpi + _WinAPI_HiWord($iSize) + DllStructGetData($tRECT, 'Bottom'), $SWP_NOACTIVATE)
			EndSwitch
			
			If _WinAPI_GetParent($hWnd) == DllStructGetData($tDROPMENUINFO, 'hwndParent') Then 
				If DllStructGetData($tDROPMENUINFO, 'Notify') Then _SendMessage(DllStructGetData($tDROPMENUINFO, 'hwndParent'), $WM_INITMENU, __DM_GetID($hWnd))
				If DllStructGetData($tDROPMENUINFO, 'Notify') Then _SendMessage(DllStructGetData($tDROPMENUINFO, 'hwndParent'), $WM_INITMENUPOPUP, __DM_GetID($hWnd))
			EndIf
			
			Return 0
		Case $DMM_SETAUTOSIZE
			Local $hMenu = __DM_GetID($hWnd)
			If @error Then Return 0
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
			Local $HwndFrom = HWnd($wParam)
			Local $tLVINFOMENU = DllStructCreate($tagLVINFOMENU, __DM_GetID($HwndFrom))
			If @error Then Return 0
			
			If DllStructGetData($tDROPMENUINFO, 'X') Then
				If DllStructGetData($tDROPMENUINFO, 'X') > $__dm_maxwidth + 4 Then DllStructSetData($tDROPMENUINFO, 'X', $__dm_maxwidth + 4)
				_GUICtrlListView_SetColumnWidth($HwndFrom, 0, (DllStructGetData($tDROPMENUINFO, 'X') - 4) * $dpi)
			Else
				
				_GUICtrlListView_SetColumnWidth($HwndFrom, 0, $LVSCW_AUTOSIZE)
				Local $tRECT = _GUICtrlListView_GetItemRectEx($HwndFrom, 0, $LVIR_ICON)
				_GUICtrlListView_SetColumnWidth($HwndFrom, 0, _GUICtrlListView_GetColumnWidth($HwndFrom, 0) - DllStructGetData($tRECT, 'Right') + DllStructGetData($tRECT, 'Left') + 8 * $dpi)
				
				If BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_NOCHECK) Then
					If (_WinAPI_LoWord($lParam) * $dpi < _GUICtrlListView_GetColumnWidth($HwndFrom, 0) + _WinAPI_LoWord(DllStructGetData($tLVINFOMENU, 'SizeSubMenu'))) And __DM_ExistsSubMenu($HwndFrom) Then
						_GUICtrlListView_SetColumnWidth($HwndFrom, 0, _GUICtrlListView_GetColumnWidth($HwndFrom, 0) + _WinAPI_LoWord(DllStructGetData($tLVINFOMENU, 'SizeSubMenu')))
					EndIf
				Else
					_GUICtrlListView_SetColumnWidth($HwndFrom, 0, _GUICtrlListView_GetColumnWidth($HwndFrom, 0) + _WinAPI_LoWord(DllStructGetData($tLVINFOMENU, 'SizeCheck')) * 2 + DllStructGetData($tRECT, 'Right'))
				EndIf
				
				If _GUICtrlListView_GetColumnWidth($HwndFrom, 0) > $__dm_maxwidth * $dpi Then _GUICtrlListView_SetColumnWidth($HwndFrom, 0, $__dm_maxwidth * $dpi)
				If _GUICtrlListView_GetColumnWidth($HwndFrom, 0) < _WinAPI_LoWord($lParam) * $dpi Then _GUICtrlListView_SetColumnWidth($HwndFrom, 0, _WinAPI_LoWord($lParam) * $dpi)
				
			EndIf
			Return 1
		Case $DMM_GETVIEWRECT
			$wParam = HWnd($wParam)
			; расчитывает кол-во пунктов для высоты
			; или высоту для количества пунктов
			Local $tRECTW = DllStructCreate($tagRECT, $lParam)
			Local $iMaxCount = DllStructGetData($tRECTW, 'Left')
			Local $iMaxHeight = DllStructGetData($tRECTW, 'Top')
			Local $iItemCount = _GUICtrlListView_GetItemCount($wParam)
			If Not $iItemCount Then Return 0
			Local $tRECT = DllStructCreate($tagRECT)
			Local $iPrewHeight = 0, $iFix = 0
			For $iIndex = 0 To $iItemCount - 1
				$tRECT = _WinAPI_UnionRect($tRECT, _GUICtrlListView_GetItemRectEx($wParam, $iIndex, $LVIR_BOUNDS))
				If $iIndex == 0 And DllStructGetData($tRECT, 'Top') < 0 Then $iFix = -DllStructGetData($tRECT, 'Top')
				If $iMaxHeight And (DllStructGetData($tRECT, 'Bottom') + $iFix > $iMaxHeight) Then
					DllStructSetData($tRECT, 'Bottom', $iPrewHeight)
					ExitLoop
				EndIf
				If $iMaxCount And ($iIndex == $iMaxCount - 1) Then ExitLoop
				If $iIndex == $iItemCount - 1 Then ExitLoop
				$iPrewHeight = DllStructGetData($tRECT, 'Bottom')
			Next
			
			_WinAPI_OffsetRect($tRECT, 0, $iFix)
			
			For $i = 1 To 4
				DllStructSetData($tRECTW, $i, DllStructGetData($tRECT, $i))
			Next
			Return $iIndex + 1
		Case $DMM_GETWIDTHSCROLLBAR
			$wParam = HWnd($wParam)
			Local $tSCROLLBARINFO = _GUIScrollBars_GetScrollBarInfoEx($wParam, $OBJID_VSCROLL)
			Return DllStructGetData($tSCROLLBARINFO, 'Right') - DllStructGetData($tSCROLLBARINFO, 'Left')
		Case $DMM_GETMINHEIGHT
			$wParam = HWnd($wParam)
			Local $tRECT = _GUICtrlListView_GetItemRectEx($wParam, 0, $LVIR_BOUNDS)
			Return (DllStructGetData($tRECT, 'Bottom') + 1) * $__dm_mincount - 1
		Case $DMM_SETWIDTHMENU
			Local $hMenu = __DM_GetID($hWnd)
			If @error Then Return 0
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
			DllStructSetData($tDROPMENUINFO, 'X', $lParam)
			If Not _WinAPI_IsWindowVisible(DllStructGetData($tDROPMENUINFO, 'hDropDown')) Then Return 1
			_SendMessage($hWnd, $DMM_INITMENU, DllStructGetData($tDROPMENUINFO, 'hHeaderView'), 0, 0, 'hwnd')
			Return 1
		Case $DMM_SETHEIGHTMENU
			Local $hMenu = __DM_GetID($hWnd)
			If @error Then Return 0
			Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
			DllStructSetData($tDROPMENUINFO, 'Y', $lParam)
			If Not _WinAPI_IsWindowVisible(DllStructGetData($tDROPMENUINFO, 'hDropDown')) Then Return 1
			_SendMessage($hWnd, $DMM_INITMENU, DllStructGetData($tDROPMENUINFO, 'hHeaderView'), 0, 0, 'hwnd')
			Return 1
		Case $DMM_GETINFOMENU
			Local $tInfo = DllStructCreate($tagLVINFOMENU, $lParam)
			For $iIndex = 1 To 13
				DllStructSetData($tInfo, $iIndex, DllStructGetData($tLVINFOMENU, $iIndex))
			Next
			Return 1
	EndSwitch
	
	Return _WinAPI_DefWindowProcW($hWnd, $iMsg, $wParam, $lParam)
EndFunc   ;==>_DropProc

#EndRegion Process Functions

#Region Internal Fonctions
Func __DM_DrawBackground($hDC, $tRECT)
	Local $iWidth = DllStructGetData($tRECT, 'Right') - DllStructGetData($tRECT, 'Left')
	Local $iHeight = DllStructGetData($tRECT, 'Bottom') - DllStructGetData($tRECT, 'Top')
	Local $iY = DllStructGetData($tRECT, 'Top')
	DllStructSetData($tRECT, 'Top', 0)
	DllStructSetData($tRECT, 'Bottom', $iHeight)
	Local $hDestDC = _WinAPI_CreateCompatibleDC($hDC)
	
	Local $hBitmap = _WinAPI_CreateCompatibleBitmapEx($hDC, $iWidth, $iHeight, _WinAPI_SwitchColor(_WinAPI_GetSysColor($COLOR_MENU)))
	Local $hDestSv = _WinAPI_SelectObject($hDestDC, $hBitmap)
	
	Local $hSource = _WinAPI_CreateCompatibleBitmapEx($hDC, $iWidth, $iHeight, _WinAPI_SwitchColor(_WinAPI_GetSysColor($COLOR_MENU)))
	Local $hSrcDC = _WinAPI_CreateCompatibleDC($hDC)
	Local $hSrcSv = _WinAPI_SelectObject($hSrcDC, $hSource)
	Local $hTheme = _WinAPI_OpenThemeData(0, 'Menu')
	_WinAPI_DrawThemeBackground($hTheme, 9, 0, $hSrcDC, $tRECT)
	_WinAPI_CloseThemeData($hTheme)
	_WinAPI_AlphaBlend($hDestDC, 0, 0, $iWidth, $iHeight, $hSrcDC, 0, 0, $iWidth, $iHeight, $DM_ALPHABK, 0)
	_WinAPI_SelectObject($hDestDC, $hDestSv)
	_WinAPI_DeleteDC($hDestDC)
	_WinAPI_SelectObject($hSrcDC, $hSrcSv)
	_WinAPI_DeleteDC($hSrcDC)
	_WinAPI_DrawBitmap($hDC, 0, $iY, $hBitmap, $SRCCOPY) ;$MERGECOPY $SRCCOPY
	_WinAPI_DeleteObject($hSource)
	_WinAPI_DeleteObject($hBitmap)
EndFunc   ;==>__DM_DrawBackground

Func __ShowDrop($_h_Wnd = 0)
	Local Static $hWnd
	If IsDeclared('_h_Wnd') Then 
		$hWnd = $_h_Wnd
		Return AdlibRegister(__ShowDrop, 50)
	EndIf
	AdlibUnRegister(__ShowDrop)
	
	_WinAPI_ShowWindow($hWnd)
	_WinAPI_RedrawWindow($hWnd, 0, 0, BitOR($RDW_INVALIDATE, $RDW_ALLCHILDREN))
EndFunc

Func __DM_SetBlur($hWnd, $iColor, $effectType)
	;ConsoleWrite('COLOR 0x' & Hex($iColor, 8) & @CR)
	SetWindowBackdrop($hWnd, 1, $iColor, $effectType)
EndFunc   ;==>__DM_SetBlur

Func __GetLevel($hWnd)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, __DM_GetID($hWnd))
	If @error Then Return 0
	Return DllStructGetData($tDROPMENUINFO, 'Level')
EndFunc

Func __DM_WindowFromPoint($hWnd, $tPOINT)
	Local $hListView = __DM_GetParam($hWnd, $id_hHeaderListView)
	If @error Then Return 0
	If _WinAPI_PtInRect(_WinAPI_GetWindowRect($hListView), $tPOINT) Then Return $hListView
	$hListView = __DM_GetParam($hWnd, $id_hMenuListView)
	If @error Then Return 0
	If _WinAPI_PtInRect(_WinAPI_GetWindowRect($hListView), $tPOINT) Then Return $hListView
	Return 0
EndFunc   ;==>__DM_WindowFromPoint

Func __DM_CreateBitmap($hWnd, $hDC, $iPosY, $iWidth, $iHeight, $bState = True)
	Local $hDestDC = _WinAPI_CreateCompatibleDC($hDC)
	Local $hBitmap = _WinAPI_CreateCompatibleBitmapEx($hDC, $iWidth, $iHeight - $iPosY, _WinAPI_SwitchColor(_WinAPI_GetSysColor($COLOR_MENU)))
	Local $hDestSv = _WinAPI_SelectObject($hDestDC, $hBitmap)
	Local $hSource = _WinAPI_CreateCompatibleBitmapEx($hDC, $iWidth, $iHeight - $iPosY, _WinAPI_SwitchColor(_WinAPI_GetSysColor($COLOR_MENU)))
	Local $hSrcDC = _WinAPI_CreateCompatibleDC($hDC)
	Local $hSrcSv = _WinAPI_SelectObject($hSrcDC, $hSource)
	Local $hTheme = _WinAPI_OpenThemeData(0, 'Menu')
	;Local $hMenuBand = _WinAPI_OpenThemeData(0, 'LVPopup::Toolbar')
	Local $tRECT = _WinAPI_CreateRectEx(0, 0, $iWidth, $iHeight)
	_WinAPI_DrawThemeBackground($hTheme, 9, 0, $hSrcDC, $tRECT)
	;_WinAPI_CloseThemeData($hTheme)
	_WinAPI_AlphaBlend($hDestDC, 0, 0, $iWidth, $iHeight - $iPosY, $hSrcDC, 0, 0, $iWidth, $iHeight - $iPosY, $DM_ALPHABK, True)
	
	;$hTheme = _WinAPI_OpenThemeData(0, 'Communications::Menu')
	_WinAPI_SetBkMode($hDestDC, $TRANSPARENT)
	
	Local $hFont = _SendMessage($hWnd, $WM_GETFONT)
	_WinAPI_SelectObject($hDestDC, $hFont)
	Local $iImage, $sText, $iStateID, $iItemState
	Local $hImage = _GUICtrlListView_GetImageList($hWnd, 1)
	Local $tLVINFOMENU = DllStructCreate($tagLVINFOMENU, __DM_GetID($hWnd))
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, DllStructGetData($tLVINFOMENU, 'hMenu'))
	Local $tRECTA
	;ConsoleWrite('++++++++++++++CREATE BITMAP START+++++++++++++++' & @CR)
	
	For $iIndex = 0 To _GUICtrlListView_GetItemCount($hWnd) - 1
		$tRECT = _GUICtrlListView_GetItemRectEx($hWnd, $iIndex, $LVIR_BOUNDS)
		If DllStructGetData($tRECT, 'Bottom') < 0 Or DllStructGetData($tRECT, 'Bottom') < $iPosY Or DllStructGetData($tRECT, 'Top') > $iHeight Then ContinueLoop
		If $iPosY Then _WinAPI_OffsetRect($tRECT, 0, -$iPosY)
		
		;ConsoleWrite('INDEX ' & $iIndex & @CR)
		; Прямоугольник фонового изображения
		$tRECTA = _WinAPI_CreateRect(DllStructGetData($tRECT, "Left") + 2, DllStructGetData($tRECT, "Top"), DllStructGetData($tRECT, "Right") - 4, DllStructGetData($tRECT, "Bottom"))
		
		$sText = _GUICtrlListView_GetItemText($hWnd, $iIndex)
		$iStateID = __DM_GetThemeState($hWnd, $iIndex, $bState)
		$iItemState = _GUICtrlListView_GetItemState($hWnd, $iIndex, 0xFFFF)
		
		If BitAND($iItemState, $DMT_SEPARATOR) Then ; Разделитель
			DllStructSetData($tRECTA, 'Top', DllStructGetData($tRECTA, "Bottom") - (DllStructGetData($tRECTA, "Bottom") - DllStructGetData($tRECTA, "Top")) / 2 - 5)
			_WinAPI_DrawThemeBackground($hTheme, 15, 0, $hDestDC, $tRECTA)
			
		Else
			_WinAPI_DrawThemeBackground($hTheme, 14, $iStateID, $hDestDC, $tRECTA)
			
			; Прямоугольник текста
			$tRECTA = __DM_GetItemTextRect($tLVINFOMENU, $tRECT)
			_WinAPI_DrawThemeText($hTheme, 14, $iStateID, $hDestDC, _GUICtrlListView_GetItemText($hWnd, $iIndex), $tRECTA, BitOR(BitAND($iItemState, $DMT_RIGHTJUSTIFY) ? $DT_RIGHT : $DT_LEFT, $DT_VCENTER, $DT_SINGLELINE, $DT_WORD_ELLIPSIS))
			
			If _GuiCtrlDropMenu_IsMenu(_GUICtrlListView_GetItemParam($hWnd, $iIndex)) Then
				$tRECTA = __DM_GetItemRightImageRect($tLVINFOMENU, $tRECT)
				_WinAPI_DrawThemeBackground($hTheme, 16, BitAND($iItemState, $DMS_DISABLED) ? 2 : 1, $hDestDC, $tRECTA)
			EndIf
			
			If BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_NOCHECK) Then ContinueLoop
			
			If BitAND($iItemState, $DMS_CHECKED) Then
				$tRECTA = __DM_GetItemLeftImageRect($tLVINFOMENU, $tRECT)
				_WinAPI_DrawThemeBackground($hTheme, 11, BitAND($iItemState, $DMT_RADIOCHECK) ? BitAND($iItemState, $DMS_DISABLED) ? 4 : 3 : BitAND($iItemState, $DMS_DISABLED) ? 2 : 1, $hDestDC, $tRECTA)
			Else
				$iImage = _GUICtrlListView_GetItemImage($hWnd, $iIndex)
				If BitAND($iItemState, $DMT_BITMAP) Then _GUIImageList_Draw($hImage, $iImage, $hDestDC, DllStructGetData($tRECT, "Left") + 8, DllStructGetData($tRECT, "Top") + 3, 1)
			EndIf
			
		EndIf
	Next
	;ConsoleWrite('++++++++++++++CREATE BITMAP AND+++++++++++++++' & @CR)
	;_WinAPI_CloseThemeData($hMenuBand)
	_WinAPI_CloseThemeData($hTheme)
	_WinAPI_SelectObject($hDestDC, $hDestSv)
	_WinAPI_DeleteDC($hDestDC)
	_WinAPI_SelectObject($hSrcDC, $hSrcSv)
	_WinAPI_DeleteDC($hSrcDC)
	_WinAPI_DeleteObject($hSource)
	
	Return $hBitmap
EndFunc   ;==>__DM_CreateBitmap

Func __DM_GetItemTextRect($tLVINFOMENU, $tRECT, $dpi)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, DllStructGetData($tLVINFOMENU, 'hMenu'))
	Local $iCheckWidth = _WinAPI_LoWord(DllStructGetData($tLVINFOMENU, 'SizeCheck')) + 12 * $dpi
	Local $iSubWidth = _WinAPI_LoWord(DllStructGetData($tLVINFOMENU, 'SizeSubMenu'))
	If BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_NOCHECK) Then $iCheckWidth = 0
	Local $iLeft = DllStructGetData($tRECT, "Left") + 8 * $dpi + $iCheckWidth
	Local $iTop = DllStructGetData($tRECT, "Top")
	Local $iRight = DllStructGetData($tRECT, "Right") - 8 * $dpi
	Return _WinAPI_CreateRect($iLeft, $iTop, $iRight, DllStructGetData($tRECT, "Bottom"))
EndFunc   ;==>__DM_GetItemTextRect

Func __DM_GetItemLeftImageRect($tLVINFOMENU, $tRECT)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, DllStructGetData($tLVINFOMENU, 'hMenu'))
	If BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_NOCHECK) Then Return 0
	Local $iCheckWidth = _WinAPI_LoWord(DllStructGetData($tLVINFOMENU, 'SizeCheck'))
	Local $iCheckHeight = _WinAPI_HiWord(DllStructGetData($tLVINFOMENU, 'SizeCheck'))
	Local $iLeft = DllStructGetData($tRECT, "Left") + 8
	Local $iTop = DllStructGetData($tRECT, "Top") + (DllStructGetData($tRECT, "Bottom") - DllStructGetData($tRECT, "Top") - $iCheckHeight) / 2
	Return _WinAPI_CreateRectEx($iLeft, $iTop, $iCheckWidth, $iCheckHeight)
EndFunc   ;==>__DM_GetItemLeftImageRect

Func __DM_GetItemRightImageRect($tLVINFOMENU, $tRECT)
	Local $iSubWidth = _WinAPI_LoWord(DllStructGetData($tLVINFOMENU, 'SizeSubMenu'))
	Local $iSubHeight = _WinAPI_HiWord(DllStructGetData($tLVINFOMENU, 'SizeSubMenu'))
	Local $iLeft = DllStructGetData($tRECT, "Right") - $iSubWidth - 4
	Local $iTop = DllStructGetData($tRECT, "Top") + (DllStructGetData($tRECT, "Bottom") - DllStructGetData($tRECT, "Top") - $iSubHeight) / 2
	Return _WinAPI_CreateRectEx($iLeft, $iTop, $iSubWidth, $iSubHeight)
EndFunc   ;==>__DM_GetItemRightImageRect

Func __DM_GetThemeState($hWnd, $iIndex, $bState = True)
	Local $iItemState = _GUICtrlListView_GetItemState($hWnd, $iIndex, BitOR($DMS_DISABLED, $DMS_SELECTED))
	If Not $bState Then $iItemState = BitXOR($iItemState, BitAND($iItemState, $DMS_SELECTED))
	Select
		Case BitAND($iItemState, $DMS_DISABLED) And BitAND($iItemState, $DMS_SELECTED)
			Return 4
		Case BitAND($iItemState, $DMS_DISABLED)
			Return 3
		Case BitAND($iItemState, $DMS_SELECTED)
			Return 2
		Case Else
			Return 1
	EndSelect
EndFunc   ;==>__DM_GetThemeState

Func __DM_ItemPaint($hWnd, $iItem = -1)
	Local $tRECT = _GUICtrlListView_GetItemRectEx($hWnd, $iItem, $LVIR_BOUNDS)
	Local $DRAWITEMSTRUCT = DllStructCreate($tagDRAWITEM)
	DllStructSetData($DRAWITEMSTRUCT, "CtlType", $ODT_LISTVIEW)
	DllStructSetData($DRAWITEMSTRUCT, "itemID", $iItem)
	DllStructSetData($DRAWITEMSTRUCT, "itemAction", $ODA_DRAWENTIRE)
	DllStructSetData($DRAWITEMSTRUCT, "hwndItem", $hWnd)
	DllStructSetData($DRAWITEMSTRUCT, "itemData", _GUICtrlListView_GetItemParam($hWnd, $iItem))
	DllStructSetData($DRAWITEMSTRUCT, "itemState", _GUICtrlListView_GetItemState($hWnd, $iItem, 0xffff))
	DllStructSetData($DRAWITEMSTRUCT, "Left", DllStructGetData($tRECT, "Left"))
	DllStructSetData($DRAWITEMSTRUCT, "Top", DllStructGetData($tRECT, "Top"))
	DllStructSetData($DRAWITEMSTRUCT, "Right", DllStructGetData($tRECT, "Right"))
	DllStructSetData($DRAWITEMSTRUCT, "Bottom", DllStructGetData($tRECT, "Bottom"))
	Local $hDC = _WinAPI_GetWindowDC($hWnd)
	DllStructSetData($DRAWITEMSTRUCT, "hDC", $hDC)
	_SendMessage($hWnd, $WM_ERASEBKGND, $hDC)
	If $iItem > -1 Then _SendMessage(_WinAPI_GetParent($hWnd), $WM_DRAWITEM, 0, $DRAWITEMSTRUCT, 0, 'wparam', 'struct*')
	_WinAPI_ReleaseDC($hWnd, $hDC)
EndFunc   ;==>__DM_ItemPaint

Func __DM_GetID($hWnd)
	Local $iValue = _WinAPI_GetWindowLong($hWnd, $GWL_USERDATA)
	Return SetError(@error Or Not $iValue, 0, Ptr($iValue))
EndFunc   ;==>__DM_GetID

Func __DM_ClearID($hWnd)
	_WinAPI_SetWindowLong($hWnd, $GWL_USERDATA, 0)
EndFunc   ;==>__DM_ClearID

Func __DM_GetParam($hWnd, $Param)
	Local $iIndex = __DM_GetID($hWnd)
	If @error Then Return SetError(1, 0, 0)
	If $iIndex > UBound($_a__DropDownData) - 1 Then Return SetError(1, 0, 0)
	Return $_a__DropDownData[$iIndex][$Param]
EndFunc   ;==>__DM_GetParam

Func __DM_SetParam($hWnd, $Param, $iVar)
	Local $iIndex = __DM_GetID($hWnd)
	If @error Then Return SetError(1, 0, 0)
	If $iIndex > UBound($_a__DropDownData) - 1 Then Return SetError(1, 0, 0)
	$_a__DropDownData[$iIndex][$Param] = $iVar
EndFunc   ;==>__DM_SetParam

Func __DM_GetSelectItem($hWnd)
	Local $aSel = _GUICtrlListView_GetSelectedIndices($hWnd, True)
	If Not $aSel[0] Then Return -1
	Return $aSel[1]
EndFunc   ;==>__DM_GetSelectItem

Func __DM_GetItemByID($hWnd, $iParam)
	For $iIndex = 0 To _GUICtrlListView_GetItemCount($hWnd) - 1
		If _GUICtrlListView_GetItemParam($hWnd, $iIndex) == $iParam Then Return $iIndex
	Next
	Return -1
EndFunc   ;==>__DM_GetItemByID

Func __DM_GetItemByIDFromData($aData, $iParam)
	If Not IsArray($aData) Then Return -1
	For $iIndex = 0 To UBound($aData) -1
		If DllStructGetData($aData[$iIndex], "itemID") == $iParam Then Return $iIndex
	Next
	Return -1
EndFunc

Func __GetItem($hWnd, $aData, $iItem)
	Local $isWindow = 0, $error = 0
	If _WinAPI_IsWindow($hWnd) Then
		If Not $bByPos Then $iItem = __DM_GetItemByID($hWnd, $iItem)
		$isWindow = 1
	Else 
		If Not $bByPos Then $iItem = __DM_GetItemByIDFromData($aData, $iItem)
	EndIf
	If $iItem == -1 Then $error = 1
	If $iItem > UBound($aData) -1 Then $error += 2
	Return SetError($error, $isWindow, $iItem)
EndFunc

Func __DM_AddMenuItem($hWnd, $sText, $iIndex, $hSubMenu)
	Local $iItem = _GUICtrlListView_AddItem($hWnd, $sText, $iIndex, $hSubMenu)
	Local $iType = 0
	If Not $sText Then $iType = BitOR($iType, $DMT_SEPARATOR, $DMS_DISABLED)
	If $iIndex > -1 Then $iType = BitOR($iType, $DMT_BITMAP)
	_GUICtrlListView_SetItemState($hWnd, $iItem, $iType, 0xFFFF)
	_GUICtrlListView_SetItemIndent($hWnd, $iItem, 0)
	_GUICtrlListView_SetItemGroupID($hWnd, $iItem, 1)
	Return $iItem
EndFunc   ;==>__DM_AddMenuItem

Func __DM_InsertMenuItem($hWnd, $iIndex, $sText, $iImage, $hSubMenu)
	_GUICtrlListView_BeginUpdate($hWnd)
	Local $iItem = _GUICtrlListView_InsertItem($hWnd, $sText, $iIndex, $iImage, $hSubMenu)
	_GUICtrlListView_SetItemIndent($hWnd, $iItem, 0)
	Local $iType = 0
	If Not $sText Then $iType = BitOR($iType, $DMT_SEPARATOR, $DMS_DISABLED)
	If $iImage > -1 Then $iType = BitOR($iType, $DMT_BITMAP)
	_GUICtrlListView_SetItemState($hWnd, $iItem, $iType, 0xFFFF)
	For $iIndex = 0 To _GUICtrlListView_GetItemCount($hWnd) - 1
		_GUICtrlListView_SetItemGroupID($hWnd, $iIndex, -1)
		_GUICtrlListView_SetItemGroupID($hWnd, $iIndex, 1)
	Next
	Local $hMenu = __DM_GetID(_WinAPI_GetParent($hWnd))
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	If _WinAPI_IsWindowVisible(DllStructGetData($tDROPMENUINFO, 'hDropDown')) Then __DM_UpdateView($hMenu)
	_GUICtrlListView_EndUpdate($hWnd)
	Return $iItem
EndFunc   ;==>__DM_InsertMenuItem

Func __DM_GetInfoMenu($hWnd, $iItem)
	Local $iParam = _GUICtrlListView_GetItemParam($hWnd, $iItem)
	Local $hSubMenu = _GuiCtrlDropMenu_IsMenu($iParam) ? $iParam : 0
	Local $tDROPMENUITEMINFO = DllStructCreate($tagDROPMENUITEMINFO)
	DllStructSetData($tDROPMENUITEMINFO, 'Type', _GUICtrlListView_GetItemState($hWnd, $iItem, $DMT_TYPE))
	DllStructSetData($tDROPMENUITEMINFO, 'State', _GUICtrlListView_GetItemState($hWnd, $iItem, $DMT_STATE))
	DllStructSetData($tDROPMENUITEMINFO, 'ID', _GUICtrlListView_MapIndexToID($hWnd, $iItem))
	DllStructSetData($tDROPMENUITEMINFO, 'SubMenu', $hSubMenu)
	DllStructSetData($tDROPMENUITEMINFO, 'Param', $hSubMenu ? 0 : $iParam)
	DllStructSetData($tDROPMENUITEMINFO, 'BmpItem', _GUICtrlListView_GetItemImage($hWnd, $iItem))
	Return $tDROPMENUITEMINFO
EndFunc   ;==>__DM_GetInfoMenu

Func __DM_UpdateView($hMenu)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	_WinAPI_LockWindowUpdate(DllStructGetData($tDROPMENUINFO, 'hDropDown'))
	_WinAPI_SetWindowPos(DllStructGetData($tDROPMENUINFO, 'hHeaderView'), $HWND_TOP, 1, 1, DllStructGetData($tDROPMENUINFO, 'X') - 4, 0, BitOR($SWP_NOACTIVATE, $SWP_NOSENDCHANGING))
	_SendMessage(DllStructGetData($tDROPMENUINFO, 'hDropDown'), $DMM_INITMENU, DllStructGetData($tDROPMENUINFO, 'hHeaderView'), 0, 0, 'handle')
	_WinAPI_LockWindowUpdate(0)
	_WinAPI_RedrawWindow(DllStructGetData($tDROPMENUINFO, 'hDropDown'), 0, 0, BitOR($RDW_INVALIDATE, $RDW_ALLCHILDREN))
	Return 1
EndFunc   ;==>__DM_UpdateView

Func __DM_ExistsSubMenu($hWnd)
	For $iIndex = 0 To _GUICtrlListView_GetItemCount($hWnd) - 1
		If _GuiCtrlDropMenu_IsMenu(_GUICtrlListView_GetItemParam($hWnd, $iIndex)) Then Return True
	Next
	Return False
EndFunc   ;==>__DM_ExistsSubMenu

Func __DM_ListViewToArray($hWnd)
	Local $iCount = _GUICtrlListView_GetItemCount($hWnd)
	Local $aArray[$iCount][3]
	For $iIndex = 0 To $iCount - 1
		$aArray[$iIndex][0] = _GUICtrlListView_GetItemText($hWnd, $iIndex)
		$aArray[$iIndex][1] = _GUICtrlListView_GetItemImage($hWnd, $iIndex)
		$aArray[$iIndex][2] = _GUICtrlListView_GetItemParam($hWnd, $iIndex)
	Next
	Return $aArray
EndFunc   ;==>__DM_ListViewToArray

Func __DM_ArrayToListView($hWnd, ByRef $aArray)
	For $iIndex = 0 To UBound($aArray) - 1
		__DM_AddMenuItem($hWnd, $aArray[$iIndex][0], $aArray[$iIndex][1], $aArray[$iIndex][2])
	Next
EndFunc   ;==>__DM_ArrayToListView

Func __DM_SubMenuActivate($_h_Wnd = 0, $_i_Item = 0, $iTime = 250)
	Local Static $hWnd, $iItem
	If IsDeclared('_h_Wnd') Then
		$hWnd = $_h_Wnd
		$iItem = $_i_Item
		Return AdlibRegister(__DM_SubMenuActivate, 300)
	EndIf
	AdlibUnRegister(__DM_SubMenuActivate)
	If __DM_GetSelectItem($hWnd) <> $iItem Then Return
	Local $iParam = Ptr(_GUICtrlListView_GetItemParam($hWnd, $iItem))
	Local $tLVINFOMENU = DllStructCreate($tagLVINFOMENU, __DM_GetID($hWnd))
	;DllStructSetData($tLVINFOMENU, 'hSubMenu', $iParam)
	Local $tRECT = _GUICtrlListView_GetItemRectEx($hWnd, $iItem, $LVIR_BOUNDS)
	__TB_RectClientToScreen($hWnd, $tRECT)
	;DllStructSetData($tRECT, 'Right', DllStructGetData($tRECT, 'Right') + _SendMessage($hWnd, $DMM_GETWIDTHSCROLLBAR))
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, DllStructGetData($tLVINFOMENU, 'hMenu'))
	DllStructSetData($tDROPMENUINFO, 'hSubMenu', $iParam)
	Local $iNotify = DllStructGetData($tDROPMENUINFO, 'Notify')
	Local $hParent = DllStructGetData($tDROPMENUINFO, 'hDropDown')
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $iParam)
	
	__TrackPopupMenu($iParam, $hParent,  DllStructGetData($tRECT, 'Right') - 4, DllStructGetData($tRECT, 'Top') - 3, 1, 1, BitOR(Not $iNotify, 4), 0)
EndFunc   ;==>__DM_SubMenuActivate

Func __TrackPopupMenu($hMenu, $hParent, $iX, $iY, $iAlignX, $iAlignY, $iNotify, $iButtons)
	Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
	Local $iGuiStyle = DllStructGetData($tDROPMENUINFO, 'GuiStyle')
	
	Local $hDropDown = _WinAPI_CreateWindowEx($WS_EX_TOOLWINDOW + $WS_EX_NOACTIVATE, 'DropDown', '#' & DllStructGetData($tDROPMENUINFO, 'Data'), $iGuiStyle, 0, 0, 0, 0, _WinAPI_GetAncestor($hParent, $GA_ROOT), 0, $h__instance_gui, $hMenu)
	If Not $hDropDown Then Return 0
	If _WinAPI_GetParent(DllStructGetData($tDROPMENUINFO, 'hDropDown')) == DllStructGetData($tDROPMENUINFO, 'hParent') Then _WinAPI_SetFocus(DllStructGetData($tDROPMENUINFO, 'hParent'))
	If Not BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_MODELESS) And Not BitAND($iNotify, 1) Then _SendMessage(DllStructGetData($tDROPMENUINFO, 'hParent'), $WM_ENTERMENULOOP, 1)
	DllStructSetData($tDROPMENUINFO, 'Notify', Not BitAND($iNotify, 1))
	;ConsoleWrite('NOTIFY ' & DllStructGetData($tDROPMENUINFO, 'hDropDown') & @CR)
	If Not _WinAPI_IsWindowVisible(DllStructGetData($tDROPMENUINFO, 'hDropDown')) Then
		_SendMessage(DllStructGetData($tDROPMENUINFO, 'hDropDown'), $DMM_INITMENU, DllStructGetData($tDROPMENUINFO, 'hHeaderView'), 0, 0, 'hwnd')
	EndIf
	
	If $iX == -1 Then $iX = _WinAPI_GetMousePosX()
	If $iY == -1 Then $iY = _WinAPI_GetMousePosY()
	
	Local $iWidth = DllStructGetData($tDROPMENUINFO, 'X'), $iHeight = DllStructGetData($tDROPMENUINFO, 'Y')
	
	Switch $iAlignX
		Case 0
			$iX -= $iWidth / 2
		Case 2
			$iX -= $iWidth
	EndSwitch
	
	Switch $iAlignY
		Case 0
			$iY -= $iHeight
		Case 2
			$iY -= $iHeight / 2
	EndSwitch
	
	If $iX + $iWidth > @DesktopWidth Then $iX = @DesktopWidth - $iWidth
	If $iY + $iHeight > @DesktopHeight Then $iY = @DesktopHeight - $iHeight
	
	_WinAPI_SetWindowPos($hDropDown, $HWND_TOPMOST, $iX, $iY, 0, 0, BitOR($SWP_NOSIZE, $SWP_NOOWNERZORDER, $SWP_NOACTIVATE))
	_WinAPI_ShowWindow($hDropDown)
	_WinAPI_RedrawWindow($hDropDown, 0, 0, BitOR($RDW_INVALIDATE, $RDW_ALLCHILDREN))
	;__TB_StructureDisplay(_WinAPI_GetClientRect($hDropDown), $tagRECT)
	;__ShowDrop($hDropDown)
	
	;ConsoleWrite('_GuiCtrlDropMenu_TrackPopupMenu ' & $hDropDown & @TAB & _WinAPI_IsWindow($hDropDown) & @TAB & _WinAPI_IsWindowVisible($hDropDown) & @TAB & WinGetState(HWnd($hDropDown)) & @CR)
	
	If Not BitAND($iNotify, 4) And Not BitAND(DllStructGetData($tDROPMENUINFO, 'Style'), $DMNS_MODELESS) Then
		
		While _WinAPI_IsWindow($hDropDown)
			Sleep(100)
		WEnd
		
		If Not BitAND($iNotify, 1) Then _SendMessage(DllStructGetData($tDROPMENUINFO, 'hParent'), $WM_EXITMENULOOP, 1)
		If BitAND($iNotify, 2) Then Return DllStructGetData($tDROPMENUINFO, 'SelectMenu')
	EndIf
	
	DllStructSetData($tDROPMENUINFO, 'SelectMenu', 0)
	
	Return 1
EndFunc

Func _WinAPI_ShowWindowEx($hWnd, $iState = @SW_SHOW, $bActivate = True)
	If $bActivate Then Return _WinAPI_ShowWindow($hWnd, $iState)
	_WinAPI_SetWindowPos($hWnd, 0, 0, 0, 0, 0, BitOR($SWP_NOACTIVATE, $SWP_NOMOVE, $SWP_NOSIZE, $SWP_NOZORDER, $SWP_NOREPOSITION, ($iState = @SW_SHOW ? $SWP_SHOWWINDOW : $SWP_HIDEWINDOW)))
	If $iState = @SW_SHOW Then _WinAPI_DefWindowProcW($hWnd, $WM_NCACTIVATE, True, 0)
	Return _SendMessage($hWnd, $WM_SHOWWINDOW, ($iState = @SW_SHOW ? True : False))
EndFunc   ;==>_WinAPI_ShowWindowEx

Func __DM_ArrayGetEmptyIndex($aArray)
	Local $iValue
	For $i = 0 To UBound($aArray) - 1
		$iValue = 0
		For $j = 0 To $id_Max - 1
			If $aArray[$i][$j] == 0 Then ContinueLoop
			$iValue = 1
			ContinueLoop 2
		Next
		If Not $iValue Then Return $i
	Next
	Return -1
EndFunc   ;==>__DM_ArrayGetEmptyIndex

Func __DM_BitExists($hBitmap)
	Local $tObj = DllStructCreate($tagBITMAP)
	Local $aRet = DllCall('gdi32.dll', 'int', 'GetObject', 'handle', $hBitmap, 'int', DllStructGetSize($tObj), 'struct*', $tObj)
	If @error Or Not $aRet[0] Then Return SetError(@error + 20, @extended, 0)
	Return 1
EndFunc   ;==>__DM_BitExists

Func _WINAPI_CreatePatternBrush($hbmp)
    Local $brush = DllCall("gdi32.dll","long","CreatePatternBrush","long",$hbmp)
    If Not @error Then Return $brush[0]
    Return 0
EndFunc

Func __HeightToSize($size, $hWnd = _WinAPI_GetDesktopWindow())
	Local $hDC = _WinAPI_GetDC($hWnd)
	Return -Round(_WinAPI_MulDiv($size, 72, _WinAPI_GetDeviceCaps($hDC, $LOGPIXELSY)), 1)
EndFunc

Func __SizeToHeight($size, $hWnd = _WinAPI_GetDesktopWindow())
	Local $hDC = _WinAPI_GetDC($hWnd)
	Return -Round(_WinAPI_MulDiv($size, _WinAPI_GetDeviceCaps($hDC, $LOGPIXELSY), 72), 1)
EndFunc

Func __BinaryToStructure($Binary, $tagSTRUCTURE)
	Local $byte = BinaryLen($Binary)*2
	If @AutoItX64 Then $byte *= 2
	Local $tPoint = DllStructCreate('byte [' & $byte & ']')
	Local $tSTRUCTURE = DllStructCreate($tagSTRUCTURE, DLLStructGetPtr($tPoint, 1))
	DllStructSetData($tPoint, 1, $Binary)
	Return $tSTRUCTURE
EndFunc

Func __StructureToBinary($structure, $tagSTRUCTURE)
	Local $byte = DllStructGetSize($structure)
	Local $tBYTE = DllStructCreate('byte [' & $byte & ']')
	Local $tSTRUCT = DllStructCreate($tagSTRUCTURE, DLLStructGetPtr($tBYTE, 1))
	Local $i = 0
	While 1
		$i += 1
		DllStructSetData($tSTRUCT, $i, DllStructGetData($structure, $i))
		If @error Then Return String(DllStructGetData($tBYTE, 1))
	WEnd 
EndFunc


Func _OnDropDownExit()
	
EndFunc   ;==>_OnDropDownExit

#EndRegion Include Functions