// ApiCoverage -- the command line of the API coverage report.
//
//   java ... com.winapk.apicov.ApiCoverage --api android-36.txt --out <dir>
//        (--runtime <staged runtime> | [--jre <java home>] [--patch <module>=<dir>]... --cp <jar or dir>...)
//        [--aosp <android-all.jar> [--natives <file>]] [--label <name>] [--strict]
//
// --runtime takes what winapk-runtime.exe puts on the JVM: <runtime>/jre (else
// the running JVM's own home), the java.base and jdk.unsupported patches in
// <runtime>/lib, and the class path android-framework.jar, macapk-loader.jar,
// bcprov. --strict exits 1 when a line of the API file could not be read.
// --aosp also judges each method that links: real code, trivial as in AOSP,
// or a stub (Bodies), with --natives the runtime's binding of its natives
// (winapk-runtime --list-natives).
//
// Needs the JDK's internal ASM: run with
//   --add-exports java.base/jdk.internal.org.objectweb.asm=ALL-UNNAMED

package com.winapk.apicov;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Properties;

public final class ApiCoverage {
    public static void main(String[] args) throws Exception {
        Path api = null, out = null, runtime = null, jre = null, aospJar = null, nativesFile = null;
        List<String[]> patches = new ArrayList<>();
        List<Path> cp = new ArrayList<>();
        String label = null;
        boolean strict = false;
        for (int i = 0; i < args.length; i++) {
            String a = args[i];
            switch (a) {
                case "--api" -> api = Path.of(args[++i]);
                case "--out" -> out = Path.of(args[++i]);
                case "--runtime" -> runtime = Path.of(args[++i]);
                case "--jre" -> jre = Path.of(args[++i]);
                case "--patch" -> {
                    String[] kv = args[++i].split("=", 2);
                    if (kv.length != 2) usage("--patch wants <module>=<dir>");
                    patches.add(kv);
                }
                case "--cp" -> cp.add(Path.of(args[++i]));
                case "--label" -> label = args[++i];
                case "--aosp" -> aospJar = Path.of(args[++i]);
                case "--natives" -> nativesFile = Path.of(args[++i]);
                case "--strict" -> strict = true;
                default -> usage("unknown argument " + a);
            }
        }
        if (api == null || out == null) usage("--api and --out are required");
        if (runtime != null) {
            Path lib = runtime.resolve("lib");
            if (jre == null && Files.isDirectory(runtime.resolve("jre"))) jre = runtime.resolve("jre");
            patches.add(new String[] {"java.base", lib.resolve("java_base_patch").toString()});
            patches.add(new String[] {"jdk.unsupported", lib.resolve("jdk_unsupported_patch").toString()});
            cp.add(lib.resolve("android-framework.jar"));
            cp.add(lib.resolve("macapk-loader.jar"));
            try (DirectoryStream<Path> ds = Files.newDirectoryStream(lib, "bcprov-*.jar")) {
                for (Path p : ds) cp.add(p);
            }
        }
        if (jre == null) jre = Path.of(System.getProperty("java.home"));
        if (cp.isEmpty()) usage("no class path: give --runtime or --cp");
        for (Path p : cp) if (!Files.exists(p)) usage("not found: " + p);
        if (aospJar != null && !Files.isRegularFile(aospJar)) usage("not found: " + aospJar);
        if (nativesFile != null && !Files.isRegularFile(nativesFile)) usage("not found: " + nativesFile);
        if (label == null) {
            label = (runtime != null ? runtime : cp.get(0)).getFileName().toString();
        }

        ApiModel model = SignatureFile.parse(api);
        Report report = new Report(label);
        report.problems.addAll(model.problems);
        report.meta.put("api", api.getFileName().toString());
        report.meta.put("api_sha256", sha256(api));
        report.meta.put("format", String.valueOf(model.format));

        try (ClassPathIndex index = new ClassPathIndex(); ClassPathIndex aospIndex = new ClassPathIndex()) {
            index.addJre(jre);
            report.inputs.put("jre", javaVersion(jre));
            for (String[] p : patches) {
                Path dir = Path.of(p[1]);
                if (Files.isDirectory(dir)) {
                    index.addPatch(p[0], dir);
                    report.inputs.put(p[0] + " patch", treeDigest(dir));
                }
            }
            for (Path p : cp) {
                index.addClassPath(p);
                report.inputs.put(p.getFileName().toString(), Files.isDirectory(p) ? treeDigest(p) : sha256(p));
            }
            Matcher matcher = new Matcher(index);
            Bodies bodies = null;
            if (aospJar != null) {
                // AOSP's framework on the JDK running the tool: only its android packages are compared.
                aospIndex.addJre(Path.of(System.getProperty("java.home")));
                aospIndex.addClassPath(aospJar);
                report.meta.put("aosp", aospJar.getFileName().toString());
                report.meta.put("aosp_sha256", sha256(aospJar));
                Map<String, Boolean> natives = null;
                if (nativesFile != null) {
                    natives = Bodies.readNatives(nativesFile);
                    report.inputs.put("natives", sha256(nativesFile));
                }
                bodies = new Bodies(matcher, new Matcher(aospIndex), natives);
            }
            for (ApiModel.ApiClass k : model.classes.values()) {
                String group = groupOf(index, k.pkg);
                report.rows.add(new Report.Row(group, k.pkg, k, null, matcher.matchClass(k), null));
                for (ApiModel.ApiMember m : k.members) {
                    Matcher.Result r = matcher.matchMember(k, m);
                    Bodies.Result b = bodies != null && group.equals("android") && m.isMethod()
                            && r.state() == Matcher.State.OK ? bodies.judge(k, m) : null;
                    report.rows.add(new Report.Row(group, k.pkg, k, m, r, b));
                }
            }
            report.bodies = bodies != null;
        }
        report.write(out);
        System.out.print(report.markdown().lines().limit(12).reduce("", (x, y) -> x + y + "\n"));
        System.out.println("[apicov] report in " + out.toAbsolutePath().normalize());
        if (!model.problems.isEmpty()) {
            System.err.println("[apicov] " + model.problems.size() + " line(s) of " + api.getFileName()
                    + " could not be read (report.md lists them)");
            if (strict) System.exit(1);
        }
    }

    // JDK packages are the runtime JRE's (java.*, and javax.* or org.* a module of the JRE owns);
    // everything else is Android's.
    static String groupOf(ClassPathIndex index, String pkg) {
        if (pkg.equals("java") || pkg.startsWith("java.")) return "jdk";
        return index.moduleOf(pkg.replace('.', '/')) != null ? "jdk" : "android";
    }

    private static String javaVersion(Path jre) throws IOException {
        Path release = jre.resolve("release");
        if (!Files.isRegularFile(release)) return "unknown";
        Properties p = new Properties();
        try (InputStream in = Files.newInputStream(release)) { p.load(in); }
        return p.getProperty("JAVA_VERSION", "unknown").replace("\"", "");
    }

    static String sha256(Path f) throws IOException {
        try (InputStream in = Files.newInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) md.update(buf, 0, n);
            return HexFormat.of().formatHex(md.digest());
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    // A directory's classes, by relative path and content, as one digest.
    private static String treeDigest(Path dir) throws IOException {
        try (var walk = Files.walk(dir)) {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            for (Path f : walk.filter(Files::isRegularFile).sorted().toList()) {
                String rel = dir.relativize(f).toString().replace('\\', '/');
                md.update(rel.getBytes(java.nio.charset.StandardCharsets.UTF_8));
                md.update(Files.readAllBytes(f));
            }
            return HexFormat.of().formatHex(md.digest());
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    private static void usage(String why) {
        System.err.println("apicov: " + why);
        System.err.println("usage: ApiCoverage --api <android.txt> --out <dir> (--runtime <dir> | --cp <jar>...)"
                + " [--jre <home>] [--patch <module>=<dir>]");
        System.err.println("                   [--aosp <jar> [--natives <file>]] [--label <name>] [--strict]");
        System.exit(2);
    }
}
