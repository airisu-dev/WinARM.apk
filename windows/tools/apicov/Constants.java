// Constants -- a field's constant as the signature file writes it, read into
// the value its ConstantValue attribute holds (ASM's boxing: Integer for
// boolean, byte, char, short and int; Long, Float, Double, String), so the two
// can be compared.
//
// The file writes chars as numbers (MENU_KEY = 115), longs with L, floats with
// f, and NaN and the infinities as divisions: (0.0f/0.0f), (-1.0/0.0).

package com.winapk.apicov;

final class Constants {
    private Constants() {}

    // The value, or null when the text is not a form this reads (the field is then not compared).
    static Object parse(String text, String desc) {
        String t = text.trim();
        try {
            switch (desc) {
                case "Z": return t.equals("true") ? 1 : t.equals("false") ? 0 : null;
                case "B": case "S": case "I": return Integer.decode(t);
                case "C": return t.startsWith("'") ? (int) unescape(t).charAt(0) : Integer.decode(t);
                case "J": return Long.decode(t.endsWith("L") ? t.substring(0, t.length() - 1) : t);
                case "F": return (float) number(t, true);
                case "D": return number(t, false);
                case "Ljava/lang/String;": return t.startsWith("\"") ? unescape(t) : null;
                default: return null;
            }
        } catch (NumberFormatException e) {
            return null;
        }
    }

    private static double number(String t, boolean isFloat) {
        if (t.startsWith("(") && t.endsWith(")") && t.contains("/")) {
            String[] p = t.substring(1, t.length() - 1).split("/");
            double a = number(p[0].trim(), isFloat), b = number(p[1].trim(), isFloat);
            return isFloat ? (double) ((float) a / (float) b) : a / b;
        }
        String s = t.endsWith("f") || t.endsWith("F") || t.endsWith("d") || t.endsWith("D")
                ? t.substring(0, t.length() - 1) : t;
        return isFloat ? Float.parseFloat(s) : Double.parseDouble(s);
    }

    static boolean same(Object want, Object have) {
        if (want instanceof Float a && have instanceof Float b) {
            return Float.floatToIntBits(a) == Float.floatToIntBits(b);
        }
        if (want instanceof Double a && have instanceof Double b) {
            return Double.doubleToLongBits(a) == Double.doubleToLongBits(b);
        }
        return want.equals(have);
    }

    static String show(Object v) {
        if (v instanceof String s) {
            StringBuilder b = new StringBuilder("\"");
            for (char c : s.toCharArray()) {
                if (c == '"' || c == '\\') b.append('\\').append(c);
                else if (c < 0x20 || c > 0x7e) b.append(String.format("\\u%04x", (int) c));
                else b.append(c);
            }
            return b.append('"').toString();
        }
        return String.valueOf(v);
    }

    // A Java string or char literal, quotes included, to its value.
    static String unescape(String lit) {
        String s = lit.substring(1, lit.length() - 1);
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c != '\\' || i + 1 >= s.length()) {
                out.append(c);
                continue;
            }
            char e = s.charAt(++i);
            switch (e) {
                case 'n' -> out.append('\n');
                case 't' -> out.append('\t');
                case 'r' -> out.append('\r');
                case 'b' -> out.append('\b');
                case 'f' -> out.append('\f');
                case 's' -> out.append(' ');
                case 'u' -> {
                    while (i + 1 < s.length() && s.charAt(i + 1) == 'u') i++;
                    out.append((char) Integer.parseInt(s.substring(i + 1, i + 5), 16));
                    i += 4;
                }
                default -> {
                    if (e >= '0' && e <= '7') {
                        int j = i, v = 0;
                        while (j < s.length() && j < i + 3 && s.charAt(j) >= '0' && s.charAt(j) <= '7') {
                            v = v * 8 + (s.charAt(j++) - '0');
                        }
                        out.append((char) v);
                        i = j - 1;
                    } else {
                        out.append(e);   // \" \' \\
                    }
                }
            }
        }
        return out.toString();
    }
}
