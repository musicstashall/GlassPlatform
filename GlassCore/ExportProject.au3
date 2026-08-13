#include <Array.au3>
#include <GUIConstantsEx.au3>
#include <WindowsConstants.au3>
#include <ButtonConstants.au3>
#include <StaticConstants.au3>
#include <File.au3>
#include <Clipboard.au3>

; -----------------------------------------
; Настройки
; -----------------------------------------
Global $g_MaxPartSize = 10000
Global $g_ExtAllowed = [".h", ".hpp", ".cpp", ".inl", ".def", ".rc"]

; -----------------------------------------
; Корень проекта = папка скрипта
; -----------------------------------------
Global $g_Root = @ScriptDir

; -----------------------------------------
; Сканируем файлы
; -----------------------------------------
Global $g_FileList = _ScanFolder($g_Root)

If UBound($g_FileList) = 0 Then
    MsgBox(48, "Export", "Нет исходников.")
    Exit
EndIf

; -----------------------------------------
; Собираем весь текст
; -----------------------------------------
Global $content = ""

For $i = 0 To UBound($g_FileList) - 1
    Local $full = $g_FileList[$i]
    Local $rel = StringReplace($full, $g_Root & "\", "")
    Local $text = FileRead($full)

    $content &= "=== FILE: " & $rel & " ===" & @CRLF
    $content &= $text & @CRLF
    $content &= "=== END FILE ===" & @CRLF & @CRLF
Next

; -----------------------------------------
; Разбивка на части
; -----------------------------------------
Global $total = StringLen($content)
Global $parts = Ceiling($total / $g_MaxPartSize)

Global $g_PartText[$parts]

For $p = 1 To $parts
    Local $start = ($p - 1) * $g_MaxPartSize + 1
    $g_PartText[$p - 1] = StringMid($content, $start, $g_MaxPartSize)
Next

; -----------------------------------------
; Создаём GUI
; -----------------------------------------
Global $hGUI = GUICreate("Export Viewer (" & $parts & " частей)", 600, 80 + $parts * 40, -1, -1, _
    BitOR($WS_SIZEBOX, $WS_MAXIMIZEBOX, $WS_MINIMIZEBOX, $WS_VSCROLL))

GUISetBkColor(0xF0F0F0)

; Кнопка копирования структуры проекта
Global $btnStruct = GUICtrlCreateButton("Копировать структуру проекта", 10, 10, 250, 30)

Global $y = 50
Global $buttons[$parts]

For $p = 1 To $parts
    GUICtrlCreateLabel("Часть " & $p & " из " & $parts, 10, $y, 200, 20)
    $buttons[$p - 1] = GUICtrlCreateButton("Копировать", 220, $y - 2, 120, 24)
    $y += 35
Next

GUISetState(@SW_SHOW)

; -----------------------------------------
; Цикл сообщений
; -----------------------------------------
While 1
    Local $msg = GUIGetMsg()

    Switch $msg
        Case $GUI_EVENT_CLOSE
            Exit

        Case $btnStruct
            Local $tree = _BuildTree($g_Root)
            _ClipBoard_Open(0)
            _ClipBoard_SetData($tree, $CF_TEXT)
            _ClipBoard_Close()
            MsgBox(64, "Готово", "Структура проекта скопирована.")

        Case Else
            For $i = 0 To $parts - 1
                If $msg = $buttons[$i] Then
                    _ClipBoard_Open(0)
                    _ClipBoard_SetData($g_PartText[$i], $CF_TEXT)
                    _ClipBoard_Close()
                    MsgBox(64, "Скопировано", "Часть " & ($i + 1) & " скопирована.")
                EndIf
            Next
    EndSwitch
WEnd


; -----------------------------------------
; Функции
; -----------------------------------------

Func _ScanFolder($path)
    Local $result[0]

    Local $search = FileFindFirstFile($path & "\*.*")
    If $search = -1 Then Return $result

    While 1
        Local $file = FileFindNextFile($search)
        If @error Then ExitLoop

        Local $full = $path & "\" & $file

        If StringInStr(FileGetAttrib($full), "D") Then
            Local $sub = _ScanFolder($full)
            If UBound($sub) > 0 Then _ArrayConcatenate($result, $sub)
        Else
            Local $ext = "." & StringLower(StringTrimLeft($file, StringInStr($file, ".", 0, -1)))
            If _IsAllowed($ext) Then _ArrayAdd($result, $full)
        EndIf
    WEnd

    FileClose($search)
    Return $result
EndFunc

Func _IsAllowed($ext)
    For $i = 0 To UBound($g_ExtAllowed) - 1
        If $g_ExtAllowed[$i] = $ext Then Return True
    Next
    Return False
EndFunc

; -----------------------------------------
; Построение структуры проекта
; -----------------------------------------

Func _BuildTree($root)
    Return _BuildTreeRec($root, "")
EndFunc

Func _BuildTreeRec($path, $indent)
    Local $folder = StringTrimLeft($path, StringLen($g_Root) + 1)
    If $folder = "" Then $folder = StringTrimRight($g_Root, StringLen(@ScriptDir) - StringInStr(@ScriptDir, "\", 0, -1))

    ; ===== ФИЛЬТР ПАПОК =====
    Local $folderName = StringLower(StringTrimLeft($folder, StringInStr($folder, "\", 0, -1)))
    Switch $folderName
        Case "export_output", "x64", "debug", "release", "bin", "obj", ".vs", "ipch"
            Return "" ; пропускаем
    EndSwitch

    Local $out = $indent & $folder & @CRLF

    Local $search = FileFindFirstFile($path & "\*.*")
    If $search = -1 Then Return $out

    Local $dirs[0], $files[0]

    While 1
        Local $file = FileFindNextFile($search)
        If @error Then ExitLoop

        Local $full = $path & "\" & $file

        If StringInStr(FileGetAttrib($full), "D") Then
            ; ===== ФИЛЬТР ПАПОК =====
            Local $lname = StringLower($file)
            Switch $lname
                Case "export_output", "x64", "debug", "release", "bin", "obj", ".vs", "ipch"
                    ContinueLoop
            EndSwitch

            _ArrayAdd($dirs, $full)

        Else
            ; ===== ФИЛЬТР ФАЙЛОВ =====
            Local $ext = "." & StringLower(StringTrimLeft($file, StringInStr($file, ".", 0, -1)))

            Switch $ext
                Case ".vcxproj", ".filters", ".user", ".sln", ".pdb", ".ilk", ".obj", ".tlog", ".cache"
                    ContinueLoop
            EndSwitch

            _ArrayAdd($files, $file)
        EndIf
    WEnd

    FileClose($search)

    ; Файлы
    For $i = 0 To UBound($files) - 1
        $out &= $indent & "│   " & $files[$i] & @CRLF
    Next

    ; Папки
    For $i = 0 To UBound($dirs) - 1
        $out &= _BuildTreeRec($dirs[$i], $indent & "│   ")
    Next

    Return $out
EndFunc

