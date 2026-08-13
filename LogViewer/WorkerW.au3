;WorkerW.au3
#include-once

;Global Const $PRF_CHECKVISIBLE = 0x00000001
;Global Const $PRF_NONCLIENT = 0x00000002
;Global Const $PRF_CLIENT = 0x00000004
;Global Const $PRF_ERASEBKGND = 0x00000008
;Global Const $PRF_CHILDREN = 0x00000010
;Global Const $PRF_OWNED = 0x00000020

Func _WndToolProc($hWnd, $iMsg, $wParam, $lParam, $iID, $pData)
	#forceref $iMsg, $wParam, $lParam
	Local Static $bBeginDrag = 0
	Local Static $iIndent = 0
	Local Static $idNew, $idOld, $dwFlags
	Local Static $bAddressMouseHover = False
	Local Static $bButtonDown = 0
	Local Static $bDropHot = 0
	Local Static $iMenuInit = False, $iDropDown = 0
	Local Static $tBandSize = DllStructCreate($tagTBBANDSIZE)
	Local Static $hTab_CurSel = 0
	Local Static $iOldWidth = 0
	Local Static $NcTest = 0
	Local Static $bDraw = 0
	Local Static $bParentActive = False
	Local Static $bPaintProgress = False
	Local Static $bBeginSizeMove = False
	Local Static $aHidedButtons[0][3]
	Local Static $bHitTest = False
	Local Static $hWeCursor
	Local Static $bRedrawBar = False 
	Local Static $bExtendFrame = False
	Local Static $iDPI = _WinAPI_GetDpiForPrimaryMonitor()
	Local Static $scaling = $iDPI / 96
	Local Static $SearchBandLength
	Local Static $BandLockMoveSize
	Local Static $WndWidth, $WndHeight
	Local Static $iTabCurSel
	Local Static $SysCommand
	Local Static $Maximized = False
	Switch $hWnd
		Case $GUI_Tool
			Switch $iMsg
				Case $WM_CLOSE
					If Not @Compiled Then ConsoleWrite('- $WM_CLOSE ' & WinGetTitle($hWnd) & @CR)
					_WinAPI_ShowWindow($hWnd, @SW_HIDE)
					_SetRectAllFolders($hWnd)
					__TB_ToolBarDestroyMenu($AddressBar)
					_GUICtrlTab_Destroy($ShellTab)
					For $iIndex = 1 To 5
						DllStructSetData($tagTBBANDSIZE, $iIndex, 0)
					Next
					$hTab_CurSel = 0
					$NcTest = 0
					$bDraw = 0
					$iOldWidth = 0
					$iMenuInit = False
					$iDropDown = 0
					_GUICtrlProgress_Destroy($hProgress)
					_GUICtrlRichEdit_Destroy($Search_Box)
				Case $WM_DESTROY
					__TB_DeleteParams()
					_WinAPI_UnregisterClass(_WinAPI_GetClassName($hWnd), _WinAPI_GetModuleHandle(0))
				Case $BWM_SETEXTENDFRAME
					If $wParam Then 
						_WinAPI_DwmExtendFrameIntoClientArea($hWnd, _WinAPI_CreateMargins(0, 0, Round($GUI_FRAME * $scaling), 0))
					Else 
						_WinAPI_DwmExtendFrameIntoClientArea($hWnd, _WinAPI_CreateMargins(0, 0, 0, 0))
					EndIf
					_WinAPI_RedrawWindow($hWnd, 0, 0, BitOR($RDW_INVALIDATE, $RDW_ALLCHILDREN, $RDW_ERASE))
					$bExtendFrame = $wParam
					Return 0
				Case $WM_GETDPISCALEDSIZE
					Local $tSize = DllStructCreate($tagSIZE, $lParam)
					$iDPI = Int($wParam)
					$scaling = $iDPI / 96
					_SendMessage($WorkerW, $WM_SETREDRAW, 0)
					;If $Maximized Then _WinAPI_ShowWindow($hWnd, @SW_RESTORE)
					;_WinAPI_SetWindowPos($hWnd, 0, 0, 0, DllStructGetData($tSize, 1), DllStructGetData($tSize, 2), BitOR($SWP_NOMOVE, $SWP_NOACTIVATE, $SWP_NOZORDER))
					$BandLockMoveSize = True
					Return 0
				Case $WM_DPICHANGED
					$iDPI = _WinAPI_LoWord($wParam)
					$scaling = $iDPI / 96
					;Local $scalling2 = 96 / _WinAPI_GetDpiForSystem()
					Local $tRECT = DllStructCreate($tagRECT, $lParam)
					Local $iWidth = DllStructGetData($tRECT, 3) -DllStructGetData($tRECT, 1)
					Local $iHeight = DllStructGetData($tRECT, 4) -DllStructGetData($tRECT, 2)
					;ConsoleWrite('$WM_DPICHANGED ' & _WinAPI_GetClassName($hWnd) & ' [' & $iWidth & ':' & $iHeight & '] (' & $scaling & ')' & @CR)
					_WinAPI_SetWindowPos($hWnd, 0, DllStructGetData($tRECT, 1), DllStructGetData($tRECT, 2), $iWidth, $iHeight, BitOR($SWP_NOACTIVATE, $SWP_NOZORDER))
					If $bExtendFrame Then _WinAPI_DwmExtendFrameIntoClientArea($hWnd, _WinAPI_CreateMargins(0, 0, Round($GUI_FRAME * $scaling), 0))
					_SendMessage($WorkerW, $WM_SETREDRAW, 1)
					_WinAPI_RedrawWindow($WorkerW, 0, 0, BitOR($RDW_INVALIDATE, $RDW_ALLCHILDREN))
					__RedrawBar($bRedrawBar)
					__MouseEvent($WM_DPICHANGED, _WinAPI_LoWord($wParam))
					$BandLockMoveSize = False
					_WinAPI_UpdateWindow($hWnd)
					Return 0
				Case $WM_SYSCOLORCHANGE
					_GUISetBkColorEx($hWnd, _WinAPI_SwitchColor(_WinAPI_GetSysColor($COLOR_3DFACE)))
					For $iIndex = 0 To _GUICtrlTab_GetItemCount($ShellTab)
						_SendMessage(_GUICtrlTab_GetItemParam($ShellTab, $iIndex), $iMsg, $wParam, $lParam)
					Next
					If $bExtendFrame Then _WinAPI_DwmExtendFrameIntoClientArea($hWnd, _WinAPI_CreateMargins(0, 0, Round($GUI_FRAME * $scaling), 0))
				Case $WM_DWMCOLORIZATIONCOLORCHANGED
					Local $Result = _WinAPI_DefWindowProcW($hWnd, $iMsg, $wParam, $lParam)
					If $bExtendFrame Then _WinAPI_DwmExtendFrameIntoClientArea($hWnd, _WinAPI_CreateMargins(0, 0, Round($GUI_FRAME * $scaling), 0))
					Return $Result
				Case $WM_THEMECHANGED
;~ 					Local $hImage = _GUICtrlToolbar_GetImageList($ButBar)
;~ 					__CreateImageList($hImage, $scaling)
;~ 					__SetActive($scaling, $bParentActive)
				Case $WM_INPUT
					Local $tRIM = DllStructCreate($tagRAWINPUTKEYBOARD)
					_WinAPI_GetRawInputData($lParam, $tRIM, DllStructGetSize($tRIM), $RID_INPUT)
					Local $tKBDLLHOOKSTRUCT = DllStructCreate($tagKBDLLHOOKSTRUCT)
					Switch DllStructGetData($tRIM, 'VKey')
						Case 0x10
							Switch DllStructGetData($tRIM, 'MakeCode')
								Case 0x2A
									DllStructSetData($tRIM, 'VKey', $VK_LSHIFT)
								Case 0x36
									DllStructSetData($tRIM, 'VKey', $VK_RSHIFT)
							EndSwitch
						Case 0x11
							DllStructSetData($tRIM, 'VKey', $VK_LCONTROL) ; $VK_RCONTROL
						Case 0x12
							DllStructSetData($tRIM, 'VKey', $VK_LMENU) ; $VK_RMENU
					EndSwitch
					;If Not @Compiled And DllStructGetData($tRIM, 'Message') Then ConsoleWrite('$WM_INPUT ' & _WinAPI_GetClassName(_WinAPI_GetFocus()) & @TAB & _WinAPI_GetClassName(_WinAPI_GetAncestor(_WinAPI_GetFocus(), $GA_ROOTOWNER)) & @CR)
					DllStructSetData($tKBDLLHOOKSTRUCT, 'vkCode', DllStructGetData($tRIM, 'VKey'))
					Return Call('__HKI_Hook', 0, DllStructGetData($tRIM, 'Message'), DllStructGetPtr($tKBDLLHOOKSTRUCT))
				Case $WM_SIZE ; размер изменился
					$Maximized = False
					Switch $wParam
						Case $SIZE_MAXIMIZED
							$Maximized = True
							ContinueCase
						Case $SIZE_RESTORED
							;_WinAPI_RedrawWindow(HWnd(_GUICtrlTab_GetItemParam($ShellTab, _GUICtrlTab_GetCurSel($ShellTab))), 0, 0, BitOR($RDW_INVALIDATE, $RDW_ALLCHILDREN))
						Case $SIZE_MINIMIZED
							Return 0
					EndSwitch
					$WndWidth = _WinAPI_LoWord($lParam)
					$WndHeight = _WinAPI_HiWord($lParam) - Round($GUI_FRAME * $scaling)
					_WinAPI_MoveWindow($WorkerW, 0, 0, $WndWidth, Round($GUI_FRAME * $scaling), False)
					_WinAPI_SetWindowPos(_GUICtrlTab_GetItemHandly($ShellTab, $iTabCurSel), 0, 0, Round($GUI_FRAME * $scaling), $WndWidth, $WndHeight, BitOR($SWP_NOACTIVATE, $SWP_NOREPOSITION, $SWP_NOZORDER, $SWP_NOREDRAW))
					
				Case $WM_SIZING ; размер изменяется
					
				Case $WM_CAPTURECHANGED ; теряет захват мыши
					
				Case $WM_LBUTTONDOWN, $WM_NCLBUTTONDOWN
					_WinAPI_SetFocus($hWnd)
				Case $WM_NOTIFY, $WM_COMMAND, $WM_MENUCOMMAND, $WM_INITMENU, $WM_MENUSELECT, $WM_UNINITMENUPOPUP, $WM_ENTERMENULOOP, $WM_EXITMENULOOP
					Return _SendMessage($WorkerW, $iMsg, $wParam, $lParam)
				Case $WM_WINDOWPOSCHANGED
					;Local $tPOS = DllStructCreate($tagWINDOWPOS, $lParam)
					;$WndWidth = DllStructGetData($tPOS, 'CX')
					;$WndHeight = DllStructGetData($tPOS, 'CY') -$GUI_FRAME * $scaling
				Case $WM_SYSCOMMAND
					$SysCommand = $wParam
				Case $WM_ENTERSIZEMOVE
					$bBeginSizeMove = True
					Switch $SysCommand
						Case 0xF012, 0xF030, 0xF032, 0xF120
							Return 0
					EndSwitch
					For $iIndex = 0 To _GUICtrlTab_GetItemCount($ShellTab)
						_SendMessage(_GUICtrlTab_GetItemParam($ShellTab, $iIndex), $iMsg)
					Next
				Case $WM_EXITSIZEMOVE
					$bBeginSizeMove = False
					Switch $SysCommand
						Case 0xF012, 0xF030, 0xF032, 0xF120
							Return 0
					EndSwitch
					Local $hWndFrom
					For $iIndex = 0 To _GUICtrlTab_GetItemCount($ShellTab)
						$hWndFrom = _GUICtrlTab_GetItemHandly($ShellTab, $iIndex)
						_WinAPI_SetWindowPos($hWndFrom, 0, 0, Round($GUI_FRAME * $scaling), $WndWidth, $WndHeight, BitOR($SWP_NOACTIVATE, $SWP_NOREPOSITION, $SWP_NOZORDER, $SWP_NOREDRAW))
						_SendMessage($hWndFrom, $iMsg)
					Next
					__RedrawBar($bRedrawBar)
				Case $WM_GETMINMAXINFO
					
				Case $WM_SETCURSOR
					If $bBeginDrag Then Return 1
				Case $WM_NCMOUSELEAVE
					
				Case $WM_NCMOUSEMOVE
					
				Case $WM_NCHITTEST
					If $bBeginDrag Then Return 1
					$NcTest = _WinAPI_DefWindowProcW($hWnd, $iMsg, $wParam, $lParam)
					Return $NcTest
				Case $WM_PARENTNOTIFY
					;ConsoleWrite('$WM_PARENTNOTIFY ' & _WinAPI_LoWord($wParam) & @CR)
;~ 				Case $WM_SETFOCUS
;~ 					Return 1
				Case $WM_IME_SETCONTEXT, $WM_IME_NOTIFY, $WM_IME_NOTIFY, $WM_GETICON
					;Return 0
				Case $WM_ACTIVATE
					Switch _WinAPI_LoWord($wParam)
						Case True
							_WinAPI_SetFocus($hWnd)
							$bParentActive = True
						Case False
							$bParentActive = False
							If $iMenuInit Then _SendMessage($hWnd, $WM_NCACTIVATE, True)
					EndSwitch
					If Not $iMenuInit Then __SetActive($scaling, $bParentActive)
				Case $WM_PRINTCLIENT
					Return 1
				Case $WM_ERASEBKGND
					;If $bExtendFrame Then Return 1
				Case Else
					;ConsoleWrite('0x' & Hex($iMsg, 4) & @TAB & $wParam & @TAB & $lParam & @CR)
			EndSwitch
		Case $WorkerW
			Local Static $bCaptureChanging = False
			Local Static $iX
			Switch $iMsg
				Case $WM_DESTROY
					_WinAPI_UnregisterClass(_WinAPI_GetClassName($hWnd), _WinAPI_GetModuleHandle(0))
				Case $WM_SIZE
					_WinAPI_SetWindowPos($g_hReBar, 0, 0, 0, _WinAPI_LoWord($lParam), _WinAPI_HiWord($lParam), BitOR($SWP_NOACTIVATE, $SWP_NOREPOSITION, $SWP_NOZORDER, $SWP_NOREDRAW))
					;__RedrawBar($bRedrawBar)
					Return 0
				Case $WM_PRINTCLIENT
					If $bExtendFrame Then 
						Local $tRECT = DllStructCreate($tagRECT)
 						_WinAPI_GetClipBox($wParam, $tRECT)
						_WinAPI_PatBlt($wParam, 0, 0, DllStructGetData($tRECT, 3), DllStructGetData($tRECT, 4), $BLACKNESS)
						
					EndIf
					Return 1
				Case $WM_ERASEBKGND
					If $bExtendFrame Then Return 1
				Case $WM_PARENTNOTIFY
					Switch _WinAPI_LoWord($wParam)
						Case $WM_LBUTTONDOWN
							Local $aHit = _GUICtrlRebar_HitTest($g_hReBar, _WinAPI_LoWord($lParam), _WinAPI_HiWord($lParam))
							Switch $aHit[0]
								Case 2
									$bCaptureChanging = True
									If Not $bBeginDrag Then
										$bBeginDrag = True
										Local $tRECT = _GUICtrlRebar_GetBandRectEx($g_hReBar, 3)
										$iIndent = DllStructGetData($tRECT, 'Left') - _WinAPI_LoWord($lParam)
										Local $tCHILDSIZE = DllStructCreate($tagNMREBARCHILDSIZE)
										DllStructSetData($tCHILDSIZE, "hWndFrom", $g_hReBar)
										DllStructSetData($tCHILDSIZE, "Code", $RBN_BEGINDRAG)
										_SendMessage($WorkerW, $WM_NOTIFY, 0, $tCHILDSIZE, 0, "wparam", "struct*")
									EndIf
							EndSwitch
							Return 1
						Case $WM_LBUTTONUP
							$bCaptureChanging = False
							If $bBeginDrag Then
								$bBeginDrag = False
								Local $tCHILDSIZE = DllStructCreate($tagNMREBARCHILDSIZE)
								DllStructSetData($tCHILDSIZE, "hWndFrom", $g_hReBar)
								DllStructSetData($tCHILDSIZE, "Code", $RBN_ENDDRAG)
								_SendMessage($WorkerW, $WM_NOTIFY, 0, $tCHILDSIZE, 0, "wparam", "struct*")
							EndIf
					EndSwitch
				Case $WM_MOUSELEAVE
					$bDropHot = 0
				Case $WM_MOUSEMOVE
					$wParam = HWnd($wParam)
					;ConsoleWrite('$WM_MOUSEMOVE ' & _WinAPI_GetClassName($wParam) & @TAB & $wParam & @TAB & $AddressBar & @CR)
					Switch $wParam
						Case $g_hReBar
							If $bCaptureChanging Then
								_SendMessage($hWnd, $WM_HSCROLL, (($iX > _WinAPI_LoWord($lParam)) ? -1 : (_WinAPI_LoWord($lParam) > $iX) ? 1 : 0), $lParam, 0, "int64", "long")
								$iX = _WinAPI_LoWord($lParam)
							EndIf
					EndSwitch
					Return 0
				Case $WM_HSCROLL
					If Not $wParam Then Return 0
					Local $tRECT = _GUICtrlRebar_GetBandRectEx($g_hReBar, 3)
					DllStructSetData($tRECT, 'Left', _WinAPI_LoWord($lParam) + $iIndent)
					Local $aPos = _WinAPI_GetPosFromRect($tRECT)
					If @error Then Return 0
					If $bPaintProgress And $bRedrawBar Then Return 0
					_SendMessage($g_hReBar, $WM_SETREDRAW, 0)
					_GUICtrlRebar_SetBandLength($g_hReBar, 3, $aPos[2])
					_SendMessage($g_hReBar, $WM_SETREDRAW, 1)
					
					Return 0
				Case $WM_POSTDROPDOWN
					Local $tNMTBHOTITEM = DllStructCreate($tagNMTBHOTITEM)
					DllStructSetData($tNMTBHOTITEM, "hWndFrom", $AddressBar)
					DllStructSetData($tNMTBHOTITEM, "Code", $TBN_DROPDOWN)
					DllStructSetData($tNMTBHOTITEM, "idNew", $idNew)
					DllStructSetData($tNMTBHOTITEM, "idOld", $idOld)
					DllStructSetData($tNMTBHOTITEM, "dwFlags", $dwFlags)
					Return _SendMessage($hWnd, $WM_NOTIFY, $wParam, $tNMTBHOTITEM, 0, 'wparam', 'struct*')
				Case $WM_NOTIFY
					
					Local $tNMHDR = DllStructCreate($tagNMHDR, $lParam)
					Local $hWndFrom = HWnd(DllStructGetData($tNMHDR, "hWndFrom"))
					Switch $hWndFrom
						Case $Search_Box
							Switch DllStructGetData($tNMHDR, "Code")
								Case $EN_CHANGE
									
									ContinueCase
								Case $EN_SELCHANGE
									_WinAPI_SetWindowText($DirectUIHWND, __TB_GetVisibleTextOfLine($hWndFrom))
							EndSwitch
						Case $ShellTab
							
							If Not $hTab_CurSel Then $hTab_CurSel = HWnd(_GUICtrlTab_GetItemParam($hWndFrom, 0))
							
							Switch DllStructGetData($tNMHDR, "Code")
								Case $TCN_SELCHANGE
									_WinAPI_ShowWindow($hTab_CurSel, @SW_HIDE)
									$iTabCurSel = _GUICtrlTab_GetCurSel($hWndFrom)
									$hTab_CurSel = HWnd(_GUICtrlTab_GetItemParam($hWndFrom, $iTabCurSel))
									WinSetTitle($hWndFrom, '', _GUICtrlTab_GetItemText($hWndFrom, $iTabCurSel))
									WinSetTitle(_WinAPI_GetParent($hWndFrom), '', _GUICtrlTab_GetItemText($hWndFrom, $iTabCurSel))
									_WinAPI_ShowWindow($hTab_CurSel)
									__SB_SetDefaultText($DirectUIHWND, StringReplace(_WinAPI_LoadIndirectString('@explorerframe.dll,-13830'), '%1', _GUICtrlTab_GetItemText($hWndFrom, $iTabCurSel)))
							EndSwitch
							
						Case $g_hReBar
							Switch DllStructGetData($tNMHDR, "Code")
								Case $RBN_BEGINDRAG
									_WinAPI_SetFocus($g_hReBar)
									_WinAPI_EnableWindow($Search_Box, 0)
									Local $HwndFrom = _GUICtrlTab_GetItemParam($ShellTab, $iTabCurSel)
									_SendMessage($HwndFrom, $WM_BEGINDRAG, 0, $bBeginDrag)
								Case $RBN_ENDDRAG
									$bBeginDrag = False
									$bCaptureChanging = False
									_WinAPI_EnableWindow($Search_Box, 1)
									__RedrawBar($bRedrawBar)
									Local $HwndFrom = _GUICtrlTab_GetItemParam($ShellTab, $iTabCurSel)
									_SendMessage($HwndFrom, $WM_BEGINDRAG, 0, $bBeginDrag)
									__SetBandLength($SearchBandLength / $scaling)
								Case $RBN_CHILDSIZE
									If $BandLockMoveSize Then Return 
									Local $tCHILDSIZE = DllStructCreate($tagNMREBARCHILDSIZE, $lParam)
									Local $iWidth = DllStructGetData($tCHILDSIZE, "CRight") - DllStructGetData($tCHILDSIZE, "CLeft")
									
									Local $iItem, $hMenuButton
									Switch DllStructGetData($tCHILDSIZE, "uBand")
										Case 1
											Local $hMenu = Ptr(_GUICtrlToolbar_GetButtonParam($AddressBar, 101))
											If Not $hMenu Then Return 0
											Local $aToolSize = _GUICtrlToolbar_GetMaxSize($AddressBar)
											Local $aButSize = _GUICtrlToolbar_GetMaxSize($ButBar)
											Local $iTooID = __TB_GetTooButton($AddressBar) ; Вторая видимая кнопка слева
											Local $tRECT, $iMenuItem
											If $iWidth < $aToolSize[0] + 74 + $iBarIndent + 5 Then
												$tRECT = _GUICtrlToolbar_GetButtonRectEx($AddressBar, $iTooID)
												_GUICtrlToolbar_HideButton($AddressBar, $iTooID)
												If Not _GuiCtrlDropMenu_GetHeaderItemCount($hMenu) Then _GuiCtrlDropMenu_AddHeaderMenuItem($hMenu) ; Разделитель
												$iItem = _GuiCtrlDropMenu_GetHeaderItemCount($hMenu) - 1
												$hMenuButton = Ptr(_GUICtrlToolbar_GetButtonParam($AddressBar, $iTooID))
												$iMenuItem = _GuiCtrlDropMenu_InsertHeaderMenuItem($hMenu, $iItem, _GUICtrlToolbar_GetButtonText($AddressBar, $iTooID), _GuiCtrlDropMenu_GetHeaderItemBmp($hMenuButton, 0), $iTooID)
												_ArrayAdd($aHidedButtons, $iTooID & '|' & $iMenuItem & '|' & DllStructGetData($tRECT, 3) -DllStructGetData($tRECT, 1))
											Else 
												For $i = UBound($aHidedButtons) - 1 To 0 Step -1
													If Not ($iWidth > ($aToolSize[0] + $aHidedButtons[$i][2] + 74 + $iBarIndent + 5)) Then ExitLoop
													_GUICtrlToolbar_HideButton($AddressBar, $aHidedButtons[$i][0], False)
													_GuiCtrlDropMenu_DeleteHeaderMenu($hMenu, $aHidedButtons[$i][1])
													If _GuiCtrlDropMenu_GetHeaderItemCount($hMenu) == 1 Then _GuiCtrlDropMenu_DeleteHeaderMenu($hMenu, 0)
													_ArrayDelete($aHidedButtons, $i)
													ExitLoop
												Next
											EndIf
										Case 3
											$SearchBandLength = $iWidth
									EndSwitch
							EndSwitch
						Case $TravelBar, $AddressBar, $ButBar
							
							Local $tNMTBHOTITEM = DllStructCreate($tagNMTBHOTITEM, $lParam)
							Local Static $tButton = DllStructCreate($tagTBBUTTON)
							
							Switch DllStructGetData($tNMHDR, "Code")
								Case $TBN_HOTITEMCHANGE
									If $bBeginDrag Then Return 1
									If $idNew == DllStructGetData($tNMTBHOTITEM, 'idNew') And $idOld == DllStructGetData($tNMTBHOTITEM, 'idOld') And $dwFlags == DllStructGetData($tNMTBHOTITEM, "dwFlags") Then Return 1
									$idNew = DllStructGetData($tNMTBHOTITEM, 'idNew')
									$idOld = DllStructGetData($tNMTBHOTITEM, 'idOld')
									$dwFlags = DllStructGetData($tNMTBHOTITEM, "dwFlags")
									_SendMessage($hWndFrom, $TB_GETBUTTON, _GUICtrlToolbar_CommandToIndex($hWndFrom, $idNew), $tButton, 0, "wparam", "struct*")
									
									$bButtonDown = 0
									
									If BitAND($dwFlags, $HICF_LEAVING) = $HICF_LEAVING Then
										
									Else
										If $hWndFrom == $AddressBar And Not $bPaintProgress And Not $iMenuInit And _WinAPI_GetFocus() <> $hBreadcrumb_Parent Then _WinAPI_SetFocus($hBreadcrumb_Parent)
										
										If $iMenuInit Then 
											If BitAND(DllStructGetData($tButton, 'Style'), $BTNS_DROPDOWN) Then 
												If $idNew And $iDropDown <> $idNew Then 
													Local $hMenu = Ptr(_GUICtrlToolbar_GetButtonParam($hWndFrom, $iDropDown))
													Local $tDROPMENUINFO = DllStructCreate($tagDROPMENUINFO, $hMenu)
													Local $hDropDown = DllStructGetData($tDROPMENUINFO, 'hDropDown')
													If _WinAPI_IsWindow($hDropDown) Then 
														If $hMenu == __DM_GetID($hDropDown) Then 
															_SendMessage($hDropDown, $WM_CLOSE)
															$iMenuInit = True
														EndIf
													EndIf
												EndIf
											EndIf
										EndIf
									EndIf	
								Case $TBN_DROPDOWN
									If $bPaintProgress Then Return 0
									
									If $idNew And ($idNew == $iDropDown) Then
										$iDropDown = 0
									Else
										
										$iMenuInit = True
										$iDropDown = $idNew
										
										Local $aRet = _GetToolbarButtonScreenPos($hBreadcrumb_Parent, $hWndFrom, $idNew)
										If Not IsArray($aRet) Then Dim $aRet[2] = [-1, -1]
										_GUICtrlToolbar_PressButton($hWndFrom, $idNew)
										
										Local $TrakCancel = _GuiCtrlDropMenu_TrackPopupMenu(_GUICtrlToolbar_GetButtonParam($hWndFrom, $iDropDown), $aRet[0] - $BTN_DROPWIDTH, $aRet[1], 1, 1, 2)
										
										If $idNew <> $iDropDown Then 
											AdlibRegister(__PostDropDown, 100)
										EndIf
									EndIf
									Return 0
								Case $NM_LDOWN
									If Not $idNew Or $iMenuInit Or $bPaintProgress Then Return $GUI_RUNDEFMSG
								Case -2
;~ 									If Not $bButtonDown Or $bButtonDown == $iDropDown Then Return $GUI_RUNDEFMSG
;~ 									$bButtonDown = 0
;~ 									If $hWndFrom == $AddressBar Then _WinAPI_InvalidateRect($hWndFrom, _GUICtrlToolbar_GetButtonRectEx($hWndFrom, $idNew), False)
								Case $NM_CUSTOMDRAW
									;Return $CDRF_DODEFAULT
									Local $tNMTBCUSTOMDRAW = DllStructCreate($tagNMTBCUSTOMDRAW, $lParam)
									Local $iDrawStage = DllStructGetData($tNMTBCUSTOMDRAW, "DrawStage")
									Local $hWndFrom = DllStructGetData($tNMTBCUSTOMDRAW, "hWndFrom")
									Local $hDC = DllStructGetData($tNMTBCUSTOMDRAW, "HDC")
									Local $ItemSpec = DllStructGetData($tNMTBCUSTOMDRAW, "ItemSpec")
									Local $ItemState = DllStructGetData($tNMTBCUSTOMDRAW, "ItemState")
									Local $tRECT = DllStructCreate($tagRECT, DllStructGetPtr($tNMTBCUSTOMDRAW, "Rect"))
									
									Switch $iDrawStage
										Case $CDDS_PREERASE
											;Return $CDRF_DODEFAULT
											If $hWndFrom == $TravelBar Then Return $CDRF_DODEFAULT
											
											Local $hDC = _WinAPI_GetWindowDC($hProgress)
											Local $tRECTA = _WinAPI_GetWindowRect($AddressBar)
											__TB_RectScreenToClient($hProgress, $tRECTA)
											_WinAPI_ExcludeClipRect($hDC, $tRECTA)
											Local $tRECTB = _WinAPI_GetWindowRect($ButBar)
											__TB_RectScreenToClient($hProgress, $tRECTB)
											_WinAPI_ExcludeClipRect($hDC, $tRECTB)
											
											_SendMessage($hProgress, ($bPaintProgress ? $WM_PRINTCLIENT : $WM_ERASEBKGND), $hDC, 0)
											
											If Not $bPaintProgress And ($idNew Or $idOld) Then 
												$hDC = DllStructGetData($tNMTBCUSTOMDRAW, "HDC")
												_WinAPI_DrawThemeParentBackground($hAddress_Band_Root, $hDC)
												Local $hTheme = _WinAPI_OpenThemeData($hAddress_Band_Root, 'AddressBand')
												
												Local $tCLIENT = _WinAPI_GetClientRect($hProgress)
												__TB_RectClientToScreen($hProgress, $tCLIENT)
												__TB_RectScreenToClient($hWndFrom, $tCLIENT)
												
												Local $iBarState = $bAddressMouseHover ? 2 : 1
												_WinAPI_DrawThemeBackground($hTheme, 1, $iBarState, $hDC, $tCLIENT, $tRECT)
												
												_WinAPI_CloseThemeData($hTheme)
												Return $CDRF_SKIPDEFAULT
											EndIf
											
										Case $CDDS_PREPAINT
											If $hWndFrom == $TravelBar Or $hWndFrom == $AddressBar Then
												Return $CDRF_NOTIFYITEMDRAW
											EndIf
										Case $CDDS_ITEMPREPAINT
											If $hWndFrom == $AddressBar And $ItemSpec <> 101 Then Return $CDRF_DODEFAULT
											Local $tInfo = DllStructCreate($tagTBBUTTONINFO)
											Local $iMask = BitOR($TBIF_IMAGE, $TBIF_TEXT, $TBIF_STATE, $TBIF_STYLE, $TBIF_LPARAM, $TBIF_COMMAND, $TBIF_SIZE)
											DllStructSetData($tInfo, "Size", DllStructGetSize($tInfo))
											DllStructSetData($tInfo, "Mask", $iMask)
											_SendMessage($hWndFrom, $TB_GETBUTTONINFOW, $ItemSpec, $tInfo, 0, "wparam", "struct*")
											
											Local $tRECTA = _WinAPI_CopyRect($tRECT)
											Local $tCLIP = _WinAPI_CopyRect($tRECT)
											
											If BitAND(DllStructGetData($tInfo, 'Style'), $BTNS_DROPDOWN) Then DllStructSetData($tRECTA, 'Left', DllStructGetData($tRECT, 'Right') - $BTN_DROPWIDTH * $scaling)
											
											Local $tRECTW = DllStructCreate($tagRECT)
											DllStructSetData($tRECTW, 1, DllStructGetData($tRECT, 1) + DllStructGetData($tNMTBCUSTOMDRAW, "iListGap"))
											DllStructSetData($tRECTW, 2, DllStructGetData($tRECT, 2) + (DllStructGetData($tRECT, 4) - DllStructGetData($tNMTBCUSTOMDRAW, "rcText", 4)) / 2)
											DllStructSetData($tRECTW, 3, DllStructGetData($tRECTW, 1) + DllStructGetData($tNMTBCUSTOMDRAW, "rcText", 3))
											DllStructSetData($tRECTW, 4, DllStructGetData($tNMTBCUSTOMDRAW, "rcText", 4) + DllStructGetData($tRECTW, 2))
											
											Local $hTheme, $iPart = 1, $sText = ''
											Local $iIndex = _GUICtrlToolbar_CommandToIndex($hWndFrom, $ItemSpec)
											
											If $ItemSpec == $iDropDown Or $ItemSpec == $bButtonDown Then $ItemState = $ODS_SELECTED
											
											Local $iStateID = $bBeginDrag ? 1 : __TB_GetStateID($ItemState)
											
											Switch $hWndFrom
												Case $TravelBar
													$iPart = $iIndex + 1
													$hTheme = _WinAPI_OpenThemeData($TravelBar, 'Navigation') ; DarkMode::Navigation
												Case $AddressBar
													$hTheme = _WinAPI_OpenThemeData($hWndFrom, 'Toolbar')
													$sText = _GUICtrlToolbar_GetButtonText($hWndFrom, $ItemSpec)
											EndSwitch
											
											If DllStructGetData($tInfo, 'Image') > -1 Then
												Local $hImage = _GUICtrlToolbar_GetImageList($hWndFrom)
												Local $tImage = _GUIImageList_GetImageInfoEx($hImage, DllStructGetData($tInfo, 'Image'))
												Local $iXIndent = $scaling + ($hWndFrom == $AddressBar And BitAND($ItemState, $ODS_SELECTED) ? $scaling : 0)
												Local $iYIndent = ((DllStructGetData($tRECT, 'Bottom') - DllStructGetData($tRECT, 'Top')) - (DllStructGetData($tImage, 'Bottom') - DllStructGetData($tImage, 'Top'))) / 2 + ($hWndFrom == $AddressBar And BitAND($ItemState, $ODS_SELECTED) ? $scaling : 0)
												_GUIImageList_Draw($hImage, DllStructGetData($tInfo, 'Image'), $hDC, DllStructGetData($tRECT, 'Left') + $iXIndent, DllStructGetData($tRECT, 'Top') + $iYIndent, 1)
												;If BitAND(DllStructGetData($tInfo, 'Style'), $BTNS_DROPDOWN) And $iStateID == 2 Then _WinAPI_DrawThemeBackground($hTheme, $iPart, 2, $hDC, $tRECTA)
											Else
												Select
													Case BitAND(DllStructGetData($tInfo, 'Style'), $BTNS_DROPDOWN)
														DllStructSetData($tCLIP, 'Right', DllStructGetData($tCLIP, 'Right') - $BTN_DROPWIDTH * $scaling)
														If $iStateID == 2 Then
															_WinAPI_DrawThemeBackground($hTheme, $iPart, 2, $hDC, $tRECTA)
														EndIf
														ContinueCase
													Case Else
														_WinAPI_DrawThemeBackground($hTheme, $iPart, $iStateID, $hDC, $tRECT, $tCLIP)
												EndSelect
											EndIf
											
											If $sText Then
												Local $hFont = _SendMessage($hWndFrom, $WM_GETFONT)
												_WinAPI_SetBkMode($hDC, $TRANSPARENT)
												_WinAPI_SelectObject($hDC, $hFont)
												Local $tDTTOPTS = DllStructCreate($tagDTTOPTS)
												DllStructSetData($tDTTOPTS, 'Size', DllStructGetSize($tDTTOPTS))
												DllStructSetData($tDTTOPTS, 'Flags', $DTT_COMPOSITED)
												If $ItemState == $ODS_SELECTED Then _WinAPI_OffsetRect($tRECTW, $scaling, $scaling)
												_WinAPI_DrawThemeTextEx($hTheme, 1, $iStateID, $hDC, $sText, $tRECTW, BitOR($DT_LEFT, $DT_VCENTER, $DT_SINGLELINE, $DT_WORD_ELLIPSIS), $tDTTOPTS)
											EndIf
											
											Local $tPOINT = _WinAPI_GetMousePos(True, $hWndFrom)
											If _WinAPI_PtInRect($tRECT, $tPOINT) Then
												If Not $bDropHot And _WinAPI_PtInRect($tRECTA, $tPOINT) Then
													$bDropHot = $idNew
												EndIf
												If $bDropHot And Not _WinAPI_PtInRect($tRECTA, $tPOINT) Then
													$bDropHot = 0
												EndIf
											EndIf
											
											If BitAND(DllStructGetData($tInfo, 'Style'), $BTNS_DROPDOWN) Then
												If $ItemSpec == 101 And UBound($aHidedButtons) Then
;~ 													Local $hMemDC = _WinAPI_CreateCompatibleDC($hDC)
;~ 													Local $hSource = _WinAPI_CreateDIB(15, -33)
;~ 													Local $hSrcSv = _WinAPI_SelectObject($hMemDC, $hSource)
;~ 													_WinAPI_DrawThemeParentBackground($hWndFrom, $hMemDC)
													Local $h_Theme = _WinAPI_OpenThemeDataForDpi($hBreadcrumb_Parent, 'BreadcrumbBar', $iDPI)
													_WinAPI_DrawThemeBackground($h_Theme, 1, __TB_GetStateID($ItemState, $bDropHot == $ItemSpec ? 2 : 7), $hDC, $tRECTA)
													_WinAPI_CloseThemeData($h_Theme)
;~ 													Local $X = DllStructGetData($tRECTA, 1), $Y = DllStructGetData($tRECTA, 2)
;~ 													Local $W = DllStructGetData($tRECTA, 3) -$X, $H = DllStructGetData($tRECTA, 4) -$Y
;~ 													_WinAPI_StretchBlt($hDC, $X, $Y, $W, $H, $hMemDC, 0, 0, 15, 33, $SRCCOPY)
;~ 													_WinAPI_DeleteDC($hMemDC)
;~ 													_WinAPI_DeleteObject($hSource)
												Else
													_WinAPI_DrawThemeBackground($hTheme, 4, __TB_GetStateID($ItemState, $bDropHot == $ItemSpec ? 2 : 7), $hDC, $tRECTA)
												EndIf
											EndIf
											
											_WinAPI_CloseThemeData($hTheme)
											
											Return BitOR($CDRF_SKIPPOSTPAINT, $CDRF_SKIPDEFAULT) ;$TBCDRF_BLENDICON $CDRF_DOERASE 
									EndSwitch
									Return $GUI_RUNDEFMSG
								Case Else
									;ConsoleWrite('NOTIFY ' & DllStructGetData($tNMHDR, "Code") & @CR)
							EndSwitch
					EndSwitch
					Return Call('WM_TOOL', $hWnd, $iMsg, $wParam, $lParam)
				Case $WM_COMMAND
					Local $nID = _WinAPI_LoWord($wParam)
					Local $iCode = _WinAPI_HiWord($wParam)
					ConsoleWrite('!$WM_COMMAND 0x' & Hex($wParam, 8) & @TAB & $nID & @TAB & HWnd($lParam) & @CR)
					Switch $lParam
						Case $AddressBar
							Switch $wParam
								Case $CBN_DROPDOWN
									ConsoleWrite('$CBN_DROPDOWN' & @CR)
							EndSwitch
						Case $ButBar
							Switch $wParam
								Case 1500
									Switch $bPaintProgress
										Case True ; Отмена загрузки
											Switch _GUICtrlTab_GetCurSel($ShellTab)
												Case 0
													
												Case 1
													
												Case 2
													$bGUI_CLOSE = True
											EndSwitch
											_SendMessage($hProgress, $PBM_SETPOS, _SendMessage($hProgress, $PBM_GETRANGE))
											_SendMessage($hProgress, $PBM_SETPOS, 0, 0)
											_WinAPI_SetWindowLong($hProgress, $GWL_STYLE, 0x52000000)
										Case False ; Обновить / перезагрузить
											Switch _GUICtrlTab_GetCurSel($ShellTab)
												Case 0
													
												Case 1
													_WinAPI_DestroyWindow($gui0)
													_WinAPI_PostMessage($hGUI, $SET_COMMAND_CREATE, 2, $iGUI)
												Case 2
													Local $iIndex = $iTabCurSel
													If $iTabCurSel > 0 And Not $tCurentModify.oldnewexplorer Then $iIndex += 1
													If $iTabCurSel > 5 And Not $tCurentModify.configcontextmenu Then $iIndex += 1
													Switch $iIndex
														Case 0 ; Общие настройки
															
														Case 1 ; Вкладка стилизации проводника
															
														Case 2 ; 'Горячие клавиши'
															
														Case 3 ; Цвета текущей темы
															_TB_ProgressSetStep(3)
															_ColorDataListCreate()
															SetButtonStat()
															_SendMessage($hProgress, $PBM_SETPOS, 0)
														Case 4 ; 'Включенные процессы'
															_ListViewCreate()
														Case 5 ; редактор контекстных меню
															GUICtrlSendToDummy($REG_KEY_UPDATE)
														Case 6 ; 'Установить шрифт'
															
													EndSwitch
											EndSwitch
									EndSwitch
							EndSwitch
						Case 0
							Switch $wParam
								Case 200 To 400
									_GUICtrlToolbar_ButtonActivate($AddressBar, $wParam)
								Case $iThemeCollectionID
									_GUICtrlToolbar_ButtonActivate($AddressBar, 200)
								Case $iWallPappersID
									_WinAPI_ShellExecute('shell:::{ED834ED6-4B5A-4bfe-8F11-A626DCB6A921} -Microsoft.Personalization\pageWallpaper', '', '', 'explore')
								Case $iColorsViewID
									_GUICtrlToolbar_ButtonActivate($AddressBar, 300)
								Case $iToolsID
									_GUICtrlToolbar_ButtonActivate($AddressBar, 400)
								Case $iHelpMeID
									Run('hh.exe ' & @ScriptDir & '\Resources\Language\' & $UserLocale & '\' & $UserLocale & '.chm::/page_1.html')
								Case $iAboutID
									If Not $GuiAbout Or Not WinExists($GuiAbout) Then
										_About()
										_CheckUpdate()
									EndIf
								Case $iColorsCompositionID To $iSystemSettingID
									_GUICtrlToolbar_ButtonActivate($AddressBar, 300)
									If _GUICtrlTab_GetCurSel($g_hTab) == $wParam - $iColorsCompositionID Then Return $GUI_RUNDEFMSG
									_GUICtrlTab_ActivateTab($g_hTab, $wParam - $iColorsCompositionID)
								Case $iGeneralToolsID To $iFotnSettingID
									_GUICtrlToolbar_ButtonActivate($AddressBar, 400)
									
									Local $iIndex = $wParam - $iGeneralToolsID
									If $iIndex > 5 And Not $tCurentModify.configcontextmenu Then $iIndex -= 1
									If $iIndex > 0 And Not $tCurentModify.oldnewexplorer Then $iIndex -= 1
									If _GUICtrlTab_GetCurSel($hTab10) == $iIndex Then Return $GUI_RUNDEFMSG
									_GUICtrlTab_ActivateTab($hTab10, $iIndex)
							EndSwitch
					EndSwitch
					Return Call('GuiMessageProc', $hWnd, $iMsg, $wParam, $lParam)
				Case $WM_ACTIVATEAPP
					Return 0
				Case $WM_MOUSEACTIVATE
					_WinAPI_PostMessage($hGUI, $MOUSEACTIVATE_EVENT, 0, 0)
					Return 1
				Case $WM_SETCURSOR
					Return 0
				Case $WM_ENTERMENULOOP
					;ConsoleWrite('$WM_ENTERMENULOOP ' & $wParam & @TAB & $lParam & @CR)
					
					;Return 0
				Case $WM_EXITMENULOOP
					;ConsoleWrite('$WM_EXITMENULOOP ' & $wParam & @TAB & $lParam & @CR)
					
					;Return 0
				Case $WM_INITMENU
					;ConsoleWrite('$WM_INITMENU ' & Ptr($wParam) & @CR)
					If _GuiCtrlDropMenu_IsMenu(Ptr($wParam)) Then 
						Local $HwndFrom = _GUICtrlTab_GetItemParam($ShellTab, $iTabCurSel)
						Local $hChild = _SendMessage($HwndFrom, $WM_GETPARENT)
						_SendMessage($hChild, $WM_SETREDRAW, 0)
					EndIf
					
					;Return 0
				Case $WM_MENUSELECT
					;ConsoleWrite('$WM_MENUSELECT ' & _WinAPI_LoWord($wParam) & @TAB & '0x' & Hex(_WinAPI_HiWord($wParam), 8) & @TAB & ptr($lParam) & @CR)
				Case $WM_UNINITMENUPOPUP
					;ConsoleWrite('$WM_UNINITMENUPOPUP ' & Ptr($wParam) & @CR)
					
					If _GuiCtrlDropMenu_IsMenu(Ptr($wParam)) Then 
						Local $HwndFrom = _GUICtrlTab_GetItemParam($ShellTab, $iTabCurSel)
						Local $hChild = _SendMessage($HwndFrom, $WM_GETPARENT)
						_SendMessage($hChild, $WM_SETREDRAW, 1)
					EndIf
					
					If $iDropDown == _GuiCtrlDropMenu_GetMenuData($wParam) Then
						Local $iItem = $iDropDown
						If Not ($idNew == $iDropDown And $bDropHot) Then 
							$iDropDown = 0
						EndIf
						$iMenuInit = False
						_GUICtrlToolbar_PressButton($AddressBar, $iItem, False)
						;_WinAPI_InvalidateRect($AddressBar, _GUICtrlToolbar_GetButtonRectEx($AddressBar, $iItem), False)
					EndIf
					Return 0
				Case $WM_MENUCOMMAND
					ConsoleWrite('$WM_MENUCOMMAND ' & $wParam & @TAB & Ptr($lParam) & @CR)
					
			EndSwitch
		Case $hProgress
			Local Static $iWidth, $iHeight, $hDIB, $clientRect
			Switch $iMsg
				Case $WM_DESTROY
					_WinAPI_DeleteObject($hDIB)
					_WinAPI_RemoveWindowSubclass($hWnd, $pData, $iID)
				Case $WM_ACTIVATEAPP
					Return 0
				Case $WM_MOUSEACTIVATE
					Return 0
				Case $WM_NCHITTEST
					Return 1
				Case $WM_SETCURSOR
					Return 1
				Case $WM_TIMER
					if $bRedrawBar Then Return 0
				Case $PBM_SETMARQUEE
					If $wParam Then _Timer_SetTimer($hWnd, $lParam, "")
				Case $PBM_SETPOS
					Select
						Case $wParam And Not $bPaintProgress
							$bPaintProgress = True
							$bAddressMouseHover = False
							_GUICtrlToolbar_HideButton($ButBar, 1400)
							_GUICtrlToolbar_SetIndent($ButBar, (5 + 34) * $scaling)
							_GUICtrlToolbar_SetButtonBitMap($ButBar, 1500, 2)
						Case Not $wParam And $bPaintProgress
							$bPaintProgress = False
							_GUICtrlToolbar_SetIndent($ButBar, 5 * $scaling)
							_GUICtrlToolbar_HideButton($ButBar, 1400, False)
							_GUICtrlToolbar_SetButtonInfo($ButBar, 1400, 0, $TBSTATE_ENABLED, $BTNS_AUTOSIZE, 34 * $scaling)
							_GUICtrlToolbar_SetButtonInfo($ButBar, 1500, 1, $TBSTATE_ENABLED, $BTNS_AUTOSIZE, 34 * $scaling)
							_GUICtrlToolbar_SetButtonSize($ButBar, 29 * $scaling, 29 * $scaling)
							__RedrawBar($bRedrawBar)
					EndSelect
				Case $WM_PRINTCLIENT
					If Not $bPaintProgress Then Return 0
					Local $tRECT = DllStructCreate($tagRECT)
 					_WinAPI_GetClipBox($wParam, $tRECT)
					
					If Not $lParam Then 
						Local $tCLIP = _WinAPI_GetWindowRect($AddressBar)
						__TB_RectScreenToClient($hWnd, $tCLIP)
						_WinAPI_ExcludeClipRect($wParam, $tCLIP)
						$tCLIP = _WinAPI_GetWindowRect($ButBar)
						__TB_RectScreenToClient($hWnd, $tCLIP)
						_WinAPI_ExcludeClipRect($wParam, $tCLIP)
						_WinAPI_DrawThemeParentBackground($hWnd, $wParam, $tRECT)
					EndIf
					
					Local $hDestDC = _WinAPI_CreateCompatibleDC($wParam)
					Local $hDestSv = _WinAPI_SelectObject($hDestDC, $hDIB)
					_WinAPI_DefSubclassProc($hWnd, $iMsg, $hDestDC, $lParam)
					Local $hTheme = _WinAPI_OpenThemeData($hAddress_Band_Root, 'AddressBand')
					Local Static $tMargins = _WinAPI_GetThemeMargins($hTheme, 1, 2, $TMT_CONTENTMARGINS, 0, $clientRect)
					Local Static $cxLeftWidth = DllStructGetData($tMargins, "cxLeftWidth")
					Local Static $cxRightWidth = DllStructGetData($tMargins, "cxRightWidth")
					Local Static $cyTopHeight = DllStructGetData($tMargins, "cyTopHeight") * 2
					Local Static $cyBottomHeight = DllStructGetData($tMargins, "cyBottomHeight") * 2
					
					_WinAPI_BitBlt($wParam, $cxLeftWidth, $cyTopHeight, $iWidth -$cxRightWidth * 2, $iHeight-$cyBottomHeight * 2, $hDestDC, $cxLeftWidth, $cyTopHeight, $SRCCOPY)
					
					Local $iBarState = $bAddressMouseHover ? 2 : 1
					_WinAPI_DrawThemeBackground($hTheme, 1, $iBarState, $wParam, $clientRect)
					_WinAPI_CloseThemeData($hTheme)
					
					_WinAPI_SelectObject($hDestDC, $hDestSv)
					_WinAPI_DeleteDC($hDestDC)
					
					Return 1
				Case $WM_NCCALCSIZE, $WM_NCPAINT
					Return 0
				Case $WM_ERASEBKGND
					Local $tRECT = DllStructCreate($tagRECT)
 					_WinAPI_GetClipBox($wParam, $tRECT)
					_WinAPI_DrawThemeParentBackground($hWnd, $wParam, $tRECT)
					Return 1
 				Case $WM_PAINT
					Local $tPAINTSTRUCT
					Local $hDC = ($wParam ? $wParam : _WinAPI_BeginPaint($hWnd, $tPAINTSTRUCT))
					If $bPaintProgress And $iWidth And $iHeight Then
						Local $hDestDC = _WinAPI_CreateCompatibleDC($hDC)
						Local $hDestSv = _WinAPI_SelectObject($hDestDC, $hDIB)
						_WinAPI_DefSubclassProc($hWnd, $iMsg, $hDestDC, $lParam)
						Local $hTheme = _WinAPI_OpenThemeData($hAddress_Band_Root, 'AddressBand')
						Local Static $tMargins = _WinAPI_GetThemeMargins($hTheme, 1, 2, $TMT_CONTENTMARGINS, 0, $clientRect)
						Local Static $cxLeftWidth = DllStructGetData($tMargins, "cxLeftWidth")
						Local Static $cxRightWidth = DllStructGetData($tMargins, "cxRightWidth")
						Local Static $cyTopHeight = DllStructGetData($tMargins, "cyTopHeight") * 2
						Local Static $cyBottomHeight = DllStructGetData($tMargins, "cyBottomHeight") * 2
						
						_WinAPI_BitBlt($hDC, $cxLeftWidth, $cyTopHeight, $iWidth -$cxRightWidth * 2, $iHeight-$cyBottomHeight * 2, $hDestDC, $cxLeftWidth, $cyTopHeight, $SRCCOPY)
						
						Local $iBarState = $bAddressMouseHover ? 2 : 1
						_WinAPI_DrawThemeBackground($hTheme, 1, $iBarState, $hDC, $clientRect)
						_WinAPI_CloseThemeData($hTheme)
						
						_WinAPI_SelectObject($hDestDC, $hDestSv)
						_WinAPI_DeleteDC($hDestDC)
						
						__RedrawBar($bRedrawBar, False)
					EndIf
					
					If Not $wParam Then _WinAPI_EndPaint($hWnd, $tPAINTSTRUCT)
					Return 0
				Case $WM_WINDOWPOSCHANGED
					Local $tPOS = DllStructCreate($tagWINDOWPOS, $lParam)
					$iWidth = DllStructGetData($tPOS, 'CX')
					$iHeight = DllStructGetData($tPOS, 'CY')
					_WinAPI_SetWindowPos($ButBar, 0, DllStructGetData($tPOS, 'CX') - 74 * $scaling, 0, 74 * $scaling, DllStructGetData($tPOS, 'CY'), BitOR($SWP_NOACTIVATE, $SWP_NOREPOSITION, $SWP_NOZORDER, $SWP_NOREDRAW))
					_WinAPI_SetWindowPos($hBreadcrumb_Parent, 0, 0, 0, DllStructGetData($tPOS, 'CX') - 74 * $scaling, DllStructGetData($tPOS, 'CY'), BitOR($SWP_NOACTIVATE, $SWP_NOREPOSITION, $SWP_NOZORDER, $SWP_NOREDRAW))
					_WinAPI_DeleteObject($hDIB)
					$hDIB = _WinAPI_CreateDIB($iWidth, -$iHeight)
					$clientRect = _WinAPI_CreateRect(0, 0, $iWidth, $iHeight)
					If $bBeginDrag Then __RedrawBar($bRedrawBar, False)
					
			EndSwitch
			Return _WinAPI_DefSubclassProc($hWnd, $iMsg, $wParam, $lParam)
		Case $TravelBar, $AddressBar, $ButBar
			Switch $iMsg
				Case $WM_DESTROY
					_GUIImageList_Destroy(_GUICtrlToolbar_GetImageList($hWnd))
					_WinAPI_RemoveWindowSubclass($hWnd, $pData, $iID)
				Case $WM_SETCURSOR
					Return 1
				Case $WM_ACTIVATEAPP
					Return 3
				Case $WM_MOUSEACTIVATE
					Return 1
				Case $WM_MOUSELEAVE
					If $hWnd == $TravelBar Then Return _WinAPI_DefSubclassProc($hWnd, $iMsg, $wParam, $lParam) 
					If Not $bPaintProgress And $bAddressMouseHover Then 
						$bAddressMouseHover = False
						
						If $hWnd == $AddressBar Then
							_WinAPI_InvalidateRect($ButBar)
						ElseIf $hWnd == $ButBar Then 
							_WinAPI_InvalidateRect($AddressBar)
						EndIf
					EndIf
				Case $WM_MOUSEMOVE
					If $hWnd == $TravelBar Then Return _WinAPI_DefSubclassProc($hWnd, $iMsg, $wParam, $lParam) 
					If Not $bPaintProgress And Not $bBeginDrag And Not $bAddressMouseHover Then 
						$bAddressMouseHover = True
						
						If $hWnd == $AddressBar Then
							_WinAPI_InvalidateRect($ButBar)
						ElseIf $hWnd == $ButBar Then 
							_WinAPI_InvalidateRect($AddressBar)
						EndIf
					EndIf
				Case $WM_PAINT
					Local $tPAINTSTRUCT
					Local $hDC = ($wParam ? $wParam : _WinAPI_BeginPaint($hWnd, $tPAINTSTRUCT))
					Local $Result = _WinAPI_DefSubclassProc($hWnd, $iMsg, $hDC, $lParam)
					If Not $wParam Then _WinAPI_EndPaint($hWnd, $tPAINTSTRUCT)
					Return $Result
				Case $WM_THEMECHANGED
					Local $Result = _WinAPI_DefSubclassProc($hWnd, $iMsg, $wParam, $lParam) 
					Local $iButtonHeight = 29 * $scaling
					Switch $hWnd
						Case $TravelBar
							_GUICtrlToolbar_SetButtonSize($hWnd, $iButtonHeight, $iButtonHeight)
						Case $AddressBar
							_GUICtrlToolbar_SetButtonSize($hWnd, $iButtonHeight, 110 * $scaling)
						Case $ButBar
							Local $hImage = _GUICtrlToolbar_GetImageList($ButBar)
							__CreateImageList($hImage, $scaling)
							If Not $bPaintProgress Then _GUICtrlToolbar_SetButtonInfo($hWnd, 1400, 0, $TBSTATE_ENABLED, $BTNS_AUTOSIZE, 34 * $scaling)
							_GUICtrlToolbar_SetButtonInfo($hWnd, 1500, $bPaintProgress ? 2 : 1, $TBSTATE_ENABLED, $BTNS_AUTOSIZE, 34 * $scaling)
							If Not $bPaintProgress Then _GUICtrlToolbar_SetIndent($hWnd, 5 * $scaling)
							If $bPaintProgress Then _GUICtrlToolbar_SetIndent($hWnd, (5 +34) * $scaling)
							_GUICtrlToolbar_SetButtonSize($hWnd, $iButtonHeight, $iButtonHeight)
					EndSwitch
					Return $Result
			EndSwitch
			
			Return _WinAPI_DefSubclassProc($hWnd, $iMsg, $wParam, $lParam) 
		Case $DirectUIHWND
			Switch $iMsg
				Case $WM_NCCALCSIZE, $WM_NCPAINT, $WM_PRINTCLIENT
					Return 0
				Case $WM_MOUSEACTIVATE
					If Not $bBeginDrag Then _WinAPI_SetFocus($Search_Box)
					Return 0
				Case $WM_NCHITTEST
					;If $bBeginDrag Then Return 2
					Return 1
				Case $WM_MOUSEMOVE, $WM_SETCURSOR
					If $bBeginDrag Then Return 0
					ContinueCase
				Case Else
					Return _SearchProc($hWnd, $iMsg, $wParam, $lParam, $scaling)
			EndSwitch
			Return _WinAPI_DefWindowProcW($hWnd, $iMsg, $wParam, $lParam)
		Case $Search_Box
			Local Static $bPressButton = False
			Switch $iMsg
				Case $WM_MOUSEACTIVATE
					If $bPressButton Then Return 3
					Return _WinAPI_DefSubclassProc($hWnd, $iMsg, $wParam, $lParam)
				Case $WM_SETFOCUS, $WM_KILLFOCUS
					_WinAPI_PostMessage($DirectUIHWND, $iMsg, $wParam, $lParam)
				Case $WM_CHAR
					Switch $wParam
						Case 9
							Return 1
						Case $VK_ESCAPE
							_WinAPI_SetFocus(0)
					EndSwitch
				Case $WM_KEYUP
					Switch _GUICtrlTab_GetCurSel($ShellTab)
						Case 0
							
						Case 1
							
						Case 2
							Local $iIndex = _GUICtrlTab_GetCurSel($hTab10)
							If $iIndex > 0 And Not $tCurentModify.oldnewexplorer Then $iIndex += 1
							If $iIndex > 5 And Not $tCurentModify.configcontextmenu Then $iIndex += 1
							Switch $iIndex
								Case 4
									AdlibRegister(FindLV, 500)
								Case 5
									Switch $wParam
										Case 0x72 ; F3
											If Not FindTV(_GUICtrlTreeView_GetNext($hTreeView, _GUICtrlTreeView_GetSelection($hTreeView)), 0) Then FindTV(0, 0)
										Case Else
											FindTV(0, 500)
									EndSwitch
								Case Else
									
							EndSwitch
					EndSwitch
				Case $WM_LBUTTONDOWN
					$bPressButton = True
				Case $WM_RBUTTONUP, $WM_LBUTTONUP
					$bPressButton = False
					
					Local $tPOINT = DllStructCreate("int X;int Y")
					DllStructSetData($tPOINT, "X", _WinAPI_LoWord($lParam))
					DllStructSetData($tPOINT, "Y", _WinAPI_HiWord($lParam))
					_WinAPI_ClientToScreen($hWnd, $tPOINT)
					_WinAPI_ScreenToClient($DirectUIHWND, $tPOINT)
					_SendMessage($DirectUIHWND, $iMsg, $wParam, _WinAPI_MakeLong(DllStructGetData($tPOINT, "X"), DllStructGetData($tPOINT, "Y")))
				Case $WM_MOUSEMOVE
					If $bPressButton Then
						Local $tPOINT = DllStructCreate("int X;int Y")
						DllStructSetData($tPOINT, "X", _WinAPI_LoWord($lParam))
						DllStructSetData($tPOINT, "Y", _WinAPI_HiWord($lParam))
						_WinAPI_ClientToScreen($hWnd, $tPOINT)
						_WinAPI_ScreenToClient($DirectUIHWND, $tPOINT)
						_SendMessage($DirectUIHWND, $iMsg, $wParam, _WinAPI_MakeLong(DllStructGetData($tPOINT, "X"), DllStructGetData($tPOINT, "Y")))
					EndIf
				Case $WM_NCPAINT
					Return 0
				Case $WM_NCCALCSIZE
					Return 0
				Case $WM_PAINT
					Local $tPAINTSTRUCT
					Local $hDC = ($wParam ? $wParam : _WinAPI_BeginPaint($hWnd, $tPAINTSTRUCT))
					
					If Not $wParam Then _WinAPI_EndPaint($hWnd, $tPAINTSTRUCT)
					Return 0
				Case $WM_ERASEBKGND;, $WM_PRINTCLIENT
					Local $tRECT = DllStructCreate($tagRECT)
  					_WinAPI_GetClipBox($wParam, $tRECT)
					_WinAPI_DrawThemeParentBackground($hWnd, $wParam, $tRECT)
					Return 1
				Case $WM_PRINTCLIENT
					Return 1;_WinAPI_DefSubclassProc($hWnd, $iMsg, $wParam, $lParam)
			EndSwitch
			ContinueCase
		Case Else
			Switch $iMsg
				Case $WM_CREATE
					If _WinAPI_GetClassName($hWnd) == "CabinetWClass" Then 
						$iDPI = _WinAPI_GetDpiForPrimaryMonitor()
						$scaling = $iDPI / 96
						Local $tCREATESTRUCT = DllStructCreate($tagCREATESTRUCT, $lParam)
						_CreateWorkerW($hWnd, DllStructGetData($tCREATESTRUCT, 'cx'), $GUI_FRAME, $iDPI)
						__SetActive($scaling, True)
						$WndWidth = DllStructGetData($tCREATESTRUCT, 'cx')
						$WndHeight = DllStructGetData($tCREATESTRUCT, 'cy') - $GUI_FRAME * $scaling
					EndIf
				Case $WM_THEMECHANGED
					If Not $wParam Then Return 0
					Switch $hWnd
;~ 						Case $hTravelBand
;~ 							_WinAPI_SetWindowTheme($TravelBar, 'BBComposited', 0)
;~ 						Case $hBreadcrumb_Parent
;~ 							_WinAPI_SetWindowTheme($AddressBar, _WinAPI_GetAncestor($hWnd, $GA_ROOTOWNER), 'BBComposited::ToolBar')
;~ 							_WinAPI_SetWindowTheme($ButBar, _WinAPI_GetAncestor($hWnd, $GA_ROOTOWNER), 'BB::ToolBar')
;~ 							_GUICtrlToolbar_SetButtonSize($AddressBar, 34, 149)
;~ 							_GUICtrlToolbar_SetMetrics($AddressBar, 10, 0, 0, 0)
						Case $hUniversalSearchBand
							_WinAPI_RedrawWindow($hWnd, 0, 0, BitOR($RDW_INVALIDATE, $RDW_ALLCHILDREN))
					EndSwitch
				Case $WM_DPICHANGED_BEFOREPARENT
					Local $scaling2 = 96 / _WinAPI_GetDpiForSystem()
					Local $tFont = _WinAPI_GetThemeSysFont(0, $TMT_ICONTITLEFONT)
					Local $iSizeFont = Round(9 * $scaling * $scaling2)
					DllStructSetData($tFont, "Height", __SizeToHeight($iSizeFont))
					Switch $hWnd
						Case $hTravelBand
							Local $iButtonHeight = 29 * $scaling
							_GUICtrlRebar_DeleteBand($g_hReBar, 0)
							_WinAPI_SetWindowPos($hWnd, 0, 0, 0, 100 * $scaling, ($GUI_FRAME -21) * $scaling, BitOR($SWP_NOMOVE, $SWP_NOZORDER))
							_WinAPI_SetFont($TravelBar, _WinAPI_CreateFontIndirect($tFont), False)
							_GUICtrlToolbar_SetButtonInfo($TravelBar, $iEditUndo, -1, $TBSTATE_ENABLED, $BTNS_AUTOSIZE, $iButtonHeight)
							_GUICtrlToolbar_SetButtonInfo($TravelBar, $iEditRedo, -1, $TBSTATE_ENABLED, $BTNS_AUTOSIZE, $iButtonHeight)
							_GUICtrlToolbar_SetButtonInfo($TravelBar, $MenuButton, -1, $TBSTATE_ENABLED, $BTNS_AUTOSIZE, 15 * $scaling)
							_GUICtrlToolbar_SetIndent($TravelBar, 3 * $scaling)
							_GUICtrlToolbar_SetButtonSize($TravelBar, $iButtonHeight, $iButtonHeight)
							_GUICtrlToolbar_SetMetrics($TravelBar, 0, 2 * $scaling, $iXSpacing * $scaling, $iYSpacing * $scaling)
							_GUICtrlToolbar_SetPadding($TravelBar, 5 * $scaling, 2 * $scaling)
							_GUICtrlRebar_AddBand($g_hReBar, $hWnd, 100 * $scaling, 100 * $scaling, "TravelBand", 0, BitOR($RBBS_FIXEDSIZE, $RBBS_NOGRIPPER, $RBBS_HIDETITLE, $RBBS_FIXEDBMP, $RBBS_NOVERT))
						Case $hAddress_Band_Root
							_GUICtrlRebar_DeleteBand($g_hReBar, 1)
							_WinAPI_SetWindowPos($hWnd, 0, 0, 0, _WinAPI_GetClientWidth($hWnd), ($GUI_FRAME - 3) * $scaling, BitOR($SWP_NOMOVE, $SWP_NOZORDER))
							_WinAPI_SetFont($AddressBar, _WinAPI_CreateFontIndirect($tFont), False)
							_WinAPI_SetFont($ButBar, _WinAPI_CreateFontIndirect($tFont), False)
							For $iIndex = 1 To _GUICtrlToolbar_ButtonCount($AddressBar) -1
								_GUICtrlToolbar_SetButtonInfo($AddressBar, _GUICtrlToolbar_IndexToCommand($AddressBar, $iIndex), -2, $TBSTATE_ENABLED, BitOR($BTNS_AUTOSIZE, $BTNS_DROPDOWN, 0, $BTNS_SHOWTEXT))
							Next
							Local $iMinSize = __TB_GetMinSize() * $scaling
							_GUICtrlRebar_AddBand($g_hReBar, $hWnd, $iMinSize, _WinAPI_GetClientWidth($AddressBar), "Address Band", 1, BitOR($RBBS_USECHEVRON, $RBBS_NOGRIPPER, $RBBS_HIDETITLE, $RBBS_FIXEDBMP, $RBBS_NOVERT))
							__SetActive($scaling, $bParentActive)
							Local $hImage = __CreateMenuImageList($scaling)
							Local $iCommandID = _GUICtrlToolbar_IndexToCommand($AddressBar, 0)
							Local $hMenu = Ptr(_GUICtrlToolbar_GetButtonParam($AddressBar, $iCommandID))
							_GUIImageList_Destroy(_GuiCtrlDropMenu_GetImageList($hMenu))
							_GuiCtrlDropMenu_SetImageList($hMenu, $hImage)
							For $iIndex = 1 To _GUICtrlToolbar_ButtonCount($AddressBar) -1
								$iCommandID = _GUICtrlToolbar_IndexToCommand($AddressBar, $iIndex)
								$hMenu = Ptr(_GUICtrlToolbar_GetButtonParam($AddressBar, $iCommandID))
								_GUIImageList_Destroy(_GuiCtrlDropMenu_GetImageList($hMenu))
								_GuiCtrlDropMenu_SetImageList($hMenu, $hImage)
								_GUICtrlToolbar_SetButtonInfo($AddressBar, $iCommandID, -2, BitOR($TBSTATE_ENABLED, $TBSTATE_ELLIPSES), BitOR($BTNS_AUTOSIZE, $BTNS_DROPDOWN, $BTNS_NOPREFIX, $BTNS_SHOWTEXT))
							Next
						Case $hSeparatorBand
							_WinAPI_SetWindowPos($hWnd, 0, 0, 0, 6 * $scaling, 1, BitOR($SWP_NOMOVE, $SWP_NOZORDER))
							_GUICtrlRebar_SetBandStyleFixedSize($g_hReBar, 2, False)
							_GUICtrlRebar_SetBandLength($g_hReBar, 2, 6 * $scaling)
							_GUICtrlRebar_SetBandStyleFixedSize($g_hReBar, 2, True)
						Case $hUniversalSearchBand
							_GUICtrlRebar_DeleteBand($g_hReBar, 3)
							_WinAPI_SetWindowPos($hWnd, 0, 0, 0, $SearchBandLength * $scaling, 40 * $scaling, BitOR($SWP_NOMOVE, $SWP_NOZORDER))
							_WinAPI_SetFont($Search_Box, _WinAPI_CreateFontIndirect($tFont), False)
							_GUICtrlRichEdit_SetFont($Search_Box, 9, $tFont.FaceName, $tFont.CharSet)
							_WinAPI_SetFont($DirectUIHWND, _WinAPI_CreateFontIndirect($tFont), False)
							_GUICtrlRebar_AddBand($g_hReBar, $hWnd, 100 * $scaling, 100 * $scaling, "UniversalSearchBand", 3, BitOR($RBBS_NOGRIPPER, $RBBS_HIDETITLE, $RBBS_FIXEDBMP, $RBBS_NOVERT))
							_GUICtrlRebar_SetBandLength($g_hReBar, 3, $SearchBandLength * $scaling)
						Case $hSeparatorBand2
							_WinAPI_SetWindowPos($hWnd, 0, 0, 0, 6 * $scaling, 1, BitOR($SWP_NOMOVE, $SWP_NOZORDER))
							_GUICtrlRebar_SetBandStyleFixedSize($g_hReBar, 4, False)
							_GUICtrlRebar_SetBandLength($g_hReBar, 4, 7 * $scaling)
							_GUICtrlRebar_SetBandStyleFixedSize($g_hReBar, 4, True)
					EndSwitch
					Return 0
				Case $WM_DPICHANGED_AFTERPARENT
					Return 0
				Case $WM_WINDOWPOSCHANGED
					Local $tPOS = DllStructCreate($tagWINDOWPOS, $lParam)
					Switch $hWnd
						Case $hTravelBand
							DllStructSetData($tPOS, 'Y', 9)
							If DllStructGetData($tBandSize, 1, 1) == DllStructGetData($tPOS, 'CX') Then Return 0
							_WinAPI_MoveWindow($TravelBar, 4 * $scaling, 2 * $scaling, DllStructGetData($tPOS, 'CX') - 4 * $scaling, DllStructGetData($tPOS, 'CY') - 2 * $scaling, False)
							DllStructSetData($tBandSize, 1, DllStructGetData($tPOS, 'CX'), 1)
							DllStructSetData($tBandSize, 1, DllStructGetData($tPOS, 'CY'), 2)
						Case $hAddress_Band_Root
							If DllStructGetData($tBandSize, 2, 1) == DllStructGetData($tPOS, 'CX') Then Return 0
							_WinAPI_SetWindowPos($hProgress, 0, 2 * $scaling, 10 * $scaling, DllStructGetData($tPOS, 'CX') - 4 * $scaling, DllStructGetData($tPOS, 'CY') - 18 * $scaling, BitOR($SWP_NOACTIVATE, $SWP_NOREPOSITION, $SWP_NOZORDER, $SWP_NOREDRAW))
							DllStructSetData($tBandSize, 2, DllStructGetData($tPOS, 'CX'), 1)
							DllStructSetData($tBandSize, 2, DllStructGetData($tPOS, 'CY'), 2)
							Local $tRECT = _GUICtrlRebar_GetBandRectEx($g_hReBar, 2)
							DllStructSetData($tRECT, 1, DllStructGetData($tRECT, 1) -10 * $scaling)
							Local $hDC = _WinAPI_GetWindowDC($g_hReBar)
							_WinAPI_IntersectClipRect($hDC, $tRECT)
							_WinAPI_DrawThemeParentBackground($g_hReBar, $hDC, $tRECT)
							_WinAPI_ReleaseDC($g_hReBar, $hDC)
						Case $hBreadcrumb_Parent
							_WinAPI_SetWindowPos($AddressBar, 0, 6 * $scaling, 0, DllStructGetData($tPOS, 'CX') - 6 * $scaling, DllStructGetData($tPOS, 'CY'), BitOR($SWP_NOACTIVATE, $SWP_NOREPOSITION, $SWP_NOZORDER, $SWP_NOMOVE, $SWP_NOREDRAW))
						Case $hSeparatorBand, $hSeparatorBand2
							DllStructSetData($tBandSize, 3, DllStructGetData($tPOS, 'CX'), 1)
							DllStructSetData($tBandSize, 3, DllStructGetData($tPOS, 'CY'), 2)
						Case $hUniversalSearchBand
							DllStructSetData($tPOS, 'Y', 6)
							If DllStructGetData($tBandSize, 4, 1) == DllStructGetData($tPOS, 'CX') Then Return 0
							_WinAPI_SetWindowPos($Search_Box, 0, 0, 3 * $scaling, DllStructGetData($tPOS, 'CX'), DllStructGetData($tPOS, 'CY') - 4 * $scaling, BitOR($SWP_NOACTIVATE, $SWP_NOREPOSITION, $SWP_NOZORDER, $SWP_NOMOVE, $SWP_NOREDRAW))
							DllStructSetData($tBandSize, 4, DllStructGetData($tPOS, 'CX'), 1)
							DllStructSetData($tBandSize, 4, DllStructGetData($tPOS, 'CY'), 2)
						Case $Search_Box
							_WinAPI_SetWindowPos($SearchEditBoxWrapperClass, 0, 0, 0, DllStructGetData($tPOS, 'CX'), DllStructGetData($tPOS, 'CY'), BitOR($SWP_NOACTIVATE, $SWP_NOREPOSITION, $SWP_NOZORDER, $SWP_NOMOVE, $SWP_NOREDRAW))
							Local $tRECT = _WinAPI_CreateRectEx(14 * $scaling, 12 * $scaling, DllStructGetData($tPOS, 'CX') - DllStructGetData($tPOS, 'CY') - 10 * $scaling, DllStructGetData($tPOS, 'CY') - 24 * $scaling)
							_SendMessage($hWnd, $EM_SETRECTNP, 0, $tRECT, 0, "wparam", "struct*")
						Case $SearchEditBoxWrapperClass
							_WinAPI_SetWindowPos($DirectUIHWND, 0, 0, 0, DllStructGetData($tPOS, 'CX'), DllStructGetData($tPOS, 'CY'), BitOR($SWP_NOACTIVATE, $SWP_NOREPOSITION, $SWP_NOZORDER, $SWP_NOMOVE, $SWP_NOREDRAW))
					EndSwitch
					;Return 0
				Case $WM_DESTROY
					Switch $hWnd
						Case $Search_Box
							_WinAPI_RemoveWindowSubclass($hWnd, $pData, $iID)
						Case Else 
							_WinAPI_UnregisterClass(_WinAPI_GetClassName($hWnd), _WinAPI_GetModuleHandle(0))
					EndSwitch
				Case $WM_NOTIFY
					Return _SendMessage($WorkerW, $iMsg, $wParam, $lParam)
				Case $WM_PAINT
					Local $tPAINTSTRUCT
					Local $hDC = ($wParam ? $wParam : _WinAPI_BeginPaint($hWnd, $tPAINTSTRUCT))
					
					If Not $wParam Then _WinAPI_EndPaint($hWnd, $tPAINTSTRUCT)
					Return 0
				Case $WM_ERASEBKGND
					Local $tCLIP = DllStructCreate($tagRECT)
  					_WinAPI_GetClipBox($wParam, $tCLIP)
					_WinAPI_DrawThemeParentBackground($hWnd, $wParam, $tCLIP)
					Switch $hWnd
						Case $hAddress_Band_Root
							Local $tRECT = _WinAPI_GetWindowRect($hProgress)
							__TB_RectScreenToClient($hWnd, $tRECT)
							Local $iBarState = $bAddressMouseHover ? 2 : 1
							Local $hTheme = _WinAPI_OpenThemeData($hAddress_Band_Root, 'AddressBand')
							_WinAPI_DrawThemeBackground($hTheme, 1, $iBarState, $wParam, $tRECT, $tCLIP)
							_WinAPI_CloseThemeData($hTheme)
					EndSwitch
					Return 1
				Case $WM_PRINTCLIENT
					Local $tCLIP = DllStructCreate($tagRECT)
  					_WinAPI_GetClipBox($wParam, $tCLIP)
					_WinAPI_DrawThemeParentBackground($hWnd, $wParam, $tCLIP)
					Switch $hWnd
						Case $hAddress_Band_Root
							Local $tRECT = _WinAPI_GetWindowRect($hProgress)
							__TB_RectScreenToClient($hWnd, $tRECT)
							Local $iBarState = $bAddressMouseHover ? 2 : 1
							Local $hTheme = _WinAPI_OpenThemeData($hAddress_Band_Root, 'AddressBand')
							_WinAPI_DrawThemeBackground($hTheme, 1, $iBarState, $wParam, $tRECT, $tCLIP)
							_WinAPI_CloseThemeData($hTheme)
						Case $hBreadcrumb_Parent
							
						Case $SearchEditBoxWrapperClass
							
							Return 1
					EndSwitch
					
					Return 1
				Case $WM_NCCALCSIZE, $WM_NCPAINT
					Return 0
				Case $WM_NCHITTEST
					Return 1
				Case $WM_SETCURSOR
					Return 1
				Case $WM_MOUSEACTIVATE
					_WinAPI_PostMessage($hGUI, $MOUSEACTIVATE_EVENT, 0, 0)
					Return 1
				Case Else
					;If $hWnd == $hBreadcrumb_Parent Then ConsoleWrite('0x' & Hex($iMsg, 8) & @CR)
			EndSwitch
	EndSwitch
	Switch $hWnd
		Case $Search_Box
			Return _WinAPI_DefSubclassProc($hWnd, $iMsg, $wParam, $lParam)
		Case Else
			Return _WinAPI_DefWindowProcW($hWnd, $iMsg, $wParam, $lParam)
	EndSwitch
EndFunc   ;==>_WndToolProc
