using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using System.Text;

static string FormatGuidC(Guid g)
{
    var b = g.ToByteArray();
    return string.Format(
        "0x{0:X8}, 0x{1:X4}, 0x{2:X4}, {{ 0x{3:X2}, 0x{4:X2}, 0x{5:X2}, 0x{6:X2}, 0x{7:X2}, 0x{8:X2}, 0x{9:X2}, 0x{10:X2} }}",
        BitConverter.ToUInt32(b, 0),
        BitConverter.ToUInt16(b, 4),
        BitConverter.ToUInt16(b, 6),
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static bool TryGetGuidAttributeCtorType(MetadataReader reader, CustomAttribute ca, out string ns, out string name)
{
    ns = "";
    name = "";
    EntityHandle parent = default;
    if (ca.Constructor.Kind == HandleKind.MemberReference)
    {
        var ctor = reader.GetMemberReference((MemberReferenceHandle)ca.Constructor);
        parent = ctor.Parent;
    }
    else if (ca.Constructor.Kind == HandleKind.MethodDefinition)
    {
        var meth = reader.GetMethodDefinition((MethodDefinitionHandle)ca.Constructor);
        parent = meth.GetDeclaringType();
    }
    else
        return false;

    if (parent.Kind == HandleKind.TypeReference)
    {
        var t = reader.GetTypeReference((TypeReferenceHandle)parent);
        ns = reader.GetString(t.Namespace);
        name = reader.GetString(t.Name);
        return true;
    }
    if (parent.Kind == HandleKind.TypeDefinition)
    {
        var t = reader.GetTypeDefinition((TypeDefinitionHandle)parent);
        ns = reader.GetString(t.Namespace);
        name = reader.GetString(t.Name);
        return true;
    }
    return false;
}

static Guid? TryParseGuidFromAttributeBlob(ref BlobReader blobReader)
{
    if (blobReader.Length < 2) return null;
    int start = blobReader.Offset;
    ushort prolog = blobReader.ReadUInt16();
    if (prolog != 1) { blobReader.Offset = start; return null; }

    if (blobReader.RemainingBytes >= 16)
    {
        int save = blobReader.Offset;
        try { return blobReader.ReadGuid(); }
        catch { blobReader.Offset = save; }
    }

    try
    {
        var s = blobReader.ReadSerializedString();
        if (!string.IsNullOrEmpty(s) && Guid.TryParse(s, out var g))
            return g;
    }
    catch { }

    return null;
}

static Guid? TryGetTypeGuid(MetadataReader reader, TypeDefinition typeDef)
{
    foreach (var h in typeDef.GetCustomAttributes())
    {
        var ca = reader.GetCustomAttribute(h);
        if (!TryGetGuidAttributeCtorType(reader, ca, out var ans, out var aname)) continue;
        if (aname != "GuidAttribute") continue;
        if (ans != "System.Runtime.InteropServices.WindowsRuntime" && ans != "Windows.Foundation.Metadata")
            continue;

        var br = reader.GetBlobReader(ca.Value);
        var g = TryParseGuidFromAttributeBlob(ref br);
        if (g.HasValue) return g;
    }
    return null;
}

/// <summary>TypeRef/TypeDef → TypeDefinitionHandle в этом модуле (по полному совпадению NS+Name).</summary>
static TypeDefinitionHandle? ResolveToTypeDefinition(MetadataReader reader, EntityHandle handle)
{
    if (handle.IsNil) return null;
    switch (handle.Kind)
    {
        case HandleKind.TypeDefinition:
            return (TypeDefinitionHandle)handle;
        case HandleKind.TypeReference:
        {
            var tr = reader.GetTypeReference((TypeReferenceHandle)handle);
            var wantNs = reader.GetString(tr.Namespace);
            var wantName = reader.GetString(tr.Name);
            foreach (var tdh in reader.TypeDefinitions)
            {
                var td = reader.GetTypeDefinition(tdh);
                if (reader.GetString(td.Name) == wantName && reader.GetString(td.Namespace) == wantNs)
                    return tdh;
            }
            return null;
        }
        default:
            return null;
    }
}

/// <summary>Цепочка интерфейсов от корня (IInspectable / первый базовый в winmd) к переданному типу.</summary>
static List<TypeDefinitionHandle>? BuildInterfaceChainToRoot(MetadataReader reader, TypeDefinitionHandle leaf)
{
    var leafTd = reader.GetTypeDefinition(leaf);
    if ((leafTd.Attributes & TypeAttributes.Interface) == 0)
    {
        Console.Error.WriteLine("Тип не является interface.");
        return null;
    }

    var leafToRoot = new List<TypeDefinitionHandle>();
    TypeDefinitionHandle? current = leaf;
    var guard = 0;
    while (current.HasValue && guard++ < 256)
    {
        leafToRoot.Add(current.Value);
        var td = reader.GetTypeDefinition(current.Value);
        var ext = td.BaseType;
        if (ext.IsNil)
            break;

        var baseTd = ResolveToTypeDefinition(reader, ext);
        if (!baseTd.HasValue)
        {
            Console.Error.WriteLine("// Не удалось разрешить BaseType в этом winmd: handle " + ext.Kind);
            break;
        }

        var baseDef = reader.GetTypeDefinition(baseTd.Value);
        // System.Object / делегаты — конец цепочки WinRT interface layout
        var baseNs = reader.GetString(baseDef.Namespace);
        var baseNm = reader.GetString(baseDef.Name);
        if ((baseDef.Attributes & TypeAttributes.Interface) == 0)
            break;
        if (baseNs == "System" && (baseNm == "Object" || baseNm == "ValueType" || baseNm == "MulticastDelegate"))
            break;

        current = baseTd.Value;
    }

    leafToRoot.Reverse();
    return leafToRoot;
}

static void DumpVtableLayout(MetadataReader reader, TypeDefinitionHandle target, string winmdPath)
{
    var chain = BuildInterfaceChainToRoot(reader, target);
    if (chain is null) return;

    var tgtTd = reader.GetTypeDefinition(target);
    var fq = reader.GetString(tgtTd.Namespace) + "." + reader.GetString(tgtTd.Name);

    Console.WriteLine("// Vtable layout для: " + fq);
    Console.WriteLine("// winmd: " + winmdPath);
    Console.WriteLine("// ABI WinRT/COM: слоты 0..2 = IUnknown (в winmd не описаны как методы интерфейса).");
    Console.WriteLine("// Слоты 3..5 = IInspectable, если тип есть в этом winmd — ниже из метаданных; иначе три строки (implicit).");
    Console.WriteLine("// Затем методы цепочки inherited WinRT-интерфейсов от базы к производному.");
    Console.WriteLine("// Для вызова по vtable на реальной сборке сверяйте с Microsoft.UI.Composition.dll / PDB.");
    Console.WriteLine();

    int slot = 0;

    Console.WriteLine($"{slot}\tIUnknown\tQueryInterface\t(implicit)");
    slot++;
    Console.WriteLine($"{slot}\tIUnknown\tAddRef\t(implicit)");
    slot++;
    Console.WriteLine($"{slot}\tIUnknown\tRelease\t(implicit)");
    slot++;

    bool chainStartsInspectable = false;
    if (chain.Count > 0)
    {
        var t0 = reader.GetTypeDefinition(chain[0]);
        chainStartsInspectable =
            reader.GetString(t0.Namespace) == "Windows.Foundation" &&
            reader.GetString(t0.Name) == "IInspectable";
    }

    if (!chainStartsInspectable)
    {
        Console.WriteLine($"{slot}\tWindows.Foundation.IInspectable\tGetIids\t(implicit ABI)");
        slot++;
        Console.WriteLine($"{slot}\tWindows.Foundation.IInspectable\tGetRuntimeClassName\t(implicit ABI)");
        slot++;
        Console.WriteLine($"{slot}\tWindows.Foundation.IInspectable\tGetTrustLevel\t(implicit ABI)");
        slot++;
    }

    foreach (var tdh in chain)
    {
        var td = reader.GetTypeDefinition(tdh);
        var declNs = reader.GetString(td.Namespace);
        var declName = reader.GetString(td.Name);

        foreach (var mh in td.GetMethods())
        {
            var m = reader.GetMethodDefinition(mh);
            if ((m.Attributes & MethodAttributes.Static) != 0)
                continue;
            var methName = reader.GetString(m.Name);
            if (methName == ".ctor")
                continue;

            Console.WriteLine($"{slot}\t{declNs}.{declName}\t{methName}");
            slot++;
        }
    }

    Console.WriteLine();
    Console.WriteLine("// Цепочка интерфейсов (база → производный):");
    foreach (var tdh in chain)
    {
        var td = reader.GetTypeDefinition(tdh);
        Console.WriteLine("//   " + reader.GetString(td.Namespace) + "." + reader.GetString(td.Name));
    }
    Console.WriteLine();
    Console.WriteLine($"// Итого слотов: {slot} (последний индекс: {slot - 1})");
}

/// <summary>Обратный поиск: какие типы WinRT/COM в winmd имеют указанный GuidAttribute.</summary>
static void ResolveIidsAcrossWinmds(IReadOnlyList<string> winmdPaths, IReadOnlyList<Guid> guids)
{
    var guidList = guids.Distinct().ToList();
    var lookup = guidList.ToDictionary(g => g, _ => new List<(string WinmdFile, string FullName, bool IsInterface)>());

    foreach (var winmdPath in winmdPaths)
    {
        if (!File.Exists(winmdPath))
        {
            Console.Error.WriteLine("// пропуск (нет файла): " + winmdPath);
            continue;
        }
        var fileLeaf = Path.GetFileName(winmdPath);
        using var fs = File.OpenRead(winmdPath);
        using var peReader = new PEReader(fs);
        if (!peReader.HasMetadata)
            continue;

        var reader = peReader.GetMetadataReader();

        foreach (var typeDefHandle in reader.TypeDefinitions)
        {
            var typeDef = reader.GetTypeDefinition(typeDefHandle);
            var guidOpt = TryGetTypeGuid(reader, typeDef);
            if (guidOpt is null)
                continue;
            if (!lookup.TryGetValue(guidOpt.Value, out var hits))
                continue;

            var ns = reader.GetString(typeDef.Namespace);
            var nm = reader.GetString(typeDef.Name);
            var isIfc = (typeDef.Attributes & TypeAttributes.Interface) != 0;
            hits.Add((fileLeaf, string.IsNullOrEmpty(ns) ? nm : $"{ns}.{nm}", isIfc));
        }
    }

    Console.WriteLine("// Reverse IID lookup (--resolve-iids)");
    Console.WriteLine("// В одном GUID могут совпасть несколько записей (класс + default interface, разные merger-файлы).");
    Console.WriteLine();
    foreach (var g in guidList.OrderBy(x => x.ToString()))
    {
        Console.WriteLine(g.ToString("B").ToUpperInvariant());
        var hits = lookup[g];
        if (hits.Count == 0)
        {
            Console.WriteLine("   (нет совпадения в указанных .winmd — см. другие merger-деметаданные или приватный COM)");
        }
        else
        {
            foreach (var (winmdFile, fullName, isIfc) in hits.OrderBy(h => h.WinmdFile).ThenBy(h => h.FullName))
            {
                var kind = isIfc ? "iface" : "type ";
                Console.WriteLine($"   [{kind}] {fullName}  ({winmdFile})");
            }
        }

        Console.WriteLine();
    }
}

static TypeDefinitionHandle? FindInterfaceByFullName(MetadataReader reader, string fullName)
{
    fullName = fullName.Trim();
    var dot = fullName.LastIndexOf('.');
    if (dot < 0)
    {
        Console.Error.WriteLine("Укажите полное имя: Namespace.IInterface (например Microsoft.UI.Composition.ICompositor)");
        return null;
    }
    var wantNs = fullName[..dot];
    var wantName = fullName[(dot + 1)..];

    foreach (var tdh in reader.TypeDefinitions)
    {
        var td = reader.GetTypeDefinition(tdh);
        if ((td.Attributes & TypeAttributes.Interface) == 0) continue;
        if (reader.GetString(td.Namespace) != wantNs) continue;
        if (reader.GetString(td.Name) != wantName) continue;
        return tdh;
    }
    return null;
}

// ─── main routing ───────────────────────────────────────────────────────────

var argv = args.ToList();

// --resolve-iids <path.winmd>... <guid>...  (GUID с/без фигурных скобок)
if (argv.Count >= 3 && argv[0] == "--resolve-iids")
{
    var paths = new List<string>();
    var guids = new List<Guid>();
    foreach (var a in argv.Skip(1))
    {
        var t = a.Trim();
        if (string.IsNullOrWhiteSpace(t))
            continue;

        var mightBeGuid = Guid.TryParse(t.Trim('{', '}', ' '), out var pg);
        var exists = File.Exists(t);
        // Путь имеет приоритет, если файл существует (даже строка-двусмысленность маловероятна).
        if (exists)
        {
            paths.Add(t);
            continue;
        }

        if (mightBeGuid)
        {
            guids.Add(pg);
            continue;
        }

        Console.Error.WriteLine("// предупреждение: не файл и не GUID: " + t);
    }

    if (paths.Count == 0 || guids.Count == 0)
    {
        Console.Error.WriteLine("Использование: dotnet run -- --resolve-iids <файл1.winmd> [<файл2.winmd> ...] <GUID> [<GUID> ...]");
        Console.Error.WriteLine("Пример:");
        Console.Error.WriteLine(
            "  dotnet run -c Release -- --resolve-iids \"%ProgramFiles(x86)%\\Windows Kits\\10\\UnionMetadata\\10.0.26100.0\\Windows.winmd\" \"C:\\Windows\\SystemApps\\...\\Microsoft.UI.winmd\" {97F7A17B-97BE-5545-9F1C-0B9D44577F57}");
        Environment.Exit(1);
    }

    ResolveIidsAcrossWinmds(paths, guids);
    Environment.Exit(0);
}

// --vtable <winmd> <Full.Interface.Name>
if (argv.Count >= 1 && argv[0] == "--vtable")
{
    if (argv.Count < 3)
    {
        Console.Error.WriteLine("Использование: dotnet run -- --vtable <путь\\Microsoft.UI.winmd> Namespace.InterfaceName");
        Console.Error.WriteLine("Пример: dotnet run -- --vtable C:\\...\\Microsoft.UI.winmd Microsoft.UI.Composition.ICompositor");
        Environment.Exit(1);
    }
    var winmd = argv[1];
    var typeFull = argv[2];
    if (!File.Exists(winmd))
    {
        Console.Error.WriteLine("File not found: " + winmd);
        Environment.Exit(1);
    }
    using var fsV = File.OpenRead(winmd);
    using var peV = new PEReader(fsV);
    if (!peV.HasMetadata)
    {
        Console.Error.WriteLine("No CLI metadata");
        Environment.Exit(2);
    }
    var readerV = peV.GetMetadataReader();
    var foundT = FindInterfaceByFullName(readerV, typeFull);
    if (!foundT.HasValue)
    {
        Console.Error.WriteLine("Интерфейс не найден: " + typeFull);
        Environment.Exit(3);
    }
    DumpVtableLayout(readerV, foundT.Value, winmd);
    Environment.Exit(0);
}

string winmdPath;
string filter;
bool listOnly = false;
if (argv.Count >= 1 && argv[0] == "--list-composition")
{
    listOnly = true;
    winmdPath = argv.Count >= 2 ? argv[1]
        : @"C:\Windows\SystemApps\Microsoft.WindowsAppRuntime.vNext.CBS_8wekyb3d8bbwe\Microsoft.UI.winmd";
    filter = "Microsoft.UI.Composition";
}
else
{
    winmdPath = argv.Count >= 1 ? argv[0]
        : @"C:\Windows\SystemApps\Microsoft.WindowsAppRuntime.vNext.CBS_8wekyb3d8bbwe\Microsoft.UI.winmd";
    filter = argv.Count >= 2 ? argv[1] : "Microsoft.UI.Composition";
}

if (!File.Exists(winmdPath))
{
    Console.Error.WriteLine("File not found: " + winmdPath);
    Environment.Exit(1);
}

using var fs = File.OpenRead(winmdPath);
using var peReader = new PEReader(fs);
if (!peReader.HasMetadata)
{
    Console.Error.WriteLine("No CLI metadata");
    Environment.Exit(2);
}

var reader = peReader.GetMetadataReader();

if (listOnly)
{
    foreach (var typeDefHandle in reader.TypeDefinitions)
    {
        var typeDef = reader.GetTypeDefinition(typeDefHandle);
        var ns = reader.GetString(typeDef.Namespace);
        var name = reader.GetString(typeDef.Name);
        if (!(ns.Contains("Composition", StringComparison.Ordinal) || name.Contains("Composition", StringComparison.Ordinal)))
            continue;
        var kind = (typeDef.Attributes & TypeAttributes.Interface) != 0 ? "iface" : "type";
        Console.WriteLine(kind + " " + ns + "." + name);
    }
    Environment.Exit(0);
}

var sb = new StringBuilder();
sb.AppendLine("// Auto-generated from: " + winmdPath);
sb.AppendLine("// Filter namespace prefix: " + filter);

var want = new HashSet<string>(StringComparer.Ordinal)
{
    "ICompositionObject", "ICompositionBrush", "ICompositor",
    "ICompositionColorBrush", "ICompositionEffectBrush",
};

var found = new HashSet<string>(StringComparer.Ordinal);

foreach (var typeDefHandle in reader.TypeDefinitions)
{
    var typeDef = reader.GetTypeDefinition(typeDefHandle);
    if ((typeDef.Attributes & TypeAttributes.Interface) == 0) continue;
    var ns = reader.GetString(typeDef.Namespace);
    if (!ns.StartsWith(filter, StringComparison.Ordinal)) continue;
    var name = reader.GetString(typeDef.Name);
    var g = TryGetTypeGuid(reader, typeDef);
    if (g is null) continue;

    sb.AppendLine();
    sb.AppendLine("// " + ns + "." + name);
    sb.AppendLine("static const GUID IID_Microsoft_UI_Composition_" + name + " =");
    sb.AppendLine("    { " + FormatGuidC(g.Value) + " };");
    if (want.Contains(name)) found.Add(name);
}

Console.Write(sb.ToString());
foreach (var n in want)
{
    if (!found.Contains(n))
        Console.Error.WriteLine("// MISSING in winmd: " + n);
}
