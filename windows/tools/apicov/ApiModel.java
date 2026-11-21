// ApiModel -- an Android API surface as SignatureFile reads it: classes and
// their members, each with the JVM descriptor an app's bytecode links against.

package com.winapk.apicov;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

final class ApiModel {
    enum Kind { CLASS, INTERFACE, ENUM, ANNOTATION }

    enum MemberKind { CTOR, METHOD, FIELD, ENUM_CONSTANT }

    static final class ApiClass {
        String name;         // internal name: android/app/Notification$Builder
        String pkg;          // android.app
        String display;      // android.app.Notification.Builder
        Kind kind;
        Set<String> mods;
        String superName;    // internal name; null for interfaces
        List<String> interfaces = new ArrayList<>();
        boolean inner;       // a non-static nested class: its constructors take the outer instance first
        int line;
        final List<ApiMember> members = new ArrayList<>();

        boolean isInterface() { return kind == Kind.INTERFACE || kind == Kind.ANNOTATION; }
    }

    static final class ApiMember {
        MemberKind kind;
        String name;         // <init> for a constructor
        String desc;         // (Ljava/lang/String;I)V, or the field type
        Set<String> mods;
        String value;        // a field's constant as the file writes it, or null
        String text;         // the line, trimmed, for the report
        int line;

        boolean isMethod() { return kind == MemberKind.CTOR || kind == MemberKind.METHOD; }

        // The descriptor without the return type: what tells a wrong return type from a missing method.
        String params() { return desc.substring(0, desc.indexOf(')') + 1); }
    }

    // A line the parser could not read: the report lists them, and a complete parse has none.
    record Problem(int line, String text, String why) {}

    final Map<String, ApiClass> classes = new LinkedHashMap<>();
    final List<Problem> problems = new ArrayList<>();
    String format;           // "2.0"
}
