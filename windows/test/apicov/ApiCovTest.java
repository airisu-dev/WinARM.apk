// ApiCovTest -- the API coverage tool (windows/tools/apicov) against a fixture:
// api.txt, a small signature file with one planted case per state and the
// parser's hard cases (annotations with arguments, generics, varargs, nested
// classes, java.lang short names, constants), and src/fx, the classes it is
// matched against. expected.tsv gives every class and member its state; a
// member without an expectation fails too, so nothing passes unchecked.
//
//   windows/test/apicov/run_test.sh

package com.winapk.apicov;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

public final class ApiCovTest {
    public static void main(String[] args) throws Exception {
        Path dir = Path.of(args[0]), classes = Path.of(args[1]);
        ApiModel model = SignatureFile.parse(dir.resolve("api.txt"));
        int failures = 0;
        for (ApiModel.Problem p : model.problems) {
            System.out.println("FAIL parse, line " + p.line() + ": " + p.why() + ": " + p.text());
            failures++;
        }

        Map<String, String> expected = new HashMap<>();
        for (String line : Files.readAllLines(dir.resolve("expected.tsv"), StandardCharsets.UTF_8)) {
            if (line.isBlank() || line.startsWith("#")) continue;
            String[] f = line.split("\t", -1);
            expected.put(f[0] + "#" + f[1], f[2]);
        }

        Set<String> seen = new HashSet<>();
        try (ClassPathIndex index = new ClassPathIndex()) {
            index.addJre(Path.of(System.getProperty("java.home")));
            index.addClassPath(classes);
            Matcher matcher = new Matcher(index);
            for (ApiModel.ApiClass k : model.classes.values()) {
                failures += check(expected, seen, k.display + "#", matcher.matchClass(k));
                for (ApiModel.ApiMember m : k.members) {
                    failures += check(expected, seen, k.display + "#" + m.name, matcher.matchMember(k, m));
                }
            }
        }
        for (String key : expected.keySet()) {
            if (!seen.contains(key)) {
                System.out.println("FAIL " + key + ": expected, but not in api.txt");
                failures++;
            }
        }

        // The constants the matcher compares: each form the signature format writes.
        failures += same(Constants.parse("115", "C"), 115);
        failures += same(Constants.parse("-1L", "J"), -1L);
        failures += same(Constants.parse("(0.0f/0.0f)", "F"), Float.NaN);
        failures += same(Constants.parse("(-1.0/0.0)", "D"), Double.NEGATIVE_INFINITY);
        failures += same(Constants.parse("9.80665f", "F"), 9.80665f);
        failures += same(Constants.parse("true", "Z"), 1);
        failures += same(Constants.parse("\"a\\\"b\\u00e9\\n\"", "Ljava/lang/String;"), "a\"bé\n");

        System.out.println(failures == 0 ? "apicov: all passed" : "apicov: " + failures + " failure(s)");
        System.exit(failures == 0 ? 0 : 1);
    }

    private static int check(Map<String, String> expected, Set<String> seen, String key, Matcher.Result r) {
        if (!seen.add(key)) {
            System.out.println("FAIL " + key + ": twice in api.txt; give the fixture unique names");
            return 1;
        }
        String want = expected.get(key);
        if (want == null) {
            System.out.println("FAIL " + key + ": no expectation (got " + r.state().id + ")");
            return 1;
        }
        if (!want.equals(r.state().id)) {
            System.out.println("FAIL " + key + ": expected " + want + ", got " + r.state().id
                    + (r.detail() == null ? "" : " (" + r.detail() + ")"));
            return 1;
        }
        return 0;
    }

    private static int same(Object got, Object want) {
        if (got != null && Constants.same(want, got)) return 0;
        System.out.println("FAIL constant: expected " + want + ", got " + got);
        return 1;
    }
}
