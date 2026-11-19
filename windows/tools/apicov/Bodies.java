// Bodies -- whether an API method that links here also does something: its
// body here (BodyClass) against the same method in AOSP's framework, and for a
// native, how the runtime binds it (winapk-runtime --list-natives).
//
//   implemented   real code here (whether it is the right code is not claimed)
//   same-as-aosp  trivial here and in AOSP too: empty or a default on purpose
//   stub          trivial here where AOSP's does work, or a native bound to a
//                 stub or not bound at all (UnsatisfiedLinkError)
//   abstract      abstract in the API: left to subclasses, or in AOSP done by a
//                 hidden class the system hands out (PackageManager), which is
//                 not judged
//   unknown       trivial here, and AOSP's cannot be found or is abstract;
//                 or a native when no listing was given
//
// A real body that only calls a stub is still "implemented": calls are not
// followed, so stubs are a lower bound.

package com.winapk.apicov;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.Map;

final class Bodies {
    enum Verdict {
        IMPLEMENTED("implemented"), SAME_AS_AOSP("same-as-aosp"), STUB("stub"), ABSTRACT("abstract"),
        UNKNOWN("unknown");

        final String id;

        Verdict(String id) { this.id = id; }
    }

    record Result(Verdict verdict, String detail) {}

    private final Matcher ours, aosp;
    private final Map<String, Boolean> natives;   // "a.b.C name (sig)" -> implemented; null: no listing

    Bodies(Matcher ours, Matcher aosp, Map<String, Boolean> natives) {
        this.ours = ours;
        this.aosp = aosp;
        this.natives = natives;
    }

    // winapk-runtime --list-natives output: "<class> <name> <signature> implemented|stub" per line.
    static Map<String, Boolean> readNatives(Path file) throws IOException {
        Map<String, Boolean> out = new HashMap<>();
        for (String line : Files.readAllLines(file, StandardCharsets.UTF_8)) {
            String[] f = line.trim().split(" ");
            if (f.length != 4) continue;
            out.put(f[0] + " " + f[1] + " " + f[2], f[3].equals("implemented"));
        }
        return out;
    }

    // Only for a method or constructor the matcher found ok.
    Result judge(ApiModel.ApiClass k, ApiModel.ApiMember m) {
        // Abstract in the API (PackageManager.resolveActivity, an interface's method): in AOSP the work is in a
        // hidden subclass the system hands out, which a body here cannot be compared with, trivial or not.
        boolean apiAbstract = m.mods.contains("abstract")
                || (k.isInterface() && !m.mods.contains("default") && !m.mods.contains("static"));
        if (apiAbstract) return new Result(Verdict.ABSTRACT, null);
        Matcher.Found here = ours.resolveMethod(k, m);
        if (here == null) return new Result(Verdict.UNKNOWN, "not found here");
        BodyClass body = here.member().body;
        String where = here.owner().name.equals(k.name) ? "" : " in " + here.owner().name;
        switch (body) {
            case ABSTRACT:
                return new Result(Verdict.ABSTRACT, null);
            case NATIVE: {
                if (natives == null) return new Result(Verdict.UNKNOWN, "native" + where + ", no listing");
                String key = here.owner().name.replace('/', '.') + " " + m.name + " " + m.desc;
                Boolean bound = natives.get(key);
                if (bound == null) return new Result(Verdict.STUB, "native" + where + ", not bound");
                return bound ? new Result(Verdict.IMPLEMENTED, null)
                        : new Result(Verdict.STUB, "native" + where + ", bound to a stub");
            }
            case REAL:
                return new Result(Verdict.IMPLEMENTED, null);
            default:
                break;
        }
        Matcher.Found ref = aosp.resolveMethod(k, m);
        if (ref == null) return new Result(Verdict.UNKNOWN, body.id() + where + "; not in AOSP's");
        BodyClass theirs = ref.member().body;
        if (theirs.trivial()) return new Result(Verdict.SAME_AS_AOSP, body.id() + where);
        if (theirs == BodyClass.ABSTRACT) return new Result(Verdict.UNKNOWN, body.id() + where + "; abstract in AOSP");
        return new Result(Verdict.STUB, body.id() + where + "; AOSP's is " + theirs.id());
    }
}
