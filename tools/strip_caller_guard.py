# -*- coding: utf-8 -*-
path = r"d:\VS_PROJECT\DWM\GlassProject\GlassCore\src\Core\Intercept\DebugDetourHooks.cpp"


def skip_return_block(lines, idx_if):
    j = idx_if + 1
    if j >= len(lines):
        return j
    cur = lines[j]
    depth = cur.count("(") - cur.count(")")
    st = cur.strip()
    if st.startswith("return ") and st.endswith(");") and depth == 0:
        return j + 1
    while j + 1 < len(lines):
        j += 1
        cur = lines[j]
        depth += cur.count("(") - cur.count(")")
        if cur.strip().endswith(");") and depth <= 0:
            return j + 1
    raise SystemExit(f"strip: no end of return after line {idx_if + 1}")


def skip_brace_block(lines, idx_if):
    j = idx_if + 1
    while j < len(lines) and not lines[j].strip():
        j += 1
    if j >= len(lines) or lines[j].strip() != "{":
        raise SystemExit("strip: expected {{ after CallerModule-if, line=%s" % (j + 1))
    brace = lines[j].count("{") - lines[j].count("}")
    j += 1
    while j < len(lines):
        lt = lines[j]
        brace += lt.count("{") - lt.count("}")
        j += 1
        if brace <= 0:
            break
    while j < len(lines) and not lines[j].strip():
        j += 1
    return j


def main():
    with open(path, encoding="utf-8") as f:
        lines = f.readlines()

    out = []
    i = 0
    while i < len(lines):
        ln = lines[i]

        if ln.startswith("        // \u041f\u0440\u044f\u043c\u043e\u0439 \u0432\u044b\u0437\u044b\u0432\u0430\u044e\u0449\u0438\u0439"):
            peek = "".join(lines[i : min(len(lines), i + 14)])
            if "CallerModuleIsGlassCore" not in peek:
                out.append(ln)
                i += 1
                continue
            out.append(
                "        // NB: \u0447\u0435\u0440\u0435\u0437 Detours "
                "\u0443\u043a\u0430\u0437\u0430\u0442\u0435\u043b\u044c "
                "g_true* "
                "\u0432\u0435\u0434\u0451\u0442 "
                "\u043d\u0430 \u0442\u0440\u0430\u043c\u043f\u043b\u0438\u043d "
                "\u0432 GlassCore.dll; "
                "_ReturnAddress() \u0443 \u0445\u0443\u043a\u0430 "
                "\u0447\u0430\u0441\u0442\u043e "
                "\u0443\u043a\u0430\u0437\u044b\u0432\u0430\u0435\u0442 "
                "\u0442\u0443\u0434\u0430 \u0436\u0435, "
                "\u043f\u043e\u044d\u0442\u043e\u043c\u0443 "
                "\xabcaller == GlassCore\xbb "
                "\u0434\u043b\u044f \u043f\u043e\u0434\u0430\u0432\u043b\u0435\u043d\u0438\u044f "
                "\u043b\u043e\u0433\u043e\u0432 "
                "\u043d\u0435\u043b\u044c\u0437\u044f.\n"
            )
            while i < len(lines) and "[[nodiscard]] bool CallerModuleIsGlassCore" not in lines[i]:
                i += 1
            if i >= len(lines):
                raise SystemExit("no CallerModuleIsGlassCore declaration")
            brace_dept = 0
            entered = False
            while i < len(lines):
                lt = lines[i]
                brace_dept += lt.count("{") - lt.count("}")
                if "{" in lt:
                    entered = True
                i += 1
                if entered and brace_dept == 0:
                    break
            continue

        if "/*if (CallerModuleIsGlassCore(_ReturnAddress()))" in ln:
            while i < len(lines) and "*/" not in lines[i]:
                i += 1
            i += 1
            continue

        if "if (CallerModuleIsGlassCore(_ReturnAddress()))" in ln:
            j = i + 1
            while j < len(lines) and not lines[j].strip():
                j += 1
            if j < len(lines) and lines[j].strip() == "{":
                i = skip_brace_block(lines, i)
            else:
                i = skip_return_block(lines, i)
            continue

        out.append(ln)
        i += 1

    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(out)


if __name__ == "__main__":
    main()
    print("OK", path)
