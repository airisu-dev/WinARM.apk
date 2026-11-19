// Report -- the results of a match as report.json (the numbers, stable from run
// to run: no times, no absolute paths), report.md (for people), gaps.tsv
// (every class and member that is not ok, one per line) and, when bodies were
// judged (--aosp), stubs.tsv (every method that links but does nothing here).

package com.winapk.apicov;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.EnumMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

final class Report {
    // What one class or member came out as; body is the verdict on a method that links, when bodies were judged.
    record Row(String group, String pkg, ApiModel.ApiClass cls, ApiModel.ApiMember member, Matcher.Result result,
               Bodies.Result body) {}

    // The methods and constructors of a group or package, by verdict; those that do not link apart.
    static final class BodyCounts {
        int total, notLinkable, notJudged;   // notJudged: links, but no verdict (JDK packages)
        final EnumMap<Bodies.Verdict, Integer> verdicts = new EnumMap<>(Bodies.Verdict.class);

        void add(Row r) {
            total++;
            if (r.result.state() != Matcher.State.OK) notLinkable++;
            else if (r.body == null) notJudged++;
            else verdicts.merge(r.body.verdict(), 1, Integer::sum);
        }

        int of(Bodies.Verdict v) { return verdicts.getOrDefault(v, 0); }

        // What does work, of what an app can call and expects to: neither abstract nor a gap counted above.
        int working() { return of(Bodies.Verdict.IMPLEMENTED) + of(Bodies.Verdict.SAME_AS_AOSP); }

        int callable() { return total - of(Bodies.Verdict.ABSTRACT); }
    }

    static final class Counts {
        int total;
        final EnumMap<Matcher.State, Integer> states = new EnumMap<>(Matcher.State.class);

        void add(Matcher.State s) {
            total++;
            states.merge(s, 1, Integer::sum);
        }

        int ok() { return states.getOrDefault(Matcher.State.OK, 0); }

        int of(Matcher.State s) { return states.getOrDefault(s, 0); }

        int wrong() { return total - ok() - of(Matcher.State.MISSING_CLASS) - of(Matcher.State.MISSING_MEMBER); }
    }

    static final class Group {
        final Counts classes = new Counts(), members = new Counts();
        final BodyCounts bodies = new BodyCounts();

        void add(Row r) {
            (r.member == null ? classes : members).add(r.result.state());
            if (r.member != null && r.member.isMethod()) bodies.add(r);
        }
    }

    final String label;
    final Map<String, String> meta = new TreeMap<>();          // api file, sha256, format...
    final Map<String, String> inputs = new TreeMap<>();        // runtime inputs and their digests
    final List<Row> rows = new ArrayList<>();
    final List<ApiModel.Problem> problems = new ArrayList<>();
    boolean bodies;                                            // verdicts were given (--aosp)

    Report(String label) { this.label = label; }

    private Map<String, Group> groups() {
        Map<String, Group> out = new TreeMap<>();
        for (Row r : rows) {
            out.computeIfAbsent(r.group, k -> new Group()).add(r);
        }
        return out;
    }

    private Map<String, Group> packages() {
        Map<String, Group> out = new TreeMap<>();
        for (Row r : rows) {
            out.computeIfAbsent(r.pkg, k -> new Group()).add(r);
        }
        return out;
    }

    private String groupOf(String pkg) {
        for (Row r : rows) if (r.pkg.equals(pkg)) return r.group;
        return "";
    }

    void write(Path out) throws IOException {
        Files.createDirectories(out);
        Files.writeString(out.resolve("report.json"), json(), StandardCharsets.UTF_8);
        Files.writeString(out.resolve("report.md"), markdown(), StandardCharsets.UTF_8);
        Files.writeString(out.resolve("gaps.tsv"), gaps(), StandardCharsets.UTF_8);
        if (bodies) Files.writeString(out.resolve("stubs.tsv"), stubs(), StandardCharsets.UTF_8);
    }

    // ---- report.md --------------------------------------------------------------------------

    String markdown() {
        StringBuilder b = new StringBuilder();
        b.append("# API coverage: ").append(label).append("\n\n");
        b.append("Every class and member of Android's public API (").append(meta.getOrDefault("api", "?"))
                .append(") checked against the classes this runtime really has, as the JVM would link them.\n\n");
        b.append("**What this measures:** whether an app that uses an API links. **Not:** whether the code behind it ")
                .append("does what Android does, nor what share of apps run.\n\n");

        b.append("| Group | Classes present | Members ok | Missing | Wrong shape |\n|---|--:|--:|--:|--:|\n");
        for (var e : groups().entrySet()) {
            Group g = e.getValue();
            int present = g.classes.total - g.classes.of(Matcher.State.MISSING_CLASS);
            int missing = g.members.of(Matcher.State.MISSING_CLASS) + g.members.of(Matcher.State.MISSING_MEMBER);
            b.append("| ").append(groupName(e.getKey())).append(" | ").append(ratio(present, g.classes.total))
                    .append(" | ").append(ratio(g.members.ok(), g.members.total)).append(" | ")
                    .append(missing).append(" | ").append(g.members.wrong()).append(" |\n");
        }
        b.append("\nMembers are constructors, methods, fields and enum constants. A member of a missing class is ")
                .append("missing. JDK packages come mostly from the runtime's JRE, so they are counted apart.\n\n");

        b.append("## By state\n\n| State | Classes | Members | Meaning |\n|---|--:|--:|---|\n");
        Counts cs = new Counts(), ms = new Counts();
        for (Row r : rows) (r.member == null ? cs : ms).add(r.result.state());
        for (Matcher.State s : Matcher.State.values()) {
            b.append("| `").append(s.id).append("` | ").append(cs.of(s)).append(" | ").append(ms.of(s))
                    .append(" | ").append(s.meaning).append(" |\n");
        }

        b.append("\n## Packages with the most gaps\n\n")
                .append("| Package | Group | Classes present | Members ok | Gaps |\n|---|---|--:|--:|--:|\n");
        List<Map.Entry<String, Group>> pk = new ArrayList<>(packages().entrySet());
        pk.sort(Comparator.<Map.Entry<String, Group>>comparingInt(e -> -(e.getValue().members.total
                - e.getValue().members.ok())).thenComparing(Map.Entry::getKey));
        int shown = 0;
        for (var e : pk) {
            Group g = e.getValue();
            int gaps = g.members.total - g.members.ok();
            if (gaps == 0 || shown++ == 40) break;
            int present = g.classes.total - g.classes.of(Matcher.State.MISSING_CLASS);
            b.append("| `").append(e.getKey()).append("` | ").append(groupOf(e.getKey())).append(" | ")
                    .append(ratio(present, g.classes.total)).append(" | ")
                    .append(ratio(g.members.ok(), g.members.total))
                    .append(" | ").append(gaps).append(" |\n");
        }
        b.append("\nEvery package is in `report.json`; every gap in `gaps.tsv`.\n");
        if (bodies) bodiesSection(b);

        if (!problems.isEmpty()) {
            b.append("\n## Lines the parser could not read: ").append(problems.size()).append("\n\n");
            for (ApiModel.Problem p : problems.subList(0, Math.min(20, problems.size()))) {
                b.append("- line ").append(p.line()).append(": ").append(p.why()).append(": `").append(p.text())
                        .append("`\n");
            }
        }
        b.append("\n## Inputs\n\n");
        for (Map<String, String> m : List.of(meta, inputs)) {
            for (var e : m.entrySet()) {
                b.append("- ").append(e.getKey()).append(": `").append(e.getValue()).append("`\n");
            }
        }
        return b.toString();
    }

    // Of the Android methods and constructors, which do work: each one that links, judged against AOSP's.
    private void bodiesSection(StringBuilder b) {
        Group android = groups().get("android");
        if (android == null) return;
        BodyCounts c = android.bodies;
        b.append("\n## Do the methods do anything?\n\n")
                .append("Each Android method and constructor that links, its code here against the same method in ")
                .append("AOSP (").append(meta.getOrDefault("aosp", "?")).append("). A body that only returns, ")
                .append("returns 0, false or null, or throws UnsupportedOperationException is trivial: a stub when ")
                .append("AOSP's does work, as Android meant when AOSP's is trivial too. ")
                .append(inputs.containsKey("natives") ? "Natives are judged by how the runtime binds them." :
                        "Natives are unknown: no `--natives` listing was given.")
                .append("\n\n| Android methods and constructors | Count | Share |\n|---|--:|--:|\n");
        bodyRow(b, "Implemented: real code here", c.of(Bodies.Verdict.IMPLEMENTED), c.total);
        bodyRow(b, "Trivial, as in AOSP", c.of(Bodies.Verdict.SAME_AS_AOSP), c.total);
        bodyRow(b, "**Stub**: trivial here, does work in AOSP; or a native that returns zero",
                c.of(Bodies.Verdict.STUB), c.total);
        bodyRow(b, "Unknown: trivial here, nothing to compare in AOSP", c.of(Bodies.Verdict.UNKNOWN), c.total);
        bodyRow(b, "Abstract in the API: not judged", c.of(Bodies.Verdict.ABSTRACT), c.total);
        bodyRow(b, "Not linkable (missing or wrong shape, above)", c.notLinkable, c.total);
        b.append("| Total | ").append(String.format("%,d", c.total)).append(" | |\n\n");
        b.append("**Working:** ").append(ratio(c.working(), c.callable()))
                .append(" of the methods and constructors an app can call (all but the abstract ones) link and do ")
                .append("work, or are trivial as in AOSP. Calls are not followed: a real body that only calls a stub ")
                .append("counts as implemented, so stubs are a lower bound. Methods abstract in the API are not ")
                .append("judged: in AOSP a hidden class the system hands out does their work (PackageManager, ")
                .append("ContentProvider's callers), and which class the runtime hands out is not followed.\n");

        b.append("\n## Packages with the most stubs\n\n")
                .append("| Package | Methods and constructors | Working | Stubs |\n|---|--:|--:|--:|\n");
        List<Map.Entry<String, Group>> pk = new ArrayList<>();
        for (var e : packages().entrySet()) {
            if (groupOf(e.getKey()).equals("android") && e.getValue().bodies.of(Bodies.Verdict.STUB) > 0) pk.add(e);
        }
        pk.sort(Comparator.<Map.Entry<String, Group>>comparingInt(e -> -e.getValue().bodies.of(Bodies.Verdict.STUB))
                .thenComparing(Map.Entry::getKey));
        for (var e : pk.subList(0, Math.min(40, pk.size()))) {
            BodyCounts p = e.getValue().bodies;
            b.append("| `").append(e.getKey()).append("` | ").append(String.format("%,d", p.total)).append(" | ")
                    .append(ratio(p.working(), p.callable())).append(" | ").append(p.of(Bodies.Verdict.STUB))
                    .append(" |\n");
        }
        b.append("\nEvery stub and unknown is in `stubs.tsv`.\n");
    }

    private static void bodyRow(StringBuilder b, String what, int n, int total) {
        b.append("| ").append(what).append(" | ").append(String.format("%,d", n)).append(" | ")
                .append(String.format("%.1f%%", total == 0 ? 0.0 : 100.0 * n / total)).append(" |\n");
    }

    // ---- stubs.tsv --------------------------------------------------------------------------

    String stubs() {
        StringBuilder b = new StringBuilder("verdict\tpackage\tclass\tmember\tdetail\tline\n");
        List<Row> sorted = new ArrayList<>();
        for (Row r : rows) {
            Bodies.Verdict v = r.body == null ? null : r.body.verdict();
            if (v == Bodies.Verdict.STUB || v == Bodies.Verdict.UNKNOWN) {
                sorted.add(r);
            }
        }
        sorted.sort(Comparator.comparing((Row r) -> r.body.verdict()).thenComparing(r -> r.cls.display)
                .thenComparing(r -> r.member.text));
        for (Row r : sorted) {
            b.append(r.body.verdict().id).append('\t').append(r.pkg).append('\t').append(r.cls.display).append('\t')
                    .append(r.member.text).append('\t').append(r.body.detail() == null ? "" : r.body.detail())
                    .append('\t').append(r.member.line).append('\n');
        }
        return b.toString();
    }

    private static String groupName(String g) {
        return g.equals("android") ? "Android packages" : g.equals("jdk") ? "JDK packages" : g;
    }

    private static String ratio(int a, int b) {
        return String.format("%,d / %,d (%.1f%%)", a, b, b == 0 ? 0.0 : 100.0 * a / b);
    }

    // ---- gaps.tsv ---------------------------------------------------------------------------

    String gaps() {
        StringBuilder b = new StringBuilder("state\tgroup\tpackage\tclass\tmember\tdetail\tline\n");
        List<Row> sorted = new ArrayList<>(rows);
        sorted.sort(Comparator.comparing((Row r) -> r.result.state()).thenComparing(r -> r.cls.display)
                .thenComparing(r -> r.member == null ? "" : r.member.text));
        for (Row r : sorted) {
            if (r.result.state() == Matcher.State.OK) continue;
            // Members of a missing class add nothing to the class's own line.
            if (r.member != null && r.result.state() == Matcher.State.MISSING_CLASS) continue;
            b.append(r.result.state().id).append('\t').append(r.group).append('\t').append(r.pkg).append('\t')
                    .append(r.cls.display).append('\t').append(r.member == null ? "" : r.member.text).append('\t')
                    .append(r.result.detail() == null ? "" : r.result.detail()).append('\t')
                    .append(r.member == null ? r.cls.line : r.member.line).append('\n');
        }
        return b.toString();
    }

    // ---- report.json ------------------------------------------------------------------------

    String json() {
        StringBuilder b = new StringBuilder("{\n");
        b.append("  \"tool\": \"apicov 1\",\n  \"label\": ").append(q(label)).append(",\n");
        b.append("  \"api\": ").append(map(meta)).append(",\n");
        b.append("  \"inputs\": ").append(map(inputs)).append(",\n");
        b.append("  \"parse_problems\": ").append(problems.size()).append(",\n");
        b.append("  \"groups\": {");
        String sep = "\n";
        for (var e : groups().entrySet()) {
            b.append(sep).append("    ").append(q(e.getKey())).append(": ").append(group(e.getValue()));
            sep = ",\n";
        }
        b.append("\n  },\n  \"packages\": {");
        sep = "\n";
        for (var e : packages().entrySet()) {
            b.append(sep).append("    ").append(q(e.getKey())).append(": ").append(group(e.getValue()));
            sep = ",\n";
        }
        return b.append("\n  }\n}\n").toString();
    }

    private String group(Group g) {
        String out = "{\"classes\": " + counts(g.classes) + ", \"members\": " + counts(g.members);
        BodyCounts c = g.bodies;
        if (bodies && !c.verdicts.isEmpty()) {
            StringBuilder b = new StringBuilder(", \"methods\": {\"total\": ").append(c.total)
                    .append(", \"not-linkable\": ").append(c.notLinkable);
            for (Bodies.Verdict v : Bodies.Verdict.values()) {
                if (c.of(v) > 0) b.append(", ").append(q(v.id)).append(": ").append(c.of(v));
            }
            out += b.append('}');
        }
        return out + "}";
    }

    private static String counts(Counts c) {
        StringBuilder b = new StringBuilder("{\"total\": ").append(c.total);
        for (Matcher.State s : Matcher.State.values()) {
            if (c.of(s) > 0) b.append(", ").append(q(s.id)).append(": ").append(c.of(s));
        }
        return b.append('}').toString();
    }

    private static String map(Map<String, String> m) {
        StringBuilder b = new StringBuilder("{");
        String sep = "";
        for (var e : m.entrySet()) {
            b.append(sep).append(q(e.getKey())).append(": ").append(q(e.getValue()));
            sep = ", ";
        }
        return b.append('}').toString();
    }

    private static String q(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            if (c == '"' || c == '\\') b.append('\\').append(c);
            else if (c < 0x20) b.append(String.format("\\u%04x", (int) c));
            else b.append(c);
        }
        return b.append('"').toString();
    }
}
