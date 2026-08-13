;GuiToolbarEx.au3
#include-once
#include <Include\WorkerW.au3>
#include <Include\SearchBox.au3>
#include <Include\GuiProgress.au3>
;#include <Include\AddressBar.au3>
#include <Include\DropDown.au3>
#include <Include\GuiTabEx.au3>
#include <Array.au3>
#include <WinAPIHObj.au3>

Global Const $BTN_DROPWIDTH = 15

Global Const $BTNS_USECHEVRON = $BTNS_CHECK

Global Const $TBN_ISMOUSHOVER = 0x2000 + 12
;Global Const $TBN_GETSIZE = 0x2000 + 13
Global Const $TBN_GETWINRECT = 0x2000 + 14
Global Const $TBN_GETBUTTONTIMER = 0x2000 + 15
Global Const $TBN_SETBUTTONTIMER = 0x2000 + 16
Global Const $TBN_GETCOUNTTIMERS = 0x2000 + 17
Global Const $TBN_GETTIMERDATA = 0x2000 + 18
Global Const $TBN_GETUPDATERECT = 0x2000 + 19

Global Const $SBN_ISMOUSHOVER = 0x2000 + 12
Global Const $SBN_GETSIZE = 0x2000 + 13
Global Const $SBN_GETWINRECT = 0x2000 + 14
Global Const $SBN_GETTEXTRECT = 0x2000 + 15
Global Const $SBN_GETBUTSTATE = 0x2000 + 16
Global Const $SBN_GETIMAGESIZE = 0x2000 + 17
Global Const $SBN_GETIMAGERECT = 0x2000 + 18
Global Const $SBN_GETSELECTTEXT = 0x2000 + 19
Global Const $SBN_SETDEFAULTTEXT = 0x2000 + 20
Global Const $SBN_GETDEFAULTTEXT = 0x2000 + 21

; Toolbar custom draw return flags
Global Const $TBCDRF_BLENDICON      = 0x200000 ; // Use ILD_BLEND50 on the icon image
;Global Const $TBCDRF_HILITEHOTTRACK = 0x020000 ; // Use color of the button bk when hottracked
Global Const $TBCDRF_NOBACKGROUND   = 0x400000 ; // Use ILD_BLEND50 on the icon image
Global Const $TBCDRF_NOEDGES        = 0x010000 ; // Don't draw button edges
Global Const $TBCDRF_NOETCHEDEFFECT = 0x100000 ; // Don't draw etched effect for disabled items
Global Const $TBCDRF_NOMARK         = 0x080000 ; // Don't draw default highlight of image/text for TBSTATE_MARKED
Global Const $TBCDRF_NOOFFSET       = 0x040000 ; // Don't offset button if pressed
;Global Const $TBCDRF_USECDCOLORS    = 0x800000 ; >= Vista // Use CustomDrawColors to RenderText regardless of VisualStyle

Global Const Enum $TB_ID_HWND, $TB_ID_BITMAPS, $TB_ID_SIZE, $TB_ID_CUSTOMDRAW, $TB_ID_HOTITEM, $TB_ID_HTIMER, $TB_ID_WNDPROC, $imax

Global Const $tagTBBITMAPS = $tagSIZE & ';handle hDefBitmap;handle hHotBitmap;handle hBackBitmap;ulong WidthBmp'
Global Const $tagTBBANDSIZE = 'struct;int [2];int [2];int [2];int [2];int [2];endstruct'
Global Const $tagTBBUTTONDATA = 'struct;handle hWnd;int Button;int Timer;int64 Step;handle hBitmap;endstruct'
Global Const $tagTBBITMAPSBOX = $tagSIZE & ';handle hDefBitBox;handle hHotBitBox;handle hFocusBitBox;endstruct'
;Global Const $tagSTYLESTRUCT = 'DWORD styleOld;DWORD styleNew'

Global $a__toolsdata[0][$imax]

Func __TB_Inicialize($hWnd)
	Local Static $hDll = DllCallbackRegister('_WndToolProc', 'ptr', 'hwnd;uint;wparam;lparam;uint_ptr;dword_ptr')
	Switch _WinAPI_GetClassName($hWnd)
		Case 'msctls_progress32'
			_WinAPI_SetWindowSubclass($hWnd, DllCallbackGetPtr($hDll), _WinAPI_GetDlgCtrlID($hWnd), DllCallbackGetPtr($hDll))
			_SendMessage($hWnd, $PBM_SETRANGE, 0, _WinAPI_MakeLong(0, 100))
			_SendMessage($hWnd, $PBM_SETSTEP, 1, 0)
		Case 'RICHEDIT50W'
			_WinAPI_SetWindowSubclass($hWnd, DllCallbackGetPtr($hDll), _WinAPI_GetDlgCtrlID($hWnd), DllCallbackGetPtr($hDll))
			_GUICtrlRichEdit_SetText($hWnd, '')
			_GUICtrlRichEdit_SetEventMask($hWnd, BitOR($ENM_CHANGE, $ENM_SELCHANGE))
			_WinAPI_SetFocus(0)
		Case Else 
			_WinAPI_SetWindowSubclass($hWnd, DllCallbackGetPtr($hDll), _WinAPI_GetDlgCtrlID($hWnd), DllCallbackGetPtr($hDll))
	EndSwitch
EndFunc   ;==>__TB_Inicialize

Func __TB_GetTextRect($tRECT, $tMetrics, $iState = 0)
	Local $iIntX = 0, $iIntY = 0
	If BitAND($iState, $TBSTATE_PRESSED) Then $iIntY += 1
	Return _WinAPI_CreateRect(DllStructGetData($tRECT, 'Left') + DllStructGetData($tMetrics, "XPad") / 2 + $iIntY, DllStructGetData($tMetrics, "YPad") / 2 + $iIntY, _
			DllStructGetData($tRECT, 'Right') - DllStructGetData($tMetrics, "XPad") / 2 - $iIntX, DllStructGetData($tRECT, 'Bottom') - DllStructGetData($tMetrics, "YPad") / 2 + $iIntY)
EndFunc   ;==>__TB_GetTextRect

Func __TB_DrawAddressBandBackground($hWnd, $hDC, $tRECT, $iBarState, $bErase = True)
	;If $bErase Then _WinAPI_DrawThemeParentBackground($hWnd, $hDC)
	;_WinAPI_PatBlt($hDC, DllStructGetData($tRECT, 'Left'), DllStructGetData($tRECT, 'Top'), DllStructGetData($tRECT, 'Right'), DllStructGetData($tRECT, 'Bottom'), $BLACKNESS)
	Local $hTheme = _WinAPI_OpenThemeData($hAddress_Band_Root, 'AddressBand') ; DarkMode_ABComposited::AddressBand
	_WinAPI_DrawThemeBackground($hTheme, 1, $iBarState, $hDC, _WinAPI_GetClientRect($hProgress), $tRECT)
	_WinAPI_CloseThemeData($hTheme)
EndFunc

Func _GetCountSteps($Section)
	Local $iRange = 0
	$FileType = _GetCurrentType()
	Local $sExt = $FileType[$Section - 1]
	$iRange += _GetCountKey(_ROOT($Section) & '\' & $SHELL_DESKTOP_TOP)
	$iRange += _GetCountKey(_ROOT($Section) & '\' & $SHELL_DESKTOP_BOTTOM)
	$iRange += _GetCountKey(_ROOT($Section) & '\' & $SHELL_COMPUTER)
	$iRange += _GetCountKey(_ROOT($Section) & '\' & $SHELL_DRIVE)
	$iRange += _GetCountKey(_ROOT($Section) & '\' & $SHELL_FOLDER_TOP)
	$iRange += _GetCountKey(_ROOT($Section) & '\' & $SHELL_FOLDER_BOTTOM)
	$iRange += _GetCountKey(_ROOT($Section) & $APP_TYPE & '\' & $sExt)
	$iRange += _GetCountKey(_ROOT($Section) & '\SystemFileAssociations\' & $sExt & '\shell')
	$iRange += _GetCountKey(_ROOT($Section) & '\' & _GetProgID($sExt) & '\shell')
	$iRange += _GetCountKey($FILE_EXTS & '\' & $sExt)
	Return $iRange
EndFunc

Func _TB_ProgressSetStep($iTabCurSel = -1, $bForce = false)
	Local $iRange = 0
	If $iTabCurSel == -1 Or $iTabCurSel = 3 Then $iRange = 31 * ($SavesThemeColor[0] + 2) ; Theme Colors
	If $iTabCurSel == -1 Or $iTabCurSel = 4 Then $iRange += UBound(StringSplit(RegRead($BWM, 'IncludeUserProcess'), @LF, 2)) ; Список процессов _GetEnumValue($BWMLIB & "\AttachList", False, True)[0]
	If $tCurentModify.configcontextmenu And $iTabCurSel == 5 Then
		Local Static $aCount[3]
		If Not $aCount[$Section -1] Or $bForce Then $aCount[$Section -1] = _GetCountSteps($Section)
		$iRange += $aCount[$Section -1]
	EndIf
	ConsoleWrite('$PBM_SETRANGE ' & $iRange & @CR)
	_WinAPI_SetWindowLong($hProgress, $GWL_STYLE, 0x52000000)
	_SendMessage($hProgress, $PBM_SETRANGE, 0, _WinAPI_MakeLong(0, $iRange))
	_SendMessage($hProgress, $PBM_SETPOS, 1)
EndFunc

Func _TB_SetRawInput($hWnd)
	Local $tRID = DllStructCreate($tagRAWINPUTDEVICE)
	DllStructSetData($tRID, 'UsagePage', 0x01)
	DllStructSetData($tRID, 'Usage', 0x06)
	DllStructSetData($tRID, 'Flags', 0)
	DllStructSetData($tRID, 'hTarget', $hWnd)
	_WinAPI_RegisterRawInputDevices($tRID)
EndFunc

Func __TB_ToolBarDestroyMenu($hWnd)
	Local $iParam
	For $iIndex = 0 To _SendMessage($hWnd, $TB_BUTTONCOUNT)
		$iParam = _GUICtrlToolbar_GetButtonParam($hWnd, _GUICtrlToolbar_IndexToCommand($hWnd, $iIndex))
		If _GuiCtrlDropMenu_IsMenu($iParam) Then _GuiCtrlDropMenu_DestroyMenu($iParam)
	Next
EndFunc

Func __TB_GetRectSelect($hWnd, $iStart, $iEnd)
	Local $tRECT = DllStructCreate($tagRECT)
	_SendMessage($DirectUIHWND, $SBN_GETTEXTRECT, 0, $tRECT, 0, "wparam", "struct*")
	Local $tPOINT = DllStructCreate("int X;int Y")
	Local $aCharPos = _GUICtrlRichEdit_GetXYFromCharPos($hWnd, $iStart)
	If @error Then Return 0
	DllStructSetData($tPOINT, "X", $aCharPos[0])
	$aCharPos = _GUICtrlRichEdit_GetXYFromCharPos($hWnd, $iEnd)
	If @error Then Return 0
	DllStructSetData($tPOINT, "Y", $aCharPos[0])
	_WinAPI_ClientToScreen($hWnd, $tPOINT)
	_WinAPI_ScreenToClient($DirectUIHWND, $tPOINT)
	DllStructSetData($tRECT, 'Left', DllStructGetData($tPOINT, 'X'))
	DllStructSetData($tRECT, 'Right', DllStructGetData($tPOINT, 'Y'))

	Return $tRECT
EndFunc   ;==>__TB_GetRectSelect

Func __TB_GetVisibleTextOfLine($hWnd)
	Local $tRECT = DllStructCreate($tagRECT)
	_SendMessage($hWnd, $EM_GETRECT, 0, $tRECT, 0, "wparam", "struct*")
	Local $iStart = _GUICtrlRichEdit_GetCharPosFromXY($hWnd, DllStructGetData($tRECT, 'Left'), DllStructGetData($tRECT, 'Top'))
	;ConsoleWrite('POS1: ' & $iStart & @TAB & @error & @CR)
	Local $iEnd = _GUICtrlRichEdit_GetCharPosFromXY($hWnd, DllStructGetData($tRECT, 'Right'), DllStructGetData($tRECT, 'Top'))
	;ConsoleWrite('POS2: ' & $iEnd & @TAB & @error & @CR)
	Local $sText = _GUICtrlRichEdit_GetTextInRange($hWnd, $iStart, $iEnd)
	If Not @error Then Return $sText
	Return ''
EndFunc   ;==>__TB_GetVisibleTextOfLine

Func __TB_RectClientToScreen($hWnd, ByRef $tRECT)
	Local $tPoint = DllStructCreate("int X;int Y")
	DllStructSetData($tPoint, "X", DllStructGetData($tRECT, 'Left'))
	DllStructSetData($tPoint, "Y", DllStructGetData($tRECT, 'Top'))
	_WinAPI_ClientToScreen($hWnd, $tPoint)
	DllStructSetData($tRECT, 'Left', DllStructGetData($tPoint, 'X'))
	DllStructSetData($tRECT, 'Top', DllStructGetData($tPoint, 'Y'))

	DllStructSetData($tPoint, "X", DllStructGetData($tRECT, 'Right'))
	DllStructSetData($tPoint, "Y", DllStructGetData($tRECT, 'Bottom'))
	_WinAPI_ClientToScreen($hWnd, $tPoint)
	DllStructSetData($tRECT, 'Right', DllStructGetData($tPoint, 'X'))
	DllStructSetData($tRECT, 'Bottom', DllStructGetData($tPoint, 'Y'))
EndFunc

Func __TB_RectScreenToClient($hWnd, ByRef $tRECT)
	Local $tPoint = DllStructCreate("int X;int Y")
	DllStructSetData($tPoint, "X", DllStructGetData($tRECT, 'Left'))
	DllStructSetData($tPoint, "Y", DllStructGetData($tRECT, 'Top'))
	_WinAPI_ScreenToClient($hWnd, $tPoint)
	DllStructSetData($tRECT, 'Left', DllStructGetData($tPoint, 'X'))
	DllStructSetData($tRECT, 'Top', DllStructGetData($tPoint, 'Y'))

	DllStructSetData($tPoint, "X", DllStructGetData($tRECT, 'Right'))
	DllStructSetData($tPoint, "Y", DllStructGetData($tRECT, 'Bottom'))
	_WinAPI_ScreenToClient($hWnd, $tPoint)
	DllStructSetData($tRECT, 'Right', DllStructGetData($tPoint, 'X'))
	DllStructSetData($tRECT, 'Bottom', DllStructGetData($tPoint, 'Y'))
EndFunc

Func __RedrawBar(ByRef $bRedrawBar, $berase = True)
	If $bRedrawBar Then Return 
	$bRedrawBar = True 
	
	Local $hDC = _WinAPI_GetWindowDC($hProgress)
	Local $tRECTA = _WinAPI_GetWindowRect($AddressBar)
	__TB_RectScreenToClient($hProgress, $tRECTA)
	_WinAPI_ExcludeClipRect($hDC, $tRECTA)
	Local $tRECTB = _WinAPI_GetWindowRect($ButBar)
	__TB_RectScreenToClient($hProgress, $tRECTB)
	_WinAPI_ExcludeClipRect($hDC, $tRECTB)
	
	_SendMessage($hProgress, ($berase ? $WM_ERASEBKGND : $WM_PRINTCLIENT), $hDC)
	_WinAPI_ReleaseDC($hProgress, $hDC)
	
	Local $aHwndFrom[2] = [$AddressBar, $ButBar]
	For $HwndFrom In $aHwndFrom 
		$hDC = _WinAPI_GetWindowDC($HwndFrom)
		_SendMessage($HwndFrom, $WM_PAINT, $hDC)
		_WinAPI_ReleaseDC($HwndFrom, $hDC)
	Next
	
	$bRedrawBar = False
EndFunc

Func __SetActive($dpi, $enable = True)
	_WinAPI_SetWindowTheme($g_hReBar, ($enable ? 'NavbarComposited' : 'InactiveNavbarComposited'), 0)
	_WinAPI_SetWindowTheme($hBreadcrumb_Parent, ($enable ? 'BreadcrumbBarComposited' : 'InactiveBreadcrumbBarComposited'), 0)
	_WinAPI_SetWindowTheme($TravelBar, ($enable ? 'BBComposited' : 'InactiveBBComposited'), 0)
	_WinAPI_SetWindowTheme($AddressBar, ($enable ? 'BBComposited' : 'InactiveBBComposited'), 0)
	_WinAPI_SetWindowTheme($ButBar, ($enable ? 'BBComposited' : 'InactiveBBComposited'), 0)
EndFunc

Func __CreateImageList(ByRef $hImage, $dpi)
	Local $iBitName = __TB_GetRes($dpi)
	Local $hBitmap = __TB_LoadBitmap(@SystemDir & '\ExplorerFrame.dll', $iBitName, 4, 3)
	Local $tSIZE = _WinAPI_GetBitmapDimension($hBitmap)
	_GUIImageList_SetIconSize($hImage, DllStructGetData($tSIZE, 2), DllStructGetData($tSIZE, 2))
	_GUIImageList_Remove($hImage)
	;ConsoleWrite('__CreateImageList ' & DllStructGetData($tSIZE, 2) & @TAB & $iBitName & @CR)
	_GUIImageList_Add($hImage, $hBitmap)
	_WinAPI_DeleteObject($hBitmap)
	$hBitmap = __TB_LoadBitmap(@SystemDir & '\ExplorerFrame.dll', $iBitName, 4, 2)
	_GUIImageList_Add($hImage, $hBitmap)
	_WinAPI_DeleteObject($hBitmap)
	$hBitmap = __TB_LoadBitmap(@SystemDir & '\ExplorerFrame.dll', $iBitName, 4, 1)
	_GUIImageList_Add($hImage, $hBitmap)
	_WinAPI_DeleteObject($hBitmap)
EndFunc

Func __TB_GetRes($dpi)
	Local $Int = 0
	If $dpi Then 
		$Int = Int(($dpi -1.0) / 0.25)
	EndIf
	If $Int > 7 Then $Int = 7
	Local Static $W_10[8][2] = [[288, 300], [295, 594], [296, 595], [320, 596], [321, 597], [322, 598], [323, 599], [592, 600]]
	Local Static $W_11[8][2] = [[288, 593], [295, 594], [296, 595], [320, 596], [321, 597], [322, 598], [323, 599], [592, 600]]
	Switch $WINVER
		Case 10
			Return (_IsWhite(_WinAPI_SwitchColor(_WinAPI_GetSysColor($COLOR_WINDOW))) ? $W_10[$Int][0] : $W_10[$Int][1])
		Case 11
			Return (_IsWhite(_WinAPI_SwitchColor(_WinAPI_GetSysColor($COLOR_WINDOW))) ? $W_11[$Int][0] : $W_11[$Int][1])
	EndSwitch
EndFunc

Func __CreateMenuImageList($dpi)
	Local $hImage = _GUIImageList_Create(16 * $dpi, 16 * $dpi, 5, 1)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 9)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 23)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 11)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 15)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 14)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 17)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 5)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 20)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 1)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 12)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 22)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 16)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 19)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 24)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 10)
	_GUIImageList_AddIcon($hImage, @ScriptDir & "\bwm.exe", 3)
	Return $hImage
EndFunc

Func __GetBandLength()
	Local $Binary = RegRead($AllFolders, 'NavBar')
	If Not $Binary Or @error Then Return 175
	Local $tPoint = DllStructCreate('byte[151]')
	DllStructSetData($tPoint, 1, $Binary)
	Local $tagNAVBAR = "byte[16];dword;dword;byte[123];byte[4]"
	Local $tNavBar = DllStructCreate($tagNAVBAR, DllStructGetPtr($tPoint))
	If Not Int(DllStructGetData($tNavBar, 5)) Then Return 175
	Return Int(DllStructGetData($tNavBar, 5))
EndFunc

Func __SetBandLength($iLength)
	Local $Binary = RegRead($AllFolders, 'NavBar')
	Local $tPoint = DllStructCreate('byte[151]')
	DllStructSetData($tPoint, 1, $Binary)
	Local $tagNAVBAR = "byte[16];dword;dword;byte[123];byte[4]"
	Local $tNavBar = DllStructCreate($tagNAVBAR, DllStructGetPtr($tPoint))
	DllStructSetData($tNavBar, 5, Ceiling($iLength))
	RegWrite($AllFolders, 'NavBar', $_REG_BINARY, DllStructGetData($tPoint, 1))
EndFunc

Func __PostDropDown()
	AdlibUnRegister(__PostDropDown)
	_WinAPI_PostMessage($WorkerW, $WM_POSTDROPDOWN, 0, 0)
EndFunc

Func __TB_SetTextRect($hWnd)
	Local $tRECT = _WinAPI_GetWindowRect($hWnd)
	Local $iWidth = DllStructGetData($tRECT, 'Right') - DllStructGetData($tRECT, 'Left')
	Local $iHeight = DllStructGetData($tRECT, 'Bottom') - DllStructGetData($tRECT, 'Top')
	$tRECT = _WinAPI_CreateRectEx(14, 12, $iWidth -$iHeight -10, $iHeight -24)
	_SendMessage($hWnd, $EM_SETRECTNP, 0, $tRECT, 0, "wparam", "struct*")
EndFunc

Func __TB_GetStateID($iItemState, $iOtherHot = 2)
	Select
		Case BitAND($iItemState, $ODS_DISABLED)
			Return 4
		Case BitAND($iItemState, $ODS_HOTLIGHT) And BitAND($iItemState, $ODS_CHECKED)
			Return 6
		Case BitAND($iItemState, $ODS_CHECKED)
			Return 5
		Case BitAND($iItemState, $ODS_SELECTED)
			Return 3
		Case BitAND($iItemState, $ODS_HOTLIGHT)
			Return $iOtherHot
		Case Else
			Return 1
	EndSelect
EndFunc   ;==>__TB_GetStateID

Func __TB_ItemStateCalc($iState, $bHot = 0)
	Local $iItemState = 0
	If $bHot Then $iItemState = BitOR($iItemState, $ODS_HOTLIGHT)
	If Not BitAND($iState, $TBSTATE_ENABLED) Then $iItemState = BitOR($iItemState, $ODS_DISABLED)
	If BitAND($iState, $TBSTATE_CHECKED) Then $iItemState = BitOR($iItemState, $ODS_CHECKED)
	If BitAND($iState, $TBSTATE_PRESSED) Then $iItemState = BitOR($iItemState, $ODS_SELECTED)
	If BitAND($iState, $TBSTATE_INDETERMINATE) Then $iItemState = BitOR($iItemState, $ODS_GRAYED)
	Return $iItemState
EndFunc   ;==>__TB_ItemStateCalc

Func __TB_GetHotIndex($hWnd)
	Local $tRECT
	For $iIndex = 0 To _GUICtrlToolbar_ButtonCount($hWnd) - 1
		$tRECT = _GUICtrlToolbar_GetButtonRectEx($hWnd, _GUICtrlToolbar_IndexToCommand($hWnd, $iIndex))
		If _WinAPI_PtInRect($tRECT, _WinAPI_GetMousePos(True, $hWnd)) Then Return $iIndex
	Next
	Return -1
EndFunc   ;==>__TB_GetHotIndex

Func __TB_GetHotIndexEx($hWnd, $tPOINT)
	Local $tRECT
	For $iIndex = 0 To _GUICtrlToolbar_ButtonCount($hWnd) - 1
		$tRECT = _GUICtrlToolbar_GetButtonRectEx($hWnd, _GUICtrlToolbar_IndexToCommand($hWnd, $iIndex))
		If _WinAPI_PtInRect($tRECT, $tPOINT) Then Return $iIndex
	Next
	Return -1
EndFunc   ;==>__TB_GetHotIndexEx

Func _SetBitmapAlpha($hWnd, $iButton, $hSource, $iAlpha, $iRop = $SRCCOPY)
	Local $tRECT = _GUICtrlToolbar_GetButtonRectEx($hWnd, $iButton)
	;DllStructSetData($tRectF, 'Right', DllStructGetData($tRectF, 'Right') + 1)
	Local $tBitmaps = __TB_GetParam($hWnd, $TB_ID_BITMAPS)
	Local $iWidth = DllStructGetData($tRECT, 3) - DllStructGetData($tRECT, 1)
	Local $iHeight = DllStructGetData($tRECT, 4) - DllStructGetData($tRECT, 2)
	Local $hDC = _WinAPI_GetWindowDC($hWnd)
	Local $hDestDC = _WinAPI_CreateCompatibleDC($hDC)
	Local $hBitmap = __TB_CreateBit($hWnd, _GUICtrlToolbar_CommandToIndex($hWnd, $iButton), $hDC, DllStructGetData($tRECT, 3), $iHeight, _SendMessage($hWnd, $TBN_ISMOUSHOVER) ? 2 : 1, $iRop, Default)
	Local $hDestSv = _WinAPI_SelectObject($hDestDC, $hBitmap)
	Local $hSrcDC = _WinAPI_CreateCompatibleDC($hDC)
	Local $hSrcSv = _WinAPI_SelectObject($hSrcDC, $hSource)
	;ConsoleWrite('_SetBitmapAlpha(' & $hWnd & ', ' & $iButton & ', ' & $hSource & ', ' & $iAlpha & ')' & @CR)
	_WinAPI_AlphaBlend($hDestDC, 0, 0, $iWidth, $iHeight, $hSrcDC, 0, 0, $iWidth, $iHeight, $iAlpha, 0)
	_WinAPI_SelectObject($hDestDC, $hDestSv)
	_WinAPI_DeleteDC($hDestDC)
	_WinAPI_SelectObject($hSrcDC, $hSrcSv)
	_WinAPI_DeleteDC($hSrcDC)
	_WinAPI_DrawBitmap($hDC, DllStructGetData($tRECT, 1), DllStructGetData($tRECT, 2), $hBitmap, $iRop)
	_WinAPI_ReleaseDC($hWnd, $hDC)
	_WinAPI_DeleteObject($hBitmap)
	Return 1
EndFunc   ;==>_SetBitmapAlpha

Func __TB_BitBitEx($hDC, $tRECT, $tRECT_EX, $hBitmap, $iRop = $SRCCOPY)
	Local $hSrcDC = _WinAPI_CreateCompatibleDC($hDC)
	Local $hSrcSv = _WinAPI_SelectObject($hSrcDC, $hBitmap)
	_WinAPI_BitBlt($hDC, 0, 0, DllStructGetData($tRECT_EX, 'Left'), DllStructGetData($tRECT, 'Bottom'), $hSrcDC, 0, 0, $iRop)
	_WinAPI_BitBlt($hDC, DllStructGetData($tRECT_EX, 'Right'), 0, DllStructGetData($tRECT, 'Right'), DllStructGetData($tRECT, 'Bottom'), $hSrcDC, DllStructGetData($tRECT_EX, 'Right'), 0, $iRop)

	_WinAPI_DeleteDC($hSrcDC)
EndFunc   ;==>__TB_BitBitEx

Func _WinAPI_DrawBitmapEx($hDC, $tRECT1, $tRECT2, $hBitmap, $iRop = $SRCCOPY)
	Local $hSrcDC = _WinAPI_CreateCompatibleDC($hDC)
	Local $hSrcSv = _WinAPI_SelectObject($hSrcDC, $hBitmap)
	Local $iX1 = DllStructGetData($tRECT1, 'Left'), $iX2 = DllStructGetData($tRECT1, 'Left') - DllStructGetData($tRECT2, 'Left')
	Local $iWidth = DllStructGetData($tRECT1, 'Right') - DllStructGetData($tRECT1, 'Left')
	_WinAPI_BitBlt($hDC, $iX1, 0, $iWidth, DllStructGetData($tRECT1, 'Bottom'), $hSrcDC, $iX2, 0, $iRop)
	_WinAPI_DeleteDC($hSrcDC)
EndFunc   ;==>_WinAPI_DrawBitmapEx

Func __TB_IsHoverBar()
	Return (_WinAPI_PtInRect(_GUICtrlToolbar_GetRect($AddressBar), _WinAPI_GetMousePos(True, $AddressBar)) Or _WinAPI_PtInRect(_GUICtrlToolbar_GetRect($ButBar), _WinAPI_GetMousePos(True, $ButBar)))
EndFunc   ;==>__TB_IsHoverBar

Func __TB_SetParam($hWnd, $id, $data)
	Local $iIndex = __TB_GetID($hWnd)
	If @error Then Return SetError(1, 0, 0)
	$a__toolsdata[$iIndex][$id] = $data
	Return $data
EndFunc   ;==>__TB_SetParam

Func __TB_GetParam($hWnd, $id)
	Local $iIndex = __TB_GetID($hWnd)
	If @error Or $iIndex > UBound($a__toolsdata) - 1 Then Return SetError(1, 0, 0)
	Return $a__toolsdata[$iIndex][$id]
EndFunc   ;==>__TB_GetParam

Func __TB_GetUpdateRect($hWnd)
	Local $tRECT = DllStructCreate($tagRECT)
	_SendMessage($hWnd, $TBN_GETUPDATERECT, 0, $tRECT, 0, 'wparam', 'struct*')
	Return $tRECT
EndFunc   ;==>__TB_GetUpdateRect

Func __TB_IsButtonRect($hWnd, $CommandID, $tSrcRECT)
	Local $tRECT = _GUICtrlToolbar_GetButtonRectEx($hWnd, $CommandID)
	Local $iStyle = _GUICtrlToolbar_GetButtonStyle($hWnd, $CommandID)
	Local $tPOINT = DllStructCreate($tagPOINT, DllStructGetPtr($tSrcRECT))
	Return _WinAPI_PtInRect($tRECT, $tPOINT)
EndFunc   ;==>__TB_IsButtonRect

Func __TB_GetDropButtonRect($hWnd, $CommandID)
	Local $tRECT = _GUICtrlToolbar_GetButtonRectEx($hWnd, $CommandID)
	DllStructSetData($tRECT, 'Left', DllStructGetData($tRECT, 'Right') - $BTN_DROPWIDTH)
	Return $tRECT
EndFunc   ;==>__TB_GetDropButtonRect

Func __TB_GetID($hWnd)
	Local $iValue = Eval($hWnd)
	Return SetError(@error, 0, Int($iValue))
EndFunc   ;==>__TB_GetID

Func __TB_GetHandle($TB_ID_GUI)
	If $TB_ID_GUI > $a__toolsdata[0][0] Then Return SetError(1, 0, -1)
	Return HWnd($a__toolsdata[$TBX_ID_GUI][0])
EndFunc   ;==>__TB_GetHandle

Func __TB_EraseEnd($hWnd, $bInvalidate = True)
	Local $tBitmaps = DllStructCreate($tagTBBITMAPS, DllStructGetPtr(__TB_GetParam($hWnd, $TB_ID_BITMAPS)))
	Local $hBitmap
	For $iIndex = 1 To 3
		$hBitmap = DllStructGetData($tBitmaps, $iIndex)
		If @error Then ExitLoop
		_WinAPI_DeleteObject($hBitmap)
	Next
	If Not _WinAPI_IsWindowVisible($hWnd) Then Return
	If $bInvalidate Then _WinAPI_InvalidateRect($hWnd, 0, True)
EndFunc   ;==>__TB_EraseEnd

Func _GUICtrlToolbar_ButtonActivate($hWnd, $iCommandID)
	Local $tNMTBHOTITEM = DllStructCreate($tagNMTBHOTITEM)
	DllStructSetData($tNMTBHOTITEM, "hWndFrom", $hWnd)
	DllStructSetData($tNMTBHOTITEM, "Code", $NM_LDOWN)
	DllStructSetData($tNMTBHOTITEM, "idOld", 0)
	DllStructSetData($tNMTBHOTITEM, "idNew", $iCommandID)
	WM_TOOL(0, $WM_NOTIFY, 0, DllStructGetPtr($tNMTBHOTITEM))
	;_SendMessage(_WinAPI_GetAncestor($hWnd, $GA_ROOT), $WM_NOTIFY, 0, $tNMTBHOTITEM, 0, 'wparam', 'struct*')
EndFunc

Func __TB_LoadBitmap($sFile, $sResource = 0, $iCount = 1, $iNumber = 0, $bVertical = False)
	Local $hInstance, $hSource, $bDel = False

	If __TB_BitExists($sFile) Then
		$hSource = $sFile
	Else
		$hInstance = _WinAPI_LoadLibraryEx($sFile, $LOAD_LIBRARY_AS_DATAFILE)
		$hSource = _WinAPI_LoadBitmap($hInstance, $sResource)
		$bDel = True
	EndIf

	Local $tSIZE = _WinAPI_GetBitmapDimension($hSource)
	Local $hDC = _WinAPI_GetDC(0)
	Local $hDestDC = _WinAPI_CreateCompatibleDC($hDC)
	Local $hBitmap = _WinAPI_CreateCompatibleBitmap($hDC, $bVertical ? DllStructGetData($tSIZE, 'X') : DllStructGetData($tSIZE, 'X') / $iCount, $bVertical ? DllStructGetData($tSIZE, 'Y') / $iCount : DllStructGetData($tSIZE, 'Y'))
	Local $hDestSv = _WinAPI_SelectObject($hDestDC, $hBitmap)
	_WinAPI_DrawBitmap($hDestDC, $bVertical ? 0 : -DllStructGetData($tSIZE, 'X') / $iCount * $iNumber, $bVertical ? -DllStructGetData($tSIZE, 'Y') / $iCount * $iNumber : 0, $hSource)
	_WinAPI_ReleaseDC(0, $hDC)
	_WinAPI_SelectObject($hDestDC, $hDestSv)
	If $bDel Then _WinAPI_DeleteObject($hSource)
	_WinAPI_DeleteDC($hDestDC)
	_WinAPI_FreeLibrary($hInstance)
	Return $hBitmap
EndFunc   ;==>__TB_LoadBitmap

Func __TB_BitExists($hBitmap)
	Local $tObj = DllStructCreate($tagBITMAP)
	Local $aRet = DllCall('gdi32.dll', 'int', 'GetObject', 'handle', $hBitmap, 'int', DllStructGetSize($tObj), 'struct*', $tObj)
	If @error Or Not $aRet[0] Then Return SetError(@error + 20, @extended, 0)
	Return 1
EndFunc   ;==>__TB_BitExists

Func __TB_GetLogFont()
	Local $hTheme = _WinAPI_OpenThemeData(0, 'ExplorerBar')
	Local $tFont = _WinAPI_GetThemeSysFont($hTheme, $TMT_ICONTITLEFONT)
	_WinAPI_CloseThemeData($hTheme)
	Return $tFont
EndFunc   ;==>__TB_GetLogFont

Func _TB_SetThemeBackground($hWnd, $sClass, $iPartID, $iStateID, $hDC = 0, $tRECT = 0)
	Local $hDestDC, $tPAINTSTRUCT

	If Not IsDllStruct($tRECT) Then
		$tRECT = _WinAPI_GetWindowRect($hWnd)
		DllStructSetData($tRECT, 'Left', 0)
		DllStructSetData($tRECT, 'Top', 0)
	EndIf

	If Not $hDC Then
		$hDestDC = _WinAPI_BeginPaint($hWnd, $tPAINTSTRUCT)
	Else
		$hDestDC = $hDC
	EndIf

	Local $hTheme = _WinAPI_OpenThemeData(0, $sClass)
	_WinAPI_DrawThemeBackground($hTheme, $iPartID, $iStateID, $hDestDC, $tRECT)
	_WinAPI_CloseThemeData($hTheme)

	If Not $hDC Then _WinAPI_EndPaint($hWnd, $tPAINTSTRUCT)
EndFunc   ;==>_TB_SetThemeBackground

Func _TB_SetThemeDrawText($hWnd, $sClass, $iPartID, $iStateID, $hDC = 0, $sText = '', $tRECT = 0, $iFlags = 0)
	Local $hDestDC, $tPAINTSTRUCT

	If Not IsDllStruct($tRECT) Then
		$tRECT = _WinAPI_GetWindowRect($hWnd)
		DllStructSetData($tRECT, 'Left', 0)
		DllStructSetData($tRECT, 'Top', 0)
	EndIf

	If Not $hDC Then
		$hDestDC = _WinAPI_BeginPaint($hWnd, $tPAINTSTRUCT)
	Else
		$hDestDC = $hDC
	EndIf

	Local $hTheme = _WinAPI_OpenThemeData(0, $sClass)
	_WinAPI_DrawThemeText($hTheme, $iPartID, $iStateID, $hDestDC, $sText, $tRECT, $iFlags)
	_WinAPI_CloseThemeData($hTheme)

	If Not $hDC Then _WinAPI_EndPaint($hWnd, $tPAINTSTRUCT)
EndFunc   ;==>_TB_SetThemeDrawText

Func _TB_RectToRect(ByRef $tSrcRECT, ByRef $tDestRECT)
	For $iIndex = 1 To 4
		DllStructSetData($tDestRECT, $iIndex, DllStructGetData($tSrcRECT, $iIndex))
	Next
EndFunc

Func __TB_DeleteParams()
	For $iIndex = 0 To UBound($a__toolsdata) - 1
		Assign($a__toolsdata[$iIndex][0], 0)
	Next
	Dim $a__toolsdata[0][$imax]
	DllCallbackFree($tool_hDll)
	_MouseSetOnEvent_RI($MOUSE_MOVE_EVENT, "")
	_MouseSetOnEvent_RI($MOUSE_PRIMARYDOWN_EVENT, "")
	_MouseSetOnEvent_RI($MOUSE_PRIMARYUP_EVENT, "")
	If $IngeneringMode Then _MouseSetOnEvent_RI($MOUSE_MOVE_EVENT, "IngeneringModeRun", 0, 3)
EndFunc   ;==>__TB_DeleteParams

Func _GUICtrlToolbar_GetRect($hWnd)
	Local $tRECT = DllStructCreate($tagRECT)
	_SendMessage($hWnd, $TBN_GETWINRECT, 0, $tRECT, 0, 'wparam', 'struct*')
	Return $tRECT
EndFunc   ;==>_GUICtrlToolbar_GetRect


Func _GUICtrlToolbar_GetButtonTimer($hWnd, $idButton)
	Local $tButData = DllStructCreate($tagTBBUTTONDATA)
	DllStructSetData($tButData, 'Button', $idButton)
	If Not _SendMessage($hWnd, $TBN_GETBUTTONTIMER, 0, $tButData, 0, "wparam", "struct*") Then Return SetError(1, 0, 0)
	Return SetError(0, DllStructGetData($tButData, 'Step'), DllStructGetData($tButData, 'Timer'))
EndFunc   ;==>_GUICtrlToolbar_GetButtonTimer

Func _GUICtrlToolbar_SetButtonTimer($hWnd, $idButton, $iTimer, $iStep)
	If $idButton == -1 Then Return 0
	Local $tButData = DllStructCreate($tagTBBUTTONDATA)
	DllStructSetData($tButData, 'hWnd', $hWnd)
	DllStructSetData($tButData, 'Button', $idButton)
	DllStructSetData($tButData, 'Timer', $iTimer)
	DllStructSetData($tButData, 'Step', $iStep)
	Return _SendMessage($hWnd, $TBN_SETBUTTONTIMER, 0, $tButData, 0, 'wparam', 'struct*')
EndFunc   ;==>_GUICtrlToolbar_SetButtonTimer




Func __TB_MetricsDisplay($hWnd)

	Local $tMetrics = DllStructCreate($tagTBMETRICS)
	Local $iMetrics = DllStructGetSize($tMetrics)
	Local $iMask = BitOR($TBMF_PAD, $TBMF_BARPAD, $TBMF_BUTTONSPACING)
	DllStructSetData($tMetrics, "Size", $iMetrics)
	DllStructSetData($tMetrics, "Mask", $iMask)
	_SendMessage($hWnd, $TB_GETMETRICS, 0, $tMetrics, 0, "wparam", "struct*")
	ConsoleWrite('+=========METRICS START========' & @CR)
	__TB_StructureDisplay($tMetrics, $tagTBMETRICS)
	ConsoleWrite('+==========END METRICS=========' & @CR)

EndFunc   ;==>__TB_MetricsDisplay

Func __TB_ButtonDisplay($hWnd, $iCommandID)
	Local $tButton = DllStructCreate($tagTBBUTTON)
	Local $iIndex = _GUICtrlToolbar_CommandToIndex($hWnd, $iCommandID)
	_SendMessage($hWnd, $TB_GETBUTTON, $iIndex, $tButton, 0, "wparam", "struct*")
	ConsoleWrite('+=======BUTTON DATA START======' & @CR)
	__TB_StructureDisplay($tButton, $tagTBBUTTON)
	ConsoleWrite('+==========END BUTTON==== =====' & @CR)
EndFunc   ;==>__TB_ButtonDisplay

Func __TB_InfoDisplay($hWnd, $iCommandID)
	Local $tInfo = DllStructCreate($tagTBBUTTONINFO)
	Local $iButton = DllStructGetSize($tInfo)
	Local $iMask = BitOR($TBIF_IMAGE, $TBIF_TEXT, $TBIF_STATE, $TBIF_STYLE, $TBIF_LPARAM, $TBIF_COMMAND, $TBIF_SIZE)
	DllStructSetData($tInfo, "Size", $iButton)
	DllStructSetData($tInfo, "Mask", $iMask)
	_SendMessage($hWnd, $TB_GETBUTTONINFOW, $iCommandID, $tInfo, 0, "wparam", "struct*")

	ConsoleWrite('+=======INFO BUTTON START======' & @CR)
	ConsoleWrite(@TAB & '<Command ' & $iCommandID & '>' & @CR & @CR)
	__TB_StructureDisplay($tInfo, $tagTBBUTTONINFO)
	ConsoleWrite('+========END INFO BUTTON=======' & @CR)
EndFunc   ;==>__TB_InfoDisplay

Func __TB_RectDisplay($hWnd, $iCommandID)
	Local $tRECT = _GUICtrlToolbar_GetButtonRectEx($hWnd, $iCommandID)
	ConsoleWrite('+=======RECT BUTTON START======' & @CR)
	ConsoleWrite(@TAB & '<Command ' & $iCommandID & '>' & @CR & @CR)
	__TB_StructureDisplay($tRECT, $tagRECT)
	ConsoleWrite('+========END RECT BUTTON=======' & @CR)
EndFunc   ;==>__TB_RectDisplay

Func __TB_FontDisplay($hWnd)
	Local $hFont = _SendMessage($hWnd, $WM_GETFONT)
	Local $tFont = _GetLogFont($hFont)
	__TB_StructureDisplay($tFont, $tagLOGFONT)
EndFunc

Func __TB_StructureDisplay($tData, $sTag)
	Local $aNameData = StringSplit($sTag, ';')
	Local $step = 0
	For $i = 1 To $aNameData[0]
		If StringInStr($aNameData[$i], 'struct') Then
			$step += 1
			ContinueLoop
		EndIf
		ConsoleWrite('-' & @TAB & $aNameData[$i] & ' = ' & DllStructGetData($tData, $i - $step) & @CR)
	Next
	Local $iWidth = DllStructGetData($tData, 'Right') - DllStructGetData($tData, 'Left'), $iHeight = DllStructGetData($tData, 'Bottom') - DllStructGetData($tData, 'Top')
	If @error Then Return 
	ConsoleWrite(@CR & '-' & @TAB & 'Width = ' & $iWidth & @CR & '-' & @TAB & 'Height = ' & $iHeight & @CR & @CR)
EndFunc   ;==>__TB_StructureDisplay

Func __TB_GetLastButton($hWnd)
	Local $iCount = _GUICtrlToolbar_ButtonCount($hWnd), $iCommandID
	For $i = $iCount - 1 To 0 Step -1
		$iCommandID = _GUICtrlToolbar_IndexToCommand($hWnd, $i)
		If Not _GUICtrlToolbar_IsButtonHidden($hWnd, $iCommandID) Then
			Return SetError(0, $i, $iCommandID)
		EndIf
	Next
EndFunc   ;==>__TB_GetLastButton

Func __TB_GetTooButton($hWnd)
	Local $iCount = _GUICtrlToolbar_ButtonCount($hWnd), $iCommandID
	For $i = 1 To $iCount - 1
		$iCommandID = _GUICtrlToolbar_IndexToCommand($hWnd, $i)
		If Not _GUICtrlToolbar_IsButtonHidden($hWnd, $iCommandID) Then
			Return SetError(0, $i, $iCommandID)
		EndIf
	Next
EndFunc   ;==>__TB_GetTooButton

Func __MouseEvent($nCode, $wParam = 0)

	Local Static $bHitTest = False
	Local Static $hWeCursor = 0
	Local Static $bPressed = False
	Local Static $hWndReg = 0
	Local Static $iDPI
	
	Switch $nCode
		Case $WM_DPICHANGED
			$iDPI = Int($wParam)
			Return 0
		Case $MOUSE_PRIMARYDOWN_EVENT
			$bPressed = True
		Case $MOUSE_PRIMARYUP_EVENT

			$bPressed = False
			If $bHitTest Then
				$bHitTest = False
				__TB_SetDefCursors()
				_WinAPI_DestroyCursor($hWeCursor)
				_SendMessage($WorkerW, $WM_PARENTNOTIFY, _WinAPI_MakeLong($WM_LBUTTONUP, 0), 0, 0, 'long')
			EndIf

			If $hWndReg Then
				Local $tPOINT = _WinAPI_GetMousePos(True, $hWndReg)
				_SendMessage($hWndReg, $WM_LBUTTONUP, 0, _WinAPI_MakeLong(DllStructGetData($tPOINT, 'X'), DllStructGetData($tPOINT, 'Y')), 0, 'long')
				$hWndReg = 0
			EndIf

		Case $MOUSE_MOVE_EVENT
			If $IngeneringMode Then Call('IngeneringModeRun', $nCode, 0)
			
			Local $tPOINT = _WinAPI_GetMousePos()
			Local Static $oldHwnd
			Local $hWnd = _WinAPI_WindowFromPoint($tPOINT)
			If $oldHwnd <> $hWnd Then 
				$iDPI = _WinAPI_GetDpiForWindow($hWnd)
				$oldHwnd = $hWnd
			EndIf
			
			Local $scaling = $iDPI / 96
			
			_WinAPI_ScreenToClient($g_hReBar, $tPOINT)
			Local $CX = Ceiling(DllStructGetData($tPOINT, 'X') * $scaling)
			Local $tRECT = _WinAPI_GetWindowRect($g_hReBar)
			If $bPressed And (DllStructGetData($tPOINT, 'X') > 0 And DllStructGetData($tPOINT, 'X') < DllStructGetData($tRECT, 'Right') - DllStructGetData($tRECT, 'Left')) Then
				Return _SendMessage($WorkerW, $WM_MOUSEMOVE, $g_hReBar, _WinAPI_MakeLong($CX, 20), 0, "wparam", "long")
			EndIf
			
			_WinAPI_ClientToScreen($g_hReBar, $tPOINT)
			
			Switch $hWnd
				Case $g_hReBar, $hSeparatorBand
					Local $aHit = _GUICtrlRebar_HitTest($g_hReBar, $CX, 20)
					Switch $aHit[0]
						Case 2
							If Not $bHitTest Then
								$bHitTest = True
								$hWeCursor = _WinAPI_CopyCursor(_WinAPI_LoadCursor(0, $OCR_SIZEWE))
								_WinAPI_SetSystemCursor($hWeCursor, $OCR_NORMAL)

							EndIf
						Case Else
							If $hWnd == $hSeparatorBand Then ContinueCase
							If $bHitTest And Not $bPressed Then
								$bHitTest = False
								__TB_SetDefCursors()
								_WinAPI_DestroyCursor($hWeCursor)
							EndIf
					EndSwitch
				Case Else
					If $bHitTest And Not $bPressed Then
						$bHitTest = False
						__TB_SetDefCursors()
						_WinAPI_DestroyCursor($hWeCursor)
					EndIf
					If _WinAPI_GetAncestor($hWnd, $GA_ROOT) == $GUI_Tool Then
						_WinAPI_ScreenToClient($hWnd, $tPOINT)
						_SendMessage($WorkerW, $WM_MOUSEMOVE, $hWnd, _WinAPI_MakeLong(DllStructGetData($tPOINT, 'X') * $scaling, DllStructGetData($tPOINT, 'Y') * $scaling), 0, "hwnd", "long")
					EndIf
			EndSwitch
		Case Else
			If IsHWnd($nCode) And $bPressed Then $hWndReg = $nCode
	EndSwitch

EndFunc   ;==>__MouseEvent

Func __TB_SetDefCursors()
	Local $aThemeFile = _WinAPI_ExpandEnvironmentStrings(RegRead($_HKEY_CURRENT_USER & '\Software\Microsoft\Windows\CurrentVersion\Themes', 'InstallTheme'))
	Local $INI_DATE = _IniVirtual_Initial(FileRead($aThemeFile))
	_SetSystemCursor($INI_DATE)
EndFunc   ;==>__TB_SetDefCursors

