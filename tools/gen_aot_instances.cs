// Generate PortAotInstances.dll, an IL-only assembly whose metadata names
// generic instances that the Mono full-AOT compiler must compile.
//
// Usage: dotnet gen_aot_instances.dll <managed dir> <list file> <output dll>
//   managed dir: the Managed directory of the game (the assemblies that the
//                list names are read from it)
//   list file:   tools/aot_instances.txt
//   output dll:  the assembly to write
//
// Mono's AOT compiler compiles all methods of the generic class of each row
// of the TypeSpec table (add_generic_instances), and each generic method
// instance that compiled code calls, with the instances that it calls. In AOT-only mode, Mono finds
// an instance in the AOT image of each loaded assembly. Thus the AOT image
// of this assembly supplies instances that the game needs and that the AOT
// compiler did not find in the game's assemblies (for example an instance
// that the game makes with reflection, or a generic virtual method).
//
// The assembly also has one static method, PortAotInstances.Instances, with
// an ldtoken instruction for each TypeSpec row and a call of each generic
// method instance (with null and zero arguments). Nothing calls it.
//
// List format (one entry per line; '#' starts a comment):
//   type    <generic type> <type argument>...
//   method  <declaring type> <type argument>... <method name> <method type argument>...
// method: an instance method or a static method; the call has a null
// reference or zero for each parameter (and for this).
//   getters <assembly> [<attribute type>]
// getters: the reflection getter instances of the properties of an
// assembly (all properties, or only the properties with the attribute).
// PropertyInfo.GetValue (MonoProperty.GetValue in mscorlib) of a property
// of a class makes a MonoProperty.Getter`2<declaring type, property type>
// delegate and calls MonoProperty.GetterAdapterFrame<declaring type,
// property type>; for a static property, StaticGetter`1<property type> and
// StaticGetterAdapterFrame<property type>. The entry adds these instances
// for each property with a getter and without parameters that a non-generic
// class (not a value type or an interface) declares.
// A type is <assembly>:<namespace>.<name> (a nested type: <outer>/<inner>).
// The number of type arguments of a type is the number after '`' in its
// name. A type argument can also be a generic instance:
// <generic type><<type argument>,...>, with no spaces, for example
// mscorlib:System.Collections.Generic.List`1<Assembly-CSharp:Console/ConsoleMessage>.
// A type argument can also be a one-dimensional array: <element type>[], for
// example mscorlib:System.Int32[].
// The method must be the only generic method with that name. The call has
// a null pointer for a by-reference parameter.
//
// The generator stops with an error when a type or method does not exist,
// or when a method name is ambiguous.
using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.IO;
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;

sealed class GenError : Exception
{
    public GenError(string m) : base(m) { }
}

// One loaded assembly of the game.
sealed class Asm
{
    public string Name = "";
    public PEReader Pe = null!;
    public MetadataReader Md = null!;
}

// A type argument or generic type: an assembly and a type definition.
readonly record struct TypeId(string Asm, TypeDefinitionHandle Def);

// How the generator pushes a value of a parameter type for a call.
enum Kind { Ref, I4, I8, R4, R8, I, Value, ByRef, Pointer, Void }

// The kind of each type of a method signature, after the substitution of
// the type arguments.
sealed class KindProvider : ISignatureTypeProvider<Kind, object?>
{
    readonly Kind[] classArgs, methodArgs;
    public KindProvider(Kind[] c, Kind[] m) { classArgs = c; methodArgs = m; }
    public Kind GetPrimitiveType(PrimitiveTypeCode t) => t switch
    {
        PrimitiveTypeCode.Int64 or PrimitiveTypeCode.UInt64 => Kind.I8,
        PrimitiveTypeCode.Single => Kind.R4,
        PrimitiveTypeCode.Double => Kind.R8,
        PrimitiveTypeCode.IntPtr or PrimitiveTypeCode.UIntPtr => Kind.I,
        PrimitiveTypeCode.String or PrimitiveTypeCode.Object => Kind.Ref,
        PrimitiveTypeCode.Void => Kind.Void,
        PrimitiveTypeCode.TypedReference => Kind.Value,
        _ => Kind.I4,
    };
    public Kind GetTypeFromDefinition(MetadataReader r, TypeDefinitionHandle h, byte raw) => raw == 0x11 ? Kind.Value : Kind.Ref;
    public Kind GetTypeFromReference(MetadataReader r, TypeReferenceHandle h, byte raw) => raw == 0x11 ? Kind.Value : Kind.Ref;
    public Kind GetTypeFromSpecification(MetadataReader r, object? c, TypeSpecificationHandle h, byte raw) =>
        r.GetTypeSpecification(h).DecodeSignature(this, c);
    public Kind GetGenericInstantiation(Kind g, ImmutableArray<Kind> a) => g;
    public Kind GetGenericTypeParameter(object? c, int i) => classArgs[i];
    public Kind GetGenericMethodParameter(object? c, int i) => methodArgs[i];
    public Kind GetSZArrayType(Kind e) => Kind.Ref;
    public Kind GetArrayType(Kind e, ArrayShape s) => Kind.Ref;
    public Kind GetPointerType(Kind e) => Kind.Pointer;
    public Kind GetByReferenceType(Kind e) => Kind.ByRef;
    public Kind GetPinnedType(Kind e) => e;
    public Kind GetFunctionPointerType(MethodSignature<Kind> s) => Kind.I;
    public Kind GetModifiedType(Kind m, Kind u, bool req) => u;
}

sealed class Generator : ISignatureTypeProvider<byte[], object?>
{
    readonly string managed;
    readonly Dictionary<string, Asm> asms = new(StringComparer.OrdinalIgnoreCase);
    readonly MetadataBuilder mb = new();
    readonly Dictionary<string, AssemblyReferenceHandle> asmRefs = new(StringComparer.OrdinalIgnoreCase);
    readonly Dictionary<(string, TypeDefinitionHandle), TypeReferenceHandle> typeRefs = new();
    readonly List<EntityHandle> tokens = new();
    // Method entries: the MethodSpec, the instance flag, the parameter kinds
    // and the return kind.
    readonly List<(MethodSpecificationHandle Spec, bool Instance, Kind[] Params, Kind Ret)> calls = new();
    // Signatures of the TypeSpec rows and MethodSpec instances, so that an
    // instance is added once.
    readonly HashSet<string> added = new();

    // Element types of the primitive types of mscorlib (ECMA-335 II.23.1.16).
    static readonly Dictionary<string, byte> Primitives = new()
    {
        ["System.Boolean"] = 0x02, ["System.Char"] = 0x03, ["System.SByte"] = 0x04, ["System.Byte"] = 0x05,
        ["System.Int16"] = 0x06, ["System.UInt16"] = 0x07, ["System.Int32"] = 0x08, ["System.UInt32"] = 0x09,
        ["System.Int64"] = 0x0A, ["System.UInt64"] = 0x0B, ["System.Single"] = 0x0C, ["System.Double"] = 0x0D,
        ["System.String"] = 0x0E, ["System.IntPtr"] = 0x18, ["System.UIntPtr"] = 0x19, ["System.Object"] = 0x1C,
    };

    public Generator(string managedDir) { managed = managedDir; }

    Asm Load(string name)
    {
        if (asms.TryGetValue(name, out var a)) return a;
        string path = Path.Combine(managed, name + ".dll");
        if (!File.Exists(path)) throw new GenError($"assembly {name} is not in {managed}");
        var pe = new PEReader(File.OpenRead(path));
        a = new Asm { Name = name, Pe = pe, Md = pe.GetMetadataReader() };
        string defName = a.Md.GetString(a.Md.GetAssemblyDefinition().Name);
        if (!string.Equals(defName, name, StringComparison.OrdinalIgnoreCase))
            throw new GenError($"{path} is the assembly {defName}, not {name}");
        asms[name] = a;
        return a;
    }

    static string FullName(MetadataReader md, TypeDefinition t)
    {
        string n = md.GetString(t.Name);
        if (t.GetDeclaringType().IsNil == false)
            return FullName(md, md.GetTypeDefinition(t.GetDeclaringType())) + "/" + n;
        string ns = md.GetString(t.Namespace);
        return ns.Length == 0 ? n : ns + "." + n;
    }

    TypeId FindType(string spec)
    {
        int c = spec.IndexOf(':');
        if (c <= 0 || c == spec.Length - 1) throw new GenError($"\"{spec}\" is not <assembly>:<type>");
        string asmName = spec[..c], typeName = spec[(c + 1)..];
        Asm a = Load(asmName);
        foreach (var h in a.Md.TypeDefinitions)
            if (FullName(a.Md, a.Md.GetTypeDefinition(h)) == typeName) return new TypeId(asmName, h);
        throw new GenError($"type {typeName} is not in {asmName}");
    }

    AssemblyReferenceHandle AsmRef(string name)
    {
        if (asmRefs.TryGetValue(name, out var h)) return h;
        Asm a = Load(name);
        var d = a.Md.GetAssemblyDefinition();
        var key = a.Md.GetBlobBytes(d.PublicKey);
        h = mb.AddAssemblyReference(mb.GetOrAddString(a.Md.GetString(d.Name)), d.Version,
            d.Culture.IsNil ? default : mb.GetOrAddString(a.Md.GetString(d.Culture)),
            key.Length == 0 ? default : mb.GetOrAddBlob(key),
            key.Length == 0 ? 0 : AssemblyFlags.PublicKey, default);
        asmRefs[name] = h;
        return h;
    }

    TypeReferenceHandle TypeRef(TypeId t)
    {
        if (typeRefs.TryGetValue((t.Asm, t.Def), out var h)) return h;
        var md = Load(t.Asm).Md;
        var d = md.GetTypeDefinition(t.Def);
        EntityHandle scope = d.GetDeclaringType().IsNil
            ? AsmRef(t.Asm)
            : TypeRef(new TypeId(t.Asm, d.GetDeclaringType()));
        h = mb.AddTypeReference(scope, d.GetDeclaringType().IsNil ? mb.GetOrAddString(md.GetString(d.Namespace)) : default,
            mb.GetOrAddString(md.GetString(d.Name)));
        typeRefs[(t.Asm, t.Def)] = h;
        return h;
    }

    bool IsValueType(TypeId t)
    {
        var md = Load(t.Asm).Md;
        var b = md.GetTypeDefinition(t.Def).BaseType;
        if (b.IsNil) return false;
        string ns, n;
        if (b.Kind == HandleKind.TypeReference)
        {
            var r = md.GetTypeReference((TypeReferenceHandle)b);
            ns = md.GetString(r.Namespace);
            n = md.GetString(r.Name);
        }
        else if (b.Kind == HandleKind.TypeDefinition)
        {
            var r = md.GetTypeDefinition((TypeDefinitionHandle)b);
            ns = md.GetString(r.Namespace);
            n = md.GetString(r.Name);
        }
        else return false;
        string full = ns + "." + n;
        if (full == "System.Enum") return true;
        return full == "System.ValueType" && FullName(md, md.GetTypeDefinition(t.Def)) != "System.Enum";
    }

    static void Compressed(List<byte> o, int v)
    {
        var b = new BlobBuilder();
        b.WriteCompressedInteger(v);
        o.AddRange(b.ToArray());
    }

    byte[] ClassOrValue(TypeId t, bool valueType)
    {
        var o = new List<byte> { valueType ? (byte)0x11 : (byte)0x12 };
        Compressed(o, CodedIndex.TypeDefOrRefOrSpec(TypeRef(t)));
        return o.ToArray();
    }

    (byte[] Sig, Kind Kind) ArgTypeKind(TypeId t)
    {
        var md = Load(t.Asm).Md;
        string full = FullName(md, md.GetTypeDefinition(t.Def));
        if (t.Asm.Equals("mscorlib", StringComparison.OrdinalIgnoreCase) && Primitives.TryGetValue(full, out byte e))
            return (new[] { e }, new KindProvider(Array.Empty<Kind>(), Array.Empty<Kind>()).GetPrimitiveType((PrimitiveTypeCode)e));
        if (md.GetTypeDefinition(t.Def).GetGenericParameters().Count != 0)
            throw new GenError($"type argument {full} is generic; give an instance in the list instead");
        bool v = IsValueType(t);
        return (ClassOrValue(t, v), v ? Kind.Value : Kind.Ref);
    }

    // The signature and kind of a type argument of the list: a type, a
    // generic instance <generic type><<argument>,...>, or a one-dimensional
    // array <element type>[] (ELEMENT_TYPE_SZARRAY, ECMA-335 II.23.2.12).
    (byte[] Sig, Kind Kind) ArgSpec(string spec)
    {
        if (spec.EndsWith("[]"))
        {
            if (spec.Length == 2) throw new GenError("\"[]\" has no element type");
            var o = new List<byte> { 0x1D };
            o.AddRange(ArgSpec(spec[..^2]).Sig);
            return (o.ToArray(), Kind.Ref);
        }
        int lt = spec.IndexOf('<');
        if (lt < 0) return ArgTypeKind(FindType(spec));
        if (lt == 0 || spec[^1] != '>') throw new GenError($"\"{spec}\" is not <generic type><<argument>,...>");
        TypeId gen = FindType(spec[..lt]);
        var args = new List<byte[]>();
        int depth = 0, start = lt + 1;
        for (int i = lt + 1; i < spec.Length - 1; i++)
        {
            if (spec[i] == '<') depth++;
            else if (spec[i] == '>') { if (--depth < 0) throw new GenError($"\"{spec}\" has an unmatched '>'"); }
            else if (spec[i] == ',' && depth == 0) { args.Add(ArgSpec(spec[start..i]).Sig); start = i + 1; }
        }
        if (depth != 0) throw new GenError($"\"{spec}\" has an unmatched '<'");
        args.Add(ArgSpec(spec[start..^1]).Sig);
        return (GenericInst(gen, args), IsValueType(gen) ? Kind.Value : Kind.Ref);
    }

    static int Arity(MetadataReader md, TypeDefinitionHandle h) => md.GetTypeDefinition(h).GetGenericParameters().Count;

    byte[] GenericInst(TypeId gen, List<byte[]> args)
    {
        var md = Load(gen.Asm).Md;
        if (Arity(md, gen.Def) != args.Count)
            throw new GenError($"{FullName(md, md.GetTypeDefinition(gen.Def))} has {Arity(md, gen.Def)} type parameters, not {args.Count}");
        var o = new List<byte> { 0x15 };
        o.AddRange(ClassOrValue(gen, IsValueType(gen)));
        Compressed(o, args.Count);
        foreach (var a in args) o.AddRange(a);
        return o.ToArray();
    }

    // ---- ISignatureTypeProvider: re-encode a signature of a game assembly
    // with the type references of this assembly.

    string decodingAsm = "";

    public byte[] GetPrimitiveType(PrimitiveTypeCode typeCode) => new[] { (byte)typeCode };

    public byte[] GetTypeFromDefinition(MetadataReader reader, TypeDefinitionHandle handle, byte rawTypeKind) =>
        ClassOrValue(new TypeId(decodingAsm, handle), rawTypeKind == 0x11);

    public byte[] GetTypeFromReference(MetadataReader reader, TypeReferenceHandle handle, byte rawTypeKind) =>
        ClassOrValue(Resolve(decodingAsm, handle), rawTypeKind == 0x11);

    public byte[] GetTypeFromSpecification(MetadataReader reader, object? genericContext, TypeSpecificationHandle handle, byte rawTypeKind) =>
        reader.GetTypeSpecification(handle).DecodeSignature(this, genericContext);

    public byte[] GetGenericInstantiation(byte[] genericType, ImmutableArray<byte[]> typeArguments)
    {
        var o = new List<byte> { 0x15 };
        o.AddRange(genericType);
        Compressed(o, typeArguments.Length);
        foreach (var a in typeArguments) o.AddRange(a);
        return o.ToArray();
    }

    public byte[] GetGenericTypeParameter(object? genericContext, int index)
    {
        var o = new List<byte> { 0x13 };
        Compressed(o, index);
        return o.ToArray();
    }

    public byte[] GetGenericMethodParameter(object? genericContext, int index)
    {
        var o = new List<byte> { 0x1E };
        Compressed(o, index);
        return o.ToArray();
    }

    public byte[] GetSZArrayType(byte[] elementType) => Prefix(0x1D, elementType);
    public byte[] GetPointerType(byte[] elementType) => Prefix(0x0F, elementType);
    public byte[] GetByReferenceType(byte[] elementType) => Prefix(0x10, elementType);
    public byte[] GetPinnedType(byte[] elementType) => Prefix(0x45, elementType);

    public byte[] GetArrayType(byte[] elementType, ArrayShape shape)
    {
        var o = new List<byte> { 0x14 };
        o.AddRange(elementType);
        Compressed(o, shape.Rank);
        Compressed(o, shape.Sizes.Length);
        foreach (int s in shape.Sizes) Compressed(o, s);
        Compressed(o, shape.LowerBounds.Length);
        foreach (int l in shape.LowerBounds)
        {
            var b = new BlobBuilder();
            b.WriteCompressedSignedInteger(l);
            o.AddRange(b.ToArray());
        }
        return o.ToArray();
    }

    public byte[] GetFunctionPointerType(MethodSignature<byte[]> signature) =>
        throw new GenError("function pointer types in a signature are not supported");

    public byte[] GetModifiedType(byte[] modifier, byte[] unmodifiedType, bool isRequired) =>
        throw new GenError("custom modifiers in a signature are not supported");

    public byte[] GetTypeFromSerializedName(string name) => throw new GenError("serialized type names are not supported");
    public PrimitiveTypeCode GetUnderlyingEnumType(byte[] type) => throw new GenError("enum types in attributes are not supported");
    public bool IsSystemType(byte[] type) => false;
    public byte[] GetSystemType() => throw new GenError("System.Type in attributes is not supported");

    static byte[] Prefix(byte p, byte[] e)
    {
        var o = new byte[e.Length + 1];
        o[0] = p;
        e.CopyTo(o, 1);
        return o;
    }

    // The definition of a type reference of assembly asmName.
    TypeId Resolve(string asmName, TypeReferenceHandle h)
    {
        var md = Load(asmName).Md;
        var r = md.GetTypeReference(h);
        string name = md.GetString(r.Name), ns = md.GetString(r.Namespace);
        switch (r.ResolutionScope.Kind)
        {
        case HandleKind.AssemblyReference:
        {
            string target = md.GetString(md.GetAssemblyReference((AssemblyReferenceHandle)r.ResolutionScope).Name);
            var tmd = Load(target).Md;
            foreach (var d in tmd.TypeDefinitions)
            {
                var td = tmd.GetTypeDefinition(d);
                if (td.GetDeclaringType().IsNil && tmd.GetString(td.Name) == name && tmd.GetString(td.Namespace) == ns)
                    return new TypeId(target, d);
            }
            throw new GenError($"type {ns}.{name} is not in {target} (a type forwarder is not supported)");
        }
        case HandleKind.TypeReference:
        {
            TypeId outer = Resolve(asmName, (TypeReferenceHandle)r.ResolutionScope);
            var omd = Load(outer.Asm).Md;
            foreach (var n in omd.GetTypeDefinition(outer.Def).GetNestedTypes())
                if (omd.GetString(omd.GetTypeDefinition(n).Name) == name) return new TypeId(outer.Asm, n);
            throw new GenError($"nested type {name} is not in {FullName(omd, omd.GetTypeDefinition(outer.Def))}");
        }
        default:
            throw new GenError($"the resolution scope of type reference {ns}.{name} in {asmName} is not supported");
        }
    }

    byte[] MethodSig(string asmName, MethodDefinition def)
    {
        Load(asmName);
        decodingAsm = asmName;
        var sig = def.DecodeSignature(this, null);
        var o = new List<byte> { sig.Header.RawValue };
        if (sig.Header.IsGeneric) Compressed(o, sig.GenericParameterCount);
        Compressed(o, sig.ParameterTypes.Length);
        o.AddRange(sig.ReturnType);
        foreach (var p in sig.ParameterTypes) o.AddRange(p);
        return o.ToArray();
    }

    // ---- list entries

    void AddType(string[] f, int line)
    {
        if (f.Length < 2) throw new GenError($"line {line}: type needs a generic type");
        TypeId gen = FindType(f[1]);
        var args = new List<byte[]>();
        for (int i = 2; i < f.Length; i++) args.Add(ArgSpec(f[i]).Sig);
        var blob = new BlobBuilder();
        blob.WriteBytes(GenericInst(gen, args));
        tokens.Add(mb.AddTypeSpecification(mb.GetOrAddBlob(blob)));
    }

    void AddMethod(string[] f, int line)
    {
        if (f.Length < 3) throw new GenError($"line {line}: method needs a declaring type and a method name");
        TypeId decl = FindType(f[1]);
        var md = Load(decl.Asm).Md;
        int arity = Arity(md, decl.Def), i = 2;
        var classArgs = new List<byte[]>();
        var classKinds = new List<Kind>();
        for (int k = 0; k < arity; k++, i++)
        {
            if (i >= f.Length) throw new GenError($"line {line}: {f[1]} needs {arity} type arguments");
            var a = ArgSpec(f[i]);
            classArgs.Add(a.Sig);
            classKinds.Add(a.Kind);
        }
        if (IsValueType(decl)) throw new GenError($"line {line}: a method of the value type {f[1]} is not supported");
        if (i >= f.Length) throw new GenError($"line {line}: the method name is missing");
        string name = f[i++];
        MethodDefinition? found = null;
        foreach (var mh in md.GetTypeDefinition(decl.Def).GetMethods())
        {
            var m = md.GetMethodDefinition(mh);
            if (md.GetString(m.Name) != name || m.GetGenericParameters().Count == 0) continue;
            if (found != null) throw new GenError($"line {line}: {f[1]} has more than one generic method {name}");
            found = m;
        }
        if (found == null) throw new GenError($"line {line}: {f[1]} has no generic method {name}");
        var methodArgs = new List<byte[]>();
        var methodKinds = new List<Kind>();
        for (; i < f.Length; i++)
        {
            var a = ArgSpec(f[i]);
            methodArgs.Add(a.Sig);
            methodKinds.Add(a.Kind);
        }
        int marity = found.Value.GetGenericParameters().Count;
        if (methodArgs.Count != marity)
            throw new GenError($"line {line}: {name} has {marity} type parameters, not {methodArgs.Count}");

        EntityHandle parent;
        if (arity == 0)
            parent = TypeRef(decl);
        else
        {
            var tb = new BlobBuilder();
            tb.WriteBytes(GenericInst(decl, classArgs));
            parent = mb.AddTypeSpecification(mb.GetOrAddBlob(tb));
        }
        var sb = new BlobBuilder();
        sb.WriteBytes(MethodSig(decl.Asm, found.Value));
        var mref = mb.AddMemberReference(parent, mb.GetOrAddString(name), mb.GetOrAddBlob(sb));
        var ib = new BlobBuilder();
        ib.WriteByte(0x0A);
        ib.WriteCompressedInteger(methodArgs.Count);
        foreach (var a in methodArgs) ib.WriteBytes(a);
        var ksig = found.Value.DecodeSignature(new KindProvider(classKinds.ToArray(), methodKinds.ToArray()), null);
        foreach (var k in ksig.ParameterTypes)
            if (k is Kind.Value or Kind.Pointer)
                throw new GenError($"line {line}: {name} has a parameter of kind {k}, which is not supported");
        calls.Add((mb.AddMethodSpecification(mref, mb.GetOrAddBlob(ib)), ksig.Header.IsInstance, new List<Kind>(ksig.ParameterTypes).ToArray(),
                   ksig.ReturnType));
    }

    // The only generic method name of type decl.
    MethodDefinition GenericMethod(TypeId decl, string name)
    {
        var md = Load(decl.Asm).Md;
        MethodDefinition? found = null;
        foreach (var mh in md.GetTypeDefinition(decl.Def).GetMethods())
        {
            var m = md.GetMethodDefinition(mh);
            if (md.GetString(m.Name) != name || m.GetGenericParameters().Count == 0) continue;
            if (found != null) throw new GenError($"{FullName(md, md.GetTypeDefinition(decl.Def))} has more than one generic method {name}");
            found = m;
        }
        return found ?? throw new GenError($"{FullName(md, md.GetTypeDefinition(decl.Def))} has no generic method {name}");
    }

    // A TypeSpec row for a generic instance (once).
    void AddTypeSpec(byte[] inst)
    {
        if (!added.Add("T" + Convert.ToHexString(inst))) return;
        var blob = new BlobBuilder();
        blob.WriteBytes(inst);
        tokens.Add(mb.AddTypeSpecification(mb.GetOrAddBlob(blob)));
    }

    // A call of the static generic method m of the non-generic class decl
    // with the type arguments args (once). The parameters of m are
    // references.
    void AddStaticCall(TypeId decl, MethodDefinition m, List<byte[]> args)
    {
        var md = Load(decl.Asm).Md;
        string name = md.GetString(m.Name);
        var ib = new BlobBuilder();
        ib.WriteByte(0x0A);
        ib.WriteCompressedInteger(args.Count);
        foreach (var a in args) ib.WriteBytes(a);
        if (!added.Add("M" + name + Convert.ToHexString(ib.ToArray()))) return;
        var kinds = new Kind[args.Count];
        Array.Fill(kinds, Kind.Ref);
        var ksig = m.DecodeSignature(new KindProvider(Array.Empty<Kind>(), kinds), null);
        if (ksig.Header.IsInstance) throw new GenError($"{name} is not static");
        foreach (var k in ksig.ParameterTypes)
            if (k != Kind.Ref) throw new GenError($"{name} has a parameter of kind {k}, which is not supported");
        var sb = new BlobBuilder();
        sb.WriteBytes(MethodSig(decl.Asm, m));
        var mref = mb.AddMemberReference(TypeRef(decl), mb.GetOrAddString(name), mb.GetOrAddBlob(sb));
        calls.Add((mb.AddMethodSpecification(mref, mb.GetOrAddBlob(ib)), false, new List<Kind>(ksig.ParameterTypes).ToArray(), ksig.ReturnType));
    }

    // True if the custom attributes of a property include one of type attr.
    bool HasAttribute(string asmName, PropertyDefinition p, TypeId attr)
    {
        var md = Load(asmName).Md;
        foreach (var ch in p.GetCustomAttributes())
        {
            var ctor = md.GetCustomAttribute(ch).Constructor;
            EntityHandle parent;
            if (ctor.Kind == HandleKind.MethodDefinition)
                parent = md.GetMethodDefinition((MethodDefinitionHandle)ctor).GetDeclaringType();
            else if (ctor.Kind == HandleKind.MemberReference)
                parent = md.GetMemberReference((MemberReferenceHandle)ctor).Parent;
            else continue;
            TypeId t;
            if (parent.Kind == HandleKind.TypeDefinition) t = new TypeId(asmName, (TypeDefinitionHandle)parent);
            else if (parent.Kind == HandleKind.TypeReference) t = Resolve(asmName, (TypeReferenceHandle)parent);
            else continue;
            if (string.Equals(t.Asm, attr.Asm, StringComparison.OrdinalIgnoreCase) && t.Def == attr.Def) return true;
        }
        return false;
    }

    void AddGetters(string[] f, int line)
    {
        if (f.Length < 2 || f.Length > 3) throw new GenError($"line {line}: getters needs an assembly and an optional attribute type");
        string asmName = f[1];
        var md = Load(asmName).Md;
        TypeId? attr = f.Length == 3 ? FindType(f[2]) : null;
        TypeId mono = FindType("mscorlib:System.Reflection.MonoProperty");
        TypeId getter = FindType("mscorlib:System.Reflection.MonoProperty/Getter`2");
        TypeId staticGetter = FindType("mscorlib:System.Reflection.MonoProperty/StaticGetter`1");
        MethodDefinition frame = GenericMethod(mono, "GetterAdapterFrame");
        MethodDefinition staticFrame = GenericMethod(mono, "StaticGetterAdapterFrame");
        int n = 0, skipped = 0;
        foreach (var th in md.TypeDefinitions)
        {
            var td = md.GetTypeDefinition(th);
            var t = new TypeId(asmName, th);
            if ((td.Attributes & TypeAttributes.Interface) != 0 || td.GetGenericParameters().Count != 0 || IsValueType(t))
                continue;
            if (FullName(md, td) == "<Module>") continue;
            foreach (var ph in td.GetProperties())
            {
                var p = md.GetPropertyDefinition(ph);
                var get = p.GetAccessors().Getter;
                if (get.IsNil) continue;
                if (attr != null && !HasAttribute(asmName, p, attr.Value)) continue;
                var gm = md.GetMethodDefinition(get);
                decodingAsm = asmName;
                var sig = gm.DecodeSignature(this, null);
                byte r0 = sig.ReturnType[0];
                if (sig.ParameterTypes.Length != 0 || r0 == 0x0F || r0 == 0x10 || r0 == 0x01 || r0 == 0x16)
                {
                    skipped++; /* an indexer, or a pointer, byref, void or TypedReference type */
                    continue;
                }
                if ((gm.Attributes & MethodAttributes.Static) != 0)
                {
                    AddTypeSpec(GenericInst(staticGetter, new List<byte[]> { sig.ReturnType }));
                    AddStaticCall(mono, staticFrame, new List<byte[]> { sig.ReturnType });
                }
                else
                {
                    var args = new List<byte[]> { ClassOrValue(t, false), sig.ReturnType };
                    AddTypeSpec(GenericInst(getter, args));
                    AddStaticCall(mono, frame, args);
                }
                n++;
            }
        }
        Console.WriteLine($"line {line}: getters {asmName}{(attr != null ? " " + f[2] : "")}: {n} properties, {skipped} indexers or pointer types");
    }

    public void Run(string listPath, string outPath)
    {
        var lines = File.ReadAllLines(listPath);

        // The module version ID comes from the list, so the same list gives
        // the same assembly (and the same AOT image).
        var mvid = new Guid(System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(listPath)).AsSpan(0, 16));
        mb.AddModule(0, mb.GetOrAddString("PortAotInstances.dll"), mb.GetOrAddGuid(mvid), default, default);
        mb.AddAssembly(mb.GetOrAddString("PortAotInstances"), new Version(1, 0, 0, 0), default, default, 0, AssemblyHashAlgorithm.Sha1);

        for (int n = 0; n < lines.Length; n++)
        {
            string l = lines[n];
            int c = l.IndexOf('#');
            if (c >= 0) l = l[..c];
            var f = l.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (f.Length == 0) continue;
            if (f[0] == "type") AddType(f, n + 1);
            else if (f[0] == "method") AddMethod(f, n + 1);
            else if (f[0] == "getters") AddGetters(f, n + 1);
            else throw new GenError($"line {n + 1}: unknown entry \"{f[0]}\"");
        }
        if (tokens.Count + calls.Count == 0) throw new GenError($"{listPath} has no entries");

        // The method body: ldtoken <row>; pop for each row; ret.
        var il = new BlobBuilder();
        var enc = new InstructionEncoder(il);
        // The AOT compiler compiles each generic class of a TypeSpec row, but
        // it compiles a generic method instance only for a call in compiled
        // code (ldtoken and ldftn are not sufficient). Thus each method entry
        // is a call with null and zero arguments. Nothing runs this code.
        int maxStack = 1;
        foreach (var t in tokens)
        {
            enc.OpCode(ILOpCode.Ldtoken);
            enc.Token(t);
            enc.OpCode(ILOpCode.Pop);
        }
        foreach (var c in calls)
        {
            if (c.Instance) enc.OpCode(ILOpCode.Ldnull); // this
            foreach (var k in c.Params)
            {
                switch (k)
                {
                case Kind.Ref: enc.OpCode(ILOpCode.Ldnull); break;
                case Kind.I4: enc.LoadConstantI4(0); break;
                case Kind.I8: enc.LoadConstantI8(0); break;
                case Kind.R4: enc.LoadConstantR4(0); break;
                case Kind.R8: enc.LoadConstantR8(0); break;
                case Kind.I: enc.LoadConstantI4(0); enc.OpCode(ILOpCode.Conv_i); break;
                // A null pointer: Mono accepts an unmanaged pointer for a
                // by-reference parameter. Nothing runs this code.
                case Kind.ByRef: enc.LoadConstantI4(0); enc.OpCode(ILOpCode.Conv_u); break;
                default: throw new GenError($"parameter kind {k} is not supported");
                }
            }
            maxStack = Math.Max(maxStack, c.Params.Length + (c.Instance ? 1 : 0));
            enc.Call(c.Spec);
            if (c.Ret != Kind.Void) enc.OpCode(ILOpCode.Pop);
        }
        enc.OpCode(ILOpCode.Ret);
        var bodies = new MethodBodyStreamEncoder(new BlobBuilder());
        int body = bodies.AddMethodBody(enc, maxStack: maxStack);

        var msig = new BlobBuilder();
        new BlobEncoder(msig).MethodSignature().Parameters(0, r => r.Void(), p => { });
        var method = mb.AddMethodDefinition(MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig,
            MethodImplAttributes.IL, mb.GetOrAddString("Instances"), mb.GetOrAddBlob(msig), body, MetadataTokens.ParameterHandle(1));

        TypeId obj = FindType("mscorlib:System.Object");
        mb.AddTypeDefinition(default, default, mb.GetOrAddString("<Module>"), default,
            MetadataTokens.FieldDefinitionHandle(1), MetadataTokens.MethodDefinitionHandle(1));
        mb.AddTypeDefinition(TypeAttributes.Public | TypeAttributes.Abstract | TypeAttributes.Sealed | TypeAttributes.BeforeFieldInit,
            default, mb.GetOrAddString("PortAotInstances"), TypeRef(obj),
            MetadataTokens.FieldDefinitionHandle(1), method);

        var root = new MetadataRootBuilder(mb, "v2.0.50727");
        var pe = new ManagedPEBuilder(PEHeaderBuilder.CreateLibraryHeader(), root, bodies.Builder,
            flags: CorFlags.ILOnly);
        var output = new BlobBuilder();
        pe.Serialize(output);
        using var fs = File.Create(outPath);
        output.WriteContentTo(fs);
        Console.WriteLine($"{outPath}: {tokens.Count} generic classes, {calls.Count} generic methods");
    }
}

static class Program
{
    static int Main(string[] args)
    {
        if (args.Length != 3)
        {
            Console.Error.WriteLine("usage: gen_aot_instances <managed dir> <list file> <output dll>");
            return 2;
        }
        try
        {
            new Generator(args[0]).Run(args[1], args[2]);
            return 0;
        }
        catch (GenError e)
        {
            Console.Error.WriteLine("error: " + e.Message);
            return 1;
        }
    }
}
