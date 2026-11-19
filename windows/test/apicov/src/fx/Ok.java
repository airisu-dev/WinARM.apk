package fx;

import java.util.List;
import java.util.Map;

public class Ok extends Base implements Iface {
    public static final char CH = 's';
    public static final int CONST = 5;
    public static final int K = 2;          // the API says 1
    public static final long L = -1L;
    public static final float NAN = 0.0f / 0.0f;
    public static final String S = "a\"bé";
    public long count;
    public long typed;                      // the API says int

    public void ann(int x) {}

    public <T extends CharSequence> T bound(T t) { return t; }

    public final void fin() {}              // the API's is not final

    @SafeVarargs
    public final void pairs(Map.Entry<String, Integer>... e) {}

    public void many(int... x) {}

    public String name(Object o, Thread.State s) { return null; }

    public int plain(String s) { return 0; }

    public long ret() { return 0; }         // the API's returns int

    public void stat() {}                   // the API's is static

    protected void vis() {}                 // the API's is public

    @SuppressWarnings("unchecked")
    public List<? extends Number>[] wild(Class<?>... c) { return null; }
}
