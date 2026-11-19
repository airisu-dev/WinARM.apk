// SignatureFile -- reads metalava's API signature format (the android.txt and
// current.txt files AOSP publishes) into an ApiModel.
//
// What the format leaves implicit and this has to restore, so that each member
// gets the descriptor an app's bytecode really names:
//
//   * Annotations, with arguments of any shape, are dropped: they never change a
//     descriptor.
//   * Format 2.0 omits "java.lang." (String, Object, Thread.State); a name with no
//     package is a java.lang one.
//   * Nested classes are written Outer.Inner, the package by its own segments;
//     names the file declares are looked up, others split at the first
//     capitalized segment.
//   * Generics are erased: a type variable becomes its first bound, found on the
//     method, then the class, then the classes enclosing an inner class.
//   * The constructor of an inner (non-static) class takes the outer instance as
//     its first parameter, which the file does not write.
//   * Varargs are arrays.
//
// Two passes: the first collects every class the file declares, so that the
// second can tell a package segment from an enclosing class.

package com.winapk.apicov;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

final class SignatureFile {
    private static final Set<String> KNOWN_FORMATS = Set.of("2.0", "3.0");
    private static final Set<String> MODIFIERS = Set.of("public", "protected", "private", "static", "final",
            "abstract", "default", "synchronized", "native", "transient", "volatile", "strictfp", "sealed",
            "non-sealed", "deprecated");
    private static final Map<String, String> PRIMITIVES = Map.of("boolean", "Z", "byte", "B", "char", "C",
            "short", "S", "int", "I", "long", "J", "float", "F", "double", "D", "void", "V");

    // A class as the first pass sees it: its header, and its member lines still unread.
    private static final class Raw {
        String pkg, simple;          // android.app, Notification.Builder
        String header;               // annotations stripped
        int line;
        Map<String, Type> typeParams = new HashMap<>();
        final List<String> memberLines = new ArrayList<>();
        final List<Integer> memberLineNos = new ArrayList<>();
    }

    // A parsed type, before erasure.
    private record Type(String name, int dims) {}

    private final ApiModel model = new ApiModel();
    private final Map<String, String> known = new HashMap<>();   // dotted -> internal, for every declared class
    private final Map<String, Raw> rawByDotted = new HashMap<>();

    static ApiModel parse(Path file) throws IOException {
        return parse(Files.readAllLines(file, StandardCharsets.UTF_8));
    }

    static ApiModel parse(List<String> lines) {
        SignatureFile f = new SignatureFile();
        List<Raw> raws = f.firstPass(lines);
        for (Raw r : raws) f.secondPass(r);
        return f.model;
    }

    // ---- pass 1: packages, class headers, member lines --------------------------------------

    private List<Raw> firstPass(List<String> lines) {
        List<Raw> raws = new ArrayList<>();
        String pkg = null;
        Raw cur = null;
        for (int i = 0; i < lines.size(); i++) {
            final int no = i + 1;
            String t = lines.get(i).trim();
            if (t.isEmpty()) continue;
            if (t.startsWith("//")) {
                if (t.startsWith("// Signature format:")) {
                    model.format = t.substring("// Signature format:".length()).trim();
                    if (!KNOWN_FORMATS.contains(model.format)) {
                        model.problems.add(new ApiModel.Problem(no, t, "unknown signature format"));
                    }
                }
                continue;
            }
            if (t.equals("}")) {
                if (cur != null) cur = null;
                else pkg = null;
                continue;
            }
            if (cur == null && pkg == null) {
                if (t.startsWith("package ") && t.endsWith("{")) {
                    pkg = t.substring(8, t.length() - 1).trim();
                } else {
                    model.problems.add(new ApiModel.Problem(no, t, "expected a package"));
                }
                continue;
            }
            if (cur == null) {
                if (!t.endsWith("{")) {
                    model.problems.add(new ApiModel.Problem(no, t, "expected a class header"));
                    continue;
                }
                String header = strip(t.substring(0, t.length() - 1)).code.trim();
                String simple = headerName(header);
                if (simple == null) {
                    model.problems.add(new ApiModel.Problem(no, t, "no class, interface, enum or @interface"));
                    continue;
                }
                cur = new Raw();
                cur.pkg = pkg;
                cur.simple = simple;
                cur.header = header;
                cur.line = no;
                raws.add(cur);
                String dotted = pkg + "." + simple;
                known.put(dotted, pkg.replace('.', '/') + "/" + simple.replace('.', '$'));
                rawByDotted.put(dotted, cur);
                continue;
            }
            cur.memberLines.add(t);
            cur.memberLineNos.add(no);
        }
        return raws;
    }

    // The class's name in its header (Notification.Builder), or null.
    private static String headerName(String header) {
        Cursor c = new Cursor(header);
        while (!c.atEnd()) {
            String w = c.word();
            if (w.isEmpty()) return null;
            if (w.equals("class") || w.equals("interface") || w.equals("enum") || w.equals("@interface")) {
                c.ws();
                return c.name();
            }
        }
        return null;
    }

    // ---- pass 2: resolve the header and every member ----------------------------------------

    private void secondPass(Raw r) {
        ApiModel.ApiClass k = new ApiModel.ApiClass();
        k.pkg = r.pkg;
        k.display = r.pkg + "." + r.simple;
        k.name = known.get(k.display);
        k.line = r.line;
        try {
            readHeader(r, k);
        } catch (RuntimeException e) {
            model.problems.add(new ApiModel.Problem(r.line, r.header, "header: " + e.getMessage()));
            return;
        }
        model.classes.put(k.name, k);
        for (int i = 0; i < r.memberLines.size(); i++) {
            String line = r.memberLines.get(i);
            int no = r.memberLineNos.get(i);
            try {
                ApiModel.ApiMember m = readMember(r, k, line);
                if (m == null) continue;   // a property: Kotlin metadata, no member of its own
                m.text = line;
                m.line = no;
                k.members.add(m);
            } catch (RuntimeException e) {
                model.problems.add(new ApiModel.Problem(no, line, e.getMessage()));
            }
        }
    }

    private void readHeader(Raw r, ApiModel.ApiClass k) {
        Cursor c = new Cursor(r.header);
        k.mods = new LinkedHashSet<>();
        String kw;
        while (true) {
            kw = c.word();
            if (kw.isEmpty()) throw new IllegalStateException("no class keyword");
            if (kw.equals("class") || kw.equals("interface") || kw.equals("enum") || kw.equals("@interface")) break;
            k.mods.add(kw);
        }
        k.kind = switch (kw) {
            case "interface" -> ApiModel.Kind.INTERFACE;
            case "enum" -> ApiModel.Kind.ENUM;
            case "@interface" -> ApiModel.Kind.ANNOTATION;
            default -> ApiModel.Kind.CLASS;
        };
        c.ws();
        c.name();
        c.ws();
        if (c.peek() == '<') r.typeParams = typeParams(c.balanced('<', '>'), List.of());

        // Whether a nested class is inner: static, an interface, enum or annotation, or a member of an
        // interface, are all static.
        int dot = r.simple.lastIndexOf('.');
        if (dot > 0 && k.kind == ApiModel.Kind.CLASS && !k.mods.contains("static")) {
            Raw outer = rawByDotted.get(r.pkg + "." + r.simple.substring(0, dot));
            String okw = outer == null ? "class" : keyword(outer.header);
            k.inner = okw.equals("class") || okw.equals("enum");
        }

        List<String> ext = new ArrayList<>(), impl = new ArrayList<>();
        List<String> into = null;
        while (true) {
            c.ws();
            if (c.atEnd()) break;
            if (c.peekWord("extends")) {
                c.word();
                into = ext;
                continue;
            }
            if (c.peekWord("implements")) {
                c.word();
                into = impl;
                continue;
            }
            if (into == null) throw new IllegalStateException("unexpected '" + c.rest() + "'");
            into.add(descriptor(c.type(), scopes(r, List.of()), false));
        }
        switch (k.kind) {
            case INTERFACE -> k.interfaces.addAll(ext);
            case ANNOTATION -> k.interfaces.add("Ljava/lang/annotation/Annotation;");
            case ENUM -> {
                k.superName = "java/lang/Enum";
                k.interfaces.addAll(impl);
            }
            default -> {
                k.superName = ext.isEmpty() ? "java/lang/Object" : internal(ext.get(0));
                k.interfaces.addAll(impl);
            }
        }
        k.interfaces.replaceAll(SignatureFile::internal);
        if (k.isInterface()) k.superName = null;
    }

    private static String keyword(String header) {
        for (String w : header.split(" ")) {
            if (w.equals("class") || w.equals("interface") || w.equals("enum") || w.equals("@interface")) return w;
        }
        return "class";
    }

    private ApiModel.ApiMember readMember(Raw r, ApiModel.ApiClass k, String line) {
        Stripped s = strip(line);
        String code = s.code.trim();
        if (code.endsWith(";")) code = code.substring(0, code.length() - 1).trim();
        Cursor c = new Cursor(code);
        String kind = c.word();
        ApiModel.ApiMember m = new ApiModel.ApiMember();
        switch (kind) {
            case "ctor" -> m.kind = ApiModel.MemberKind.CTOR;
            case "method" -> m.kind = ApiModel.MemberKind.METHOD;
            case "field" -> m.kind = ApiModel.MemberKind.FIELD;
            case "enum_constant" -> m.kind = ApiModel.MemberKind.ENUM_CONSTANT;
            case "property" -> { return null; }
            default -> throw new IllegalStateException("unknown member kind '" + kind + "'");
        }
        m.mods = new LinkedHashSet<>();
        while (true) {
            c.ws();
            if (!MODIFIERS.contains(c.peekWordText())) break;
            m.mods.add(c.word());
        }
        c.ws();
        Map<String, Type> methodParams = Map.of();
        if (c.peek() == '<') {
            methodParams = typeParams(c.balanced('<', '>'), scopes(r, List.of()));
            c.ws();
        }
        List<Map<String, Type>> sc = scopes(r, List.of(methodParams));
        switch (m.kind) {
            case CTOR -> {
                c.name();
                c.ws();
                StringBuilder d = new StringBuilder("(");
                if (k.inner) {
                    int dot = k.name.lastIndexOf('$');
                    d.append('L').append(k.name, 0, dot).append(';');
                }
                d.append(params(c, sc)).append(")V");
                m.name = "<init>";
                m.desc = d.toString();
            }
            case METHOD -> {
                Type ret = c.type();
                c.ws();
                m.name = c.name();
                c.ws();
                m.desc = "(" + params(c, sc) + ")" + descriptor(ret, sc, false);
            }
            default -> {
                Type t = c.type();
                c.ws();
                m.name = c.name();
                m.desc = descriptor(t, sc, false);
                if (s.value != null) m.value = s.value;
            }
        }
        if (m.name.isEmpty()) throw new IllegalStateException("no member name");
        return m;
    }

    // "(" params ")" at the cursor, as descriptors; whatever follows (throws, default) is ignored.
    private String params(Cursor c, List<Map<String, Type>> sc) {
        if (c.peek() != '(') throw new IllegalStateException("expected '(' at '" + c.rest() + "'");
        String inside = c.balanced('(', ')');
        inside = inside.substring(1, inside.length() - 1).trim();
        StringBuilder d = new StringBuilder();
        if (inside.isEmpty()) return "";
        for (String p : splitTop(inside)) {
            Cursor pc = new Cursor(p.trim());
            while (pc.peekWord("final")) pc.word();
            pc.ws();
            d.append(descriptor(pc.type(), sc, false));
        }
        return d.toString();
    }

    // <T extends A & B, U> -> T: A, U: Object (each bound unerased, so a bound naming another variable resolves).
    private Map<String, Type> typeParams(String decl, List<Map<String, Type>> outer) {
        Map<String, Type> out = new HashMap<>();
        String inside = decl.substring(1, decl.length() - 1).trim();
        for (String p : splitTop(inside)) {
            Cursor c = new Cursor(p.trim());
            String name = c.name();
            c.ws();
            Type bound = new Type("java.lang.Object", 0);
            if (c.peekWord("extends")) {
                c.word();
                c.ws();
                bound = c.type();
            }
            out.put(name, bound);
        }
        return out;
    }

    // The type-variable scopes for a member, innermost first: the method's, the class's, then for an
    // inner class the scopes of the classes around it.
    private List<Map<String, Type>> scopes(Raw r, List<Map<String, Type>> first) {
        List<Map<String, Type>> out = new ArrayList<>(first);
        Raw cur = r;
        while (cur != null) {
            out.add(cur.typeParams);
            int dot = cur.simple.lastIndexOf('.');
            if (dot < 0) break;
            Raw outer = rawByDotted.get(cur.pkg + "." + cur.simple.substring(0, dot));
            ApiModel.ApiClass ck = model.classes.get(known.get(cur.pkg + "." + cur.simple));
            boolean isStatic = cur.header.contains("static ") || !keyword(cur.header).equals("class")
                    || (ck != null && !ck.inner);
            if (isStatic) break;
            cur = outer;
        }
        return out;
    }

    private String descriptor(Type t, List<Map<String, Type>> sc, boolean resolving) {
        String arr = "[".repeat(t.dims);
        String prim = PRIMITIVES.get(t.name);
        if (prim != null) return arr + prim;
        if (t.name.indexOf('.') < 0) {
            for (Map<String, Type> s : sc) {
                Type bound = s.get(t.name);
                if (bound != null) {
                    if (resolving && bound.name.equals(t.name)) return arr + "Ljava/lang/Object;";
                    // T extends U: U is looked up in the same scopes; a bound is never an array here.
                    return arr + descriptor(new Type(bound.name, bound.dims), sc, true);
                }
            }
        }
        return arr + "L" + internalOf(t.name) + ";";
    }

    private static String internal(String desc) {
        return desc.startsWith("L") && desc.endsWith(";") ? desc.substring(1, desc.length() - 1) : desc;
    }

    // A source name (android.app.Notification.Builder, String, Thread.State) to an internal name.
    private String internalOf(String dotted) {
        String k = known.get(dotted);
        if (k != null) return k;
        if (Character.isUpperCase(dotted.charAt(0))) {
            k = known.get("java.lang." + dotted);
            return k != null ? k : "java/lang/" + dotted.replace('.', '$');
        }
        String[] seg = dotted.split("\\.");
        StringBuilder out = new StringBuilder();
        boolean inClass = false;
        for (int i = 0; i < seg.length; i++) {
            if (i > 0) out.append(inClass ? '$' : '/');
            if (!inClass && Character.isUpperCase(seg[i].charAt(0))) inClass = true;
            out.append(seg[i]);
        }
        return out.toString();
    }

    // ---- text helpers ------------------------------------------------------------------------

    record Stripped(String code, String value) {}

    // Removes the annotations of a line and splits off a field's constant (" = value"), both outside literals.
    static Stripped strip(String line) {
        StringBuilder out = new StringBuilder();
        int i = 0, n = line.length();
        while (i < n) {
            char ch = line.charAt(i);
            if (ch == '"' || ch == '\'') {
                int j = skipLiteral(line, i);
                out.append(line, i, j);
                i = j;
                continue;
            }
            if (ch == '@' && !line.startsWith("@interface", i)) {
                i++;
                while (i < n && (Character.isJavaIdentifierPart(line.charAt(i)) || line.charAt(i) == '.')) i++;
                if (i < n && line.charAt(i) == '(') i = skipBalanced(line, i, '(', ')');
                while (i < n && line.charAt(i) == ' ') i++;
                continue;
            }
            if (line.startsWith(" = ", i)) return new Stripped(out.toString(), constant(line.substring(i + 3)));
            out.append(ch);
            i++;
        }
        return new Stripped(out.toString(), null);
    }

    // A constant's text without the ';' and the "// 0x..." comment after it.
    private static String constant(String s) {
        s = s.trim();
        if (s.startsWith("\"") || s.startsWith("'")) return s.substring(0, skipLiteral(s, 0));
        int semi = s.indexOf(';');
        return (semi < 0 ? s : s.substring(0, semi)).trim();
    }

    static int skipLiteral(String s, int i) {
        char q = s.charAt(i);
        int j = i + 1;
        while (j < s.length()) {
            char ch = s.charAt(j);
            if (ch == '\\') { j += 2; continue; }
            if (ch == q) return j + 1;
            j++;
        }
        return s.length();
    }

    static int skipBalanced(String s, int i, char open, char close) {
        int depth = 0;
        for (int j = i; j < s.length(); j++) {
            char ch = s.charAt(j);
            if (ch == '"' || ch == '\'') { j = skipLiteral(s, j) - 1; continue; }
            if (ch == open) depth++;
            else if (ch == close && --depth == 0) return j + 1;
        }
        throw new IllegalStateException("unbalanced '" + open + "' in '" + s + "'");
    }

    // Splits on commas outside <>, () and {}.
    static List<String> splitTop(String s) {
        List<String> out = new ArrayList<>();
        int depth = 0, start = 0;
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            if (ch == '<' || ch == '(' || ch == '{') depth++;
            else if (ch == '>' || ch == ')' || ch == '}') depth--;
            else if (ch == ',' && depth == 0) {
                out.add(s.substring(start, i));
                start = i + 1;
            }
        }
        out.add(s.substring(start));
        return out;
    }

    // A cursor over one line of code with its annotations removed.
    private static final class Cursor {
        private final String s;
        private int p;

        Cursor(String s) { this.s = s; }

        boolean atEnd() { ws(); return p >= s.length(); }

        char peek() { return p < s.length() ? s.charAt(p) : '\0'; }

        String rest() { return s.substring(p); }

        void ws() { while (p < s.length() && s.charAt(p) == ' ') p++; }

        String peekWordText() {
            int q = p;
            while (q < s.length() && s.charAt(q) != ' ' && s.charAt(q) != '<' && s.charAt(q) != '(') q++;
            return s.substring(p, q);
        }

        boolean peekWord(String w) { ws(); return peekWordText().equals(w); }

        // The next space-separated word.
        String word() {
            ws();
            String w = peekWordText();
            p += w.length();
            return w;
        }

        // An identifier or a dotted name; a dot counts only when a name part follows it, so "V..." is V and varargs.
        String name() {
            int q = p;
            while (p < s.length()) {
                char ch = s.charAt(p);
                if (Character.isJavaIdentifierPart(ch)) p++;
                else if (ch == '.' && p + 1 < s.length() && Character.isJavaIdentifierStart(s.charAt(p + 1))) p++;
                else break;
            }
            return s.substring(q, p);
        }

        String balanced(char open, char close) {
            int q = p;
            p = skipBalanced(s, p, open, close);
            return s.substring(q, p);
        }

        // A type: a name, its type arguments (dropped), an inner part after them, and [] or ... suffixes.
        Type type() {
            ws();
            StringBuilder n = new StringBuilder(name());
            if (n.length() == 0) throw new IllegalStateException("expected a type at '" + rest() + "'");
            while (peek() == '<') {
                balanced('<', '>');
                if (peek() == '.' && p + 1 < s.length() && Character.isJavaIdentifierStart(s.charAt(p + 1))) {
                    p++;
                    n.append('.').append(name());
                }
            }
            int dims = 0;
            while (true) {
                if (s.startsWith("[]", p)) { p += 2; dims++; }
                else if (s.startsWith("...", p)) { p += 3; dims++; }
                else break;
            }
            return new Type(n.toString(), dims);
        }
    }
}
