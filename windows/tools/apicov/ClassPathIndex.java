// ClassPathIndex -- the classes an app running on the runtime can link
// against, found as the JVM finds them:
//
//   1. A package that belongs to a module of the runtime's JRE is that
//      module's, with the runtime's patch directories (lib/java_base_patch,
//      lib/jdk_unsupported_patch) on top. A class in such a package on the class
//      path is never loaded, so it is not seen here either.
//   2. Any other package comes from the class path, the first entry that has the
//      class winning: android-framework.jar, macapk-loader.jar, bcprov.
//
// Classes are read on demand, headers and member signatures only (ASM with
// SKIP_CODE), and kept.

package com.winapk.apicov;

import jdk.internal.org.objectweb.asm.ClassReader;
import jdk.internal.org.objectweb.asm.ClassVisitor;
import jdk.internal.org.objectweb.asm.FieldVisitor;
import jdk.internal.org.objectweb.asm.MethodVisitor;
import jdk.internal.org.objectweb.asm.Opcodes;

import java.io.Closeable;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.net.URI;
import java.nio.file.DirectoryStream;
import java.nio.file.FileSystem;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.TreeSet;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

final class ClassPathIndex implements AutoCloseable {
    static final class MemberInfo {
        final int access;
        final Object value;   // a field's ConstantValue, or null

        MemberInfo(int access, Object value) {
            this.access = access;
            this.value = value;
        }
    }

    static final class ClassInfo {
        String name;
        int access;
        String superName;
        String[] interfaces;
        String from;          // "java.base", "android-framework.jar"...
        final Map<String, MemberInfo> methods = new HashMap<>();   // name + desc
        final Map<String, MemberInfo> fields = new HashMap<>();    // name + ":" + desc

        boolean isInterface() { return (access & Opcodes.ACC_INTERFACE) != 0; }
    }

    // Where the class bytes come from.
    private interface Source {
        byte[] read(String path) throws IOException;   // null when absent
        String label();
    }

    private final Map<String, String> packageModule = new HashMap<>();     // a/b -> java.base
    private final Map<String, List<Source>> moduleSources = new HashMap<>();
    private final List<Source> classPath = new ArrayList<>();
    private final Map<String, Optional<ClassInfo>> cache = new HashMap<>();
    private final List<Closeable> open = new ArrayList<>();
    private final Map<String, String> inputs = new LinkedHashMap<>();       // label -> description, for the report

    // The JRE whose modules supply the JDK packages: a runtime's jre/ directory, or a JDK home.
    void addJre(Path home) throws IOException {
        FileSystem jrt = FileSystems.newFileSystem(URI.create("jrt:/"), Map.of("java.home", home.toString()));
        open.add(jrt);
        Path packages = jrt.getPath("/packages");
        try (DirectoryStream<Path> ps = Files.newDirectoryStream(packages)) {
            for (Path p : ps) {
                String pkg = p.getFileName().toString().replace('.', '/');
                try (DirectoryStream<Path> ms = Files.newDirectoryStream(p)) {
                    for (Path m : ms) {
                        String module = m.getFileName().toString();
                        packageModule.putIfAbsent(pkg, module);
                    }
                }
            }
        }
        for (String module : new TreeSet<>(packageModule.values())) {
            Path root = jrt.getPath("/modules", module);
            moduleSources.computeIfAbsent(module, k -> new ArrayList<>()).add(dirSource(root, module));
        }
        inputs.put("jre", home.toAbsolutePath().normalize().toString());
    }

    // Classes patched into a module (--patch-module, or baked into the JRE): they win over the module's own.
    void addPatch(String module, Path dir) throws IOException {
        if (!Files.isDirectory(dir)) return;
        List<Source> list = moduleSources.computeIfAbsent(module, k -> new ArrayList<>());
        list.add(0, dirSource(dir, module + " patch"));
        try (var walk = Files.walk(dir)) {
            walk.filter(f -> f.toString().endsWith(".class")).forEach(f -> {
                Path rel = dir.relativize(f.getParent() == null ? f : f.getParent());
                packageModule.putIfAbsent(rel.toString().replace('\\', '/'), module);
            });
        }
        inputs.put(module + " patch", dir.toAbsolutePath().normalize().toString());
    }

    // A class path entry: a jar or a directory of classes.
    void addClassPath(Path entry) throws IOException {
        String label = entry.getFileName().toString();
        if (Files.isDirectory(entry)) {
            classPath.add(dirSource(entry, label));
        } else {
            ZipFile z = new ZipFile(entry.toFile());
            open.add(z);
            classPath.add(new Source() {
                public byte[] read(String path) throws IOException {
                    ZipEntry e = z.getEntry(path);
                    if (e == null) return null;
                    try (var in = z.getInputStream(e)) { return in.readAllBytes(); }
                }

                public String label() { return label; }
            });
        }
        inputs.put(label, entry.toAbsolutePath().normalize().toString());
    }

    private static Source dirSource(Path root, String label) {
        return new Source() {
            public byte[] read(String path) throws IOException {
                Path f = root.resolve(path);
                return Files.isRegularFile(f) ? Files.readAllBytes(f) : null;
            }

            public String label() { return label; }
        };
    }

    Map<String, String> inputs() { return inputs; }

    // The module that owns a package (internal form, a/b), or null for a class path package.
    String moduleOf(String pkg) { return packageModule.get(pkg); }

    ClassInfo get(String internalName) {
        return cache.computeIfAbsent(internalName, n -> Optional.ofNullable(load(n))).orElse(null);
    }

    private ClassInfo load(String name) {
        int slash = name.lastIndexOf('/');
        String pkg = slash < 0 ? "" : name.substring(0, slash);
        String module = packageModule.get(pkg);
        List<Source> sources = module != null ? moduleSources.getOrDefault(module, List.of()) : classPath;
        try {
            for (Source s : sources) {
                byte[] b = s.read(name + ".class");
                if (b != null) return read(b, s.label());
            }
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
        return null;
    }

    private static ClassInfo read(byte[] bytes, String from) {
        ClassInfo ci = new ClassInfo();
        ci.from = from;
        new ClassReader(bytes).accept(new ClassVisitor(Opcodes.ASM9) {
            @Override
            public void visit(int version, int access, String name, String signature, String superName,
                              String[] interfaces) {
                ci.name = name;
                ci.access = access;
                ci.superName = superName;
                ci.interfaces = interfaces == null ? new String[0] : interfaces;
            }

            @Override
            public FieldVisitor visitField(int access, String name, String desc, String signature, Object value) {
                ci.fields.put(name + ":" + desc, new MemberInfo(access, value));
                return null;
            }

            @Override
            public MethodVisitor visitMethod(int access, String name, String desc, String signature,
                                             String[] exceptions) {
                ci.methods.put(name + desc, new MemberInfo(access, null));
                return null;
            }
        }, ClassReader.SKIP_CODE | ClassReader.SKIP_DEBUG | ClassReader.SKIP_FRAMES);
        return ci;
    }

    @Override
    public void close() throws IOException {
        for (Closeable c : open) c.close();
    }
}
