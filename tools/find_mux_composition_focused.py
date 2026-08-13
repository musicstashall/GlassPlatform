# -*- coding: utf-8 -*-
# Save this file as UTF-8 (not UTF-16). IDA Python compiles the script as UTF-8;
# UTF-16 content triggers: SyntaxError: source code string cannot contain null bytes
#
# Scan IDB for MUX / Composition / Backdrop (narrow keywords).
# Wide keys removed: visual, mica, tint, opacity, ... (see find_mux_composition_keywords.py)
# Run in IDA: File -> Script file...
#
# Report:  C:\Users\music\Documents\IDA-test\mux_composition_focused_hits.txt
# Errors: C:\Users\music\Documents\IDA-test\mux_composition_focused_hits_ERROR.txt

from __future__ import annotations

import os
import sys
import traceback
from typing import Any, List, Optional, Tuple

OUTPUT_DIR = r"C:\Users\music\Documents\IDA-test"
DEFAULT_REPORT = os.path.join(OUTPUT_DIR, "mux_composition_focused_hits.txt")

FOCUS_NAME_PATTERNS: Tuple[str, ...] = (
    "cpopup",
    "contentexternalbackdrop",
    "ensurecontentexternalbackdrop",
    "discardcontentexternalbackdrop",
    "dcomptreehost",
    "hwcomptreenode",
    "getsystembackdropbrush",
    "setsystembackdropbrush",
    "systembackdrop",
    "getsystembackdrop",
    "setsystembackdrop",
    "compositionsupportssystembackdrop",
    "systembackdropconfiguration",
    "xamlcomposition",
    "cxamlcomposition",
    "compositioneffectbrush",
    "wucbrush",
    "getwucbrush",
    "createeffectfactory",
    "effectfactory",
    "createeffectbrush",
    "effectbrush",
    "graphicseffect",
    "compositeeffect",
    "effectsource",
    "effectsurface",
    "noisesurface",
    "getcompositor",
    "getcompositorforcurrentthread",
    "compositorscheduler",
    "icompositor",
    "backdropbrush",
    "compositionbackdrop",
    "compositionbrush",
    "hostbackdrop",
    "compositioncolor",
    "compsolidcolorbrush",
    "solidcolorbrush",
    "colorbrush",
    "theming_tryenablehostbackdrop",
    "elementcompositionpreview",
    "createbackdrop",
    "dcomposition",
    "dcompositor",
)

STRING_EXTRA: Tuple[str, ...] = (
    "microsoft.ui.composition",
    "windows.ui.composition",
    "dcomposition.dll",
    "dwmapi.dll",
    "dcdcomposition",
    "createeffectfactory",
    "dcompositionwaitforcompositorclock",
)

STRING_KEYWORDS: Tuple[str, ...] = FOCUS_NAME_PATTERNS + STRING_EXTRA


def _ensure_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


def _get_input_basename() -> str:
    try:
        import ida_nalt
        return ida_nalt.get_root_filename() or "unknown"
    except Exception:
        return "unknown"


def _safe_seg_name(ea: int) -> str:
    try:
        import ida_segment
        seg = ida_segment.getseg(ea)
        if seg:
            return ida_segment.get_segm_name(seg) or "?"
    except Exception:
        pass
    return "?"


def _is_function_start(ea: int) -> bool:
    try:
        import ida_funcs
        f = ida_funcs.get_func(ea)
        return bool(f) and f.start_ea == ea
    except Exception:
        return False


def _name_hits_focus(name: str) -> List[str]:
    if not name:
        return []
    nl = name.lower()
    out: List[str] = []
    for kw in FOCUS_NAME_PATTERNS:
        if kw in nl:
            out.append(kw)
    return sorted(set(out))


def _string_text(s: Any) -> Optional[str]:
    try:
        import ida_bytes
        ea = int(s.ea)
        length = int(getattr(s, "length", 0) or 0)
        strtype = int(getattr(s, "strtype", 0) or 0)
        raw = ida_bytes.get_strlit_contents(ea, length, strtype)
        if not raw:
            return None
        enc = "utf-16-le" if (strtype & 0xFF) in (1, 2) else "utf-8"
        try:
            return raw.decode(enc, errors="replace")
        except Exception:
            return raw.decode("utf-8", errors="replace")
    except Exception:
        return None


def _string_matches(t: str) -> List[str]:
    if not t:
        return []
    tl = t.lower()
    hits: List[str] = []
    for kw in STRING_KEYWORDS:
        if kw in tl:
            hits.append(kw)
    return sorted(set(hits))


def run_ida() -> str:
    import idautils

    _ensure_dir(OUTPUT_DIR)

    root = _get_input_basename()
    report_path = DEFAULT_REPORT

    lines: List[str] = []
    ap = lines.append
    ap("=== MUX / Composition / Backdrop [focused scan, IDA] ===")
    ap("Wide keys omitted — full list: find_mux_composition_keywords.py")
    ap("IDB / module: %s" % root)
    try:
        import ida_loader
        ip = ida_loader.get_input_file_path()
        if ip:
            ap("Input path: %s" % ip)
    except Exception:
        pass

    ap("")
    ap("--- Named locations (idautils.Names) matching FOCUS_NAME_PATTERNS ---")
    name_rows: List[Tuple[int, str, str, str]] = []
    for ea, name in idautils.Names():
        try:
            eai = int(ea)
        except Exception:
            continue
        hits = _name_hits_focus(str(name))
        if not hits:
            continue
        is_fn = _is_function_start(eai)
        tag = "FUNC" if is_fn else "NAME"
        seg = _safe_seg_name(eai)
        name_rows.append((eai, tag, seg, "%s  [%s]" % (name, ",".join(hits))))

    name_rows.sort(key=lambda x: x[0])
    for eai, tag, seg, txt in name_rows:
        ap("0x%016X  %-5s  %-12s  %s" % (eai, tag, seg, txt))

    ap("")
    ap("--- Functions only (entry == ea) subset ---")
    for eai, tag, seg, txt in name_rows:
        if tag == "FUNC":
            ap("0x%016X  %-12s  %s" % (eai, seg, txt))

    ap("")
    ap("--- String literals (idautils.Strings) matching STRING_KEYWORDS ---")
    ap("(min length 6; long lines truncated to 400 chars)")

    str_rows: List[Tuple[int, str]] = []

    for s in idautils.Strings():
        try:
            te = _string_text(s)
            if te is None or len(te) < 6:
                continue
            if len(te) > 400:
                te = te[:400] + "..."
            sh = _string_matches(te)
            if not sh:
                continue
            ea = int(s.ea)
            seg = _safe_seg_name(ea)
            one_line = te.replace("\r", " ").replace("\n", " ")
            str_rows.append(
                (ea, "0x%016X  %-12s  [%s]  %s" % (ea, seg, ",".join(sh), one_line))
            )
        except Exception:
            continue

    str_rows.sort(key=lambda x: x[0])
    for _, row in str_rows:
        ap(row)

    ap("")
    ap("--- Summary ---")
    ap("Named hits: %u" % len(name_rows))
    ap("String hits: %u" % len(str_rows))
    ap("Done.")

    body = "\n".join(lines)
    with open(report_path, "w", encoding="utf-8") as f:
        f.write(body)

    try:
        import ida_kernwin
        ida_kernwin.info(
            "Focused report:\n%s\n\nNamed: %u  Strings: %u"
            % (report_path, len(name_rows), len(str_rows))
        )
    except Exception:
        pass

    return report_path


def main() -> int:
    try:
        import ida_loader  # noqa: F401
        path = run_ida()
        print("[find_mux_composition_focused] OK -> %s" % path)
        return 0
    except Exception:
        err = traceback.format_exc()
        _ensure_dir(OUTPUT_DIR)
        fail = os.path.join(OUTPUT_DIR, "mux_composition_focused_hits_ERROR.txt")
        try:
            with open(fail, "w", encoding="utf-8") as f:
                f.write(err)
                f.write("\n")
                f.write("sys.path:\n")
                f.write(repr(sys.path))
                f.write("\n")
        except Exception:
            pass
        print(err)
        print("[find_mux_composition_focused] FAIL -> %s" % fail)
        try:
            import ida_kernwin
            ida_kernwin.warning("Script failed. See:\n%s" % fail)
        except Exception:
            pass
        return 1


if __name__ == "__main__":
    main()