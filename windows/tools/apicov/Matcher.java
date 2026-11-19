// Matcher -- gives every class and member of the API a state: whether an app
// that uses it links, and if not, the error it would get. Lookups follow the
// JVM's resolution rules (JVMS 5.4.3): a method in the class and its
// superclasses, then its superinterfaces; a field in the class, its
// superinterfaces, then its superclass. A synthetic bridge counts, since a
// call links to it.

package com.winapk.apicov;

import jdk.internal.org.objectweb.asm.Opcodes;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Objects;
import java.util.Set;

final class Matcher {
    enum State {
        OK("ok", "Exists with the right shape"),
        MISSING_CLASS("missing-class", "The class is absent: NoClassDefFoundError"),
        MISSING_MEMBER("missing-member",
                "The class exists, the member does not: NoSuchMethodError, NoSuchFieldError"),
        WRONG_KIND("wrong-kind",
                "Interface where the API has a class, or the reverse: IncompatibleClassChangeError"),
        WRONG_RETURN("wrong-return", "Same name and parameters, another return type: NoSuchMethodError"),
        WRONG_TYPE("wrong-type", "Same name, another field type: NoSuchFieldError"),
        WRONG_STATIC("wrong-static",
                "Static where the API has an instance member, or the reverse: IncompatibleClassChangeError"),
        WRONG_ACCESS("wrong-access", "Less visible than the API: IllegalAccessError"),
        WRONG_FINAL("wrong-final",
                "Final where the API lets an app override or subclass it: the app's class fails to load"),
        WRONG_ABSTRACT("wrong-abstract",
                "Abstract where the API is concrete: AbstractMethodError, InstantiationError"),
        WRONG_CONSTANT("wrong-constant", "A constant with another value: no error, wrong behaviour"),
        WRONG_HIERARCHY("wrong-hierarchy",
                "A superclass or interface of the API is missing: failed casts and overrides");

        final String id, meaning;

        State(String id, String meaning) {
            this.id = id;
            this.meaning = meaning;
        }
    }

    record Result(State state, String detail) {
        static final Result OK = new Result(State.OK, null);
    }

    private final ClassPathIndex index;

    Matcher(ClassPathIndex index) { this.index = index; }

    // ---- classes ----------------------------------------------------------------------------

    Result matchClass(ApiModel.ApiClass k) {
        ClassPathIndex.ClassInfo ci = index.get(k.name);
        if (ci == null) return new Result(State.MISSING_CLASS, null);
        if (ci.isInterface() != k.isInterface()) {
            return new Result(State.WRONG_KIND, ci.isInterface() ? "an interface here" : "a class here");
        }
        if ((ci.access & Opcodes.ACC_PUBLIC) == 0) return new Result(State.WRONG_ACCESS, "not public here");
        boolean apiFinal = k.mods.contains("final") || k.kind == ApiModel.Kind.ENUM;
        if (!apiFinal && !k.isInterface() && (ci.access & Opcodes.ACC_FINAL) != 0) {
            return new Result(State.WRONG_FINAL, "final here");
        }
        if (k.kind == ApiModel.Kind.CLASS && !k.mods.contains("abstract") && (ci.access & Opcodes.ACC_ABSTRACT) != 0
                && k.members.stream().anyMatch(m -> m.kind == ApiModel.MemberKind.CTOR)) {
            return new Result(State.WRONG_ABSTRACT, "abstract here");
        }
        List<String> missing = new ArrayList<>();
        if (k.superName != null && !k.superName.equals("java/lang/Object") && !superclasses(ci).contains(k.superName)) {
            missing.add("extends " + k.superName);
        }
        Set<String> have = allInterfaces(ci);
        for (String i : k.interfaces) if (!have.contains(i)) missing.add("implements " + i);
        if (!missing.isEmpty()) return new Result(State.WRONG_HIERARCHY, String.join(", ", missing));
        return Result.OK;
    }

    // ---- members ----------------------------------------------------------------------------

    Result matchMember(ApiModel.ApiClass k, ApiModel.ApiMember m) {
        ClassPathIndex.ClassInfo ci = index.get(k.name);
        if (ci == null) return new Result(State.MISSING_CLASS, null);
        return m.isMethod() ? matchMethod(k, ci, m) : matchField(ci, m);
    }

    private record Found(ClassPathIndex.ClassInfo owner, ClassPathIndex.MemberInfo member) {}

    private Result matchMethod(ApiModel.ApiClass k, ClassPathIndex.ClassInfo ci, ApiModel.ApiMember m) {
        Found f;
        if (m.kind == ApiModel.MemberKind.CTOR) {
            ClassPathIndex.MemberInfo mi = ci.methods.get("<init>" + m.desc);
            f = mi == null ? null : new Found(ci, mi);
        } else {
            f = findInClasses(ci, m.name + m.desc);
            if (f == null) {
                f = findInInterfaces(ci, m.name + m.desc);
            }
        }
        if (f == null) {
            String other = otherReturn(ci, m);
            return other != null ? new Result(State.WRONG_RETURN, "returns " + other + " here")
                    : new Result(State.MISSING_MEMBER, null);
        }
        int a = f.member.access;
        boolean apiStatic = m.mods.contains("static");
        if (apiStatic != ((a & Opcodes.ACC_STATIC) != 0)) {
            return new Result(State.WRONG_STATIC, apiStatic ? "an instance method here" : "static here");
        }
        Result access = access(m, a);
        if (access != null) return access;
        boolean apiFinal = m.mods.contains("final") || k.mods.contains("final") || k.kind == ApiModel.Kind.ENUM;
        if (!apiFinal && !apiStatic && m.kind == ApiModel.MemberKind.METHOD && (a & Opcodes.ACC_FINAL) != 0) {
            return new Result(State.WRONG_FINAL, "final here" + where(f.owner, ci));
        }
        boolean apiAbstract = m.mods.contains("abstract") || (k.isInterface() && !m.mods.contains("default")
                && !apiStatic);
        if (!apiAbstract && (a & Opcodes.ACC_ABSTRACT) != 0) {
            return new Result(State.WRONG_ABSTRACT, "abstract here" + where(f.owner, ci));
        }
        return Result.OK;
    }

    private Result matchField(ClassPathIndex.ClassInfo ci, ApiModel.ApiMember m) {
        Found f = findField(ci, m.name + ":" + m.desc, new HashSet<>());
        if (f == null) {
            String other = otherFieldType(ci, m.name, new HashSet<>());
            return other != null ? new Result(State.WRONG_TYPE, "of type " + other + " here")
                    : new Result(State.MISSING_MEMBER, null);
        }
        int a = f.member.access;
        boolean apiStatic = m.mods.contains("static") || m.kind == ApiModel.MemberKind.ENUM_CONSTANT;
        if (apiStatic != ((a & Opcodes.ACC_STATIC) != 0)) {
            return new Result(State.WRONG_STATIC, apiStatic ? "an instance field here" : "static here");
        }
        Result access = access(m, a);
        if (access != null) return access;
        if (!m.mods.contains("final") && m.kind == ApiModel.MemberKind.FIELD && (a & Opcodes.ACC_FINAL) != 0) {
            return new Result(State.WRONG_FINAL, "final here: an app cannot write it");
        }
        if (m.value != null && f.member.value != null) {
            Object want = Constants.parse(m.value, m.desc);
            if (want != null && !Constants.same(want, f.member.value)) {
                return new Result(State.WRONG_CONSTANT, "= " + Constants.show(f.member.value) + " here");
            }
        }
        return Result.OK;
    }

    // Public in the API needs public here; protected needs protected or public.
    private static Result access(ApiModel.ApiMember m, int a) {
        boolean pub = (a & Opcodes.ACC_PUBLIC) != 0, prot = (a & Opcodes.ACC_PROTECTED) != 0;
        if (m.mods.contains("public") && !pub) {
            return new Result(State.WRONG_ACCESS, (prot ? "protected" : "not public") + " here");
        }
        if (m.mods.contains("protected") && !pub && !prot) return new Result(State.WRONG_ACCESS, "not visible here");
        return null;
    }

    private static String where(ClassPathIndex.ClassInfo owner, ClassPathIndex.ClassInfo ci) {
        return owner == ci ? "" : " (declared in " + owner.name + ")";
    }

    // ---- lookups ----------------------------------------------------------------------------

    private Found findInClasses(ClassPathIndex.ClassInfo ci, String key) {
        for (ClassPathIndex.ClassInfo c = ci; c != null; c = c.superName == null ? null : index.get(c.superName)) {
            ClassPathIndex.MemberInfo mi = c.methods.get(key);
            if (mi != null) return new Found(c, mi);
        }
        return null;
    }

    // Superinterface methods, breadth first: a non-abstract (default) one before an abstract one.
    private Found findInInterfaces(ClassPathIndex.ClassInfo ci, String key) {
        Found abstractOne = null;
        for (String i : allInterfaces(ci)) {
            ClassPathIndex.ClassInfo ic = index.get(i);
            if (ic == null) continue;
            ClassPathIndex.MemberInfo mi = ic.methods.get(key);
            if (mi == null || (mi.access & Opcodes.ACC_PRIVATE) != 0) continue;
            if ((mi.access & Opcodes.ACC_ABSTRACT) == 0) return new Found(ic, mi);
            if (abstractOne == null) abstractOne = new Found(ic, mi);
        }
        return abstractOne;
    }

    private Found findField(ClassPathIndex.ClassInfo ci, String key, Set<String> seen) {
        if (ci == null || !seen.add(ci.name)) return null;
        ClassPathIndex.MemberInfo mi = ci.fields.get(key);
        if (mi != null) return new Found(ci, mi);
        for (String i : ci.interfaces) {
            Found f = findField(index.get(i), key, seen);
            if (f != null) return f;
        }
        return ci.superName == null ? null : findField(index.get(ci.superName), key, seen);
    }

    // The return type of a method with the API's name and parameters but another return type, or null.
    private String otherReturn(ClassPathIndex.ClassInfo ci, ApiModel.ApiMember m) {
        String prefix = m.name + m.params();
        List<ClassPathIndex.ClassInfo> all = new ArrayList<>();
        if (m.kind == ApiModel.MemberKind.CTOR) {
            all.add(ci);
        } else {
            for (ClassPathIndex.ClassInfo c = ci; c != null; c = c.superName == null ? null : index.get(c.superName)) {
                all.add(c);
            }
            for (String i : allInterfaces(ci)) {
                ClassPathIndex.ClassInfo ic = index.get(i);
                if (ic != null) all.add(ic);
            }
        }
        for (ClassPathIndex.ClassInfo c : all) {
            for (String key : c.methods.keySet()) {
                if (key.startsWith(prefix)) return key.substring(prefix.length());
            }
        }
        return null;
    }

    private String otherFieldType(ClassPathIndex.ClassInfo ci, String name, Set<String> seen) {
        if (ci == null || !seen.add(ci.name)) return null;
        for (String key : ci.fields.keySet()) {
            if (key.startsWith(name + ":")) return key.substring(name.length() + 1);
        }
        for (String i : ci.interfaces) {
            String t = otherFieldType(index.get(i), name, seen);
            if (t != null) return t;
        }
        return ci.superName == null ? null : otherFieldType(index.get(ci.superName), name, seen);
    }

    // The superclass chain above ci, as far as the index has it.
    private Set<String> superclasses(ClassPathIndex.ClassInfo ci) {
        Set<String> out = new LinkedHashSet<>();
        for (String s = ci.superName; s != null && out.add(s); ) {
            ClassPathIndex.ClassInfo c = index.get(s);
            s = c == null ? null : c.superName;
        }
        return out;
    }

    // Every interface ci implements, directly or through its superclasses and superinterfaces, breadth first.
    private Set<String> allInterfaces(ClassPathIndex.ClassInfo ci) {
        Set<String> out = new LinkedHashSet<>();
        Deque<String> todo = new ArrayDeque<>();
        for (ClassPathIndex.ClassInfo c = ci; c != null; c = c.superName == null ? null : index.get(c.superName)) {
            for (String i : c.interfaces) todo.add(i);
            if (Objects.equals(c.superName, c.name)) break;
        }
        while (!todo.isEmpty()) {
            String i = todo.poll();
            if (!out.add(i)) continue;
            ClassPathIndex.ClassInfo ic = index.get(i);
            if (ic != null) for (String j : ic.interfaces) todo.add(j);
        }
        return out;
    }
}
